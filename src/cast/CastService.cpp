#include "CastService.h"

#include "sources/PortalClient.h"

#include <QtDebug>

CastService::CastService(QObject *parent)
    : QObject(parent), m_discovery(new CastDiscovery(this)), m_relay(new CastRelay(this))
{
    connect(m_discovery, &CastDiscovery::devicesChanged, this, &CastService::devicesChanged);
    connect(m_discovery, &CastDiscovery::message, this, [this](const QString &t) { emit message(t, true); });
    m_scanTimer.setSingleShot(true);
    m_scanTimer.setInterval(6000);
    connect(&m_scanTimer, &QTimer::timeout, this, &CastService::devicesChanged);
}

QVariantList CastService::devices() const
{
    QList<CastDevice> list = m_discovery->devices().values();
    std::sort(list.begin(), list.end(), [](const CastDevice &a, const CastDevice &b) {
        return a.name.localeAwareCompare(b.name) < 0;
    });
    QVariantList out;
    for (const CastDevice &d : list) out.append(d.toMap());
    return out;
}

void CastService::scan()
{
    m_discovery->scan();
    // Again a moment later: answers get lost on busy networks.
    QTimer::singleShot(1500, m_discovery, &CastDiscovery::scan);
    m_scanTimer.start();
    emit devicesChanged();
}

void CastService::forgetDevice(const QString &id)
{
    m_discovery->forget(id);
}

void CastService::addDevice(const QString &address)
{
    m_discovery->addByAddress(address);
}

QString CastService::mimeFor(const QString &url, bool live)
{
    const QString path = QUrl(url).path().toLower();
    if (path.endsWith(QLatin1String(".m3u8")) || path.endsWith(QLatin1String(".m3u"))) return QStringLiteral("application/x-mpegurl");
    if (path.endsWith(QLatin1String(".mp4")) || path.endsWith(QLatin1String(".m4v"))) return QStringLiteral("video/mp4");
    if (path.endsWith(QLatin1String(".mkv"))) return QStringLiteral("video/x-matroska");
    if (path.endsWith(QLatin1String(".avi"))) return QStringLiteral("video/x-msvideo");
    if (path.endsWith(QLatin1String(".webm"))) return QStringLiteral("video/webm");
    if (path.endsWith(QLatin1String(".mp3"))) return QStringLiteral("audio/mpeg");
    if (path.endsWith(QLatin1String(".aac"))) return QStringLiteral("audio/aac");
    return live || path.endsWith(QLatin1String(".ts")) ? QStringLiteral("video/mp2t") : QStringLiteral("video/mp4");
}

void CastService::setState(const QString &s)
{
    if (s == m_state) return;
    m_state = s;
    emit stateChanged();
}

void CastService::start(const QString &deviceId, const QVariantMap &source)
{
    const auto it = m_discovery->devices().constFind(deviceId);
    if (it == m_discovery->devices().constEnd()) { emit message(tr("That device isn't around any more · scan again"), true); return; }
    const QString url = source.value(QStringLiteral("url")).toString();
    if (url.isEmpty()) { emit message(tr("Nothing to cast"), true); return; }

    // Another device: let go of the one before.
    if (m_device.id != it->id) {
        if (m_dlna) { m_dlna->stopAndDelete(); m_dlna = nullptr; }
        if (m_cc) { m_cc->stopAndDelete(); m_cc = nullptr; }
    }
    m_device = it.value();
    m_title = source.value(QStringLiteral("title")).toString();
    m_live = source.value(QStringLiteral("live")).toBool();
    const quint64 gen = ++m_generation;
    setState(QStringLiteral("connecting"));
    qInfo().noquote() << QStringLiteral("Cast: \"%1\" to %2 \"%3\"").arg(m_title, m_device.kind, m_device.name);

    if (PortalClient::isPortalUrl(url)) {
        // The portal hands out the real address when asked.
        PortalClient::instance()->resolveAt(url, this, [this, gen, source](const QString &real, const QString &error, int skip) {
            if (gen != m_generation) return;
            if (real.isEmpty()) { finish(error); return; }
            QVariantMap s = source;
            if (skip > 1) s.insert(QStringLiteral("start"), s.value(QStringLiteral("start")).toDouble() + skip);
            begin(real, s);
        });
        return;
    }
    begin(url, source);
}

void CastService::begin(const QString &url, const QVariantMap &source)
{
    if (!m_path.isEmpty()) m_relay->remove(m_path);
    m_path.clear();
    const bool chromecast = m_device.kind == QLatin1String("chromecast");
    const QString mime = mimeFor(url, m_live);
    if (chromecast) { serve(url, source, mime); return; }

    if (!m_dlna) {
        m_dlna = new DlnaRenderer(m_device, this);
        connect(m_dlna, &DlnaRenderer::playing, this, [this] { setState(QStringLiteral("playing")); });
        connect(m_dlna, &DlnaRenderer::failed, this, [this](const QString &problem) { finish(problem); });
        connect(m_dlna, &DlnaRenderer::stopped, this, [this] { finish(QString()); });
    }
    // The name this TV uses for the type (Samsung is particular about it).
    const quint64 generation = m_generation;
    m_dlna->acceptedType(mime, [this, url, source, generation](const QString &type) {
        if (generation != m_generation || !m_dlna) return;   // stopped or moved on meanwhile
        serve(url, source, type);
    });
}

void CastService::serve(const QString &url, const QVariantMap &source, const QString &mime)
{
    const bool chromecast = m_device.kind == QLatin1String("chromecast");
    const bool hlsSource = mime == QLatin1String("application/x-mpegurl");

    CastRelay::Source rs;
    rs.url = url;
    rs.userAgent = source.value(QStringLiteral("userAgent")).toString();
    rs.referrer = source.value(QStringLiteral("referrer")).toString();
    // Chromecast plays HLS, not a raw TS stream: live TS goes out as HLS.
    rs.asHls = chromecast && m_live && !hlsSource && mime == QLatin1String("video/mp2t");
    rs.contentType = mime;
    if (!chromecast) {
        rs.forceType = !hlsSource;   // the same type the TV was told
        rs.dlnaFeatures = DlnaRenderer::features(m_live);
    }
    m_path = m_relay->add(rs);
    if (m_path.isEmpty()) { finish(tr("Couldn't start sharing the stream on this computer")); return; }
    const QString castUrl = m_relay->urlFor(m_path, m_device.address);
    const double start = source.value(QStringLiteral("start")).toDouble();
    qInfo().noquote() << "Cast: serving" << castUrl;

    if (chromecast) {
        if (!m_cc) {
            m_cc = new ChromecastClient(m_device, this);
            connect(m_cc, &ChromecastClient::playing, this, [this] { setState(QStringLiteral("playing")); });
            connect(m_cc, &ChromecastClient::failed, this, [this](const QString &problem) { finish(problem); });
            connect(m_cc, &ChromecastClient::stopped, this, [this] { finish(QString()); });
        }
        const QString type = rs.asHls || hlsSource ? QStringLiteral("application/x-mpegurl")
                           : mime == QLatin1String("video/x-matroska") ? QStringLiteral("video/mp4")   // what it plays them as
                           : mime;
        m_cc->load(castUrl, m_title, type, m_live, m_live ? 0 : start);
    } else {
        if (!m_live) m_dlna->seekWhenPlaying(start);
        m_dlna->load(castUrl, m_title, mime, m_live);
    }
}

void CastService::togglePause()
{
    if (m_state == QLatin1String("playing")) {
        if (m_cc) m_cc->pause();
        if (m_dlna) m_dlna->pause();
        setState(QStringLiteral("paused"));
    } else if (m_state == QLatin1String("paused")) {
        if (m_cc) m_cc->play();
        if (m_dlna) m_dlna->play();
        setState(QStringLiteral("playing"));
    }
}

void CastService::stop()
{
    if (!active()) return;
    ++m_generation;
    if (m_cc) { m_cc->stopAndDelete(); m_cc = nullptr; }
    if (m_dlna) { m_dlna->stopAndDelete(); m_dlna = nullptr; }
    finish(QString());
}

void CastService::finish(const QString &problem)
{
    if (!active()) return;
    ++m_generation;
    if (!problem.isEmpty()) {
        qWarning().noquote() << "Cast:" << problem;   // shown with ended()
    } else {
        qInfo().noquote() << "Cast: stopped";
    }
    if (m_cc) { m_cc->deleteLater(); m_cc = nullptr; }
    if (m_dlna) { m_dlna->deleteLater(); m_dlna = nullptr; }
    m_relay->clear();
    m_path.clear();
    setState(QString());
    emit ended(problem);
}
