// The LG remote has few buttons Relay can have (webOS keeps GUIDE, 123, AI and
// ? for itself), so on the TV OK also works held down:
//
//   hold OK -> TV guide
//
// A short press still does what OK (Return) always does; it reaches Relay when
// the button comes up. The long press becomes the remote's Guide key, which
// Relay already knows (src/app/RemoteInput.cpp). Back can't do the same:
// holding it is the TV's own "leave this app".

#include <QCoreApplication>
#include <QKeyEvent>
#include <QPointer>
#include <QTimer>
#include <QWindow>

namespace {

class TvLongPress : public QObject
{
public:
    explicit TvLongPress(QObject *parent) : QObject(parent)
    {
        m_timer.setSingleShot(true);
        m_timer.setInterval(600);
        connect(&m_timer, &QTimer::timeout, this, &TvLongPress::held);
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (m_replaying || (event->type() != QEvent::KeyPress && event->type() != QEvent::KeyRelease)
            || !watched->isWindowType())
            return false;
        auto *k = static_cast<QKeyEvent *>(event);
        if (k->key() != Qt::Key_Return || k->modifiers() != Qt::NoModifier) return false;

        if (event->type() == QEvent::KeyPress) {
            if (m_down) return true;   // held: the remote repeats it
            m_down = true;
            m_window = static_cast<QWindow *>(watched);
            m_fired = false;
            m_timer.start();
            return true;
        }
        if (!m_down) return false;
        m_timer.stop();
        m_down = false;
        if (!m_fired) send(Qt::Key_Return);   // a short press: OK as usual
        return true;
    }

private:
    void held()
    {
        m_fired = true;
        qInfo("Remote: OK held: TV guide");
        send(Qt::Key_Guide);
    }
    void send(int key)
    {
        if (!m_window) return;
        m_replaying = true;
        QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier);
        QKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier);
        QCoreApplication::sendEvent(m_window, &press);
        if (m_window) QCoreApplication::sendEvent(m_window, &release);
        m_replaying = false;
    }

    QTimer m_timer;
    QPointer<QWindow> m_window;
    bool m_down = false;
    bool m_fired = false;
    bool m_replaying = false;
};

void startTvRemote()
{
    QCoreApplication::instance()->installEventFilter(new TvLongPress(QCoreApplication::instance()));
}
} // namespace

Q_COREAPP_STARTUP_FUNCTION(startTvRemote)
