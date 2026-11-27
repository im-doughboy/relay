#include "PhoneRemote.h"

#include "RemoteInput.h"
#include "third_party/qrcodegen/qrcodegen.hpp"

#include <QCryptographicHash>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkInterface>
#include <QRandomGenerator>
#include <QSettings>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QtDebug>

namespace {
constexpr quint16 kFirstPort = 8800;
constexpr int kPorts = 10;
constexpr int kMaxWrongCodes = 5;

QByteArray hashOf(const QByteArray &token)
{
    return QCryptographicHash::hash(token, QCryptographicHash::Sha256).toHex();
}
} // namespace

PhoneRemote::PhoneRemote(RemoteInput *remote, QObject *parent) : QObject(parent), m_remote(remote)
{
    if (code().isEmpty()) newCode();
    if (enabled()) start();
}

bool PhoneRemote::enabled() const { return QSettings().value(QStringLiteral("phone/enabled"), false).toBool(); }

void PhoneRemote::setEnabled(bool on)
{
    if (on == enabled() && wanted() == running()) return;
    QSettings().setValue(QStringLiteral("phone/enabled"), on);
    if (wanted()) start(); else stop();
    emit changed();
}

void PhoneRemote::setSetup(bool on)
{
    if (on == m_setup) return;
    m_setup = on;
    if (!m_setupTimer) {
        m_setupTimer = new QTimer(this);
        m_setupTimer->setInterval(m_setupLifetimeMs);
        connect(m_setupTimer, &QTimer::timeout, this, [this] { newSetupKey(); emit changed(); });
    }
    if (on) {
        newSetupKey();
        qInfo().noquote() << "Phone remote: setup from the phone is on";
        start();
    } else {
        m_setupTimer->stop();
        m_setupKey.clear();
        qInfo().noquote() << "Phone remote: setup from the phone is off";
        if (!wanted()) stop();
    }
    emit changed();
}

void PhoneRemote::setSetupKeyLifetime(int ms)
{
    m_setupLifetimeMs = ms;
    if (m_setupTimer) m_setupTimer->setInterval(ms);
}

void PhoneRemote::newSetupKey()
{
    QByteArray fresh(12, Qt::Uninitialized);
    QRandomGenerator::system()->fillRange(reinterpret_cast<quint32 *>(fresh.data()), fresh.size() / 4);
    m_setupKey = fresh.toHex();
    if (m_setupTimer) m_setupTimer->start();   // 15 minutes from now
}

// The key goes after # : a browser never sends that part to a server.
QString PhoneRemote::setupUrl() const
{
    const QString base = url();
    return m_setup && !base.isEmpty() && !m_setupKey.isEmpty() ? base + QStringLiteral("/#k=") + QString::fromLatin1(m_setupKey) : QString();
}

bool PhoneRemote::running() const { return m_server && m_server->isListening(); }
quint16 PhoneRemote::port() const { return running() ? m_server->serverPort() : 0; }

QStringList PhoneRemote::urls() const
{
    QStringList out;
    if (!running()) return out;
    for (const QNetworkInterface &iface : QNetworkInterface::allInterfaces()) {
        const auto flags = iface.flags();
        if (!(flags & QNetworkInterface::IsUp) || !(flags & QNetworkInterface::IsRunning) || (flags & QNetworkInterface::IsLoopBack)) continue;
        for (const QNetworkAddressEntry &e : iface.addressEntries()) {
            const QHostAddress a = e.ip();
            if (a.protocol() == QAbstractSocket::IPv4Protocol && isLocal(a) && !a.isLoopback())
                out << QStringLiteral("http://%1:%2").arg(a.toString()).arg(port());
        }
    }
    // Home networks first (192.168.x, then 10.x / 172.16-31.x).
    std::stable_sort(out.begin(), out.end(), [](const QString &a, const QString &b) {
        return a.contains(QLatin1String("//192.168.")) && !b.contains(QLatin1String("//192.168."));
    });
    return out;
}

QString PhoneRemote::url() const { return urls().value(0); }
QString PhoneRemote::code() const { return QSettings().value(QStringLiteral("phone/code")).toString(); }
int PhoneRemote::paired() const { return QSettings().value(QStringLiteral("phone/paired")).toStringList().size(); }

void PhoneRemote::setNowShowing(const QString &text)
{
    if (text == m_nowShowing) return;
    m_nowShowing = text;
    emit nowShowingChanged();
}

void PhoneRemote::newCode()
{
    QSettings().setValue(QStringLiteral("phone/code"),
                         QStringLiteral("%1").arg(QRandomGenerator::system()->bounded(10000), 4, 10, QLatin1Char('0')));
}

void PhoneRemote::forgetPhones()
{
    QSettings().remove(QStringLiteral("phone/paired"));
    newCode();
    qInfo().noquote() << "Phone remote: forgot the paired phones";
    emit changed();
}

bool PhoneRemote::isLocal(const QHostAddress &peerIn)
{
    bool ok = false;
    const quint32 v4 = peerIn.toIPv4Address(&ok);   // also IPv4 written as IPv6 (::ffff:a.b.c.d)
    if (ok) {
        const QHostAddress peer(v4);
        return peer.isLoopback() || peer.isInSubnet(QHostAddress(QStringLiteral("10.0.0.0")), 8)
            || peer.isInSubnet(QHostAddress(QStringLiteral("172.16.0.0")), 12)
            || peer.isInSubnet(QHostAddress(QStringLiteral("192.168.0.0")), 16)
            || peer.isInSubnet(QHostAddress(QStringLiteral("169.254.0.0")), 16);
    }
    return peerIn.isLoopback() || peerIn.isLinkLocal() || peerIn.isUniqueLocalUnicast();
}

// A phone that isn't paired (or was forgotten in Settings): it has to pair again.
void PhoneRemote::refused(QTcpSocket *socket, const QByteArray &token)
{
    noteOnce(QStringLiteral("unpaired ") + socket->peerAddress().toString(),
             QStringLiteral("Phone remote: a phone at %1 isn't paired%2 · it shows the code screen again")
                 .arg(socket->peerAddress().toString(), token.isEmpty() ? QString() : QStringLiteral(" (forgotten, or paired with another computer)")));
    reply(socket, 403, "text/plain", "Pair this phone first.");
}

bool PhoneRemote::tokenValid(const QByteArray &token) const
{
    if (token.size() < 16) return false;
    return QSettings().value(QStringLiteral("phone/paired")).toStringList().contains(QString::fromLatin1(hashOf(token)));
}

QVariantMap PhoneRemote::qr(const QString &text)
{
    const auto code = qrcodegen::QrCode::encodeText(text.toUtf8().constData(), qrcodegen::QrCode::Ecc::MEDIUM);
    QString modules;
    modules.reserve(code.getSize() * code.getSize());
    for (int y = 0; y < code.getSize(); ++y)
        for (int x = 0; x < code.getSize(); ++x) modules += code.getModule(x, y) ? QLatin1Char('1') : QLatin1Char('0');
    return {{QStringLiteral("size"), code.getSize()}, {QStringLiteral("modules"), modules}};
}

// ---- the server --------------------------------------------------------------------------------

void PhoneRemote::start()
{
    if (running()) return;
    if (!m_server) {
        m_server = new QTcpServer(this);
        connect(m_server, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *socket = m_server->nextPendingConnection()) {
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                if (!isLocal(socket->peerAddress())) {   // home network only
                    noteOnce(QStringLiteral("refused ") + socket->peerAddress().toString(),
                             QStringLiteral("Phone remote: turned away %1 (not an address on a home network)").arg(socket->peerAddress().toString()));
                    socket->abort();
                    continue;
                }
                auto buffer = std::make_shared<QByteArray>();
                connect(socket, &QTcpSocket::readyRead, this, [this, socket, buffer] {
                    buffer->append(socket->readAll());
                    if (buffer->size() > 64 * 1024) { socket->abort(); return; }
                    const int end = buffer->indexOf("\r\n\r\n");
                    if (end < 0) return;
                    const QList<QByteArray> lines = buffer->left(end).split('\n');
                    const QList<QByteArray> first = lines.value(0).trimmed().split(' ');
                    QHash<QByteArray, QByteArray> headers;
                    for (int i = 1; i < lines.size(); ++i) {
                        const int colon = lines[i].indexOf(':');
                        if (colon > 0) headers.insert(lines[i].left(colon).trimmed().toLower(), lines[i].mid(colon + 1).trimmed());
                    }
                    const int length = headers.value("content-length").toInt();
                    if (buffer->size() - end - 4 < length) return;   // the body isn't all here yet
                    const QByteArray body = buffer->mid(end + 4, length);
                    disconnect(socket, &QTcpSocket::readyRead, this, nullptr);
                    onRequest(socket, first.value(0), QString::fromUtf8(first.value(1)), headers, body);
                });
            }
        });
    }
    // The same address every time (the phone keeps it): 8800, waiting a few
    // seconds if it's still taken (Relay closing after an update), and only
    // then the next free port.
    if (m_server->listen(QHostAddress::Any, kFirstPort)) { announce(); return; }
    m_portTries = 0;
    auto *retry = new QTimer(this);
    retry->setInterval(500);
    connect(retry, &QTimer::timeout, this, [this, retry] {
        if (running() || !wanted()) { retry->deleteLater(); return; }
        if (m_server->listen(QHostAddress::Any, kFirstPort) || ++m_portTries >= 16) {
            retry->deleteLater();
            if (!running()) {
                qWarning().noquote() << QStringLiteral("Phone remote: port %1 is in use by another program; using the next free one (the phone needs the new address)").arg(kFirstPort);
                for (int i = 1; i < kPorts; ++i)
                    if (m_server->listen(QHostAddress::Any, quint16(kFirstPort + i))) break;
            }
            announce();
        }
    });
    retry->start();
}

void PhoneRemote::announce()
{
    if (running()) qInfo().noquote() << "Phone remote: on, at" << (url().isEmpty() ? QStringLiteral("port %1").arg(port()) : url());
    else qWarning().noquote() << "Phone remote: couldn't open a port:" << m_server->errorString();
    emit changed();
}

void PhoneRemote::noteOnce(const QString &key, const QString &line)
{
    if (m_noted.contains(key)) return;
    if (m_noted.size() > 50) m_noted.clear();
    m_noted.insert(key);
    qWarning().noquote() << line;
}

void PhoneRemote::stop()
{
    if (m_server) m_server->close();
    qInfo().noquote() << "Phone remote: off";
}

void PhoneRemote::reply(QTcpSocket *socket, int status, const QByteArray &type, const QByteArray &body)
{
    const QByteArray text = status == 200 ? "OK" : status == 204 ? "No Content" : status == 403 ? "Forbidden"
                          : status == 429 ? "Too Many Requests" : status == 400 ? "Bad Request" : status == 409 ? "Conflict" : "Not Found";
    QByteArray h = "HTTP/1.1 " + QByteArray::number(status) + ' ' + text + "\r\n";
    if (!type.isEmpty()) h += "Content-Type: " + type + "\r\n";
    h += "Content-Length: " + QByteArray::number(body.size()) + "\r\n"
         "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nConnection: close\r\n\r\n";
    socket->write(h + body);
    socket->disconnectFromHost();
}

void PhoneRemote::onRequest(QTcpSocket *socket, const QByteArray &method, const QString &path,
                            const QHash<QByteArray, QByteArray> &headers, const QByteArray &body)
{
    const QByteArray token = headers.value("x-relay-token");
    if (method == "GET" && (path == QLatin1String("/") || path.startsWith(QLatin1String("/?")))) {
        QFile page(QStringLiteral(":/web/remote.html"));
        page.open(QIODevice::ReadOnly);
        reply(socket, 200, "text/html; charset=utf-8", page.readAll());
        return;
    }
    if (method == "GET" && path == QLatin1String("/state")) {
        const bool ok = tokenValid(token);
        QJsonObject o{{QStringLiteral("paired"), ok}};
        if (ok) {
            o.insert(QStringLiteral("now"), m_nowShowing);
            o.insert(QStringLiteral("field"), QJsonObject::fromVariantMap(RemoteInput::fieldState()));
        }
        reply(socket, 200, "application/json", QJsonDocument(o).toJson(QJsonDocument::Compact));
        return;
    }
    if (method == "POST" && path == QLatin1String("/pair")) {
        const QDateTime now = QDateTime::currentDateTimeUtc();
        if (m_lockedUntil.isValid() && now < m_lockedUntil) { reply(socket, 429, "text/plain", "Too many wrong codes. Wait a minute."); return; }
        const QJsonObject asked = QJsonDocument::fromJson(body).object();
        const QString given = asked.value(QLatin1String("code")).toString().trimmed();
        const QByteArray key = asked.value(QLatin1String("key")).toString().toLatin1();
        // The QR code's one-time key (setup), or the code Relay shows.
        const bool byKey = !key.isEmpty() && !m_setupKey.isEmpty() && key == m_setupKey;
        if (!byKey && (given.isEmpty() || given != code())) {
            if (++m_wrongCodes >= kMaxWrongCodes) {
                // Too many guesses: wait, and a new code.
                m_wrongCodes = 0;
                m_lockedUntil = now.addSecs(60);
                newCode();
                emit changed();
                qWarning().noquote() << "Phone remote: too many wrong codes from" << socket->peerAddress().toString() << "· new code";
            }
            reply(socket, 403, "text/plain", "That code isn't right.");
            return;
        }
        m_wrongCodes = 0;
        if (byKey) newSetupKey();   // once only: the next phone gets a new QR code
        QByteArray fresh(24, Qt::Uninitialized);
        QRandomGenerator::system()->fillRange(reinterpret_cast<quint32 *>(fresh.data()), fresh.size() / 4);
        const QByteArray newToken = fresh.toHex();
        QSettings s;
        QStringList pairedList = s.value(QStringLiteral("phone/paired")).toStringList();
        pairedList << QString::fromLatin1(hashOf(newToken));
        s.setValue(QStringLiteral("phone/paired"), pairedList);
        qInfo().noquote() << "Phone remote: paired a phone at" << socket->peerAddress().toString() << (byKey ? "(setup QR code)" : "");
        emit changed();
        emit phonePaired();
        reply(socket, 200, "application/json", QJsonDocument(QJsonObject{{QStringLiteral("token"), QString::fromLatin1(newToken)}}).toJson(QJsonDocument::Compact));
        return;
    }
    if (method == "POST" && path.startsWith(QLatin1String("/a/"))) {
        if (!tokenValid(token)) { refused(socket, token); return; }
        const QString action = path.mid(3);
        if (!RemoteInput::actions().contains(action)) { reply(socket, 400, "text/plain", "Unknown button."); return; }
        emit m_remote->used(QStringLiteral("phone"));
        m_remote->trigger(action);
        reply(socket, 204, QByteArray(), QByteArray());
        return;
    }
    if (method == "POST" && path == QLatin1String("/text")) {
        if (!tokenValid(token)) { refused(socket, token); return; }
        const QJsonValue text = QJsonDocument::fromJson(body).object().value(QLatin1String("text"));
        if (!text.isString()) { reply(socket, 400, "text/plain", "No text."); return; }
        if (!m_remote->setFieldText(text.toString())) { reply(socket, 409, "text/plain", "Nothing to type into."); return; }
        reply(socket, 204, QByteArray(), QByteArray());
        return;
    }
    reply(socket, 404, "text/plain", "Not here.");
}
