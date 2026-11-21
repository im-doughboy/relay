#pragma once

#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QVector>

// One live channel, independent of where it came from (M3U or Xtream).
struct Channel
{
    int number = 0;          // display number (tvg-chno / Xtream "num" / position)
    QString name;
    QString group;           // group-title / Xtream category
    QString logo;            // tvg-logo / stream_icon
    QString url;             // stream URL handed to mpv
    QString tvgId;           // EPG id from the playlist
    QString epgId;           // guide channel this resolved to ("" = no guide)
    int tvgShiftMinutes = 0; // tvg-shift: move this channel's listings (playlist says hours)

    // Per-stream HTTP options some providers require.
    QString userAgent;
    QString referrer;

    // Catch-up metadata, stored now and used in phase 3.
    QString catchupType;     // "default", "append", "shift", "flussonic", "xc"...
    QString catchupSource;
    int catchupDays = 0;
};

struct PlaylistResult
{
    QVector<Channel> channels;
    QString epgUrl;          // url-tvg from the M3U header, or Xtream xmltv.php
    bool guideIsNew = false; // the guide at epgUrl was just fetched (portals): import it now
    QStringList groupOrder;  // provider's group order (Xtream); empty = order of appearance
    int skippedMovies = 0;   // video-on-demand entries left out of live TV
    int skippedSeries = 0;

    // Movies and series. The entries themselves are kept for playlists that
    // aren't from an Xtream server; with an Xtream login the catalogue comes
    // straight from the server's API instead (VodService).
    QVector<Channel> vodMovies;
    QVector<Channel> vodEpisodes;
    QString xtreamServer, xtreamUser, xtreamPassword;
    QString portalAccount;   // Stalker / MAC portal (PortalClient account): its movies come from the portal

    QString error;           // non-empty when loading failed
};

Q_DECLARE_METATYPE(PlaylistResult)
