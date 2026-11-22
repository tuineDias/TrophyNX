#include "sync.h"
#include "cJSON.h"
#include "version.h"
#include <cstdio>
#include <cstring>

// Sends the unlocked achievements to the TrophyNX Hub site. The payload is built from the library the app
// already has in memory (same data as the screen), so the site needs no copy of the game definitions.
//   POST <url>   Authorization: Bearer <token>   Content-Type: application/json
//   { "v":1, "user":..., "device":"Switch", "app_version":..., "sent":<unix>,
//     "totals":{"points","unlocked","count"},
//     "games":[ { "id":"0100...", "name", "launches", "hours", "last_played", "icon":"data:image/jpeg;base64,...",
//                 "achievements":[ {"id","name","desc","points","tier","hidden","unlocked","time"} ] } ] }

static const size_t MAX_ICON = 80 * 1024;          // one icon (JPEG bytes)
static const size_t MAX_ICONS_TOTAL = 1500 * 1024; // all icons together
static const size_t MAX_BODY = 4800 * 1024;        // the server accepts up to 5 MB

static std::string jstr(cJSON* o, const char* k) {
    cJSON* v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) && v->valuestring ? v->valuestring : "";
}

static std::string trim(std::string s) {
    while (!s.empty() && (unsigned char)s.back() <= ' ') s.pop_back();
    size_t i = 0;
    while (i < s.size() && (unsigned char)s[i] <= ' ') i++;
    return s.substr(i);
}

static std::string base64(const std::vector<uint8_t>& in) {
    static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        uint32_t v = (in[i] << 16) | (in[i + 1] << 8) | in[i + 2];
        out += T[v >> 18]; out += T[(v >> 12) & 63]; out += T[(v >> 6) & 63]; out += T[v & 63];
    }
    if (i + 1 == in.size()) {
        uint32_t v = in[i] << 16;
        out += T[v >> 18]; out += T[(v >> 12) & 63]; out += "==";
    } else if (i + 2 == in.size()) {
        uint32_t v = (in[i] << 16) | (in[i + 1] << 8);
        out += T[v >> 18]; out += T[(v >> 12) & 63]; out += T[(v >> 6) & 63]; out += '=';
    }
    return out;
}

static char* buildPayload(const Library& lib, const std::string& user, bool withIcons, int& nGames, int& nAchs) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "v", 1);
    cJSON_AddStringToObject(root, "user", user.c_str());
    cJSON_AddStringToObject(root, "device", "Switch");
    cJSON_AddStringToObject(root, "app_version", APP_VERSION);
    cJSON_AddNumberToObject(root, "sent", (double)plat::now());
    cJSON* totals = cJSON_AddObjectToObject(root, "totals");
    cJSON_AddNumberToObject(totals, "points", lib.totalPoints);
    cJSON_AddNumberToObject(totals, "unlocked", lib.totalUnlocked);
    cJSON_AddNumberToObject(totals, "count", lib.totalCount);
    cJSON* games = cJSON_AddArrayToObject(root, "games");
    size_t iconBytes = 0;
    nGames = nAchs = 0;
    for (size_t gi = 0; gi < lib.games.size(); gi++) {
        const Game& g = lib.games[gi];
        if (g.unlocked <= 0) continue;   // like a profile page: only games you have progress in
        cJSON* jg = cJSON_CreateObject();
        char idb[24];
        if (gi == 0) snprintf(idb, sizeof(idb), "global");
        else snprintf(idb, sizeof(idb), "%016llX", (unsigned long long)g.id);
        cJSON_AddStringToObject(jg, "id", idb);
        cJSON_AddStringToObject(jg, "name", g.name.c_str());
        if (gi != 0 && g.stats.valid) {
            cJSON_AddNumberToObject(jg, "launches", g.stats.launches);
            cJSON_AddNumberToObject(jg, "hours", (double)(long long)(g.stats.playtimeNs / 3.6e10) / 100.0);
            cJSON_AddNumberToObject(jg, "last_played", (double)g.stats.lastPlayed);
        }
        if (withIcons && !g.iconJpeg.empty() && g.iconJpeg.size() <= MAX_ICON && iconBytes + g.iconJpeg.size() <= MAX_ICONS_TOTAL) {
            iconBytes += g.iconJpeg.size();
            cJSON_AddStringToObject(jg, "icon", ("data:image/jpeg;base64," + base64(g.iconJpeg)).c_str());
        }
        cJSON* ja = cJSON_AddArrayToObject(jg, "achievements");
        for (const Achievement& a : g.achs) {
            cJSON* o = cJSON_CreateObject();
            bool secret = a.hidden && !a.unlocked;   // hidden achievements stay hidden on the site too
            cJSON_AddStringToObject(o, "id", a.id.c_str());
            cJSON_AddStringToObject(o, "name", secret ? "???" : a.name.c_str());
            cJSON_AddStringToObject(o, "desc", secret ? "" : a.desc.c_str());
            cJSON_AddNumberToObject(o, "points", a.points);
            if (!a.tier.empty()) cJSON_AddStringToObject(o, "tier", a.tier.c_str());
            if (a.hidden) cJSON_AddBoolToObject(o, "hidden", 1);
            cJSON_AddBoolToObject(o, "unlocked", a.unlocked ? 1 : 0);
            if (a.unlocked) { cJSON_AddNumberToObject(o, "time", (double)a.unlockTime); nAchs++; }
            cJSON_AddItemToArray(ja, o);
        }
        cJSON_AddItemToArray(games, jg);
        nGames++;
    }
    char* s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return s;
}

bool syncToSite(const Library& lib, std::string& msg) {
    std::vector<uint8_t> buf;
    std::string path = plat::dataDir() + "/sync.json";
    if (!readFile(path, buf)) { msg = "No sync.json yet: see docs/site-sync.md"; return false; }
    buf.push_back(0);
    cJSON* cfg = cJSON_Parse((const char*)buf.data());
    if (!cfg) { msg = "sync.json is not valid JSON"; return false; }
    std::string url = trim(jstr(cfg, "url")), token = trim(jstr(cfg, "token")), user = trim(jstr(cfg, "user"));
    cJSON_Delete(cfg);
    if (url.empty() || token.empty() || user.empty()) { msg = "sync.json needs url, token and user"; return false; }
    if (url.rfind("https://", 0) != 0) { msg = "sync.json: url must start with https:// (the token is a secret)"; return false; }

    int nGames = 0, nAchs = 0;
    char* raw = buildPayload(lib, user, true, nGames, nAchs);
    if (!raw) { msg = "Not enough memory to build the data"; return false; }
    std::string body(raw);
    cJSON_free(raw);
    if (body.size() > MAX_BODY) {   // too big with icons: send without them
        raw = buildPayload(lib, user, false, nGames, nAchs);
        if (!raw) { msg = "Not enough memory to build the data"; return false; }
        body = raw;
        cJSON_free(raw);
    }
    if (nGames == 0) { msg = "Nothing to send yet: no achievement unlocked."; return false; }

    std::vector<std::string> headers = {"Content-Type: application/json", "Authorization: Bearer " + token, "User-Agent: TrophyNX-sync"};
    std::vector<uint8_t> answer;
    std::string err;
    int st = plat::httpPost(url, headers, body, answer, err);
    if (st == 200) {
        msg = "Sent to the site: " + std::to_string(nGames) + (nGames == 1 ? " game, " : " games, ") + std::to_string(nAchs) + " achievements.";
        return true;
    }
    if (st == 0) msg = "Could not reach the site (is the Switch online?) " + err;
    else if (st == 401) msg = "The site refused the token (401): check sync.json.";
    else if (st == 413) msg = "The data is too big for the site (413).";
    else msg = "The site answered HTTP " + std::to_string(st) + ".";
    return false;
}
