#include "PlayerController.h"

#include "sources/PortalClient.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>

#include "app/AppSettings.h"
#include "epg/EpgService.h"
#include "models/ChannelModel.h"
#include "sources/CatchupService.h"
#include "app/Parental.h"

#include <QDateTime>
#include <QLocale>

namespace {
constexpr int kNumberCommitMs = 1500;
constexpr int kMaxDigits = 5;
}

PlayerController::PlayerController(ChannelModel *channels, AppSettings *settings, QObject *parent)
    : QObject(parent), m_channels(channels), m_settings(settings)
{
    m_numberTimer.setSingleShot(true);
    m_numberTimer.setInterval(kNumberCommitMs);
    connect(&m_numberTimer, &QTimer::timeout, this, &PlayerController::commitNumber);
    m_watchedTimer.setSingleShot(true);
    m_watchedTimer.setInterval(8000);
    connect(&m_watchedTimer, &QTimer::timeout, this, [this] {
        if (m_current >= 0 && !m_vodActive)
            m_channels->noteWatched(m_current, zapGroup() == ChannelModel::RecentGroup);
    });

    connect(this, &PlayerController::currentChanged, this, &PlayerController::refreshProgram);
    connect(m_channels, &ChannelModel::channelRenamed, this, [this](int i) {
        if (i == m_current) emit currentChanged();
    });
    connect(m_channels, &ChannelModel::groupsChanged, this, &PlayerController::currentChanged);
    m_programTick.setInterval(15000);
    connect(&m_programTick, &QTimer::timeout, this, &PlayerController::refreshProgram);
    m_programTick.start();
}

namespace {
QVariantMap toMap(const Programme &p)
{
    return {
        {QStringLiteral("valid"), true},
        {QStringLiteral("title"), p.title},
        {QStringLiteral("subTitle"), p.subTitle},
        {QStringLiteral("desc"), p.desc},
        {QStringLiteral("category"), p.category},
        {QStringLiteral("episode"), p.episode},
        {QStringLiteral("start"), double(p.start) * 1000.0},
        {QStringLiteral("stop"), double(p.stop) * 1000.0},
    };
}
const QVariantMap kNoProgram{{QStringLiteral("valid"), false}};
} // namespace

void PlayerController::setEpg(EpgService *epg)
{
    m_epg = epg;
    connect(epg, &EpgService::guideUpdated, this, &PlayerController::refreshProgram);
    refreshProgram();
}

void PlayerController::refreshProgram()
{
    QVariantMap now = kNoProgram, next = kNoProgram;
    const Channel *c = m_channels->channel(m_current);
    if (m_epg && c && !c->epgId.isEmpty()) {
        const auto nn = m_epg->nowNext(c->epgId);
        if (nn.hasNow) now = toMap(nn.now);
        if (nn.hasNext) next = toMap(nn.next);
    }
    if (now == m_now && next == m_next) return;
    m_now = now;
    m_next = next;
    emit programChanged();
}

int PlayerController::zapGroup() const
{
    const int idx = m_channels->groupIndex(m_zapGroup);
    return idx < 0 ? ChannelModel::AllGroup : idx;
}

int PlayerController::currentNumber() const
{
    const Channel *c = m_channels->channel(m_current);
    return c ? c->number : 0;
}

QString PlayerController::currentName() const
{
    const Channel *c = m_channels->channel(m_current);
    return c ? c->name : QString();
}

QString PlayerController::currentGroup() const
{
    const Channel *c = m_channels->channel(m_current);
    return c ? m_channels->labelForGroupId(c->group) : QString();
}

QString PlayerController::currentLogo() const
{
    const Channel *c = m_channels->channel(m_current);
    return c ? c->logo : QString();
}

bool PlayerController::blockedNow(int globalIndex) const
{
    return m_parental && m_parental->blocked(globalIndex);
}

void PlayerController::resumePending()
{
    const Pending p = m_pendingAction;
    m_pendingAction = {};
    if (p.kind == 1) tune(p.index, p.group);
    else if (p.kind == 2) playCatchup(p.index, p.programme, p.offset);
}

void PlayerController::cancelPending() { m_pendingAction = {}; }

void PlayerController::tune(int globalIndex, int fromGroup)
{
    const Channel *c = m_channels->channel(globalIndex);
    if (!c) return;
    if (blockedNow(globalIndex)) {
        m_pendingAction = {1, globalIndex, fromGroup, {}, 0};
        emit pinRequired(c->name);
        return;
    }

    if (globalIndex != m_current) {
        m_previous = m_current;
        m_previousZapGroup = m_zapGroup;
    }
    m_current = globalIndex;
    m_zapGroup = m_channels->groupName(fromGroup >= 0 ? fromGroup
                                                      : m_channels->groupOfChannel(globalIndex));

    m_settings->setLastChannelUrl(m_channels->keyAt(globalIndex));   // a channel id, not its address
    if (m_vodActive) {          // picking a channel ends the movie
        m_vodActive = false;
        m_vod.clear();
        emit vodChanged();
    }
    emit currentChanged();
    {
        QString guide = QStringLiteral("no guide match");
        if (!c->epgId.isEmpty()) {
            const QString now = m_now.value(QStringLiteral("title")).toString();
            guide = QStringLiteral("guide id %1, now: %2")
                        .arg(EpgService::baseId(c->epgId), now.isEmpty() ? QStringLiteral("nothing listed") : now);
        }
        qInfo().noquote() << QStringLiteral("Tuned: %1 %2 [%3] · %4")
                                 .arg(c->number).arg(c->name, c->group, guide);
    }
    m_watchedTimer.start();
    emit playRequested(c->url, c->userAgent, c->referrer);
}

void PlayerController::zap(int delta)
{
    if (m_channels->totalCount() == 0) return;
    const int group = zapGroup();
    const int next = m_channels->step(m_current, delta, group);
    if (next >= 0) tune(next, group);
}

void PlayerController::channelUp() { zap(+1); }
void PlayerController::channelDown() { zap(-1); }

void PlayerController::previousChannel()
{
    if (m_previous < 0) return;
    const int group = m_channels->groupIndex(m_previousZapGroup);
    tune(m_previous, group < 0 ? ChannelModel::AllGroup : group);
}

void PlayerController::retry()
{
    if (m_vodActive && m_vod.value(QStringLiteral("kind")).toString() == QLatin1String("catchup")) {
        seekCatchup(m_vod.value(QStringLiteral("offset")).toDouble());
        return;
    }
    if (m_vodActive) {
        emit vodRequested(m_vod.value(QStringLiteral("url")).toString(),
                          m_vod.value(QStringLiteral("start")).toDouble());
        return;
    }
    const Channel *c = m_channels->channel(m_current);
    if (c) emit playRequested(c->url, c->userAgent, c->referrer);
}

void PlayerController::playVod(const QVariantMap &item)
{
    const QString url = item.value(QStringLiteral("url")).toString();
    if (url.isEmpty()) return;
    cancelNumber();
    m_vod = item;
    m_vodActive = true;
    emit vodChanged();
    qInfo().noquote() << QStringLiteral("Playing %1: %2%3 from %4 s")
                             .arg(item.value(QStringLiteral("kind")).toString(),
                                  item.value(QStringLiteral("title")).toString(),
                                  item.value(QStringLiteral("subtitle")).toString().isEmpty()
                                      ? QString() : QStringLiteral(" · ") + item.value(QStringLiteral("subtitle")).toString())
                             .arg(qRound(item.value(QStringLiteral("start")).toDouble()));
    emit vodRequested(url, item.value(QStringLiteral("start")).toDouble());
}

bool PlayerController::canCatchup(int globalIndex, double startMs) const
{
    return m_catchup && m_catchup->available(globalIndex, startMs);
}

bool PlayerController::playCatchup(int globalIndex, const QVariantMap &programme, double offsetSecs)
{
    const Channel *c = m_channels->channel(globalIndex);
    const double startMs = programme.value(QStringLiteral("start")).toDouble();
    const double stopMs = programme.value(QStringLiteral("stop")).toDouble();
    if (!c || !canCatchup(globalIndex, startMs) || stopMs <= startMs) return false;
    if (blockedNow(globalIndex)) {
        m_pendingAction = {2, globalIndex, -1, programme, offsetSecs};
        emit pinRequired(c->name);
        return true;
    }

    const qint64 start = qint64(startMs / 1000), stop = qint64(stopMs / 1000);
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    // Never ask for a time that hasn't happened yet.
    offsetSecs = qBound(0.0, offsetSecs, double(qMax<qint64>(0, qMin(stop, now - 30) - start)));
    int adjust = 0;
    const qint64 from = start + qint64(offsetSecs);
    const QString url = m_catchup->url(globalIndex, from, stop, &adjust);
    if (url.isEmpty()) return false;
    // A portal's archive streams start where the show starts.
    if (PortalClient::isPortalUrl(url)) adjust = int(offsetSecs);

    // The channel becomes the current one, so leaving goes to it live.
    if (globalIndex != m_current) {
        m_previous = m_current;
        m_previousZapGroup = m_zapGroup;
        m_current = globalIndex;
        m_zapGroup = m_channels->groupName(m_channels->groupOfChannel(globalIndex));
        emit currentChanged();
    }
    const QLocale loc;
    const QString when = QDateTime::fromSecsSinceEpoch(start).date() == QDate::currentDate()
        ? QString() : loc.toString(QDateTime::fromSecsSinceEpoch(start).date(), QStringLiteral("ddd d MMM  "));
    const QString fmt = loc.timeFormat(QLocale::ShortFormat).remove(QStringLiteral(":ss"));
    cancelNumber();
    m_vod = programme;
    m_vod.insert(QStringLiteral("kind"), QStringLiteral("catchup"));
    m_vod.insert(QStringLiteral("url"), url);
    m_vod.insert(QStringLiteral("channelIndex"), globalIndex);
    m_vod.insert(QStringLiteral("channelName"), c->name);
    m_vod.insert(QStringLiteral("offset"), offsetSecs);
    m_vod.insert(QStringLiteral("start"), double(adjust));   // seconds into this stream
    m_vod.insert(QStringLiteral("subtitle"),
                 QStringLiteral("%1 · %2%3–%4").arg(
                     // A stretch without listings is titled with the channel's name.
                     programme.value(QStringLiteral("placeholder")).toBool() ? tr("No guide listings") : c->name, when,
                     loc.toString(QDateTime::fromSecsSinceEpoch(start).time(), fmt),
                     loc.toString(QDateTime::fromSecsSinceEpoch(stop).time(), fmt)));
    m_vod.insert(QStringLiteral("programmeStart"), startMs);
    m_vod.insert(QStringLiteral("programmeStop"), stopMs);
    m_vodActive = true;
    emit vodChanged();
    qInfo().noquote() << QStringLiteral("Catch-up: %1 · %2 from %3 s")
                             .arg(c->name, programme.value(QStringLiteral("title")).toString())
                             .arg(qRound(offsetSecs));
    emit vodRequested(url, adjust);
    return true;
}

void PlayerController::seekCatchup(double offsetSecs)
{
    if (!m_vodActive || m_vod.value(QStringLiteral("kind")).toString() != QLatin1String("catchup")) return;
    QVariantMap programme = m_vod;   // "start" here is the in-stream skip; restore the show's times
    programme.insert(QStringLiteral("start"), m_vod.value(QStringLiteral("programmeStart")));
    programme.insert(QStringLiteral("stop"), m_vod.value(QStringLiteral("programmeStop")));
    playCatchup(m_vod.value(QStringLiteral("channelIndex")).toInt(), programme, offsetSecs);
}

bool PlayerController::playNextCatchup()
{
    if (!m_vodActive || m_vod.value(QStringLiteral("kind")).toString() != QLatin1String("catchup") || !m_epg)
        return false;
    const int idx = m_vod.value(QStringLiteral("channelIndex")).toInt();
    const Channel *c = m_channels->channel(idx);
    const double stop = m_vod.value(QStringLiteral("programmeStop")).toDouble();
    if (!c) return false;
    const double now = double(QDateTime::currentMSecsSinceEpoch());
    if (stop > now - 60 * 1000) return false;            // caught up: watch it live
    // The next listing; when there's none, the next hour under the channel's
    // name (up to the next listing), as the guide shows it.
    const QVariantList next = c->epgId.isEmpty() ? QVariantList()
                              : m_epg->programmes(c->epgId, stop - 1000, stop + 60 * 60 * 1000);
    double nextListing = 0;
    for (const QVariant &v : next) {
        const QVariantMap p = v.toMap();
        const double start = p.value(QStringLiteral("start")).toDouble();
        if (start < stop - 1000) continue;
        if (start <= stop + 60 * 1000) {
            if (start > now - 60 * 1000) return false;   // on now: watch it live
            return playCatchup(idx, p, 0);
        }
        nextListing = start;
        break;
    }
    QDateTime hour = QDateTime::fromMSecsSinceEpoch(qint64(stop));
    hour.setTime(QTime(hour.time().hour(), 0));
    hour = hour.addSecs(3600);
    double end = double(hour.toMSecsSinceEpoch());
    if (nextListing > 0) end = qMin(end, nextListing);
    if (end - stop < 60 * 1000) end = stop + 60 * 60 * 1000;
    const QVariantMap block{
        {QStringLiteral("title"), c->name},
        {QStringLiteral("start"), stop},
        {QStringLiteral("stop"), end},
        {QStringLiteral("placeholder"), true},
    };
    return playCatchup(idx, block, 0);
}

void PlayerController::stopVod()
{
    if (!m_vodActive) return;
    m_vodActive = false;
    m_vod.clear();
    emit vodChanged();
    const Channel *c = m_channels->channel(m_current);
    if (c) emit playRequested(c->url, c->userAgent, c->referrer);
    else emit stopRequested();
}

void PlayerController::typeDigit(int digit)
{
    if (digit < 0 || digit > 9) return;
    if (m_pending.isEmpty() && digit == 0) return;   // leading zero means nothing
    m_pending += QString::number(digit);
    emit pendingNumberChanged();
    if (m_pending.size() >= kMaxDigits) commitNumber();
    else m_numberTimer.start();
}

void PlayerController::commitNumber()
{
    m_numberTimer.stop();
    if (m_pending.isEmpty()) return;
    const int number = m_pending.toInt();
    m_pending.clear();
    emit pendingNumberChanged();

    const int idx = m_channels->findByNumber(number);
    if (idx < 0) emit numberNotFound(number);
    else tune(idx, ChannelModel::AllGroup);   // a typed number zaps through all channels afterwards
}

void PlayerController::cancelNumber()
{
    m_numberTimer.stop();
    if (m_pending.isEmpty()) return;
    m_pending.clear();
    emit pendingNumberChanged();
}

void PlayerController::reset()
{
    cancelNumber();
    m_current = m_previous = -1;
    m_zapGroup.clear();
    m_previousZapGroup.clear();
    emit currentChanged();
}

void PlayerController::rebindAfterReload(const QString &currentKey, const QString &previousKey,
                                         const QString &oldUrl)
{
    const int idx = currentKey.isEmpty() ? -1 : m_channels->indexOfKey(currentKey);
    if (idx < 0) {
        // The channel is gone from the playlist. A movie keeps playing; live
        // TV starts over with the remembered channel.
        restoreAfterLoad();
        return;
    }
    m_current = idx;
    m_previous = previousKey.isEmpty() ? -1 : m_channels->indexOfKey(previousKey);
    emit currentChanged();
    refreshProgram();
    const Channel *c = m_channels->channel(idx);
    if (!m_vodActive && c && c->url != oldUrl) tune(idx, zapGroup());   // e.g. the provider moved servers
}

void PlayerController::restoreAfterLoad()
{
    reset();
    if (m_channels->totalCount() == 0) return;

    const QString last = m_settings->lastChannelUrl();
    const QVector<Channel> &all = m_channels->channels();
    int idx = m_channels->step(-1, 0, ChannelModel::AllGroup);   // first visible channel
    if (!last.isEmpty()) {
        // Saved as a channel id; older versions saved the address.
        const int byKey = last.contains(QLatin1String("://")) && !last.startsWith(QLatin1String("u:"))
            ? -1 : m_channels->indexOfKey(last);
        if (byKey >= 0) idx = byKey;
        else for (int i = 0; i < all.size(); ++i)
            if (all[i].url == last) { idx = i; break; }
    }
    if (idx >= 0 && blockedNow(idx)) idx = m_channels->step(-1, 0, ChannelModel::AllGroup);   // first unlocked
    if (idx < 0) return;
    if (m_vodActive) {
        // A movie is on: remember the channel for afterwards, don't start it.
        m_current = idx;
        m_zapGroup = m_channels->groupName(m_channels->groupOfChannel(idx));
    } else {
        tune(idx);
    }
    m_previous = -1;   // nothing to go back to yet
    emit currentChanged();
}

// ---- casting ----------------------------------------------------------------------

QVariantMap PlayerController::castSource() const
{
    if (m_vodActive) {
        const QString title = m_vod.value(QStringLiteral("title")).toString();
        const QString sub = m_vod.value(QStringLiteral("subtitle")).toString();
        return {{QStringLiteral("url"), m_vod.value(QStringLiteral("url"))},
                {QStringLiteral("title"), sub.isEmpty() ? title : title + QStringLiteral(" · ") + sub},
                {QStringLiteral("live"), false},
                {QStringLiteral("userAgent"), m_settings->userAgent()}};
    }
    const Channel *c = m_channels->channel(m_current);
    if (!c) return {};
    return {{QStringLiteral("url"), c->url},
            {QStringLiteral("title"), c->name},
            {QStringLiteral("live"), true},
            {QStringLiteral("userAgent"), c->userAgent.isEmpty() ? m_settings->userAgent() : c->userAgent},
            {QStringLiteral("referrer"), c->referrer}};
}

// ---- another player ----------------------------------------------------------------

void PlayerController::openExternally()
{
    const QString program = m_settings->externalPlayer();
    if (program.isEmpty()) {
        emit externalFailed(tr("Choose the other player in Settings (S) first"));
        return;
    }
    if (!QFileInfo::exists(program)) {
        emit externalFailed(tr("The external player isn't at %1 any more").arg(QDir::toNativeSeparators(program)));
        return;
    }
    QString address;
    if (m_vodActive) address = m_vod.value(QStringLiteral("url")).toString();
    else if (const Channel *c = m_channels->channel(m_current)) address = c->url;
    if (address.isEmpty()) { emit externalFailed(tr("Nothing is playing")); return; }

    auto launch = [this, program](const QString &url) {
        // A local recording is a file path; streams are addresses.
        const QString target = QFileInfo::exists(url) ? QDir::toNativeSeparators(url) : url;
        bool ok;
#ifdef Q_OS_MACOS
        if (program.endsWith(QLatin1String(".app")))
            ok = QProcess::startDetached(QStringLiteral("open"), {QStringLiteral("-a"), program, target});
        else
#endif
            ok = QProcess::startDetached(program, {target});
        const QString name = QFileInfo(program).completeBaseName();
        qInfo().noquote() << "Playback: opened in external player" << name << (ok ? "" : "(failed to start)");
        if (ok) emit externalOpened(name);
        else emit externalFailed(tr("Couldn't start %1").arg(name));
    };
    if (PortalClient::isPortalUrl(address)) {
        PortalClient::instance()->resolve(address, this, [this, launch](const QString &real, const QString &error) {
            if (real.isEmpty()) emit externalFailed(error);
            else launch(real);
        });
        return;
    }
    launch(address);
}
