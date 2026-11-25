#include "qsdlintegration.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_webOS.h>
#include <dlfcn.h>
#include <time.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QInputMethodQueryEvent>
#include <QOpenGLContext>
#include <QSet>
#include <QSurfaceFormat>
#include <QThread>
#include <QTimer>
#include <QWindow>
#include <QtGui/private/qgenericunixeventdispatcher_p.h>
#include <QtGui/private/qgenericunixfontdatabase_p.h>
#include <qpa/qplatforminputcontext.h>
#include <qpa/qplatformintegration.h>
#include <qpa/qplatformintegrationplugin.h>
#include <qpa/qplatformnativeinterface.h>
#include <qpa/qplatformoffscreensurface.h>
#include <qpa/qplatformopenglcontext.h>
#include <qpa/qplatformscreen.h>
#include <qpa/qplatformwindow.h>
#include <qpa/qwindowsysteminterface.h>

namespace {
SDL_Window *g_window = nullptr;
SDL_GLContext g_context = nullptr;
QSize g_size(1920, 1080);
bool g_onScreen = true;

QRect screenRect() { return QRect(QPoint(), g_size); }

// How responsive the screen is, for the log: how long a button press takes
// to show (press to the next frame on screen), how long frames take to draw,
// and how long the main thread was held up. All on the main thread.
struct Responsiveness {
    QElapsedTimer clock;
    qint64 keyAt = -1;        // a press waiting for its frame (ms)
    qint64 frameStart = -1;   // first makeCurrent of the frame being drawn (ms)
    int keys = 0, frames = 0, slowFrames = 0, stalls = 0;
    qint64 keySum = 0, keyMax = 0, frameMax = 0, stallMax = 0;
    qint64 since = 0;
    // Time between one frame on screen and the next, while things move
    // (under 200 ms): up to 20 ms is 60 frames a second, 20-40 ms is 30.
    int gaps60 = 0, gaps30 = 0, gapsSlower = 0;
    qint64 lastSwap = -1;
    // Handing a frame to the TV's compositor (SDL_GL_SwapWindow), which waits
    // when the compositor isn't ready; and press -> its frame starting.
    qint64 swapSum = 0, swapMax = 0, startSum = 0, startMax = 0;
    int swaps = 0, slowSwaps = 0, starts = 0;
    bool keyStartPending = false;
    qint64 now() { if (!clock.isValid()) clock.start(); return clock.elapsed(); }
    void reset(qint64 t)
    {
        keys = frames = slowFrames = stalls = gaps60 = gaps30 = gapsSlower = swaps = slowSwaps = starts = 0;
        keySum = keyMax = frameMax = stallMax = swapSum = swapMax = startSum = startMax = 0;
        since = t;
    }
} g_resp;

class QSdlScreen : public QPlatformScreen
{
public:
    QRect geometry() const override { return screenRect(); }
    int depth() const override { return 32; }
    QImage::Format format() const override { return QImage::Format_ARGB32_Premultiplied; }
    // Qt Quick asks for frames at this rate: half the panel's 120 Hz, so
    // movement stays even.
    qreal refreshRate() const override { return 60; }
};

// Qt's view of SDL's context. Making it current makes SDL's context current,
// and a finished frame of a shown window goes to the screen through SDL.
class QSdlGLContext : public QPlatformOpenGLContext
{
public:
    explicit QSdlGLContext(const QSurfaceFormat &requested) : m_format(requested)
    {
        int major = 2, minor = 0, alpha = 0, depth = 0, stencil = 0;
        SDL_GL_GetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, &major);
        SDL_GL_GetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, &minor);
        SDL_GL_GetAttribute(SDL_GL_ALPHA_SIZE, &alpha);
        SDL_GL_GetAttribute(SDL_GL_DEPTH_SIZE, &depth);
        SDL_GL_GetAttribute(SDL_GL_STENCIL_SIZE, &stencil);
        m_format.setRenderableType(QSurfaceFormat::OpenGLES);
        m_format.setVersion(major, minor);
        m_format.setAlphaBufferSize(alpha);
        m_format.setDepthBufferSize(depth);
        m_format.setStencilBufferSize(stencil);
        m_format.setSwapBehavior(QSurfaceFormat::DoubleBuffer);
        m_clock.start();
    }
    QSurfaceFormat format() const override { return m_format; }
    void swapBuffers(QPlatformSurface *surface) override
    {
        // Nothing to show in the background (and the TV may have taken the surface).
        if (surface->surface()->surfaceClass() != QSurface::Window || !g_onScreen) return;
        if (g_resp.frameStart >= 0) {   // drawing time of this frame
            const qint64 took = g_resp.now() - g_resp.frameStart;
            if (took < 1000) {          // (older: a makeCurrent that drew nothing)
                ++g_resp.frames;
                g_resp.frameMax = qMax(g_resp.frameMax, took);
                if (took > 33) ++g_resp.slowFrames;
            }
            g_resp.frameStart = -1;
        }
        // At most 60 frames a second, whatever Qt asks for.
        constexpr qint64 kFrameNs = 1000000000LL / 60;
        const qint64 now = m_clock.nsecsElapsed();
        if (m_last && now - m_last < kFrameNs - 1000000) {
            const qint64 wait = kFrameNs - (now - m_last) - 1000000;   // vsync takes the last bit
            timespec ts{ time_t(wait / 1000000000LL), long(wait % 1000000000LL) };
            nanosleep(&ts, nullptr);
        }
        const qint64 swapStart = g_resp.now();
        SDL_GL_SwapWindow(g_window);
        m_last = m_clock.nsecsElapsed();
        {
            const qint64 t = g_resp.now();
            const qint64 swapTook = t - swapStart;
            ++g_resp.swaps;
            g_resp.swapSum += swapTook;
            g_resp.swapMax = qMax(g_resp.swapMax, swapTook);
            if (swapTook > 20) ++g_resp.slowSwaps;
            if (g_resp.lastSwap >= 0) {
                const qint64 gap = t - g_resp.lastSwap;
                if (gap <= 20) ++g_resp.gaps60;
                else if (gap <= 40) ++g_resp.gaps30;
                else if (gap < 200) ++g_resp.gapsSlower;
            }
            g_resp.lastSwap = t;
        }
        if (g_resp.keyAt >= 0) {        // a button press is on screen now
            const qint64 took = g_resp.now() - g_resp.keyAt;
            ++g_resp.keys;
            g_resp.keySum += took;
            g_resp.keyMax = qMax(g_resp.keyMax, took);
            g_resp.keyAt = -1;
        }
    }
    bool makeCurrent(QPlatformSurface *surface) override
    {
        if (surface && surface->surface()->surfaceClass() == QSurface::Window && g_resp.frameStart < 0) {
            g_resp.frameStart = g_resp.now();
            if (g_resp.keyAt >= 0 && g_resp.keyStartPending) {   // a press: how soon its frame began
                const qint64 took = g_resp.frameStart - g_resp.keyAt;
                ++g_resp.starts;
                g_resp.startSum += took;
                g_resp.startMax = qMax(g_resp.startMax, took);
                g_resp.keyStartPending = false;
            }
        }
        return SDL_GL_MakeCurrent(g_window, g_context) == 0;
    }
    void doneCurrent() override {}   // SDL's is the only context
    bool isValid() const override { return g_window && g_context; }
    QFunctionPointer getProcAddress(const char *name) override
    {
        // EGL before 1.5 only has to return extensions, not core GLES
        // functions; those come from libGLESv2 itself.
        void *p = SDL_GL_GetProcAddress(name);
        if (!p) p = dlsym(RTLD_DEFAULT, name);
        return reinterpret_cast<QFunctionPointer>(p);
    }

private:
    QSurfaceFormat m_format;
    QElapsedTimer m_clock;
    qint64 m_last = 0;
};

class QSdlOffscreenSurface : public QPlatformOffscreenSurface
{
public:
    using QPlatformOffscreenSurface::QPlatformOffscreenSurface;
    bool isValid() const override { return true; }
};

// The remote's and a keyboard's keys as the Qt keys Relay already handles on
// Windows (src/app/RemoteInput.cpp): Back is Escape, OK is Return.
int qtKey(const SDL_Keysym &k)
{
    switch (k.scancode) {
    case SDL_WEBOS_SCANCODE_BACK: return Qt::Key_Escape;
    case SDL_WEBOS_SCANCODE_CH_UP: return Qt::Key_ChannelUp;
    case SDL_WEBOS_SCANCODE_CH_DOWN: return Qt::Key_ChannelDown;
    case SDL_WEBOS_SCANCODE_RED: return Qt::Key_Red;
    case SDL_WEBOS_SCANCODE_GREEN: return Qt::Key_Green;
    case SDL_WEBOS_SCANCODE_YELLOW: return Qt::Key_Yellow;
    case SDL_WEBOS_SCANCODE_BLUE: return Qt::Key_Blue;
    default: break;
    }
    const SDL_Keycode s = k.sym;
    if (s >= SDLK_a && s <= SDLK_z) return Qt::Key_A + (s - SDLK_a);
    if (s >= SDLK_0 && s <= SDLK_9) return Qt::Key_0 + (s - SDLK_0);
    if (s >= SDLK_KP_1 && s <= SDLK_KP_9) return Qt::Key_1 + (s - SDLK_KP_1);
    if (s >= SDLK_F1 && s <= SDLK_F12) return Qt::Key_F1 + (s - SDLK_F1);
    switch (s) {
    case SDLK_KP_0: return Qt::Key_0;
    case SDLK_ESCAPE: return Qt::Key_Escape;
    case SDLK_UP: return Qt::Key_Up;
    case SDLK_DOWN: return Qt::Key_Down;
    case SDLK_LEFT: return Qt::Key_Left;
    case SDLK_RIGHT: return Qt::Key_Right;
    case SDLK_RETURN: case SDLK_KP_ENTER: return Qt::Key_Return;
    case SDLK_BACKSPACE: return Qt::Key_Backspace;
    case SDLK_DELETE: return Qt::Key_Delete;
    case SDLK_TAB: return Qt::Key_Tab;
    case SDLK_SPACE: return Qt::Key_Space;
    case SDLK_HOME: return Qt::Key_Home;
    case SDLK_END: return Qt::Key_End;
    case SDLK_PAGEUP: return Qt::Key_PageUp;
    case SDLK_PAGEDOWN: return Qt::Key_PageDown;
    case SDLK_MINUS: return Qt::Key_Minus;
    case SDLK_PERIOD: return Qt::Key_Period;
    case SDLK_SLASH: return Qt::Key_Slash;
    case SDLK_AUDIOPLAY: return Qt::Key_MediaTogglePlayPause;
    case SDLK_AUDIOSTOP: return Qt::Key_MediaStop;
    case SDLK_AUDIONEXT: return Qt::Key_MediaNext;
    case SDLK_AUDIOPREV: return Qt::Key_MediaPrevious;
    case SDLK_AUDIOREWIND: return Qt::Key_AudioRewind;
    case SDLK_AUDIOFASTFORWARD: return Qt::Key_AudioForward;
    case SDLK_MENU: case SDLK_APPLICATION: return Qt::Key_Menu;
    default: return 0;
    }
}

Qt::KeyboardModifiers qtModifiers(Uint16 m)
{
    Qt::KeyboardModifiers r;
    if (m & KMOD_SHIFT) r |= Qt::ShiftModifier;
    if (m & KMOD_CTRL) r |= Qt::ControlModifier;
    if (m & KMOD_ALT) r |= Qt::AltModifier;
    return r;
}

Qt::MouseButton qtButton(Uint8 b)
{
    switch (b) {
    case SDL_BUTTON_LEFT: return Qt::LeftButton;
    case SDL_BUTTON_RIGHT: return Qt::RightButton;
    case SDL_BUTTON_MIDDLE: return Qt::MiddleButton;
    default: return Qt::NoButton;
    }
}

// SDL's events, read on Qt's event loop once a window is shown.
class QSdlEvents : public QObject
{
public:
    QSdlEvents()
    {
        // Typed text only arrives while a text field has focus (see
        // QSdlInputContext), so keys are not typed twice.
        SDL_StopTextInput();
        m_timer.setInterval(4);
        m_timer.setTimerType(Qt::PreciseTimer);
        connect(&m_timer, &QTimer::timeout, this, &QSdlEvents::poll);
        m_timer.start();
    }

private:
    static QWindow *target()
    {
        if (QWindow *w = QGuiApplication::focusWindow()) return w;
        const auto windows = QGuiApplication::topLevelWindows();
        for (QWindow *w : windows)
            if (w->isVisible()) return w;
        return nullptr;
    }
    QPointF toScene(int x, int y) const
    {
        int ww = g_size.width(), wh = g_size.height();
        SDL_GetWindowSize(g_window, &ww, &wh);
        return QPointF(x * double(g_size.width()) / qMax(1, ww), y * double(g_size.height()) / qMax(1, wh));
    }
    // The responsiveness line in the log, every 10 s while the remote is used
    // (or the main thread was held up).
    void measure()
    {
        const qint64 t = g_resp.now();
        if (m_lastPoll > 0 && t - m_lastPoll > 150) {   // this timer is due every 4 ms
            ++g_resp.stalls;
            g_resp.stallMax = qMax(g_resp.stallMax, t - m_lastPoll);
        }
        m_lastPoll = t;
        if (g_resp.keyAt >= 0 && t - g_resp.keyAt > 3000) { g_resp.keyAt = -1; g_resp.keyStartPending = false; }   // changed nothing
        if (t - g_resp.since < 10000) return;
        if (g_resp.keys > 0 || g_resp.stallMax > 250)
            qInfo("Screen: %d presses showed in %lld ms on average, slowest %lld ms (their frames began after %lld ms"
                  " on average, slowest %lld) · %d frames drawn, slowest %lld ms (%d over 33 ms) · frame gaps while"
                  " moving: %d at 60/s, %d at 30/s, %d slower · handing frames to the TV: %lld ms on average,"
                  " slowest %lld, %d over 20 ms · main thread held up %d times, longest %lld ms",
                  g_resp.keys, g_resp.keys ? g_resp.keySum / g_resp.keys : 0, g_resp.keyMax,
                  g_resp.starts ? g_resp.startSum / g_resp.starts : 0, g_resp.startMax, g_resp.frames,
                  g_resp.frameMax, g_resp.slowFrames, g_resp.gaps60, g_resp.gaps30, g_resp.gapsSlower,
                  g_resp.swaps ? g_resp.swapSum / g_resp.swaps : 0, g_resp.swapMax, g_resp.slowSwaps,
                  g_resp.stalls, g_resp.stallMax);
        g_resp.reset(t);
    }

    void poll()
    {
        measure();
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            QWindow *w = target();
            switch (e.type) {
            case SDL_QUIT:
                qInfo("TV: asked to close Relay");
                QCoreApplication::quit();
                break;
            case SDL_KEYDOWN:
            case SDL_KEYUP: {
                // The Magic Remote's pointer appearing and going away: not keys.
                if (e.key.keysym.scancode == SDL_WEBOS_SCANCODE_CURSOR_SHOW
                    || e.key.keysym.scancode == SDL_WEBOS_SCANCODE_CURSOR_HIDE)
                    break;
                const int key = qtKey(e.key.keysym);
                // Each button once in the log, to learn what a remote sends.
                if (e.type == SDL_KEYDOWN && !m_seen.contains(e.key.keysym.scancode)) {
                    m_seen.insert(e.key.keysym.scancode);
                    qInfo("Remote: button with scancode %d (sym %d) is %s", int(e.key.keysym.scancode),
                          int(e.key.keysym.sym), key ? qPrintable(QStringLiteral("Qt key 0x%1").arg(key, 0, 16))
                                                     : "not used");
                }
                if (!key) break;
                if (!w) break;
                // From here until its frame is on screen (OK acts when it comes up).
                if (g_resp.keyAt < 0 && (key == Qt::Key_Return ? e.type == SDL_KEYUP : e.type == SDL_KEYDOWN)) {
                    g_resp.keyAt = g_resp.now();
                    g_resp.keyStartPending = true;
                }
                // Printable keys carry their text unless a text field gets it
                // from SDL's text input instead.
                QString text;
                const SDL_Keycode s = e.key.keysym.sym;
                if (!SDL_IsTextInputActive() && s >= 32 && s < 127) {
                    const QChar c = QLatin1Char(char(s));
                    text = (e.key.keysym.mod & KMOD_SHIFT) ? c.toUpper() : c;
                }
                QWindowSystemInterface::handleKeyEvent(w, e.type == SDL_KEYDOWN ? QEvent::KeyPress : QEvent::KeyRelease,
                                                       key, qtModifiers(e.key.keysym.mod), text, e.key.repeat != 0);
                break;
            }
            case SDL_TEXTINPUT: {
                if (!w) break;
                const QString text = QString::fromUtf8(e.text.text);
                for (const QChar c : text) {
                    QWindowSystemInterface::handleKeyEvent(w, QEvent::KeyPress, 0, Qt::NoModifier, QString(c));
                    QWindowSystemInterface::handleKeyEvent(w, QEvent::KeyRelease, 0, Qt::NoModifier, QString(c));
                }
                break;
            }
            case SDL_MOUSEMOTION:
                if (!w) break;
                m_pointer = toScene(e.motion.x, e.motion.y);
                QWindowSystemInterface::handleMouseEvent(w, m_pointer, m_pointer, m_buttons, Qt::NoButton,
                                                         QEvent::MouseMove);
                break;
            case SDL_MOUSEBUTTONDOWN:
            case SDL_MOUSEBUTTONUP: {
                const Qt::MouseButton b = qtButton(e.button.button);
                if (!w || b == Qt::NoButton) break;
                m_pointer = toScene(e.button.x, e.button.y);
                m_buttons.setFlag(b, e.type == SDL_MOUSEBUTTONDOWN);
                QWindowSystemInterface::handleMouseEvent(
                    w, m_pointer, m_pointer, m_buttons, b,
                    e.type == SDL_MOUSEBUTTONDOWN ? QEvent::MouseButtonPress : QEvent::MouseButtonRelease);
                break;
            }
            case SDL_MOUSEWHEEL:
                if (!w) break;
                QWindowSystemInterface::handleWheelEvent(w, m_pointer, m_pointer, QPoint(),
                                                         QPoint(e.wheel.x * 120, e.wheel.y * 120));
                break;
            case SDL_APP_WILLENTERBACKGROUND:
            case SDL_APP_DIDENTERBACKGROUND:
                setOnScreen(false, w);
                break;
            case SDL_APP_WILLENTERFOREGROUND:
            case SDL_APP_DIDENTERFOREGROUND:
                setOnScreen(true, w);
                break;
            case SDL_WINDOWEVENT:
                if (!m_seenWindowEvents.contains(e.window.event)) {   // what the TV sends, once each
                    m_seenWindowEvents.insert(e.window.event);
                    qInfo("TV: window event %d", int(e.window.event));
                }
                switch (e.window.event) {
                // The TV's home screen, another app or input in front: Relay
                // is in the background (the TV's own menus over it only take focus).
                case SDL_WINDOWEVENT_HIDDEN: case SDL_WINDOWEVENT_MINIMIZED:
                    setOnScreen(false, w); break;
                case SDL_WINDOWEVENT_SHOWN: case SDL_WINDOWEVENT_RESTORED:
                    setOnScreen(true, w); break;
                case SDL_WINDOWEVENT_EXPOSED:
                    if (w && g_onScreen) QWindowSystemInterface::handleExposeEvent(w, QRegion(screenRect()));
                    break;
                default: break;
                }
                break;
            default:
                break;
            }
        }
    }

    // In the background Qt stops drawing (nothing to show it on) and the app
    // reports itself suspended, which Relay's TV player uses to let go of
    // the TV's video decoder; back in front, both resume.
    static void setOnScreen(bool on, QWindow *w)
    {
        if (on == g_onScreen) return;
        g_onScreen = on;
        qInfo("TV: Relay is %s", on ? "back on screen" : "in the background");
        QWindowSystemInterface::handleApplicationStateChanged(on ? Qt::ApplicationActive : Qt::ApplicationSuspended);
        if (w) QWindowSystemInterface::handleExposeEvent(w, on ? QRegion(screenRect()) : QRegion());
    }

    QTimer m_timer;
    qint64 m_lastPoll = 0;
    QPointF m_pointer;
    Qt::MouseButtons m_buttons;
    QSet<int> m_seen;
    QSet<int> m_seenWindowEvents;
};

QSdlEvents *g_events = nullptr;

// Relay's window: SDL's, always full screen.
class QSdlWindow : public QPlatformWindow
{
public:
    using QPlatformWindow::QPlatformWindow;
    QRect geometry() const override { return screenRect(); }
    void setGeometry(const QRect &) override
    {
        QPlatformWindow::setGeometry(screenRect());
        QWindowSystemInterface::handleGeometryChange(window(), screenRect());
    }
    void setVisible(bool visible) override
    {
        QPlatformWindow::setVisible(visible);
        if (!visible) {
            QWindowSystemInterface::handleExposeEvent(window(), QRegion());
            return;
        }
        if (!g_events) g_events = new QSdlEvents;
        QWindowSystemInterface::handleGeometryChange(window(), screenRect());
        QWindowSystemInterface::handleWindowStateChanged(window(), Qt::WindowFullScreen);
        QWindowSystemInterface::handleFocusWindowChanged(window(), Qt::ActiveWindowFocusReason);
        QWindowSystemInterface::handleExposeEvent(window(), QRegion(screenRect()));
    }
    // The window can't leave full screen (Back arrives as Escape, and Relay's
    // Escape leaves full screen on a computer): report it straight back.
    void setWindowState(Qt::WindowStates state) override
    {
        if (!(state & Qt::WindowFullScreen))
            QWindowSystemInterface::handleWindowStateChanged(window(), Qt::WindowFullScreen);
    }
    bool isExposed() const override { return window()->isVisible() && g_onScreen; }
};

// A text field with focus turns on SDL's text input (and the TV's keyboard).
class QSdlInputContext : public QPlatformInputContext
{
public:
    bool isValid() const override { return true; }
    void setFocusObject(QObject *object) override
    {
        bool wants = false;
        if (object) {
            QInputMethodQueryEvent q(Qt::ImEnabled);
            QCoreApplication::sendEvent(object, &q);
            wants = q.value(Qt::ImEnabled).toBool();
        }
        if (wants && !SDL_IsTextInputActive()) SDL_StartTextInput();
        else if (!wants && SDL_IsTextInputActive()) SDL_StopTextInput();
    }
    void showInputPanel() override { SDL_StartTextInput(); }
    void hideInputPanel() override { SDL_StopTextInput(); }
    bool isInputPanelVisible() const override { return SDL_IsTextInputActive(); }
};

class QSdlIntegration : public QPlatformIntegration
{
public:
    ~QSdlIntegration() override
    {
        delete g_events;
        g_events = nullptr;
        if (m_screen) QWindowSystemInterface::handleScreenRemoved(m_screen);
        delete m_fonts;
    }
    void initialize() override
    {
        m_screen = new QSdlScreen;
        QWindowSystemInterface::handleScreenAdded(m_screen, true);
    }
    bool hasCapability(Capability cap) const override
    {
        switch (cap) {
        case OpenGL: return true;
        // Qt Quick draws on the main thread, with SDL.
        case ThreadedOpenGL: return false;
        case BufferQueueingOpenGL: return false;
        // One full-screen window: dialogs and menus open inside it.
        case MultipleWindows: return false;
        case NonFullScreenWindows: return false;
        case WindowManagement: return false;
        default: return QPlatformIntegration::hasCapability(cap);
        }
    }
    QPlatformWindow *createPlatformWindow(QWindow *window) const override { return new QSdlWindow(window); }
    QPlatformBackingStore *createPlatformBackingStore(QWindow *) const override { return nullptr; }
    QPlatformOpenGLContext *createPlatformOpenGLContext(QOpenGLContext *context) const override
    {
        return new QSdlGLContext(context->format());
    }
    QPlatformOffscreenSurface *createPlatformOffscreenSurface(QOffscreenSurface *surface) const override
    {
        return new QSdlOffscreenSurface(surface);
    }
    QAbstractEventDispatcher *createEventDispatcher() const override { return createUnixEventDispatcher(); }
    QPlatformFontDatabase *fontDatabase() const override
    {
        if (!m_fonts) m_fonts = new QGenericUnixFontDatabase;
        return m_fonts;
    }
    QPlatformInputContext *inputContext() const override { return &m_input; }
    QOpenGLContext::OpenGLModuleType openGLModuleType() override { return QOpenGLContext::LibGLES; }
    // Qt's fontconfig code asks for this and doesn't check for none.
    QPlatformNativeInterface *nativeInterface() const override { return &m_native; }

private:
    mutable QPlatformNativeInterface m_native;
    mutable QSdlInputContext m_input;
    QSdlScreen *m_screen = nullptr;
    mutable QPlatformFontDatabase *m_fonts = nullptr;
};
} // namespace

void qsdlSetWindow(SDL_Window *window, SDL_GLContext context, const QSize &size)
{
    g_window = window;
    g_context = context;
    g_size = size;
}

bool qsdlOnScreen() { return g_onScreen; }

class QSdlIntegrationPlugin : public QPlatformIntegrationPlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID QPlatformIntegrationFactoryInterface_iid FILE "qsdl.json")
public:
    QPlatformIntegration *create(const QString &key, const QStringList &) override
    {
        return key.compare(QLatin1String("sdl"), Qt::CaseInsensitive) == 0 ? new QSdlIntegration : nullptr;
    }
};

#include "qsdlintegration.moc"
