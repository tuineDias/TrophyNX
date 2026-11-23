// The module's heap, sized on demand.
//
// Every background module shares one "system" memory group (~306 MB), and on a console with a few
// sysmodules it can be almost full: when a game launches, Nintendo's own modules then fail with
// 2001-0132 (LimitReached) and the console shuts down. So the module keeps a small heap while idle
// (HEAP_IDLE_SIZE) and only grows it (HEAP_BIG_SIZE) while a popup is on screen or a save is read,
// when there is room for it, then gives the memory back. Without room, tasks make do with the idle heap.
#pragma once
#include <cstddef>

#define HEAP_IDLE_SIZE 0x200000   // 2 MB (svcSetHeapSize granularity): definitions, flag scanner, logs
#define HEAP_BIG_SIZE  0x400000   // 4 MB: popup layer buffers, scan on close (one save file at a time)

// Tries to take the big heap for `why`. False: not enough free system memory (logged) - the task then
// runs in the idle heap if it fits (popup ~1 MB; scan on close skips save files that don't fit).
// Nested/parallel leases (popup thread + main loop) are counted; each successful acquire needs a release.
bool heapAcquire(const char* why);
void heapRelease();
// Current heap size, and what malloc can still hand out in it.
size_t heapSize();
size_t heapFree();
// Logger for the heap (main.cpp's logf).
void heapSetLog(void (*logFn)(const char* fmt, ...));
