#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include "achievements.h"
#include "platform.h"
#include "render.h"

#include "version.h"
#include "scan.h"
#include "update.h"
#include "captures.h"
#include "share_server.h"
#include "sync.h"

// ---------------------------------------------------------------- theme
static const uint32_t C_BG = 0xFF12141A, C_PANEL = 0xFF1B1E26, C_PANEL_HI = 0xFF272B36, C_LINE = 0xFF2E3340;
static const uint32_t C_TEXT = 0xFFEDEFF4, C_SUB = 0xFF9BA1AF, C_DIM = 0xFF5B6170;
static const uint32_t C_GOLD = 0xFFF2C14E, C_GREEN = 0xFF4CD08A, C_RED = 0xFFE5676B, C_TRACK = 0xFF2C303B;

static uint32_t tierColor(int points) {
    if (points <= 10) return 0xFFCD8B4E;   // bronze
    if (points <= 25) return 0xFFC3CAD4;   // silver
    if (points <= 50) return 0xFFF2C14E;   // gold
    return 0xFF8FE3F5;                     // platinum
}

struct UI {
    Canvas cv;
    Font font;
    Library lib;
    std::vector<Image> icons;
    std::vector<bool> iconTried;
    int sel = 0, gameScroll = 0;
    int achSel = 0, achScroll = 0;
    bool focusAch = false;
    bool popup = false;
    bool debug = false;
    int confirm = 0;          // 0 none, 1 one achievement, 2 whole game, 3 everything
    std::string toast;        // short message shown at the bottom after an action
    int debugScroll = 0;
    int lastProgress = -1;
    const char* loadingLabel = "Reading your games";
    int upd = 0;              // update panel: 0 none, 1 check result, 2 install result
    UpdateInfo updInfo;
    bool updOk = false;
    std::string updResult;
    int cap = 0;              // save captures panel: 0 none, 1 panel, 2 sharing over Wi-Fi
    int capDelete = 0;        // - pressed once: press again to delete
    CaptureSettings capSet;
    std::vector<CaptureInfo> capList;
    ShareServer server;
    std::string shareIp, shareErr;
};

static const int SHARE_PORT = 8080;

static UI* g_ui = nullptr;

static std::string fmtInt(long long v) {
    std::string s = std::to_string(v), out;
    int n = 0;
    for (int i = (int)s.size() - 1; i >= 0; i--) {
        out.insert(out.begin(), s[i]);
        if (++n % 3 == 0 && i > 0 && s[i - 1] != '-') out.insert(out.begin(), ',');
    }
    return out;
}

static std::string fmtNum(double v) {
    if (fabs(v - llround(v)) < 0.05) return fmtInt(llround(v));
    char b[32];
    snprintf(b, sizeof(b), "%.1f", v);
    return b;
}

static std::string fmtDate(uint64_t t) {
    if (!t) return "";
    time_t tt = (time_t)t;
    struct tm* tmv = gmtime(&tt);
    if (!tmv) return "";
    char b[32];
    strftime(b, sizeof(b), "%d %b %Y", tmv);
    return b;
}

// ---------------------------------------------------------------- drawing helpers
static void drawTrophy(Canvas& c, float cx, float cy, float s, uint32_t col) {
    // cup
    c.fillRound(cx - s * 0.42f, cy - s * 0.5f, s * 0.84f, s * 0.55f, s * 0.22f, col);
    c.fillRect((int)(cx - s * 0.42f), (int)(cy - s * 0.5f), (int)(s * 0.84f), (int)(s * 0.2f), col);
    // handles
    c.ring(cx - s * 0.42f, cy - s * 0.3f, s * 0.2f, s * 0.08f, col);
    c.ring(cx + s * 0.42f, cy - s * 0.3f, s * 0.2f, s * 0.08f, col);
    // stem + base
    c.fillRect((int)(cx - s * 0.07f), (int)(cy + s * 0.02f), (int)(s * 0.14f), (int)(s * 0.22f), col);
    c.fillRound(cx - s * 0.3f, cy + s * 0.22f, s * 0.6f, s * 0.16f, s * 0.05f, col);
}

static void drawLock(Canvas& c, float cx, float cy, float s, uint32_t col) {
    c.ring(cx, cy - s * 0.12f, s * 0.22f, s * 0.08f, col);
    c.fillRound(cx - s * 0.3f, cy - s * 0.08f, s * 0.6f, s * 0.46f, s * 0.08f, col);
}

static void drawCheck(Canvas& c, float cx, float cy, float s, uint32_t col) {
    c.line(cx - s * 0.28f, cy + s * 0.02f, cx - s * 0.08f, cy + s * 0.22f, s * 0.12f, col);
    c.line(cx - s * 0.08f, cy + s * 0.22f, cx + s * 0.3f, cy - s * 0.2f, s * 0.12f, col);
}

static void progressBar(Canvas& c, int x, int y, int w, int h, double frac, uint32_t col) {
    frac = std::max(0.0, std::min(1.0, frac));
    c.fillRound((float)x, (float)y, (float)w, (float)h, h / 2.0f, C_TRACK);
    if (frac > 0) c.fillRound((float)x, (float)y, std::max((float)h, (float)(w * frac)), (float)h, h / 2.0f, col);
}

static void buttonHint(UI& ui, int& x, int y, const char* btn, const char* label) {
    Canvas& c = ui.cv;
    c.fillCircle(x + 13.f, y + 13.f, 13.f, C_TEXT);
    int tw = ui.font.measure(15, btn);
    ui.font.draw(c, x + 13 - tw / 2, y + 4, 15, btn, C_BG);
    x += 32;
    x += ui.font.draw(c, x, y + 2, 19, label, C_SUB) + 26;
}

static const Image* gameIcon(UI& ui, int gi) {
    if (gi <= 0) return nullptr;
    if ((int)ui.icons.size() != (int)ui.lib.games.size()) {
        ui.icons.assign(ui.lib.games.size(), Image());
        ui.iconTried.assign(ui.lib.games.size(), false);
    }
    if (!ui.iconTried[gi]) {
        ui.iconTried[gi] = true;
        decodeIcon(ui.lib.games[gi].iconJpeg, 56, ui.icons[gi]);
    }
    return ui.icons[gi].valid() ? &ui.icons[gi] : nullptr;
}

static void drawHeader(UI& ui) {
    Canvas& c = ui.cv;
    c.gradientV(0, 0, 1280, 84, 0xFF1F2330, C_BG);
    c.fillCircle(52, 42, 26, 0xFF2A2F3D);
    drawTrophy(c, 52, 44, 30, C_GOLD);
    ui.font.draw(c, 92, 22, 30, "TrophyNX", C_TEXT);
    std::string user = ui.font.ellipsize(22, plat::userName(), 300);
    int uw = ui.font.measure(22, user);
    ui.font.draw(c, 1256 - uw, 16, 22, user, C_TEXT);
    std::string stats = fmtInt(ui.lib.totalUnlocked) + " / " + fmtInt(ui.lib.totalCount) + " unlocked   " + fmtInt(ui.lib.totalPoints) + " pts";
    int sw = ui.font.measure(18, stats);
    ui.font.draw(c, 1256 - sw, 46, 18, stats, C_GOLD);
    c.fillRect(0, 84, 1280, 1, C_LINE);
}

static const int LIST_X = 24, LIST_Y = 100, LIST_W = 420, ROW_H = 78, LIST_ROWS = 7;
static const int DET_X = 464, DET_W = 792, ACH_Y = 236, ACH_H = 84, ACH_ROWS = 5;

static void drawGameList(UI& ui) {
    Canvas& c = ui.cv;
    Library& lib = ui.lib;
    c.fillRound(LIST_X, LIST_Y - 4, LIST_W, ROW_H * LIST_ROWS + 8, 14, C_PANEL);
    c.setClip(LIST_X, LIST_Y - 4, LIST_W, ROW_H * LIST_ROWS + 8);
    for (int r = 0; r < LIST_ROWS; r++) {
        int gi = ui.gameScroll + r;
        if (gi >= (int)lib.games.size()) break;
        const Game& g = lib.games[gi];
        int y = LIST_Y + r * ROW_H;
        bool selected = gi == ui.sel;
        if (selected) {
            c.fillRound(LIST_X + 6, y + 2, LIST_W - 12, ROW_H - 4, 10, C_PANEL_HI);
            if (!ui.focusAch) c.fillRound(LIST_X + 6, y + 14, 4, ROW_H - 28, 2, C_GOLD);
        }
        int ix = LIST_X + 18, iy = y + 11;
        if (gi == 0) {
            c.fillRound(ix, iy, 56, 56, 12, 0xFF2F3545);
            drawTrophy(c, ix + 28, iy + 30, 34, C_GOLD);
        } else if (const Image* img = gameIcon(ui, gi)) {
            c.blitRounded(*img, ix, iy, 10, g.stats.launches == 0);
        } else {
            c.fillRound(ix, iy, 56, 56, 12, 0xFF343A4A);
            std::string ch = g.name.substr(0, 1);
            ui.font.draw(c, ix + 28 - ui.font.measure(26, ch) / 2, iy + 12, 26, ch, C_SUB);
        }
        int tx = ix + 70;
        ui.font.draw(c, tx, y + 12, 20, g.name, g.stats.launches > 0 || gi == 0 ? C_TEXT : C_SUB, LIST_W - (tx - LIST_X) - 16);
        double frac = g.achs.empty() ? 0 : (double)g.unlocked / g.achs.size();
        progressBar(c, tx, y + 48, 200, 7, frac, frac >= 1 ? C_GREEN : C_GOLD);
        std::string cnt = std::to_string(g.unlocked) + "/" + std::to_string(g.achs.size());
        ui.font.draw(c, tx + 212, y + 41, 17, cnt, C_SUB);
        if (g.hasDefinition) {
            int bx = LIST_X + LIST_W - 30;
            c.fillCircle(bx, y + 52, 9, 0xFF3A3220);
            drawTrophy(c, bx, y + 53, 12, C_GOLD);
        }
    }
    c.resetClip();
    // scrollbar
    int n = (int)lib.games.size();
    if (n > LIST_ROWS) {
        int track = ROW_H * LIST_ROWS - 16;
        int th = std::max(30, track * LIST_ROWS / n);
        int ty = LIST_Y + 4 + (track - th) * ui.gameScroll / std::max(1, n - LIST_ROWS);
        c.fillRound(LIST_X + LIST_W - 7, ty, 4, th, 2, C_DIM);
    }
}

static void drawDetails(UI& ui) {
    Canvas& c = ui.cv;
    Library& lib = ui.lib;
    const Game& g = lib.games[ui.sel];
    c.fillRound(DET_X, LIST_Y - 4, DET_W, ROW_H * LIST_ROWS + 8, 14, C_PANEL);

    int x = DET_X + 24;
    std::string title = ui.sel == 0 ? "Overview" : g.name;
    ui.font.draw(c, x, LIST_Y + 12, 26, title, C_TEXT, DET_W - 200);

    std::string line = std::to_string(g.unlocked) + " of " + std::to_string(g.achs.size()) + " unlocked   " + fmtInt(g.points) + " / " + fmtInt(g.maxPoints) + " pts";
    if (ui.sel == 0) {
        int played = 0;
        double hours = 0;
        for (size_t i = 1; i < lib.games.size(); i++) {
            if (lib.games[i].stats.launches > 0) played++;
            hours += lib.games[i].stats.playtimeNs / 3.6e12;
        }
        line += "   " + std::to_string(played) + " games played   " + fmtInt((long long)hours) + " h";
    } else if (g.stats.valid) {
        line += "   " + fmtNum(floor(g.stats.playtimeNs / 3.6e11) / 10.0) + " h played";
    }
    ui.font.draw(c, x, LIST_Y + 50, 18, line, C_SUB, DET_W - 200);

    double frac = g.achs.empty() ? 0 : (double)g.unlocked / g.achs.size();
    std::string pct = std::to_string((int)floor(frac * 100)) + "%";
    int pw = ui.font.measure(40, pct);
    ui.font.draw(c, DET_X + DET_W - 24 - pw, LIST_Y + 14, 40, pct, frac >= 1 ? C_GREEN : C_GOLD);
    progressBar(c, x, LIST_Y + 86, DET_W - 48, 8, frac, frac >= 1 ? C_GREEN : C_GOLD);

    // save status line
    std::string status;
    uint32_t scol = C_SUB;
    if (ui.sel == 0) {
        if (!lib.loadErrors.empty()) { status = "Some definition files have errors (press X)"; scol = C_RED; }
        else if (lib.liveStatus.find("running") == 0 || lib.liveStatus.find("started now") != std::string::npos) { status = "Live module: " + lib.liveStatus; scol = C_GREEN; }
        else { status = "Live module: " + lib.liveStatus; scol = lib.liveStatus.find("not installed") == 0 ? C_SUB : C_RED; }
    } else if (g.hasDefinition && g.reader.empty()) {
        status = "Themed play-time set (save format not supported yet)";
    } else if (g.hasDefinition) {
        switch (g.saveStatus) {
            case SaveStatus::Ok: status = "Save data read"; scol = C_GREEN; break;
            case SaveStatus::NoSave: status = "No save data for this user yet (if the game is suspended, close it and press Y)"; scol = C_GOLD; break;
            case SaveStatus::Unreadable: status = "Save found but not readable (unsupported version?)"; scol = C_RED; break;
            default: break;
        }
    } else {
        status = "Play-time achievements only (no game-specific set yet)";
    }
    ui.font.draw(c, x, LIST_Y + 104, 16, status, scol, DET_W - 48);
    c.fillRect(DET_X + 16, ACH_Y - 8, DET_W - 32, 1, C_LINE);

    c.setClip(DET_X, ACH_Y - 6, DET_W, ACH_H * ACH_ROWS + 6);
    for (int r = 0; r < ACH_ROWS; r++) {
        int ai = ui.achScroll + r;
        if (ai >= (int)g.achs.size()) break;
        const Achievement& a = g.achs[ai];
        int y = ACH_Y + r * ACH_H;
        if (ui.focusAch && ai == ui.achSel) {
            c.fillRound(DET_X + 10, y, DET_W - 20, ACH_H - 6, 10, C_PANEL_HI);
            c.fillRound(DET_X + 10, y + 14, 4, ACH_H - 34, 2, C_GOLD);
        }
        float bx = DET_X + 52, by = y + 38;
        uint32_t tc = tierColor(a.points);
        if (a.unlocked) {
            c.fillCircle(bx, by, 27, tc);
            c.fillCircle(bx, by, 22, 0xFF1B1E26);
            drawTrophy(c, bx, by + 2, 26, tc);
        } else {
            c.fillCircle(bx, by, 27, 0xFF2C303B);
            drawLock(c, bx, by + 2, 26, C_DIM);
        }
        int tx = DET_X + 96;
        int rightW = 230;
        std::string name = a.name;
        std::string desc = (a.hidden && !a.unlocked) ? "Hidden achievement - keep playing!" : a.desc;
        int nw = ui.font.draw(c, tx, y + 12, 21, name, a.unlocked ? C_TEXT : 0xFFC4C8D2, DET_W - (tx - DET_X) - rightW - 70);
        if (a.isNew) {
            int px = tx + nw + 10;
            c.fillRound(px, y + 13, 46, 22, 11, C_GOLD);
            ui.font.draw(c, px + 8, y + 15, 15, "NEW", C_BG);
        }
        ui.font.draw(c, tx, y + 44, 17, desc, C_SUB, DET_W - (tx - DET_X) - rightW - 10);

        int rx = DET_X + DET_W - 24;
        std::string pts = std::to_string(a.points) + " pts";
        int ptw = ui.font.measure(16, pts);
        if (a.unlocked) {
            std::string d = fmtDate(a.unlockTime);
            int dw = ui.font.measure(17, d);
            c.fillCircle(rx - dw - 16, y + 24, 9, C_GREEN);
            drawCheck(c, rx - dw - 16, y + 24, 16, C_BG);
            ui.font.draw(c, rx - dw, y + 14, 17, d, C_GREEN);
            ui.font.draw(c, rx - ptw, y + 46, 16, pts, tc);
        } else {
            const Rule& rl = a.rule;
            if (rl.showProgress && !(a.hidden)) {
                double cur = std::min(a.value, rl.target) / rl.divisor, tgt = rl.target / rl.divisor;
                std::string p = fmtNum(floor(cur * 10) / 10) + " / " + fmtNum(tgt);
                int pw2 = ui.font.measure(17, p);
                ui.font.draw(c, rx - pw2, y + 12, 17, p, C_TEXT);
                progressBar(c, rx - 170, y + 38, 170, 6, rl.target > 0 ? a.value / rl.target : 0, tc);
            } else {
                ui.font.draw(c, rx - ui.font.measure(17, "Locked"), y + 12, 17, "Locked", C_DIM);
            }
            ui.font.draw(c, rx - ptw, y + 52, 16, pts, C_DIM);
        }
    }
    c.resetClip();
    int n = (int)g.achs.size();
    if (n > ACH_ROWS) {
        int track = ACH_H * ACH_ROWS - 12;
        int th = std::max(30, track * ACH_ROWS / n);
        int ty = ACH_Y + (track - th) * ui.achScroll / std::max(1, n - ACH_ROWS);
        c.fillRound(DET_X + DET_W - 7, ty, 4, th, 2, C_DIM);
    }
}

static void drawFooter(UI& ui) {
    Canvas& c = ui.cv;
    c.fillRect(0, 668, 1280, 1, C_LINE);
    int x = 28;
    if (ui.debug) { buttonHint(ui, x, 680, "B", "Close debug"); buttonHint(ui, x, 680, "+", "Exit"); return; }
    if (ui.upd || ui.cap) { buttonHint(ui, x, 680, "+", "Exit"); return; }
    if (ui.popup) buttonHint(ui, x, 680, "A", "Close");
    else if (ui.focusAch) buttonHint(ui, x, 680, "B", "Back to games");
    else buttonHint(ui, x, 680, "A", "Open");
    buttonHint(ui, x, 680, "Y", "Rescan saves");
    if (ui.sel == 0 && !ui.focusAch) buttonHint(ui, x, 680, "ZR", "Compatibility scan");
    if (ui.sel != 0) buttonHint(ui, x, 680, "ZR", "Save captures");
    buttonHint(ui, x, 680, "X", "Debug");
    buttonHint(ui, x, 680, "-", ui.focusAch ? "Re-lock" : (ui.sel == 0 ? "Re-lock all" : "Re-lock game"));
    buttonHint(ui, x, 680, "ZL", "Update");
    buttonHint(ui, x, 680, "R3", "Sync");
    buttonHint(ui, x, 680, "+", "Exit");
    std::string v = "v" APP_VERSION;
    ui.font.draw(c, 1256 - ui.font.measure(16, v), 684, 16, v, C_DIM);
}

static void drawPopup(UI& ui) {
    Canvas& c = ui.cv;
    Library& lib = ui.lib;
    c.fillRect(0, 0, 1280, 720, 0xB0000000);
    int n = (int)lib.newlyUnlocked.size();
    int shown = std::min(n, 6);
    int h = 130 + shown * 62 + (n > shown ? 30 : 0);
    int w = 720, x = (1280 - w) / 2, y = (720 - h) / 2;
    c.fillRound(x, y, w, h, 18, C_PANEL);
    c.fillRound(x, y, w, 6, 3, C_GOLD);
    drawTrophy(c, x + 48, y + 52, 40, C_GOLD);
    std::string t = n == 1 ? "Achievement unlocked!" : std::to_string(n) + " achievements unlocked!";
    ui.font.draw(c, x + 84, y + 30, 28, t, C_TEXT);
    for (int i = 0; i < shown; i++) {
        const Game& g = lib.games[lib.newlyUnlocked[i].first];
        const Achievement& a = g.achs[lib.newlyUnlocked[i].second];
        int ry = y + 96 + i * 62;
        uint32_t tc = tierColor(a.points);
        c.fillCircle(x + 48, ry + 24, 20, tc);
        c.fillCircle(x + 48, ry + 24, 16, C_PANEL);
        drawTrophy(c, x + 48, ry + 26, 19, tc);
        ui.font.draw(c, x + 84, ry + 4, 20, a.name, C_TEXT, w - 220);
        ui.font.draw(c, x + 84, ry + 30, 16, lib.newlyUnlocked[i].first == 0 ? "Overview" : g.name, C_SUB, w - 220);
        std::string p = "+" + std::to_string(a.points);
        ui.font.draw(c, x + w - 36 - ui.font.measure(20, p), ry + 12, 20, p, tc);
    }
    if (n > shown) ui.font.draw(c, x + 84, y + 96 + shown * 62, 17, "...and " + std::to_string(n - shown) + " more", C_SUB);
}

static const int DBG_LINES = 22;

static void drawDebug(UI& ui) {
    Canvas& c = ui.cv;
    const Game& g = ui.lib.games[ui.sel];
    c.fillRect(0, 0, 1280, 720, 0xC0000000);
    int x = 60, y = 40, w = 1160, h = 616;
    c.fillRound(x, y, w, h, 16, C_PANEL);
    c.fillRound(x, y, w, 5, 2, 0xFF8FE3F5);
    ui.font.draw(c, x + 24, y + 18, 24, "Debug - " + (ui.sel == 0 ? std::string("Overview") : g.name), C_TEXT, w - 260);
    std::string hint = "also in /switch/TrophyNX/log.txt";
    ui.font.draw(c, x + w - 24 - ui.font.measure(15, hint), y + 26, 15, hint, C_SUB);
    c.fillRect(x + 16, y + 60, w - 32, 1, C_LINE);
    int n = (int)g.debug.size();
    for (int i = 0; i < DBG_LINES; i++) {
        int li = ui.debugScroll + i;
        if (li >= n) break;
        const std::string& l = g.debug[li];
        uint32_t col = C_TEXT;
        if (l.find("[MET]") != std::string::npos) col = C_GREEN;
        if (l.find("NEW UNLOCK") != std::string::npos || l.find("UNLOCKED") != std::string::npos) col = C_GOLD;
        if (l.find("not present") != std::string::npos || l.find("[not yet]") != std::string::npos) col = C_SUB;
        if (l.find("!!") != std::string::npos || l.find("ERROR") != std::string::npos || l.find("failed") != std::string::npos ||
            l.find("NOT RECOGNISED") != std::string::npos) col = C_RED;
        ui.font.draw(c, x + 28, y + 74 + i * 24, 17, l, col, w - 70);
    }
    if (n > DBG_LINES) {
        int track = DBG_LINES * 24;
        int th = std::max(30, track * DBG_LINES / n);
        int ty = y + 74 + (track - th) * ui.debugScroll / std::max(1, n - DBG_LINES);
        c.fillRound(x + w - 14, ty, 4, th, 2, C_DIM);
    }
}

static void drawConfirm(UI& ui) {
    Canvas& c = ui.cv;
    const Game& g = ui.lib.games[ui.sel];
    c.fillRect(0, 0, 1280, 720, 0xB0000000);
    int w = 700, h = 230, x = (1280 - w) / 2, y = (720 - h) / 2;
    c.fillRound(x, y, w, h, 18, C_PANEL);
    c.fillRound(x, y, w, 6, 3, C_RED);
    std::string title, line1, line2;
    if (ui.confirm == 1) {
        title = "Re-lock this achievement?";
        line1 = g.achs[ui.achSel].name + "  (" + (ui.sel == 0 ? std::string("Overview") : g.name) + ")";
    } else if (ui.confirm == 2) {
        title = "Re-lock every achievement of this game?";
        line1 = g.name + "  (" + std::to_string(g.unlocked) + " unlocked)";
    } else {
        title = "Re-lock ALL achievements for this user?";
        line1 = std::to_string(ui.lib.totalUnlocked) + " unlocked achievements will be cleared";
    }
    line2 = "They unlock again at the next scan (Y) if the save still qualifies.";
    ui.font.draw(c, x + 32, y + 32, 26, title, C_TEXT, w - 64);
    ui.font.draw(c, x + 32, y + 82, 19, line1, C_GOLD, w - 64);
    ui.font.draw(c, x + 32, y + 116, 17, line2, C_SUB, w - 64);
    int bx = x + 32;
    buttonHint(ui, bx, y + 170, "A", "Re-lock");
    buttonHint(ui, bx, y + 170, "B", "Cancel");
}

static std::string fmtSize(uint64_t b) {
    char t[32];
    if (b >= 1024 * 1024) snprintf(t, sizeof(t), "%.1f MB", b / 1048576.0);
    else snprintf(t, sizeof(t), "%llu KB", (unsigned long long)((b + 1023) / 1024));
    return t;
}

static std::string fileLabel(const std::string& path) {
    if (path == "switch/TrophyNX.nro") return "app";
    if (path.find("/exefs.nsp") != std::string::npos) return "live module";
    size_t s = path.find_last_of('/');
    return s == std::string::npos ? path : path.substr(s + 1);
}

static void drawUpdate(UI& ui) {
    Canvas& c = ui.cv;
    const UpdateInfo& u = ui.updInfo;
    c.fillRect(0, 0, 1280, 720, 0xB0000000);
    // room for "What's new" (CHANGELOG.md bullets) when an update is available
    size_t notes = (ui.upd != 2 && u.state == UpdateInfo::Available) ? u.notes.size() : 0;
    if (notes > 7) notes = 7;
    int w = 960, h = 380 + (notes ? 34 + 34 * (int)notes : 0), x = (1280 - w) / 2, y = (720 - h) / 2 - 20;
    if (h > 680) h = 680;
    if (y < 10) y = 10;
    c.fillRound(x, y, w, h, 18, C_PANEL);
    std::string title;
    uint32_t accent = C_GOLD;
    std::vector<std::pair<std::string, uint32_t>> lines;   // text, colour ('\n' splits)
    std::vector<std::pair<const char*, const char*>> btns;
    std::string build = (u.fromRelease ? "Release " + u.source.substr(u.source.find(' ') + 1) : (u.commit.empty() ? std::string("") : "Test build " + u.commit.substr(0, 7))) +
                        (u.date.empty() ? "" : "  ·  " + u.date);
    if (ui.upd == 2) {
        title = ui.updOk ? "Update installed" : "Update failed";
        accent = ui.updOk ? C_GREEN : C_RED;
        lines.push_back({ui.updResult, C_TEXT});
        if (u.appChanged) {   // the app released its own file to replace it: it has to restart
            lines.push_back({ui.updOk ? "Restart the app to use the new version." : "Restart the app (or reboot) to finish.", C_SUB});
            btns.push_back({"A", "Restart the app"});
            btns.push_back({"+", "Exit"});
        } else btns.push_back({"B", "Close"});
    } else if (u.state == UpdateInfo::Error) {
        title = "Update check failed";
        accent = C_RED;
        lines.push_back({u.error, C_TEXT});
        btns.push_back({"B", "Close"});
    } else if (u.state == UpdateInfo::UpToDate) {
        title = "You're up to date";
        accent = C_GREEN;
        lines.push_back({build, C_SUB});
        if (!u.message.empty()) lines.push_back({u.message, C_SUB});
        btns.push_back({"B", "Close"});
    } else {
        title = "Update available";
        lines.push_back({build, C_GOLD});
        if (!u.message.empty()) lines.push_back({u.message, C_TEXT});
        std::string vers;
        if (!u.appVersion.empty()) vers = "App " + u.appVersion;
        if (!u.moduleVersion.empty()) vers += (vers.empty() ? "" : "  ·  ") + std::string("Live module ") + u.moduleVersion;
        if (!vers.empty()) lines.push_back({vers, C_SUB});
        if (notes) {
            lines.push_back({"What's new:", C_GOLD});
            for (size_t i = 0; i < notes; i++) lines.push_back({"\xC2\xB7 " + u.notes[i], C_TEXT});
        }
        std::string ch;
        for (const UpdateFile& f : u.files)
            if (f.changed) ch += (ch.empty() ? "" : ", ") + fileLabel(f.path) + " (" + fmtSize(f.size) + ")";
        lines.push_back({"Changes: " + ch, C_SUB});
        if (u.moduleChanged) lines.push_back({"The live module is restarted after the update.", C_SUB});
        btns.push_back({"A", "Download and install"});
        btns.push_back({"B", "Later"});
    }
    c.fillRound(x, y, w, 6, 3, accent);
    ui.font.draw(c, x + 32, y + 30, 26, title, C_TEXT, w - 64);
    int ly = y + 84;
    for (auto& l : lines) {
        size_t a = 0;
        while (a <= l.first.size() && ly < y + h - 70) {
            size_t b = l.first.find('\n', a);
            std::string part = l.first.substr(a, b == std::string::npos ? std::string::npos : b - a);
            ui.font.draw(c, x + 32, ly, 18, part, l.second, w - 64);
            ly += 30;
            if (b == std::string::npos) break;
            a = b + 1;
        }
        ly += 4;
    }
    int bx = x + 32;
    for (auto& b : btns) buttonHint(ui, bx, y + h - 50, b.first, b.second);
}

static void openCaptures(UI& ui) {
    ui.capSet = loadCaptureSettings(plat::dataDir());
    ui.capList = listCaptures(plat::dataDir(), ui.lib.games[ui.sel].id);
    ui.capDelete = 0;
    ui.cap = 1;
}

static void startSharing(UI& ui) {
    ui.shareErr.clear();
    ui.shareIp.clear();
    std::string err;
    if (!plat::netInit(err)) { ui.shareErr = err; ui.cap = 2; return; }
    ui.shareIp = plat::localIp();
    if (ui.shareIp.empty()) { ui.shareErr = "The console is not connected to a network. Connect it to Wi-Fi first."; ui.cap = 2; return; }
    ui.server.dataDir = plat::dataDir();
    ui.server.pagePath = plat::romfsDir() + "/savelab.html";
    ui.server.names.clear();
    for (size_t i = 1; i < ui.lib.games.size(); i++) ui.server.names[ui.lib.games[i].id] = ui.lib.games[i].name;
    ui.server.requests = 0;
    ui.server.bytesSent = 0;
    ui.server.lastPath.clear();
    if (!ui.server.start(SHARE_PORT, err)) ui.shareErr = "Could not start the server: " + err;
    else plat::keepAwake(true);
    ui.cap = 2;
}

static void drawCaptures(UI& ui) {
    Canvas& c = ui.cv;
    const Game& g = ui.lib.games[ui.sel];
    c.fillRect(0, 0, 1280, 720, 0xB0000000);
    int w = 900, h = 440, x = (1280 - w) / 2, y = (720 - h) / 2 - 20;
    c.fillRound(x, y, w, h, 18, C_PANEL);
    int bx = x + 32;
    if (ui.cap == 2) {
        bool ok = ui.shareErr.empty();
        c.fillRound(x, y, w, 6, 3, ok ? C_GREEN : C_RED);
        ui.font.draw(c, x + 32, y + 30, 26, ok ? "Sharing over Wi-Fi" : "Wi-Fi sharing unavailable", C_TEXT, w - 64);
        if (!ok) {
            ui.font.draw(c, x + 32, y + 90, 19, ui.shareErr, C_RED, w - 64);
        } else {
            ui.font.draw(c, x + 32, y + 88, 19, "On your phone or computer, connected to the same Wi-Fi, open:", C_SUB, w - 64);
            std::string url = "http://" + ui.shareIp + ":" + std::to_string(SHARE_PORT);
            ui.font.draw(c, x + 32, y + 126, 40, url, C_GOLD, w - 64);
            ui.font.draw(c, x + 32, y + 196, 18, "The Save Lab opens with your captures: \"From your Switch\" at the top of the page.", C_TEXT, w - 64);
            ui.font.draw(c, x + 32, y + 226, 18, "Type the address exactly (http, not https). Keep this screen open while it loads.", C_SUB, w - 64);
            std::string st = std::to_string(ui.server.requests) + " file(s) sent, " + fmtSize(ui.server.bytesSent);
            ui.font.draw(c, x + 32, y + 290, 18, st, C_GREEN, w - 64);
            if (!ui.server.lastPath.empty()) ui.font.draw(c, x + 32, y + 318, 15, "last: " + ui.server.lastPath, C_DIM, w - 64);
        }
        buttonHint(ui, bx, y + h - 50, "B", ok ? "Stop sharing" : "Back");
        return;
    }
    bool on = captureEnabled(ui.capSet, g.id);
    c.fillRound(x, y, w, 6, 3, on ? C_GREEN : 0xFF8FE3F5);
    ui.font.draw(c, x + 32, y + 30, 26, "Save captures - " + g.name, C_TEXT, w - 64);
    ui.font.draw(c, x + 32, y + 84, 21, on ? "Recording: ON" : "Recording: off", on ? C_GREEN : C_SUB);
    uint64_t bytes = 0;
    for (const CaptureInfo& ci : ui.capList) bytes += ci.bytes;
    std::string n = ui.capList.empty() ? std::string("No capture yet.")
                                        : std::to_string(ui.capList.size()) + (ui.capList.size() == 1 ? " capture, " : " captures, ") + fmtSize(bytes) +
                                              "  ·  last: " + fmtDate(ui.capList.back().time) + "  ·  keeps the last " + std::to_string(ui.capSet.keep);
    ui.font.draw(c, x + 32, y + 120, 18, n, C_TEXT, w - 64);
    const char* lines[] = {
        "When recording is on, the live module copies this game's save each time you CLOSE the game.",
        "A game keeps its save locked while it runs, so closing it is when a copy can be taken:",
        "close it (HOME, then X) after each step worth comparing (a boss, a new item, a chapter...).",
        "Unchanged saves are not kept twice. Then share them over Wi-Fi to the Save Lab (Y).",
    };
    for (int i = 0; i < 4; i++) ui.font.draw(c, x + 32, y + 168 + i * 28, 17, lines[i], C_SUB, w - 64);
    if (ui.capDelete) ui.font.draw(c, x + 32, y + 300, 18, "Press - again to delete every capture of this game.", C_RED, w - 64);
    buttonHint(ui, bx, y + h - 50, "A", on ? "Stop recording" : "Start recording");
    buttonHint(ui, bx, y + h - 50, "Y", "Share over Wi-Fi");
    if (!ui.capList.empty()) buttonHint(ui, bx, y + h - 50, "-", "Delete captures");
    buttonHint(ui, bx, y + h - 50, "B", "Close");
}

static void drawAll(UI& ui) {
    int stride;
    ui.cv.px = plat::beginDraw(stride);
    ui.cv.stride = stride;
    ui.cv.resetClip();
    ui.cv.clear(C_BG);
    drawHeader(ui);
    drawGameList(ui);
    drawDetails(ui);
    if (ui.popup) drawPopup(ui);
    if (ui.debug) drawDebug(ui);
    if (ui.confirm) drawConfirm(ui);
    if (ui.upd) drawUpdate(ui);
    if (ui.cap) drawCaptures(ui);
    drawFooter(ui);
    if (!ui.toast.empty()) {
        int tw = ui.font.measure(18, ui.toast) + 40;
        ui.cv.fillRound(640 - tw / 2, 616, tw, 40, 20, 0xFF2F3545);
        ui.font.draw(ui.cv, 640 - tw / 2 + 20, 625, 18, ui.toast, C_TEXT);
    }
    plat::endDraw();
}

static void drawLoading(UI& ui, int done, int total) {
    int stride;
    ui.cv.px = plat::beginDraw(stride);
    ui.cv.stride = stride;
    ui.cv.resetClip();
    Canvas& c = ui.cv;
    c.clear(C_BG);
    c.fillCircle(640, 280, 56, 0xFF232733);
    drawTrophy(c, 640, 286, 68, C_GOLD);
    std::string t = "TrophyNX";
    ui.font.draw(c, 640 - ui.font.measure(34, t) / 2, 360, 34, t, C_TEXT);
    std::string s = std::string(ui.loadingLabel) + (total > 0 ? "  " + std::to_string(done) + " / " + std::to_string(total) : "");
    ui.font.draw(c, 640 - ui.font.measure(19, s) / 2, 412, 19, s, C_SUB);
    progressBar(c, 440, 450, 400, 8, total > 0 ? (double)done / total : 0, C_GOLD);
    plat::endDraw();
}

static void onProgress(int done, int total, void* ctx) {
    UI* ui = (UI*)ctx;
    int pct = total > 0 ? done * 50 / total : 0;  // redraw at most ~50 times
    if (pct == ui->lastProgress && done != total) return;
    ui->lastProgress = pct;
    drawLoading(*ui, done, total);
}

static void showFatal(UI& ui, const std::string& msg) {
    while (plat::poll()) {
        int stride;
        ui.cv.px = plat::beginDraw(stride);
        if (!ui.cv.px) break;
        ui.cv.stride = stride;
        ui.cv.resetClip();
        ui.cv.clear(C_BG);
        ui.font.draw(ui.cv, 80, 300, 28, "TrophyNX could not start", C_TEXT);
        ui.font.draw(ui.cv, 80, 350, 20, msg, C_RED);
        ui.font.draw(ui.cv, 80, 400, 18, "Press + to exit", C_SUB);
        plat::endDraw();
        if (plat::buttonsDown() & (plat::BTN_PLUS | plat::BTN_B)) break;
        plat::sleepMs(16);
    }
}

static void clampScroll(UI& ui) {
    int n = (int)ui.lib.games.size();
    ui.sel = std::max(0, std::min(ui.sel, n - 1));
    if (ui.sel < ui.gameScroll) ui.gameScroll = ui.sel;
    if (ui.sel >= ui.gameScroll + LIST_ROWS) ui.gameScroll = ui.sel - LIST_ROWS + 1;
    int na = (int)ui.lib.games[ui.sel].achs.size();
    ui.achSel = std::max(0, std::min(ui.achSel, std::max(0, na - 1)));
    if (ui.achSel < ui.achScroll) ui.achScroll = ui.achSel;
    if (ui.achSel >= ui.achScroll + ACH_ROWS) ui.achScroll = ui.achSel - ACH_ROWS + 1;
    ui.achScroll = std::max(0, std::min(ui.achScroll, std::max(0, na - ACH_ROWS)));
}

int main(int argc, char** argv) {
    static UI ui;
    g_ui = &ui;
    if (argc > 0 && argv[0]) plat::setSelfPath(argv[0]);   // hbloader passes the .nro path
    std::string err;
    bool ok = plat::init(err);
    const uint8_t *f0 = nullptr, *f1 = nullptr;
    size_t s0 = 0, s1 = 0;
    plat::fontData(0, &f0, &s0);
    plat::fontData(1, &f1, &s1);
    if (f0) ui.font.load(f0, s0, f1, s1);
    if (!ok) {
        if (err != "No user selected.") showFatal(ui, err);
        plat::exit();
        return 0;
    }

    // The app was called "Switch Achievements" before 0.9.0: keep the unlocks, settings and token
    std::string moved = migrateLegacyDir(plat::sdRoot() + "switch/SwitchAchievements", plat::dataDir());
    std::string self = plat::selfPath(), oldNro = plat::sdRoot() + "switch/SwitchAchievements.nro";
    if (self.size() >= 12 && self.compare(self.size() - 12, 12, "TrophyNX.nro") == 0 && self != oldNro && pathExists(oldNro))
        remove(oldNro.c_str());   // installed by the old updater next to the old name: one app entry only
    drawLoading(ui, 0, 0);
    buildLibrary(ui.lib, onProgress, &ui);
    if (!moved.empty()) ui.toast = "Renamed to TrophyNX: your achievements were kept.";
    ui.popup = !ui.lib.newlyUnlocked.empty();
    if (!ui.lib.loadErrors.empty()) {
        makeDirs(plat::dataDir());
        writeFile(plat::dataDir() + "/definition_errors.txt", ui.lib.loadErrors);
    }

    bool dirty = true;
    int holdFrames = 0;
    uint32_t lastHeld = 0;
    while (plat::poll()) {
        uint32_t down = plat::buttonsDown(), held = plat::buttonsHeld();
        // key repeat for up/down
        uint32_t dirs = held & (plat::BTN_UP | plat::BTN_DOWN);
        if (dirs && dirs == lastHeld) {
            if (++holdFrames > 24 && holdFrames % 5 == 0) down |= dirs;
        } else holdFrames = 0;
        lastHeld = dirs;

        if (down & plat::BTN_PLUS) break;
        if (down && !ui.toast.empty()) { ui.toast.clear(); dirty = true; }
        if (ui.cap == 2) {
            if (ui.server.running() && ui.server.poll() > 0) dirty = true;
            if (down & plat::BTN_B) { ui.server.stop(); plat::keepAwake(false); ui.cap = 1; ui.capList = listCaptures(plat::dataDir(), ui.lib.games[ui.sel].id); dirty = true; }
        } else if (ui.cap == 1) {
            if (down) dirty = true;
            const Game& g = ui.lib.games[ui.sel];
            if (down & plat::BTN_MINUS) {
                if (ui.capList.empty()) {}
                else if (ui.capDelete) {
                    int n = deleteCaptures(plat::dataDir(), g.id);
                    ui.capList.clear();
                    ui.capDelete = 0;
                    ui.toast = std::to_string(n) + " capture(s) deleted.";
                } else ui.capDelete = 1;
            } else if (down) ui.capDelete = 0;
            if (down & plat::BTN_A) {
                bool on = !captureEnabled(ui.capSet, g.id);
                setCaptureEnabled(ui.capSet, g.id, on);
                if (!saveCaptureSettings(plat::dataDir(), ui.capSet)) ui.toast = "Could not write captures/settings.json";
                else ui.toast = on ? "Recording on: close the game to take a capture." : "Recording off (captures are kept).";
            }
            if (down & plat::BTN_Y) startSharing(ui);
            else if (down & plat::BTN_B) ui.cap = 0;
        } else if (ui.upd) {
            if (ui.upd == 1 && ui.updInfo.state == UpdateInfo::Available && (down & plat::BTN_A)) {
                ui.loadingLabel = "Downloading the update (KB)";
                ui.lastProgress = -1;
                drawLoading(ui, 0, 0);
                ui.updOk = applyUpdate(ui.updInfo, onProgress, &ui, ui.updResult);
                ui.upd = 2;
                dirty = true;
            } else if (ui.upd == 2 && ui.updInfo.appChanged && (down & plat::BTN_A)) {
                plat::relaunchSelf();
                break;
            } else if ((down & plat::BTN_B) && !(ui.upd == 2 && ui.updInfo.appChanged)) { ui.upd = 0; dirty = true; }
        } else if (ui.confirm) {
            if (down & plat::BTN_A) {
                int n = 0;
                if (ui.confirm == 1) n = relockAchievements(ui.lib, ui.sel, ui.achSel);
                else if (ui.confirm == 2) n = relockAchievements(ui.lib, ui.sel, -1);
                else n = relockAchievements(ui.lib, -1, -1);
                ui.toast = std::to_string(n) + (n == 1 ? " achievement" : " achievements") + " re-locked. Press Y to rescan.";
                ui.confirm = 0;
                dirty = true;
            } else if (down & plat::BTN_B) { ui.confirm = 0; dirty = true; }
        } else if (ui.popup) {
            if (down & (plat::BTN_A | plat::BTN_B)) { ui.popup = false; dirty = true; }
        } else if (ui.debug) {
            if (down) {
                dirty = true;
                int n = (int)ui.lib.games[ui.sel].debug.size();
                if (down & (plat::BTN_B | plat::BTN_X)) ui.debug = false;
                if (down & plat::BTN_UP) ui.debugScroll--;
                if (down & plat::BTN_DOWN) ui.debugScroll++;
                if (down & plat::BTN_L) ui.debugScroll -= DBG_LINES;
                if (down & plat::BTN_R) ui.debugScroll += DBG_LINES;
                ui.debugScroll = std::max(0, std::min(ui.debugScroll, std::max(0, n - DBG_LINES)));
            }
        } else if (down) {
            dirty = true;
            if (down & plat::BTN_Y) {
                ui.loadingLabel = "Rescanning saves";
                ui.lastProgress = -1;
                uint64_t keepId = ui.lib.games[ui.sel].id;
                rescanLibrary(ui.lib, onProgress, &ui);
                ui.icons.clear();   // order may have changed
                for (size_t i = 0; i < ui.lib.games.size(); i++) if (ui.lib.games[i].id == keepId) ui.sel = (int)i;
                ui.popup = !ui.lib.newlyUnlocked.empty();
            }
            if ((down & plat::BTN_ZR) && ui.sel != 0) openCaptures(ui);
            if ((down & plat::BTN_ZR) && ui.sel == 0 && !ui.focusAch) {
                ui.loadingLabel = "Checking every save";
                ui.lastProgress = -1;
                ScanSummary sum;
                if (runCompatibilityScan(ui.lib, onProgress, &ui, sum)) {
                    ui.toast = std::to_string(sum.withSave) + " saves checked: " + std::to_string(sum.draftable) + " draftable, " +
                               std::to_string(sum.text) + " text, " + std::to_string(sum.binary) + " binary. Upload scan_bundle.json.";
                } else ui.toast = "Scan failed: " + sum.error;
            }
            if (down & plat::BTN_STICKR) {   // R3: send the achievements to the TrophyNX Hub site
                ui.loadingLabel = "Sending to the site";
                drawLoading(ui, 0, 0);
                std::string syncMsg;
                syncToSite(ui.lib, syncMsg);
                ui.toast = syncMsg;
            }
            if (down & plat::BTN_ZL) {
                ui.loadingLabel = "Checking for updates";
                drawLoading(ui, 0, 0);
                ui.updInfo = checkForUpdate();
                ui.upd = 1;
            }
            if (down & plat::BTN_MINUS) {
                const Game& g = ui.lib.games[ui.sel];
                if (ui.focusAch) {
                    if (!g.achs.empty() && g.achs[ui.achSel].unlocked) ui.confirm = 1;
                    else ui.toast = "This achievement is still locked.";
                } else ui.confirm = ui.sel == 0 ? 3 : 2;
            } else if (down & plat::BTN_X) { ui.debug = true; ui.debugScroll = 0; }
            else if (!ui.focusAch) {
                if (down & plat::BTN_UP) ui.sel--;
                if (down & plat::BTN_DOWN) ui.sel++;
                if (down & plat::BTN_L) ui.sel -= LIST_ROWS;
                if (down & plat::BTN_R) ui.sel += LIST_ROWS;
                if (down & (plat::BTN_UP | plat::BTN_DOWN | plat::BTN_L | plat::BTN_R)) { ui.achSel = 0; ui.achScroll = 0; }
                if (down & (plat::BTN_A | plat::BTN_RIGHT)) ui.focusAch = true;
            } else if (!(down & plat::BTN_X)) {
                if (down & plat::BTN_UP) ui.achSel--;
                if (down & plat::BTN_DOWN) ui.achSel++;
                if (down & plat::BTN_L) ui.achSel -= ACH_ROWS;
                if (down & plat::BTN_R) ui.achSel += ACH_ROWS;
                if (down & (plat::BTN_B | plat::BTN_LEFT)) ui.focusAch = false;
            }
            clampScroll(ui);
        }
        if (dirty) { drawAll(ui); dirty = false; }
        plat::sleepMs(16);
    }
    plat::exit();
    return 0;
}
