// The module's heap, sized on demand (see heap.h).
#include "heap.h"
#include <switch.h>
#include <malloc.h>
#include <reent.h>
#include <unistd.h>

extern "C" {
extern char* fake_heap_start;
extern char* fake_heap_end;
void __malloc_lock(struct _reent*);
void __malloc_unlock(struct _reent*);

// libnx calls this before anything else: the heap is the process heap region (svcSetHeapSize), not a
// fixed array in .bss, so it can grow and shrink. newlib's sbrk reads fake_heap_end on every call.
void __libnx_initheap(void) {
    void* addr = nullptr;
    Result rc = svcSetHeapSize(&addr, HEAP_IDLE_SIZE);
    if (R_FAILED(rc)) diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_HeapAllocFailed));
    fake_heap_start = (char*)addr;
    fake_heap_end = (char*)addr + HEAP_IDLE_SIZE;
}
}

static const size_t GRANULE = 0x200000;           // svcSetHeapSize works in 2 MB steps
static const s64 MIN_FREE_AFTER = 6 * 1024 * 1024;  // never leave less than this to the other modules (game launches)

static Mutex g_mtx;   // zero-initialised = unlocked
static int g_leases = 0;
static size_t g_size = HEAP_IDLE_SIZE;
static void (*g_log)(const char*, ...) = nullptr;

void heapSetLog(void (*logFn)(const char*, ...)) { g_log = logFn; }
size_t heapSize() { return g_size; }
size_t heapFree() {
    struct mallinfo mi = mallinfo();
    char* top = (char*)sbrk(0);
    return (size_t)mi.fordblks + ((top && fake_heap_end > top) ? (size_t)(fake_heap_end - top) : 0);
}

// Free memory of the shared system group (InfoType_ResourceLimit only takes INVALID_HANDLE).
static bool systemFree(s64& freeBytes) {
    u64 h = 0;
    if (R_FAILED(svcGetInfo(&h, InfoType_ResourceLimit, INVALID_HANDLE, 0)) || h == 0) return false;
    s64 cur = 0, lim = 0;
    bool ok = R_SUCCEEDED(svcGetResourceLimitCurrentValue(&cur, (Handle)h, LimitableResource_Memory)) &&
              R_SUCCEEDED(svcGetResourceLimitLimitValue(&lim, (Handle)h, LimitableResource_Memory));
    svcCloseHandle((Handle)h);
    freeBytes = lim - cur;
    return ok;
}

// Resizes the heap region; the caller holds the malloc lock (nothing else can be in sbrk).
static bool resize(size_t size) {
    void* addr = nullptr;
    if (size > g_size) {
        if (R_FAILED(svcSetHeapSize(&addr, size))) return false;
        fake_heap_end = fake_heap_start + size;
    } else {
        fake_heap_end = fake_heap_start + size;
        if (R_FAILED(svcSetHeapSize(&addr, size))) { fake_heap_end = fake_heap_start + g_size; return false; }
    }
    g_size = size;
    return true;
}

bool heapAcquire(const char* why) {
    mutexLock(&g_mtx);
    if (g_leases > 0 || g_size >= HEAP_BIG_SIZE) { g_leases++; mutexUnlock(&g_mtx); return true; }
    s64 freeBytes = 0;
    size_t grow = HEAP_BIG_SIZE - g_size;
    if (systemFree(freeBytes) && freeBytes - (s64)grow < MIN_FREE_AFTER) {
        mutexUnlock(&g_mtx);
        if (g_log) g_log("memory: %s in the %u KB heap - system memory too low to grow (%lld KB free, needs %u KB + %lld KB spare)", why,
                         (unsigned)(g_size >> 10),
                         (long long)(freeBytes >> 10), (unsigned)(grow >> 10), (long long)(MIN_FREE_AFTER >> 10));
        return false;
    }
    __malloc_lock(_REENT);
    bool ok = resize(HEAP_BIG_SIZE);
    __malloc_unlock(_REENT);
    if (ok) g_leases++;
    mutexUnlock(&g_mtx);
    if (!ok && g_log) g_log("memory: %s in the %u KB heap - could not grow to %u KB", why, (unsigned)(g_size >> 10), (unsigned)(HEAP_BIG_SIZE >> 10));
    return ok;
}

void heapRelease() {
    mutexLock(&g_mtx);
    if (g_leases > 0 && --g_leases == 0) {
        __malloc_lock(_REENT);
        malloc_trim(0);   // hand the free top of the heap back to sbrk
        size_t used = (size_t)((char*)sbrk(0) - fake_heap_start);
        size_t need = (used + GRANULE - 1) / GRANULE * GRANULE;
        if (need < HEAP_IDLE_SIZE) need = HEAP_IDLE_SIZE;
        bool shrunk = need >= g_size || resize(need);
        size_t now = g_size;
        __malloc_unlock(_REENT);
        if (g_log && (!shrunk || now > HEAP_IDLE_SIZE))
            g_log("memory: heap stays at %u KB (in use up to %u KB)", (unsigned)(now >> 10), (unsigned)(used >> 10));
    }
    mutexUnlock(&g_mtx);
}
