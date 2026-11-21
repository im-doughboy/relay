#include "app/AppSettings.h"
#include "Recorder.h"

#include <QCoreApplication>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QtDebug>

namespace {
constexpr int kRetryMs = 3000;
constexpr int kMaxFailuresBeforeError = 20;   // ~1 minute of failures before reporting
}

Recorder::Recorder(QNetworkAccessManager *net, const QUrl &url, const QString &userAgent,
                   const QString &referrer, const QString &filePath, QObject *parent)
    : QObject(parent), m_net(net), m_url(url), m_userAgent(userAgent), m_referrer(referrer),
      m_file(filePath)
{
    m_hls = Hls::looksLikeHls(url);
    m_retry.setSingleShot(true);
    connect(&m_retry, &QTimer::timeout, this, &Recorder::connectStream);
    m_poll.setSingleShot(true);
    connect(&m_poll, &QTimer::timeout, this, &Recorder::fetchPlaylist);
}

Recorder::~Recorder()
{
    m_stopping = true;
    if (m_reply) m_reply->abort();
    m_file.close();
}

QNetworkReply *Recorder::get(const QUrl &url)
{
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader,
                  m_userAgent.isEmpty() ? AppSettings::effectiveUserAgent()
                                        : m_userAgent);
    if (!m_referrer.isEmpty()) req.setRawHeader("Referer", m_referrer.toUtf8());
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    // Live streams never finish; only give up on a connection that stalls.
    req.setTransferTimeout(20000);
    return m_net->get(req);
}

void Recorder::start()
{
    if (!m_file.open(QIODevice::WriteOnly | QIODevice::Append)) {
        m_error = tr("Couldn't create the recording file: %1").arg(m_file.errorString());
        setState(State::Failed);
        emit stopped();
        return;
    }
    connectStream();
}

void Recorder::stop()
{
    if (m_stopping) return;
    m_stopping = true;
    m_retry.stop();
    m_poll.stop();
    if (m_reply) m_reply->abort();
    m_file.close();
    if (m_bytes == 0 && m_error.isEmpty()) m_error = tr("Nothing was received from the channel.");
    setState(m_bytes > 0 ? State::Finished : State::Failed);
    emit stopped();
}

void Recorder::setState(State s)
{
    if (s == m_state) return;
    m_state = s;
    emit changed();
}

void Recorder::write(const QByteArray &data)
{
    if (data.isEmpty() || !m_file.isOpen()) return;
    if (m_file.write(data) < 0) {
        m_error = tr("Couldn't write the recording: %1").arg(m_file.errorString());
        stop();
        return;
    }
    m_bytes += data.size();
    m_failures = 0;
    if (m_state != State::Recording) { m_error.clear(); setState(State::Recording); }
    emit changed();
}

void Recorder::retryLater(const QString &why)
{
    if (m_stopping) return;
    ++m_failures;
    qInfo().noquote() << "Recording: connection problem (" << why << "), retrying";
    if (m_failures >= kMaxFailuresBeforeError) m_error = why;
    setState(State::Retrying);
    emit changed();
    m_retry.start(kRetryMs);
}

void Recorder::connectStream()
{
    if (m_stopping) return;
    if (m_hls) { fetchPlaylist(); return; }

    m_head.clear();
    QNetworkReply *reply = get(m_url);
    m_reply = reply;
    connect(reply, &QNetworkReply::readyRead, this, [this, reply] {
        QByteArray data = reply->readAll();
        // Some servers answer a stream address with an HLS playlist.
        if (m_head.size() < 7) {
            m_head += data.left(7 - m_head.size());
            if (m_head.size() >= 7 && m_head.startsWith("#EXTM3U")) {
                m_hls = true;
                m_mediaUrl.clear();
                reply->abort();
                QMetaObject::invokeMethod(this, &Recorder::fetchPlaylist, Qt::QueuedConnection);
                return;
            }
        }
        write(data);
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        if (m_stopping || m_hls) return;
        const int http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (http >= 400) retryLater(tr("the server responded with HTTP %1").arg(http));
        else if (reply->error() != QNetworkReply::NoError) retryLater(reply->errorString());
        else retryLater(tr("the stream ended"));
    });
}

// ---- HLS ----------------------------------------------------------------------------

void Recorder::fetchPlaylist()
{
    if (m_stopping) return;
    const QUrl url = m_mediaUrl.isEmpty() ? m_url : m_mediaUrl;
    QNetworkReply *reply = get(url);
    m_reply = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        if (m_stopping) return;
        if (reply->error() != QNetworkReply::NoError) { retryLater(reply->errorString()); return; }
        const QUrl base = reply->url();
        const Hls::Playlist p = Hls::parse(reply->readAll(), base);
        if (!p.valid) { retryLater(tr("the server didn't send a playlist")); return; }
        if (p.master) {
            if (p.variants.isEmpty()) { retryLater(tr("the playlist has no streams")); return; }
            m_mediaUrl = p.variants.first();
            fetchPlaylist();
            return;
        }
        onPlaylist(p);
    });
}

void Recorder::onPlaylist(const Hls::Playlist &p)
{
    if (p.encrypted || p.fragmentedMp4) {
        m_error = p.encrypted ? tr("This channel is encrypted and can't be recorded.")
                              : tr("This channel's format can't be recorded yet.");
        m_stopping = true;
        m_file.close();
        setState(State::Failed);
        emit stopped();
        return;
    }
    // On the first look, start near live (the last three segments).
    if (m_lastSequence < 0 && p.segments.size() > 3)
        m_lastSequence = p.segments[p.segments.size() - 4].sequence;
    for (const Hls::Segment &s : p.segments)
        if (s.sequence > m_lastSequence) m_queue.append(s);
    if (m_queue.isEmpty()) {
        if (p.endList) { stop(); return; }
        m_poll.start(int(qMax(1.0, p.targetDuration / 2) * 1000));
        return;
    }
    nextSegment();
}

void Recorder::nextSegment()
{
    if (m_stopping) return;
    if (m_queue.isEmpty()) { m_poll.start(1000); return; }
    const Hls::Segment seg = m_queue.takeFirst();
    QNetworkReply *reply = get(seg.url);
    m_reply = reply;
    connect(reply, &QNetworkReply::readyRead, this, [this, reply] { write(reply->readAll()); });
    connect(reply, &QNetworkReply::finished, this, [this, reply, seg] {
        reply->deleteLater();
        if (m_stopping) return;
        if (reply->error() != QNetworkReply::NoError) {
            // Skip a missing segment; the playlist moves on anyway.
            qInfo().noquote() << "Recording: segment failed:" << reply->errorString();
            ++m_failures;
            if (m_failures >= kMaxFailuresBeforeError) m_error = reply->errorString();
        }
        m_lastSequence = qMax(m_lastSequence, seg.sequence);
        if (m_queue.isEmpty()) fetchPlaylist();
        else nextSegment();
    });
}
