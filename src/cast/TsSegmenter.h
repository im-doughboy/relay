#pragma once

#include <QByteArray>
#include <QList>
#include <QString>

// Cuts a live MPEG-TS stream into short HLS segments, for receivers that
// only play HLS (Chromecast). Segments start on a video key frame once
// they're long enough: the random-access flag where broadcasters set it,
// otherwise found in the video itself (an H.264/HEVC IDR or parameter sets,
// an MPEG-2 sequence header). Each begins with the stream's PAT and PMT, and
// their length comes from the stream's clock (PCR), so a burst at the start
// doesn't make them look short.
//
// When the provider's connection drops and comes back, discontinuity()
// starts afresh at the next key frame and marks the break in the playlist,
// so the receiver resets its clock instead of stalling.
//
// Feed it bytes as they arrive; ask for the playlist and segments.
class TsSegmenter
{
public:
    explicit TsSegmenter(double targetSecs = 4.0, int keep = 8);

    void feed(const QByteArray &data);
    void finish();   // the stream ended: close the last segment
    void discontinuity();   // a new connection: what follows doesn't continue what came before

    struct Segment { int sequence; double duration; QByteArray data; bool discontinuity = false; };
    const QList<Segment> &segments() const { return m_segments; }
    // A live HLS playlist of the segments kept; empty until there are `minimum`.
    QByteArray playlist(const QString &segmentPrefix, int minimum = 3) const;
    QByteArray segment(int sequence) const;
    // For a receiver joining a live stream: the newest whole segment (from a
    // key frame) and everything since, so it can carry on with what comes next.
    QByteArray joinData() const;
    // Is this packet (a video packet that starts a frame) a key frame, by what's in it?
    static bool keyFrameInPayload(const char *packet, int streamType);

    int videoPid() const { return m_videoPid; }
    int videoStreamType() const { return m_videoType; }   // 0x1B H.264, 0x24 HEVC, 0x02 MPEG-2

private:
    void packet(const char *p);
    void close(double duration);
    void parsePat(const char *p);
    void parsePmt(const char *p);

    double m_target;
    int m_keep;
    QByteArray m_pending;           // less than a packet left over
    QByteArray m_current;           // the segment being built
    QByteArray m_pat, m_pmt;        // latest copies, put at each segment's start
    int m_pmtPid = -1, m_videoPid = -1, m_videoType = 0;
    double m_segStartPcr = -1, m_lastPcr = -1, m_pcrSinceCut = 0;
    bool m_sawKeyFrameFlag = false;
    bool m_sawKeyFrameInVideo = false;
    bool m_waitForKey = true;       // the first segment (and after a break) starts on a key frame
    double m_waitedPcr = 0;         // how long it's been waiting for one
    bool m_nextIsBreak = false;
    int m_discontinuitySequence = 0;
    int m_nextSequence = 0;
    QList<Segment> m_segments;
};
