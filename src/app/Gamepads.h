#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QStringList>
#include <QTimer>

class RemoteInput;

// Game controllers (Xbox, PlayStation, Switch and others, wired or Bluetooth)
// through SDL, sent on as Relay's remote actions (QML: Gamepads).
// Built without SDL, it's here but never finds a controller.
class Gamepads : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QStringList connected READ connected NOTIFY connectedChanged)
    Q_PROPERTY(bool available READ available CONSTANT)   // built with controller support

public:
    explicit Gamepads(RemoteInput *remote, QObject *parent = nullptr);
    ~Gamepads() override;

    QStringList connected() const { return m_names.values(); }
    bool available() const;

    // SDL's button numbers (SDL_GameControllerButton) and axes
    // (SDL_GameControllerAxis) -> Relay's actions; "" for unused.
    static QString actionForButton(int button);
    // A stick or trigger past the line: the action, and which way ("" in the middle).
    static QString actionForAxis(int axis, int value);

    // Tests: act even when Relay isn't the app in front.
    void setRequireActive(bool on) { m_requireActive = on; }

signals:
    void connectedChanged();

private:
    void poll();
    void press(const QString &action);
    void release(const QString &action);

    RemoteInput *m_remote;
    QTimer m_poll;
    QHash<int, QString> m_names;          // instance id -> controller name
    QHash<int, void *> m_open;            // instance id -> SDL_GameController*
    QHash<QString, QString> m_axisHeld;
    // Holding a direction repeats it, like a held key.
    QString m_held;
    QElapsedTimer m_heldFor;
    qint64 m_nextRepeat = 0;
    bool m_started = false;
    bool m_requireActive = true;
};
