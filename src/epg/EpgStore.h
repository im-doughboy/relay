#pragma once

#include "Programme.h"

#include <QHash>
#include <QPair>
#include <QString>
#include <QVector>

#include <memory>

class QSqlQuery;

// SQLite storage for the guide. Each instance owns its own database
// connection, so the importer (worker thread) and the UI (main thread) each
// create one. WAL mode lets the UI keep reading while an import is running;
// it sees the new guide the moment the import commits.
class EpgStore
{
public:
    EpgStore(const QString &path, const QString &connectionName);
    ~EpgStore();

    EpgStore(const EpgStore &) = delete;
    EpgStore &operator=(const EpgStore &) = delete;

    bool isOpen() const { return m_open; }
    QString errorString() const { return m_error; }

    // ---- import (replaces the whole guide atomically) ----
    bool beginImport();
    void addChannel(const EpgChannel &channel, const QStringList &normalizedNames);
    void addProgramme(const Programme &programme);
    // Every channel the guide files list (kept or not), so listings can be
    // picked by hand.
    void addListed(const QString &id, const QString &name);
    bool commitImport(const QHash<QString, QString> &meta);
    void rollbackImport();

    // ---- queries ----
    QString meta(const QString &key) const;
    QHash<QString, QString> channelIdsByLowercase() const;   // lower(id) -> id
    QHash<QString, QString> channelIdsByName() const;        // normalized name -> id
    // Up to `limit` programmes still running or upcoming at `now`, in order.
    QVector<Programme> upcoming(const QString &channel, qint64 now, int limit = 2) const;
    // Programmes overlapping [from, to), for the guide grid.
    QVector<Programme> range(const QString &channel, qint64 from, qint64 to) const;
    int programmeCount() const;
    bool hasChannel(const QString &id) const;
    // Guide channels whose id or name contains every word of `text`
    // (all of them, up to `limit`, when it's empty): (id, name) pairs.
    QVector<QPair<QString, QString>> listed(const QString &text, int limit) const;
    // Titles containing `text` (case-insensitive for ASCII), still airing
    // or upcoming at `now`; airing ones first, then by start time.
    QVector<Programme> search(const QString &text, qint64 now, int limit) const;

private:
    bool exec(const QString &sql) const;
    void createSchema();

    QString m_connection;
    bool m_open = false;
    mutable QString m_error;
    std::unique_ptr<QSqlQuery> m_insertProgramme;
    std::unique_ptr<QSqlQuery> m_insertChannel;
    std::unique_ptr<QSqlQuery> m_insertName;
    std::unique_ptr<QSqlQuery> m_insertListed;
};
