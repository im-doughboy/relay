#pragma once

#include "CastDevices.h"
#include "CastRelay.h"

#include <QObject>
#include <QPointer>
#include <QVariantList>

// Casting to a Chromecast or a smart TV (QML: Cast). Relay finds the
// devices, serves the stream to them through its relay (CastRelay), and
// controls playback there.
class CastService : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantList devices READ devices NOTIFY devicesChanged)
    Q_PROPERTY(bool scanning READ scanning NOTIFY devicesChanged)
    Q_PROPERTY(bool active READ active NOTIFY stateChanged)          // casting (or starting to)
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)         // "", connecting, playing, paused
    Q_PROPERTY(QString deviceId READ deviceId NOTIFY stateChanged)
    Q_PROPERTY(QString deviceName READ deviceName NOTIFY stateChanged)
    Q_PROPERTY(QString title READ title NOTIFY stateChanged)
    Q_PROPERTY(bool live READ live NOTIFY stateChanged)

public:
    explicit CastService(QObject *parent = nullptr);

    QVariantList devices() const;
    bool scanning() const { return m_scanTimer.isActive(); }
    bool active() const { return !m_state.isEmpty(); }
    QString state() const { return m_state; }
    QString deviceId() const { return m_device.id; }
    QString deviceName() const { return m_device.name; }
    QString title() const { return m_title; }
    bool live() const { return m_live; }

    Q_INVOKABLE void scan();
    Q_INVOKABLE void addDevice(const QString &address);
    Q_INVOKABLE void forgetDevice(const QString &id);
    // source: {url, userAgent, referrer, title, live, start (seconds)} (Player.castSource()).
    Q_INVOKABLE void start(const QString &deviceId, const QVariantMap &source);
    Q_INVOKABLE void togglePause();
    Q_INVOKABLE void stop();

    // What a receiver is told the stream is, from its address.
    static QString mimeFor(const QString &url, bool live);

signals:
    void devicesChanged();
    void stateChanged();
    void message(const QString &text, bool problem);
    void ended(const QString &problem);   // casting stopped (by Relay, the TV, or an error: why)

private:
    void begin(const QString &url, const QVariantMap &source);
    // makeHls: a TV gets live MPEG-TS as HLS made here (the second try).
    // joinHls: an HLS channel sent to a TV as one MPEG-TS stream.
    void serve(const QString &url, const QVariantMap &source, const QString &mime, bool makeHls = false, bool joinHls = false);
    void retryAsHls();
    void setState(const QString &s);
    void finish(const QString &problem);

    CastDiscovery *m_discovery;
    CastRelay *m_relay;
    QTimer m_scanTimer;
    CastDevice m_device;
    QPointer<DlnaRenderer> m_dlna;
    QPointer<ChromecastClient> m_cc;
    QString m_state, m_title, m_path;
    bool m_live = false;
    QString m_castUrl;
    QVariantMap m_castSource;
    bool m_triedHls = false;
    int m_recoveries = 0;      // a live channel the Chromecast dropped, started again
    quint64 m_generation = 0;
};
