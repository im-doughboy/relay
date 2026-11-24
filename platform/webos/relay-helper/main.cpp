// Runs Relay's own stream relay on the TV for the spike. Commands arrive on
// stdin, one per line, tab separated:
//   add <ts|hls> <user agent> <url>   answers: ok <port> <path>, or err
//   remove <path>                     answers: ok
// The helper exits when stdin closes.

#include "CastRelay.h"

#include <QCoreApplication>
#include <QSocketNotifier>
#include <QSslSocket>

#include <cstdio>
#include <unistd.h>

static void handle(CastRelay &relay, const QByteArray &line)
{
    const QList<QByteArray> f = line.split('\t');
    if (f.value(0) == "add" && f.size() == 4) {
        CastRelay::Source s;
        s.userAgent = QString::fromUtf8(f[2]);
        s.url = QString::fromUtf8(f[3]);
        if (f[1] == "hls") s.contentType = QStringLiteral("application/vnd.apple.mpegurl");
        else { s.contentType = QStringLiteral("video/mp2t"); s.shareLive = true; }
        const QString path = relay.add(s);
        if (path.isEmpty()) printf("err\n");
        else printf("ok\t%u\t%s\n", unsigned(relay.port()), path.toUtf8().constData());
    } else if (f.value(0) == "remove" && f.size() == 2) {
        relay.remove(QString::fromUtf8(f[1]));
        printf("ok\n");
    } else {
        printf("err\n");
    }
    fflush(stdout);
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    fprintf(stderr, "relay-helper: Qt %s, TLS backend %s, TLS support %d\n", qVersion(),
            qPrintable(QSslSocket::activeBackend()), int(QSslSocket::supportsSsl()));
    CastRelay relay;
    QByteArray pending;
    QSocketNotifier input(STDIN_FILENO, QSocketNotifier::Read);
    QObject::connect(&input, &QSocketNotifier::activated, &app, [&] {
        char chunk[8192];
        const ssize_t n = read(STDIN_FILENO, chunk, sizeof chunk);
        if (n <= 0) { input.setEnabled(false); app.quit(); return; }
        pending.append(chunk, n);
        for (qsizetype end; (end = pending.indexOf('\n')) >= 0; pending.remove(0, end + 1))
            handle(relay, pending.left(end));
    });
    printf("ready\n");
    fflush(stdout);
    return app.exec();
}
