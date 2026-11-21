#include "app/AppSettings.h"
#include "CatchupService.h"

#include "XtreamParser.h"
#include "models/ChannelModel.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QtConcurrent>

namespace {
QNetworkRequest request(const QUrl &url)
{
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader,
                  AppSettings::effectiveUserAgent());
    req.setTransferTimeout(60000);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    return req;
}
} // namespace

CatchupService::CatchupService(ChannelModel *channels, QObject *parent)
    : QObject(parent), m_channels(channels), m_net(new QNetworkAccessManager(this))
{
}

void CatchupService::clear()
{
    ++m_generation;
    m_account = {};
    m_pending = {};
    m_accountKey.clear();
    m_days.clear();
    m_haveList = m_needsList = false;
    m_channels->setCatchupAccount({});
    if (m_count) { m_count = 0; emit changed(); }
}

void CatchupService::setSource(const PlaylistResult &result)
{
    const QString key = result.xtreamServer.isEmpty()
        ? QString() : XtreamParser::normalizeServer(result.xtreamServer).toString()
                          + QLatin1Char('\n') + result.xtreamUser + QLatin1Char('\n') + result.xtreamPassword;
    // Does the playlist itself say which channels keep an archive?
    bool listed = false;
    for (const Channel &c : result.channels)
        if (!c.catchupType.isEmpty()) { listed = true; break; }

    if (key != m_accountKey || key.isEmpty()) {
        clear();
        m_accountKey = key;
        if (!key.isEmpty()) {
            m_pending.server = result.xtreamServer;
            m_pending.username = result.xtreamUser;
            m_pending.password = result.xtreamPassword;
            m_needsList = !listed;
            fetch();
        }
    } else {
        m_needsList = !listed;
        if (m_needsList && !m_haveList) fetch();
    }
    apply();
}

void CatchupService::fetch()
{
    const quint64 gen = m_generation;
    XtreamParser::Credentials c{m_pending.server, m_pending.username, m_pending.password};
    QNetworkReply *auth = m_net->get(request(XtreamParser::apiUrl(c)));
    connect(auth, &QNetworkReply::finished, this, [this, auth, gen, c] {
        auth->deleteLater();
        if (gen != m_generation) return;
        if (auth->error() != QNetworkReply::NoError) {
            qInfo().noquote() << "Catch-up: server info unavailable:" << auth->errorString();
            return;
        }
        const auto a = XtreamParser::parseAuth(auth->readAll());
        if (!a.ok) { qInfo().noquote() << "Catch-up: login refused:" << a.message; return; }

        Catchup::Account acc = m_pending;
        if (!a.timezone.isEmpty()) acc.zone = QTimeZone(a.timezone.toUtf8());
        QDateTime serverNow = QDateTime::fromString(a.timeNow, QStringLiteral("yyyy-MM-dd HH:mm:ss"));
        if (serverNow.isValid()) {
            serverNow.setTimeZone(QTimeZone::utc());
            const qint64 diff = serverNow.toSecsSinceEpoch() - QDateTime::currentSecsSinceEpoch();
            acc.offsetSecs = int(qRound(diff / 900.0) * 900);   // nearest quarter hour
        }
        m_account = acc;
        qInfo().noquote() << QStringLiteral("Catch-up: server time zone %1 (UTC%2%3 min)")
                                 .arg(a.timezone.isEmpty() ? QStringLiteral("not given") : a.timezone,
                                      acc.offsetSecs >= 0 ? QStringLiteral("+") : QString())
                                 .arg(acc.offsetSecs / 60);
        apply();
        if (!m_needsList || m_haveList) return;

        // The playlist didn't say which channels have an archive: ask.
        QTimer::singleShot(4000, this, [this, gen, c] {
            if (gen != m_generation) return;
            QNetworkReply *list = m_net->get(request(XtreamParser::apiUrl(c, QStringLiteral("get_live_streams"))));
            connect(list, &QNetworkReply::finished, this, [this, list, gen] {
                list->deleteLater();
                if (gen != m_generation) return;
                if (list->error() != QNetworkReply::NoError) {
                    qInfo().noquote() << "Catch-up: channel list unavailable:" << list->errorString();
                    return;
                }
                const QByteArray data = list->readAll();
                QtConcurrent::run([data] { return XtreamParser::parseArchiveDays(data); })
                    .then(this, [this, gen](QHash<QString, int> days) {
                        if (gen != m_generation) return;
                        m_days = std::move(days);
                        m_haveList = true;
                        apply();
                    });
            });
        });
    });
}

void CatchupService::apply()
{
    m_channels->setCatchupAccount(m_account);
    if (m_needsList && m_haveList) {
        const int n = m_channels->applyArchiveDays(m_days);
        qInfo().noquote() << QStringLiteral("Catch-up: %1 channels keep an archive").arg(n);
    }
    const int count = m_channels->catchupChannelCount();
    if (count != m_count) { m_count = count; emit changed(); }
}

int CatchupService::days(int globalIndex) const
{
    const Channel *c = m_channels->channel(globalIndex);
    return c && Catchup::supported(*c, m_account) ? c->catchupDays : 0;
}

bool CatchupService::available(int globalIndex, double startMs) const
{
    const int d = days(globalIndex);
    if (d <= 0) return false;
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    const qint64 start = qint64(startMs / 1000);
    return start < now && start >= now - qint64(d) * 86400;
}

QString CatchupService::url(int globalIndex, qint64 start, qint64 stop, int *startAdjust) const
{
    const Channel *c = m_channels->channel(globalIndex);
    if (!c) return QString();
    return Catchup::url(*c, start, stop, QDateTime::currentSecsSinceEpoch(), m_account, startAdjust);
}
