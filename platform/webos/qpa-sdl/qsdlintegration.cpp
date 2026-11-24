#include "qsdlintegration.h"

#include <SDL2/SDL.h>
#include <dlfcn.h>

#include <QOpenGLContext>
#include <QSurfaceFormat>
#include <QtGui/private/qgenericunixeventdispatcher_p.h>
#include <QtGui/private/qgenericunixfontdatabase_p.h>
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

class QSdlScreen : public QPlatformScreen
{
public:
    QRect geometry() const override { return QRect(QPoint(), g_size); }
    int depth() const override { return 32; }
    QImage::Format format() const override { return QImage::Format_ARGB32_Premultiplied; }
};

// Qt's view of SDL's context. Making it current makes SDL's context current;
// SDL presents the frame, so swapping does nothing here.
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
    }
    QSurfaceFormat format() const override { return m_format; }
    void swapBuffers(QPlatformSurface *) override {}
    bool makeCurrent(QPlatformSurface *) override { return SDL_GL_MakeCurrent(g_window, g_context) == 0; }
    void doneCurrent() override {}   // the spike draws with the same context next
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
};

class QSdlOffscreenSurface : public QPlatformOffscreenSurface
{
public:
    using QPlatformOffscreenSurface::QPlatformOffscreenSurface;
    bool isValid() const override { return true; }
};

class QSdlWindow : public QPlatformWindow
{
public:
    using QPlatformWindow::QPlatformWindow;
};

class QSdlIntegration : public QPlatformIntegration
{
public:
    ~QSdlIntegration() override
    {
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
        if (cap == OpenGL) return true;
        if (cap == ThreadedOpenGL) return false;
        return QPlatformIntegration::hasCapability(cap);
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
    QOpenGLContext::OpenGLModuleType openGLModuleType() override { return QOpenGLContext::LibGLES; }
    // Qt's fontconfig code asks for this and doesn't check for none.
    QPlatformNativeInterface *nativeInterface() const override { return &m_native; }

private:
    mutable QPlatformNativeInterface m_native;
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
