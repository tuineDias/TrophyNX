// Platform layer: everything that touches Switch services lives behind this
// interface, so the rules engine and UI can also be built and tested on a PC.
#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

struct GameInfo {
    uint64_t id = 0;
    std::string name;
    std::vector<uint8_t> iconJpeg;
};

struct PlayStats {
    bool valid = false;
    uint64_t playtimeNs = 0;
    uint32_t launches = 0;
    uint64_t lastPlayed = 0; // POSIX time
};

namespace plat {
    // Initializes services and selects the user. Returns false and fills err on failure.
    bool init(std::string& err);
    void exit();

    std::string userName();
    std::string userKey();   // stable hex id of the selected user, used for unlock files

    using ProgressFn = void (*)(int done, int total, void* ctx);
    void listGames(std::vector<GameInfo>& out, ProgressFn progress, void* ctx);
    PlayStats playStats(uint64_t titleId);

    // Mounts the selected user's save for a title (read-only).
    bool openSave(uint64_t titleId);
    bool readSaveFile(const std::string& relPath, std::vector<uint8_t>& out);
    // Every file of the mounted save: relative path ('/' separated) and size.
    void listSaveFiles(std::vector<std::pair<std::string, uint64_t>>& out);
    void closeSave();
    std::string lastError();
    // Checks the background module (sys-trophynx); starts it if it isn't running.
    std::string liveModuleStatus();   // human-readable reason for the last failed openSave

    // Updates (update.cpp)
    // HTTPS GET into memory. Returns the HTTP status, or 0 on a network/TLS error (err says why).
    int httpGet(const std::string& url, const std::vector<std::string>& headers, std::vector<uint8_t>& out,
                ProgressFn progress, void* ctx, std::string& err);
    // Sockets (shared by httpGet and the Wi-Fi share server). Starts them once; false + err if no network.
    bool netInit(std::string& err);
    std::string localIp();
    void keepAwake(bool on);   // no auto-sleep / screen dimming (while sharing over Wi-Fi)   // the console's address on the local network ("" if not connected)
    void setSelfPath(const std::string& path);   // argv[0] of the running .nro
    std::string selfPath();
    std::string sdRoot();                          // "sdmc:/" (host: <root>/sd/)
    bool restartLiveModule(std::string& msg);      // stops and starts sys-trophynx (new exefs.nsp)
    void relaunchSelf();                           // reload this .nro when the app exits
    // Closes the app's own .nro (romfs reads keep it open, and an open file can't be deleted or replaced).
    // Call before replacing the running .nro; romfs is unavailable afterwards (the app must restart).
    void releaseSelfFile();
    // Live module process id, 0 if it isn't running.
    uint64_t liveModulePid();

    std::string dataDir();   // writable folder for unlocks/custom definitions
    std::string romfsDir();  // bundled definitions
    bool fontData(int which, const uint8_t** data, size_t* size); // 0 = standard, 1 = Nintendo extended
    uint64_t now();

    // Input (polled once per frame)
    enum Button : uint32_t {
        BTN_A = 1, BTN_B = 2, BTN_X = 4, BTN_Y = 8, BTN_UP = 16, BTN_DOWN = 32,
        BTN_LEFT = 64, BTN_RIGHT = 128, BTN_PLUS = 256, BTN_MINUS = 512, BTN_L = 1024, BTN_R = 2048, BTN_ZR = 4096, BTN_ZL = 8192,
    };
    bool poll();                          // false = app should quit; updates buttons
    uint32_t* beginDraw(int& stride);     // 1280x720 RGBA8888 buffer
    void endDraw();
    uint32_t buttonsDown();   // newly pressed
    uint32_t buttonsHeld();
    void sleepMs(int ms);
}

bool readFile(const std::string& path, std::vector<uint8_t>& out);
void listFilesRecursive(const std::string& root, const std::string& rel, std::vector<std::pair<std::string, uint64_t>>& out, int depth = 0);
// Full paths of the files called `name` under dir (up to 16 folders deep, at most 16 results).
void findFilesNamed(const std::string& dir, const std::string& name, std::vector<std::string>& out, int depth = 0);
bool isCleanText(const std::string& s);   // valid UTF-8, no control characters
bool writeFile(const std::string& path, const std::string& data);
void makeDirs(const std::string& path);
std::vector<std::string> listDir(const std::string& path);
bool pathExists(const std::string& path);
// Renamed app (Switch Achievements -> TrophyNX): moves the old data folder to the new one if the new one
// doesn't exist yet (rename, or copy when the rename fails). Returns a line for the log ("" = nothing done).
std::string migrateLegacyDir(const std::string& oldDir, const std::string& newDir);
