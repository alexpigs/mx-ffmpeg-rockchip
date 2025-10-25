#include "config.h"
#include "config_components.h"

#if CONFIG_NETWORK

#include <errno.h>
#include <ctype.h>
#include <stdarg.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <limits.h>

#include "libavutil/avassert.h"
#include "libavutil/avstring.h"
#include "libavutil/avutil.h"
#include "libavutil/base64.h"
#include "libavutil/bprint.h"
#include "libavutil/channel_layout.h"
#include "libavutil/common.h"
#include "libavutil/error.h"
#include "libavutil/fifo.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/rational.h"
#include "libavutil/time.h"
#include "libavutil/timestamp.h"
#include "libavcodec/codec_par.h"
#include "avformat.h"
#include "avio_internal.h"
#include "demux.h"
#include "internal.h"
#include "network.h"
#include "rtp.h"
#include "rtpdec.h"
#include "url.h"
#include "http.h"

#include "rtc/rtc.h"

#define MAX_SDP_SIZE 8192
#define WHEP_MAX_TRACKS 8

typedef struct WHEPQueue {
    AVFifo *fifo;
    int abort_request;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} WHEPQueue;

struct WHEPTrack;
struct WHEPContext;

typedef struct WHEPLocalTrackBinding {
    struct WHEPContext *ctx;
    int track_id;
    char mid[64];
    struct WHEPTrack *track;
} WHEPLocalTrackBinding;

typedef struct WHEPRtpPacket {
    struct WHEPTrack *track;
    uint8_t *data;
    int size;
} WHEPRtpPacket;

typedef struct WHEPRemoteCandidate {
    char *mid;
    char *candidate;
} WHEPRemoteCandidate;

typedef struct WHEPLocalCandidate {
    char *mid;
    char *candidate;
    int is_end;
    int sent;
} WHEPLocalCandidate;

typedef struct WHEPTrack {
    struct WHEPContext *parent;
    AVStream *st;
    enum AVMediaType media_type;
    int payload_type;
    int payload_priority;
    int clock_rate;
    int track_id;
    uint32_t ssrc;
    char mid[64];
    char codec_name[64];
    char *fmtp;
    const RTPDynamicProtocolHandler *handler;
    PayloadContext *dynamic_ctx;
} WHEPTrack;

typedef struct WHEPContext {
    AVClass *class;
    AVFormatContext *fmt;
    int pc_id;
    rtcConfiguration rtc_config;
    const char **ice_servers;
    int nb_ice_servers;
    char *ice_server_list;
    char *authorization;
    int handshake_timeout;
    int ice_timeout;

    char *local_sdp;
    char *remote_sdp;
    char *resource_url;

    pthread_mutex_t state_mutex;
    pthread_cond_t state_cond;
    int gather_complete;
    int ice_connected;
    int pc_failed;
    int tracks_ready;
    int abort_flag;

    int cleaned;

    WHEPQueue rtp_queue;
    WHEPQueue pkt_queue;
    pthread_t worker_thread;
    int worker_started;
    int worker_stop;

    WHEPTrack tracks[WHEP_MAX_TRACKS];
    int nb_tracks;

    WHEPRemoteCandidate candidates[WHEP_MAX_TRACKS * 4];
    int nb_candidates;
    int64_t start_ts_ms;

    WHEPLocalCandidate local_candidates[WHEP_MAX_TRACKS * 8];
    int nb_local_candidates;
    char *ice_ufrag_local;
    char *ice_pwd_local;

    WHEPLocalTrackBinding track_bindings[WHEP_MAX_TRACKS];
    int nb_track_bindings;

    // RTP demuxer contexts indexed by payload type (0-127 for RTP)
    RTPDemuxContext *rtp_demux_by_pt[256];
} WHEPContext;

static void whep_flush_local_candidates(WHEPContext *ctx);
static void whep_extract_local_ice_credentials(WHEPContext *ctx);
static int whep_send_trickle_candidate(WHEPContext *ctx, const char *mid,
                                       const char *candidate, int is_end);

static int whep_read_close(AVFormatContext *s);

static int64_t whep_now_ms(void)
{
    return av_gettime_relative() / 1000;
}

static void whep_log(WHEPContext *ctx, int level, const char *fmt, ...)
{
    va_list ap;
    int64_t rel = 0;

    if (!ctx || !ctx->fmt)
        return;

    if (!ctx->start_ts_ms)
        ctx->start_ts_ms = whep_now_ms();

    rel = whep_now_ms() - ctx->start_ts_ms;
    av_log(ctx->fmt, level, "whep[%lld ms] ", (long long)rel);

    va_start(ap, fmt);
    av_vlog(ctx->fmt, level, fmt, ap);
    va_end(ap);

    av_log(ctx->fmt, level, "\n");
}

static void whep_log_packet(WHEPContext *ctx, WHEPTrack *track, const AVPacket *pkt,
                            const char *label)
{
    if (!ctx || !ctx->fmt || !pkt)
        return;

    AVRational tb = { 1, 90000 };
    const char *mid = "<none>";
    int stream_index = -1;

    if (track) {
        if (track->mid[0])
            mid = track->mid;
        if (track->st) {
            stream_index = track->st->index;
            tb = track->st->time_base;
        }
    }

    whep_log(ctx, AV_LOG_TRACE,
             "rtp_worker %s mid=%s stream=%d size=%d pts:%s pts_time:%s dts:%s dts_time:%s dur=%d flags=0x%x",
             label ? label : "packet",
             mid,
             stream_index,
             pkt->size,
             av_ts2str(pkt->pts),
             av_ts2timestr(pkt->pts, &tb),
             av_ts2str(pkt->dts),
             av_ts2timestr(pkt->dts, &tb),
             pkt->duration,
             pkt->flags);
}

static int whep_payload_priority(enum AVMediaType media_type, const char *encoding)
{
    if (!encoding || !*encoding)
        return INT_MIN;

    if (!strcmp(encoding, "RTX") || !strcmp(encoding, "RED") ||
        !strcmp(encoding, "ULPFEC") || !strcmp(encoding, "FLEXFEC-03") ||
        !strcmp(encoding, "TELEPHONE-EVENT") || !strcmp(encoding, "CN") ||
        !strcmp(encoding, "T140") || !strcmp(encoding, "T140RED"))
        return INT_MIN;

    switch (media_type) {
    case AVMEDIA_TYPE_VIDEO:
        if (!strcmp(encoding, "H266") || !strcmp(encoding, "VVC"))
            return 120;
        if (!strcmp(encoding, "H265") || !strcmp(encoding, "HEVC"))
            return 110;
        if (!strcmp(encoding, "AV1"))
            return 105;
        if (!strcmp(encoding, "H264"))
            return 100;
        if (!strcmp(encoding, "VP9"))
            return 90;
        if (!strcmp(encoding, "VP8"))
            return 80;
        return 60;
    case AVMEDIA_TYPE_AUDIO:
        if (!strcmp(encoding, "OPUS"))
            return 100;
        if (!strcmp(encoding, "AAC"))
            return 90;
        if (!strcmp(encoding, "MP4A-LATM") || !strcmp(encoding, "MPEG4-GENERIC"))
            return 85;
        if (!strcmp(encoding, "G722"))
            return 70;
        if (!strcmp(encoding, "PCMU") || !strcmp(encoding, "PCMA"))
            return 60;
        return 50;
    default:
        return 0;
    }
}

static void whep_release_track_payload(WHEPTrack *track)
{
    if (!track)
        return;

    av_freep(&track->fmtp);

    // Note: RTPDemuxContext is now managed at WHEPContext level by payload type,
    // not per-track. It will be cleaned up in whep_cleanup_rtp_demuxers().

    if (track->dynamic_ctx) {
        if (track->handler && track->handler->close)
            track->handler->close(track->dynamic_ctx);
        av_freep(&track->dynamic_ctx);
    }

    if (track->st && track->st->codecpar) {
        track->st->codecpar->codec_id = AV_CODEC_ID_NONE;
        track->st->codecpar->codec_tag = 0;
        track->st->codecpar->sample_rate = 0;
        av_channel_layout_uninit(&track->st->codecpar->ch_layout);
    }

    track->clock_rate = 0;
    track->payload_type = -1;
    track->payload_priority = INT_MIN;
    track->codec_name[0] = '\0';
    track->handler = NULL;
}

static int whep_queue_init(WHEPQueue *q)
{
    memset(q, 0, sizeof(*q));
    q->fifo = av_fifo_alloc2(1024, sizeof(void *), AV_FIFO_FLAG_AUTO_GROW);
    if (!q->fifo)
        return AVERROR(ENOMEM);
    if (pthread_mutex_init(&q->mutex, NULL)) {
        av_fifo_freep2(&q->fifo);
        return AVERROR(errno);
    }
    if (pthread_cond_init(&q->cond, NULL)) {
        pthread_mutex_destroy(&q->mutex);
        av_fifo_freep2(&q->fifo);
        return AVERROR(errno);
    }
    return 0;
}

static void whep_queue_flush(WHEPQueue *q, void (*free_elem)(void *))
{
    pthread_mutex_lock(&q->mutex);
    if (q->fifo) {
        void *elem;
        while (av_fifo_read(q->fifo, &elem, 1) >= 0) {
            if (free_elem && elem)
                free_elem(elem);
        }
    }
    pthread_mutex_unlock(&q->mutex);
}

static void whep_queue_destroy(WHEPQueue *q, void (*free_elem)(void *))
{
    whep_queue_flush(q, free_elem);
    av_fifo_freep2(&q->fifo);
    pthread_mutex_destroy(&q->mutex);
    pthread_cond_destroy(&q->cond);
}

static WHEPLocalTrackBinding *whep_find_binding_by_mid(WHEPContext *ctx, const char *mid)
{
    if (!ctx || !mid)
        return NULL;

    for (int i = 0; i < ctx->nb_track_bindings; i++) {
        if (!strcmp(ctx->track_bindings[i].mid, mid))
            return &ctx->track_bindings[i];
    }

    return NULL;
}

static WHEPTrack *whep_find_track_by_mid(WHEPContext *ctx, const char *mid)
{
    if (!ctx || !mid)
        return NULL;

    for (int i = 0; i < ctx->nb_tracks; i++) {
        if (!strcmp(ctx->tracks[i].mid, mid))
            return &ctx->tracks[i];
    }

    return NULL;
}

static void whep_bind_track_to_binding(WHEPContext *ctx, WHEPTrack *track)
{
    if (!ctx || !track || !track->mid[0])
        return;

    WHEPLocalTrackBinding *binding = whep_find_binding_by_mid(ctx, track->mid);
    if (!binding)
        return;

    binding->track = track;
    track->track_id = binding->track_id;
    rtcSetUserPointer(binding->track_id, binding);
}

static int whep_queue_push(WHEPQueue *q, void *elem)
{
    int ret;

    pthread_mutex_lock(&q->mutex);
    if (q->abort_request) {
        pthread_mutex_unlock(&q->mutex);
        return AVERROR_EOF;
    }
    ret = av_fifo_write(q->fifo, &elem, 1);
    if (ret < 0) {
        pthread_mutex_unlock(&q->mutex);
        return ret;
    }
    pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->mutex);
    return 0;
}

static void *whep_queue_pop(WHEPQueue *q, int block)
{
    void *elem = NULL;

    pthread_mutex_lock(&q->mutex);
    for (;;) {
        if (q->abort_request)
            break;
        if (av_fifo_can_read(q->fifo)) {
            av_fifo_read(q->fifo, &elem, 1);
            break;
        }
        if (!block)
            break;
        pthread_cond_wait(&q->cond, &q->mutex);
    }
    pthread_mutex_unlock(&q->mutex);
    return elem;
}

static void whep_queue_abort(WHEPQueue *q)
{
    pthread_mutex_lock(&q->mutex);
    q->abort_request = 1;
    pthread_cond_broadcast(&q->cond);
    pthread_mutex_unlock(&q->mutex);
}

static void whep_free_rtp_packet(void *elem)
{
    WHEPRtpPacket *pkt = elem;
    if (!pkt)
        return;
    // ff rtp模块会自动释放数据
    //av_freep(&pkt->data);
    av_free(pkt);
}

static void whep_free_avpacket(void *elem)
{
    AVPacket *pkt = elem;
    if (!pkt)
        return;
    av_packet_free(&pkt);
}

static int whep_wait_for_flag(WHEPContext *ctx, int *flag, int expected, int timeout_ms)
{
    int ret = 0;
    pthread_mutex_lock(&ctx->state_mutex);
    if (timeout_ms >= 0) {
        struct timespec ts;
#ifdef CLOCK_REALTIME
        clock_gettime(CLOCK_REALTIME, &ts);
#else
        struct timeval tv;
        gettimeofday(&tv, NULL);
        ts.tv_sec = tv.tv_sec;
        ts.tv_nsec = (long)tv.tv_usec * 1000L;
#endif
        ts.tv_sec += timeout_ms / 1000;
        ts.tv_nsec += (timeout_ms % 1000) * 1000000LL;
        if (ts.tv_nsec >= 1000000000LL) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000LL;
        }
        while (*flag != expected && !ctx->abort_flag && !ctx->pc_failed) {
            ret = pthread_cond_timedwait(&ctx->state_cond, &ctx->state_mutex, &ts);
            if (ret == ETIMEDOUT)
                break;
        }
        if (ret == ETIMEDOUT)
            ret = AVERROR(ETIMEDOUT);
        else
            ret = 0;
    } else {
        while (*flag != expected && !ctx->abort_flag && !ctx->pc_failed)
            pthread_cond_wait(&ctx->state_cond, &ctx->state_mutex);
    }
    if (*flag != expected && !ret)
        ret = AVERROR(ETIMEDOUT);
    if (ctx->pc_failed && !ret)
        ret = AVERROR(EIO);
    pthread_mutex_unlock(&ctx->state_mutex);
    return ret;
}

static void RTC_API whep_on_state_change(int pc, rtcState state, void *ptr)
{
    WHEPContext *ctx = ptr;
    pthread_mutex_lock(&ctx->state_mutex);
    if (state == RTC_CONNECTED)
        ctx->ice_connected = 1;
    else if (state == RTC_FAILED || state == RTC_CLOSED)
        ctx->pc_failed = 1;
    pthread_cond_broadcast(&ctx->state_cond);
    pthread_mutex_unlock(&ctx->state_mutex);
}

static void RTC_API whep_on_ice_state(int pc, rtcIceState state, void *ptr)
{
    WHEPContext *ctx = ptr;
    pthread_mutex_lock(&ctx->state_mutex);
    if (state == RTC_ICE_CONNECTED || state == RTC_ICE_COMPLETED)
        ctx->ice_connected = 1;
    else if (state == RTC_ICE_FAILED)
        ctx->pc_failed = 1;
    pthread_cond_broadcast(&ctx->state_cond);
    pthread_mutex_unlock(&ctx->state_mutex);
}

static void RTC_API whep_on_gathering_state(int pc, rtcGatheringState state, void *ptr)
{
    WHEPContext *ctx = ptr;
    if (state != RTC_GATHERING_COMPLETE)
        return;

    char buf[MAX_SDP_SIZE] = { 0 };
    int ret = rtcGetLocalDescription(pc, buf, sizeof(buf));
    if (ret < 0)
        return;

    pthread_mutex_lock(&ctx->state_mutex);
    av_freep(&ctx->local_sdp);
    ctx->local_sdp = av_strndup(buf, ret);
    if (ctx->local_sdp) {
        ctx->gather_complete = 1;
        whep_log(ctx, AV_LOG_TRACE, "local offer SDP:\n%s", ctx->local_sdp);
        whep_extract_local_ice_credentials(ctx);
    }
    pthread_cond_broadcast(&ctx->state_cond);
    pthread_mutex_unlock(&ctx->state_mutex);
}

static void RTC_API whep_on_candidate(int pc, const char *cand, const char *mid, void *ptr)
{
    WHEPContext *ctx = ptr;
    int index = -1;
    int is_end = !cand || !*cand;

    pthread_mutex_lock(&ctx->state_mutex);
    if (ctx->nb_local_candidates < FF_ARRAY_ELEMS(ctx->local_candidates)) {
        WHEPLocalCandidate *dst = &ctx->local_candidates[ctx->nb_local_candidates++];
        dst->candidate = cand && *cand ? av_strdup(cand) : NULL;
        dst->mid = mid ? av_strdup(mid) : NULL;
        dst->is_end = is_end;
        dst->sent = 0;
        index = ctx->nb_local_candidates - 1;
        if (ctx->fmt)
            av_log(ctx->fmt, AV_LOG_TRACE, "whep: queued local candidate mid=%s%s\n",
                   dst->mid ? dst->mid : "<none>", dst->is_end ? " (end)" : "");
    } else if (ctx->fmt) {
        av_log(ctx->fmt, AV_LOG_WARNING, "whep: too many local candidates, dropping mid=%s\n",
               mid ? mid : "<none>");
    }
    pthread_mutex_unlock(&ctx->state_mutex);

    if (index >= 0)
        whep_flush_local_candidates(ctx);
}

static void whep_trim_whitespace(char *str)
{
    char *start;
    char *end;

    if (!str)
        return;

    start = str;
    while (*start && isspace((unsigned char)*start))
        start++;

    if (start != str)
        memmove(str, start, strlen(start) + 1);

    if (!*str)
        return;

    end = str + strlen(str) - 1;
    while (end >= str && isspace((unsigned char)*end))
        *end-- = '\0';
}

static void whep_parse_ice_servers(WHEPContext *ctx)
{
    char *saveptr = NULL;
    char *entry;
    int count = 0;

    av_freep(&ctx->ice_servers);
    ctx->nb_ice_servers = 0;

    if (!ctx->ice_server_list)
        return;

    for (char *p = ctx->ice_server_list; *p; ++p)
        if (*p == ',')
            count++;
    count += 1;

    ctx->ice_servers = av_calloc(count + 1, sizeof(*ctx->ice_servers));
    if (!ctx->ice_servers)
        return;

    entry = av_strtok(ctx->ice_server_list, ",", &saveptr);
    while (entry && ctx->nb_ice_servers < count) {
        whep_trim_whitespace(entry);
        if (*entry) {
            ctx->ice_servers[ctx->nb_ice_servers++] = entry;
            if (ctx->fmt)
                av_log(ctx->fmt, AV_LOG_TRACE, "whep: configured ICE server: %s\n", entry);
        }
        entry = av_strtok(NULL, ",", &saveptr);
    }
    ctx->ice_servers[ctx->nb_ice_servers] = NULL;
}

static void whep_reset_tracks(WHEPContext *ctx)
{
    for (int i = 0; i < ctx->nb_track_bindings; i++)
        ctx->track_bindings[i].track = NULL;

    for (int i = 0; i < ctx->nb_tracks; i++) {
        WHEPTrack *track = &ctx->tracks[i];
        whep_release_track_payload(track);
        memset(track, 0, sizeof(*track));
        track->parent = ctx;
        track->payload_type = -1;
        track->payload_priority = INT_MIN;
    }
    ctx->nb_tracks = 0;
}

static int whep_http_exchange(AVFormatContext *s, WHEPContext *ctx)
{
    int ret;
    char header_buf[512];
    AVBPrint answer;
    URLContext *uc = NULL;
    AVDictionary *opts = NULL;
    const char *proto = avio_find_protocol_name(s->url);
    char *hex_body = NULL;

    if (!proto || !av_strstart(proto, "http", NULL))
        return AVERROR_PROTOCOL_NOT_FOUND;

    if (!ctx->local_sdp)
        return AVERROR(EINVAL);

    av_bprint_init(&answer, 1, MAX_SDP_SIZE);

    whep_log(ctx, AV_LOG_INFO, "HTTP exchange: sending offer (%zu bytes)", strlen(ctx->local_sdp));

    ret = av_strlcpy(header_buf, "Cache-Control: no-cache\r\nContent-Type: application/sdp\r\n", sizeof(header_buf));
    if (ret >= sizeof(header_buf)) {
        ret = AVERROR(EINVAL);
        goto end;
    }

    if (ctx->authorization) {
        int len = snprintf(header_buf + ret, sizeof(header_buf) - ret,
                           "Authorization: Bearer %s\r\n", ctx->authorization);
        if (len < 0 || len >= sizeof(header_buf) - ret) {
            ret = AVERROR(EINVAL);
            goto end;
        }
    }

    av_dict_set(&opts, "headers", header_buf, 0);
    av_dict_set_int(&opts, "chunked_post", 0, 0);

    hex_body = av_mallocz(2 * strlen(ctx->local_sdp) + 1);
    if (!hex_body) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
    ff_data_to_hex(hex_body, ctx->local_sdp, strlen(ctx->local_sdp), 0);
    av_dict_set(&opts, "post_data", hex_body, 0);

    whep_log(ctx, AV_LOG_TRACE, "HTTP exchange: opening URL %s", s->url);

    ret = ffurl_open_whitelist(&uc, s->url, AVIO_FLAG_READ_WRITE,
                               &s->interrupt_callback, &opts,
                               s->protocol_whitelist, s->protocol_blacklist, NULL);
    if (ret < 0) {
        whep_log(ctx, AV_LOG_ERROR, "HTTP exchange: failed to open URL: %s", av_err2str(ret));
        goto end;
    }

    if (ff_http_get_new_location(uc)) {
        av_freep(&ctx->resource_url);
        ctx->resource_url = av_strdup(ff_http_get_new_location(uc));
        if (!ctx->resource_url) {
            ret = AVERROR(ENOMEM);
            goto end;
        }
        whep_log(ctx, AV_LOG_INFO, "HTTP exchange: redirected to %s", ctx->resource_url);
    }

    whep_log(ctx, AV_LOG_TRACE, "HTTP exchange: reading answer");
    while (1) {
        char buf[1024];
        ret = ffurl_read(uc, buf, sizeof(buf));
        if (ret == AVERROR_EOF) {
            ret = 0;
            break;
        }
        if (ret <= 0) {
            whep_log(ctx, AV_LOG_ERROR, "HTTP exchange: read failed: %s", av_err2str(ret));
            goto end;
        }
        av_bprintf(&answer, "%.*s", ret, buf);
        if (!av_bprint_is_complete(&answer)) {
            ret = AVERROR(EIO);
            goto end;
        }
    }

    if (answer.len == 0 || !av_strstart(answer.str, "v=", NULL)) {
        ret = AVERROR_INVALIDDATA;
        goto end;
    }

    av_freep(&ctx->remote_sdp);
    ctx->remote_sdp = av_strdup(answer.str);
    if (!ctx->remote_sdp) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    whep_log(ctx, AV_LOG_INFO, "HTTP exchange: received answer (%zu bytes)", strlen(ctx->remote_sdp));
    whep_log(ctx, AV_LOG_TRACE, "HTTP exchange: answer SDP begins with: %.64s", ctx->remote_sdp);

    if (!ctx->resource_url) {
        ctx->resource_url = av_strdup(s->url);
        if (!ctx->resource_url) {
            ret = AVERROR(ENOMEM);
            goto end;
        }
        whep_log(ctx, AV_LOG_TRACE, "HTTP exchange: using request URL as resource %s", ctx->resource_url);
    }

    whep_flush_local_candidates(ctx);

    ret = 0;

end:
    if (ret < 0)
        whep_log(ctx, AV_LOG_ERROR, "HTTP exchange: failed: %s", av_err2str(ret));
    if (uc)
        ffurl_closep(&uc);
    av_dict_free(&opts);
    av_bprint_finalize(&answer, NULL);
    av_free(hex_body);
    return ret;
}

static void whep_add_candidate(WHEPContext *ctx, const char *mid, const char *cand)
{
    if (ctx->nb_candidates >= FF_ARRAY_ELEMS(ctx->candidates))
        return;
    WHEPRemoteCandidate *dst = &ctx->candidates[ctx->nb_candidates++];
    dst->mid = av_strdup(mid ? mid : "");
    dst->candidate = av_strdup(cand ? cand : "");
}

static void whep_extract_local_ice_credentials(WHEPContext *ctx)
{
    const char *line;

    av_freep(&ctx->ice_ufrag_local);
    av_freep(&ctx->ice_pwd_local);

    if (!ctx->local_sdp)
        return;

    line = ctx->local_sdp;
    while (*line) {
        const char *eol = strchr(line, '\n');
        size_t len = eol ? (size_t)(eol - line) : strlen(line);
        char buf[256];

        if (len >= sizeof(buf))
            len = sizeof(buf) - 1;
        memcpy(buf, line, len);
        buf[len] = '\0';
        if (len && buf[len - 1] == '\r')
            buf[len - 1] = '\0';

        if (!ctx->ice_ufrag_local && !av_strncasecmp(buf, "a=ice-ufrag:", 11)) {
            ctx->ice_ufrag_local = av_strdup(buf + 11);
        } else if (!ctx->ice_pwd_local && !av_strncasecmp(buf, "a=ice-pwd:", 9)) {
            ctx->ice_pwd_local = av_strdup(buf + 9);
        }

        if (ctx->ice_ufrag_local && ctx->ice_pwd_local)
            break;

        line = eol ? eol + 1 : line + len;
    }
}

static int whep_send_trickle_candidate(WHEPContext *ctx, const char *mid,
                                       const char *candidate, int is_end)
{
    AVDictionary *opts = NULL;
    URLContext *uc = NULL;
    AVBPrint body;
    char header[512];
    char *payload = NULL;
    char *hex_body = NULL;
    int ret;

    if (!ctx || ctx->abort_flag)
        return AVERROR_EXIT;

    if (!ctx->resource_url) {
        whep_log(ctx, AV_LOG_TRACE, "defer trickle candidate mid=%s: resource URL not ready",
                 mid ? mid : "<none>");
        return AVERROR(EAGAIN);
    }

    av_bprint_init(&body, 128, AV_BPRINT_SIZE_AUTOMATIC);

    if (mid && *mid)
        av_bprintf(&body, "a=mid:%s\r\n", mid);
    if (ctx->ice_ufrag_local)
        av_bprintf(&body, "a=ice-ufrag:%s\r\n", ctx->ice_ufrag_local);
    if (ctx->ice_pwd_local)
        av_bprintf(&body, "a=ice-pwd:%s\r\n", ctx->ice_pwd_local);
    if (is_end)
        av_bprintf(&body, "a=end-of-candidates\r\n");
    else if (candidate && *candidate)
        av_bprintf(&body, "a=candidate:%s\r\n", candidate);

    if (!av_bprint_is_complete(&body)) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    av_bprint_finalize(&body, &payload);
    if (!payload) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    hex_body = av_mallocz(2 * strlen(payload) + 1);
    if (!hex_body) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
    ff_data_to_hex(hex_body, payload, strlen(payload), 0);

    ret = av_strlcpy(header, "Cache-Control: no-cache\r\nContent-Type: application/trickle-ice-sdpfrag\r\n",
                     sizeof(header));
    if (ret >= sizeof(header)) {
        ret = AVERROR(EINVAL);
        goto end;
    }
    if (ctx->authorization) {
        int len = snprintf(header + ret, sizeof(header) - ret,
                           "Authorization: Bearer %s\r\n", ctx->authorization);
        if (len < 0 || len >= sizeof(header) - ret) {
            ret = AVERROR(EINVAL);
            goto end;
        }
    }

    av_dict_set(&opts, "method", "PATCH", 0);
    av_dict_set(&opts, "headers", header, 0);
    av_dict_set_int(&opts, "chunked_post", 0, 0);
    av_dict_set(&opts, "post_data", hex_body, 0);

    whep_log(ctx, AV_LOG_VERBOSE, "sending trickle candidate mid=%s%s", mid ? mid : "<none>",
             is_end ? " (end)" : "");

    ret = ffurl_open_whitelist(&uc, ctx->resource_url, AVIO_FLAG_READ_WRITE,
                               ctx->fmt ? &ctx->fmt->interrupt_callback : NULL, &opts,
                               ctx->fmt ? ctx->fmt->protocol_whitelist : NULL,
                               ctx->fmt ? ctx->fmt->protocol_blacklist : NULL, NULL);
    if (ret < 0)
        goto end;

    if (ret >= 0)
        ret = 0;

end:
    if (uc)
        ffurl_closep(&uc);
    av_dict_free(&opts);
    av_freep(&hex_body);
    av_free(payload);
    return ret;
}

static void whep_flush_local_candidates(WHEPContext *ctx)
{
    for (;;) {
        char *mid = NULL;
        char *cand = NULL;
        int is_end = 0;
        int index = -1;
        int need_retry = 0;
        int ret;
        int need_mid = 0;
        int need_cand = 0;
        const char *orig_mid = NULL;

        pthread_mutex_lock(&ctx->state_mutex);
        for (int i = 0; i < ctx->nb_local_candidates; i++) {
            if (!ctx->local_candidates[i].sent) {
                index = i;
                is_end = ctx->local_candidates[i].is_end;
                need_mid = ctx->local_candidates[i].mid != NULL;
                need_cand = ctx->local_candidates[i].candidate != NULL;
                orig_mid = ctx->local_candidates[i].mid;
                if (need_mid)
                    mid = av_strdup(ctx->local_candidates[i].mid);
                if (need_cand)
                    cand = av_strdup(ctx->local_candidates[i].candidate);
                break;
            }
        }
        pthread_mutex_unlock(&ctx->state_mutex);

        if (index < 0)
            break;

        if ((need_mid && !mid) || (need_cand && !cand && !is_end)) {
            whep_log(ctx, AV_LOG_ERROR, "OOM while duplicating candidate mid=%s",
                     orig_mid ? orig_mid : "<none>");
            av_free(mid);
            av_free(cand);
            break;
        }

        if (!cand && !is_end) {
            av_free(mid);
            continue;
        }

        ret = whep_send_trickle_candidate(ctx, mid, cand, is_end);
        if (ret < 0) {
            if (ret != AVERROR(EAGAIN))
                whep_log(ctx, AV_LOG_WARNING, "failed to send candidate mid=%s: %s",
                         mid ? mid : "<none>", av_err2str(ret));
            need_retry = 1;
        }

        if (!ret) {
            pthread_mutex_lock(&ctx->state_mutex);
            ctx->local_candidates[index].sent = 1;
            av_freep(&ctx->local_candidates[index].mid);
            av_freep(&ctx->local_candidates[index].candidate);
            pthread_mutex_unlock(&ctx->state_mutex);
        }

        av_free(mid);
        av_free(cand);

        if (need_retry)
            break;
    }
}

static int whep_parse_sdp(AVFormatContext *s)
{
    WHEPContext *ctx = s->priv_data;
    const char *line = ctx->remote_sdp;
    WHEPTrack *track = NULL;
    int ret = 0;

    if (!line)
        return AVERROR(EINVAL);

    whep_reset_tracks(ctx);
    for (int i = 0; i < ctx->nb_candidates; i++) {
        av_freep(&ctx->candidates[i].mid);
        av_freep(&ctx->candidates[i].candidate);
    }
    ctx->nb_candidates = 0;

    while (*line) {
        const char *eol = strchr(line, '\n');
        size_t len = eol ? (size_t)(eol - line) : strlen(line);
        char buf[1024];

        if (len >= sizeof(buf))
            return AVERROR_INVALIDDATA;

        memcpy(buf, line, len);
        buf[len] = '\0';
        if (len && buf[len - 1] == '\r') {
            buf[--len] = '\0';
        }
        if (!len)
            goto next_line;

        if (buf[0] == 'm' && buf[1] == '=') {
            char media[16];
            int payload = 0;

            if (ctx->nb_tracks >= WHEP_MAX_TRACKS)
                return AVERROR(ENOSPC);
            if (sscanf(buf, "m=%15s %*s %*s %d", media, &payload) < 2)
                return AVERROR_INVALIDDATA;

            track = &ctx->tracks[ctx->nb_tracks++];
            memset(track, 0, sizeof(*track));
            track->parent = ctx;
            track->payload_type = payload;
            track->payload_priority = INT_MIN;

            if (!strcmp(media, "audio"))
                track->media_type = AVMEDIA_TYPE_AUDIO;
            else if (!strcmp(media, "video"))
                track->media_type = AVMEDIA_TYPE_VIDEO;
            else
                track->media_type = AVMEDIA_TYPE_DATA;

            track->st = avformat_new_stream(s, NULL);
            if (!track->st)
                return AVERROR(ENOMEM);
            track->st->codecpar->codec_type = track->media_type;
            track->st->codecpar->codec_id = AV_CODEC_ID_NONE;
            track->st->codecpar->codec_tag = 0;
            track->st->id = track->payload_type;

            av_log(s, AV_LOG_INFO, "whep: discovered %s track payload %d\n",
                   av_get_media_type_string(track->media_type), track->payload_type);
        } else if (buf[0] == 'a' && buf[1] == '=') {
            const char *value = buf + 2;

            if (!track)
                goto next_line;

            if (!av_strncasecmp(value, "mid:", 4)) {
                av_strlcpy(track->mid, value + 4, sizeof(track->mid));
                whep_log(ctx, AV_LOG_TRACE, "track mid=%s", track->mid);
                whep_bind_track_to_binding(ctx, track);
            } else if (!av_strncasecmp(value, "rtpmap:", 7)) {
                int pt = 0, clock = 0, channels = 0;
                char encoding[64] = { 0 };
                const char *fmt = value + 7;
                int priority;

                if (sscanf(fmt, "%d %63[^/]/%d/%d", &pt, encoding, &clock, &channels) < 3) {
                    if (sscanf(fmt, "%d %63[^/]/%d", &pt, encoding, &clock) < 3)
                        return AVERROR_INVALIDDATA;
                }

                for (char *p = encoding; *p; ++p)
                    *p = av_toupper(*p);

                priority = whep_payload_priority(track->media_type, encoding);
                if (priority == INT_MIN) {
                    whep_log(ctx, AV_LOG_TRACE,
                             "ignoring auxiliary payload %d (%s) for mid=%s",
                             pt, encoding, track->mid);
                    goto next_line;
                }

                if (track->payload_priority != INT_MIN && priority < track->payload_priority) {
                    whep_log(ctx, AV_LOG_TRACE,
                             "ignoring lower-priority payload %d (%s) for mid=%s (selected payload %d)",
                             pt, encoding, track->mid, track->payload_type);
                    goto next_line;
                }

                if (track->payload_priority != INT_MIN && priority == track->payload_priority &&
                    track->payload_type != pt) {
                    whep_log(ctx, AV_LOG_TRACE,
                             "keeping payload %d for mid=%s over alternative %d (%s)",
                             track->payload_type, track->mid, pt, encoding);
                    goto next_line;
                }

                if (track->payload_priority != INT_MIN && track->payload_type == pt)
                    goto next_line;

                whep_release_track_payload(track);
                track->payload_priority = priority;
                track->payload_type = pt;
                av_strlcpy(track->codec_name, encoding, sizeof(track->codec_name));
                track->clock_rate = clock;
                track->st->time_base = av_make_q(1, track->clock_rate ? track->clock_rate : 90000);
                track->st->id = track->payload_type;
                track->st->codecpar->codec_tag = 0;
                track->st->codecpar->codec_type = track->media_type;
                track->st->codecpar->codec_id = AV_CODEC_ID_NONE;

                switch (track->media_type) {
                case AVMEDIA_TYPE_VIDEO:
                    if (!strcmp(encoding, "H264")) {
                        track->st->codecpar->codec_id = AV_CODEC_ID_H264;
                        ffstream(track->st)->need_parsing = AVSTREAM_PARSE_FULL;
                    } else if (!strcmp(encoding, "H265") || !strcmp(encoding, "HEVC")) {
                        track->st->codecpar->codec_id = AV_CODEC_ID_HEVC;
                        ffstream(track->st)->need_parsing = AVSTREAM_PARSE_FULL;
                    } else if (!strcmp(encoding, "H266") || !strcmp(encoding, "VVC"))
                        track->st->codecpar->codec_id = AV_CODEC_ID_VVC;
                    else if (!strcmp(encoding, "AV1"))
                        track->st->codecpar->codec_id = AV_CODEC_ID_AV1;
                    else if (!strcmp(encoding, "VP9"))
                        track->st->codecpar->codec_id = AV_CODEC_ID_VP9;
                    else if (!strcmp(encoding, "VP8"))
                        track->st->codecpar->codec_id = AV_CODEC_ID_VP8;
                    break;
                case AVMEDIA_TYPE_AUDIO:
                    if (!channels)
                        channels = 2;
                    av_channel_layout_uninit(&track->st->codecpar->ch_layout);
                    av_channel_layout_default(&track->st->codecpar->ch_layout, channels);
                    track->st->codecpar->sample_rate = clock;
                    if (!strcmp(encoding, "OPUS"))
                        track->st->codecpar->codec_id = AV_CODEC_ID_OPUS;
                    else if (!strcmp(encoding, "AAC") || !strcmp(encoding, "MPEG4-GENERIC"))
                        track->st->codecpar->codec_id = AV_CODEC_ID_AAC;
                    else if (!strcmp(encoding, "G722"))
                        track->st->codecpar->codec_id = AV_CODEC_ID_ADPCM_G722;
                    else if (!strcmp(encoding, "PCMU"))
                        track->st->codecpar->codec_id = AV_CODEC_ID_PCM_MULAW;
                    else if (!strcmp(encoding, "PCMA"))
                        track->st->codecpar->codec_id = AV_CODEC_ID_PCM_ALAW;
                    break;
                default:
                    break;
                }

                track->handler = ff_rtp_handler_find_by_name(track->codec_name,
                                                             track->st->codecpar->codec_type);
                if (track->handler && track->handler->priv_data_size) {
                    track->dynamic_ctx = av_mallocz(track->handler->priv_data_size);
                    if (!track->dynamic_ctx)
                        return AVERROR(ENOMEM);
                }
                if (track->handler && track->dynamic_ctx && track->handler->init) {
                    int init_ret = track->handler->init(s, track->st->index, track->dynamic_ctx);
                    if (init_ret < 0) {
                        if (track->handler->close)
                            track->handler->close(track->dynamic_ctx);
                        av_freep(&track->dynamic_ctx);
                        track->handler = NULL;
                    }
                }

                // Create RTPDemuxContext for this payload type if not already created
                pt = track->payload_type;
                if (pt >= 0 && pt < 256 && !ctx->rtp_demux_by_pt[pt]) {
                    ctx->rtp_demux_by_pt[pt] = ff_rtp_parse_open(s, track->st, pt,
                                                                   RTP_REORDER_QUEUE_DEFAULT_SIZE);
                    if (!ctx->rtp_demux_by_pt[pt])
                        return AVERROR(ENOMEM);

                    if (track->handler)
                        ff_rtp_parse_set_dynamic_protocol(ctx->rtp_demux_by_pt[pt], track->dynamic_ctx,
                                                           track->handler);

                    whep_log(ctx, AV_LOG_INFO, "created RTP demuxer for payload type %d (mid=%s, codec=%s)",
                             pt, track->mid, track->codec_name);
                } else if (pt >= 0 && pt < 256) {
                    whep_log(ctx, AV_LOG_TRACE, "reusing existing RTP demuxer for payload type %d (mid=%s)",
                             pt, track->mid);
                }

                whep_log(ctx, AV_LOG_INFO, "track mid=%s uses codec %s (payload %d)",
                         track->mid, track->codec_name, track->payload_type);
            } else if (!av_strncasecmp(value, "fmtp:", 5)) {
                int fmtp_pt = -1;
                const char *params = strchr(value, ' ');

                if (sscanf(value + 5, "%d", &fmtp_pt) != 1)
                    fmtp_pt = -1;

                if (fmtp_pt == track->payload_type) {
                    av_freep(&track->fmtp);
                    if (params)
                        track->fmtp = av_strdup(params + 1);
                    if (track->handler && track->handler->parse_sdp_a_line)
                        track->handler->parse_sdp_a_line(s, track->st->index,
                                                         track->dynamic_ctx, value);
                    whep_log(ctx, AV_LOG_TRACE, "fmtp mid=%s: %s",
                             track->mid, track->fmtp ? track->fmtp : "<none>");
                }
            } else if (!av_strncasecmp(value, "ssrc:", 5)) {
                unsigned long ssrc = strtoul(value + 5, NULL, 10);
                track->ssrc = (uint32_t)ssrc;
                int pt = track->payload_type;
                if (pt >= 0 && pt < 256 && ctx->rtp_demux_by_pt[pt])
                    ctx->rtp_demux_by_pt[pt]->ssrc = track->ssrc;
                whep_log(ctx, AV_LOG_TRACE, "track %s ssrc %u", track->mid, track->ssrc);
            } else if (!av_strncasecmp(value, "candidate:", 10)) {
                const char *cand = value + 10;
                whep_add_candidate(ctx, track->mid, cand);
                whep_log(ctx, AV_LOG_TRACE, "cached remote candidate for %s", track->mid);
            } else if (!av_strcasecmp(value, "end-of-candidates")) {
                whep_add_candidate(ctx, track->mid, NULL);
                whep_log(ctx, AV_LOG_TRACE, "end of candidates for %s", track->mid);
            }
        }

    next_line:
        line = eol ? eol + 1 : line + len;
    }

    for (int i = 0; i < ctx->nb_tracks; i++) {
        WHEPTrack *t = &ctx->tracks[i];
        int pt = t->payload_type;
        if (pt < 0 || pt >= 256 || !ctx->rtp_demux_by_pt[pt]) {
            ret = AVERROR(EINVAL);
            break;
        }
        if (t->handler && t->handler->need_keyframe)
            t->handler->need_keyframe(t->dynamic_ctx);
    }

    if (!ret)
        av_log(s, AV_LOG_INFO, "whep: parsed %d remote tracks\n", ctx->nb_tracks);

    return ret;
}

static void RTC_API whep_on_track_open(int id, void *ptr)
{
    WHEPLocalTrackBinding *binding = rtcGetUserPointer(id);
    if (!binding || !binding->ctx)
        return;
    WHEPContext *ctx = binding->ctx;
    WHEPTrack *track = binding->track;
    if (!track) {
        track = whep_find_track_by_mid(ctx, binding->mid);
        if (track) {
            binding->track = track;
            track->track_id = binding->track_id;
        } else {
            return;
        }
    }
    pthread_mutex_lock(&ctx->state_mutex);
    ctx->tracks_ready++;
    pthread_cond_broadcast(&ctx->state_cond);
    pthread_mutex_unlock(&ctx->state_mutex);
    if (ctx->fmt)
        av_log(ctx->fmt, AV_LOG_INFO, "whep: track mid=%s opened\n", track->mid);
}

static void *whep_rtp_worker(void *opaque)
{
    WHEPContext *ctx = opaque;

    while (!ctx->worker_stop) {
        WHEPRtpPacket *pkt = whep_queue_pop(&ctx->rtp_queue, 1);
        if (!pkt) {
            if (ctx->rtp_queue.abort_request)
                break;
            continue;
        }

        WHEPTrack *track = pkt->track;
        if (!track) {
            whep_free_rtp_packet(pkt);
            continue;
        }

        uint8_t *buf = pkt->data;
        int len = pkt->size;

        // Extract payload type from RTP packet header
        // RTP header: V(2) P(1) X(1) CC(4) | M(1) PT(7)
        if (len < 12) {
            whep_log(ctx, AV_LOG_WARNING, "whep: packet too short (%d bytes) for mid=%s", len, track->mid);
            whep_free_rtp_packet(pkt);
            continue;
        }

        int pt = buf[1] & 0x7f;  // Extract 7-bit payload type
        RTPDemuxContext *rtp_ctx = NULL;
        if (pt >= 0 && pt < 256)
            rtp_ctx = ctx->rtp_demux_by_pt[pt];

        if (!rtp_ctx) {
            whep_log(ctx, AV_LOG_WARNING, "whep: no RTP demuxer for PT=%d (mid=%s)", pt, track->mid);
            whep_free_rtp_packet(pkt);
            continue;
        }

        // Treat RTCP packets separately so timing/quality stats get updated
        if (len >= 2 && (buf[0] & 0xc0) == (RTP_VERSION << 6) && RTP_PT_IS_RTCP(buf[1])) {
            AVPacket rtcp_pkt = { 0 };
            int ret = ff_rtp_parse_packet(rtp_ctx, &rtcp_pkt, &buf, len);

            whep_log(ctx, AV_LOG_VERBOSE, "whep: RTCP packet received (pt=%d, %d bytes) mid=%s ret=%d",
                     pt, len, track->mid, ret);

            if (ret == -RTCP_BYE)
                whep_log(ctx, AV_LOG_WARNING, "whep: peer sent RTCP BYE for mid=%s", track->mid);

            av_packet_unref(&rtcp_pkt);
            whep_free_rtp_packet(pkt);
            continue;
        }

        //log incoming RTP packet
        whep_log(ctx, AV_LOG_TRACE, "whep: received RTP packet %d bytes for mid=%s (PT=%d)",
                 len, track->mid, pt);
        AVPacket avpkt = { 0 };
        int ret = ff_rtp_parse_packet(rtp_ctx, &avpkt, &buf, len);
        whep_log(ctx, AV_LOG_TRACE, "whep: parsed RTP packet for mid=%s (PT=%d), ret=%d, size=%d",
                 track->mid, pt, ret, avpkt.size);
        if (ret >= 0 && avpkt.size > 0) {
            whep_log_packet(ctx, track, &avpkt, "primary");
            AVPacket *dst = av_packet_alloc();
            if (dst) {
                av_packet_move_ref(dst, &avpkt);
                dst->stream_index = track->st->index;
                if (whep_queue_push(&ctx->pkt_queue, dst) < 0)
                    av_packet_free(&dst);
                else
                    whep_log_packet(ctx, track, dst, "assembled");
            } else {
                av_packet_unref(&avpkt);
            }
        } else {
            av_packet_unref(&avpkt);
        }
        av_packet_unref(&avpkt);

        while (ret > 0) {
            AVPacket pending = { 0 };
            ret = ff_rtp_parse_packet(rtp_ctx, &pending, NULL, 0);
            if (ret >= 0 && pending.size > 0) {
                whep_log_packet(ctx, track, &pending, "drain");
                AVPacket *dst = av_packet_alloc();
                if (dst) {
                    av_packet_move_ref(dst, &pending);
                    dst->stream_index = track->st->index;
                    if (whep_queue_push(&ctx->pkt_queue, dst) < 0)
                        av_packet_free(&dst);
                    else
                        whep_log_packet(ctx, track, dst, "assembled");
                } else {
                    av_packet_unref(&pending);
                }
            }
            av_packet_unref(&pending);
        }

        whep_free_rtp_packet(pkt);
    }

    return NULL;
}

static void RTC_API whep_on_track_message(int id, const char *message, int size, void *ptr)
{
    WHEPLocalTrackBinding *binding = rtcGetUserPointer(id);
    if (!binding || size <= 0)
        return;
    WHEPContext *ctx = binding->ctx;
    if (!ctx)
        return;
    WHEPTrack *track = binding->track;
    if (!track) {
        track = whep_find_track_by_mid(ctx, binding->mid);
        if (track) {
            binding->track = track;
            track->track_id = binding->track_id;
        } else {
            return;
        }
    }
    WHEPRtpPacket *pkt = av_mallocz(sizeof(*pkt));
    if (!pkt)
        return;
    pkt->track = track;
    pkt->data = av_memdup(message, size);
    pkt->size = size;
    if (!pkt->data || whep_queue_push(&ctx->rtp_queue, pkt) < 0) {
        whep_free_rtp_packet(pkt);
        return;
    }
    if (ctx->fmt)
        av_log(ctx->fmt, AV_LOG_TRACE, "whep: queued %d bytes for mid=%s\n", size, track->mid);
}

static void RTC_API whep_on_track(int pc, int tr, void *ptr)
{
    WHEPContext *ctx = ptr;
    char mid[64] = { 0 };
    if (rtcGetTrackMid(tr, mid, sizeof(mid)) < 0)
        return;

    pthread_mutex_lock(&ctx->state_mutex);
    for (int i = 0; i < ctx->nb_tracks; i++) {
        WHEPTrack *track = &ctx->tracks[i];
        if (track->mid[0] && !strcmp(track->mid, mid)) {
            WHEPLocalTrackBinding *binding = whep_find_binding_by_mid(ctx, mid);
            if (binding) {
                binding->track = track;
                binding->track_id = tr;
                rtcSetUserPointer(tr, binding);
            }
            track->track_id = tr;
            whep_log(ctx, AV_LOG_TRACE, "track callback resolved mid=%s (id=%d)", track->mid, tr);

            if (ctx->fmt)
                av_log(ctx->fmt, AV_LOG_INFO, "whep: remote track ready mid=%s (id=%d)\n",
                       track->mid, tr);
            break;
        }
    }
    pthread_mutex_unlock(&ctx->state_mutex);
}

static int whep_start_worker(WHEPContext *ctx)
{
    if (ctx->worker_started)
        return 0;
    ctx->worker_stop = 0;
    if (pthread_create(&ctx->worker_thread, NULL, whep_rtp_worker, ctx))
        return AVERROR(errno);
    ctx->worker_started = 1;
    return 0;
}

static void whep_stop_worker(WHEPContext *ctx)
{
    if (!ctx->worker_started)
        return;
    ctx->worker_stop = 1;
    whep_queue_abort(&ctx->rtp_queue);
    pthread_join(ctx->worker_thread, NULL);
    ctx->worker_started = 0;
}

static int whep_add_track_template(WHEPContext *ctx, const char *sdp, const char *mid)
{
    WHEPLocalTrackBinding *binding;
    int track_id;

    if (!ctx || !sdp || !mid)
        return AVERROR(EINVAL);

    track_id = rtcAddTrack(ctx->pc_id, sdp);
    if (track_id < 0)
        return AVERROR_EXTERNAL;

    if (ctx->nb_track_bindings >= WHEP_MAX_TRACKS)
        return AVERROR(ENOSPC);

    binding = &ctx->track_bindings[ctx->nb_track_bindings++];
    memset(binding, 0, sizeof(*binding));
    binding->ctx = ctx;
    binding->track_id = track_id;
    av_strlcpy(binding->mid, mid, sizeof(binding->mid));
    binding->track = NULL;

    rtcSetUserPointer(track_id, binding);
    rtcSetMessageCallback(track_id, whep_on_track_message);
    rtcSetOpenCallback(track_id, whep_on_track_open);

    whep_log(ctx, AV_LOG_TRACE, "registered callbacks for template mid=%s (track=%d)", mid, track_id);

    return 0;
}

static int whep_add_media_tracks(WHEPContext *ctx)
{
    static const char audio_track_sdp[] =
        "m=audio 9 UDP/TLS/RTP/SAVPF 111 63 9 0 8 13 110 126\r\n"
        "c=IN IP4 0.0.0.0\r\n"
        "a=rtcp:9 IN IP4 0.0.0.0\r\n"
        "a=mid:0\r\n"
        "a=extmap:1 urn:ietf:params:rtp-hdrext:ssrc-audio-level\r\n"
        "a=extmap:2 http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time\r\n"
        "a=extmap:3 http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01\r\n"
        "a=extmap:4 urn:ietf:params:rtp-hdrext:sdes:mid\r\n"
        "a=recvonly\r\n"
        "a=rtcp-mux\r\n"
        "a=rtcp-rsize\r\n"
        "a=rtpmap:111 opus/48000/2\r\n"
        "a=rtcp-fb:111 transport-cc\r\n"
        "a=fmtp:111 minptime=10;useinbandfec=1\r\n"
        "a=rtpmap:63 red/48000/2\r\n"
        "a=fmtp:63 111/111\r\n"
        "a=rtpmap:9 G722/8000\r\n"
        "a=rtpmap:0 PCMU/8000\r\n"
        "a=rtpmap:8 PCMA/8000\r\n"
        "a=rtpmap:13 CN/8000\r\n"
        "a=rtpmap:110 telephone-event/48000\r\n"
        "a=rtpmap:126 telephone-event/8000\r\n";

    static const char video_track_sdp[] =
        "m=video 9 UDP/TLS/RTP/SAVPF 96 97 98 99 100 101 35 36 37 38 103 104 107 108 109 114 115 116 117 118 39 40 41 42 43 44 45 46 47 48 119 120 121 49\r\n"
        "c=IN IP4 0.0.0.0\r\n"
        "a=rtcp:9 IN IP4 0.0.0.0\r\n"
        "a=mid:1\r\n"
        "a=extmap:14 urn:ietf:params:rtp-hdrext:toffset\r\n"
        "a=extmap:2 http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time\r\n"
        "a=extmap:13 urn:3gpp:video-orientation\r\n"
        "a=extmap:3 http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01\r\n"
        "a=extmap:5 http://www.webrtc.org/experiments/rtp-hdrext/playout-delay\r\n"
        "a=extmap:6 http://www.webrtc.org/experiments/rtp-hdrext/video-content-type\r\n"
        "a=extmap:7 http://www.webrtc.org/experiments/rtp-hdrext/video-timing\r\n"
        "a=extmap:8 http://www.webrtc.org/experiments/rtp-hdrext/color-space\r\n"
        "a=extmap:4 urn:ietf:params:rtp-hdrext:sdes:mid\r\n"
        "a=extmap:10 urn:ietf:params:rtp-hdrext:sdes:rtp-stream-id\r\n"
        "a=extmap:11 urn:ietf:params:rtp-hdrext:sdes:repaired-rtp-stream-id\r\n"
        "a=recvonly\r\n"
        "a=rtcp-mux\r\n"
        "a=rtcp-rsize\r\n"
        "a=rtpmap:96 VP8/90000\r\n"
        "a=rtcp-fb:96 goog-remb\r\n"
        "a=rtcp-fb:96 transport-cc\r\n"
        "a=rtcp-fb:96 ccm fir\r\n"
        "a=rtcp-fb:96 nack\r\n"
        "a=rtcp-fb:96 nack pli\r\n"
        "a=rtpmap:97 rtx/90000\r\n"
        "a=fmtp:97 apt=96\r\n"
        "a=rtpmap:98 VP9/90000\r\n"
        "a=rtcp-fb:98 goog-remb\r\n"
        "a=rtcp-fb:98 transport-cc\r\n"
        "a=rtcp-fb:98 ccm fir\r\n"
        "a=rtcp-fb:98 nack\r\n"
        "a=rtcp-fb:98 nack pli\r\n"
        "a=fmtp:98 profile-id=0\r\n"
        "a=rtpmap:99 rtx/90000\r\n"
        "a=fmtp:99 apt=98\r\n"
        "a=rtpmap:100 VP9/90000\r\n"
        "a=rtcp-fb:100 goog-remb\r\n"
        "a=rtcp-fb:100 transport-cc\r\n"
        "a=rtcp-fb:100 ccm fir\r\n"
        "a=rtcp-fb:100 nack\r\n"
        "a=rtcp-fb:100 nack pli\r\n"
        "a=fmtp:100 profile-id=2\r\n"
        "a=rtpmap:101 rtx/90000\r\n"
        "a=fmtp:101 apt=100\r\n"
        "a=rtpmap:35 VP9/90000\r\n"
        "a=rtcp-fb:35 goog-remb\r\n"
        "a=rtcp-fb:35 transport-cc\r\n"
        "a=rtcp-fb:35 ccm fir\r\n"
        "a=rtcp-fb:35 nack\r\n"
        "a=rtcp-fb:35 nack pli\r\n"
        "a=fmtp:35 profile-id=1\r\n"
        "a=rtpmap:36 rtx/90000\r\n"
        "a=fmtp:36 apt=35\r\n"
        "a=rtpmap:37 VP9/90000\r\n"
        "a=rtcp-fb:37 goog-remb\r\n"
        "a=rtcp-fb:37 transport-cc\r\n"
        "a=rtcp-fb:37 ccm fir\r\n"
        "a=rtcp-fb:37 nack\r\n"
        "a=rtcp-fb:37 nack pli\r\n"
        "a=fmtp:37 profile-id=3\r\n"
        "a=rtpmap:38 rtx/90000\r\n"
        "a=fmtp:38 apt=37\r\n"
        "a=rtpmap:103 H264/90000\r\n"
        "a=rtcp-fb:103 goog-remb\r\n"
        "a=rtcp-fb:103 transport-cc\r\n"
        "a=rtcp-fb:103 ccm fir\r\n"
        "a=rtcp-fb:103 nack\r\n"
        "a=rtcp-fb:103 nack pli\r\n"
        "a=fmtp:103 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42001f\r\n"
        "a=rtpmap:104 rtx/90000\r\n"
        "a=fmtp:104 apt=103\r\n"
        "a=rtpmap:107 H264/90000\r\n"
        "a=rtcp-fb:107 goog-remb\r\n"
        "a=rtcp-fb:107 transport-cc\r\n"
        "a=rtcp-fb:107 ccm fir\r\n"
        "a=rtcp-fb:107 nack\r\n"
        "a=rtcp-fb:107 nack pli\r\n"
        "a=fmtp:107 level-asymmetry-allowed=1;packetization-mode=0;profile-level-id=42001f\r\n"
        "a=rtpmap:108 rtx/90000\r\n"
        "a=fmtp:108 apt=107\r\n"
        "a=rtpmap:109 H264/90000\r\n"
        "a=rtcp-fb:109 goog-remb\r\n"
        "a=rtcp-fb:109 transport-cc\r\n"
        "a=rtcp-fb:109 ccm fir\r\n"
        "a=rtcp-fb:109 nack\r\n"
        "a=rtcp-fb:109 nack pli\r\n"
        "a=fmtp:109 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f\r\n"
        "a=rtpmap:114 rtx/90000\r\n"
        "a=fmtp:114 apt=109\r\n"
        "a=rtpmap:115 H264/90000\r\n"
        "a=rtcp-fb:115 goog-remb\r\n"
        "a=rtcp-fb:115 transport-cc\r\n"
        "a=rtcp-fb:115 ccm fir\r\n"
        "a=rtcp-fb:115 nack\r\n"
        "a=rtcp-fb:115 nack pli\r\n"
        "a=fmtp:115 level-asymmetry-allowed=1;packetization-mode=0;profile-level-id=42e01f\r\n"
        "a=rtpmap:116 rtx/90000\r\n"
        "a=fmtp:116 apt=115\r\n"
        "a=rtpmap:117 H264/90000\r\n"
        "a=rtcp-fb:117 goog-remb\r\n"
        "a=rtcp-fb:117 transport-cc\r\n"
        "a=rtcp-fb:117 ccm fir\r\n"
        "a=rtcp-fb:117 nack\r\n"
        "a=rtcp-fb:117 nack pli\r\n"
        "a=fmtp:117 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=4d001f\r\n"
        "a=rtpmap:118 rtx/90000\r\n"
        "a=fmtp:118 apt=117\r\n"
        "a=rtpmap:39 H264/90000\r\n"
        "a=rtcp-fb:39 goog-remb\r\n"
        "a=rtcp-fb:39 transport-cc\r\n"
        "a=rtcp-fb:39 ccm fir\r\n"
        "a=rtcp-fb:39 nack\r\n"
        "a=rtcp-fb:39 nack pli\r\n"
        "a=fmtp:39 level-asymmetry-allowed=1;packetization-mode=0;profile-level-id=4d001f\r\n"
        "a=rtpmap:40 rtx/90000\r\n"
        "a=fmtp:40 apt=39\r\n"
        "a=rtpmap:41 H264/90000\r\n"
        "a=rtcp-fb:41 goog-remb\r\n"
        "a=rtcp-fb:41 transport-cc\r\n"
        "a=rtcp-fb:41 ccm fir\r\n"
        "a=rtcp-fb:41 nack\r\n"
        "a=rtcp-fb:41 nack pli\r\n"
        "a=fmtp:41 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=f4001f\r\n"
        "a=rtpmap:42 rtx/90000\r\n"
        "a=fmtp:42 apt=41\r\n"
        "a=rtpmap:43 H264/90000\r\n"
        "a=rtcp-fb:43 goog-remb\r\n"
        "a=rtcp-fb:43 transport-cc\r\n"
        "a=rtcp-fb:43 ccm fir\r\n"
        "a=rtcp-fb:43 nack\r\n"
        "a=rtcp-fb:43 nack pli\r\n"
        "a=fmtp:43 level-asymmetry-allowed=1;packetization-mode=0;profile-level-id=f4001f\r\n"
        "a=rtpmap:44 rtx/90000\r\n"
        "a=fmtp:44 apt=43\r\n"
        "a=rtpmap:45 AV1/90000\r\n"
        "a=rtcp-fb:45 goog-remb\r\n"
        "a=rtcp-fb:45 transport-cc\r\n"
        "a=rtcp-fb:45 ccm fir\r\n"
        "a=rtcp-fb:45 nack\r\n"
        "a=rtcp-fb:45 nack pli\r\n"
        "a=fmtp:45 level-idx=5;profile=0;tier=0\r\n"
        "a=rtpmap:46 rtx/90000\r\n"
        "a=fmtp:46 apt=45\r\n"
        "a=rtpmap:47 AV1/90000\r\n"
        "a=rtcp-fb:47 goog-remb\r\n"
        "a=rtcp-fb:47 transport-cc\r\n"
        "a=rtcp-fb:47 ccm fir\r\n"
        "a=rtcp-fb:47 nack\r\n"
        "a=rtcp-fb:47 nack pli\r\n"
        "a=fmtp:47 level-idx=5;profile=1;tier=0\r\n"
        "a=rtpmap:48 rtx/90000\r\n"
        "a=fmtp:48 apt=47\r\n"
        "a=rtpmap:119 red/90000\r\n"
        "a=rtpmap:120 rtx/90000\r\n"
        "a=fmtp:120 apt=119\r\n"
        "a=rtpmap:121 ulpfec/90000\r\n"
        "a=rtpmap:49 flexfec-03/90000\r\n"
        "a=rtcp-fb:49 goog-remb\r\n"
        "a=rtcp-fb:49 transport-cc\r\n"
        "a=fmtp:49 repair-window=10000000\r\n";

    int ret = whep_add_track_template(ctx, audio_track_sdp, "0");
    if (ret < 0)
        return ret;

    ret = whep_add_track_template(ctx, video_track_sdp, "1");
    if (ret < 0)
        return ret;

    return 0;
}

static int whep_create_pc(WHEPContext *ctx)
{
    rtcConfiguration config = { 0 };

    if (ctx->ice_servers && ctx->nb_ice_servers > 0) {
        config.iceServers = ctx->ice_servers;
        config.iceServersCount = ctx->nb_ice_servers;
    }

    ctx->rtc_config = config;
    ctx->pc_id = rtcCreatePeerConnection(&ctx->rtc_config);
    if (ctx->pc_id < 0) {
        ctx->pc_id = -1;
        return AVERROR_EXTERNAL;
    }

    rtcSetUserPointer(ctx->pc_id, ctx);
    rtcSetStateChangeCallback(ctx->pc_id, whep_on_state_change);
    rtcSetIceStateChangeCallback(ctx->pc_id, whep_on_ice_state);
    rtcSetGatheringStateChangeCallback(ctx->pc_id, whep_on_gathering_state);
    rtcSetLocalCandidateCallback(ctx->pc_id, whep_on_candidate);
    rtcSetTrackCallback(ctx->pc_id, whep_on_track);

    return whep_add_media_tracks(ctx);
}

static int whep_create_offer(WHEPContext *ctx)
{
    char tmp[MAX_SDP_SIZE];
    ctx->gather_complete = 0;
    int ret = rtcCreateOffer(ctx->pc_id, tmp, sizeof(tmp));
    if (ret < 0)
        return AVERROR_EXTERNAL;
    ret = rtcSetLocalDescription(ctx->pc_id, "offer");
    if (ret < 0)
        return AVERROR_EXTERNAL;
    ret = whep_wait_for_flag(ctx, &ctx->gather_complete, 1, ctx->handshake_timeout);
    return ret;
}

static int whep_send_candidates(WHEPContext *ctx)
{
    for (int i = 0; i < ctx->nb_candidates; i++) {
        WHEPRemoteCandidate *cand = &ctx->candidates[i];
        if (ctx->fmt)
            av_log(ctx->fmt, AV_LOG_TRACE, "whep: applying remote candidate mid=%s%s\n",
                   cand->mid ? cand->mid : "<none>",
                   (cand->candidate && *cand->candidate) ? "" : " (end)");
        if (!cand->candidate || !*cand->candidate)
            rtcAddRemoteCandidate(ctx->pc_id, NULL, cand->mid);
        else
            rtcAddRemoteCandidate(ctx->pc_id, cand->candidate, cand->mid);
    }
    return 0;
}

static av_cold int whep_init(AVFormatContext *s)
{
    WHEPContext *ctx = s->priv_data;
    int ret;

    ctx->fmt = s;
    ctx->pc_id = -1;
    ctx->tracks_ready = 0;
    ctx->gather_complete = 0;
    ctx->ice_connected = 0;
    ctx->pc_failed = 0;
    ctx->abort_flag = 0;
    ctx->worker_started = 0;
    ctx->worker_stop = 0;
    ctx->cleaned = 0;
    ctx->nb_local_candidates = 0;
    ctx->nb_track_bindings = 0;
    memset(ctx->track_bindings, 0, sizeof(ctx->track_bindings));
    memset(ctx->rtp_demux_by_pt, 0, sizeof(ctx->rtp_demux_by_pt));

    ctx->handshake_timeout = ctx->handshake_timeout ? ctx->handshake_timeout : 10000;
    ctx->ice_timeout = ctx->ice_timeout ? ctx->ice_timeout : 15000;

    if ((ret = pthread_mutex_init(&ctx->state_mutex, NULL)))
        return AVERROR(ret);
    if ((ret = pthread_cond_init(&ctx->state_cond, NULL))) {
        pthread_mutex_destroy(&ctx->state_mutex);
        return AVERROR(ret);
    }

    ret = whep_queue_init(&ctx->rtp_queue);
    if (ret < 0)
        goto fail;
    ret = whep_queue_init(&ctx->pkt_queue);
    if (ret < 0)
        goto fail;

    whep_parse_ice_servers(ctx);
    whep_reset_tracks(ctx);

    return 0;

fail:
    whep_queue_destroy(&ctx->rtp_queue, whep_free_rtp_packet);
    pthread_cond_destroy(&ctx->state_cond);
    pthread_mutex_destroy(&ctx->state_mutex);
    ctx->cleaned = 1;
    return ret;
}

static int whep_read_header(AVFormatContext *s)
{
    WHEPContext *ctx = s->priv_data;
    int ret;

    ctx->fmt = s;
    ctx->start_ts_ms = whep_now_ms();
    whep_log(ctx, AV_LOG_INFO, "read_header start url=%s", s->url ? s->url : "<unknown>");

    whep_log(ctx, AV_LOG_TRACE, "initializing context");
    ret = whep_init(s);
    if (ret < 0) {
        whep_log(ctx, AV_LOG_ERROR, "context init failed: %s", av_err2str(ret));
        return ret;
    }

    whep_log(ctx, AV_LOG_INFO, "context initialized");

    ret = whep_create_pc(ctx);
    if (ret < 0)
        goto fail;

    whep_log(ctx, AV_LOG_INFO, "peer connection created (pc_id=%d)", ctx->pc_id);

    ret = whep_create_offer(ctx);
    if (ret < 0)
        goto fail;

    whep_log(ctx, AV_LOG_INFO, "offer created");

    ret = whep_http_exchange(s, ctx);
    if (ret < 0)
        goto fail;

    whep_log(ctx, AV_LOG_INFO, "HTTP exchange completed");

    ret = whep_parse_sdp(s);
    if (ret < 0)
        goto fail;

    whep_log(ctx, AV_LOG_INFO, "SDP parsed (%d tracks)", ctx->nb_tracks);

    ret = rtcSetRemoteDescription(ctx->pc_id, ctx->remote_sdp, "answer");
    if (ret < 0) {
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    whep_log(ctx, AV_LOG_INFO, "remote description applied");

    whep_send_candidates(ctx);
    whep_log(ctx, AV_LOG_INFO, "pushed %d cached remote candidates", ctx->nb_candidates);

    ret = whep_wait_for_flag(ctx, &ctx->ice_connected, 1, ctx->ice_timeout);
    if (ret < 0)
        goto fail;

    whep_log(ctx, AV_LOG_INFO, "ICE connected");

    ret = whep_start_worker(ctx);
    if (ret < 0)
        goto fail;

    whep_log(ctx, AV_LOG_INFO, "worker thread started");

    return 0;

fail:
    whep_log(ctx, AV_LOG_ERROR, "initialization failed: %s", av_err2str(ret));
    ctx->abort_flag = 1;
    whep_read_close(s);
    return ret;
}

static int whep_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    WHEPContext *ctx = s->priv_data;
    AVPacket *node;

    for (;;) {
        node = whep_queue_pop(&ctx->pkt_queue, 1);
        if (!node) {
            if (ctx->pkt_queue.abort_request)
                return AVERROR_EOF;
            continue;
        }
        av_packet_move_ref(pkt, node);
        av_packet_free(&node);
        return 0;
    }
}

static int whep_read_close(AVFormatContext *s)
{
    WHEPContext *ctx = s->priv_data;

    if (ctx->cleaned)
        return 0;

    ctx->abort_flag = 1;

    whep_stop_worker(ctx);

    whep_queue_abort(&ctx->pkt_queue);
    whep_queue_abort(&ctx->rtp_queue);
    whep_queue_destroy(&ctx->pkt_queue, whep_free_avpacket);
    whep_queue_destroy(&ctx->rtp_queue, whep_free_rtp_packet);

    if (ctx->pc_id >= 0) {
        rtcClosePeerConnection(ctx->pc_id);
        rtcDeletePeerConnection(ctx->pc_id);
        ctx->pc_id = -1;
    }

    pthread_cond_destroy(&ctx->state_cond);
    pthread_mutex_destroy(&ctx->state_mutex);

    whep_reset_tracks(ctx);

    // Clean up RTP demuxer contexts indexed by payload type
    for (int i = 0; i < 256; i++) {
        if (ctx->rtp_demux_by_pt[i]) {
            ff_rtp_parse_close(ctx->rtp_demux_by_pt[i]);
            ctx->rtp_demux_by_pt[i] = NULL;
        }
    }

    av_freep(&ctx->local_sdp);
    av_freep(&ctx->remote_sdp);
    av_freep(&ctx->resource_url);
    av_freep(&ctx->ice_ufrag_local);
    av_freep(&ctx->ice_pwd_local);

    for (int i = 0; i < ctx->nb_candidates; i++) {
        av_freep(&ctx->candidates[i].mid);
        av_freep(&ctx->candidates[i].candidate);
    }
    ctx->nb_candidates = 0;

    for (int i = 0; i < ctx->nb_local_candidates; i++) {
        av_freep(&ctx->local_candidates[i].mid);
        av_freep(&ctx->local_candidates[i].candidate);
    }
    ctx->nb_local_candidates = 0;

    ctx->nb_track_bindings = 0;
    memset(ctx->track_bindings, 0, sizeof(ctx->track_bindings));

    av_freep(&ctx->ice_servers);

    ctx->cleaned = 1;

    return 0;
}

static const AVOption whep_options[] = {
    { "ice_servers", "Comma-separated list of ICE server URLs", offsetof(WHEPContext, ice_server_list), AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0, AV_OPT_FLAG_DECODING_PARAM },
    { "authorization", "Bearer token for HTTP Authorization header", offsetof(WHEPContext, authorization), AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0, AV_OPT_FLAG_DECODING_PARAM },
    { "handshake_timeout", "Signaling timeout in milliseconds", offsetof(WHEPContext, handshake_timeout), AV_OPT_TYPE_INT, { .i64 = 10000 }, 1000, 60000, AV_OPT_FLAG_DECODING_PARAM },
    { "ice_timeout", "ICE connection timeout in milliseconds", offsetof(WHEPContext, ice_timeout), AV_OPT_TYPE_INT, { .i64 = 15000 }, 1000, 60000, AV_OPT_FLAG_DECODING_PARAM },
    { NULL }
};

static const AVClass whep_demuxer_class = {
    .class_name = "whep demuxer",
    .item_name  = av_default_item_name,
    .option     = whep_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

const FFInputFormat ff_whep_demuxer = {
    .p.name         = "whep",
#if CONFIG_SMALL
    .p.long_name    = NULL,
#else
    .p.long_name    = "WebRTC-HTTP Egress Protocol demuxer",
#endif
    .p.priv_class   = &whep_demuxer_class,
    .p.flags        = AVFMT_NOFILE,
    .priv_data_size = sizeof(WHEPContext),
    .flags_internal = FF_INFMT_FLAG_INIT_CLEANUP,
    .read_header    = whep_read_header,
    .read_packet    = whep_read_packet,
    .read_close     = whep_read_close,
};

#endif /* CONFIG_NETWORK */
