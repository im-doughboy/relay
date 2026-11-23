#include "CastDevices.h"

#include <QHash>

#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkDatagram>
#include <QNetworkInterface>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSettings>
#include <QSslSocket>
#include <QUdpSocket>
#include <QXmlStreamReader>
#include <QtDebug>
#include <QtEndian>

namespace {
const QHostAddress kMdnsGroup(QStringLiteral("224.0.0.251"));
const QHostAddress kSsdpGroup(QStringLiteral("239.255.255.250"));
const char kMediaReceiver[] = "CC1AD845";   // Google's Default Media Receiver
const char kNsConnection[] = "urn:x-cast:com.google.cast.tp.connection";
const char kNsHeartbeat[] = "urn:x-cast:com.google.cast.tp.heartbeat";
const char kNsReceiver[] = "urn:x-cast:com.google.cast.receiver";
const char kNsMedia[] = "urn:x-cast:com.google.cast.media";
const char kAvTransport[] = "urn:schemas-upnp-org:service:AVTransport:1";
const char kConnectionManager[] = "urn:schemas-upnp-org:service:ConnectionManager:1";

QString xmlEscape(const QString &s)
{
    QString out = s;
    out.replace(QLatin1Char('&'), QLatin1String("&amp;")).replace(QLatin1Char('<'), QLatin1String("&lt;"))
       .replace(QLatin1Char('>'), QLatin1String("&gt;")).replace(QLatin1Char('"'), QLatin1String("&quot;"));
    return out;
}
} // namespace

QVariantMap CastDevice::toMap() const
{
    return {{QStringLiteral("id"), id}, {QStringLiteral("name"), name}, {QStringLiteral("model"), model},
            {QStringLiteral("kind"), kind}, {QStringLiteral("address"), address.toString()}};
}

// ---- discovery ---------------------------------------------------------------------------------

CastDiscovery::CastDiscovery(QObject *parent) : QObject(parent), m_net(new QNetworkAccessManager(this)) {}

QByteArray CastDiscovery::mdnsQuery(const QByteArray &service)
{
    QByteArray q(12, '\0');
    q[5] = 1;                                   // one question
    for (const QByteArray &label : service.split('.')) {
        if (label.isEmpty()) continue;
        q += char(label.size());
        q += label;
    }
    q += '\0';
    q += QByteArray::fromHex("000c8001");       // PTR, IN with "answer me directly"
    return q;
}

namespace {
// A DNS name at `pos` (following compression pointers); moves `pos` past it.
QString readName(const QByteArray &p, int &pos)
{
    QStringList labels;
    int at = pos, jumps = 0;
    bool jumped = false;
    while (at < p.size() && jumps < 20) {
        const int len = uchar(p[at]);
        if (len == 0) { if (!jumped) pos = at + 1; break; }
        if ((len & 0xC0) == 0xC0) {
            if (at + 1 >= p.size()) break;
            if (!jumped) pos = at + 2;
            at = ((len & 0x3F) << 8) | uchar(p[at + 1]);
            jumped = true;
            ++jumps;
            continue;
        }
        labels << QString::fromUtf8(p.mid(at + 1, len));
        at += 1 + len;
    }
    return labels.join(QLatin1Char('.'));
}
} // namespace

QList<CastDevice> CastDiscovery::parseMdns(const QByteArray &p, const QHostAddress &from)
{
    QList<CastDevice> out;
    if (p.size() < 12) return out;
    const int counts = qFromBigEndian<quint16>(p.constData() + 6) + qFromBigEndian<quint16>(p.constData() + 8)
                     + qFromBigEndian<quint16>(p.constData() + 10);
    const int questions = qFromBigEndian<quint16>(p.constData() + 4);
    int pos = 12;
    for (int i = 0; i < questions && pos < p.size(); ++i) { readName(p, pos); pos += 4; }
    QStringList instances;
    QHash<QString, QPair<QString, quint16>> srv;    // instance -> host, port
    QHash<QString, QHash<QString, QString>> txt;    // instance -> key -> value
    QHash<QString, QHostAddress> hosts;             // host name -> IPv4
    for (int i = 0; i < counts && pos + 10 <= p.size(); ++i) {
        const QString name = readName(p, pos);
        if (pos + 10 > p.size()) break;
        const int type = qFromBigEndian<quint16>(p.constData() + pos);
        const int len = qFromBigEndian<quint16>(p.constData() + pos + 8);
        const int data = pos + 10;
        pos = data + len;
        if (pos > p.size()) break;
        if (type == 12) {                                         // PTR
            int at = data;
            const QString instance = readName(p, at);
            if (instance.contains(QLatin1String("_googlecast._tcp"))) instances << instance;
        } else if (type == 33 && len >= 7) {                      // SRV
            int at = data + 6;
            srv.insert(name, {readName(p, at), qFromBigEndian<quint16>(p.constData() + data + 4)});
        } else if (type == 16) {                                  // TXT
            for (int at = data; at < data + len;) {
                const int l = uchar(p[at]);
                const QString kv = QString::fromUtf8(p.mid(at + 1, l));
                const int eq = kv.indexOf(QLatin1Char('='));
                if (eq > 0) txt[name].insert(kv.left(eq), kv.mid(eq + 1));
                at += 1 + l;
            }
        } else if (type == 1 && len == 4) {                       // A
            hosts.insert(name, QHostAddress(qFromBigEndian<quint32>(p.constData() + data)));
        }
    }
    for (const QString &instance : std::as_const(srv).keys())
        if (!instances.contains(instance) && instance.contains(QLatin1String("_googlecast._tcp"))) instances << instance;
    for (const QString &instance : instances) {
        CastDevice d;
        d.kind = QStringLiteral("chromecast");
        const auto t = txt.value(instance);
        d.name = t.value(QStringLiteral("fn"), instance.section(QLatin1Char('.'), 0, 0));
        d.model = t.value(QStringLiteral("md"), QStringLiteral("Chromecast"));
        const auto s = srv.value(instance);
        d.port = s.second ? s.second : 8009;
        d.address = hosts.value(s.first, from);
        if (d.address.isNull()) continue;
        d.id = QStringLiteral("cc:%1:%2").arg(d.address.toString()).arg(d.port);
        out.append(d);
    }
    return out;
}

QByteArray CastDiscovery::ssdpSearch()
{
    return QByteArray("M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 2\r\nST: ")
           + kAvTransport + "\r\nUSER-AGENT: Relay/1.0 UPnP/1.1\r\n\r\n";
}

QUrl CastDiscovery::ssdpLocation(const QByteArray &answer)
{
    for (const QByteArray &line : answer.split('\n')) {
        const int colon = line.indexOf(':');
        if (colon > 0 && line.left(colon).trimmed().toLower() == "location")
            return QUrl(QString::fromUtf8(line.mid(colon + 1).trimmed()));
    }
    return QUrl();
}

CastDevice CastDiscovery::parseDescription(const QByteArray &xml, const QUrl &location)
{
    CastDevice d;
    d.kind = QStringLiteral("dlna");
    QXmlStreamReader r(xml);
    QUrl base = location;
    QString serviceType, controlUrl, found, connection;
    while (!r.atEnd()) {
        r.readNext();
        if (!r.isStartElement()) continue;
        const QStringView n = r.name();
        if (n == QLatin1String("URLBase")) base = QUrl(r.readElementText().trimmed());
        else if (n == QLatin1String("friendlyName") && d.name.isEmpty()) d.name = r.readElementText().trimmed();
        else if (n == QLatin1String("modelName") && d.model.isEmpty()) d.model = r.readElementText().trimmed();
        else if (n == QLatin1String("service")) { serviceType.clear(); controlUrl.clear(); }
        else if (n == QLatin1String("serviceType")) serviceType = r.readElementText().trimmed();
        else if (n == QLatin1String("controlURL")) {
            controlUrl = r.readElementText().trimmed();
            if (found.isEmpty() && serviceType.startsWith(QLatin1String("urn:schemas-upnp-org:service:AVTransport:")))
                found = controlUrl;
            if (connection.isEmpty() && serviceType.startsWith(QLatin1String("urn:schemas-upnp-org:service:ConnectionManager:")))
                connection = controlUrl;
        }
    }
    if (!found.isEmpty()) d.controlUrl = base.resolved(QUrl(found));
    if (!connection.isEmpty()) d.connectionUrl = base.resolved(QUrl(connection));
    d.address = QHostAddress(location.host());
    d.id = QStringLiteral("dlna:") + d.controlUrl.toString();
    if (d.name.isEmpty()) d.name = location.host();
    return d;
}

void CastDiscovery::scan()
{
    // mDNS: ask, and listen on the group too (some devices answer there only).
    if (!m_mdnsQuery) {
        m_mdnsQuery = new QUdpSocket(this);
        m_mdnsQuery->bind(QHostAddress::AnyIPv4, 0);
        connect(m_mdnsQuery, &QUdpSocket::readyRead, this, [this] { readMdns(m_mdnsQuery); });
        m_mdnsListen = new QUdpSocket(this);
        if (m_mdnsListen->bind(QHostAddress::AnyIPv4, 5353, QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint))
            m_mdnsListen->joinMulticastGroup(kMdnsGroup);
        connect(m_mdnsListen, &QUdpSocket::readyRead, this, [this] { readMdns(m_mdnsListen); });
        m_ssdp = new QUdpSocket(this);
        m_ssdp->bind(QHostAddress::AnyIPv4, 0);
        connect(m_ssdp, &QUdpSocket::readyRead, this, &CastDiscovery::readSsdp);
    }
    for (const QString &manual : QSettings().value(QStringLiteral("cast/manual")).toStringList())
        if (manual.startsWith(QLatin1String("http"))) fetchDescription(QUrl(manual));
        else if (!m_devices.contains(QStringLiteral("cc:%1:%2").arg(manual.section(QLatin1Char(':'), 0, 0))
                                         .arg(manual.contains(QLatin1Char(':')) ? manual.section(QLatin1Char(':'), 1) : QStringLiteral("8009"))))
            addByAddress(manual);
    const QByteArray mdns = mdnsQuery("_googlecast._tcp.local");
    const QByteArray ssdp = ssdpSearch();
    // On every network the computer is on.
    for (const QNetworkInterface &nif : QNetworkInterface::allInterfaces()) {
        if (!(nif.flags() & QNetworkInterface::IsUp) || !(nif.flags() & QNetworkInterface::CanMulticast)
            || (nif.flags() & QNetworkInterface::IsLoopBack)) continue;
        m_mdnsQuery->setMulticastInterface(nif);
        m_mdnsQuery->writeDatagram(mdns, kMdnsGroup, 5353);
        m_ssdp->setMulticastInterface(nif);
        m_ssdp->writeDatagram(ssdp, kSsdpGroup, 1900);
    }
    m_mdnsQuery->writeDatagram(mdns, kMdnsGroup, 5353);
    m_ssdp->writeDatagram(ssdp, kSsdpGroup, 1900);
}

void CastDiscovery::readMdns(QUdpSocket *socket)
{
    while (socket->hasPendingDatagrams()) {
        const QNetworkDatagram d = socket->receiveDatagram();
        for (const CastDevice &dev : parseMdns(d.data(), d.senderAddress())) addDevice(dev);
    }
}

void CastDiscovery::readSsdp()
{
    while (m_ssdp->hasPendingDatagrams()) {
        const QNetworkDatagram d = m_ssdp->receiveDatagram();
        const QUrl location = ssdpLocation(d.data());
        if (location.isValid()) fetchDescription(location);
    }
}

void CastDiscovery::fetchDescription(const QUrl &location)
{
    if (m_described.contains(location.toString())) return;
    m_described.insert(location.toString());
    QNetworkRequest req(location);
    req.setTransferTimeout(5000);
    QNetworkReply *reply = m_net->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, location] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) { m_described.remove(location.toString()); return; }
        const CastDevice d = parseDescription(reply->readAll(), location);
        if (d.controlUrl.isValid()) addDevice(d);
    });
}

void CastDiscovery::addDevice(const CastDevice &d)
{
    // Chromecasts with Google TV also answer as DLNA renderers: keep one.
    if (d.kind == QLatin1String("dlna"))
        for (const CastDevice &other : std::as_const(m_devices))
            if (other.kind == QLatin1String("chromecast") && other.address == d.address) return;
    if (d.kind == QLatin1String("chromecast"))
        for (auto it = m_devices.begin(); it != m_devices.end();)
            it = it->kind == QLatin1String("dlna") && it->address == d.address ? m_devices.erase(it) : std::next(it);
    const bool isNew = !m_devices.contains(d.id);
    m_devices.insert(d.id, d);
    if (isNew) qInfo().noquote() << QStringLiteral("Cast: found %1 \"%2\" (%3)").arg(d.kind, d.name, d.address.toString());
    emit devicesChanged();
}

void CastDiscovery::forget(const QString &id)
{
    const auto it = m_devices.constFind(id);
    if (it == m_devices.constEnd()) return;
    QSettings settings;
    QStringList manual = settings.value(QStringLiteral("cast/manual")).toStringList();
    manual.removeIf([&it](const QString &m) {
        return QUrl(m).host() == it->address.toString() || m.section(QLatin1Char(':'), 0, 0) == it->address.toString();
    });
    settings.setValue(QStringLiteral("cast/manual"), manual);
    m_described.remove(it->controlUrl.toString());
    m_devices.remove(id);
    emit devicesChanged();
}

void CastDiscovery::addByAddress(const QString &textIn)
{
    const QString text = textIn.trimmed();
    if (text.isEmpty()) return;
    // Remembered, for networks where the scan can't see it.
    QSettings settings;
    QStringList manual = settings.value(QStringLiteral("cast/manual")).toStringList();
    if (!manual.contains(text)) { manual.append(text); settings.setValue(QStringLiteral("cast/manual"), manual); }
    if (text.startsWith(QLatin1String("http"))) {
        m_described.remove(text);
        fetchDescription(QUrl(text));
        return;
    }
    const QString host = text.section(QLatin1Char(':'), 0, 0);
    const QHostAddress address(host);
    if (address.isNull()) { emit message(tr("Enter an address like 192.168.1.20")); return; }
    CastDevice d;
    d.kind = QStringLiteral("chromecast");
    d.address = address;
    d.port = text.contains(QLatin1Char(':')) ? quint16(text.section(QLatin1Char(':'), 1).toUInt()) : 8009;
    d.name = tr("Chromecast at %1").arg(host);
    d.model = QStringLiteral("Chromecast");
    d.id = QStringLiteral("cc:%1:%2").arg(address.toString()).arg(d.port);
    addDevice(d);
}

// ---- DLNA -----------------------------------------------------------------------------------------

DlnaRenderer::DlnaRenderer(const CastDevice &device, QObject *parent)
    : QObject(parent), m_device(device), m_net(new QNetworkAccessManager(this))
{
    m_poll.setInterval(3000);
    connect(&m_poll, &QTimer::timeout, this, &DlnaRenderer::pollState);
}

QByteArray DlnaRenderer::soapBody(const QString &action, const QString &args, const char *service)
{
    // ConnectionManager's actions take no InstanceID.
    const bool avt = !service || qstrcmp(service, kAvTransport) == 0;
    return QStringLiteral("<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                          "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
                          "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body>"
                          "<u:%1 xmlns:u=\"%2\">%3%4</u:%1></s:Body></s:Envelope>")
        .arg(action, QLatin1String(service ? service : kAvTransport),
             avt ? QStringLiteral("<InstanceID>0</InstanceID>") : QString(), args).toUtf8();
}

QString DlnaRenderer::features(bool live)
{
    return live ? QStringLiteral("DLNA.ORG_OP=00;DLNA.ORG_CI=0;DLNA.ORG_FLAGS=01700000000000000000000000000000")
                : QStringLiteral("DLNA.ORG_OP=01;DLNA.ORG_CI=0;DLNA.ORG_FLAGS=01500000000000000000000000000000");
}

QString DlnaRenderer::pickType(const QString &sink, const QString &mime)
{
    // Other names TVs use for the same kind of stream, best first.
    static const QHash<QString, QStringList> aliases{
        {QStringLiteral("video/mp2t"), {QStringLiteral("video/mp2t"), QStringLiteral("video/vnd.dlna.mpeg-tts"),
                                        QStringLiteral("video/mpeg"), QStringLiteral("video/x-mpegts")}},
        {QStringLiteral("video/x-matroska"), {QStringLiteral("video/x-matroska"), QStringLiteral("video/x-mkv"),
                                              QStringLiteral("video/mkv")}},
        {QStringLiteral("video/mp4"), {QStringLiteral("video/mp4"), QStringLiteral("video/mpeg4")}},
        {QStringLiteral("video/x-msvideo"), {QStringLiteral("video/x-msvideo"), QStringLiteral("video/avi")}},
        {QStringLiteral("application/vnd.apple.mpegurl"), {QStringLiteral("application/vnd.apple.mpegurl"),
                                                           QStringLiteral("application/x-mpegurl"), QStringLiteral("audio/mpegurl"),
                                                           QStringLiteral("audio/x-mpegurl")}},
    };
    QHash<QString, QString> offered;   // lower case -> as the TV wrote it
    for (const QString &entry : sink.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        const QStringList f = entry.trimmed().split(QLatin1Char(':'));
        if (f.size() >= 3 && f[0].compare(QLatin1String("http-get"), Qt::CaseInsensitive) == 0 && !offered.contains(f[2].toLower()))
            offered.insert(f[2].toLower(), f[2]);
    }
    if (offered.isEmpty()) return mime;
    for (const QString &name : aliases.value(mime, {mime}))
        if (offered.contains(name)) return offered.value(name);
    return mime;
}

QByteArray DlnaRenderer::didl(const QString &url, const QString &title, const QString &mime, bool live)
{
    return QStringLiteral("<DIDL-Lite xmlns=\"urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/\" "
                          "xmlns:dc=\"http://purl.org/dc/elements/1.1/\" xmlns:upnp=\"urn:schemas-upnp-org:metadata-1-0/upnp/\">"
                          "<item id=\"relay\" parentID=\"0\" restricted=\"1\"><dc:title>%1</dc:title>"
                          "<upnp:class>object.item.videoItem</upnp:class>"
                          "<res protocolInfo=\"http-get:*:%2:%3\">%4</res></item></DIDL-Lite>")
        .arg(xmlEscape(title), mime, features(live), xmlEscape(url)).toUtf8();
}

void DlnaRenderer::call(const QString &action, const QString &args, std::function<void(bool, const QByteArray &)> done,
                        const QUrl &url, const char *service)
{
    const char *svc = service ? service : kAvTransport;
    QNetworkRequest req(url.isValid() ? url : m_device.controlUrl);
    req.setRawHeader("Content-Type", "text/xml; charset=\"utf-8\"");
    req.setRawHeader("SOAPAction", QStringLiteral("\"%1#%2\"").arg(QLatin1String(svc), action).toUtf8());
    req.setTransferTimeout(8000);
    QNetworkReply *reply = m_net->post(req, soapBody(action, args, svc));
    connect(reply, &QNetworkReply::finished, this, [reply, action, done] {
        reply->deleteLater();
        const bool ok = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200;
        const QByteArray body = reply->readAll();
        if (!ok) qWarning().noquote() << "Cast: TV refused" << action << body.left(300);
        if (done) done(ok, body);
    });
}

void DlnaRenderer::acceptedType(const QString &mime, std::function<void(const QString &)> done)
{
    if (m_askedTypes || !m_device.connectionUrl.isValid()) {
        const QString type = pickType(m_sink, mime);
        if (type != mime) qInfo().noquote() << "Cast: the TV calls" << mime << "\"" + type + "\"";
        done(type);
        return;
    }
    call(QStringLiteral("GetProtocolInfo"), QString(), [this, mime, done](bool ok, const QByteArray &body) {
        m_askedTypes = true;
        if (ok) {
            const int at = body.indexOf("<Sink>");
            const int end = body.indexOf("</Sink>", at);
            if (at >= 0 && end > at) {
                QString sink = QString::fromUtf8(body.mid(at + 6, end - at - 6));
                sink.replace(QLatin1String("&amp;"), QLatin1String("&"));
                m_sink = sink;
            }
            QStringList types;
            for (const QString &e : m_sink.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
                const QString t = e.section(QLatin1Char(':'), 2, 2);
                if (!t.isEmpty() && !types.contains(t)) types << t;
            }
            qInfo().noquote() << "Cast: the TV plays" << (types.isEmpty() ? QStringLiteral("(it didn't say)") : types.join(QLatin1String(", ")));
        }
        const QString type = pickType(m_sink, mime);
        if (type != mime) qInfo().noquote() << "Cast: the TV calls" << mime << "\"" + type + "\"";
        done(type);
    }, m_device.connectionUrl, kConnectionManager);
}

void DlnaRenderer::load(const QString &url, const QString &title, const QString &mime, bool live)
{
    m_started = false;
    m_stoppedPolls = 0;
    m_lastState.clear();
    qInfo().noquote() << "Cast: asking the TV to play" << mime << (live ? "(live)" : "");
    const QString meta = QString::fromUtf8(didl(url, title, mime, live));
    call(QStringLiteral("SetAVTransportURI"),
         QStringLiteral("<CurrentURI>%1</CurrentURI><CurrentURIMetaData>%2</CurrentURIMetaData>").arg(xmlEscape(url), xmlEscape(meta)),
         [this](bool ok, const QByteArray &) {
             if (!ok) { emit failed(tr("The TV didn't accept the stream")); return; }
             play();
         });
}

void DlnaRenderer::play()
{
    call(QStringLiteral("Play"), QStringLiteral("<Speed>1</Speed>"), [this](bool ok, const QByteArray &) {
        if (!ok) { emit failed(tr("The TV didn't start playing")); return; }
        m_poll.start();
    });
}

void DlnaRenderer::pause() { call(QStringLiteral("Pause"), QString()); }

void DlnaRenderer::seek(double secs)
{
    const int t = int(secs);
    const QString target = QStringLiteral("%1:%2:%3").arg(t / 3600).arg(t / 60 % 60, 2, 10, QLatin1Char('0')).arg(t % 60, 2, 10, QLatin1Char('0'));
    call(QStringLiteral("Seek"), QStringLiteral("<Unit>REL_TIME</Unit><Target>%1</Target>").arg(target));
}

void DlnaRenderer::stop()
{
    m_poll.stop();
    call(QStringLiteral("Stop"), QString());
}

void DlnaRenderer::stopAndDelete()
{
    m_poll.stop();
    disconnect(this, nullptr, nullptr, nullptr);   // nobody listens any more
    call(QStringLiteral("Stop"), QString(), [this](bool, const QByteArray &) { deleteLater(); });
    QTimer::singleShot(10000, this, &QObject::deleteLater);   // a TV that never answers
}

void DlnaRenderer::pollState()
{
    call(QStringLiteral("GetTransportInfo"), QString(), [this](bool ok, const QByteArray &body) {
        if (!ok) return;
        auto value = [&body](const QByteArray &tag) {
            const int at = body.indexOf("<" + tag + ">");
            if (at < 0) return QByteArray();
            const int from = at + tag.size() + 2;
            return body.mid(from, body.indexOf('<', from) - from);
        };
        const QByteArray state = value("CurrentTransportState");
        const QByteArray status = value("CurrentTransportStatus");
        if (QString::fromLatin1(state + '/' + status) != m_lastState) {
            m_lastState = QString::fromLatin1(state + '/' + status);
            qInfo().noquote() << "Cast: the TV is" << m_lastState;
        }
        if (status == "ERROR_OCCURRED") {
            m_poll.stop();
            emit failed(m_started ? tr("The TV stopped with an error") : tr("The TV couldn't play this"));
            return;
        }
        if (state == "PLAYING" && !m_started) {
            m_started = true;
            if (m_seekOnStart > 5) { seek(m_seekOnStart); m_seekOnStart = 0; }
            emit playing();
        }
        else if (state == "STOPPED" || state == "NO_MEDIA_PRESENT") {
            if (m_started) { m_poll.stop(); emit stopped(); return; }
            // Never started: the TV gave up (Samsung: "An unexpected error has
            // occurred") and won't say more.
            if (++m_stoppedPolls >= 3) {
                m_poll.stop();
                emit failed(tr("The TV couldn't play this"));
            }
        }
    });
}

// ---- Chromecast --------------------------------------------------------------------------------------

namespace {
void varint(QByteArray &out, quint64 v)
{
    do {
        uchar b = v & 0x7F;
        v >>= 7;
        if (v) b |= 0x80;
        out += char(b);
    } while (v);
}
void field(QByteArray &out, int number, const QByteArray &bytes)
{
    varint(out, quint64(number << 3 | 2));
    varint(out, quint64(bytes.size()));
    out += bytes;
}
} // namespace

QByteArray ChromecastClient::frame(const QString &source, const QString &destination, const QString &ns, const QByteArray &json)
{
    QByteArray m;
    varint(m, 1 << 3 | 0); varint(m, 0);                  // protocol_version CASTV2_1_0
    field(m, 2, source.toUtf8());
    field(m, 3, destination.toUtf8());
    field(m, 4, ns.toUtf8());
    varint(m, 5 << 3 | 0); varint(m, 0);                  // payload_type STRING
    field(m, 6, json);
    QByteArray out(4, '\0');
    qToBigEndian<quint32>(quint32(m.size()), out.data());
    return out + m;
}

QList<ChromecastClient::Message> ChromecastClient::unframe(QByteArray &buffer)
{
    QList<Message> out;
    while (buffer.size() >= 4) {
        const quint32 len = qFromBigEndian<quint32>(buffer.constData());
        if (len > 1024 * 1024) { buffer.clear(); break; }
        if (buffer.size() < int(4 + len)) break;
        const QByteArray m = buffer.mid(4, len);
        buffer.remove(0, 4 + len);
        Message msg;
        int pos = 0;
        auto readVarint = [&m, &pos]() -> quint64 {
            quint64 v = 0;
            for (int shift = 0; pos < m.size() && shift < 64; shift += 7) {
                const uchar b = uchar(m[pos++]);
                v |= quint64(b & 0x7F) << shift;
                if (!(b & 0x80)) break;
            }
            return v;
        };
        while (pos < m.size()) {
            const quint64 key = readVarint();
            const int number = int(key >> 3), wire = int(key & 7);
            if (wire == 0) { readVarint(); continue; }
            if (wire != 2) break;
            const int len2 = int(readVarint());
            const QByteArray bytes = m.mid(pos, len2);
            pos += len2;
            switch (number) {
            case 2: msg.source = QString::fromUtf8(bytes); break;
            case 3: msg.destination = QString::fromUtf8(bytes); break;
            case 4: msg.ns = QString::fromUtf8(bytes); break;
            case 6: msg.payload = bytes; break;
            default: break;
            }
        }
        out.append(msg);
    }
    return out;
}

ChromecastClient::ChromecastClient(const CastDevice &device, QObject *parent)
    : QObject(parent), m_device(device), m_socket(new QSslSocket(this))
{
    // Chromecasts present a certificate of their own; there's no name to check it against.
    m_socket->setPeerVerifyMode(QSslSocket::VerifyNone);
    connect(m_socket, &QSslSocket::encrypted, this, [this] {
        connectTo(QStringLiteral("receiver-0"));
        send(QStringLiteral("receiver-0"), QLatin1String(kNsReceiver),
             {{"type", "LAUNCH"}, {"appId", QLatin1String(kMediaReceiver)}, {"requestId", m_requestId++}});
        m_heartbeat.start();
    });
    connect(m_socket, &QSslSocket::readyRead, this, [this] {
        m_buffer.append(m_socket->readAll());
        const auto messages = unframe(m_buffer);
        if (!messages.isEmpty()) m_answered = true;
        for (const Message &m : messages) onMessage(m);
    });
    connect(m_socket, &QSslSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        const auto error = m_socket->error();
        if (error == QAbstractSocket::RemoteHostClosedError && m_sawPlaying) { emit stopped(); return; }
        qWarning().noquote() << "Cast: Chromecast connection:" << m_socket->errorString() << (m_answered ? "(after it answered)" : "(before any answer)");
        if (!m_answered) {
            // Something at that address, but not speaking Cast: a Fire TV, a Roku, a computer…
            if (error == QAbstractSocket::RemoteHostClosedError || error == QAbstractSocket::ConnectionRefusedError
                    || error == QAbstractSocket::SslHandshakeFailedError) {
                emit failed(tr("That device doesn't answer like a Chromecast. Fire TV and Roku devices don't support "
                               "Chromecast casting (a Fire TV can, with a receiver app such as AirScreen)."));
                return;
            }
            if (error == QAbstractSocket::SocketTimeoutError || error == QAbstractSocket::HostNotFoundError
                    || error == QAbstractSocket::NetworkError) {
                emit failed(tr("Nothing answered at %1. Check the address, and that it's on and on this network.")
                                .arg(m_device.address.toString()));
                return;
            }
        }
        emit failed(tr("Couldn't talk to the Chromecast: %1").arg(m_socket->errorString()));
    });
    m_heartbeat.setInterval(5000);
    connect(&m_heartbeat, &QTimer::timeout, this, [this] {
        send(QStringLiteral("receiver-0"), QLatin1String(kNsHeartbeat), {{"type", "PING"}});
    });
}

ChromecastClient::~ChromecastClient()
{
    if (m_socket->state() == QAbstractSocket::ConnectedState) m_socket->disconnectFromHost();
}

void ChromecastClient::send(const QString &destination, const QString &ns, const QJsonObject &payload)
{
    if (m_socket->state() != QAbstractSocket::ConnectedState) return;
    m_socket->write(frame(QStringLiteral("sender-relay"), destination, ns, QJsonDocument(payload).toJson(QJsonDocument::Compact)));
}

void ChromecastClient::connectTo(const QString &destination)
{
    send(destination, QLatin1String(kNsConnection), {{"type", "CONNECT"}, {"origin", QJsonObject()}});
}

void ChromecastClient::load(const QString &url, const QString &title, const QString &contentType, bool live, double startSecs)
{
    m_pendingLoad = QJsonObject{
        {"type", "LOAD"},
        {"autoplay", true},
        {"currentTime", startSecs},
        {"media", QJsonObject{{"contentId", url}, {"contentUrl", url}, {"contentType", contentType},
                              {"streamType", live ? "LIVE" : "BUFFERED"},
                              {"metadata", QJsonObject{{"metadataType", 0}, {"title", title}}}}},
    };
    m_sawPlaying = false;
    if (!m_transport.isEmpty()) {
        QJsonObject l = m_pendingLoad;
        l.insert(QStringLiteral("requestId"), m_requestId++);
        send(m_transport, QLatin1String(kNsMedia), l);
        return;
    }
    if (m_socket->state() == QAbstractSocket::UnconnectedState)
        m_socket->connectToHostEncrypted(m_device.address.toString(), m_device.port ? m_device.port : 8009);
}

void ChromecastClient::onMessage(const Message &m)
{
    const QJsonObject o = QJsonDocument::fromJson(m.payload).object();
    const QString type = o.value(QLatin1String("type")).toString();
    if (m.ns == QLatin1String(kNsHeartbeat)) {
        if (type == QLatin1String("PING")) send(m.source, QLatin1String(kNsHeartbeat), {{"type", "PONG"}});
        return;
    }
    if (m.ns == QLatin1String(kNsConnection) && type == QLatin1String("CLOSE")) {
        if (m.source == m_transport) { m_transport.clear(); emit stopped(); }
        return;
    }
    if (m.ns == QLatin1String(kNsReceiver) && type == QLatin1String("RECEIVER_STATUS")) {
        const QJsonArray apps = o.value(QLatin1String("status")).toObject().value(QLatin1String("applications")).toArray();
        QString transport, session;
        for (const QJsonValue &a : apps)
            if (a.toObject().value(QLatin1String("appId")).toString() == QLatin1String(kMediaReceiver)) {
                transport = a.toObject().value(QLatin1String("transportId")).toString();
                session = a.toObject().value(QLatin1String("sessionId")).toString();
            }
        if (transport.isEmpty()) {
            if (!m_transport.isEmpty()) { m_transport.clear(); emit stopped(); }   // someone else took over
            return;
        }
        if (transport == m_transport) return;
        m_transport = transport;
        m_session = session;
        connectTo(m_transport);
        if (!m_pendingLoad.isEmpty()) {
            QJsonObject l = m_pendingLoad;
            l.insert(QStringLiteral("requestId"), m_requestId++);
            send(m_transport, QLatin1String(kNsMedia), l);
        }
        return;
    }
    if (m.ns == QLatin1String(kNsReceiver) && type == QLatin1String("LAUNCH_ERROR")) {
        emit failed(tr("The Chromecast couldn't start its player"));
        return;
    }
    if (m.ns == QLatin1String(kNsMedia)) {
        if (type == QLatin1String("LOAD_FAILED") || type == QLatin1String("LOAD_CANCELLED") || type == QLatin1String("INVALID_REQUEST")) {
            qWarning().noquote() << "Cast: the Chromecast said" << type << QString::fromUtf8(m.payload).left(200);
            emit failed(tr("The Chromecast couldn't play this stream"));
            return;
        }
        if (type != QLatin1String("MEDIA_STATUS")) return;
        for (const QJsonValue &v : o.value(QLatin1String("status")).toArray()) {
            const QJsonObject s = v.toObject();
            m_mediaSession = s.value(QLatin1String("mediaSessionId")).toInt(m_mediaSession);
            const QString state = s.value(QLatin1String("playerState")).toString();
            const QString reason = s.value(QLatin1String("idleReason")).toString();
            const QString said = reason.isEmpty() ? state : state + QLatin1Char(' ') + reason;
            if (!state.isEmpty() && said != m_lastState) {
                m_lastState = said;
                qInfo().noquote() << "Cast: the Chromecast is" << said.toLower();
            }
            if (state == QLatin1String("PLAYING") && !m_sawPlaying) { m_sawPlaying = true; emit playing(); }
            if (state == QLatin1String("IDLE") && m_sawPlaying) {
                const bool live = m_pendingLoad.value(QLatin1String("media")).toObject().value(QLatin1String("streamType")).toString() == QLatin1String("LIVE");
                m_sawPlaying = false;
                if (reason == QLatin1String("ERROR")) emit failed(tr("The Chromecast stopped with an error"));
                // A live channel doesn't finish: the stream stalled.
                else if (live && reason == QLatin1String("FINISHED")) emit failed(tr("The channel stopped on the TV"));
                else emit stopped();
                continue;
            }
            if (state == QLatin1String("IDLE") && !m_sawPlaying && s.value(QLatin1String("idleReason")).toString() == QLatin1String("ERROR"))
                emit failed(tr("The Chromecast couldn't play this stream"));
        }
    }
}

void ChromecastClient::play()
{
    if (m_mediaSession >= 0)
        send(m_transport, QLatin1String(kNsMedia), {{"type", "PLAY"}, {"mediaSessionId", m_mediaSession}, {"requestId", m_requestId++}});
}

void ChromecastClient::pause()
{
    if (m_mediaSession >= 0)
        send(m_transport, QLatin1String(kNsMedia), {{"type", "PAUSE"}, {"mediaSessionId", m_mediaSession}, {"requestId", m_requestId++}});
}

void ChromecastClient::stopAndDelete()
{
    disconnect(this, nullptr, nullptr, nullptr);
    if (m_socket->state() != QAbstractSocket::ConnectedState) { deleteLater(); return; }
    connect(m_socket, &QSslSocket::disconnected, this, &QObject::deleteLater);
    QTimer::singleShot(5000, this, &QObject::deleteLater);
    stop();
}

void ChromecastClient::stop()
{
    m_pendingLoad = QJsonObject();
    if (!m_session.isEmpty())
        send(QStringLiteral("receiver-0"), QLatin1String(kNsReceiver), {{"type", "STOP"}, {"sessionId", m_session}, {"requestId", m_requestId++}});
    m_transport.clear();
    m_session.clear();
    m_heartbeat.stop();
    m_socket->flush();
    m_socket->disconnectFromHost();
}
