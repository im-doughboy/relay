#include "TvStack.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

// Where Relay's own code starts and ends (from the linker).
extern "C" char __executable_start, etext;

void TvSafeReader::open()
{
    int fds[2];
    if (pipe2(fds, O_NONBLOCK | O_CLOEXEC) != 0) return;
    m_out = fds[0];
    m_in = fds[1];
}

size_t TvSafeReader::read(unsigned long from, void *to, size_t len) const
{
    if (m_in < 0) return 0;
    size_t done = 0;
    while (done < len) {
        // A page at a time: write() stops, or fails, where the memory does.
        const unsigned long at = from + done;
        size_t chunk = 4096 - (at & 4095);
        if (chunk > len - done) chunk = len - done;
        ssize_t w;
        do w = ::write(m_in, reinterpret_cast<const void *>(at), chunk); while (w < 0 && errno == EINTR);
        if (w <= 0) break;
        size_t taken = 0;
        while (taken < size_t(w)) {   // back out of the pipe, all of it
            const ssize_t r = ::read(m_out, static_cast<char *>(to) + done + taken, size_t(w) - taken);
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) return done + taken;
            taken += size_t(r);
        }
        done += size_t(w);
        if (size_t(w) < chunk) break;
    }
    return done;
}

void tvWriteStackWords(int fd, const TvSafeReader &reader, unsigned long sp, int maxFound, size_t maxBytes)
{
    const auto lo = reinterpret_cast<unsigned long>(&__executable_start);
    const auto hi = reinterpret_cast<unsigned long>(&etext);
    unsigned long words[256];
    const size_t count = sizeof words / sizeof words[0];
    int found = 0;
    sp &= ~3UL;
    for (size_t off = 0; off < maxBytes && found < maxFound; off += sizeof words) {
        const size_t got = reader.read(sp + off, words, sizeof words) / sizeof words[0];
        for (size_t i = 0; i < got && found < maxFound; ++i) {
            if (words[i] < lo || words[i] >= hi) continue;
            char text[16];
            snprintf(text, sizeof text, " %08lx", words[i]);
            if (::write(fd, text, strlen(text)) < 0) break;
            ++found;
        }
        if (got < count) break;   // the stack ends here
    }
}
