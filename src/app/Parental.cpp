#include "Parental.h"

#include "UserLibrary.h"
#include "models/ChannelModel.h"

#include <QCryptographicHash>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QtDebug>

namespace {
constexpr int kMaxFailures = 5;
constexpr int kLockoutMs = 30000;
}

Parental::Parental(ChannelModel *channels, UserLibrary *library, QObject *parent)
    : QObject(parent), m_channels(channels), m_library(library)
{
    m_channels->setBlockedCheck([this](int gi) { return blocked(gi); });
}

QString Parental::hash(const QString &pin, const QByteArray &salt)
{
    // Many rounds so a copied settings file doesn't give the PIN away instantly.
    QByteArray h = salt + pin.toUtf8();
    for (int i = 0; i < 20000; ++i) h = QCryptographicHash::hash(h + salt, QCryptographicHash::Sha256);
    return QString::fromLatin1(h.toHex());
}

bool Parental::validPin(const QString &pin)
{
    static const QRegularExpression re(QStringLiteral("^\\d{4}$"));
    return re.match(pin).hasMatch();
}

bool Parental::hasPin() const { return !m_s.value("parental/pinHash").toString().isEmpty(); }
bool Parental::lockSettings() const { return m_s.value("parental/lockSettings", true).toBool(); }

void Parental::setLockSettings(bool on)
{
    if (on == lockSettings()) return;
    m_s.setValue("parental/lockSettings", on);
    notify();
}

int Parental::lockCount() const { return m_library->lockCount(); }

void Parental::notify()
{
    ++m_revision;
    m_channels->notifyLocksChanged();
    emit changed();
}

int Parental::retryWait() const
{
    if (m_failures < kMaxFailures || !m_lockout.isValid()) return 0;
    const qint64 left = kLockoutMs - m_lockout.elapsed();
    return left > 0 ? int((left + 999) / 1000) : 0;
}

bool Parental::enterPin(const QString &pin)
{
    if (!hasPin()) return true;
    if (retryWait() > 0) return false;
    const QByteArray salt = QByteArray::fromHex(m_s.value("parental/salt").toByteArray());
    if (hash(pin, salt) == m_s.value("parental/pinHash").toString()) {
        m_failures = 0;
        if (!m_unlocked) { m_unlocked = true; qInfo() << "Parental: unlocked"; notify(); }
        return true;
    }
    ++m_failures;
    if (m_failures >= kMaxFailures) m_lockout.start();
    qInfo().noquote() << "Parental: wrong PIN";
    return false;
}

bool Parental::setPin(const QString &currentPin, const QString &newPin)
{
    if (!validPin(newPin)) return false;
    if (hasPin() && !enterPin(currentPin)) return false;
    QByteArray salt(16, 0);
    for (char &c : salt) c = char(QRandomGenerator::system()->bounded(256));
    m_s.setValue("parental/salt", QString::fromLatin1(salt.toHex()));
    m_s.setValue("parental/pinHash", hash(newPin, salt));
    m_unlocked = true;      // the person who just set it can carry on
    qInfo() << "Parental: PIN set";
    notify();
    return true;
}

bool Parental::removePin(const QString &currentPin)
{
    if (!hasPin()) return true;
    if (!enterPin(currentPin)) return false;
    m_s.remove("parental/pinHash");
    m_s.remove("parental/salt");
    qInfo() << "Parental: PIN removed (locks are kept but inactive)";
    notify();
    return true;
}

void Parental::lock()
{
    if (!hasPin() || !m_unlocked) return;
    m_unlocked = false;
    qInfo() << "Parental: locked";
    notify();
}

bool Parental::isChannelLocked(int globalIndex) const { return m_channels->isLocked(globalIndex); }

bool Parental::blocked(int globalIndex) const
{
    return !unlocked() && m_channels->isLocked(globalIndex);
}

bool Parental::toggleChannelLock(int globalIndex)
{
    const QString key = m_channels->keyAt(globalIndex);
    const bool locked = !m_library->isChannelLocked(key);
    m_library->setChannelLocked(key, locked);
    notify();
    return locked;
}

bool Parental::toggleGroupLock(int group)
{
    if (group <= ChannelModel::AllGroup) return false;
    const QString name = m_channels->groupName(group);
    const bool locked = !m_library->isGroupLocked(name);
    m_library->setGroupLocked(name, locked);
    notify();
    return locked;
}

bool Parental::isCategoryLocked(int section, const QString &categoryId) const
{
    return m_library->isVodCategoryLocked(QString::number(section) + QLatin1Char(':') + categoryId);
}

bool Parental::categoryBlocked(int section, const QString &categoryId) const
{
    return !unlocked() && isCategoryLocked(section, categoryId);
}

bool Parental::toggleCategoryLock(int section, const QString &categoryId)
{
    const QString key = QString::number(section) + QLatin1Char(':') + categoryId;
    const bool locked = !m_library->isVodCategoryLocked(key);
    m_library->setVodCategoryLocked(key, locked);
    notify();
    return locked;
}
