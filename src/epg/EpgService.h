#pragma once

#include "Programme.h"
#include "sources/Channel.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QVariantList>

#include <atomic>
#include <memory>
#include <vector>

class EpgStore;
class QNetworkAccessManager;
class QTemporaryFile;

// Owns the TV guide: downloads XMLTV, imports it on a worker thread into
// SQLite, maps playlist channels to guide channels, and answers now/next
// questions quickly from a small cache.
class EpgService : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(QString status READ status NOTIFY stateChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY stateChanged)
    Q_PROPERTY(bool hasGuide READ hasGuide NOTIFY guideUpdated)
    Q_PROPERTY(int programmeCount READ programmeCount NOTIFY guideUpdated)
    Q_PROPERTY(QDateTime lastUpdated READ lastUpdated NOTIFY guideUpdated)

public:
    struct NowNext {
        bool hasNow = false;
        bool hasNext = false;
        Programme now;
        Programme next;
    };

    // What the importer keeps: guide channels matching these playlist
    // tvg-ids or normalized names. Everything else in the file is skipped.
    struct Wanted {
        QSet<QString> tvgIds;   // lowercase
        QSet<QString> names;    // EpgMatcher::normalize()
    };

    explicit EpgService(const QString &databasePath, QObject *parent = nullptr);
    ~EpgService() override;

    bool busy() const { return m_busy; }
    QString status() const { return m_status; }
    QString lastError() const { return m_lastError; }
    bool hasGuide() const { return m_programmeCount > 0; }
    int programmeCount() const { return m_programmeCount; }
    QDateTime lastUpdated() const { return m_lastUpdated; }

    // Imports the guide files at `urls` (http(s), file:// or local paths;
    // earlier ones win when two list the same channel) for these channels,
    // unless the stored guide is already fresh for the same sources and
    // channels. `extraIds` are guide channels to keep whatever the playlist
    // says (listings chosen by hand).
    void refresh(const QVector<Channel> &channels, const QStringList &urls, bool force,
                 const QSet<QString> &extraIds = {});
    // How often refresh() re-downloads, how far ahead listings are kept, and
    // minutes to move every listing by (shown times, not stored ones).
    void setRefreshHours(int hours) { m_refreshAfter = qMax(1, hours) * 3600; }
    void setDaysAhead(int days) { m_keepFuture = qint64(qBound(1, days, 14)) * 24 * 3600; }
    void setOffsetMinutes(int minutes);
    Q_INVOKABLE void cancel();
    // Closes the database. Call while the application object still exists
    // (main() wires it to aboutToQuit); afterwards every query returns nothing.
    void shutdown();
    // Use another guide database (another playlist's). Stops any update.
    void setDatabase(const QString &path);

    // Guide channel id for each playlist channel ("" when there's no match).
    // `overrides` (by position) are listings chosen by hand: a guide channel
    // id, or "-" for none. `offsets` (by position, minutes) move a channel's
    // listings; they travel inside the returned id (see withOffset).
    QVector<QString> resolve(const QVector<Channel> &channels,
                             const QHash<int, QString> &overrides = {},
                             const QHash<int, int> &offsets = {}) const;
    static QString withOffset(const QString &id, int minutes);
    static QString baseId(const QString &epgId);
    qint64 offsetSecs(const QString &epgId) const;   // the playlist's plus the channel's
    bool hasChannel(const QString &id) const;        // listings stored for this guide channel
    // Guide channels to pick from: [{id, name}] matching `text`.
    Q_INVOKABLE QVariantList guideChannels(const QString &text, int limit = 200) const;

    NowNext nowNext(const QString &epgId) const;
    QVector<Programme> range(const QString &epgId, qint64 from, qint64 to) const;
    QVector<Programme> search(const QString &text, int limit) const;
    // For the guide grid: programmes overlapping [fromMs, toMs) as a list of
    // {title, subTitle, desc, category, episode, start, stop} (ms since epoch).
    Q_INVOKABLE QVariantList programmes(const QString &epgId, double fromMs, double toMs) const;

    static Wanted wantedFor(const QVector<Channel> &channels);
    static QString fingerprint(const QVector<Channel> &channels, const QString &url);
    static QString fingerprint(const QVector<Channel> &channels, const QStringList &urls,
                               const QSet<QString> &extraIds);

signals:
    void stateChanged();
    void guideUpdated();
    void importFailed(const QString &message);

private:
    struct ImportResult {
        bool ok = false;
        int programmes = 0;
        QString error;
        QString warning;
    };
    static ImportResult runImport(const QStringList &files, const QString &databasePath,
                                  const Wanted &wanted, const QHash<QString, QString> &meta,
                                  qint64 keepFuture,
                                  std::shared_ptr<std::atomic_bool> cancelled,
                                  const std::function<void(int)> &progress);

    void startImport(const QStringList &files, const Wanted &wanted,
                     const QHash<QString, QString> &meta);
    // Fetches sources[index…] one after another, then imports what arrived.
    void fetchNext(quint64 gen, const QStringList &sources, int index, QStringList files,
                   QStringList problems, const Wanted &wanted, const QHash<QString, QString> &meta);
    void setState(bool busy, const QString &status);
    void finish(const ImportResult &result);
    void reloadStats();

    QString m_dbPath;
    std::unique_ptr<EpgStore> m_store;     // main-thread reader
    QNetworkAccessManager *m_net = nullptr;
    std::vector<std::unique_ptr<QTemporaryFile>> m_downloads;
    qint64 m_refreshAfter = 12 * 3600;
    qint64 m_keepFuture = 8 * 24 * 3600;
    qint64 m_offsetSecs = 0;
    std::shared_ptr<std::atomic_bool> m_cancelled;
    quint64 m_generation = 0;
    QString m_pendingFingerprint;
    QElapsedTimer m_importClock;

    bool m_busy = false;
    QString m_status;
    QString m_lastError;
    int m_programmeCount = 0;
    QDateTime m_lastUpdated;

    struct CacheEntry { NowNext value; qint64 validUntil = 0; };
    mutable QHash<QString, CacheEntry> m_cache;
};
