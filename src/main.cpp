#include "app/AppSettings.h"
#include "app/Backup.h"
#include "app/Log.h"
#include "app/Session.h"
#include "app/Trakt.h"
#include "app/Updater.h"
#include "cast/CastService.h"
#include "app/UserLibrary.h"
#include "app/Parental.h"
#include "app/OtherPlaylists.h"
#include "epg/EpgService.h"
#include "models/ChannelModel.h"
#include "player/MpvItem.h"
#include "player/PlayerController.h"
#include "sources/PlaylistService.h"
#include "vod/VodService.h"
#include "sources/CatchupService.h"
#include "record/ScheduleService.h"

#include <QCommandLineParser>
#include <QNetworkAccessManager>
#include <QNetworkDiskCache>
#include <QQmlNetworkAccessManagerFactory>
#include <QDir>
#include <QSettings>
#include <QStandardPaths>
#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSurfaceFormat>
#include <QSysInfo>
#include <QTimer>

#include <clocale>

#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
#include <QStyleHints>
#endif
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dwmapi.h>
#endif

namespace {
// Channel logos and posters are kept on disk (up to 300 MB), so the movie
// grid doesn't download every poster again on each visit.
class CachedNetworkFactory : public QQmlNetworkAccessManagerFactory
{
public:
    explicit CachedNetworkFactory(QString dir) : m_dir(std::move(dir)) {}
    QNetworkAccessManager *create(QObject *parent) override
    {
        auto *nam = new QNetworkAccessManager(parent);
        auto *cache = new QNetworkDiskCache(nam);
        cache->setCacheDirectory(m_dir);
        cache->setMaximumCacheSize(300LL * 1024 * 1024);
        nam->setCache(cache);
        nam->setTransferTimeout(20000);
        return nam;
    }
private:
    QString m_dir;
};
// A dark title bar in Relay's colours, whatever the system theme.
void darkTitleBar(QWindow *window)
{
#ifdef Q_OS_WIN
    if (!window) return;
    const HWND hwnd = reinterpret_cast<HWND>(window->winId());
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof dark);
    // Windows 11: the exact background colour (#0A0C10) and light text.
    const COLORREF caption = RGB(0x0A, 0x0C, 0x10);
    const COLORREF text = RGB(0xE8, 0xEA, 0xEE);
    DwmSetWindowAttribute(hwnd, 35 /* DWMWA_CAPTION_COLOR */, &caption, sizeof caption);
    DwmSetWindowAttribute(hwnd, 34 /* DWMWA_BORDER_COLOR */, &caption, sizeof caption);
    DwmSetWindowAttribute(hwnd, 36 /* DWMWA_TEXT_COLOR */, &text, sizeof text);
#else
    Q_UNUSED(window);
#endif
}
} // namespace

int main(int argc, char *argv[])
{
    // libmpv renders through OpenGL, so the scene graph must use OpenGL too
    // (Qt 6 defaults to Direct3D on Windows and Metal on macOS).
    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
    QSurfaceFormat fmt = QSurfaceFormat::defaultFormat();
#ifdef Q_OS_MACOS
    fmt.setVersion(3, 2);
    fmt.setProfile(QSurfaceFormat::CoreProfile);
#endif
    // Ask to be told when the graphics card resets (a driver fault, a driver
    // update): Qt then rebuilds the picture instead of drawing into a dead
    // OpenGL context.
    fmt.setOption(QSurfaceFormat::ResetNotification);
    QSurfaceFormat::setDefaultFormat(fmt);

    // Interface size (Settings): everything Relay draws is scaled by Qt, so
    // it has to be known before the application object exists.
    if (!qEnvironmentVariableIsSet("QT_SCALE_FACTOR")) {
        const int percent = QSettings(QStringLiteral("Relay"), QStringLiteral("Relay"))
                                .value(QStringLiteral("ui/scale"), 100).toInt();
        if (percent != 100 && percent >= 75 && percent <= 200)
            qputenv("QT_SCALE_FACTOR", QByteArray::number(percent / 100.0, 'f', 2));
    }

    QGuiApplication app(argc, argv);
    QGuiApplication::setOrganizationName(QStringLiteral("Relay"));
    QGuiApplication::setApplicationName(QStringLiteral("Relay"));
    QGuiApplication::setApplicationVersion(QStringLiteral(RELAY_VERSION));
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    // Relay is always dark: dark window frame and controls on every system.
    QGuiApplication::styleHints()->setColorScheme(Qt::ColorScheme::Dark);
#endif
    QGuiApplication::setWindowIcon(QIcon(QStringLiteral(":/icons/relay.png")));

    // mpv requires the C numeric locale (Qt may have changed it).
    std::setlocale(LC_NUMERIC, "C");

    const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dataDir);
    Log::install(dataDir);
    qInfo().noquote() << QStringLiteral("Relay %1 · Qt %2 · %3")
                             .arg(QGuiApplication::applicationVersion(), QString::fromLatin1(qVersion()),
                                  QSysInfo::prettyProductName());
    qInfo().noquote() << "Data folder:" << QDir::toNativeSeparators(dataDir);

    QCommandLineParser cli;
    cli.addHelpOption();
    cli.addVersionOption();
    QCommandLineOption playlistOpt(QStringLiteral("playlist"),
                                   QStringLiteral("Open this M3U URL or file and remember it."),
                                   QStringLiteral("location"));
    QCommandLineOption fullscreenOpt(QStringLiteral("fullscreen"), QStringLiteral("Start fullscreen."));
    QCommandLineOption shotOpt(QStringLiteral("screenshot"),
                               QStringLiteral("Developer: save a window capture to <file> and quit."),
                               QStringLiteral("file"));
    QCommandLineOption shotDelayOpt(QStringLiteral("screenshot-delay"),
                                    QStringLiteral("Developer: milliseconds before the capture."),
                                    QStringLiteral("ms"), QStringLiteral("4000"));
    QCommandLineOption shotCountOpt(QStringLiteral("screenshot-count"),
                                    QStringLiteral("Developer: take <n> captures, one per delay."),
                                    QStringLiteral("n"), QStringLiteral("1"));
    cli.addOptions({playlistOpt, fullscreenOpt, shotOpt, shotDelayOpt, shotCountOpt});
    cli.process(app);

    QQuickStyle::setStyle(QStringLiteral("Basic"));

    // A backup chosen in Settings → Restore is put in place before anything
    // opens the files it replaces.
    Backup::applyPending(dataDir);

    auto *settings = new AppSettings(&app);
    auto *playlists = new PlaylistService(&app);
    // Each playlist has its own favorites/locks/progress and guide database.
    Session::prepareStorage(dataDir, settings->activeId());
    const QString playlistDir = Session::storageDir(dataDir, settings->activeId());
    auto *library = new UserLibrary(playlistDir + QStringLiteral("/library.json"), &app);
    auto *channels = new ChannelModel(&app);
    channels->setLibrary(library);
    QObject::connect(&app, &QCoreApplication::aboutToQuit, library, &UserLibrary::saveNow);
    auto *player = new PlayerController(channels, settings, &app);

    auto *epg = new EpgService(playlistDir + QStringLiteral("/guide.sqlite"), &app);
    channels->setEpg(epg);
    // Close the guide database while the application still exists; Qt's SQL
    // module needs it (otherwise a warning is printed on exit).
    QObject::connect(&app, &QCoreApplication::aboutToQuit, epg, &EpgService::shutdown);
    player->setEpg(epg);

    auto *vod = new VodService(library, dataDir + QStringLiteral("/vod"), &app);
    vod->setTmdbKey(settings->tmdbKey());
    auto *trakt = new TraktService(&app);
    QObject::connect(trakt, &TraktService::watchedFetched, vod, &VodService::applyTraktWatched);
    QObject::connect(vod, &VodService::watchedByHand, trakt, &TraktService::markWatched);
    QObject::connect(settings, &AppSettings::tmdbKeyChanged, vod, [vod, settings] { vod->setTmdbKey(settings->tmdbKey()); });
    auto *parental = new Parental(channels, library, &app);
    player->setParental(parental);
    vod->setBlockedCategoryCheck([parental](int section, const QString &id) {
        return parental->categoryBlocked(section, id);
    });
    QObject::connect(parental, &Parental::changed, vod, &VodService::refresh);
    auto *catchup = new CatchupService(channels, &app);
    auto *schedule = new ScheduleService(
        channels, epg, dataDir,
        QDir(QStandardPaths::writableLocation(QStandardPaths::MoviesLocation)).filePath(QStringLiteral("Relay")),
        settings->activeId(), &app);
    QObject::connect(&app, &QCoreApplication::aboutToQuit, schedule, &ScheduleService::shutdown);
    player->setCatchup(catchup);
    auto *session = new Session(settings, playlists, channels, player, epg, vod, catchup, &app);
    session->setStorage(library, schedule, dataDir);
    auto *others = new OtherPlaylists(settings, dataDir, &app);

    qmlRegisterType<MpvItem>("Relay.Video", 1, 0, "MpvVideo");
    qmlRegisterSingletonInstance("Relay.Core", 1, 0, "Settings", settings);
    qmlRegisterSingletonInstance("Relay.Core", 1, 0, "Playlists", playlists);
    qmlRegisterSingletonInstance("Relay.Core", 1, 0, "Channels", channels);
    qmlRegisterSingletonInstance("Relay.Core", 1, 0, "Player", player);
    qmlRegisterSingletonInstance("Relay.Core", 1, 0, "Session", session);
    qmlRegisterSingletonInstance("Relay.Core", 1, 0, "Epg", epg);
    qmlRegisterSingletonInstance("Relay.Core", 1, 0, "Vod", vod);
    qmlRegisterSingletonInstance("Relay.Core", 1, 0, "Trakt", trakt);
    qmlRegisterSingletonInstance("Relay.Core", 1, 0, "Updates", new Updater(&app));
    auto *cast = new CastService(&app);
    qmlRegisterSingletonInstance("Relay.Core", 1, 0, "Cast", cast);
    qmlRegisterSingletonInstance("Relay.Core", 1, 0, "Catchup", catchup);
    qmlRegisterSingletonInstance("Relay.Core", 1, 0, "Schedule", schedule);
    qmlRegisterSingletonInstance("Relay.Core", 1, 0, "Parental", parental);
    qmlRegisterSingletonInstance("Relay.Core", 1, 0, "OtherPlaylists", others);
    qmlRegisterSingletonInstance("Relay.Core", 1, 0, "Backup", new Backup(settings, library, dataDir, &app));
    qmlRegisterUncreatableType<VodModel>("Relay.Core", 1, 0, "VodModel", QStringLiteral("Use Vod.items"));

    // Declared before the engine so it outlives it.
    CachedNetworkFactory imageCache(
        QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/images"));
    QQmlApplicationEngine engine;
    engine.setNetworkAccessManagerFactory(&imageCache);
    engine.setInitialProperties({{QStringLiteral("startFullscreen"), cli.isSet(fullscreenOpt)}});
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app,
                     [] { QCoreApplication::exit(1); }, Qt::QueuedConnection);
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    engine.loadFromModule("Relay", "Main");
#else
    engine.load(QUrl(QStringLiteral("qrc:/Relay/Main.qml")));
#endif
    if (engine.rootObjects().isEmpty()) {
        epg->shutdown();   // close the guide database while the app object still exists
        return 1;
    }

    darkTitleBar(qobject_cast<QWindow *>(engine.rootObjects().first()));
    if (auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first())) {
        QObject::connect(window, &QQuickWindow::sceneGraphError, window, [](QQuickWindow::SceneGraphError, const QString &message) {
            qWarning().noquote() << "Graphics:" << message;
        });
        QObject::connect(window, &QQuickWindow::sceneGraphInvalidated, window, [] {
            qInfo().noquote() << "Graphics: the picture's graphics resources were released";
        });
    }

    // Open straight into live TV, like a set-top box.
    if (cli.isSet(playlistOpt)) session->addM3u(cli.value(playlistOpt));
    else if (settings->hasSource()) session->reload();

    if (cli.isSet(shotOpt)) {
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        const QString path = cli.value(shotOpt);
        const int count = qMax(1, cli.value(shotCountOpt).toInt());
        auto *timer = new QTimer(&app);
        timer->setInterval(cli.value(shotDelayOpt).toInt());
        QObject::connect(timer, &QTimer::timeout, &app, [window, path, count, n = 0]() mutable {
            ++n;
            QString file = path;
            if (count > 1) {
                const int dot = file.lastIndexOf(QLatin1Char('.'));
                file.insert(dot < 0 ? file.size() : dot, QStringLiteral("-%1").arg(n));
            }
            window->grabWindow().save(file);
            if (n >= count) QCoreApplication::quit();
        });
        timer->start();
    }

    return app.exec();
}
