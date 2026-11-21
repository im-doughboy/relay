#include "EpgStore.h"

#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace {

constexpr int kSchemaVersion = 1;
constexpr qint64 kMaxProgrammeLength = 24 * 3600;   // for range queries
constexpr qint64 kFallbackLength = 3600;            // when a programme has no end time

Programme fromRow(const QSqlQuery &q, const QString &channel)
{
    Programme p;
    p.channel = channel;
    p.start = q.value(0).toLongLong();
    p.stop = q.value(1).toLongLong();
    p.title = q.value(2).toString();
    p.subTitle = q.value(3).toString();
    p.desc = q.value(4).toString();
    p.category = q.value(5).toString();
    p.episode = q.value(6).toString();
    return p;
}

} // namespace

EpgStore::EpgStore(const QString &path, const QString &connectionName)
    : m_connection(connectionName)
{
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connection);
    db.setDatabaseName(path);
    if (!db.open()) {
        m_error = db.lastError().text();
        return;
    }
    m_open = true;
    exec(QStringLiteral("PRAGMA journal_mode=WAL"));
    exec(QStringLiteral("PRAGMA synchronous=NORMAL"));
    exec(QStringLiteral("PRAGMA busy_timeout=5000"));
    createSchema();
}

EpgStore::~EpgStore()
{
    m_insertProgramme.reset();
    m_insertChannel.reset();
    m_insertName.reset();
    m_insertListed.reset();
    {
        QSqlDatabase db = QSqlDatabase::database(m_connection, false);
        if (db.isOpen()) db.close();
    }
    QSqlDatabase::removeDatabase(m_connection);
}

bool EpgStore::exec(const QString &sql) const
{
    QSqlQuery q(QSqlDatabase::database(m_connection, false));
    if (!q.exec(sql)) {
        m_error = q.lastError().text();
        return false;
    }
    return true;
}

void EpgStore::createSchema()
{
    exec(QStringLiteral("CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY, value TEXT)"));
    if (meta(QStringLiteral("schema")).toInt() != kSchemaVersion) {
        exec(QStringLiteral("DROP TABLE IF EXISTS programmes"));
        exec(QStringLiteral("DROP TABLE IF EXISTS epg_channels"));
        exec(QStringLiteral("DROP TABLE IF EXISTS epg_names"));
        exec(QStringLiteral("DELETE FROM meta"));
    }
    exec(QStringLiteral("CREATE TABLE IF NOT EXISTS epg_channels("
                        "id TEXT PRIMARY KEY, name TEXT, icon TEXT)"));
    exec(QStringLiteral("CREATE TABLE IF NOT EXISTS epg_names(norm TEXT, id TEXT)"));
    exec(QStringLiteral("CREATE TABLE IF NOT EXISTS epg_listed(id TEXT PRIMARY KEY, name TEXT, folded TEXT)"));
    exec(QStringLiteral("CREATE TABLE IF NOT EXISTS programmes("
                        "channel TEXT NOT NULL, start INTEGER NOT NULL, stop INTEGER NOT NULL,"
                        "title TEXT, subtitle TEXT, descr TEXT, category TEXT, episode TEXT)"));
    exec(QStringLiteral("CREATE INDEX IF NOT EXISTS idx_programmes ON programmes(channel, start)"));
    exec(QStringLiteral("INSERT OR REPLACE INTO meta(key, value) VALUES('schema', '%1')")
             .arg(kSchemaVersion));
}

// ---- import -------------------------------------------------------------------------

bool EpgStore::beginImport()
{
    if (!m_open) return false;
    QSqlDatabase db = QSqlDatabase::database(m_connection, false);
    if (!db.transaction()) {
        m_error = db.lastError().text();
        return false;
    }
    // Dropping the index during a bulk insert and rebuilding it once is much
    // faster than maintaining it row by row.
    exec(QStringLiteral("DROP INDEX IF EXISTS idx_programmes"));
    exec(QStringLiteral("DELETE FROM programmes"));
    exec(QStringLiteral("DELETE FROM epg_channels"));
    exec(QStringLiteral("DELETE FROM epg_names"));
    exec(QStringLiteral("DELETE FROM epg_listed"));

    m_insertProgramme = std::make_unique<QSqlQuery>(db);
    m_insertProgramme->prepare(QStringLiteral(
        "INSERT INTO programmes(channel, start, stop, title, subtitle, descr, category, episode) "
        "VALUES(?, ?, ?, ?, ?, ?, ?, ?)"));
    m_insertChannel = std::make_unique<QSqlQuery>(db);
    m_insertChannel->prepare(QStringLiteral(
        "INSERT OR REPLACE INTO epg_channels(id, name, icon) VALUES(?, ?, ?)"));
    m_insertName = std::make_unique<QSqlQuery>(db);
    m_insertName->prepare(QStringLiteral("INSERT INTO epg_names(norm, id) VALUES(?, ?)"));
    m_insertListed = std::make_unique<QSqlQuery>(db);
    m_insertListed->prepare(QStringLiteral("INSERT OR IGNORE INTO epg_listed(id, name, folded) VALUES(?, ?, ?)"));
    return true;
}

void EpgStore::addChannel(const EpgChannel &c, const QStringList &normalizedNames)
{
    if (!m_insertChannel) return;
    m_insertChannel->addBindValue(c.id);
    m_insertChannel->addBindValue(c.names.value(0));
    m_insertChannel->addBindValue(c.icon);
    m_insertChannel->exec();
    for (const QString &n : normalizedNames) {
        if (n.isEmpty()) continue;
        m_insertName->addBindValue(n);
        m_insertName->addBindValue(c.id);
        m_insertName->exec();
    }
}

void EpgStore::addListed(const QString &id, const QString &name)
{
    if (!m_insertListed || id.isEmpty()) return;
    m_insertListed->addBindValue(id);
    m_insertListed->addBindValue(name);
    m_insertListed->addBindValue((id + QLatin1Char(' ') + name).toLower());
    m_insertListed->exec();
}

void EpgStore::addProgramme(const Programme &p)
{
    if (!m_insertProgramme) return;
    m_insertProgramme->addBindValue(p.channel);
    m_insertProgramme->addBindValue(p.start);
    m_insertProgramme->addBindValue(p.stop);
    m_insertProgramme->addBindValue(p.title);
    m_insertProgramme->addBindValue(p.subTitle);
    m_insertProgramme->addBindValue(p.desc);
    m_insertProgramme->addBindValue(p.category);
    m_insertProgramme->addBindValue(p.episode);
    m_insertProgramme->exec();
}

bool EpgStore::commitImport(const QHash<QString, QString> &metaValues)
{
    if (!m_insertProgramme) return false;
    m_insertProgramme.reset();
    m_insertChannel.reset();
    m_insertName.reset();
    m_insertListed.reset();

    exec(QStringLiteral("CREATE INDEX IF NOT EXISTS idx_programmes ON programmes(channel, start)"));
    // Programmes without an end time end when the next one on that channel starts.
    exec(QStringLiteral(
        "UPDATE programmes SET stop = (SELECT MIN(p2.start) FROM programmes p2 "
        "WHERE p2.channel = programmes.channel AND p2.start > programmes.start) "
        "WHERE stop = 0"));
    exec(QStringLiteral("UPDATE programmes SET stop = start + %1 WHERE stop IS NULL OR stop = 0")
             .arg(kFallbackLength));

    QSqlDatabase db = QSqlDatabase::database(m_connection, false);
    QSqlQuery q(db);
    q.prepare(QStringLiteral("INSERT OR REPLACE INTO meta(key, value) VALUES(?, ?)"));
    for (auto it = metaValues.constBegin(); it != metaValues.constEnd(); ++it) {
        q.addBindValue(it.key());
        q.addBindValue(it.value());
        q.exec();
    }
    q.finish();

    if (!db.commit()) {
        m_error = db.lastError().text();
        db.rollback();
        return false;
    }
    return true;
}

void EpgStore::rollbackImport()
{
    m_insertProgramme.reset();
    m_insertChannel.reset();
    m_insertName.reset();
    m_insertListed.reset();
    QSqlDatabase db = QSqlDatabase::database(m_connection, false);
    db.rollback();
    exec(QStringLiteral("CREATE INDEX IF NOT EXISTS idx_programmes ON programmes(channel, start)"));
}

// ---- queries --------------------------------------------------------------------------

QString EpgStore::meta(const QString &key) const
{
    QSqlQuery q(QSqlDatabase::database(m_connection, false));
    q.prepare(QStringLiteral("SELECT value FROM meta WHERE key = ?"));
    q.addBindValue(key);
    return (q.exec() && q.next()) ? q.value(0).toString() : QString();
}

bool EpgStore::hasChannel(const QString &id) const
{
    QSqlQuery q(QSqlDatabase::database(m_connection, false));
    q.prepare(QStringLiteral("SELECT 1 FROM epg_channels WHERE id = ?"));
    q.addBindValue(id);
    return q.exec() && q.next();
}

QVector<QPair<QString, QString>> EpgStore::listed(const QString &text, int limit) const
{
    QVector<QPair<QString, QString>> out;
    QString sql = QStringLiteral("SELECT id, name FROM epg_listed");
    QStringList words = text.toLower().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    for (int i = 0; i < words.size(); ++i)
        sql += (i == 0 ? QStringLiteral(" WHERE") : QStringLiteral(" AND")) + QStringLiteral(" folded LIKE ? ESCAPE '!'");
    sql += QStringLiteral(" ORDER BY name COLLATE NOCASE, id LIMIT ?");
    QSqlQuery q(QSqlDatabase::database(m_connection, false));
    q.prepare(sql);
    for (QString w : words) {
        w.replace(QLatin1Char('!'), QLatin1String("!!")).replace(QLatin1Char('%'), QLatin1String("!%"))
            .replace(QLatin1Char('_'), QLatin1String("!_"));
        q.addBindValue(QLatin1Char('%') + w + QLatin1Char('%'));
    }
    q.addBindValue(limit);
    if (q.exec())
        while (q.next()) out.append({q.value(0).toString(), q.value(1).toString()});
    return out;
}

QHash<QString, QString> EpgStore::channelIdsByLowercase() const
{
    QHash<QString, QString> map;
    QSqlQuery q(QSqlDatabase::database(m_connection, false));
    if (q.exec(QStringLiteral("SELECT id FROM epg_channels")))
        while (q.next()) {
            const QString id = q.value(0).toString();
            map.insert(id.toLower(), id);
        }
    return map;
}

QHash<QString, QString> EpgStore::channelIdsByName() const
{
    QHash<QString, QString> map;
    QSqlQuery q(QSqlDatabase::database(m_connection, false));
    if (q.exec(QStringLiteral("SELECT norm, id FROM epg_names")))
        while (q.next()) {
            const QString norm = q.value(0).toString();
            if (!map.contains(norm)) map.insert(norm, q.value(1).toString());
        }
    return map;
}

QVector<Programme> EpgStore::upcoming(const QString &channel, qint64 now, int limit) const
{
    QVector<Programme> out;
    QSqlQuery q(QSqlDatabase::database(m_connection, false));
    q.prepare(QStringLiteral(
        "SELECT start, stop, title, subtitle, descr, category, episode FROM programmes "
        "WHERE channel = ? AND start >= ? AND stop > ? ORDER BY start LIMIT ?"));
    q.addBindValue(channel);
    q.addBindValue(now - kMaxProgrammeLength);
    q.addBindValue(now);
    q.addBindValue(limit);
    if (q.exec())
        while (q.next()) out.append(fromRow(q, channel));
    return out;
}

QVector<Programme> EpgStore::range(const QString &channel, qint64 from, qint64 to) const
{
    QVector<Programme> out;
    QSqlQuery q(QSqlDatabase::database(m_connection, false));
    q.prepare(QStringLiteral(
        "SELECT start, stop, title, subtitle, descr, category, episode FROM programmes "
        "WHERE channel = ? AND start >= ? AND start < ? AND stop > ? ORDER BY start"));
    q.addBindValue(channel);
    q.addBindValue(from - kMaxProgrammeLength);
    q.addBindValue(to);
    q.addBindValue(from);
    if (q.exec())
        while (q.next()) out.append(fromRow(q, channel));
    return out;
}

QVector<Programme> EpgStore::search(const QString &text, qint64 now, int limit) const
{
    QVector<Programme> out;
    // LIKE wildcards in the user's text are matched literally ('!' escapes).
    QString pattern = text;
    pattern.replace(QStringLiteral("!"), QStringLiteral("!!"))
           .replace(QStringLiteral("%"), QStringLiteral("!%"))
           .replace(QStringLiteral("_"), QStringLiteral("!_"));
    QSqlQuery q(QSqlDatabase::database(m_connection, false));
    q.prepare(QStringLiteral(
        "SELECT start, stop, title, subtitle, descr, category, episode, channel FROM programmes "
        "WHERE stop > ? AND title LIKE ? ESCAPE '!' "
        "ORDER BY (start > ?), start LIMIT ?"));
    q.addBindValue(now);
    q.addBindValue(QStringLiteral("%") + pattern + QStringLiteral("%"));
    q.addBindValue(now);
    q.addBindValue(limit);
    if (q.exec())
        while (q.next()) out.append(fromRow(q, q.value(7).toString()));
    return out;
}

int EpgStore::programmeCount() const
{
    QSqlQuery q(QSqlDatabase::database(m_connection, false));
    return (q.exec(QStringLiteral("SELECT COUNT(*) FROM programmes")) && q.next())
        ? q.value(0).toInt() : 0;
}
