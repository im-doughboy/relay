#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QSettings>

class ChannelModel;
class UserLibrary;

// Parental PIN. Channels, playlist groups and movie/series categories can
// be locked; while the PIN hasn't been entered they don't play (Relay asks
// for the PIN), zapping skips them and their programme details are hidden.
// Entering the PIN unlocks everything until Relay closes or lock() is
// called. The PIN is stored as a salted hash in the settings file.
class Parental : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool hasPin READ hasPin NOTIFY changed)
    // True when nothing is held back: no PIN, or the PIN was entered.
    Q_PROPERTY(bool unlocked READ unlocked NOTIFY changed)
    Q_PROPERTY(bool lockSettings READ lockSettings WRITE setLockSettings NOTIFY changed)
    Q_PROPERTY(int lockCount READ lockCount NOTIFY changed)
    // Bumped whenever locks or the unlocked state change (for QML bindings).
    Q_PROPERTY(int revision READ revision NOTIFY changed)

public:
    Parental(ChannelModel *channels, UserLibrary *library, QObject *parent = nullptr);

    bool hasPin() const;
    bool unlocked() const { return !hasPin() || m_unlocked; }
    bool lockSettings() const;
    void setLockSettings(bool on);
    int lockCount() const;
    int revision() const { return m_revision; }

    // Checks the PIN; on success everything is unlocked. After five wrong
    // tries, waits 30 seconds before accepting another (returns false and
    // retryWait() > 0).
    Q_INVOKABLE bool enterPin(const QString &pin);
    Q_INVOKABLE int retryWait() const;
    Q_INVOKABLE bool setPin(const QString &currentPin, const QString &newPin);   // current ignored without a PIN
    Q_INVOKABLE bool removePin(const QString &currentPin);
    Q_INVOKABLE void lock();

    Q_INVOKABLE bool isChannelLocked(int globalIndex) const;   // marked as locked
    Q_INVOKABLE bool blocked(int globalIndex) const;           // locked and PIN not entered
    Q_INVOKABLE bool toggleChannelLock(int globalIndex);       // returns the new state
    Q_INVOKABLE bool toggleGroupLock(int group);
    // Movie/series categories: section 0 movies, 1 series.
    Q_INVOKABLE bool isCategoryLocked(int section, const QString &categoryId) const;
    Q_INVOKABLE bool categoryBlocked(int section, const QString &categoryId) const;
    Q_INVOKABLE bool toggleCategoryLock(int section, const QString &categoryId);

    static QString hash(const QString &pin, const QByteArray &salt);
    static bool validPin(const QString &pin);   // 4 digits

signals:
    void changed();

private:
    void notify();

    ChannelModel *m_channels;
    UserLibrary *m_library;
    QSettings m_s;
    bool m_unlocked = false;
    int m_failures = 0;
    QElapsedTimer m_lockout;
    int m_revision = 0;
};
