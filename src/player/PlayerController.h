#pragma once

#include <QObject>
#include <QTimer>
#include <QVariantMap>

class ChannelModel;
class AppSettings;
class EpgService;
class CatchupService;
class Parental;
struct Programme;

// Channel-switching logic: tune, zap up/down within a group, jump back to the
// previous channel, and type a channel number. Emits playRequested(); QML
// forwards that to the video item so this class stays UI-agnostic.
class PlayerController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int currentIndex READ currentIndex NOTIFY currentChanged)
    Q_PROPERTY(bool hasChannel READ hasChannel NOTIFY currentChanged)
    Q_PROPERTY(int currentNumber READ currentNumber NOTIFY currentChanged)
    Q_PROPERTY(QString currentName READ currentName NOTIFY currentChanged)
    Q_PROPERTY(QString currentGroup READ currentGroup NOTIFY currentChanged)
    Q_PROPERTY(QString currentLogo READ currentLogo NOTIFY currentChanged)
    Q_PROPERTY(int zapGroup READ zapGroup NOTIFY currentChanged)
    Q_PROPERTY(bool hasPrevious READ hasPrevious NOTIFY currentChanged)
    Q_PROPERTY(QString pendingNumber READ pendingNumber NOTIFY pendingNumberChanged)
    // Guide info for the current channel. Keys: valid, title, subTitle, desc,
    // category, episode, start, stop (ms since epoch, for JS Date).
    Q_PROPERTY(QVariantMap nowProgram READ nowProgram NOTIFY programChanged)
    Q_PROPERTY(QVariantMap nextProgram READ nextProgram NOTIFY programChanged)
    // A movie or episode is playing instead of live TV. `vod` is the
    // descriptor passed to playVod (title, subtitle, poster, url, key, ...).
    Q_PROPERTY(bool vodActive READ vodActive NOTIFY vodChanged)
    Q_PROPERTY(QVariantMap vod READ vod NOTIFY vodChanged)

public:
    PlayerController(ChannelModel *channels, AppSettings *settings, QObject *parent = nullptr);

    int currentIndex() const { return m_current; }
    bool hasChannel() const { return m_current >= 0; }
    int currentNumber() const;
    QString currentName() const;
    QString currentGroup() const;
    QString currentLogo() const;
    int zapGroup() const;
    bool hasPrevious() const { return m_previous >= 0; }
    QString pendingNumber() const { return m_pending; }
    QVariantMap nowProgram() const { return m_now; }
    QVariantMap nextProgram() const { return m_next; }
    bool vodActive() const { return m_vodActive; }
    QVariantMap vod() const { return m_vod; }

    void setEpg(EpgService *epg);
    void refreshProgram();

    // fromGroup: group the user picked the channel in (zapping stays inside
    // it). -1 = use the channel's own group.
    Q_INVOKABLE void tune(int globalIndex, int fromGroup = -1);
    Q_INVOKABLE void channelUp();
    Q_INVOKABLE void channelDown();
    Q_INVOKABLE void previousChannel();
    Q_INVOKABLE void retry();

    // Plays a movie/episode descriptor (needs url; start = seconds). Tuning
    // a channel ends it; stopVod() goes back to the channel that was on.
    Q_INVOKABLE void playVod(const QVariantMap &item);
    Q_INVOKABLE void stopVod();

    // Catch-up: play a past show (programme map with title, start, stop in
    // ms, as the guide gives them) from `offsetSecs` into it. Seeking asks
    // the server for the archive again from the new time. Returns false if
    // the channel has no archive for that time.
    Q_INVOKABLE bool playCatchup(int globalIndex, const QVariantMap &programme, double offsetSecs = 0);
    Q_INVOKABLE void seekCatchup(double offsetSecs);
    // At the end of a catch-up show: play the next show on that channel if
    // it has already aired. False means it's time to go live.
    Q_INVOKABLE bool playNextCatchup();
    Q_INVOKABLE bool canCatchup(int globalIndex, double startMs) const;
    void setCatchup(CatchupService *catchup) { m_catchup = catchup; }

    // Parental lock: tuning a locked channel (or playing its catch-up) waits
    // for the PIN. pinRequired() asks for it; after Parental.enterPin()
    // succeeds, resumePending() carries on.
    void setParental(Parental *parental) { m_parental = parental; }
    Q_INVOKABLE void resumePending();
    Q_INVOKABLE void cancelPending();

    // Hands what's playing (the channel, movie or episode) to the other
    // player program chosen in Settings. externalOpened / externalFailed follow.
    Q_INVOKABLE void openExternally();
    // What's playing, for casting: {url, userAgent, referrer, title, live}.
    Q_INVOKABLE QVariantMap castSource() const;

    Q_INVOKABLE void typeDigit(int digit);
    Q_INVOKABLE void commitNumber();
    Q_INVOKABLE void cancelNumber();

    // After a playlist (re)load: reopen the last channel, or the first one.
    void restoreAfterLoad();
    // After the playlist was refreshed underneath: find the channels again
    // by key; the picture keeps playing unless its address changed.
    void rebindAfterReload(const QString &currentKey, const QString &previousKey, const QString &oldUrl);
    int previousIndex() const { return m_previous; }
    void reset();

signals:
    void currentChanged();
    void pendingNumberChanged();
    void playRequested(const QString &url, const QString &userAgent, const QString &referrer);
    void numberNotFound(int number);
    void programChanged();
    void vodChanged();
    void vodRequested(const QString &url, double startSeconds);
    void stopRequested();
    void pinRequired(const QString &channelName);
    void externalOpened(const QString &playerName);
    void externalFailed(const QString &message);

private:
    void zap(int delta);

    ChannelModel *m_channels;
    AppSettings *m_settings;
    int m_current = -1;
    int m_previous = -1;
    // Groups are remembered by name: group indexes shift when the user
    // hides groups or adds the first favorite.
    QString m_zapGroup;
    QString m_previousZapGroup;
    QString m_pending;
    QTimer m_numberTimer;
    QTimer m_watchedTimer;   // a channel counts as watched after a few seconds
    EpgService *m_epg = nullptr;
    QVariantMap m_now;
    QVariantMap m_next;
    QTimer m_programTick;
    CatchupService *m_catchup = nullptr;
    Parental *m_parental = nullptr;
    struct Pending { int kind = 0; int index = -1; int group = -1; QVariantMap programme; double offset = 0; };
    Pending m_pendingAction;   // kind 1 = tune, 2 = catch-up
    bool blockedNow(int globalIndex) const;
    bool m_vodActive = false;
    QVariantMap m_vod;
};
