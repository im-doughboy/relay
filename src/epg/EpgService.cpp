#include "EpgService.h"

#include "EpgMatcher.h"
#include "EpgStore.h"
#include "XmltvParser.h"
#include "app/AppSettings.h"
#include "app/Log.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPointer>
#include <QTemporaryFile>
#include <QThread>
#include <QUrl>
#include <QtConcurrent>

namespace {

constexpr qint64 kKeepPast = 2 * 24 * 3600;      // keep 2 days back (catch-up later)
constexpr int kTimeoutMs = 90000;

qint64 nowSecs() { return QDateTime::currentSecsSinceEpoch(); }

} // namespace

EpgService::EpgService(const QString &databasePath, QObject *parent)
    : QObject(parent),
      m_dbPath(databasePath),
      m_store(std::make_unique<EpgStore>(databasePath, QStringLiteral("relay-epg-ui"))),
      m_net(new QNetworkAccessManager(this))
{
    if (!m_store->isOpen())
        qWarning() << "Guide database could not be opened:" << m_store->errorString();
    reloadStats();
}

EpgService::~EpgService()
{
    cancel();
}

void EpgService::setState(bool busy, const QString &status)
{
    if (busy == m_busy && status == m_status) return;
    m_busy = busy;
    m_status = status;
    emit stateChanged();
}

void EpgService::reloadStats()
{
    if (!m_store) return;
    m_programmeCount = m_store->programmeCount();
    const qint64 updated = m_store->meta(QStringLiteral("updated")).toLongLong();
    m_lastUpdated = updated > 0 ? QDateTime::fromSecsSinceEpoch(updated) : QDateTime();
    m_cache.clear();
}

void EpgService::shutdown()
{
    cancel();
    m_cache.clear();
    m_programmeCount = 0;
    m_store.reset();
}

void EpgService::setDatabase(const QString &path)
{
    if (path == m_dbPath && m_store) return;
    cancel();
    m_store.reset();                         // closes the old connection first
    m_dbPath = path;
    m_store = std::make_unique<EpgStore>(path, QStringLiteral("relay-epg-ui"));
    if (!m_store->isOpen())
        qWarning() << "Guide database could not be opened:" << m_store->errorString();
    m_lastError.clear();
    reloadStats();
    emit stateChanged();
    emit guideUpdated();
}

void EpgService::cancel()
{
    ++m_generation;
    if (m_cancelled) m_cancelled->store(true);
    m_downloads.clear();
    if (m_busy) setState(false, QString());
}

EpgService::Wanted EpgService::wantedFor(const QVector<Channel> &channels)
{
    Wanted w;
    for (const Channel &c : channels) {
        if (!c.tvgId.isEmpty()) w.tvgIds.insert(c.tvgId.toLower());
        const QString n = EpgMatcher::normalize(c.name);
        if (!n.isEmpty()) w.names.insert(n);
        if (!c.tvgId.isEmpty()) {
            // Some playlists put the channel name in tvg-id.
            const QString idAsName = EpgMatcher::normalize(c.tvgId);
            if (!idAsName.isEmpty()) w.names.insert(idAsName);
        }
    }
    return w;
}

QString EpgService::fingerprint(const QVector<Channel> &channels, const QString &url)
{
    QCryptographicHash h(QCryptographicHash::Sha1);
    h.addData(url.toUtf8());
    for (const Channel &c : channels) {
        h.addData(c.tvgId.toUtf8());
        h.addData(QByteArrayView("\x1f", 1));
        h.addData(c.name.toUtf8());
        h.addData(QByteArrayView("\x1e", 1));
    }
    return QString::fromLatin1(h.result().toHex());
}

QString EpgService::fingerprint(const QVector<Channel> &channels, const QStringList &urls,
                                const QSet<QString> &extraIds)
{
    QStringList extra(extraIds.cbegin(), extraIds.cend());
    extra.sort();
    // One address and nothing chosen by hand: the same as before, so guides
    // stored by earlier versions still count as current.
    return fingerprint(channels, extra.isEmpty() ? urls.join(QLatin1Char('\n'))
                                                 : urls.join(QLatin1Char('\n')) + QLatin1Char('\x1d') + extra.join(QLatin1Char('\x1f')));
}

void EpgService::setOffsetMinutes(int minutes)
{
    const qint64 secs = qint64(qBound(-720, minutes, 720)) * 60;
    if (secs == m_offsetSecs) return;
    m_offsetSecs = secs;
    m_cache.clear();
    emit guideUpdated();
}

void EpgService::refresh(const QVector<Channel> &channels, const QStringList &urls, bool force,
                         const QSet<QString> &extraIds)
{
    QStringList sources;
    for (const QString &u : urls)
        if (!u.trimmed().isEmpty() && !sources.contains(u.trimmed())) sources << u.trimmed();
    if (sources.isEmpty()) {
        qInfo() << "Guide: no guide address for this playlist";
        m_lastError = tr("This playlist doesn't include a guide address. "
                         "Enter one in the \"Guide (EPG) address\" field in Playlist settings (S).");
        emit stateChanged();
        if (force) emit importFailed(m_lastError);
        return;
    }

    if (!m_store) return;
    const QString print = fingerprint(channels, sources, extraIds);
    const qint64 updated = m_store->meta(QStringLiteral("updated")).toLongLong();
    const bool fresh = m_store->meta(QStringLiteral("fingerprint")) == print
        && nowSecs() - updated < m_refreshAfter;
    if (fresh && !force) {
        qInfo().noquote() << "Guide: stored guide is current, updated"
                          << m_lastUpdated.toString(Qt::ISODate);
        return;
    }
    if (m_busy && !force && print == m_pendingFingerprint) return;   // already on its way

    cancel();
    m_importClock.start();
    m_pendingFingerprint = print;
    m_lastError.clear();
    emit stateChanged();
    Wanted wanted = wantedFor(channels);
    for (const QString &id : extraIds) wanted.tvgIds.insert(id.toLower());
    QHash<QString, QString> meta{
        {QStringLiteral("url"), sources.join(QLatin1Char('\n'))},
        {QStringLiteral("fingerprint"), print},
    };
    fetchNext(m_generation, sources, 0, {}, {}, wanted, meta);
}

void EpgService::fetchNext(quint64 gen, const QStringList &sources, int index, QStringList files,
                           QStringList problems, const Wanted &wanted, const QHash<QString, QString> &meta)
{
    if (gen != m_generation) return;
    if (index >= sources.size()) {
        if (files.isEmpty()) {
            ImportResult r;
            r.error = problems.join(QLatin1Char(' '));
            m_downloads.clear();
            finish(r);
            return;
        }
        if (!problems.isEmpty()) qWarning().noquote() << "Guide:" << problems.join(QLatin1Char(' '));
        startImport(files, wanted, meta);
        return;
    }
    const QString source = sources[index];
    const QString which = sources.size() > 1 ? tr(" %1 of %2").arg(index + 1).arg(sources.size()) : QString();
    qInfo().noquote() << "Guide: updating from" << Log::redact(source);

    const QUrl qurl = QUrl::fromUserInput(source, QString(), QUrl::AssumeLocalFile);
    if (qurl.isLocalFile()) {
        const QString path = qurl.toLocalFile();
        if (QFileInfo::exists(path)) files << path;
        else problems << tr("Guide file not found: %1").arg(path);
        fetchNext(gen, sources, index + 1, files, problems, wanted, meta);
        return;
    }

    auto file = std::make_unique<QTemporaryFile>();
    if (!file->open()) {
        problems << tr("Couldn't create a temporary file for the guide.");
        fetchNext(gen, sources, index + 1, files, problems, wanted, meta);
        return;
    }
    QTemporaryFile *download = file.get();
    m_downloads.push_back(std::move(file));

    setState(true, tr("Downloading guide%1…").arg(which));
    QNetworkRequest req(qurl);
    req.setHeader(QNetworkRequest::UserAgentHeader,
                  AppSettings::effectiveUserAgent());
    req.setTransferTimeout(kTimeoutMs);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply *reply = m_net->get(req);

    // `download` stays valid while the generation is current (cancel() bumps
    // it before dropping the files).
    connect(reply, &QNetworkReply::readyRead, this, [this, reply, gen, download] {
        if (gen != m_generation) { reply->abort(); return; }
        download->write(reply->readAll());
    });
    connect(reply, &QNetworkReply::downloadProgress, this, [this, gen, which](qint64 got, qint64) {
        if (gen == m_generation && got > 0)
            setState(true, tr("Downloading guide%1… %2 MB").arg(which).arg(got / 1048576.0, 0, 'f', 1));
    });
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, gen, download, sources, index, files, problems, wanted, meta]() mutable {
        reply->deleteLater();
        if (gen != m_generation) return;
        if (reply->error() != QNetworkReply::NoError) {
            const int http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            problems << (http >= 400 ? tr("Guide download failed (HTTP %1).").arg(http)
                                     : tr("Guide download failed: %1").arg(reply->errorString()));
        } else {
            download->write(reply->readAll());
            download->flush();
            qInfo().noquote() << QStringLiteral("Guide: downloaded %1 MB")
                                     .arg(download->size() / 1048576.0, 0, 'f', 1);
            files << download->fileName();
            download->close();   // the file stays until m_downloads is cleared; lets the importer open it on Windows
        }
        fetchNext(gen, sources, index + 1, files, problems, wanted, meta);
    });
}

void EpgService::startImport(const QStringList &files, const Wanted &wanted,
                             const QHash<QString, QString> &meta)
{
    const quint64 gen = m_generation;
    m_cancelled = std::make_shared<std::atomic_bool>(false);
    auto cancelled = m_cancelled;
    setState(true, tr("Updating guide…"));

    QPointer<EpgService> self(this);
    auto progress = [self, gen](int percent) {
        QMetaObject::invokeMethod(qApp, [self, gen, percent] {
            if (self && gen == self->m_generation && self->m_busy)
                self->setState(true, tr("Updating guide… %1%").arg(percent));
        }, Qt::QueuedConnection);
    };

    const QString db = m_dbPath;
    const qint64 keepFuture = m_keepFuture;
    QtConcurrent::run([files, db, wanted, meta, keepFuture, cancelled, progress] {
        return runImport(files, db, wanted, meta, keepFuture, cancelled, progress);
    }).then(this, [this, gen](ImportResult r) {
        if (gen != m_generation) return;
        m_downloads.clear();
        finish(r);
    });
}

void EpgService::finish(const ImportResult &r)
{
    setState(false, QString());
    if (!r.ok) {
        qWarning().noquote() << "Guide: update failed:" << r.error;
        m_lastError = r.error;
        emit stateChanged();
        emit importFailed(r.error);
        return;
    }
    if (!r.warning.isEmpty()) qWarning().noquote() << "Guide:" << r.warning;
    qInfo().noquote() << QStringLiteral("Guide: imported %1 programmes in %2 s")
                             .arg(r.programmes).arg(m_importClock.elapsed() / 1000.0, 0, 'f', 1);
    reloadStats();
    emit guideUpdated();
}

EpgService::ImportResult EpgService::runImport(const QStringList &files, const QString &databasePath,
                                               const Wanted &wanted,
                                               const QHash<QString, QString> &metaIn,
                                               qint64 keepFuture,
                                               std::shared_ptr<std::atomic_bool> cancelled,
                                               const std::function<void(int)> &progress)
{
    ImportResult result;
    const QString conn = QStringLiteral("relay-epg-import-%1")
                             .arg(quintptr(QThread::currentThreadId()));
    EpgStore store(databasePath, conn);
    if (!store.isOpen() || !store.beginImport()) {
        result.error = tr("Couldn't write the guide database: %1").arg(store.errorString());
        return result;
    }

    const qint64 now = nowSecs();
    const qint64 keepFrom = now - kKeepPast;
    const qint64 keepTo = now + keepFuture;
    QHash<QString, int> kept;   // guide channel id -> the file its programmes come from
    QSet<QString> declared;     // every <channel> id seen in the current file
    qint64 totalBytes = 0, doneBytes = 0;
    for (const QString &f : files) totalBytes += QFileInfo(f).size();
    totalBytes = qMax<qint64>(1, totalBytes);
    qint64 parsedProgrammes = 0;
    QStringList problems;
    int lastPercent = -1;

    for (int fileIndex = 0; fileIndex < files.size(); ++fileIndex) {
        QFile in(files[fileIndex]);
        if (!in.open(QIODevice::ReadOnly)) {
            problems << tr("Couldn't read the guide file.");
            continue;
        }
        declared.clear();
        XmltvParser parser;
        parser.onChannel = [&](const EpgChannel &c) {
            declared.insert(c.id);
            store.addListed(c.id, c.names.value(0));
            QStringList norms;
            bool want = wanted.tvgIds.contains(c.id.toLower());
            for (const QString &name : c.names) {
                const QString n = EpgMatcher::normalize(name);
                norms << n;
                if (wanted.names.contains(n)) want = true;
            }
            const QString idNorm = EpgMatcher::normalize(c.id);
            if (!want && wanted.names.contains(idNorm)) want = true;
            norms << idNorm;
            if (want && !kept.contains(c.id)) {     // an earlier file's channel wins
                kept.insert(c.id, fileIndex);
                norms.removeDuplicates();
                store.addChannel(c, norms);
            }
        };
        parser.onProgramme = [&](const Programme &p) {
            const auto it = kept.constFind(p.channel);
            if (it == kept.constEnd()) {
                // Programmes for a channel the file never declared still count if
                // a playlist tvg-id points at it.
                if (declared.contains(p.channel) || !wanted.tvgIds.contains(p.channel.toLower()))
                    return;
                declared.insert(p.channel);
                kept.insert(p.channel, fileIndex);
                EpgChannel c;
                c.id = p.channel;
                store.addChannel(c, {});
                store.addListed(c.id, QString());
            } else if (it.value() != fileIndex) {
                return;                              // listed by an earlier file already
            }
            const qint64 end = p.stop > 0 ? p.stop : p.start + 6 * 3600;
            if (end < keepFrom || p.start > keepTo) return;
            store.addProgramme(p);
            ++result.programmes;
        };

        while (!in.atEnd()) {
            if (cancelled->load()) {
                store.rollbackImport();
                result.error = tr("Guide update cancelled.");
                return result;
            }
            parser.feed(in.read(64 * 1024));   // small chunks keep memory flat (gzip expands ~30x)
            if (parser.failed()) break;
            const int percent = int((doneBytes + in.pos()) * 100 / totalBytes);
            if (percent != lastPercent) {
                lastPercent = percent;
                progress(percent);
            }
        }
        doneBytes += in.size();
        if (!parser.finish() || !parser.errorString().isEmpty()) problems << parser.errorString();
        parsedProgrammes += parser.programmeCount();
    }

    if (result.programmes == 0) {
        store.rollbackImport();
        result.error = parsedProgrammes > 0
            ? tr("The guide loaded, but none of its channels match this playlist.")
            : !problems.isEmpty() ? problems.first()
            : tr("The guide file has no programmes.");
        return result;
    }

    QHash<QString, QString> meta = metaIn;
    meta.insert(QStringLiteral("updated"), QString::number(now));
    if (!store.commitImport(meta)) {
        result.error = tr("Couldn't save the guide: %1").arg(store.errorString());
        return result;
    }
    result.warning = problems.join(QLatin1Char(' '));
    result.ok = true;
    return result;
}

// A channel's own offset rides along in its guide id: "<id>\x1f<minutes>".
QString EpgService::withOffset(const QString &id, int minutes)
{
    return id.isEmpty() || minutes == 0 ? id : id + QChar(0x1f) + QString::number(minutes);
}

QString EpgService::baseId(const QString &epgId)
{
    const int i = epgId.lastIndexOf(QChar(0x1f));
    return i < 0 ? epgId : epgId.left(i);
}

qint64 EpgService::offsetSecs(const QString &epgId) const
{
    const int i = epgId.lastIndexOf(QChar(0x1f));
    return m_offsetSecs + (i < 0 ? 0 : epgId.mid(i + 1).toLongLong() * 60);
}

bool EpgService::hasChannel(const QString &id) const
{
    return m_store && m_store->hasChannel(id);
}

QVariantList EpgService::guideChannels(const QString &text, int limit) const
{
    QVariantList out;
    if (!m_store) return out;
    for (const auto &c : m_store->listed(text.trimmed(), limit))
        out.append(QVariantMap{{QStringLiteral("id"), c.first},
                               {QStringLiteral("name"), c.second.isEmpty() ? c.first : c.second}});
    return out;
}

QVector<QString> EpgService::resolve(const QVector<Channel> &channels,
                                     const QHash<int, QString> &overrides,
                                     const QHash<int, int> &offsets) const
{
    QVector<QString> ids(channels.size());
    if (m_programmeCount == 0 || !m_store) return ids;
    const QHash<QString, QString> byId = m_store->channelIdsByLowercase();
    const QHash<QString, QString> byName = m_store->channelIdsByName();
    for (int i = 0; i < channels.size(); ++i) {
        const Channel &c = channels[i];
        QString id;
        const QString chosen = overrides.value(i);
        if (chosen == QLatin1String("-")) continue;                 // "no listings"
        if (!chosen.isEmpty()) id = byId.value(chosen.toLower());   // falls back to automatic until it's imported
        if (id.isEmpty() && !c.tvgId.isEmpty()) id = byId.value(c.tvgId.toLower());
        if (id.isEmpty()) id = byName.value(EpgMatcher::normalize(c.name));
        if (id.isEmpty() && !c.tvgId.isEmpty()) id = byName.value(EpgMatcher::normalize(c.tvgId));
        ids[i] = withOffset(id, offsets.contains(i) ? offsets.value(i) : c.tvgShiftMinutes);
    }
    return ids;
}

EpgService::NowNext EpgService::nowNext(const QString &epgId) const
{
    if (epgId.isEmpty() || m_programmeCount == 0 || !m_store) return {};
    const qint64 now = nowSecs();
    auto it = m_cache.constFind(epgId);
    if (it != m_cache.constEnd() && now < it->validUntil) return it->value;

    NowNext nn;
    const qint64 off = offsetSecs(epgId);
    QVector<Programme> list = m_store->upcoming(baseId(epgId), now - off, 2);
    for (Programme &p : list) { p.start += off; p.stop += off; }
    int i = 0;
    if (i < list.size() && list[i].start <= now) { nn.now = list[i++]; nn.hasNow = true; }
    if (i < list.size()) { nn.next = list[i]; nn.hasNext = true; }

    CacheEntry e;
    e.value = nn;
    e.validUntil = nn.hasNow ? nn.now.stop : (nn.hasNext ? nn.next.start : now + 300);
    m_cache.insert(epgId, e);
    return nn;
}

QVector<Programme> EpgService::search(const QString &text, int limit) const
{
    if (!m_store || m_programmeCount == 0 || text.isEmpty()) return {};
    QVector<Programme> list = m_store->search(text, nowSecs() - m_offsetSecs, limit);
    for (Programme &p : list) { p.start += m_offsetSecs; p.stop += m_offsetSecs; }
    return list;
}

QVariantList EpgService::programmes(const QString &epgId, double fromMs, double toMs) const
{
    QVariantList out;
    const QVector<Programme> list = range(epgId, qint64(fromMs / 1000), qint64(toMs / 1000) + 1);
    out.reserve(list.size());
    for (const Programme &p : list) {
        out.append(QVariantMap{
            {QStringLiteral("title"), p.title},
            {QStringLiteral("subTitle"), p.subTitle},
            {QStringLiteral("desc"), p.desc},
            {QStringLiteral("category"), p.category},
            {QStringLiteral("episode"), p.episode},
            {QStringLiteral("start"), double(p.start) * 1000.0},
            {QStringLiteral("stop"), double(p.stop) * 1000.0},
        });
    }
    return out;
}

QVector<Programme> EpgService::range(const QString &epgId, qint64 from, qint64 to) const
{
    if (epgId.isEmpty() || !m_store) return {};
    const qint64 off = offsetSecs(epgId);
    QVector<Programme> list = m_store->range(baseId(epgId), from - off, to - off);
    for (Programme &p : list) { p.start += off; p.stop += off; }
    return list;
}
