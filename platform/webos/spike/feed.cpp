// Feed mode: FFmpeg opens and demuxes the stream, and the TV's Starfish media
// pipeline decodes the packets in hardware. This is the path Relay itself will
// use on webOS, because it keeps the network side (provider quirks,
// reconnects, HLS) in our hands instead of the TV's URL player.

#include "feed.h"

#include <StarfishMediaAPIs.h>
#include "ndl.h"

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

const FeedHooks *g_ndlHooks = nullptr;

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

// Different firmware wants slightly different load settings, so the spike
// tries a few known shapes in turn and logs which one the TV accepts.
struct Variant {
    const char *name;
    bool foreground;  // call notifyForeground() before Load
    int sizes;        // 0 none, 1 width/height + whole-number fps
    const char *contents; // transmission contentsType, or nullptr to leave out
    bool window;      // pass the exported window id
    bool buffering;   // pass bufferingCtrInfo
};
const Variant kVariants[] = {
    {"1 sizes, LIVE", false, 1, "LIVE", false, true},
    {"2 sizes, LIVE, foreground", true, 1, "LIVE", false, true},
    {"3 minimal, foreground", true, 0, nullptr, false, false},
    {"4 sizes, WEBRTC, foreground", true, 1, "WEBRTC", false, true},
    {"5 sizes, LIVE, window, foreground", true, 1, "LIVE", true, true},
};
const int kVariantCount = sizeof kVariants / sizeof kVariants[0];

std::string loadPayload(const FeedConfig &cfg, const Variant &v, const char *vcodec, const char *acodec,
                        const AVCodecParameters *vp, const AVRational &fps)
{
    std::string es = "\"esInfo\":{\"ptsToDecode\":0,\"seperatedPTS\":true,\"pauseAtDecodeTime\":true";
    if (v.sizes) {
        // A whole number: 60000/1001 read as "60000 fps" can exhaust the decoder budget.
        int f = (fps.num > 0 && fps.den > 0) ? (int)((double)fps.num / fps.den + 0.5) : 30;
        es += ",\"videoWidth\":" + std::to_string(vp->width) + ",\"videoHeight\":" + std::to_string(vp->height)
            + ",\"videoFpsValue\":" + std::to_string(f) + ",\"videoFpsScale\":1";
    }
    es += "}";
    std::string codec = std::string("\"codec\":{\"video\":\"") + vcodec + "\"";
    if (acodec) codec += std::string(",\"audio\":\"") + acodec + "\"";
    codec += "}";
    std::string p = "{\"args\":[{\"mediaTransportType\":\"BUFFERSTREAM\",\"option\":{";
    p += "\"appId\":\"" + cfg.appId + "\",";
    if (v.window && !cfg.windowId.empty()) p += "\"windowId\":\"" + cfg.windowId + "\",";
    p += "\"externalStreamingInfo\":{\"contents\":{" + codec + "," + es + ",\"format\":\"RAW\"}";
    if (v.buffering)
        p += ",\"bufferingCtrInfo\":{\"bufferMaxLevel\":0,\"bufferMinLevel\":0,\"preBufferByte\":0,"
             "\"qBufferLevelAudio\":0,\"qBufferLevelVideo\":0,"
             "\"srcBufferLevelAudio\":{\"maximum\":2097152,\"minimum\":1},"
             "\"srcBufferLevelVideo\":{\"maximum\":8388608,\"minimum\":1}}";
    p += "},";
    if (v.contents) p += std::string("\"transmission\":{\"contentsType\":\"") + v.contents + "\"},";
    p += std::string("\"needAudio\":") + (acodec ? "true" : "false") + ",";
    p += "\"queryPosition\":true}}]}";
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
    av_dict_set(&opts, "user_agent", cfg.userAgent.c_str(), 0);
    // Live streams can't seek; without this FFmpeg asks for "Range: bytes=0-",
    // which some providers answer with an unusual 4xx error.
    av_dict_set(&opts, "seekable", "0", 0);
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

    for (unsigned i = 0; i < fmt->nb_streams; i++) {
        const AVCodecParameters *p = fmt->streams[i]->codecpar;
        hooks.log("stream " + std::to_string(i) + ": " + (av_get_media_type_string(p->codec_type) ? av_get_media_type_string(p->codec_type) : "?")
                  + " " + avcodec_get_name(p->codec_id) + " " + std::to_string(p->sample_rate) + " Hz");
    }
    int vi = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    int ai = -1;
    // av_find_best_stream skips audio it can't describe (no decoders are built
    // in), so pick the first audio stream with a codec the TV knows.
    for (unsigned i = 0; cfg.audio && i < fmt->nb_streams && ai < 0; i++)
        if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO
            && audioCodecName(fmt->streams[i]->codecpar->codec_id)) ai = (int)i;
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
    bool withAudio = false;
    auto tryLoad = [&](const Variant &v, bool audio) -> bool {
        sf = new StarfishMediaAPIs(cfg.appId.c_str());
        if (v.foreground) hooks.log(std::string("notifyForeground ") + (sf->notifyForeground() ? "ok" : "failed"));
        std::string payload = loadPayload(cfg, v, vcodec, audio ? acodec : nullptr, vs->codecpar, vs->avg_frame_rate);
        hooks.log(std::string("try variant ") + v.name + (audio ? " + audio" : "") + ": " + payload);
        st.loadState = 0;
        bool ok = sf->Load(payload.c_str(), onEvent, &st);
        // Some firmware sends LOADCOMPLETED, others just accept data after Load;
        // a refusal (e.g. 601) arrives within a second.
        for (int i = 0; ok && i < 30 && st.loadState == 0 && !quit; i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (ok && st.loadState != 2) {
            hooks.log(std::string("variant ") + v.name + (audio ? " + audio" : "") + " ACCEPTED");
            return true;
        }
        hooks.log(std::string("variant ") + v.name + " refused");
        sf->Unload(); delete sf; sf = nullptr;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        return false;
    };
    int first = 0, last = kVariantCount - 1;
    if (cfg.variant > 0 && cfg.variant <= kVariantCount) first = last = cfg.variant - 1;
    bool useSmp = cfg.backend != "ndl";
    for (int i = first; useSmp && i <= last && !sf && !quit; i++) {
        if (kVariants[i].window && cfg.windowId.empty()) continue;
        if (!tryLoad(kVariants[i], false)) continue;
        if (acodec) {
            // Video works with this shape; now see whether sound can join it.
            sf->Unload(); delete sf; sf = nullptr;
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            if (tryLoad(kVariants[i], true)) { withAudio = true; break; }
            hooks.log("audio refused, playing video only");
            tryLoad(kVariants[i], false);
        }
    }
    // NDL DirectMedia: the lower-level path Moonlight uses on webOS 5 and later.
    bool ndl = false;
    if (!sf && cfg.backend != "smp" && !quit && vs->codecpar->codec_id != AV_CODEC_ID_MPEG2VIDEO) {
        hooks.log("trying NDL DirectMedia");
        g_ndlHooks = &hooks;
        int rr = ndl_open(cfg.appId.c_str(), vs->codecpar->width, vs->codecpar->height,
                          vs->codecpar->codec_id == AV_CODEC_ID_HEVC,
                          [](const char *what, int code, long long num, const char *text) {
            if (g_ndlHooks) g_ndlHooks->log(std::string("NDL ") + what + " " + std::to_string(code) + " "
                                            + std::to_string(num) + " " + text);
        });
        ndl = rr == 0;
    }
    if (ndl) {
        hooks.status(1, "Buffering (NDL, no sound in this test)", cfg.name);
        AVPacket *pkt = av_packet_alloc();
        int64_t base = AV_NOPTS_VALUE;
        long fed = 0, errs = 0;
        bool shown = false;
        auto lastReport = std::chrono::steady_clock::now();
        auto play = [&](AVPacket *p) {
            int64_t ts = p->pts != AV_NOPTS_VALUE ? p->pts : p->dts;
            if (ts == AV_NOPTS_VALUE) return;
            int64_t us = av_rescale_q(ts, vs->time_base, AVRational{1, 1000000});
            if (base == AV_NOPTS_VALUE) base = us;
            if (us < base) return;
            // NDL plays as fast as it is fed, so pace by the timestamps.
            static auto t0 = std::chrono::steady_clock::now();
            auto due = t0 + std::chrono::microseconds(us - base + 300000);
            std::this_thread::sleep_until(due);
            int rr = ndl_play_video(p->data, p->size, (long long)(us - base) * 1000);
            if (rr == 0) { fed++; if (!shown && fed > 30) { shown = true; hooks.status(2, "Playing (NDL hardware decode)", ""); } }
            else if (++errs < 5) hooks.log("NDL video play " + std::to_string(rr) + " " + ndl_error());
        };
        while (!quit) {
            int rd = av_read_frame(fmt, pkt);
            if (rd == AVERROR(EAGAIN)) continue;
            if (rd < 0) { char e[128]; av_strerror(rd, e, sizeof e); hooks.status(3, "Stream ended", e); break; }
            if (pkt->stream_index == vi) {
                if (bsf) { if (av_bsf_send_packet(bsf, pkt) == 0) while (av_bsf_receive_packet(bsf, pkt) == 0) { play(pkt); av_packet_unref(pkt); } }
                else play(pkt);
            }
            av_packet_unref(pkt);
            auto now = std::chrono::steady_clock::now();
            if (now - lastReport > std::chrono::seconds(10)) {
                lastReport = now;
                hooks.log("NDL fed " + std::to_string(fed) + " frames, errors " + std::to_string(errs));
            }
        }
        av_packet_free(&pkt);
        ndl_close();
        if (bsf) av_bsf_free(&bsf);
        avformat_close_input(&fmt);
        hooks.log("NDL stopped");
        return;
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
