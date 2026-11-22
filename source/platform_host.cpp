// PC test harness: fake games and stats, scripted input, frames written as PNG.
#ifndef __SWITCH__
#include "platform.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sstream>
#include <unistd.h>
#include "stb_image_write.h"

static std::vector<uint32_t> g_px(1280 * 720);
static std::vector<std::string> g_script;
static size_t g_step = 0;
static uint32_t g_down = 0;
static int g_frame = 0;
static std::vector<uint8_t> g_font;
static uint64_t g_mounted = 0;
static std::string g_root;

bool plat::init(std::string& err) {
    const char* r = getenv("ACH_HOST_ROOT");
    g_root = r ? r : "host";
    const char* s = getenv("ACH_SCRIPT");
    if (s) {
        std::stringstream ss(s);
        std::string t;
        while (std::getline(ss, t, ',')) g_script.push_back(t);
    }
    if (!readFile("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", g_font)) { err = "no host font"; return false; }
    return true;
}
void plat::exit() {}
std::string plat::userName() { return "Player"; }
std::string plat::userKey() { return "host-user"; }

void plat::listGames(std::vector<GameInfo>& out, ProgressFn progress, void* ctx) {
    struct { uint64_t id; const char* name; } fake[] = {
        {0x01007EF00011E000ULL, "The Legend of Zelda: Breath of the Wild"},
        {0x0100F2C0115B6000ULL, "The Legend of Zelda: Tears of the Kingdom"},
        {0x0100000000010000ULL, "Super Mario Odyssey"},
        {0x01006A800016E000ULL, "Super Smash Bros. Ultimate"},
        {0x0100152000022000ULL, "Mario Kart 8 Deluxe"},
        {0x01006F8002326000ULL, "Animal Crossing: New Horizons"},
        {0x0100633007D48000ULL, "Hollow Knight"},
        {0x0100307018934000ULL, "SIGNALIS"},
    };
    int n = sizeof(fake) / sizeof(fake[0]);
    for (int i = 0; i < n; i++) {
        if (progress) progress(i, n, ctx);
        GameInfo g;
        g.id = fake[i].id;
        g.name = fake[i].name;
        char p[256];
        snprintf(p, sizeof(p), "%s/icons/%016llX.jpg", g_root.c_str(), (unsigned long long)g.id);
        readFile(p, g.iconJpeg);
        out.push_back(g);
    }
}

PlayStats plat::playStats(uint64_t id) {
    PlayStats p;
    p.valid = true;
    uint64_t h = 3600ULL * 1000000000ULL;
    switch (id) {
        case 0x01007EF00011E000ULL: p.playtimeNs = 142 * h; p.launches = 210; p.lastPlayed = 1763300000; break;
        case 0x0100F2C0115B6000ULL: p.playtimeNs = 61 * h; p.launches = 80; p.lastPlayed = 1763200000; break;
        case 0x0100000000010000ULL: p.playtimeNs = 12 * h; p.launches = 25; p.lastPlayed = 1760000000; break;
        case 0x01006A800016E000ULL: p.playtimeNs = 3 * h / 2; p.launches = 6; p.lastPlayed = 1750000000; break;
        case 0x0100152000022000ULL: p.playtimeNs = 25 * h; p.launches = 40; p.lastPlayed = 1755000000; break;
        case 0x0100633007D48000ULL: p.playtimeNs = 30 * h; p.launches = 50; p.lastPlayed = 1763400000; break;
        case 0x0100307018934000ULL: p.playtimeNs = 14 * h; p.launches = 20; p.lastPlayed = 1763500000; break;
        default: p.playtimeNs = 0; p.launches = 0; break;
    }
    return p;
}

bool plat::openSave(uint64_t id) {
    char p[256];
    snprintf(p, sizeof(p), "%s/saves/%016llX", g_root.c_str(), (unsigned long long)id);
    if (access(p, F_OK) != 0) return false;
    g_mounted = id;
    return true;
}
bool plat::readSaveFile(const std::string& rel, std::vector<uint8_t>& out) {
    char p[256];
    snprintf(p, sizeof(p), "%s/saves/%016llX/", g_root.c_str(), (unsigned long long)g_mounted);
    return readFile(p + rel, out);
}
void plat::listSaveFiles(std::vector<std::pair<std::string, uint64_t>>& out) {
    out.clear();
    if (!g_mounted) return;
    char p[256];
    snprintf(p, sizeof(p), "%s/saves/%016llX/", g_root.c_str(), (unsigned long long)g_mounted);
    listFilesRecursive(p, "", out);
}
void plat::closeSave() { g_mounted = 0; }
std::string plat::lastError() { return "no save folder (host)"; }
std::string plat::liveModuleStatus() { return "not available on PC"; }
// Updates on PC: https://api.github.com/repos/<repo>/contents/<path>?ref=<branch> is read from
// $ACH_UPDATE_DIR/<path> (a copy of the "builds" branch). A token "bad" in update.json -> 401.
int plat::httpGet(const std::string& url, const std::vector<std::string>& headers, std::vector<uint8_t>& out,
                  ProgressFn progress, void* ctx, std::string& err) {
    out.clear();
    const char* dir = getenv("ACH_UPDATE_DIR");
    if (!dir) { err = "Could not resolve host (no ACH_UPDATE_DIR on PC)"; return 0; }
    for (const std::string& h : headers) if (h == "Authorization: Bearer bad") return 401;
    // releases: $ACH_UPDATE_DIR/release/latest.json (assets[].url = .../releases/assets/<name>) + release/<name>
    size_t ra = url.find("/releases/assets/");
    if (url.size() > 16 && url.compare(url.size() - 16, 16, "/releases/latest") == 0) {
        if (!readFile(std::string(dir) + "/release/latest.json", out)) return 404;
        return 200;
    }
    if (ra != std::string::npos) {
        if (!readFile(std::string(dir) + "/release/" + url.substr(ra + 17), out)) return 404;
        if (progress) progress((int)(out.size() >> 10), (int)(out.size() >> 10), ctx);
        return 200;
    }
    size_t c = url.find("/contents/"), q = url.find('?');
    if (c == std::string::npos) return 404;
    std::string rel = url.substr(c + 10, q == std::string::npos ? std::string::npos : q - c - 10);
    if (!readFile(std::string(dir) + "/" + rel, out)) return 404;
    if (progress) progress((int)(out.size() >> 10), (int)(out.size() >> 10), ctx);
    return 200;
}
// Site sync on PC: the body is written to $ACH_SYNC_OUT (if set) and answered with 200 {"ok":true}.
// A token "bad" in the Authorization header -> 401, like the real server.
int plat::httpPost(const std::string& url, const std::vector<std::string>& headers, const std::string& body,
                   std::vector<uint8_t>& out, std::string& err) {
    out.clear();
    for (const std::string& h : headers) if (h == "Authorization: Bearer bad") { out.assign({'{', '}'}); return 401; }
    if (url.find("unreachable") != std::string::npos) { err = "Could not resolve host (test)"; return 0; }
    if (const char* f = getenv("ACH_SYNC_OUT")) writeFile(f, body);
    const char* ok = "{\"ok\":true}";
    out.assign(ok, ok + strlen(ok));
    return 200;
}
static std::string g_self;
void plat::setSelfPath(const std::string& p) { g_self = p; }
std::string plat::selfPath() { return g_self.empty() ? g_root + "/sd/switch/TrophyNX.nro" : g_self; }
std::string plat::sdRoot() { return g_root + "/sd/"; }
bool plat::restartLiveModule(std::string& msg) { msg = "live module restarted (PC: nothing to do)"; return true; }
void plat::relaunchSelf() {}

std::string plat::dataDir() { return g_root + "/sd/switch/TrophyNX"; }
std::string plat::romfsDir() { return "romfs"; }
bool plat::fontData(int which, const uint8_t** data, size_t* size) {
    if (which != 0) return false;
    *data = g_font.data();
    *size = g_font.size();
    return true;
}
uint64_t plat::now() { return (uint64_t)time(NULL); }

bool plat::poll() {
    if (g_step >= g_script.size()) return false;
    const std::string& t = g_script[g_step++];
    g_down = 0;
    if (t == "A") g_down = BTN_A; else if (t == "B") g_down = BTN_B; else if (t == "X") g_down = BTN_X;
    else if (t == "Y") g_down = BTN_Y; else if (t == "UP") g_down = BTN_UP; else if (t == "DOWN") g_down = BTN_DOWN;
    else if (t == "LEFT") g_down = BTN_LEFT; else if (t == "RIGHT") g_down = BTN_RIGHT; else if (t == "PLUS") g_down = BTN_PLUS;
    else if (t == "R") g_down = BTN_R; else if (t == "ZR") g_down = BTN_ZR; else if (t == "ZL") g_down = BTN_ZL; else if (t == "L") g_down = BTN_L; else if (t == "MINUS") g_down = BTN_MINUS;
    else if (t == "STICKR") g_down = BTN_STICKR;
    return true;
}
uint32_t* plat::beginDraw(int& stride) { stride = 1280; return g_px.data(); }
void plat::endDraw() {
    char p[256];
    snprintf(p, sizeof(p), "%s/out/frame_%03d.png", g_root.c_str(), g_frame++);
    if (!getenv("ACH_NO_PNG")) stbi_write_png(p, 1280, 720, 4, g_px.data(), 1280 * 4);
}
uint32_t plat::buttonsDown() { return g_down; }
uint32_t plat::buttonsHeld() { return 0; }
void plat::sleepMs(int) {}
bool plat::netInit(std::string&) { return true; }
std::string plat::localIp() { return "127.0.0.1"; }
void plat::keepAwake(bool) {}
void plat::releaseSelfFile() {}
uint64_t plat::liveModulePid() { return 0; }
#endif
