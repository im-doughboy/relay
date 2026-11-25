#include "Remuxer.h"

#include "app/Languages.h"

#include <QMetaObject>
#include <QtDebug>

#include <chrono>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/mathematics.h>
}

namespace {

// FFmpeg 7 made the buffer const.
#if LIBAVFORMAT_VERSION_MAJOR >= 61
int collect(void *opaque, const uint8_t *data, int size)
#else
int collect(void *opaque, uint8_t *data, int size)
#endif
{
    static_cast<QByteArray *>(opaque)->append(reinterpret_cast<const char *>(data), size);
    return size;
}

int interrupted(void *flag) { return static_cast<std::atomic<bool> *>(flag)->load() ? 1 : 0; }

QString avError(int code)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buf, sizeof buf);
    return QString::fromUtf8(buf);
}

// Opens the provider's address the way the relay asks for it elsewhere.
int openInput(AVFormatContext **ic, const QString &url, const QString &userAgent, const QString &referrer,
              std::atomic<bool> *stop)
{
    *ic = avformat_alloc_context();
    (*ic)->interrupt_callback.callback = interrupted;
    (*ic)->interrupt_callback.opaque = stop;
    AVDictionary *opts = nullptr;
    if (!userAgent.isEmpty()) av_dict_set(&opts, "user_agent", userAgent.toUtf8().constData(), 0);
    if (!referrer.isEmpty())
        av_dict_set(&opts, "headers", ("Referer: " + referrer.toUtf8() + "\r\n").constData(), 0);
    av_dict_set(&opts, "reconnect", "1", 0);
    av_dict_set(&opts, "reconnect_on_network_error", "1", 0);
    av_dict_set(&opts, "reconnect_delay_max", "10", 0);
    av_dict_set(&opts, "rw_timeout", "20000000", 0);   // 20 s without data: give up
    const int r = avformat_open_input(ic, url.toUtf8().constData(), nullptr, &opts);
    av_dict_free(&opts);
    if (r < 0) return r;
    return avformat_find_stream_info(*ic, nullptr);
}

QList<Remuxer::Track> audioTracks(AVFormatContext *ic, QList<int> *indexes = nullptr)
{
    QList<Remuxer::Track> out;
    for (unsigned i = 0; i < ic->nb_streams; ++i) {
        const AVStream *s = ic->streams[i];
        if (s->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) continue;
        Remuxer::Track t;
        if (const AVDictionaryEntry *e = av_dict_get(s->metadata, "language", nullptr, 0)) t.lang = QString::fromUtf8(e->value);
        if (const AVDictionaryEntry *e = av_dict_get(s->metadata, "title", nullptr, 0)) t.title = QString::fromUtf8(e->value);
        t.isDefault = s->disposition & AV_DISPOSITION_DEFAULT;
        out << t;
        if (indexes) *indexes << int(i);
    }
    return out;
}

}

Remuxer::Remuxer(const Options &options, QObject *parent) : QObject(parent), m_options(options) {}

Remuxer::~Remuxer()
{
    m_stop = true;
    if (m_thread.joinable()) m_thread.join();
}

void Remuxer::start()
{
    if (m_thread.joinable()) return;
    m_thread = std::thread([this] { run(); });
}

Remuxer::Probe Remuxer::probe(const QString &url, const QString &userAgent, const QString &referrer)
{
    Probe p;
    std::atomic<bool> stop{false};
    AVFormatContext *ic = nullptr;
    const int r = openInput(&ic, url, userAgent, referrer, &stop);
    if (r < 0) {
        p.error = avError(r);
    } else {
        p.duration = ic->duration > 0 ? double(ic->duration) / AV_TIME_BASE : 0;
        p.audio = audioTracks(ic);
    }
    avformat_close_input(&ic);
    return p;
}

int Remuxer::receiverDefault(const QList<Track> &audio)
{
    for (int i = 0; i < audio.size(); ++i)
        if (audio[i].isDefault) return i;
    return audio.isEmpty() ? -1 : 0;
}

int Remuxer::choose(const QList<Track> &audio, int pickedIndex, const QString &pickedLang,
                    const QString &pickedTitle, const QString &preferred)
{
    const bool picked = pickedIndex >= 0 || !pickedLang.isEmpty() || !pickedTitle.isEmpty();
    if (picked) {
        // The same track as in the player: same place and same labels, or
        // failing the place, the same labels.
        if (pickedIndex >= 0 && pickedIndex < audio.size()
            && audio[pickedIndex].lang == pickedLang && audio[pickedIndex].title == pickedTitle)
            return pickedIndex;
        for (int i = 0; i < audio.size(); ++i)
            if (audio[i].lang == pickedLang && audio[i].title == pickedTitle) return i;
    }
    if (!preferred.isEmpty())
        for (int i = 0; i < audio.size(); ++i)
            if (Languages::matches(preferred, audio[i].lang, audio[i].title)) return i;
    if (pickedIndex >= 0 && pickedIndex < audio.size()) return pickedIndex;
    return -1;
}

void Remuxer::run()
{
    QString error;
    AVFormatContext *ic = nullptr;
    AVFormatContext *oc = nullptr;
    AVPacket *pkt = av_packet_alloc();
    QByteArray pending;   // MPEG-TS not handed out yet

    auto flush = [&](bool all) {
        if (pending.size() >= 128 * 1024 || (all && !pending.isEmpty())) {
            const QByteArray chunk = pending;
            pending.clear();
            ++m_inFlight;
            QMetaObject::invokeMethod(this, [this, chunk] { emit data(chunk); }, Qt::QueuedConnection);
        }
    };

    do {
        int r = openInput(&ic, m_options.url, m_options.userAgent, m_options.referrer, &m_stop);
        if (r < 0) { error = QStringLiteral("couldn't open the video (%1)").arg(avError(r)); break; }

        const int video = av_find_best_stream(ic, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        QList<int> audioIndexes;
        audioTracks(ic, &audioIndexes);
        int audio = m_options.audio >= 0 && m_options.audio < audioIndexes.size() ? audioIndexes[m_options.audio]
                  : av_find_best_stream(ic, AVMEDIA_TYPE_AUDIO, -1, video, nullptr, 0);
        if (video < 0) { error = QStringLiteral("no picture in the video"); break; }

        r = avformat_alloc_output_context2(&oc, nullptr, "mpegts", nullptr);
        if (r < 0 || !oc) { error = QStringLiteral("couldn't start the MPEG-TS output"); break; }
        const int bufSize = 188 * 64;
        auto *buf = static_cast<unsigned char *>(av_malloc(bufSize));
        oc->pb = avio_alloc_context(buf, bufSize, 1, &pending, nullptr, collect, nullptr);

        int map[64];
        for (int &m : map) m = -1;
        for (int in : {video, audio}) {
            if (in < 0 || in >= 64) continue;
            AVStream *os = avformat_new_stream(oc, nullptr);
            avcodec_parameters_copy(os->codecpar, ic->streams[in]->codecpar);
            os->codecpar->codec_tag = 0;
            if (const AVDictionaryEntry *e = av_dict_get(ic->streams[in]->metadata, "language", nullptr, 0))
                av_dict_set(&os->metadata, "language", e->value, 0);
            map[in] = os->index;
        }
        r = avformat_write_header(oc, nullptr);
        if (r < 0) { error = QStringLiteral("couldn't write the MPEG-TS header (%1)").arg(avError(r)); break; }

        if (m_options.start > 1) {
            const int64_t target = int64_t(m_options.start * AV_TIME_BASE)
                                 + (ic->start_time != AV_NOPTS_VALUE ? ic->start_time : 0);
            if (avformat_seek_file(ic, -1, INT64_MIN, target, target, 0) < 0)
                qWarning().noquote() << "Cast: couldn't jump to" << m_options.start << "s; starting at the beginning";
        }

        // The output starts at zero from the first key frame, so the receiver
        // counts from the start of what it was sent.
        int64_t base = AV_NOPTS_VALUE;   // in AV_TIME_BASE
        while (!m_stop) {
            while ((m_hold || m_inFlight >= 2) && !m_stop) std::this_thread::sleep_for(std::chrono::milliseconds(10));
            if (m_stop) break;
            r = av_read_frame(ic, pkt);
            if (r == AVERROR(EAGAIN)) continue;
            if (r == AVERROR_EOF) break;
            if (r < 0) { if (!m_stop) error = QStringLiteral("the provider's connection failed (%1)").arg(avError(r)); break; }
            const int in = pkt->stream_index;
            if (in >= 64 || map[in] < 0) { av_packet_unref(pkt); continue; }
            AVStream *is = ic->streams[in];
            if (base == AV_NOPTS_VALUE) {
                if (in != video || !(pkt->flags & AV_PKT_FLAG_KEY) || pkt->dts == AV_NOPTS_VALUE) { av_packet_unref(pkt); continue; }
                base = av_rescale_q(pkt->dts, is->time_base, AV_TIME_BASE_Q);
            }
            const int64_t shift = av_rescale_q(base, AV_TIME_BASE_Q, is->time_base);
            if (pkt->pts != AV_NOPTS_VALUE) pkt->pts -= shift;
            if (pkt->dts != AV_NOPTS_VALUE) pkt->dts -= shift;
            if ((pkt->dts != AV_NOPTS_VALUE && pkt->dts < 0) || (pkt->pts != AV_NOPTS_VALUE && pkt->pts < 0)) {
                av_packet_unref(pkt);   // sound from just before the first picture
                continue;
            }
            AVStream *os = oc->streams[map[in]];
            av_packet_rescale_ts(pkt, is->time_base, os->time_base);
            pkt->stream_index = map[in];
            pkt->pos = -1;
            r = av_interleaved_write_frame(oc, pkt);   // frees the packet's data
            if (r < 0) qWarning().noquote() << "Cast: a packet didn't fit the MPEG-TS output:" << avError(r);
            flush(false);
        }
        if (!m_stop && error.isEmpty()) {
            av_write_trailer(oc);
            avio_flush(oc->pb);
        }
    } while (false);

    flush(true);
    av_packet_free(&pkt);
    if (oc) {
        if (oc->pb) { av_freep(&oc->pb->buffer); avio_context_free(&oc->pb); }
        avformat_free_context(oc);
    }
    avformat_close_input(&ic);
    if (!m_stop) QMetaObject::invokeMethod(this, [this, error] { emit ended(error); }, Qt::QueuedConnection);
}
