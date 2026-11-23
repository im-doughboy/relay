#pragma once

#include <QObject>
#include <QStringList>
#include <QVariantMap>

// One way in for remotes, game controllers and the phone remote (QML: Remote).
//
// Actions are names: "up", "down", "left", "right", "ok", "back",
// "playpause", "stop", "next", "previous", "rewind", "forward",
// "chup", "chdown", "volup", "voldown", "mute", "pageup", "pagedown"
// become the key Relay already understands, sent to the window as if typed;
// anything else ("home", "menu", "guide", "info", "search", "favorites",
// "captions", "audio", "multiview", "cast", "movies", "settings") is
// emitted as action(name) for the QML side, which knows the user's keys.
//
// Installed on the application, it also turns the keys TV-style PC remotes
// send (Back, OK/Select, Home, Menu, Guide, Info…) into those actions.
class RemoteInput : public QObject
{
    Q_OBJECT

public:
    explicit RemoteInput(QObject *parent = nullptr);

    Q_INVOKABLE void trigger(const QString &action);
    // A key press and release to the window that has the keyboard.
    Q_INVOKABLE void sendKey(int key, int modifiers = 0);
    // The key an action becomes (0: it's an action(), not a key).
    static int keyFor(const QString &action);
    // The action a remote's own key means ("" for ordinary keys).
    static QString actionForRemoteKey(int key);
    static QStringList actions();

    // Typing from the phone: the text box that has the keyboard in Relay
    // (search, a playlist address…), if there is one.
    static QObject *focusedField();
    // {typing, text, hint, secret}: what the phone shows.
    static QVariantMap fieldState();
    // Puts this text in that box, as if typed; false without one.
    bool setFieldText(const QString &text);

signals:
    void action(const QString &name);
    void used(const QString &source);   // "remote", "controller", "phone" (Settings shows what's connected)

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
};
