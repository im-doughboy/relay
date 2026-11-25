// Where Relay is when its main thread is stuck (the screen and the remote
// stop answering): a watchdog thread notices a missed heartbeat and samples
// the main thread with a signal, writing where it is to <data>/freeze.log.
// The addresses are Relay's own code (Qt is built in), to look up in the
// unstripped relay.debug that build-app.sh keeps. Relay's log gets one line
// per freeze with how long it lasted.
//
// The same samples make a CPU profile of the main thread, for working on
// smoothness: while the file <data>/profile-on exists, one every 10 ms goes
// to <data>/profile.log.

#include "TvPlatform.h"
#include "TvStack.h"

#include <QCoreApplication>
#include <QTimer>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/syscall.h>
#include <thread>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

namespace {
std::atomic<long long> g_beat{0};   // the main thread's last heartbeat (ms); 0: not watching
pid_t g_mainTid = 0;
int g_fd = -1;                      // freeze.log
std::atomic<int> g_sampleFd{-1};    // where samples go: freeze.log, or profile.log while profiling
char g_profileFlag[512];            // <data>/profile-on: while it exists, sample every 10 ms
char g_profilePath[512];
TvSafeReader g_stack;               // the main thread's stack, read without running off its end
std::atomic<bool> g_stop{false};

long long nowMs()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

void writeTo(int fd, const char *s) { if (fd >= 0 && write(fd, s, strlen(s)) < 0) {} }
void writeAll(const char *s) { writeTo(g_fd, s); }

// On the main thread, when the watchdog asks: where it is right now.
void onSample(int, siginfo_t *, void *context)
{
    const int fd = g_sampleFd.load();
    const auto *uc = static_cast<const ucontext_t *>(context);
    char line[160];
    // r7: the system call number, when it was in one (poll = idle, others = waiting).
    snprintf(line, sizeof line, "sample: pc %08lx lr %08lx r7 %lu stack:", static_cast<unsigned long>(uc->uc_mcontext.arm_pc),
             static_cast<unsigned long>(uc->uc_mcontext.arm_lr), static_cast<unsigned long>(uc->uc_mcontext.arm_r7));
    writeTo(fd, line);
    if (fd >= 0) tvWriteStackWords(fd, g_stack, uc->uc_mcontext.arm_sp, 40);
    writeTo(fd, "\n");
}

// A CPU profile of the main thread: while the flag file exists, a sample
// every 10 ms to profile.log, which starts with where everything is loaded
// (to tell Relay's code from the TV's libraries).
bool profile()
{
    // Opened once and kept: a sample still being written when profiling
    // stops must not land in whatever file got a closed descriptor's number.
    static int fd = -1;
    static bool active = false;
    const bool on = access(g_profileFlag, F_OK) == 0;
    if (on && !active) {
        if (fd < 0) fd = open(g_profilePath, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        else if (ftruncate(fd, 0) == 0) lseek(fd, 0, SEEK_SET);
        if (fd < 0) return false;
        const int maps = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
        if (maps >= 0) {
            char buf[4096];
            ssize_t n;
            while ((n = read(maps, buf, sizeof buf)) > 0)
                if (write(fd, buf, size_t(n)) < 0) break;
            close(maps);
        }
        g_sampleFd = fd;
        active = true;
    } else if (!on && active) {
        g_sampleFd = g_fd;
        active = false;
    }
    if (active) syscall(SYS_tgkill, getpid(), g_mainTid, SIGUSR2);
    return active;
}

void watch()
{
    bool frozen = false;
    long long since = 0, lastSample = 0;
    int samples = 0;
    for (;;) {
        if (g_stop) return;   // Relay is closing: its main thread isn't watched any more
        if (profile()) { usleep(10000); continue; }
        usleep(250000);
        const long long t = nowMs(), beat = g_beat.load();
        if (!beat) continue;
        const long long late = t - beat;
        if (late > 1500) {
            if (!frozen) {
                frozen = true;
                since = beat;
                samples = 0;
                char line[96];
                snprintf(line, sizeof line, "freeze: the main thread stopped answering %lld ms ago\n", late);
                writeAll(line);
            }
            // A sample a second, up to 30 a freeze.
            if (samples < 30 && t - lastSample >= 1000) {
                lastSample = t;
                ++samples;
                syscall(SYS_tgkill, getpid(), g_mainTid, SIGUSR2);
            }
        } else if (frozen && late < 500) {
            frozen = false;
            char line[96];
            snprintf(line, sizeof line, "freeze over after %lld ms\n", beat - since);
            writeAll(line);
            qWarning("Freeze: the main thread was held up %lld ms (where: freeze.log)", beat - since);
        }
    }
}

void startWatchdog()
{
    const QByteArray path = tvDataBase() + "/freeze.log";
    g_fd = open(path.constData(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    g_sampleFd = g_fd;
    snprintf(g_profileFlag, sizeof g_profileFlag, "%s/profile-on", tvDataBase().constData());
    snprintf(g_profilePath, sizeof g_profilePath, "%s/profile.log", tvDataBase().constData());
    unlink(g_profileFlag);   // never left on from an earlier run
    g_mainTid = pid_t(syscall(SYS_gettid));   // this runs on the main thread
    g_stack.open();
    struct sigaction sa{};
    sa.sa_sigaction = onSample;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIGUSR2, &sa, nullptr);

    auto *heartbeat = new QTimer(QCoreApplication::instance());
    QObject::connect(heartbeat, &QTimer::timeout, [] { g_beat = nowMs(); });
    heartbeat->start(200);
    g_beat = nowMs();
    // Closing takes a moment with no heartbeat: that isn't a freeze.
    QObject::connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, [] { g_stop = true; });
    std::thread(watch).detach();
}
} // namespace

Q_COREAPP_STARTUP_FUNCTION(startWatchdog)
