// Feed mode: FFmpeg opens and demuxes the stream, and the TV's Starfish media
// pipeline decodes the packets in hardware. This is the path Relay itself will
// use on webOS, because it keeps the network side (provider quirks,
// reconnects, HLS) in our hands instead of the TV's URL player.

#include "feed.h"

#include <StarfishMediaAPIs.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/bsf.h>
}

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

namespace {

struct FeedState {
    FeedHooks hooks;
    std::atomic<int> loadState{0}; // 0 waiting, 1 loaded, 2 failed
    std::atomic<bool> playing{false};
};

void onEvent(int type, int64_t num, const char *str, void *data)
{
    auto *st = static_cast<FeedState *>(data);
    const char *s = str ? str : "";
    switch (type) {
    case PF_EVENT_TYPE_STR_STATE_UPDATE__LOADCOMPLETED:
        st->hooks.log("starfish: load completed");
        st->loadState = 1;
        break;
    case PF_EVENT_TYPE_STR_STATE_UPDATE__PLAYING:
        st->hooks.log("starfish: playing");
        st->playing = true;
        st->hooks.status(2, "Playing (hardware decode)", "");
        break;
    case PF_EVENT_TYPE_STR_STATE_UPDATE__PAUSED:
        st->hooks.log("starfish: paused");
        st->playing = false;
        break;
    case PF_EVENT_TYPE_INT_ERROR:
        st->hooks.log("starfish: error " + std::to_string(num) + " " + s);
        st->hooks.status(3, "Player error " + std::to_string(num), s);
        if (st->loadState == 0) st->loadState = 2;
        break;
    case PF_EVENT_TYPE_STR_ERROR:
        st->hooks.log(std::string("starfish: error ") + s);
        st->hooks.status(3, "Player error", s);
        if (st->loadState == 0) st->loadState = 2;
        break;
    case PF_EVENT_TYPE_STR_VIDEO_INFO:
        st->hooks.log(std::string("starfish: video info ") + s);
        break;
    case PF_EVENT_TYPE_STR_AUDIO_INFO:
        st->hooks.log(std::string("starfish: audio info ") + s);
        break;
    case PF_EVENT_TYPE_FRAMEREADY:
    case PF_EVENT_TYPE_INT_NEED_DATA:
    case PF_EVENT_TYPE_INT_ENOUGH_DATA:
    case PF_EVENT_TYPE_INT_BUFFER_RANGE_INFO:
        break; // very frequent
    case PF_EVENT_TYPE_DROPPED_FRAME:
        st->hooks.counter("dropped");
        break;
    default:
        st->hooks.log("starfish: event " + std::to_string(type) + " " + std::to_string(num) + " " + s);
    }
}

const char *videoCodecName(AVCodecID id)
{
    switch (id) {
    case AV_CODEC_ID_H264: return "H264";
    case AV_CODEC_ID_HEVC: return "H265";
    case AV_CODEC_ID_MPEG2VIDEO: return "MPEG2";
    default: return nullptr;
    }
}

const char *audioCodecName(AVCodecID id)
{
    switch (id) {
    case AV_CODEC_ID_AAC: return "AAC";
    case AV_CODEC_ID_AC3: return "AC3";
    case AV_CODEC_ID_EAC3: return "EAC3";
    case AV_CODEC_ID_MP2:
    case AV_CODEC_ID_MP3: return "MPEG";
    default: return nullptr;
    }
}

std::string loadPayload(const FeedConfig &cfg, const char *vcodec, const char *acodec,
                        const AVCodecParameters *vp, const AVRational &fps)
{
    char es[512];
    snprintf(es, sizeof es,
             "\"esInfo\":{\"ptsToDecode\":0,\"seperatedPTS\":true,\"pauseAtDecodeTime\":true,"
             "\"videoWidth\":%d,\"videoHeight\":%d,\"videoFpsValue\":%d,\"videoFpsScale\":%d}",
             vp->width, vp->height, fps.num > 0 ? fps.num : 25, fps.den > 0 ? fps.den : 1);
    std::string codec = std::string("\"codec\":{\"video\":\"") + vcodec + "\"";
    if (acodec) codec += std::string(",\"audio\":\"") + acodec + "\"";
    codec += "}";
    std::string p = "{\"args\":[{\"mediaTransportType\":\"BUFFERSTREAM\",\"option\":{";
    p += "\"appId\":\"" + cfg.appId + "\",";
    if (!cfg.windowId.empty()) p += "\"windowId\":\"" + cfg.windowId + "\",";
    p += "\"externalStreamingInfo\":{\"contents\":{" + codec + "," + es + ",\"format\":\"RAW\"},"
         "\"bufferingCtrInfo\":{\"bufferMaxLevel\":0,\"bufferMinLevel\":0,\"preBufferByte\":0,"
         "\"qBufferLevelAudio\":0,\"qBufferLevelVideo\":0,"
         "\"srcBufferLevelAudio\":{\"maximum\":2097152,\"minimum\":1},"
         "\"srcBufferLevelVideo\":{\"maximum\":8388608,\"minimum\":1}}},";
    p += "\"transmission\":{\"contentsType\":\"LIVE\"},";
    p += std::string("\"needAudio\":") + (acodec ? "true" : "false") + ",";
    p += "\"lowDelayMode\":false,\"queryPosition\":true}}]}";
    return p;
}

bool needsAnnexB(const AVCodecParameters *p)
{
    if (p->codec_id != AV_CODEC_ID_H264 && p->codec_id != AV_CODEC_ID_HEVC) return false;
    if (!p->extradata || p->extradata_size < 4) return false;
    const uint8_t *d = p->extradata;
    bool startCode = (d[0] == 0 && d[1] == 0 && d[2] == 1) || (d[0] == 0 && d[1] == 0 && d[2] == 0 && d[3] == 1);
    return !startCode; // avcC / hvcC extradata: packets are length-prefixed
}

int interruptCb(void *data) { return static_cast<std::atomic<bool> *>(data)->load() ? 1 : 0; }

} // namespace

void runFeed(const FeedConfig &cfg, const FeedHooks &hooks, std::atomic<bool> &quit,
             std::atomic<void *> &control)
{
    FeedState st;
    st.hooks = hooks;
    avformat_network_init();

    hooks.status(1, "Opening stream", cfg.name);
    AVFormatContext *fmt = avformat_alloc_context();
    fmt->interrupt_callback.callback = interruptCb;
    fmt->interrupt_callback.opaque = &quit;
    AVDictionary *opts = nullptr;
    av_dict_set(&opts, "user_agent", "Relay", 0);
    av_dict_set(&opts, "reconnect", "1", 0);
    av_dict_set(&opts, "reconnect_streamed", "1", 0);
    av_dict_set(&opts, "reconnect_delay_max", "5", 0);
    av_dict_set(&opts, "rw_timeout", "15000000", 0);
    av_dict_set(&opts, "probesize", "2000000", 0);
    av_dict_set(&opts, "analyzeduration", "3000000", 0);
    int r = avformat_open_input(&fmt, cfg.url.c_str(), nullptr, &opts);
    av_dict_free(&opts);
    if (r < 0) {
        char e[128]; av_strerror(r, e, sizeof e);
        hooks.log(std::string("open failed: ") + e);
        hooks.status(3, "Could not open the stream", e);
        return;
    }
    if (avformat_find_stream_info(fmt, nullptr) < 0) hooks.log("stream info incomplete, carrying on");
    hooks.log(std::string("container ") + fmt->iformat->name + ", " + std::to_string(fmt->nb_streams) + " streams");

    int vi = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    int ai = cfg.audio ? av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, vi, nullptr, 0) : -1;
    if (vi < 0) { hooks.status(3, "No video in this stream", ""); avformat_close_input(&fmt); return; }
    AVStream *vs = fmt->streams[vi];
    AVStream *as = ai >= 0 ? fmt->streams[ai] : nullptr;
    const char *vcodec = videoCodecName(vs->codecpar->codec_id);
    const char *acodec = as ? audioCodecName(as->codecpar->codec_id) : nullptr;
    {
        char b[256];
        snprintf(b, sizeof b, "video %s %dx%d %d/%d fps; audio %s %d Hz %d ch",
                 avcodec_get_name(vs->codecpar->codec_id), vs->codecpar->width, vs->codecpar->height,
                 vs->avg_frame_rate.num, vs->avg_frame_rate.den,
                 as ? avcodec_get_name(as->codecpar->codec_id) : "none",
                 as ? as->codecpar->sample_rate : 0, as ? as->codecpar->ch_layout.nb_channels : 0);
        hooks.log(b);
        hooks.status(1, "Starting hardware player", b);
    }
    if (!vcodec) { hooks.status(3, "Video codec not supported yet", avcodec_get_name(vs->codecpar->codec_id)); avformat_close_input(&fmt); return; }
    if (as && !acodec) hooks.log("audio codec not handled by the TV player in this test, video only");

    AVBSFContext *bsf = nullptr;
    if (needsAnnexB(vs->codecpar)) {
        const AVBitStreamFilter *f = av_bsf_get_by_name(vs->codecpar->codec_id == AV_CODEC_ID_H264
                                                        ? "h264_mp4toannexb" : "hevc_mp4toannexb");
        if (f && av_bsf_alloc(f, &bsf) == 0) {
            avcodec_parameters_copy(bsf->par_in, vs->codecpar);
            bsf->time_base_in = vs->time_base;
            if (av_bsf_init(bsf) < 0) av_bsf_free(&bsf);
            else hooks.log("converting video to Annex B");
        }
    }

    StarfishMediaAPIs *sf = nullptr;
    bool withAudio = acodec != nullptr;
    for (int attempt = 0; attempt < 2 && !quit; attempt++) {
        sf = new StarfishMediaAPIs(cfg.appId.c_str());
        std::string payload = loadPayload(cfg, vcodec, withAudio ? acodec : nullptr, vs->codecpar, vs->avg_frame_rate);
        hooks.log("starfish load " + payload);
        st.loadState = 0;
        bool ok = sf->Load(payload.c_str(), onEvent, &st);
        hooks.log(std::string("starfish load returned ") + (ok ? "true" : "false"));
        // Some firmware sends LOADCOMPLETED, others just accept data after Load.
        for (int i = 0; ok && i < 50 && st.loadState == 0 && !quit; i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (ok && st.loadState != 2) break;
        hooks.log("load failed" + std::string(withAudio ? ", trying video only" : ""));
        sf->Unload(); delete sf; sf = nullptr;
        if (!withAudio) break;
        withAudio = false;
    }
    if (!sf) { hooks.status(3, "The TV player refused the stream", ""); if (bsf) av_bsf_free(&bsf); avformat_close_input(&fmt); return; }
    control = sf;
    sf->Play();
    hooks.status(1, withAudio ? "Buffering" : "Buffering (no sound in this test)", cfg.name);

    AVPacket *pkt = av_packet_alloc();
    int64_t base = AV_NOPTS_VALUE; // first timestamp, in microseconds
    int64_t lastV = 0;
    long fed = 0, full = 0;
    auto lastReport = std::chrono::steady_clock::now();
    auto feed = [&](AVPacket *p, AVStream *s, int esData) {
        int64_t ts = p->pts != AV_NOPTS_VALUE ? p->pts : p->dts;
        if (ts == AV_NOPTS_VALUE) return;
        int64_t us = av_rescale_q(ts, s->time_base, AVRational{1, 1000000});
        if (base == AV_NOPTS_VALUE) base = us;
        int64_t rel = us - base;
        if (esData == 1) {
            if (rel < lastV - 1000000 || rel > lastV + 10000000)
                hooks.log("timestamp jump " + std::to_string((rel - lastV) / 1000) + " ms");
            lastV = rel;
        }
        if (rel < 0) return;
        char fp[160];
        snprintf(fp, sizeof fp, "{\"bufferAddr\":\"%p\",\"bufferSize\":%d,\"pts\":%lld,\"esData\":%d}",
                 (void *)p->data, p->size, (long long)rel * 1000, esData);
        while (!quit) {
            std::string res = sf->Feed(fp);
            if (res.find("Ok") != std::string::npos || res.find("OK") != std::string::npos) { fed++; return; }
            if (res.find("BufferFull") != std::string::npos) {
                full++;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            hooks.log("feed said: " + res);
            return;
        }
    };

    while (!quit) {
        r = av_read_frame(fmt, pkt);
        if (r == AVERROR(EAGAIN)) continue;
        if (r < 0) {
            char e[128]; av_strerror(r, e, sizeof e);
            hooks.log(std::string("read ended: ") + e);
            hooks.status(3, "Stream ended", e);
            break;
        }
        if (pkt->stream_index == vi) {
            if (bsf) {
                if (av_bsf_send_packet(bsf, pkt) == 0) {
                    while (av_bsf_receive_packet(bsf, pkt) == 0) { feed(pkt, vs, 1); av_packet_unref(pkt); }
                }
            } else {
                feed(pkt, vs, 1);
            }
        } else if (withAudio && pkt->stream_index == ai) {
            feed(pkt, as, 2);
        }
        av_packet_unref(pkt);
        auto now = std::chrono::steady_clock::now();
        if (now - lastReport > std::chrono::seconds(10)) {
            lastReport = now;
            hooks.log("fed " + std::to_string(fed) + " packets, buffer full " + std::to_string(full)
                      + " times, playtime " + std::to_string(sf->getCurrentPlaytime() / 1000000) + " ms");
        }
    }
    av_packet_free(&pkt);
    control = nullptr;
    sf->Unload();
    delete sf;
    if (bsf) av_bsf_free(&bsf);
    avformat_close_input(&fmt);
    hooks.log("feed stopped");
}
