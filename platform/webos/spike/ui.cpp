// Qt Quick on webOS, the way Relay's screens will work: SDL keeps the window,
// input and video layer; Qt Quick renders through QQuickRenderControl into a
// texture in SDL's GL context, and the spike draws that texture over the video.

#include "ui.h"

#include "Fonts.h"
#include "qsdlintegration.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_webOS.h>
#include <GLES2/gl2.h>

#include <QElapsedTimer>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickGraphicsDevice>
#include <QQuickItem>
#include <QQuickRenderControl>
#include <QQuickRenderTarget>
#include <QQuickWindow>
#include <QtPlugin>

Q_IMPORT_PLUGIN(QSdlIntegrationPlugin)

namespace {

std::function<void(const std::string &)> g_log;

// Qt's own messages (QML errors, console.log) go to the spike's log.
void onQtMessage(QtMsgType, const QMessageLogContext &, const QString &message)
{
    if (g_log) g_log("qt: " + message.toStdString());
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

class SpikeState : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int state READ state NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(QString detail READ detail NOTIFY changed)
    Q_PROPERTY(double fps READ fps NOTIFY fpsChanged)
    Q_PROPERTY(QString fontInfo READ fontInfo CONSTANT)
public:
    QString fontInfo() const
    {
        return QStringLiteral("App font: %1 (Qt %2)").arg(QGuiApplication::font().family(), QString::fromLatin1(qVersion()));
    }
    Q_INVOKABLE void quit() { m_quit = true; }
    bool quitRequested() const { return m_quit; }
    int state() const { return m_state; }
    QString status() const { return m_status; }
    QString detail() const { return m_detail; }
    double fps() const { return m_fps; }
    void set(const UiStatus &s)
    {
        const QString st = QString::fromStdString(s.status), de = QString::fromStdString(s.detail);
        if (s.state == m_state && st == m_status && de == m_detail) return;
        m_state = s.state; m_status = st; m_detail = de;
        emit changed();
    }
    void setFps(double f) { m_fps = f; emit fpsChanged(); }
signals:
    void changed();
    void fpsChanged();
private:
    int m_state = 0;
    QString m_status, m_detail;
    double m_fps = 0;
    bool m_quit = false;
};

GLuint compile(GLenum type, const char *source, const std::function<void(const std::string &)> &log)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &source, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char buf[512] = {};
        glGetShaderInfoLog(s, sizeof buf - 1, nullptr, buf);
        log(std::string("ui: shader: ") + buf);
    }
    return s;
}

// The remote's keys as the Qt keys Relay already handles on Windows
// (src/app/RemoteInput.cpp): Back is Escape, OK is Return.
int qtKey(const SDL_Keysym &k)
{
    if (k.scancode == SDL_WEBOS_SCANCODE_BACK) return Qt::Key_Escape;   // scancode 482
    switch (k.sym) {
    case SDLK_ESCAPE: return Qt::Key_Escape;
    case SDLK_UP: return Qt::Key_Up;
    case SDLK_DOWN: return Qt::Key_Down;
    case SDLK_LEFT: return Qt::Key_Left;
    case SDLK_RIGHT: return Qt::Key_Right;
    case SDLK_RETURN: case SDLK_KP_ENTER: return Qt::Key_Return;
    default: return 0;
    }
}

} // namespace

void runUi(SDL_Window *window, const std::function<UiStatus()> &status,
           const std::function<void(const std::string &)> &log)
{
    SDL_GLContext ctx = SDL_GL_CreateContext(window);
    if (!ctx) { log(std::string("ui: no GL context: ") + SDL_GetError()); return; }
    SDL_GL_MakeCurrent(window, ctx);
    SDL_GL_SetSwapInterval(1);
    // Screens are drawn at 1080p and the TV scales them to its 4K panel: much
    // lighter on the TV, and the video has its own full-resolution layer.
    const int w = 1920, h = 1080;
    int dw = w, dh = h;
    SDL_GL_GetDrawableSize(window, &dw, &dh);
    log("ui: GL " + std::string(reinterpret_cast<const char *>(glGetString(GL_VERSION))) + ", "
        + reinterpret_cast<const char *>(glGetString(GL_RENDERER)) + ", screens " + std::to_string(w) + "x"
        + std::to_string(h) + ", window " + std::to_string(dw) + "x" + std::to_string(dh));

    qputenv("QT_QPA_PLATFORM", "sdl");
    qputenv("QSG_RHI_BACKEND", "opengl");
    qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");
    static int argc = 1;
    static char name[] = "relay-spike";
    static char *argv[] = { name, nullptr };
    g_log = log;
    qInstallMessageHandler(onQtMessage);
    QGuiApplication app(argc, argv);
    useBundledFonts();
    log("ui: Qt " + std::string(qVersion()) + ", " + std::to_string(QFontDatabase::families().size()) + " font families, using "
        + QGuiApplication::font().families().join(QStringLiteral(", ")).toStdString());

    qsdlSetWindow(window, ctx, QSize(w, h));
    QSurfaceFormat format;
    format.setRenderableType(QSurfaceFormat::OpenGLES);
    format.setVersion(2, 0);
    format.setAlphaBufferSize(8);
    QSurfaceFormat::setDefaultFormat(format);
    QOpenGLContext gl;
    gl.setFormat(format);
    if (!gl.create()) { log("ui: Qt could not use SDL's GL context"); return; }
    log("ui: Qt has SDL's GL context");

    QQuickRenderControl control;
    QQuickWindow quickWindow(&control);
    quickWindow.setColor(Qt::transparent);
    quickWindow.setGeometry(0, 0, w, h);
    quickWindow.setGraphicsDevice(QQuickGraphicsDevice::fromOpenGLContext(&gl));
    // Qt Quick only gives keys to the focused item of the active window. This
    // window is never shown (SDL owns the real one), so make it the active one.
    quickWindow.create();
    quickWindow.requestActivate();
    QCoreApplication::processEvents();
    log("ui: Quick window active: " + std::string(QGuiApplication::focusWindow() == &quickWindow ? "yes" : "no"));
    log("ui: starting QQuickRenderControl");
    if (!control.initialize()) { log("ui: QQuickRenderControl did not start"); return; }
    log("ui: QQuickRenderControl started");

    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    quickWindow.setRenderTarget(QQuickRenderTarget::fromOpenGLTexture(texture, QSize(w, h)));

    SpikeState state;
    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("spike"), &state);
    QQmlComponent component(&engine);
    log("ui: loading QML");
    component.loadFromModule(u"RelaySpike", u"Main");
    QQuickItem *root = qobject_cast<QQuickItem *>(component.create());
    if (!root) {
        log("ui: QML: " + component.errorString().toStdString());
        control.invalidate();
        log("ui: stopped after the QML error");
        return;
    }
    root->setParentItem(quickWindow.contentItem());
    root->setSize(QSizeF(w, h));
    root->forceActiveFocus();
    QQuickItem *focused = quickWindow.activeFocusItem();
    log("ui: QML loaded, key focus on " + std::string(focused ? focused->metaObject()->className() : "nothing"));

    // Draws Qt Quick's texture over whatever is under the window (the video).
    SDL_GL_MakeCurrent(window, ctx);
    const char *vs = "attribute vec2 p; varying vec2 t;\n"
                     "void main() { t = p * 0.5 + 0.5; gl_Position = vec4(p, 0.0, 1.0); }\n";
    const char *fs = "precision mediump float; varying vec2 t; uniform sampler2D s;\n"
                     "void main() { gl_FragColor = texture2D(s, t); }\n";
    GLuint program = glCreateProgram();
    glAttachShader(program, compile(GL_VERTEX_SHADER, vs, log));
    glAttachShader(program, compile(GL_FRAGMENT_SHADER, fs, log));
    glBindAttribLocation(program, 0, "p");
    glLinkProgram(program);
    const GLfloat quad[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
    GLuint vbo = 0;
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);

    QElapsedTimer clock;
    clock.start();
    int frames = 0;
    qint64 lastFps = 0;
    // The pointer (LG's Magic Remote): SDL's window coordinates to the 1080p scene.
    int winW = w, winH = h;
    SDL_GetWindowSize(window, &winW, &winH);
    const double sx = double(w) / (winW > 0 ? winW : w), sy = double(h) / (winH > 0 ? winH : h);
    Qt::MouseButtons buttons;
    QPointF pointer;
    int pointerLogs = 0;

    bool running = true;
    while (running && !state.quitRequested()) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = false;
            if (e.type == SDL_MOUSEMOTION || e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP) {
                const int mx = e.type == SDL_MOUSEMOTION ? e.motion.x : e.button.x;
                const int my = e.type == SDL_MOUSEMOTION ? e.motion.y : e.button.y;
                pointer = QPointF(mx * sx, my * sy);
                QEvent::Type type = QEvent::MouseMove;
                Qt::MouseButton button = Qt::NoButton;
                if (e.type != SDL_MOUSEMOTION) {
                    button = qtButton(e.button.button);
                    if (button == Qt::NoButton) continue;
                    type = e.type == SDL_MOUSEBUTTONDOWN ? QEvent::MouseButtonPress : QEvent::MouseButtonRelease;
                    buttons.setFlag(button, e.type == SDL_MOUSEBUTTONDOWN);
                }
                if (type != QEvent::MouseMove || pointerLogs < 3) {
                    log("ui: pointer " + std::to_string(int(pointer.x())) + "," + std::to_string(int(pointer.y()))
                        + (type == QEvent::MouseButtonPress ? " press" : type == QEvent::MouseButtonRelease ? " release" : " move"));
                    if (type == QEvent::MouseMove) pointerLogs++;
                }
                QMouseEvent ev(type, pointer, pointer, button, buttons, Qt::NoModifier);
                QCoreApplication::sendEvent(&quickWindow, &ev);
                continue;
            }
            if (e.type == SDL_MOUSEWHEEL) {
                QWheelEvent ev(pointer, pointer, QPoint(), QPoint(e.wheel.x * 120, e.wheel.y * 120), buttons,
                               Qt::NoModifier, Qt::NoScrollPhase, false);
                QCoreApplication::sendEvent(&quickWindow, &ev);
                continue;
            }
            if (e.type != SDL_KEYDOWN && e.type != SDL_KEYUP) continue;
            const int key = qtKey(e.key.keysym);
            if (e.type == SDL_KEYDOWN)
                log("ui: key scancode " + std::to_string(e.key.keysym.scancode) + " sym " + std::to_string(e.key.keysym.sym)
                    + " -> Qt " + (key ? std::to_string(key) : std::string("(not mapped)")));
            if (key) {
                QKeyEvent ev(e.type == SDL_KEYDOWN ? QEvent::KeyPress : QEvent::KeyRelease, key, Qt::NoModifier,
                             QString(), e.key.repeat != 0);
                QCoreApplication::sendEvent(&quickWindow, &ev);
            }
        }
        QCoreApplication::processEvents();
        state.set(status());

        gl.makeCurrent(&quickWindow);
        control.polishItems();
        control.beginFrame();
        control.sync();
        control.render();
        control.endFrame();

        SDL_GL_MakeCurrent(window, ctx);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, dw, dh);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_STENCIL_TEST);
        glDisable(GL_CULL_FACE);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);   // Qt Quick's output is premultiplied
        glUseProgram(program);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texture);
        glUniform1i(glGetUniformLocation(program, "s"), 0);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
        glEnableVertexAttribArray(0);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glDisableVertexAttribArray(0);
        SDL_GL_SwapWindow(window);

        frames++;
        const qint64 now = clock.elapsed();
        if (now - lastFps >= 1000) {
            state.setFps(frames * 1000.0 / double(now - lastFps));
            if (now < 15000 || (now / 1000) % 30 == 0)
                log("ui: " + std::to_string(frames) + " frames in " + std::to_string(now - lastFps) + " ms");
            frames = 0;
            lastFps = now;
        }
    }
    log("ui: closing");
    delete root;
    control.invalidate();
    qInstallMessageHandler(nullptr);
    g_log = nullptr;
}

#include "ui.moc"
