#pragma once

#include "Catchup.h"

#include <QHash>
#include <QObject>
#include <QVariantMap>

class ChannelModel;
class QNetworkAccessManager;
struct PlaylistResult;

// Which channels can play past shows, and their archive addresses.
//
// With an Xtream account (typed in, or inside a get.php playlist link) it
// asks the server for its time zone, which archive addresses are written
// in, and, when the playlist doesn't say which channels keep an archive,
// for the live stream list with each channel's archive length.
class CatchupService : public QObject
{
    Q_OBJECT
    // Channels with catch-up, for Settings.
    Q_PROPERTY(int channelCount READ channelCount NOTIFY changed)

public:
    explicit CatchupService(ChannelModel *channels, QObject *parent = nullptr);

    int channelCount() const { return m_count; }

    // The Xtream account with its time zone (normally from the server).
    void setAccount(const Catchup::Account &account) { m_account = account; apply(); }

    // After ChannelModel::setPlaylist().
    void setSource(const PlaylistResult &result);
    void clear();

    // Can the show starting at `startMs` (ms since epoch) on this channel be
    // played from the archive?
    Q_INVOKABLE bool available(int globalIndex, double startMs) const;
    // Days of archive for the channel (0 = none).
    Q_INVOKABLE int days(int globalIndex) const;

    // Archive address from `start` to `stop` (Unix seconds); see Catchup::url.
    QString url(int globalIndex, qint64 start, qint64 stop, int *startAdjust = nullptr) const;

signals:
    void changed();

private:
    void fetch();
    void apply();

    ChannelModel *m_channels;
    QNetworkAccessManager *m_net;
    Catchup::Account m_account;      // valid once the server has answered
    Catchup::Account m_pending;      // credentials waiting for the answer
    QString m_accountKey;
    QHash<QString, int> m_days;      // from the server's stream list
    bool m_needsList = false;
    bool m_haveList = false;
    int m_count = 0;
    quint64 m_generation = 0;
};
