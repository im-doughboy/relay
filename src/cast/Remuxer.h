#pragma once

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QString>

#include <atomic>
#include <thread>

// Repackages a movie or episode as MPEG-TS with one chosen sound track, for
// casting. TVs and Chromecasts play a file's first sound track and offer no
// way to pick another, so when the viewer picked a different one Relay sends
// the picture and that track only. Nothing is re-encoded (FFmpeg copies the
// packets), so it's light on the computer.
//
// It reads from the provider on its own thread and hands out MPEG-TS as it
// goes; hold() pauses reading while the receiver has enough. Seeking is
// starting again somewhere else (Options::start).
class Remuxer : public QObject
{
    Q_OBJECT

public:
    struct Options {
        QString url, userAgent, referrer;
        double start = 0;     // seconds into the video
        int audio = -1;       // which sound track (0 = the first), -1 = the file's own choice
    };
    struct Track {
        QString lang, title;
        bool isDefault = false;
    };
    struct Probe {
        QString error;        // empty: it opened
        double duration = 0;
        QList<Track> audio;
    };

    explicit Remuxer(const Options &options, QObject *parent = nullptr);
    ~Remuxer() override;   // stops reading and waits for the thread

    void start();
    void hold(bool on) { m_hold = on; }
    // The receiving side has dealt with one data() chunk. Only two are ever
    // waiting, so hold() takes effect straight away, not after a backlog.
    void taken() { --m_inFlight; }
    bool holding() const { return m_hold; }

    // Opens the address and lists its sound tracks (blocking: call off the UI thread).
    static Probe probe(const QString &url, const QString &userAgent, const QString &referrer);
    // The track a TV plays when told nothing: the one marked default, else the first.
    static int receiverDefault(const QList<Track> &audio);
    // The track to send: the one picked while watching (its position, language
    // tag and title, as the player listed it), else one in `preferred`
    // (a Settings code like "en"). -1: nothing to choose.
    static int choose(const QList<Track> &audio, int pickedIndex, const QString &pickedLang,
                      const QString &pickedTitle, const QString &preferred);

signals:
    void data(const QByteArray &ts);
    void ended(const QString &error);   // empty: the end of the video

private:
    void run();
    Options m_options;
    std::atomic<bool> m_hold{false};
    std::atomic<bool> m_stop{false};
    std::atomic<int> m_inFlight{0};
    std::thread m_thread;
};
