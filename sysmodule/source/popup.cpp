// In-game achievement popups. A worker thread opens a small managed display layer above the game
// (same technique as Tesla/Ultrahand overlays), slides the card in, holds it, slides it out and closes
// the layer again. If the layer can't be created, the popup falls back to an Ultrahand toast.
//
// Config (optional): sdmc:/switch/TrophyNX/popup.json
//   {"style": "custom" | "both" | "ultrahand" | "off", "theme": "dark" | "light",
//    "icon": "right" | "left", "duration_ms": 4000}
// Per-achievement art (optional): sdmc:/switch/TrophyNX/art/<TITLEID>/<achId>.png (or .jpg),
// otherwise the game's own icon, otherwise the app's trophy icon.
#include "popup.h"
#include "store.h"
#include "popup_draw.h"
#include "heap.h"
#include <switch.h>
#include <cstdio>
#include <cstring>
#include <deque>
#include <sys/stat.h>
#include "cJSON.h"
#include "icon_jpg.h"   // the app icon (trophy), embedded: used when the game has no readable icon

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#include "stb_image.h"

extern "C" u64 __nx_vi_layer_id;

static const char* NOTIFY_DIR = "sdmc:/config/ultrahand/notifications";

static void (*g_log)(const char*, ...) = nullptr;
#define PLOG(...) do { if (g_log) g_log(__VA_ARGS__); } while (0)

static Mutex g_mtx;
static CondVar g_cv;
static std::deque<PopupRequest> g_queue;
static volatile bool g_busy = false;   // a popup is being shown (its layer buffers are allocated)
static Thread g_thread;
static bool g_threadOk = false;
// Watchdog state: the worker says what it is doing; the main loop checks it is not stuck (popupWatchdog).
static volatile const char* g_stage = nullptr;
static volatile u64 g_stageTick = 0;
static volatile bool g_stuck = false;
static void stage(const char* s) { g_stageTick = armGetSystemTick(); g_stage = s; }
// Safety: layers currently open (must be 0 between popups) and "safe mode" (system close to its limits:
// no display layer at all, Ultrahand toast only), set by the main loop's health check.
static volatile int g_layersOpen = 0;
static volatile bool g_safe = false;
static const size_t POPUP_MIN_HEAP = 1400 * 1024;   // card peak ~1 MB + icon decode, with some slack

// ---------------------------------------------------------------- config
struct PopupConfig {
    std::string style = "custom";   // Ultrahand toast only as fallback
    bool dark = true, iconLeft = false;
    int durationMs = 4000;
};

static PopupConfig loadConfig() {
    PopupConfig c;
    char path[128];
    snprintf(path, sizeof(path), "%s/popup.json", dataDir().c_str());
    FILE* f = fopen(path, "rb");
    if (!f) return c;
    char buf[1024];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
    cJSON* root = cJSON_Parse(buf);
    if (!root) { PLOG("popup.json: invalid JSON, using defaults"); return c; }
    cJSON* v;
    if (cJSON_IsString(v = cJSON_GetObjectItem(root, "style"))) c.style = v->valuestring;
    if (cJSON_IsString(v = cJSON_GetObjectItem(root, "theme"))) c.dark = strcmp(v->valuestring, "light") != 0;
    if (cJSON_IsString(v = cJSON_GetObjectItem(root, "icon"))) c.iconLeft = strcmp(v->valuestring, "left") == 0;
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(root, "duration_ms"))) c.durationMs = v->valueint;
    if (c.durationMs < 1500) c.durationMs = 1500;
    if (c.durationMs > 15000) c.durationMs = 15000;
    cJSON_Delete(root);
    return c;
}

// ---------------------------------------------------------------- Ultrahand toast
static std::string jsonEscape(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') o += '\\';
        o += c;
    }
    return o;
}

static void ultrahandToast(const PopupRequest& r) {
    struct stat st;
    if (stat("sdmc:/config/ultrahand", &st) != 0) { PLOG("popup: Ultrahand not installed, no toast"); return; }
    mkdir(NOTIFY_DIR, 0777);
    char path[160];
    snprintf(path, sizeof(path), "%s/TrophyNX-%llu.notify", NOTIFY_DIR, (unsigned long long)armGetSystemTick());
    std::string body = "{\"title\":\"Achievement unlocked!\",\"text\":\"" + jsonEscape(r.title) + "  (+" +
                       std::to_string(r.points) + " pts)  " + jsonEscape(r.game) + "\",\"duration\":6000,\"priority\":30}";
    FILE* f = fopen(path, "wb");
    if (!f) { PLOG("popup: cannot write %s", path); return; }
    fwrite(body.data(), 1, body.size(), f);
    fclose(f);
    PLOG("popup: Ultrahand toast written");
}

// ---------------------------------------------------------------- font + icons
static bool g_fontReady = false;

static bool ensureFont() {
    if (g_fontReady) return true;
    if (R_FAILED(plInitialize(PlServiceType_User))) { PLOG("popup: pl init failed"); return false; }
    PlFontData fd;
    if (R_FAILED(plGetSharedFontByType(&fd, PlSharedFontType_Standard)) || !popupFontInit((const uint8_t*)fd.address)) {
        PLOG("popup: system font unavailable");
        return false;
    }
    g_fontReady = true;   // pl stays open: the shared font memory must stay mapped
    return true;
}

static bool decodeTo(const uint8_t* data, int size, int side, PopupImage& out) {
    int w, h, n;
    uint8_t* px = stbi_load_from_memory(data, size, &w, &h, &n, 4);
    if (!px) return false;
    popupScaleImage(px, w, h, side, side, out);
    stbi_image_free(px);
    return true;
}

static int iconSide() { return (int)(66 * POPUP_SCALE + 0.5f); }   // icon frame inner size, see popup_draw.cpp

static uint64_t g_iconTid = 0;
static PopupImage g_gameIcon;

static const PopupImage* gameIcon(uint64_t tid) {
    if (tid == g_iconTid) return g_gameIcon.w ? &g_gameIcon : nullptr;
    g_iconTid = tid;
    g_gameIcon = PopupImage();
    if (R_FAILED(nsInitialize())) return nullptr;
    NsApplicationControlData* ctl = new NsApplicationControlData;
    u64 sz = 0;
    if (R_SUCCEEDED(nsGetApplicationControlData(NsApplicationControlSource_Storage, tid, ctl, sizeof(*ctl), &sz)) &&
        sz > sizeof(ctl->nacp)) {
        if (!decodeTo(ctl->icon, (int)(sz - sizeof(ctl->nacp)), iconSide(), g_gameIcon)) PLOG("popup: game icon decode failed");
    }
    delete ctl;
    nsExit();
    if (g_gameIcon.w) return &g_gameIcon;
    decodeTo(icon_jpg, (int)icon_jpg_size, iconSide(), g_gameIcon);   // trophy icon, freed with the popup
    return g_gameIcon.w ? &g_gameIcon : nullptr;
}

static bool achievementArt(const PopupRequest& r, PopupImage& out) {
    static const char* exts[2] = {"png", "jpg"};
    for (const char* e : exts) {
        char path[256];
        snprintf(path, sizeof(path), "%s/art/%016llX/%s.%s", dataDir().c_str(), (unsigned long long)r.titleId, r.achId.c_str(), e);
        FILE* f = fopen(path, "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        bool ok = false;
        if (size > 0 && size < 4 * 1024 * 1024) {
            std::vector<uint8_t> data(size);
            ok = fread(data.data(), 1, size, f) == (size_t)size && decodeTo(data.data(), (int)size, iconSide(), out);
        }
        fclose(f);
        if (ok) return true;
    }
    return false;
}

// ---------------------------------------------------------------- display layer
static Result addToLayerStack(ViLayer* layer, ViLayerStack stack) {   // IManagerDisplayService::AddToLayerStack
    const struct { u32 stack; u64 layerId; } in = {(u32)stack, layer->layer_id};
    return serviceDispatchIn(viGetSession_IManagerDisplayService(), 6000, in);
}

struct Overlay {
    ViDisplay display = {};
    ViLayer layer = {};
    NWindow win = {};
    Framebuffer fb = {};
    int stage = 0;   // how far open() got, for close()

    bool open(int w, int h, int y) {
        Result rc;
        ::stage("nv init");
        if (R_FAILED(rc = nvInitialize())) { PLOG("popup: nv init 0x%X", rc); return false; }
        stage = 1;
        ::stage("vi init");
        if (R_FAILED(rc = viInitialize(ViServiceType_Manager))) { PLOG("popup: vi init 0x%X", rc); return false; }
        stage = 2;
        if (R_FAILED(rc = viOpenDefaultDisplay(&display))) { PLOG("popup: open display 0x%X", rc); return false; }
        stage = 3;
        ::stage("create layer");
        if (R_FAILED(rc = viCreateManagedLayer(&display, (ViLayerFlags)0, 0, &__nx_vi_layer_id))) { PLOG("popup: managed layer 0x%X", rc); return false; }
        if (R_FAILED(rc = viCreateLayer(&display, &layer))) {
            PLOG("popup: create layer 0x%X", rc);
            layer.layer_id = __nx_vi_layer_id;   // the managed layer exists: destroy it, or vi keeps it forever
            viDestroyManagedLayer(&layer);
            __nx_vi_layer_id = 0;
            return false;
        }
        stage = 4;
        g_layersOpen++;
        viSetLayerScalingMode(&layer, ViScalingMode_FitToLayer);
        s32 z = 0;
        if (R_SUCCEEDED(viGetZOrderCountMax(&display, &z)) && z > 0) viSetLayerZ(&layer, z);
        else viSetLayerZ(&layer, 255);
        // also in screenshots and recordings, like system notifications
        const ViLayerStack stacks[] = {ViLayerStack_Default, ViLayerStack_Screenshot, ViLayerStack_Recording,
                                       ViLayerStack_Arbitrary, ViLayerStack_LastFrame, ViLayerStack_Null,
                                       ViLayerStack_ApplicationForDebug, ViLayerStack_Lcd};
        for (ViLayerStack s : stacks) addToLayerStack(&layer, s);
        if (R_FAILED(rc = viSetLayerSize(&layer, w, h)) || R_FAILED(rc = viSetLayerPosition(&layer, 0, (float)y))) {
            PLOG("popup: layer size/position 0x%X", rc); return false;
        }
        if (R_FAILED(rc = nwindowCreateFromLayer(&win, &layer))) { PLOG("popup: window 0x%X", rc); return false; }
        stage = 5;
        ::stage("framebuffer");
        if (R_FAILED(rc = framebufferCreate(&fb, &win, w, h, PIXEL_FORMAT_RGBA_4444, 2))) { PLOG("popup: framebuffer 0x%X", rc); return false; }
        stage = 6;
        if (R_FAILED(rc = framebufferMakeLinear(&fb))) { PLOG("popup: linear fb 0x%X", rc); return false; }
        PLOG("popup: layer %llu open, %dx%d at y=%d, z=%d", (unsigned long long)layer.layer_id, w, h, y, (int)z);
        return true;
    }

    void close() {
        if (stage >= 6) framebufferClose(&fb);
        if (stage >= 5) nwindowClose(&win);
        if (stage >= 4) { viDestroyManagedLayer(&layer); __nx_vi_layer_id = 0; g_layersOpen--; }
        if (stage >= 3) viCloseDisplay(&display);
        if (stage >= 2) viExit();
        if (stage >= 1) nvExit();
        stage = 0;
    }
};

static inline u16 to4444(const uint8_t* p) {   // r in the low nibble (same layout as Tesla's Color)
    return (u16)((p[0] >> 4) | ((p[1] >> 4) << 4) | ((p[2] >> 4) << 8) | ((p[3] >> 4) << 12));
}

static bool showCustom(const PopupRequest& r, const PopupConfig& cfg) {
    stage("font");
    if (!ensureFont()) return false;
    stage("icon + render");
    PopupImage art;
    PopupInfo info;
    info.title = r.title;
    info.desc = r.desc;
    info.tier = r.tier;
    info.dark = cfg.dark;
    info.iconLeft = cfg.iconLeft;
    info.icon = achievementArt(r, art) ? &art : gameIcon(r.titleId);

    // render once, keep it as 16-bit to save memory, then only blit while animating
    std::vector<u16> card;
    int cw, ch, cardWidth;
    {
        PopupImage img;
        cardWidth = popupRenderCard(info, img);
        cw = img.w; ch = img.h;
        card.resize((size_t)cw * ch);
        for (size_t i = 0; i < card.size(); i++) card[i] = to4444(&img.px[i * 4]);
    }

    Overlay ov;   // re-created for every popup: confirmed working on hardware (0.3.1 log)
    if (!ov.open(cw, ch, (int)(POPUP_Y720 * POPUP_SCALE + 0.5f))) { ov.close(); return false; }

    const u64 dur = (u64)cfg.durationMs, t0 = armGetSystemTick();
    int frames = 0;
    stage("first frame");
    while (true) {
        u64 t = armTicksToNs(armGetSystemTick() - t0) / 1000000;
        float s = popupSlide(t, dur);
        int off = (int)((1.f - s) * (cardWidth + 8));   // card shifted left by off pixels
        u32 stride = 0;
        u16* fb = (u16*)framebufferBegin(&ov.fb, &stride);
        for (int y = 0; y < ch; y++) {
            u16* row = fb + (size_t)y * (stride / 2);
            const u16* src = &card[(size_t)y * cw];
            int vis = cw - off;
            if (vis > 0) memcpy(row, src + off, (size_t)vis * 2);
            if (vis < 0) vis = 0;
            memset(row + vis, 0, (size_t)(cw - vis) * 2);
        }
        framebufferEnd(&ov.fb);
        if (++frames == 3) stage("animating");   // a 3rd frame means the display really consumes ours
        if (t >= dur) break;
    }
    stage("close layer");
    ov.close();
    PLOG("popup: card shown, %d frames in %llu ms", frames,
         (unsigned long long)(armTicksToNs(armGetSystemTick() - t0) / 1000000));
    return true;
}

// ---------------------------------------------------------------- worker
static void worker(void*) {
    while (true) {
        mutexLock(&g_mtx);
        while (g_queue.empty()) condvarWait(&g_cv, &g_mtx);
        PopupRequest r = g_queue.front();
        g_queue.pop_front();
        g_busy = true;
        mutexUnlock(&g_mtx);

        PopupConfig cfg = loadConfig();
        PLOG("popup: %s (style %s)", r.achId.c_str(), cfg.style.c_str());
        if (cfg.style == "off") { g_busy = false; continue; }
        if (cfg.style == "ultrahand") { ultrahandToast(r); g_busy = false; continue; }
        if (g_safe && cfg.style != "both") { PLOG("popup: safe mode, toast instead of a display layer"); ultrahandToast(r); g_busy = false; continue; }
        if (cfg.style == "both") ultrahandToast(r);
        if (g_safe) { g_busy = false; continue; }
        // The card (~1 MB: 720x150 render, 16-bit copy, 2 buffers + linear) fits in the idle heap; the big
        // heap is only a bonus. Toast only if even the idle heap is too full.
        bool big = heapAcquire("popup layer");
        if (!big && heapFree() < POPUP_MIN_HEAP) {
            PLOG("popup: only %u KB free in the heap - Ultrahand toast instead", (unsigned)(heapFree() >> 10));
            if (cfg.style != "both") ultrahandToast(r);
            g_busy = false;
            continue;
        }
        if (!showCustom(r, cfg)) {
            PLOG("popup: custom popup failed for %s - Ultrahand toast instead", r.achId.c_str());
            if (cfg.style != "both") ultrahandToast(r);
        }
        g_iconTid = 0;               // drop the cached icon: it would keep the big heap from shrinking
        g_gameIcon = PopupImage();
        if (big) heapRelease();
        g_stage = nullptr;
        g_busy = false;
        svcSleepThread(300000000ULL);   // small gap between queued popups
    }
}

void popupStart(void (*logFn)(const char*, ...)) {
    g_log = logFn;
    mutexInit(&g_mtx);
    condvarInit(&g_cv);
    // priority 44: above the scanning loop (49) so the animation stays smooth; core 3 (the only one allowed)
    Result rc = threadCreate(&g_thread, worker, nullptr, nullptr, 0x10000, 44, -2);
    if (R_SUCCEEDED(rc)) rc = threadStart(&g_thread);
    g_threadOk = R_SUCCEEDED(rc);
    if (!g_threadOk) PLOG("popup: thread start failed 0x%X - Ultrahand toasts only", rc);
}

void popupWatchdog() {
    const char* st = (const char*)g_stage;
    if (!st || g_stuck) return;
    u64 sec = armTicksToNs(armGetSystemTick() - g_stageTick) / 1000000000ULL;
    if (sec >= 20) {
        g_stuck = true;
        PLOG("popup: STUCK for %llu s at '%s' - Ultrahand toasts only from now on", (unsigned long long)sec, st);
    }
}

bool popupIdle() {
    if (!g_threadOk || g_stuck) return true;
    mutexLock(&g_mtx);
    bool idle = g_queue.empty() && !g_busy;
    mutexUnlock(&g_mtx);
    return idle;
}

int popupLayersOpen() { return g_layersOpen; }
void popupSetSafeMode(bool on) { g_safe = on; }

void popupEnqueue(const PopupRequest& r) {
    if (!g_threadOk || g_stuck) { ultrahandToast(r); return; }
    mutexLock(&g_mtx);
    if (g_queue.size() < 32) g_queue.push_back(r);
    condvarWakeOne(&g_cv);
    mutexUnlock(&g_mtx);
}
