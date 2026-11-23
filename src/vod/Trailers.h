#pragma once

#include <QDateTime>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QStringList>

class QProcess;

// Trailers (QML: Trailers). A trailer is a YouTube id or address (from the
// provider or TMDB), or a direct video address. YouTube ones are turned into
// playable addresses with yt-dlp, found next to Relay (the Windows download
// includes it) or on the PATH. The answers are kept for two hours.
class Trailers : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool available READ available NOTIFY availableChanged)       // yt-dlp found
    Q_PROPERTY(bool autoplay READ autoplay WRITE setAutoplay NOTIFY autoplayChanged)

public:
    explicit Trailers(QObject *parent = nullptr);

    bool available() const { return !m_tool.isEmpty(); }
    bool autoplay() const;
    void setAutoplay(bool on);

    // {video, audio} for `trailer` through resolved(); failed() when there's none.
    Q_INVOKABLE void resolve(const QString &trailer);
    Q_INVOKABLE static QString pageUrl(const QString &trailer);   // the YouTube page, for the browser
    // "dQw4w9WgXcQ", "https://youtu.be/…", "https://www.youtube.com/watch?v=…" -> the id; "" otherwise.
    static QString youtubeId(const QString &trailer);
    static QString findTool();

signals:
    void availableChanged();
    void autoplayChanged();
    void resolved(const QString &trailer, const QString &video, const QString &audio);
    void failed(const QString &trailer, const QString &message);

private:
    void updateTool();
    QString m_tool;
    struct Answer { qint64 at; QString video, audio; };
    QHash<QString, Answer> m_answers;
    QHash<QString, QPointer<QProcess>> m_running;
};
