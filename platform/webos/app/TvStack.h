#pragma once

// Reading a thread's stack from a signal handler, for the crash note
// (TvPlatform.cpp) and the freeze and profile samples (TvWatchdog.cpp).
//
// A stack can end a few hundred bytes above where a thread is (the main
// thread during shutdown, say), so it can't just be read: reading past its
// end is a crash of its own.

#include <cstddef>

// Copies memory that may not all be readable: the kernel does the copy,
// through a pipe, and stops where reading would fault.
class TvSafeReader
{
public:
    void open();   // once, before the first read (not from a signal handler)
    // Bytes copied: fewer than `len` where the memory ends. Async-signal-safe.
    size_t read(unsigned long from, void *to, size_t len) const;

private:
    int m_in = -1, m_out = -1;
};

// Writes to `fd` the words on the stack above `sp` that point into Relay's own
// code (" %08lx" each), up to `maxFound` of them, looking at most `maxBytes` up
// the stack: the calls that led there, with some stale ones among them. Look
// them up in the unstripped relay.debug that build-app.sh keeps.
// Async-signal-safe.
void tvWriteStackWords(int fd, const TvSafeReader &reader, unsigned long sp, int maxFound, size_t maxBytes = 8192);
