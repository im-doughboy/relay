#pragma once

#include "sources/Channel.h"

#include <QHash>
#include <QObject>
#include <QSet>
#include <QVariantList>
#include <QVariantMap>

class AppSettings;
class PlaylistService;

// Channel lists of playlists other than the one being watched, for
// multi-view tiles from another playlist. Loaded on first use (in the
// background, without switching playlists) and kept for the session.
// Channels hidden in that playlist are left out; its parental locks are
// read from its own library so they still apply.
class OtherPlaylists : public QObject
{
    Q_OBJECT
    // Bumped when a list finishes loading (or fails), for QML bindings.
    Q_PROPERTY(int revision READ revision NOTIFY changed)

public:
    OtherPlaylists(AppSettings *settings, const QString &dataDir, QObject *parent = nullptr);

    int revision() const { return m_revision; }

    // Start loading a playlist's channels if they aren't here yet.
    Q_INVOKABLE void ensure(const QString &playlistId);
    // "", "loading", "ready" or "failed".
    Q_INVOKABLE QString state(const QString &playlistId) const;
    Q_INVOKABLE QString error(const QString &playlistId) const;
    // [{key, number, name, group, logo, locked}]; empty query = all (up to limit).
    Q_INVOKABLE QVariantList channels(const QString &playlistId, const QString &query, int limit = 300) const;
    // {key, number, name, group, logo, url, userAgent, referrer, locked}; {} if unknown.
    Q_INVOKABLE QVariantMap info(const QString &playlistId, const QString &key) const;
    // Key of the next/previous channel in that playlist, skipping `skip`
    // keys (and locked ones when skipLocked). "" if there's none.
    Q_INVOKABLE QString step(const QString &playlistId, const QString &key, int delta,
                             const QStringList &skip, bool skipLocked) const;
    Q_INVOKABLE bool isLocked(const QString &playlistId, const QString &key) const;

    // Forget a playlist's list (it was edited or removed).
    void forget(const QString &playlistId);

signals:
    void changed();

private:
    struct Entry {
        QString state, error;
        QVector<Channel> channels;
        QStringList folded;
        QHash<QString, int> byKey;
        QSet<QString> lockedChannels, lockedGroups;
    };
    void readLibrary(const QString &id, Entry &e) const;
    QVariantMap toMap(const Entry &e, int i, bool withUrl) const;

    AppSettings *m_settings;
    QString m_dataDir;
    QHash<QString, Entry> m_entries;
    QHash<QString, PlaylistService *> m_loaders;
    int m_revision = 0;
};
