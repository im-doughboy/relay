#pragma once

#include <QObject>
#include <QString>

class AppSettings;
class UserLibrary;

// Backup and restore: one file with the settings, every playlist (with its
// login), favorites, hidden and locked channels, custom names and order,
// guide choices, Continue watching, reminders and scheduled recordings.
//
// Restoring is done in two steps so nothing that's open gets overwritten:
// restoreFrom() checks the file and sets it aside, Relay restarts, and
// applyPending() (first thing in main) puts everything in place.
class Backup : public QObject
{
    Q_OBJECT
public:
    Backup(AppSettings *settings, UserLibrary *library, const QString &dataDir, QObject *parent = nullptr);

    // Both return "" on success, or what went wrong. `file` may be a URL
    // from a file dialog or a plain path.
    Q_INVOKABLE QString saveTo(const QString &file);
    Q_INVOKABLE QString restoreFrom(const QString &file);
    // What a backup file holds, for the "Restore?" question: {ok, created,
    // playlists (names), error}.
    Q_INVOKABLE QVariantMap describe(const QString &file) const;
    // Close Relay and start it again (after restoreFrom).
    Q_INVOKABLE void restart();
    Q_INVOKABLE QString suggestedName() const;

    // Returns true when a restore was waiting and has been applied.
    static bool applyPending(const QString &dataDir);

private:
    static QString localPath(const QString &file);
    AppSettings *m_settings;
    UserLibrary *m_library;
    QString m_dataDir;
};
