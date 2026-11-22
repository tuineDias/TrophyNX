#ifdef __SWITCH__
#include "platform.h"
#include <switch.h>
#include <cstring>
#include <cstdio>
#include <ctime>
#include <memory>
#include "render.h"
#include <curl/curl.h>

static AccountUid g_uid;
static std::string g_userName = "Player";
static PadState g_pad;
static Framebuffer g_fb;
static bool g_fbReady = false;
static bool g_saveMounted = false;
static bool g_pl = false, g_pdm = false, g_ns = false, g_acc = false, g_romfs = false;
static uint64_t g_down = 0, g_held = 0;
static std::string g_lastError;

static uint32_t mapButtons(uint64_t k) {
    uint32_t b = 0;
    if (k & HidNpadButton_A) b |= plat::BTN_A;
    if (k & HidNpadButton_B) b |= plat::BTN_B;
    if (k & HidNpadButton_X) b |= plat::BTN_X;
    if (k & HidNpadButton_Y) b |= plat::BTN_Y;
    if (k & (HidNpadButton_Up | HidNpadButton_StickLUp | HidNpadButton_StickRUp)) b |= plat::BTN_UP;
    if (k & (HidNpadButton_Down | HidNpadButton_StickLDown | HidNpadButton_StickRDown)) b |= plat::BTN_DOWN;
    if (k & (HidNpadButton_Left | HidNpadButton_StickLLeft)) b |= plat::BTN_LEFT;
    if (k & (HidNpadButton_Right | HidNpadButton_StickLRight)) b |= plat::BTN_RIGHT;
    if (k & HidNpadButton_Plus) b |= plat::BTN_PLUS;
    if (k & HidNpadButton_Minus) b |= plat::BTN_MINUS;
    if (k & HidNpadButton_L) b |= plat::BTN_L;
    if (k & HidNpadButton_ZL) b |= plat::BTN_ZL;
    if (k & HidNpadButton_R) b |= plat::BTN_R;
    if (k & HidNpadButton_ZR) b |= plat::BTN_ZR;
    if (k & HidNpadButton_StickR) b |= plat::BTN_STICKR;
    return b;
}

bool plat::init(std::string& err) {
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&g_pad);

    if (R_FAILED(framebufferCreate(&g_fb, nwindowGetDefault(), 1280, 720, PIXEL_FORMAT_RGBA_8888, 2))) {
        err = "Could not create the framebuffer.";
        return false;
    }
    framebufferMakeLinear(&g_fb);
    g_fbReady = true;

    g_romfs = R_SUCCEEDED(romfsInit());
    g_pl = R_SUCCEEDED(plInitialize(PlServiceType_User));
    g_ns = R_SUCCEEDED(nsInitialize());
    g_pdm = R_SUCCEEDED(pdmqryInitialize());
    g_acc = R_SUCCEEDED(accountInitialize(AccountServiceType_Administrator));
    if (!g_acc) g_acc = R_SUCCEEDED(accountInitialize(AccountServiceType_Application));
    if (!g_acc) { err = "Account service unavailable."; return false; }
    if (!g_ns) { err = "NS service unavailable."; return false; }

    // Pick the user: preselected (title override launch) or ask with the system selector.
    Result rc = accountGetPreselectedUser(&g_uid);
    if (R_FAILED(rc) || !accountUidIsValid(&g_uid)) {
        PselUserSelectionSettings s;
        memset(&s, 0, sizeof(s));
        rc = pselShowUserSelector(&g_uid, &s);
        if (R_FAILED(rc) || !accountUidIsValid(&g_uid)) {
            // Fall back to the most recently opened user, then the first user.
            rc = accountGetLastOpenedUser(&g_uid);
            if (R_FAILED(rc) || !accountUidIsValid(&g_uid)) {
                AccountUid users[ACC_USER_LIST_SIZE];
                s32 n = 0;
                if (R_SUCCEEDED(accountListAllUsers(users, ACC_USER_LIST_SIZE, &n)) && n > 0) g_uid = users[0];
                else { err = "No user selected."; return false; }
            }
        }
    }
    AccountProfile prof;
    if (R_SUCCEEDED(accountGetProfile(&prof, g_uid))) {
        AccountProfileBase base;
        memset(&base, 0, sizeof(base));
        if (R_SUCCEEDED(accountProfileGet(&prof, NULL, &base))) g_userName = std::string(base.nickname, strnlen(base.nickname, sizeof(base.nickname)));
        accountProfileClose(&prof);
    }
    return true;
}

static bool g_net = false;      // sockets + curl, started by the first httpGet
static std::string g_self;

void plat::exit() {
    closeSave();
    if (g_net) { curl_global_cleanup(); socketExit(); }
    if (g_fbReady) framebufferClose(&g_fb);
    if (g_acc) accountExit();
    if (g_pdm) pdmqryExit();
    if (g_ns) nsExit();
    if (g_pl) plExit();
    if (g_romfs) romfsExit();
}

std::string plat::userName() { return g_userName; }

std::string plat::userKey() {
    char buf[40];
    snprintf(buf, sizeof(buf), "%016lx%016lx", (unsigned long)g_uid.uid[1], (unsigned long)g_uid.uid[0]);
    return buf;
}

void plat::listGames(std::vector<GameInfo>& out, ProgressFn progress, void* ctx) {
    out.clear();
    std::vector<NsApplicationRecord> recs;
    NsApplicationRecord buf[64];
    s32 offset = 0;
    while (true) {
        s32 n = 0;
        if (R_FAILED(nsListApplicationRecord(buf, 64, offset, &n)) || n <= 0) break;
        recs.insert(recs.end(), buf, buf + n);
        offset += n;
    }
    std::unique_ptr<NsApplicationControlData> ctl(new NsApplicationControlData);
    for (size_t i = 0; i < recs.size(); i++) {
        if (progress) progress((int)i, (int)recs.size(), ctx);
        GameInfo g;
        g.id = recs[i].application_id;
        u64 sz = 0;
        if (R_SUCCEEDED(nsGetApplicationControlData(NsApplicationControlSource_Storage, g.id, ctl.get(), sizeof(NsApplicationControlData), &sz))) {
            NacpLanguageEntry* le = NULL;
            int idx = -1;
            if (R_SUCCEEDED(nacpGetLanguageEntry(&ctl->nacp, &le)) && le) idx = (int)(le - ctl->nacp.lang_data.lang);
            std::vector<std::string> names;
            if (ctl->nacp.titles_data_format == 1) {
                // [21.0.0+] compressed titles: the plain lang[] array would read as garbage
                decodeCompressedTitles((const uint8_t*)&ctl->nacp.lang_data, sizeof(ctl->nacp.lang_data), names);
            } else {
                for (int li = 0; li < 16; li++)
                    names.push_back(std::string(ctl->nacp.lang_data.lang[li].name, strnlen(ctl->nacp.lang_data.lang[li].name, 0x200)));
            }
            if (idx >= 0 && idx < (int)names.size() && isCleanText(names[idx])) g.name = names[idx];
            for (size_t li = 0; li < names.size() && !isCleanText(g.name); li++) g.name = names[li];   // any valid language
            if (!isCleanText(g.name)) g.name.clear();
            if (sz > sizeof(NacpStruct)) g.iconJpeg.assign(ctl->icon, ctl->icon + (sz - sizeof(NacpStruct)));
        }
        if (g.name.empty()) {
            char t[32];
            snprintf(t, sizeof(t), "%016lX", (unsigned long)g.id);
            g.name = t;
        }
        out.push_back(std::move(g));
    }
    if (progress) progress((int)recs.size(), (int)recs.size(), ctx);
}

PlayStats plat::playStats(uint64_t id) {
    PlayStats p;
    if (!g_pdm) return p;
    PdmPlayStatistics s;
    memset(&s, 0, sizeof(s));
    if (R_SUCCEEDED(pdmqryQueryPlayStatisticsByApplicationIdAndUserAccountId(id, g_uid, false, &s))) {
        p.valid = true;
        p.playtimeNs = s.playtime;
        p.launches = s.total_launches;
        p.lastPlayed = s.last_timestamp_user;
    }
    return p;
}

bool plat::openSave(uint64_t id) {
    closeSave();
    Result rc = fsdevMountSaveDataReadOnly("achsave", id, g_uid);
    g_saveMounted = R_SUCCEEDED(rc);
    if (!g_saveMounted) {
        char b[160];
        // 0x7D402 (module 2 / desc 1002) = no save for this user; other codes often mean the game is running
        snprintf(b, sizeof(b), "mount failed: rc=0x%X (module %u, desc %u)%s", rc, R_MODULE(rc), R_DESCRIPTION(rc),
                 rc == 0x7D402 ? " = no save data for this user" : " (is the game still running/suspended?)");
        g_lastError = b;
    } else g_lastError.clear();
    return g_saveMounted;
}

std::string plat::lastError() { return g_lastError; }

std::string plat::liveModuleStatus() {
    const u64 LIVE_ID = 0x420000000000AC01ULL;
    char b[200];
    u64 pid = 0;
    bool pmd = R_SUCCEEDED(pmdmntInitialize());
    if (pmd && R_SUCCEEDED(pmdmntGetProcessId(&pid, LIVE_ID)) && pid) {
        pmdmntExit();
        snprintf(b, sizeof(b), "running (pid %llu)", (unsigned long long)pid);
        return b;
    }
    if (pmd) pmdmntExit();
    FILE* f = fopen("sdmc:/atmosphere/contents/420000000000AC01/exefs.nsp", "rb");
    if (!f) return "not installed (atmosphere/contents/420000000000AC01/exefs.nsp missing)";
    fclose(f);
    FILE* flag = fopen("sdmc:/atmosphere/contents/420000000000AC01/flags/boot2.flag", "rb");
    bool hasFlag = flag != nullptr;
    if (flag) fclose(flag);
    Result rc = pmshellInitialize();
    if (R_FAILED(rc)) {
        snprintf(b, sizeof(b), "not running; pm:shell unavailable rc=0x%X", rc);
        return b;
    }
    NcmProgramLocation loc;
    memset(&loc, 0, sizeof(loc));
    loc.program_id = LIVE_ID;
    loc.storageID = NcmStorageId_None;
    rc = pmshellLaunchProgram(0, &loc, &pid);
    pmshellExit();
    if (R_SUCCEEDED(rc)) snprintf(b, sizeof(b), "was not running -> started now (pid %llu)%s", (unsigned long long)pid,
                                  hasFlag ? "" : "; boot2.flag missing, it won't start at boot");
    else snprintf(b, sizeof(b), "not running and could not start: rc=0x%X (module %u, desc %u)%s", rc, R_MODULE(rc),
                  R_DESCRIPTION(rc), hasFlag ? "" : "; boot2.flag missing");
    return b;
}

// ---------------------------------------------------------------- updates
struct HttpCtx {
    std::vector<uint8_t>* out;
    plat::ProgressFn progress;
    void* ctx;
};

static size_t httpWrite(char* p, size_t sz, size_t n, void* user) {
    HttpCtx* h = (HttpCtx*)user;
    h->out->insert(h->out->end(), (uint8_t*)p, (uint8_t*)p + sz * n);
    return sz * n;
}

static int httpProgress(void* user, curl_off_t total, curl_off_t now, curl_off_t, curl_off_t) {
    HttpCtx* h = (HttpCtx*)user;
    if (h->progress && total > 0) h->progress((int)(now >> 10), (int)(total >> 10), h->ctx);   // KB
    return 0;
}

bool plat::netInit(std::string& err) {
    if (g_net) return true;
    Result rc = socketInitializeDefault();
    if (R_FAILED(rc)) { char b[64]; snprintf(b, sizeof(b), "network unavailable (rc=0x%X)", rc); err = b; return false; }
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) { socketExit(); err = "curl init failed"; return false; }
    g_net = true;
    return true;
}

void plat::keepAwake(bool on) { appletSetMediaPlaybackState(on); }

std::string plat::localIp() {
    if (R_FAILED(nifmInitialize(NifmServiceType_User))) return "";
    u32 ip = 0;
    Result rc = nifmGetCurrentIpAddress(&ip);
    nifmExit();
    if (R_FAILED(rc) || ip == 0) return "";
    char b[20];
    snprintf(b, sizeof(b), "%u.%u.%u.%u", ip & 0xFF, (ip >> 8) & 0xFF, (ip >> 16) & 0xFF, ip >> 24);
    return b;
}

int plat::httpGet(const std::string& url, const std::vector<std::string>& headers, std::vector<uint8_t>& out,
                  ProgressFn progress, void* ctx, std::string& err) {
    out.clear();
    if (!netInit(err)) return 0;
    CURL* c = curl_easy_init();
    if (!c) { err = "curl init failed"; return 0; }
    struct curl_slist* hl = nullptr;
    for (const std::string& h : headers) hl = curl_slist_append(hl, h.c_str());
    HttpCtx hc = {&out, progress, ctx};
    char ebuf[CURL_ERROR_SIZE] = "";
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hl);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, httpWrite);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &hc);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, httpProgress);
    curl_easy_setopt(c, CURLOPT_XFERINFODATA, &hc);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_ERRORBUFFER, ebuf);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 100L);   // give up after 30 s below 100 B/s
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 30L);
    CURLcode r = curl_easy_perform(c);
    long code = 0;
    if (r == CURLE_OK) curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    else err = std::string(curl_easy_strerror(r)) + (ebuf[0] ? std::string(": ") + ebuf : "");
    curl_slist_free_all(hl);
    curl_easy_cleanup(c);
    return (int)code;
}

int plat::httpPost(const std::string& url, const std::vector<std::string>& headers, const std::string& body,
                   std::vector<uint8_t>& out, std::string& err) {
    out.clear();
    if (!netInit(err)) return 0;
    CURL* c = curl_easy_init();
    if (!c) { err = "curl init failed"; return 0; }
    struct curl_slist* hl = nullptr;
    for (const std::string& h : headers) hl = curl_slist_append(hl, h.c_str());
    HttpCtx hc = {&out, nullptr, nullptr};
    char ebuf[CURL_ERROR_SIZE] = "";
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hl);
    curl_easy_setopt(c, CURLOPT_POST, 1L);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.data());
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)body.size());
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, httpWrite);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &hc);
    curl_easy_setopt(c, CURLOPT_ERRORBUFFER, ebuf);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 100L);   // give up after 30 s below 100 B/s
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 30L);
    CURLcode r = curl_easy_perform(c);
    long code = 0;
    if (r == CURLE_OK) curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    else err = std::string(curl_easy_strerror(r)) + (ebuf[0] ? std::string(": ") + ebuf : "");
    curl_slist_free_all(hl);
    curl_easy_cleanup(c);
    return (int)code;
}

void plat::setSelfPath(const std::string& p) { g_self = p; }
std::string plat::selfPath() { return g_self; }
std::string plat::sdRoot() { return "sdmc:/"; }

bool plat::restartLiveModule(std::string& msg) {
    const u64 LIVE_ID = 0x420000000000AC01ULL;
    Result rc = pmshellInitialize();
    if (R_FAILED(rc)) { char b[80]; snprintf(b, sizeof(b), "pm:shell unavailable (rc=0x%X)", rc); msg = b; return false; }
    u64 oldPid = liveModulePid();
    pmshellTerminateProgram(LIVE_ID);   // fails harmlessly if it wasn't running
    // termination is asynchronous: launching again while the old process still exists fails
    for (int i = 0; i < 50 && liveModulePid() != 0; i++) svcSleepThread(100000000ULL);
    if (liveModulePid() != 0) {
        pmshellExit();
        char b[120];
        snprintf(b, sizeof(b), "the old live module (pid %llu) did not stop: reboot to use the new one", (unsigned long long)oldPid);
        msg = b;
        return false;
    }
    NcmProgramLocation loc;
    memset(&loc, 0, sizeof(loc));
    loc.program_id = LIVE_ID;
    loc.storageID = NcmStorageId_None;
    u64 pid = 0;
    rc = pmshellLaunchProgram(0, &loc, &pid);
    pmshellExit();
    char b[80];
    if (R_SUCCEEDED(rc)) { snprintf(b, sizeof(b), "live module restarted (pid %llu)", (unsigned long long)pid); msg = b; return true; }
    snprintf(b, sizeof(b), "live module not restarted (rc=0x%X): reboot to use the new one", rc);
    msg = b;
    return false;
}

void plat::releaseSelfFile() {
    if (g_romfs) romfsExit();
    g_romfs = false;
}

uint64_t plat::liveModulePid() {
    u64 pid = 0;
    if (R_FAILED(pmdmntInitialize())) return 0;
    if (R_FAILED(pmdmntGetProcessId(&pid, 0x420000000000AC01ULL))) pid = 0;
    pmdmntExit();
    return pid;
}

void plat::relaunchSelf() {
    if (!g_self.empty()) envSetNextLoad(g_self.c_str(), g_self.c_str());
}

bool plat::readSaveFile(const std::string& rel, std::vector<uint8_t>& out) {
    if (!g_saveMounted) return false;
    return readFile("achsave:/" + rel, out);
}

void plat::listSaveFiles(std::vector<std::pair<std::string, uint64_t>>& out) {
    out.clear();
    if (g_saveMounted) listFilesRecursive("achsave:/", "", out);
}

void plat::closeSave() {
    if (g_saveMounted) fsdevUnmountDevice("achsave");
    g_saveMounted = false;
}

std::string plat::dataDir() { return "sdmc:/switch/TrophyNX"; }
std::string plat::romfsDir() { return "romfs:"; }

bool plat::fontData(int which, const uint8_t** data, size_t* size) {
    if (!g_pl) return false;
    PlFontData f;
    if (R_FAILED(plGetSharedFontByType(&f, which == 0 ? PlSharedFontType_Standard : PlSharedFontType_NintendoExt))) return false;
    *data = (const uint8_t*)f.address;
    *size = f.size;
    return true;
}

uint64_t plat::now() { return (uint64_t)time(NULL); }

bool plat::poll() {
    if (!appletMainLoop()) return false;
    padUpdate(&g_pad);
    g_down = padGetButtonsDown(&g_pad);
    g_held = padGetButtons(&g_pad);
    return true;
}

uint32_t* plat::beginDraw(int& stride) {
    u32 s = 0;
    uint32_t* p = (uint32_t*)framebufferBegin(&g_fb, &s);
    stride = (int)(s / 4);
    return p;
}

void plat::endDraw() { framebufferEnd(&g_fb); }
uint32_t plat::buttonsDown() { return mapButtons(g_down); }
uint32_t plat::buttonsHeld() { return mapButtons(g_held); }
void plat::sleepMs(int ms) { svcSleepThread((s64)ms * 1000000LL); }
#endif
