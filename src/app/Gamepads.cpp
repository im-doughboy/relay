#include "Gamepads.h"

#include "RemoteInput.h"

#include <QGuiApplication>
#include <QtDebug>

#ifdef RELAY_HAVE_SDL
# ifndef SDL_MAIN_HANDLED
#  define SDL_MAIN_HANDLED
# endif
# include <SDL.h>
#endif

namespace {
constexpr int kPollMs = 16;
constexpr int kRepeatDelayMs = 400;
constexpr int kRepeatEveryMs = 110;
constexpr int kStickLine = 20000;     // of 32767: past this is a press
constexpr int kTriggerLine = 16000;

bool repeats(const QString &action)
{
    return action == QLatin1String("up") || action == QLatin1String("down") || action == QLatin1String("left")
        || action == QLatin1String("right") || action == QLatin1String("volup") || action == QLatin1String("voldown")
        || action == QLatin1String("pageup") || action == QLatin1String("pagedown");
}
} // namespace

// SDL_GameControllerButton values (A is the bottom face button on every pad).
QString Gamepads::actionForButton(int button)
{
    switch (button) {
    case 0: return QStringLiteral("ok");          // A / Cross
    case 1: return QStringLiteral("back");        // B / Circle
    case 2: return QStringLiteral("menu");        // X / Square
    case 3: return QStringLiteral("guide");       // Y / Triangle
    case 4: return QStringLiteral("info");        // Back / View / Share
    case 5: return QStringLiteral("home");        // Guide: the Xbox / PS button
    case 6: return QStringLiteral("playpause");   // Start / Menu / Options
    case 7: return QStringLiteral("captions");    // left stick click
    case 8: return QStringLiteral("audio");       // right stick click
    case 9: return QStringLiteral("rewind");      // LB / L1
    case 10: return QStringLiteral("forward");    // RB / R1
    case 11: return QStringLiteral("up");         // D-pad
    case 12: return QStringLiteral("down");
    case 13: return QStringLiteral("left");
    case 14: return QStringLiteral("right");
    default: return QString();
    }
}

// SDL_GameControllerAxis: 0 left X, 1 left Y, 2 right X, 3 right Y, 4 LT, 5 RT.
QString Gamepads::actionForAxis(int axis, int value)
{
    switch (axis) {
    case 0: return value < -kStickLine ? QStringLiteral("left") : value > kStickLine ? QStringLiteral("right") : QString();
    case 1: return value < -kStickLine ? QStringLiteral("up") : value > kStickLine ? QStringLiteral("down") : QString();
    case 3: return value < -kStickLine ? QStringLiteral("volup") : value > kStickLine ? QStringLiteral("voldown") : QString();
    case 4: return value > kTriggerLine ? QStringLiteral("pageup") : QString();
    case 5: return value > kTriggerLine ? QStringLiteral("pagedown") : QString();
    default: return QString();
    }
}

Gamepads::Gamepads(RemoteInput *remote, QObject *parent) : QObject(parent), m_remote(remote)
{
#ifdef RELAY_HAVE_SDL
    // No SDL window: without this SDL thinks Relay is in the background and drops the events.
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    // SDL would take Ctrl+C / "please quit" for itself: Relay closes as usual.
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_GAMECONTROLLER) != 0) {
        qWarning().noquote() << "Controllers: SDL couldn't start:" << SDL_GetError();
        return;
    }
    m_started = true;
    m_poll.setInterval(kPollMs);
    connect(&m_poll, &QTimer::timeout, this, &Gamepads::poll);
    m_poll.start();
#endif
}

Gamepads::~Gamepads()
{
#ifdef RELAY_HAVE_SDL
    for (void *c : std::as_const(m_open)) SDL_GameControllerClose(static_cast<SDL_GameController *>(c));
    if (m_started) SDL_Quit();
#endif
}

bool Gamepads::available() const
{
#ifdef RELAY_HAVE_SDL
    return m_started;
#else
    return false;
#endif
}

void Gamepads::press(const QString &action)
{
    if (action.isEmpty()) return;
    // Only while Relay is the app in front (not while you're in a game).
    if (m_requireActive && QGuiApplication::applicationState() != Qt::ApplicationActive) return;
    emit m_remote->used(QStringLiteral("controller"));
    m_remote->trigger(action);
    if (repeats(action)) {
        m_held = action;
        m_heldFor.start();
        m_nextRepeat = kRepeatDelayMs;
    }
}

void Gamepads::release(const QString &action)
{
    if (action == m_held) m_held.clear();
}

void Gamepads::poll()
{
#ifdef RELAY_HAVE_SDL
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_CONTROLLERDEVICEADDED: {
            SDL_GameController *c = SDL_GameControllerOpen(e.cdevice.which);
            if (!c) break;
            const int id = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(c));
            const char *name = SDL_GameControllerName(c);
            m_open.insert(id, c);
            m_names.insert(id, QString::fromUtf8(name ? name : "Controller"));
            qInfo().noquote() << "Controllers: connected" << m_names.value(id);
            emit connectedChanged();
            break;
        }
        case SDL_CONTROLLERDEVICEREMOVED: {
            const int id = e.cdevice.which;
            if (void *c = m_open.take(id)) SDL_GameControllerClose(static_cast<SDL_GameController *>(c));
            qInfo().noquote() << "Controllers: disconnected" << m_names.take(id);
            emit connectedChanged();
            break;
        }
        case SDL_CONTROLLERBUTTONDOWN:
            press(actionForButton(e.cbutton.button));
            break;
        case SDL_CONTROLLERBUTTONUP:
            release(actionForButton(e.cbutton.button));
            break;
        case SDL_CONTROLLERAXISMOTION: {
            const QString key = QStringLiteral("%1:%2").arg(e.caxis.which).arg(e.caxis.axis);
            const QString now = actionForAxis(e.caxis.axis, e.caxis.value);
            const QString before = m_axisHeld.value(key);
            if (now == before) break;
            if (!before.isEmpty()) release(before);
            m_axisHeld.insert(key, now);
            press(now);
            break;
        }
        default:
            break;
        }
    }
    if (!m_held.isEmpty() && m_heldFor.elapsed() >= m_nextRepeat) {
        m_nextRepeat = m_heldFor.elapsed() + kRepeatEveryMs;
        if (!m_requireActive || QGuiApplication::applicationState() == Qt::ApplicationActive) m_remote->trigger(m_held);
    }
#endif
}
