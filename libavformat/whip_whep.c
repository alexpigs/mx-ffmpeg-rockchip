/*
 * WHIP/WHEP shared functions
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <rtc/rtc.h>
#include <string.h>

#include "libavutil/avstring.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"

#include "whip_whep.h"

// Convert libdatachannel log level to equivalent ffmpeg log level.
static int libdatachannel_to_ffmpeg_log_level(int libdatachannel_log_level)
{
    if      (libdatachannel_log_level >= RTC_LOG_VERBOSE) return AV_LOG_TRACE;
    else if (libdatachannel_log_level >= RTC_LOG_DEBUG)   return AV_LOG_DEBUG;
    else if (libdatachannel_log_level >= RTC_LOG_INFO)    return AV_LOG_VERBOSE;
    else if (libdatachannel_log_level >= RTC_LOG_WARNING) return AV_LOG_WARNING;
    else if (libdatachannel_log_level >= RTC_LOG_ERROR)   return AV_LOG_ERROR;
    else if (libdatachannel_log_level >= RTC_LOG_FATAL)   return AV_LOG_FATAL;
    else                                                  return AV_LOG_QUIET;
}

static void libdatachannel_log(rtcLogLevel rtc_level, const char *message)
{
    av_log(NULL, libdatachannel_to_ffmpeg_log_level(rtc_level), "[libdatachannel] %s\n", message);
}

void ff_whip_whep_init_rtc_logger(void)
{
    rtcInitLogger(RTC_LOG_VERBOSE, libdatachannel_log);
}

static char *normalize_webrtc_url(const char *url)
{
    const char *suffix = NULL;

    if (av_strstart(url, "webrtc+http://", &suffix))
        return av_asprintf("http://%s", suffix);
    if (av_strstart(url, "webrtc+https://", &suffix))
        return av_asprintf("https://%s", suffix);
    if (av_strstart(url, "webrtch://", &suffix))
        return av_asprintf("http://%s", suffix);
    if (av_strstart(url, "webrtcs://", &suffix))
        return av_asprintf("https://%s", suffix);
    if (av_strstart(url, "webrtc://", &suffix))
        return av_asprintf("https://%s", suffix);

    return av_strdup(url);
}

static char *create_srs_json_offer(const char *sdp, const char *api_url, const char *stream_url) {
    // Escape SDP for JSON
    char *escaped_sdp = av_malloc(strlen(sdp) * 2 + 1);
    if (!escaped_sdp)
        return NULL;

    const char *src = sdp;
    char *dst = escaped_sdp;
    while (*src) {
        if (*src == '"' || *src == '\\') {
            *dst++ = '\\';
        } else if (*src == '\r') {
            *dst++ = '\\';
            *dst++ = 'r';
            src++;
            continue;
        } else if (*src == '\n') {
            *dst++ = '\\';
            *dst++ = 'n';
            src++;
            continue;
        }
        *dst++ = *src++;
    }
    *dst = '\0';

    // Create JSON
    char *json = av_asprintf("{\"api\":\"%s\",\"streamurl\":\"%s\",\"clientip\":null,\"sdp\":\"%s\"}",
                             api_url, stream_url, escaped_sdp);
    av_free(escaped_sdp);
    return json;
}

static int parse_srs_json_answer(const char *json, char *sdp, int sdp_size) {
    // Simple JSON parsing to extract SDP
    const char *sdp_start = strstr(json, "\"sdp\":\"");
    if (!sdp_start)
        return -1;

    sdp_start += 7; // Skip "sdp":"
    const char *sdp_end = sdp_start;
    char *out = sdp;

    while (*sdp_end && *sdp_end != '"' && (out - sdp) < sdp_size - 1) {
        if (*sdp_end == '\\') {
            sdp_end++;
            if (*sdp_end == 'r') {
                *out++ = '\r';
            } else if (*sdp_end == 'n') {
                *out++ = '\n';
            } else if (*sdp_end == '"') {
                *out++ = '"';
            } else if (*sdp_end == '\\') {
                *out++ = '\\';
            }
            sdp_end++;
        } else {
            *out++ = *sdp_end++;
        }
    }
    *out = '\0';

    return 0;
}

int ff_whip_whep_exchange_and_set_sdp(AVFormatContext *s, int pc, const char *token, char **session_url, const char *server_type)
{
    char offer[SDP_MAX_SIZE], offer_hex[2 * SDP_MAX_SIZE], answer[SDP_MAX_SIZE];
    AVDictionary *options = NULL;
    AVIOContext *io_ctx = NULL;
    char *request_url = NULL;
    int ret;

    request_url = normalize_webrtc_url(s->url);
    if (!request_url)
        return AVERROR(ENOMEM);

    if (!av_strstart(request_url, "http", NULL)) {
        av_log(s, AV_LOG_ERROR, "Unsupported URL scheme for WHIP/WHEP input: %s\n", s->url);
        ret = AVERROR(EINVAL);
        goto fail;
    }

    if (rtcCreateOffer(pc, offer, sizeof(offer)) < 0) {
        av_log(s, AV_LOG_ERROR, "Failed to create offer\n");
        ret = AVERROR_EXTERNAL;
        goto fail;
    }
    av_log(s, AV_LOG_DEBUG, "Generated offer: %s\n", offer);

    if (rtcSetLocalDescription(pc, "offer") < 0) {
        av_log(s, AV_LOG_ERROR, "Failed to set local description\n");
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    if (server_type && !strcmp(server_type, "srs")) {
        char *json_offer;
        char *json_offer_hex = NULL;
        char api_url[1024];
        char proto[16], host[1024];
        int port;

        av_url_split(proto, sizeof(proto), NULL, 0, host, sizeof(host), &port, NULL, 0, s->url);
        if (port > 0 && port != 443) {
            snprintf(api_url, sizeof(api_url), "https://%s:%d/rtc/v1/play/", host, port);
        } else {
            snprintf(api_url, sizeof(api_url), "https://%s/rtc/v1/play/", host);
        }

        // Correctly set the request_url for SRS
        av_freep(&request_url);
        request_url = av_strdup(api_url);
        if (!request_url) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }

        json_offer = create_srs_json_offer(offer, api_url, s->url);
        if (!json_offer) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }
        json_offer_hex = av_malloc(strlen(json_offer) * 2 + 1);
        if (!json_offer_hex) {
            av_free(json_offer);
            ret = AVERROR(ENOMEM);
            goto fail;
        }
        ff_data_to_hex(json_offer_hex, json_offer, strlen(json_offer), 0);

        av_dict_set(&options, "content_type", "application/json", 0);
        av_dict_set(&options, "post_data", json_offer_hex, 0);

        av_free(json_offer);
        av_free(json_offer_hex);
    } else {
        av_dict_set(&options, "content_type", "application/sdp", 0);
        ff_data_to_hex(offer_hex, offer, strlen(offer), 0);
        av_dict_set(&options, "post_data", offer_hex, 0);
    }

    if (token) {
        char *headers = av_asprintf("Authorization: Bearer %s\r\n", token);
        if (!headers) {
            av_log(s, AV_LOG_ERROR, "Failed to allocate headers\n");
            ret = AVERROR(ENOMEM);
            goto fail;
        }
        av_dict_set(&options, "headers", headers, 0);
        av_free(headers);
    }

    ret = avio_open2(&io_ctx, request_url, AVIO_FLAG_READ, NULL, &options);
    if (ret < 0) {
        av_log(s, AV_LOG_ERROR, "Failed to send offer to endpoint: %s\n", av_err2str(ret));
        goto fail;
    }

    ret = avio_read(io_ctx, answer, sizeof(answer) - 1);
    if (ret <= 0) {
        av_log(s, AV_LOG_ERROR, "Failed to read answer: %s\n", av_err2str(ret));
        ret = AVERROR(EIO);
        goto fail;
    }
    answer[ret] = 0;
    av_log(s, AV_LOG_DEBUG, "Received answer: %s\n", answer);

    if (server_type && !strcmp(server_type, "srs")) {
        char sdp_answer[SDP_MAX_SIZE];
        if (parse_srs_json_answer(answer, sdp_answer, sizeof(sdp_answer)) < 0) {
            av_log(s, AV_LOG_ERROR, "Failed to parse SRS JSON answer\n");
            ret = AVERROR_INVALIDDATA;
            goto fail;
        }
        if (rtcSetRemoteDescription(pc, sdp_answer, "answer") < 0) {
            av_log(s, AV_LOG_ERROR, "Failed to set remote description: %s\n", sdp_answer);
            ret = AVERROR_EXTERNAL;
            goto fail;
        }
    } else {
        if (rtcSetRemoteDescription(pc, answer, "answer") < 0) {
            av_log(s, AV_LOG_ERROR, "Failed to set remote description: %s\n", answer);
            ret = AVERROR_EXTERNAL;
            goto fail;
        }
    }

    if (session_url)
        av_opt_get(io_ctx, "new_location", AV_OPT_SEARCH_CHILDREN, (uint8_t **)session_url);

    ret = 0;
fail:
    avio_closep(&io_ctx);
    av_dict_free(&options);
    av_freep(&request_url);
    return ret;
}

int ff_whip_whep_delete_session(AVFormatContext *s, const char *token, const char *session_url)
{
    AVDictionary *options = NULL;
    AVIOContext *io_ctx = NULL;
    int ret;

    if (!session_url) {
        av_log(s, AV_LOG_ERROR, "No session URL provided\n");
        return AVERROR(EINVAL);
    }
    if (!av_strstart(session_url, "http", NULL)) {
        av_log(s, AV_LOG_ERROR, "Unsupported URL scheme\n");
        return AVERROR(EINVAL);
    }

    av_dict_set(&options, "method", "DELETE", 0);
    if (token) {
        char *headers = av_asprintf("Authorization: Bearer %s\r\n", token);
        if (!headers) {
            av_log(s, AV_LOG_ERROR, "Failed to allocate headers\n");
            return AVERROR(ENOMEM);
        }
        av_dict_set(&options, "headers", headers, 0);
        av_free(headers);
    }
    ret = avio_open2(&io_ctx, session_url, AVIO_FLAG_READ, NULL, &options);
    if (ret < 0)
        av_log(s, AV_LOG_ERROR, "Failed to delete session: %s\n", av_err2str(ret));

    avio_closep(&io_ctx);
    av_dict_free(&options);
    return ret;
}
