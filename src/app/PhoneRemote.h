#pragma once

#include <QDateTime>
#include <QHash>
#include <QHostAddress>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QVariantMap>

class QTcpServer;
class QTcpSocket;
class RemoteInput;

// Phone as a remote (QML: Phone). When it's on, Relay serves a remote page on
// the home network: http://<this computer>:8800. A phone pairs once with the
// four-digit code Relay shows, then sends Relay's remote actions.
//
// Only devices on the local network are answered, and only paired phones can
// press anything. Paired phones are remembered (as hashes of their tokens).
class PhoneRemote : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY changed)
    Q_PROPERTY(bool running READ running NOTIFY changed)
    Q_PROPERTY(QString url READ url NOTIFY changed)              // the address to open on the phone
    Q_PROPERTY(QStringList urls READ urls NOTIFY changed)        // every address this computer has
    Q_PROPERTY(QString code READ code NOTIFY changed)            // the pairing code
    Q_PROPERTY(int paired READ paired NOTIFY changed)
    Q_PROPERTY(QString nowShowing READ nowShowing WRITE setNowShowing NOTIFY nowShowingChanged)

public:
    explicit PhoneRemote(RemoteInput *remote, QObject *parent = nullptr);

    bool enabled() const;
    void setEnabled(bool on);
    bool running() const;
    quint16 port() const;
    QString url() const;
    QStringList urls() const;
    QString code() const;
    int paired() const;
    QString nowShowing() const { return m_nowShowing; }
    void setNowShowing(const QString &text);

    Q_INVOKABLE void forgetPhones();
    // A QR code for `text`: {size, modules} with modules a string of '1'/'0', row by row.
    Q_INVOKABLE static QVariantMap qr(const QString &text);
    // Home networks only (private, link-local, loopback).
    static bool isLocal(const QHostAddress &peer);

signals:
    void changed();
    void nowShowingChanged();
    void phonePaired();

private:
    void start();
    void stop();
    void onRequest(QTcpSocket *socket, const QByteArray &method, const QString &path,
                   const QHash<QByteArray, QByteArray> &headers, const QByteArray &body);
    void reply(QTcpSocket *socket, int status, const QByteArray &type, const QByteArray &body);
    bool tokenValid(const QByteArray &token) const;
    void refused(QTcpSocket *socket, const QByteArray &token);
    void announce();
    void noteOnce(const QString &key, const QString &line);   // a problem, in the log once per phone
    QSet<QString> m_noted;
    int m_portTries = 0;
    void newCode();

    RemoteInput *m_remote;
    QTcpServer *m_server = nullptr;
    QString m_nowShowing;
    int m_wrongCodes = 0;
    QDateTime m_lockedUntil;
};
