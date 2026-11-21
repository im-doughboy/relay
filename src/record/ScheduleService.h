#pragma once

#include <QHash>
#include <QObject>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>

class ChannelModel;
class EpgService;
class Recorder;
class QNetworkAccessManager;

// Reminders and recordings. Kept in schedule.json in the app data folder.
//
// A reminder fires a minute before its show starts. A recording starts a
// minute before and stops two minutes after its show (shows rarely run to
// the second), saving to the recordings folder. Finished recordings stay in
// the list until deleted.
class ScheduleService : public QObject
{
    Q_OBJECT
    // All entries, newest first. Each: id, type ("reminder" | "recording"),
    // status ("scheduled" | "recording" | "done" | "failed" | "missed"),
    // title, channelName, channelKey, start, stop (ms), file, bytes, error.
    Q_PROPERTY(QVariantList items READ items NOTIFY changed)
    Q_PROPERTY(int recordingCount READ recordingCount NOTIFY changed)   // recording right now
    // Bumped on every change, so QML bindings that call markFor() update.
    Q_PROPERTY(int revision READ revision NOTIFY changed)
    Q_PROPERTY(QString folder READ folder WRITE setFolder NOTIFY folderChanged)
    // "Record every episode" rules: [{id, title, channelName, playlistId,
    // upcoming (scheduled recordings), next (ms, 0 = none listed)}].
    Q_PROPERTY(QVariantList series READ series NOTIFY changed)

public:
    ScheduleService(ChannelModel *channels, EpgService *epg, const QString &dataDir,
                    const QString &defaultFolder, const QString &playlistId = QString(),
                    QObject *parent = nullptr);
    // The open playlist. Entries belong to the playlist they were made in;
    // a recording due while another playlist is open is marked missed.
    void setPlaylist(const QString &id);
    ~ScheduleService() override;

    QVariantList items() const;
    int recordingCount() const;
    int revision() const { return m_revision; }
    QString folder() const { return m_folder; }
    void setFolder(const QString &folder);

    // programme: {title, start, stop} in ms, as the guide gives them.
    Q_INVOKABLE QString addReminder(int globalIndex, const QVariantMap &programme);
    Q_INVOKABLE QString addRecording(int globalIndex, const QVariantMap &programme);
    // Record the channel now, until the end of its current show (or for an hour).
    Q_INVOKABLE QString recordNow(int globalIndex);
    // "reminder", "recording" (scheduled or running) or "" for a show.
    Q_INVOKABLE QString markFor(int globalIndex, double startMs) const;
    Q_INVOKABLE QString idFor(int globalIndex, double startMs) const;
    Q_INVOKABLE void stopRecording(const QString &id);
    // Cancels a reminder or scheduled recording; for a finished recording
    // also deletes its file when deleteFile is true.
    Q_INVOKABLE void remove(const QString &id, bool deleteFile = true);
    Q_INVOKABLE QVariantMap item(const QString &id) const;

    // Record every episode of this show on this channel: each new airing in
    // the guide is scheduled as the guide updates. An episode that was
    // already recorded (same episode number or episode title) isn't recorded
    // again. programme: {title, …} as the guide gives it.
    Q_INVOKABLE QString addSeries(int globalIndex, const QVariantMap &programme);
    Q_INVOKABLE QString seriesFor(int globalIndex, const QString &title) const;   // "" = none
    // Removes the rule and cancels its recordings that haven't started.
    Q_INVOKABLE void removeSeries(const QString &id);
    QVariantList series() const;
    // Schedules what the rules ask for from the guide as it is now.
    void applySeries();

    void shutdown();      // stop running recordings (app is quitting)
    void tick();          // exposed for tests

signals:
    void changed();
    void folderChanged();
    void reminderDue(const QVariantMap &item);
    void message(const QString &text);

private:
    struct Item {
        QString id, type, status, playlistId;
        QString channelKey, channelName, url, userAgent, referrer;
        QString title, file, error;
        qint64 start = 0, stop = 0;   // Unix seconds
        qint64 bytes = 0;
        QString seriesId, episodeKey;   // made by a "record every episode" rule
    };
    struct Series {
        QString id, playlistId, channelKey, channelName, title;
        QStringList seen;               // episodes recorded (episodeKey)
        QVector<qint64> skipped;        // airings the user cancelled (start)
    };
    Series *findSeries(const QString &id);
    QVariantMap toMap(const Item &it) const;
    Item *find(const QString &id);
    QString add(const QString &type, int globalIndex, qint64 start, qint64 stop, const QString &title);
    void startRecording(Item &it);
    void finishRecording(const QString &id);
    QString fileNameFor(const Item &it) const;
    void load();
    void save();
    void touch();

    ChannelModel *m_channels;
    EpgService *m_epg;
    QString m_path;
    QString m_folder;
    QNetworkAccessManager *m_net;
    QVector<Item> m_items;
    QVector<Series> m_series;
    qint64 m_lastApply = 0;
    QHash<QString, Recorder *> m_recorders;   // id -> running recorder
    QTimer m_timer;
    int m_revision = 0;
    QString m_playlist;
};
