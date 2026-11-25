#include "TsSegmenter.h"

#include <cmath>

namespace {
constexpr int kPacket = 188;
constexpr double kMaxWithoutKeyFrame = 10.0;   // cut anyway (some streams never flag key frames)
constexpr double kMaxWaitForKey = 6.0;         // start without one after this long

int pidOf(const char *p) { return ((uchar(p[1]) & 0x1F) << 8) | uchar(p[2]); }
bool payloadStart(const char *p) { return uchar(p[1]) & 0x40; }
int adaptation(const char *p) { return (uchar(p[3]) >> 4) & 0x3; }   // 1 payload, 2 adaptation, 3 both

// Where the payload starts (after the adaptation field), or -1.
int payloadOffset(const char *p)
{
    const int a = adaptation(p);
    if (a == 2) return -1;
    if (a == 3) {
        const int len = uchar(p[4]);
        return 5 + len < kPacket ? 5 + len : -1;
    }
    return 4;
}
} // namespace

TsSegmenter::TsSegmenter(double targetSecs, int keep) : m_target(targetSecs), m_keep(qMax(3, keep)) {}

void TsSegmenter::feed(const QByteArray &data)
{
    m_pending.append(data);
    int pos = 0;
    // Find the sync byte (0x47 at packet spacing).
    while (pos + kPacket <= m_pending.size()) {
        if (m_pending[pos] != 0x47) { ++pos; continue; }
        if (pos + kPacket < m_pending.size() && m_pending[pos + kPacket] != 0x47) { ++pos; continue; }
        packet(m_pending.constData() + pos);
        pos += kPacket;
    }
    m_pending.remove(0, pos);
}

void TsSegmenter::finish()
{
    if (!m_current.isEmpty()) close(qMax(0.5, m_pcrSinceCut));
    m_ended = true;
}

void TsSegmenter::discontinuity()
{
    // Whatever was half built belongs to the old connection.
    if (!m_current.isEmpty() && m_pcrSinceCut >= 1.0) close(m_pcrSinceCut);
    m_current.clear();
    m_pending.clear();
    m_pat.clear();
    m_pmt.clear();
    m_pmtPid = m_videoPid = -1;
    m_lastPcr = -1;
    m_pcrSinceCut = 0;
    m_waitForKey = true;
    m_waitedPcr = 0;
    if (!m_segments.isEmpty()) m_nextIsBreak = true;
}

bool TsSegmenter::keyFrameInPayload(const char *p, int streamType)
{
    int off = payloadOffset(p);
    if (off < 0 || off + 9 > kPacket) return false;
    const uchar *b = reinterpret_cast<const uchar *>(p);
    if (b[off] != 0 || b[off + 1] != 0 || b[off + 2] != 1) return false;   // not a PES start
    off += 9 + b[off + 8];                                                  // past the PES header
    for (int i = off; i + 3 < kPacket; ++i) {
        if (b[i] != 0 || b[i + 1] != 0 || b[i + 2] != 1) continue;
        const uchar n = b[i + 3];
        if (streamType == 0x1B) {                 // H.264
            const int t = n & 0x1F;
            if (t == 5 || t == 7) return true;    // IDR slice, SPS
            if (t >= 1 && t <= 4) return false;   // a slice of another frame
        } else if (streamType == 0x24) {          // HEVC
            const int t = (n >> 1) & 0x3F;
            if ((t >= 16 && t <= 21) || t == 32 || t == 33) return true;   // IRAP, VPS, SPS
            if (t <= 9) return false;
        } else {                                  // MPEG-1/2
            if (n == 0xB3) return true;           // sequence header
            if (n == 0x00) return false;          // a picture without one
        }
        i += 3;
    }
    return false;
}
void TsSegmenter::parsePat(const char *p)
{
    const int off = payloadOffset(p);
    if (off < 0) return;
    const int start = off + 1 + uchar(p[off]);    // pointer field
    if (start + 8 >= kPacket) return;
    const int sectionLen = ((uchar(p[start + 1]) & 0x0F) << 8) | uchar(p[start + 2]);
    const int end = qMin(kPacket - 4, start + 3 + sectionLen - 4);
    for (int i = start + 8; i + 4 <= end; i += 4) {
        const int program = (uchar(p[i]) << 8) | uchar(p[i + 1]);
        if (program == 0) continue;                // network PID
        m_pmtPid = ((uchar(p[i + 2]) & 0x1F) << 8) | uchar(p[i + 3]);
        break;
    }
    m_pat = QByteArray(p, kPacket);
}

void TsSegmenter::parsePmt(const char *p)
{
    const int off = payloadOffset(p);
    if (off < 0) return;
    const int start = off + 1 + uchar(p[off]);
    if (start + 12 >= kPacket) return;
    const int sectionLen = ((uchar(p[start + 1]) & 0x0F) << 8) | uchar(p[start + 2]);
    const int infoLen = ((uchar(p[start + 10]) & 0x0F) << 8) | uchar(p[start + 11]);
    const int end = qMin(kPacket - 4, start + 3 + sectionLen - 4);
    for (int i = start + 12 + infoLen; i + 5 <= end;) {
        const int type = uchar(p[i]);
        const int pid = ((uchar(p[i + 1]) & 0x1F) << 8) | uchar(p[i + 2]);
        const int esLen = ((uchar(p[i + 3]) & 0x0F) << 8) | uchar(p[i + 4]);
        if (m_videoPid < 0 && (type == 0x1B || type == 0x24 || type == 0x02 || type == 0x01 || type == 0x10)) {
            m_videoPid = pid;
            m_videoType = type;
        }
        i += 5 + esLen;
    }
    m_pmt = QByteArray(p, kPacket);
}

void TsSegmenter::packet(const char *p)
{
    const int pid = pidOf(p);
    if (pid == 0 && payloadStart(p)) parsePat(p);
    else if (pid == m_pmtPid && payloadStart(p)) parsePmt(p);

    // The stream's clock: 90 kHz PCR in the adaptation field.
    double pcr = -1;
    bool keyFrame = false;
    if ((adaptation(p) & 2) && uchar(p[4]) > 0) {
        const uchar flags = uchar(p[5]);
        if (flags & 0x40) keyFrame = true;          // random_access_indicator
        if ((flags & 0x10) && uchar(p[4]) >= 7) {
            const quint64 base = (quint64(uchar(p[6])) << 25) | (quint64(uchar(p[7])) << 17) | (quint64(uchar(p[8])) << 9)
                               | (quint64(uchar(p[9])) << 1) | (quint64(uchar(p[10])) >> 7);
            pcr = double(base) / 90000.0;
        }
    }
    if (pcr >= 0) {
        if (m_lastPcr >= 0) {
            double step = pcr - m_lastPcr;
            if (step < -5 || step > 5) step = 0;         // a jump (wrap, new programme): don't count it
            if (step > 0) { m_pcrSinceCut += step; if (m_waitForKey) m_waitedPcr += step; }
        }
        m_lastPcr = pcr;
    }
    const bool frameStart = pid == m_videoPid && payloadStart(p);
    if (keyFrame && pid == m_videoPid) m_sawKeyFrameFlag = true;
    // Many IPTV streams never set the flag: look at the video itself.
    if (frameStart && !keyFrame && keyFrameInPayload(p, m_videoType)) {
        keyFrame = true;
        m_sawKeyFrameInVideo = true;
    }
    const bool knowsKeys = m_sawKeyFrameFlag || m_sawKeyFrameInVideo;

    // Cut before a video packet that starts a frame, once the segment is long enough.
    if (frameStart && !m_current.isEmpty()) {
        const bool longEnough = m_pcrSinceCut >= m_target - 0.5;   // the clock runs a little ahead of the video
        const bool atKey = keyFrame || !knowsKeys;
        if ((longEnough && atKey) || m_pcrSinceCut >= kMaxWithoutKeyFrame) close(m_pcrSinceCut);
    }
    if (m_current.isEmpty()) {
        if (m_pat.isEmpty() || m_pmt.isEmpty()) return;  // can't start a segment without them
        if (m_videoPid >= 0 && !frameStart) return;      // start on a frame
        // The first segment (and the first after a break) on a key frame,
        // so the receiver's decoder has what it needs to start.
        if (m_waitForKey && m_videoPid >= 0) {
            if (!keyFrame && m_waitedPcr < kMaxWaitForKey) return;
            m_waitForKey = false;
            m_pcrSinceCut = 0;
        }
        m_current.reserve(2 * 1024 * 1024);
        m_current.append(m_pat);
        m_current.append(m_pmt);
    }
    m_current.append(p, kPacket);
}

void TsSegmenter::close(double duration)
{
    m_segments.append({m_nextSequence++, duration, m_current, m_nextIsBreak});
    m_nextIsBreak = false;
    while (m_segments.size() > m_keep) {
        if (m_segments.size() > 1 && m_segments.at(1).discontinuity) ++m_discontinuitySequence;   // the break scrolls off
        m_segments.removeFirst();
    }
    m_current.clear();
    m_pcrSinceCut = 0;
}

QByteArray TsSegmenter::playlist(const QString &prefix, int minimum) const
{
    if (m_segments.size() < minimum) return QByteArray();
    double longest = m_target;
    for (const Segment &s : m_segments) longest = qMax(longest, s.duration);
    QByteArray out = "#EXTM3U\n#EXT-X-VERSION:3\n";
    out += "#EXT-X-TARGETDURATION:" + QByteArray::number(int(std::ceil(longest))) + "\n";
    out += "#EXT-X-MEDIA-SEQUENCE:" + QByteArray::number(m_segments.first().sequence) + "\n";
    if (m_discontinuitySequence > 0) out += "#EXT-X-DISCONTINUITY-SEQUENCE:" + QByteArray::number(m_discontinuitySequence) + "\n";
    for (int i = 0; i < m_segments.size(); ++i) {
        const Segment &s = m_segments.at(i);
        if (s.discontinuity && i > 0) out += "#EXT-X-DISCONTINUITY\n";
        out += "#EXTINF:" + QByteArray::number(s.duration, 'f', 3) + ",\n";
        out += prefix.toUtf8() + QByteArray::number(s.sequence) + ".ts\n";
    }
    if (m_ended) out += "#EXT-X-ENDLIST\n";   // a movie that has been sent in full
    return out;
}

QByteArray TsSegmenter::joinData() const
{
    QByteArray out;
    if (!m_segments.isEmpty()) out = m_segments.last().data;
    out += m_current;
    out += m_pending;   // the start of a packet whose rest comes next
    return out;
}

QByteArray TsSegmenter::segment(int sequence) const
{
    for (const Segment &s : m_segments)
        if (s.sequence == sequence) return s.data;
    return QByteArray();
}
