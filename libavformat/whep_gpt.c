#include "avformat.h"
#include "avformat_internal.h"
#include "libavutil/avstring.h"
#include "libavutil/opt.h"
#include "libavutil/mem.h"
#include "libavutil/time.h"
#include "libavcodec/codec_id.h"
#include "libavcodec/packet_internal.h"
#include "rtpdec.h"
#include "rtpenc_chain.h"
#include "whip_whep.h"

#include <inttypes.h>
#include <pthread.h>

// This file is a copy of whep.c registered under a different demuxer name (whep_gpt)
// for sandbox experimentation without impacting the main whep demuxer behavior.

#define WHEP_LOG(ctx, level, fmt, ...) \
    av_log(ctx, level, fmt, ##__VA_ARGS__)

typedef struct WHEPContext {
    const AVClass *class;
    AVFormatContext *fmt_ctx;

    // libdatachannel state
    int pc;
    int video_track;
    char *session_url;
    char *token;

    // RTP contexts mapped by payload type (0..127)
    RTPDemuxContext *payload_map[128];

    // buffering complete frames out of rtpdec
    AVFifo *video_queue;
    pthread_mutex_t video_queue_mutex;
    pthread_cond_t  video_queue_cond;
    AVPacket *video_pkt; // temporary holder for rtpdec output
} WHEPContext;

static int whep_get_sdp_a_line(int track, char *buffer, int size, int payload_type)
{
    // Minimal stub: if needed we could reconstruct an "a=rtpmap" line here.
    // In this fork we rely on rtpdec dynamic protocol initialized from SDP already.
    return AVERROR(ENOSYS);
}

static RTPDemuxContext* whep_new_rtp_context(AVFormatContext *s, int payload_type);

static int whep_parse_sdp_and_create_contexts(AVFormatContext *s)
{
    WHEPContext *whep = s->priv_data;

    char answer[SDP_MAX_SIZE] = {0};
    if (rtcGetRemoteDescription(whep->pc, answer, sizeof(answer)) < 0) {
        WHEP_LOG(s, AV_LOG_ERROR, "Failed to get remote description\n");
        return AVERROR_EXTERNAL;
    }

    WHEP_LOG(s, AV_LOG_DEBUG, "Parsing SDP answer: %s\n", answer);

    // Very light parser: scan for "m=video" and extract payload numbers, then open RTP contexts.
    const char *p = answer;
    while (p && *p) {
        const char *line_end = strchr(p, '\n');
        size_t len = line_end ? (size_t)(line_end - p) : strlen(p);
        if (len >= 2 && p[0] == 'm' && p[1] == '=') {
            // e.g., m=video 9 UDP/TLS/RTP/SAVPF 96 97
            if (!av_strncasecmp(p + 2, "video", 5)) {
                const char *pt_list = strstr(p, "SAVP");
                if (!pt_list) pt_list = strstr(p, "AVP");
                if (pt_list) {
                    // Skip profile token
                    while (*pt_list && *pt_list != ' ') pt_list++;
                    while (*pt_list == ' ') pt_list++;
                    // Iterate space-separated payload ids
                    while (*pt_list && *pt_list != '\r' && *pt_list != '\n') {
                        char numbuf[8] = {0};
                        int i = 0;
                        while (*pt_list >= '0' && *pt_list <= '9' && i < (int)sizeof(numbuf) - 1) {
                            numbuf[i++] = *pt_list++;
                        }
                        if (i > 0) {
                            int payload_type = atoi(numbuf);
                            WHEP_LOG(s, AV_LOG_DEBUG, "[GPT] Create RTP ctx for PT %d\n", payload_type);
                            if (!whep->payload_map[payload_type]) {
                                RTPDemuxContext *rtp = whep_new_rtp_context(s, payload_type);
                                if (!rtp) {
                                    WHEP_LOG(s, AV_LOG_ERROR, "Failed to create RTP context for PT %d\n", payload_type);
                                    return AVERROR_EXTERNAL;
                                }
                                whep->payload_map[payload_type] = rtp;
                            }
                        }
                        while (*pt_list == ' ') pt_list++;
                        // skip non-digits until next space or EOL
                        while (*pt_list && !(*pt_list >= '0' && *pt_list <= '9') && *pt_list != '\r' && *pt_list != '\n')
                            pt_list++;
                    }
                }
            }
        }
        p = line_end ? line_end + 1 : NULL;
    }
    return 0;
}

static RTPDemuxContext* whep_new_rtp_context(AVFormatContext *s, int payload_type)
{
    WHEPContext *whep = s->priv_data;

    AVStream *st = avformat_new_stream(s, NULL);
    if (!st) {
        WHEP_LOG(s, AV_LOG_ERROR, "Failed to allocate stream\n");
        return NULL;
    }
    st->id = payload_type;
    st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO; // focus on video in this variant
    // Leave codec_id unknown here; rtpdec will set as dynamic payload is bound.

    AVRational tb = (AVRational){1, 90000};
    st->time_base = tb;
    st->r_frame_rate = (AVRational){0, 1};

    RTPDemuxContext *rtp_ctx = ff_rtp_parse_open(s, st, 90000, NULL, NULL, 0);
    if (!rtp_ctx) {
        WHEP_LOG(s, AV_LOG_ERROR, "Failed to open RTP context\n");
        return NULL;
    }

    // Try to set dynamic protocol from SDP a= lines if available
    char line[256];
    if (whep_get_sdp_a_line(whep->video_track, line, sizeof(line), payload_type) >= 0) {
        void *payload_ctx = ff_rtp_parse_set_dynamic_protocol(s, rtp_ctx, line, st->codecpar, 90000);
        if (!payload_ctx) {
            WHEP_LOG(s, AV_LOG_ERROR, "Failed to init dynamic protocol for PT %d\n", payload_type);
        }
    }

    return rtp_ctx;
}

static void message_callback(const rtcMessage *msg, void *ptr)
{
    WHEPContext *whep = ptr;
    AVFormatContext *s = whep->fmt_ctx;

    if (!msg || msg->type != RTC_MESSAGE_BINARY || msg->size < 2) {
        WHEP_LOG(s, AV_LOG_DEBUG, "[GPT] drop non-binary or too small message\n");
        return;
    }

    const uint8_t *buf = (const uint8_t*)msg->bin.data;
    int size = (int)msg->size;

    // Classify RTP vs RTCP quickly; ignore RTCP in this simple fork
    int pt = buf[1] & 0x7F;
    if (size < 12 || (buf[1] & 0x80)) {
        // likely RTCP or invalid
        return;
    }

    RTPDemuxContext *rtp = whep->payload_map[pt];
    if (!rtp) {
        WHEP_LOG(s, AV_LOG_WARNING, "[GPT] No RTP ctx for PT %d\n", pt);
        return;
    }

    if (!whep->video_pkt)
        whep->video_pkt = av_packet_alloc();
    if (!whep->video_pkt) {
        WHEP_LOG(s, AV_LOG_ERROR, "[GPT] alloc pkt failed\n");
        return;
    }
    AVPacket *pkt = whep->video_pkt;
    av_packet_unref(pkt);

    int ret = ff_rtp_parse_packet(rtp, pkt, buf, size);
    if (ret == 0 && pkt->size > 0) {
        // enqueue complete frame
        AVPacket *cp = av_packet_alloc();
        if (!cp) return;
        if (av_packet_ref(cp, pkt) < 0) {
            av_packet_free(&cp);
            return;
        }
        pthread_mutex_lock(&whep->video_queue_mutex);
        if (!whep->video_queue)
            whep->video_queue = av_fifo_alloc2(128, sizeof(AVPacket*), AV_FIFO_FLAG_AUTO_GROW);
        if (whep->video_queue)
            av_fifo_write(whep->video_queue, &cp, 1);
        pthread_cond_signal(&whep->video_queue_cond);
        pthread_mutex_unlock(&whep->video_queue_mutex);
    }
}

static int whep_read_header(AVFormatContext *s)
{
    WHEPContext *whep = s->priv_data;
    WHEP_LOG(s, AV_LOG_INFO, "[WHEP_GPT] whep_read_header\n");

    ff_whip_whep_init_rtc_logger();

    memset(whep->payload_map, 0, sizeof(whep->payload_map));

    pthread_mutex_init(&whep->video_queue_mutex, NULL);
    pthread_cond_init(&whep->video_queue_cond, NULL);

    // Setup peer connection
    struct rtcConfiguration config = {0};
    whep->pc = rtcCreatePeerConnection(&config);
    if (whep->pc <= 0) {
        WHEP_LOG(s, AV_LOG_ERROR, "Failed to create peer connection\n");
        return AVERROR_EXTERNAL;
    }
    whep->fmt_ctx = s;
    rtcSetUserPointer(whep->pc, whep);

    int video_mline = 0; // single video
    whep->video_track = rtcAddTrack(whep->pc, video_mline);
    if (whep->video_track <= 0) {
        WHEP_LOG(s, AV_LOG_ERROR, "Failed to add video track\n");
        return AVERROR_EXTERNAL;
    }
    if (rtcSetMessageCallback(whep->video_track, message_callback) < 0) {
        WHEP_LOG(s, AV_LOG_ERROR, "Failed to set message callback\n");
        return AVERROR_EXTERNAL;
    }

    int ret = ff_whip_whep_exchange_and_set_sdp(s, whep->pc, whep->token, &whep->session_url);
    if (ret < 0) return ret;

    ret = whep_parse_sdp_and_create_contexts(s);
    if (ret < 0) return ret;

    WHEP_LOG(s, AV_LOG_INFO, "[WHEP_GPT] Created %d streams\n", s->nb_streams);
    return 0;
}

static int whep_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    WHEPContext *whep = s->priv_data;
    pthread_mutex_lock(&whep->video_queue_mutex);
    if (!whep->video_queue || av_fifo_can_read(whep->video_queue) == 0) {
        pthread_mutex_unlock(&whep->video_queue_mutex);
        return AVERROR(EAGAIN);
    }
    AVPacket *out = NULL;
    av_fifo_read(whep->video_queue, &out, 1);
    pthread_mutex_unlock(&whep->video_queue_mutex);
    if (!out)
        return AVERROR(EAGAIN);
    int ret = av_packet_ref(pkt, out);
    av_packet_free(&out);
    return ret;
}

static int whep_read_close(AVFormatContext *s)
{
    WHEPContext *whep = s->priv_data;

    if (whep->video_track > 0)
        rtcDeleteTrack(whep->video_track);
    if (whep->pc > 0)
        rtcDeletePeerConnection(whep->pc);

    for (int i = 0; i < 128; ++i) {
        if (whep->payload_map[i])
            ff_rtp_parse_close(whep->payload_map[i]);
        whep->payload_map[i] = NULL;
    }

    if (whep->video_queue) {
        AVPacket *pkt;
        while (av_fifo_can_read(whep->video_queue) > 0) {
            av_fifo_read(whep->video_queue, &pkt, 1);
            av_packet_free(&pkt);
        }
        av_fifo_freep2(&whep->video_queue);
    }
    pthread_mutex_destroy(&whep->video_queue_mutex);
    pthread_cond_destroy(&whep->video_queue_cond);

    if (whep->video_pkt)
        av_packet_free(&whep->video_pkt);

    if (whep->session_url) {
        ff_whip_whep_delete_session(s, whep->token, whep->session_url);
        av_freep(&whep->session_url);
    }
    return 0;
}

#define OFFSET(x) offsetof(WHEPContext, x)
static const AVOption whep_options[] = {
    { "token",     "WHEP auth token", OFFSET(token), AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0, AV_OPT_FLAG_DECODING_PARAM | AV_OPT_FLAG_INPUT },
    { NULL }
};

static const AVClass whep_class = {
    .class_name = "WHEP_GPT demuxer",
    .item_name  = av_default_item_name,
    .option     = whep_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

const FFInputFormat ff_whep_gpt_demuxer = {
    .p.name         = "whep_gpt",
    .p.long_name    = NULL_IF_CONFIG_SMALL("WHEP (GPT variant)"),
    .p.priv_class   = &whep_class,
    .priv_data_size = sizeof(WHEPContext),
    .read_header    = whep_read_header,
    .read_packet    = whep_read_packet,
    .read_close     = whep_read_close,
};
