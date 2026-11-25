#include "AppSettings.h"

#include "SecretStore.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QRegularExpression>
#include <QUrl>
#include <QtDebug>

// QSettings layout:
//   playlists/order          ids in the user's order
//   playlists/active         active id
//   playlists/<id>/name, type, xtreamServer, xtreamUser, xtreamOutput,
//                  guideUrl, lastChannel, multiviewKeys,
//                  m3uSecure / xtreamPasswordSecure (true: in the credential store,
//                  as "playlist-<id>" / "xtream-password-<id>"),
//                  m3u / xtreamPassword (only where there's no credential store)
//   player/timeshiftMb       global
//   player/audioLanguage, player/subtitleLanguage   "" (as the video says), a code ("en"), subtitles also "off"

namespace { QString s_userAgent; }

QString AppSettings::effectiveUserAgent()
{
    return s_userAgent.isEmpty() ? QStringLiteral("Relay/%1").arg(QCoreApplication::applicationVersion())
                                 : s_userAgent;
}

void AppSettings::setTransientUserAgent(const QString &ua)
{
    if (ua.trimmed().isEmpty()) syncUserAgent();
    else s_userAgent = ua.trimmed();
}

void AppSettings::syncUserAgent() { s_userAgent = userAgent(); }

bool AppSettings::guideKeepOwn() const { return !m_active.isEmpty() && m_s.value(key("guideKeepOwn"), false).toBool(); }
void AppSettings::setGuideKeepOwn(bool v)
{
    if (m_active.isEmpty() || v == guideKeepOwn()) return;
    m_s.setValue(key("guideKeepOwn"), v);
    emit sourceChanged();
}

AppSettings::AppSettings(QObject *parent) : QObject(parent)
{
    migrateSinglePlaylist();
    m_active = m_s.value("playlists/active").toString();
    if (!playlistIds().contains(m_active)) m_active = playlistIds().value(0);
    moveSecretsToStore();
    syncUserAgent();
}

QString AppSettings::key(const QString &id, const char *name) const
{
    return QStringLiteral("playlists/%1/%2").arg(id, QLatin1String(name));
}

QString AppSettings::secretName(const QString &id, const char *name) const
{
    return QLatin1String(name) + QLatin1Char('-') + id;
}

// Before multiple playlists: one source under "source/".
void AppSettings::migrateSinglePlaylist()
{
    if (!m_s.value("playlists/order").toStringList().isEmpty()) return;
    const QString type = m_s.value("source/type").toString();
    if (type.isEmpty()) return;
    const QString id = QStringLiteral("p1");
    auto copy = [&](const char *from, const char *to) {
        if (m_s.contains(from)) m_s.setValue(key(id, to), m_s.value(from));
    };
    m_s.setValue(key(id, "type"), type);
    copy("source/xtreamServer", "xtreamServer");
    copy("source/xtreamUser", "xtreamUser");
    copy("source/xtreamOutput", "xtreamOutput");
    copy("source/m3u", "m3u");
    copy("source/xtreamPassword", "xtreamPassword");
    copy("guide/url", "guideUrl");
    copy("player/lastChannelUrl", "lastChannel");
    copy("multiview/keys", "multiviewKeys");
    // Secrets stored by the previous version under the old names.
    auto moveSecret = [&](const char *flag, const char *oldName, const char *newName, const char *newFlag) {
        if (!m_s.value(flag).toBool()) return;
        bool found = false;
        const QString v = SecretStore::read(QLatin1String(oldName), &found);
        if (found && SecretStore::write(secretName(id, newName), v)) {
            SecretStore::remove(QLatin1String(oldName));
            m_s.setValue(key(id, newFlag), true);
        }
    };
    moveSecret("source/m3uSecure", "playlist", "playlist", "m3uSecure");
    moveSecret("source/xtreamPasswordSecure", "xtream-password", "xtream-password", "xtreamPasswordSecure");

    const QString location = type == QLatin1String("xtream")
        ? m_s.value("source/xtreamServer").toString() : m_s.value("source/m3u").toString();
    QString name = suggestName(location);
    m_s.setValue(key(id, "name"), name.isEmpty() ? tr("My playlist") : name);
    m_s.setValue("playlists/order", QStringList{id});
    m_s.setValue("playlists/active", id);
    m_s.remove("source");
    m_s.remove("guide");
    m_s.remove("player/lastChannelUrl");
    m_s.remove("multiview");
    qInfo() << "Settings: moved the playlist into the playlist list";
}

// Plain-text logins (from older versions, or saved where there was no
// store) move into the credential store.
void AppSettings::moveSecretsToStore()
{
    if (!SecretStore::available()) return;
    // A TMDB key restored from a backup is written plainly; put it in the store.
    const QString tmdb = m_s.value(QStringLiteral("tmdb/key")).toString();
    if (!tmdb.isEmpty() && SecretStore::write(QStringLiteral("tmdb-key"), tmdb)) {
        m_s.remove(QStringLiteral("tmdb/key"));
        m_s.setValue(QStringLiteral("tmdb/keySecure"), true);
    }
    for (const QString &id : playlistIds()) {
        auto move = [&](const char *plain, const char *flag, const char *name) {
            const QString v = m_s.value(key(id, plain)).toString();
            if (v.isEmpty()) return;
            if (SecretStore::write(secretName(id, name), v)) {
                m_s.remove(key(id, plain));
                m_s.setValue(key(id, flag), true);
                qInfo().noquote() << "Settings: moved a playlist login to the system credential store";
            } else {
                qWarning().noquote() << "Settings: couldn't use the system credential store";
            }
        };
        move("m3u", "m3uSecure", "playlist");
        move("xtreamPassword", "xtreamPasswordSecure", "xtream-password");
    }
}

QString AppSettings::secret(const char *plainKey, const char *flag, const char *name) const
{
    return secretFor(m_active, plainKey, flag, name);
}

QString AppSettings::secretFor(const QString &id, const char *plainKey, const char *flag, const char *name) const
{
    if (id.isEmpty()) return QString();
    if (m_s.value(key(id, flag)).toBool()) {
        const QString n = secretName(id, name);
        auto it = m_secretCache.constFind(n);
        if (it != m_secretCache.constEnd()) return it.value();
        const QString v = SecretStore::read(n);
        m_secretCache.insert(n, v);
        return v;
    }
    return m_s.value(key(id, plainKey)).toString();
}

AppSettings::Source AppSettings::source(const QString &id) const
{
    Source src;
    if (!playlistIds().contains(id)) return src;
    src.type = m_s.value(key(id, "type")).toString();
    src.m3u = secretFor(id, "m3u", "m3uSecure", "playlist");
    src.server = m_s.value(key(id, "xtreamServer")).toString();
    src.user = m_s.value(key(id, "xtreamUser")).toString();
    src.password = secretFor(id, "xtreamPassword", "xtreamPasswordSecure", "xtream-password");
    src.output = m_s.value(key(id, "xtreamOutput"), QStringLiteral("ts")).toString();
    return src;
}

void AppSettings::setSecret(const char *plainKey, const char *flag, const char *name, const QString &value)
{
    const QString n = secretName(m_active, name);
    m_secretCache.remove(n);
    if (SecretStore::available() && SecretStore::write(n, value)) {
        m_s.remove(key(plainKey));
        m_s.setValue(key(flag), true);
        m_secretCache.insert(n, value);
    } else {
        m_s.remove(key(flag));
        m_s.setValue(key(plainKey), value);
    }
}

void AppSettings::removeSecrets(const QString &id)
{
    for (const char *name : {"playlist", "xtream-password"}) {
        SecretStore::remove(secretName(id, name));
        m_secretCache.remove(secretName(id, name));
    }
}

// ---- active playlist -------------------------------------------------------------

bool AppSettings::hasSource() const { return !sourceType().isEmpty(); }
QString AppSettings::sourceType() const { return m_active.isEmpty() ? QString() : m_s.value(key("type")).toString(); }
QString AppSettings::m3uLocation() const { return secret("m3u", "m3uSecure", "playlist"); }
QString AppSettings::xtreamServer() const { return m_s.value(key("xtreamServer")).toString(); }
QString AppSettings::xtreamUser() const { return m_s.value(key("xtreamUser")).toString(); }
QString AppSettings::xtreamPassword() const { return secret("xtreamPassword", "xtreamPasswordSecure", "xtream-password"); }
QString AppSettings::xtreamOutput() const { return m_s.value(key("xtreamOutput"), QStringLiteral("ts")).toString(); }

QString AppSettings::epgOverride() const { return m_s.value(key("guideUrl")).toString(); }
void AppSettings::setEpgOverride(const QString &url)
{
    if (m_active.isEmpty() || url.trimmed() == epgOverride()) return;
    m_s.setValue(key("guideUrl"), url.trimmed());
    emit sourceChanged();
}

int AppSettings::pipCorner() const { return qBound(0, m_s.value("pip/corner", 0).toInt(), 3); }
void AppSettings::setPipCorner(int corner)
{
    corner = ((corner % 4) + 4) % 4;
    if (corner == pipCorner()) return;
    m_s.setValue("pip/corner", corner);
    emit pipCornerChanged();
}

QString AppSettings::multiviewLayout() const { return m_s.value("multiview/layout", QStringLiteral("auto")).toString(); }
void AppSettings::setMultiviewLayout(const QString &layout)
{
    if (layout == multiviewLayout()) return;
    m_s.setValue("multiview/layout", layout);
    emit multiviewLayoutChanged();
}

// Multi-view tiles are shared by all playlists ("<playlist id>::<channel key>");
// older versions kept plain keys per playlist.
QStringList AppSettings::multiviewKeys() const
{
    const QStringList global = m_s.value("multiview/tiles").toStringList();
    return global.isEmpty() ? m_s.value(key("multiviewKeys")).toStringList() : global;
}
void AppSettings::setMultiviewKeys(const QStringList &keys)
{
    if (keys == multiviewKeys()) return;
    m_s.setValue("multiview/tiles", keys);
    emit multiviewKeysChanged();
}

QString AppSettings::lastChannelUrl() const { return m_s.value(key("lastChannel")).toString(); }
void AppSettings::setLastChannelUrl(const QString &k)
{
    if (!m_active.isEmpty()) m_s.setValue(key("lastChannel"), k);
}

QString AppSettings::ensureActive()
{
    if (m_active.isEmpty()) setActive(createPlaylist(QString()));
    return m_active;
}

void AppSettings::saveM3u(const QString &location)
{
    ensureActive();
    const QString loc = location.trimmed();
    m_s.setValue(key("type"), QStringLiteral("m3u"));
    for (const char *k : {"xtreamServer", "xtreamUser", "xtreamOutput", "xtreamPassword", "xtreamPasswordSecure"})
        m_s.remove(key(k));
    SecretStore::remove(secretName(m_active, "xtream-password"));
    m_secretCache.remove(secretName(m_active, "xtream-password"));
    setSecret("m3u", "m3uSecure", "playlist", loc);
    if (playlistName(m_active).isEmpty()) m_s.setValue(key("name"), suggestName(loc));
    emit sourceChanged();
    emit playlistsChanged();
}

void AppSettings::saveXtream(const QString &server, const QString &user,
                             const QString &password, const QString &output)
{
    ensureActive();
    m_s.setValue(key("type"), QStringLiteral("xtream"));
    m_s.setValue(key("xtreamServer"), server.trimmed());
    m_s.setValue(key("xtreamUser"), user);
    m_s.setValue(key("xtreamOutput"), output);
    m_s.remove(key("m3u"));
    m_s.remove(key("m3uSecure"));
    SecretStore::remove(secretName(m_active, "playlist"));
    m_secretCache.remove(secretName(m_active, "playlist"));
    setSecret("xtreamPassword", "xtreamPasswordSecure", "xtream-password", password);
    if (playlistName(m_active).isEmpty()) m_s.setValue(key("name"), suggestName(server));
    emit sourceChanged();
    emit playlistsChanged();
}

void AppSettings::clearSource()
{
    if (!m_active.isEmpty()) removePlaylist(m_active);
}

// ---- the list ------------------------------------------------------------------------

QStringList AppSettings::playlistIds() const { return m_s.value("playlists/order").toStringList(); }

QString AppSettings::playlistName(const QString &id) const
{
    return id.isEmpty() ? QString() : m_s.value(key(id, "name")).toString();
}

QVariantList AppSettings::playlists() const
{
    QVariantList out;
    for (const QString &id : playlistIds()) {
        const QString type = m_s.value(key(id, "type")).toString();
        QString detail;
        if (type == QLatin1String("xtream"))
            detail = QStringLiteral("Xtream · %1").arg(QUrl::fromUserInput(m_s.value(key(id, "xtreamServer")).toString()).host());
        else if (type == QLatin1String("m3u"))
            detail = QStringLiteral("M3U");
        out.append(QVariantMap{{"id", id}, {"name", playlistName(id)}, {"type", type},
                               {"detail", detail}, {"active", id == m_active}});
    }
    return out;
}

QString AppSettings::suggestName(const QString &location)
{
    const QString loc = location.trimmed();
    if (loc.isEmpty()) return QString();
    const QUrl url = QUrl::fromUserInput(loc);
    if (url.isLocalFile()) return QFileInfo(url.toLocalFile()).completeBaseName();
    QString host = url.host();
    if (host.startsWith(QLatin1String("www."))) host = host.mid(4);
    return host;
}

QString AppSettings::createPlaylist(const QString &name)
{
    QStringList ids = playlistIds();
    int n = 1;
    while (ids.contains(QStringLiteral("p%1").arg(n))) ++n;
    const QString id = QStringLiteral("p%1").arg(n);
    ids.append(id);
    m_s.setValue("playlists/order", ids);
    m_s.setValue(key(id, "name"), name.trimmed());
    emit playlistsChanged();
    return id;
}

void AppSettings::setActive(const QString &id)
{
    if (id == m_active) return;
    m_active = id;
    m_s.setValue("playlists/active", id);
    syncUserAgent();
    emit sourceChanged();
    emit multiviewKeysChanged();
    emit playlistsChanged();
}

void AppSettings::removePlaylist(const QString &id)
{
    QStringList ids = playlistIds();
    if (!ids.removeOne(id)) return;
    removeSecrets(id);
    m_s.remove(QStringLiteral("playlists/") + id);
    m_s.setValue("playlists/order", ids);
    if (id == m_active) {
        m_active.clear();
        m_s.setValue("playlists/active", QString());
        emit sourceChanged();
    }
    emit playlistsChanged();
}

void AppSettings::renamePlaylist(const QString &id, const QString &name)
{
    if (!playlistIds().contains(id) || name.trimmed().isEmpty() || name.trimmed() == playlistName(id)) return;
    m_s.setValue(key(id, "name"), name.trimmed());
    emit playlistsChanged();
}

void AppSettings::movePlaylist(const QString &id, int delta)
{
    QStringList ids = playlistIds();
    const int i = ids.indexOf(id);
    const int j = i + delta;
    if (i < 0 || j < 0 || j >= ids.size()) return;
    ids.move(i, j);
    m_s.setValue("playlists/order", ids);
    emit playlistsChanged();
}

// ---- global ---------------------------------------------------------------------------

int AppSettings::timeshiftMb() const { return m_s.value("player/timeshiftMb", 2048).toInt(); }

void AppSettings::setTimeshiftMb(int mb)
{
    if (mb == timeshiftMb()) return;
    m_s.setValue("player/timeshiftMb", mb);
    emit timeshiftMbChanged();
}

#define RELAY_OPTION(Type, getter, Setter, keyName, def, conv, clamp) \
    Type AppSettings::getter() const { Type v = m_s.value(keyName, def).conv(); return clamp; } \
    void AppSettings::Setter(Type v) \
    { \
        v = clamp; \
        if (v == getter()) return; \
        m_s.setValue(keyName, v); \
        emit optionsChanged(); \
    }

using OptString = QString;
RELAY_OPTION(OptString, pictureShape, setPictureShape, "player/pictureShape", QStringLiteral("auto"), toString, v)
RELAY_OPTION(OptString, deinterlace, setDeinterlace, "player/deinterlace", QStringLiteral("auto"), toString, v)
RELAY_OPTION(OptString, audioLanguage, setAudioLanguage, "player/audioLanguage", QString(), toString, v.trimmed().toLower())
RELAY_OPTION(OptString, subtitleLanguage, setSubtitleLanguage, "player/subtitleLanguage", QString(), toString, v.trimmed().toLower())
RELAY_OPTION(bool, hardwareDecoding, setHardwareDecoding, "player/hardwareDecoding", true, toBool, v)
RELAY_OPTION(int, audioDelayMs, setAudioDelayMs, "player/audioDelayMs", 0, toInt, qBound(-2000, v, 2000))
RELAY_OPTION(int, guideRefreshHours, setGuideRefreshHours, "guide/refreshHours", 12, toInt, qBound(1, v, 168))
RELAY_OPTION(int, guideDaysAhead, setGuideDaysAhead, "guide/daysAhead", 8, toInt, qBound(1, v, 14))
RELAY_OPTION(int, playlistRefreshHours, setPlaylistRefreshHours, "playlists/refreshHours", 24, toInt, qBound(0, v, 168))
RELAY_OPTION(int, uiScale, setUiScale, "ui/scale", 100, toInt, qBound(75, v, 200))
RELAY_OPTION(OptString, externalPlayer, setExternalPlayer, "player/external", QString(), toString,
             (v.startsWith(QLatin1String("file:")) ? QUrl(v).toLocalFile() : v.trimmed()))

QVariantMap AppSettings::keys() const
{
    QVariantMap out;
    QSettings &s = const_cast<QSettings &>(m_s);
    s.beginGroup(QStringLiteral("keys"));
    for (const QString &action : s.childKeys())
        if (s.value(action).toInt() > 0) out.insert(action, s.value(action).toInt());
    s.endGroup();
    return out;
}

void AppSettings::setKey(const QString &action, int key)
{
    if (action.isEmpty()) return;
    if (key > 0) m_s.setValue(QStringLiteral("keys/") + action, key);
    else m_s.remove(QStringLiteral("keys/") + action);
    emit keysChanged();
}

QString AppSettings::tmdbKey() const
{
    if (m_s.value(QStringLiteral("tmdb/keySecure")).toBool()) {
        auto it = m_secretCache.constFind(QStringLiteral("tmdb-key"));
        if (it != m_secretCache.constEnd()) return it.value();
        const QString v = SecretStore::read(QStringLiteral("tmdb-key"));
        m_secretCache.insert(QStringLiteral("tmdb-key"), v);
        return v;
    }
    return m_s.value(QStringLiteral("tmdb/key")).toString();
}

void AppSettings::setTmdbKey(const QString &keyIn)
{
    const QString k = keyIn.trimmed();
    if (k == tmdbKey()) return;
    m_secretCache.remove(QStringLiteral("tmdb-key"));
    SecretStore::remove(QStringLiteral("tmdb-key"));
    m_s.remove(QStringLiteral("tmdb/keySecure"));
    m_s.remove(QStringLiteral("tmdb/key"));
    if (!k.isEmpty()) {
        if (SecretStore::available() && SecretStore::write(QStringLiteral("tmdb-key"), k)) {
            m_s.setValue(QStringLiteral("tmdb/keySecure"), true);
            m_secretCache.insert(QStringLiteral("tmdb-key"), k);
        } else {
            m_s.setValue(QStringLiteral("tmdb/key"), k);
        }
    }
    emit tmdbKeyChanged();
}

void AppSettings::resetKeys()
{
    m_s.remove(QStringLiteral("keys"));
    emit keysChanged();
}

QString AppSettings::userAgentFor(const QString &id) const
{
    return id.isEmpty() ? QString() : m_s.value(key(id, "userAgent")).toString();
}

void AppSettings::setUserAgent(const QString &v)
{
    if (m_active.isEmpty() || v.trimmed() == userAgent()) return;
    m_s.setValue(key("userAgent"), v.trimmed());
    syncUserAgent();
    emit sourceChanged();
}

int AppSettings::guideOffsetMinutes() const
{
    return m_active.isEmpty() ? 0 : qBound(-720, m_s.value(key("guideOffsetMinutes"), 0).toInt(), 720);
}

void AppSettings::setGuideOffsetMinutes(int v)
{
    v = qBound(-720, v, 720);
    if (m_active.isEmpty() || v == guideOffsetMinutes()) return;
    m_s.setValue(key("guideOffsetMinutes"), v);
    emit sourceChanged();
}

QStringList AppSettings::splitAddresses(const QString &text)
{
    QStringList out;
    // New lines always separate addresses; commas and semicolons only when
    // another address follows (they can be part of an address's query).
    const QStringList lines = text.split(QRegularExpression(QStringLiteral("[\\r\\n]+|[,;]\\s*(?=(?:https?://|file:|[A-Za-z]:[\\\\/]|/))")), Qt::SkipEmptyParts);
    for (const QString &l : lines) {
        const QString t = l.trimmed();
        if (!t.isEmpty() && !out.contains(t)) out << t;
    }
    return out;
}
