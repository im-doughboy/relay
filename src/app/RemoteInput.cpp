#include "RemoteInput.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QHash>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QQuickItem>
#include <QQuickWindow>
#include <QWindow>

namespace {
const QHash<QString, int> &keys()
{
    static const QHash<QString, int> map{
        {QStringLiteral("up"), Qt::Key_Up},
        {QStringLiteral("down"), Qt::Key_Down},
        {QStringLiteral("left"), Qt::Key_Left},
        {QStringLiteral("right"), Qt::Key_Right},
        {QStringLiteral("ok"), Qt::Key_Return},
        {QStringLiteral("back"), Qt::Key_Escape},
        {QStringLiteral("playpause"), Qt::Key_MediaTogglePlayPause},
        {QStringLiteral("stop"), Qt::Key_MediaStop},
        {QStringLiteral("next"), Qt::Key_MediaNext},
        {QStringLiteral("previous"), Qt::Key_MediaPrevious},
        {QStringLiteral("rewind"), Qt::Key_AudioRewind},
        {QStringLiteral("forward"), Qt::Key_AudioForward},
        {QStringLiteral("chup"), Qt::Key_ChannelUp},
        {QStringLiteral("chdown"), Qt::Key_ChannelDown},
        {QStringLiteral("volup"), Qt::Key_VolumeUp},
        {QStringLiteral("voldown"), Qt::Key_VolumeDown},
        {QStringLiteral("mute"), Qt::Key_VolumeMute},
        {QStringLiteral("pageup"), Qt::Key_PageUp},
        {QStringLiteral("pagedown"), Qt::Key_PageDown},
    };
    return map;
}
} // namespace

RemoteInput::RemoteInput(QObject *parent) : QObject(parent) {}

int RemoteInput::keyFor(const QString &action) { return keys().value(action, 0); }

QStringList RemoteInput::actions()
{
    QStringList out = keys().keys();
    out << QStringLiteral("home") << QStringLiteral("menu") << QStringLiteral("guide") << QStringLiteral("info")
        << QStringLiteral("search") << QStringLiteral("favorites") << QStringLiteral("captions") << QStringLiteral("audio")
        << QStringLiteral("multiview") << QStringLiteral("cast") << QStringLiteral("movies") << QStringLiteral("settings");
    return out;
}

QString RemoteInput::actionForRemoteKey(int key)
{
    switch (key) {
    case Qt::Key_Back: return QStringLiteral("back");             // remotes' Back, the browser Back key
    case Qt::Key_Select: return QStringLiteral("ok");
    case Qt::Key_HomePage: return QStringLiteral("home");
    case Qt::Key_Menu: return QStringLiteral("menu");             // the Menu / Apps key
    case Qt::Key_Guide: return QStringLiteral("guide");
    case Qt::Key_Info: return QStringLiteral("info");
    case Qt::Key_Search: return QStringLiteral("search");
    case Qt::Key_Favorites: return QStringLiteral("favorites");
    case Qt::Key_Subtitle: return QStringLiteral("captions");
    case Qt::Key_AudioCycleTrack: return QStringLiteral("audio");
    case Qt::Key_Settings: return QStringLiteral("settings");
    default: return QString();
    }
}

void RemoteInput::sendKey(int key, int modifiers)
{
    QWindow *w = QGuiApplication::focusWindow();
    if (!w) {
        const auto windows = QGuiApplication::topLevelWindows();
        for (QWindow *candidate : windows) if (candidate->isVisible()) { w = candidate; break; }
    }
    if (!w) return;
    w->requestActivate();
    const auto mods = Qt::KeyboardModifiers(modifiers);
    QKeyEvent press(QEvent::KeyPress, key, mods);
    QKeyEvent release(QEvent::KeyRelease, key, mods);
    QCoreApplication::sendEvent(w, &press);
    QCoreApplication::sendEvent(w, &release);
}

void RemoteInput::trigger(const QString &name)
{
    emit triggered(name);
    if (const int key = keyFor(name)) { sendKey(key); return; }
    if (!name.isEmpty()) emit action(name);
}

bool RemoteInput::eventFilter(QObject *watched, QEvent *event)
{
    // The mouse's back button (thumb button): back, as on a remote.
    if (event->type() == QEvent::MouseButtonPress && watched->isWindowType()
            && static_cast<QMouseEvent *>(event)->button() == Qt::BackButton) {
        trigger(QStringLiteral("back"));
        return true;
    }
    // Only what reaches windows, once (not again for each item it passes).
    if ((event->type() == QEvent::KeyPress || event->type() == QEvent::KeyRelease) && watched->isWindowType()) {
        auto *k = static_cast<QKeyEvent *>(event);
        const QString name = actionForRemoteKey(k->key());
        if (!name.isEmpty()) {
            if (event->type() == QEvent::KeyPress) {
                emit used(QStringLiteral("remote"));
                trigger(name);
            }
            return true;
        }
    }
    return QObject::eventFilter(watched, event);
}

// ---- typing from the phone -----------------------------------------------------------------------

QObject *RemoteInput::focusedField()
{
    auto isField = [](QObject *o) {
        return o && (o->inherits("QQuickTextInput") || o->inherits("QQuickTextEdit")) && !o->property("readOnly").toBool();
    };
    // The window in front first, then any (the phone works while Relay is behind other windows).
    QList<QWindow *> windows;
    if (QWindow *w = QGuiApplication::focusWindow()) windows << w;
    for (QWindow *w : QGuiApplication::topLevelWindows()) if (!windows.contains(w) && w->isVisible()) windows << w;
    for (QWindow *w : std::as_const(windows)) {
        auto *qw = qobject_cast<QQuickWindow *>(w);
        if (!qw) continue;
        QQuickItem *item = qw->activeFocusItem();
        if (isField(item) && item->isVisible()) return item;
    }
    return nullptr;
}

QVariantMap RemoteInput::fieldState()
{
    QObject *f = focusedField();
    if (!f) return {{QStringLiteral("typing"), false}};
    const bool secret = f->property("echoMode").toInt() != 0;   // TextInput.Normal is 0
    QString hint = f->property("placeholderText").toString();
    if (hint.isEmpty()) hint = f->property("accessibleName").toString();
    return {{QStringLiteral("typing"), true},
            {QStringLiteral("text"), secret ? QString() : f->property("text").toString()},
            {QStringLiteral("hint"), hint},
            {QStringLiteral("secret"), secret}};
}

bool RemoteInput::setFieldText(const QString &textIn)
{
    QObject *f = focusedField();
    if (!f) return false;
    const QString text = textIn.left(500);
    f->setProperty("text", text);
    f->setProperty("cursorPosition", int(text.size()));
    emit used(QStringLiteral("phone"));
    return true;
}
