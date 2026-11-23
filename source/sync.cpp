/**
 * TrophyNX Sync - Multi-user achievement sync for TrophyNX
 * Implementation file
 *
 * Integration notes:
 * - Include this in your build (add to Makefile SOURCES)
 * - Call trophynx::sync::doSync() from your button handler (e.g., R3 press)
 * - Requires: cJSON (already in TrophyNX), libcurl (via devkitPro), libnx
 * - Uses the same TLS/curl setup as update.cpp (ZL updater)
 */

#include "sync.h"
#include <switch.h>
#include <curl/curl.h>
#include <cjson/cJSON.h>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cstring>

#include "achievements.h"  // For Library, Game, Achievement structures
#include "render.h"        // For Image, icon handling
#include "fileutil.h"      // For file utilities

namespace trophynx::sync {

// ===== Configuration =====
static const char* CONFIG_PATH = "sdmc:/switch/TrophyNX/sync.json";
static const size_t MAX_ICON_BASE64 = 50000; // ~50KB limit for base64 icon
static const int HTTP_TIMEOUT_MS = 15000;

// ===== cURL write callback =====
static size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* response) {
    size_t total = size * nmemb;
    response->append(static_cast<char*>(contents), total);
    return total;
}

// ===== Base64 encoding =====
static const char* B64_CHARS = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64Encode(const uint8_t* data, size_t len) {
    std::string result;
    result.reserve((len + 2) / 3 * 4);

    for (size_t i = 0; i < len; i += 3) {
        uint32_t triplet = 0;
        int valid = 0;
        for (int j = 0; j < 3; ++j) {
            if (i + j < len) {
                triplet = (triplet << 8) | data[i + j];
                valid++;
            } else {
                triplet <<= 8;
            }
        }

        result.push_back(B64_CHARS[(triplet >> 18) & 0x3F]);
        result.push_back(B64_CHARS[(triplet >> 12) & 0x3F]);
        result.push_back(valid > 1 ? B64_CHARS[(triplet >> 6) & 0x3F] : '=');
        result.push_back(valid > 2 ? B64_CHARS[triplet & 0x3F] : '=');
    }
    return result;
}

std::string encodeIconBase64(const uint8_t* jpegData, size_t jpegSize, size_t maxSize) {
    if (!jpegData || jpegSize == 0 || jpegSize > maxSize * 4 / 3) {
        return "";
    }
    return base64Encode(jpegData, jpegSize);
}

// ===== Load sync.json config =====
bool loadConfig(SyncConfig& config) {
    FILE* f = fopen(CONFIG_PATH, "rb");
    if (!f) return false;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0 || size > 16384) {
        fclose(f);
        return false;
    }

    std::string jsonStr(size, '\0');
    fread(jsonStr.data(), 1, size, f);
    fclose(f);

    cJSON* root = cJSON_Parse(jsonStr.c_str());
    if (!root) return false;

    auto getStr = [&](const char* key, std::string& out) {
        cJSON* item = cJSON_GetObjectItem(root, key);
        if (cJSON_IsString(item) && item->valuestring) out = item->valuestring;
    };

    getStr("url", config.apiUrl);
    getStr("token", config.token);
    getStr("user", config.user);

    cJSON_Delete(root);

    // Validate
    if (config.apiUrl.empty() || config.token.empty() || config.user.empty()) return false;
    if (config.apiUrl.find("http") != 0) return false;
    if (config.user.length() < 2 || config.user.length() > 32) return false;

    return true;
}

// ===== Get icon data from game =====
static std::pair<const uint8_t*, size_t> getGameIconData(const Game& game) {
    // TrophyNX stores icon as JPEG in game.iconJpeg (vector<uint8_t>)
    // Check if we have access to it via the Game struct
    if (!game.iconJpeg.empty()) {
        return { game.iconJpeg.data(), game.iconJpeg.size() };
    }
    return { nullptr, 0 };
}

// ===== Collect game data from TrophyNX library =====
std::vector<GameData> collectGameData() {
    std::vector<GameData> result;

    // Access the global library from achievements.cpp
    // This assumes the library is accessible (extern or global)
    extern Library g_library;  // Defined in achievements.cpp

    for (size_t i = 1; i < g_library.games.size(); ++i) { // Skip index 0 (Overview)
        const Game& game = g_library.games[i];

        // Only sync games with at least one unlocked achievement
        if (game.unlocked == 0) continue;

        GameData data;
        data.titleId = "0" + std::to_string(game.id); // Convert to 16-char hex
        while (data.titleId.length() < 16) data.titleId = "0" + data.titleId;
        if (data.titleId.length() > 16) data.titleId = data.titleId.substr(data.titleId.length() - 16);

        data.name = game.name;
        data.unlocked = game.unlocked;
        data.total = static_cast<int>(game.achs.size());
        data.points = game.points;
        data.maxPoints = game.maxPoints;
        data.playtime = game.stats.playtime;
        data.launches = game.stats.launches;
        data.lastPlayed = game.stats.lastPlayed;
        data.hasDefinition = game.hasDefinition;

        // Try to get icon data
        auto [iconData, iconSize] = getGameIconData(game);
        if (iconData && iconSize > 0) {
            data.iconBase64 = encodeIconBase64(iconData, iconSize, MAX_ICON_BASE64);
            // Also store raw JPEG as fallback (smaller games only)
            if (iconSize <= 30000) {
                data.iconJpeg = base64Encode(iconData, iconSize);
            }
        }

        // Collect achievements
        for (const Achievement& ach : game.achs) {
            AchievementData achData;
            achData.id = ach.id;
            achData.name = ach.name;
            achData.desc = ach.desc;
            achData.points = ach.points;
            achData.unlocked = ach.unlocked;
            achData.hidden = ach.hidden;
            achData.unlockTime = ach.unlockTime;
            achData.value = ach.value;

            // Determine tier from points
            if (ach.points >= 100) achData.tier = "platinum";
            else if (ach.points >= 50) achData.tier = "gold";
            else if (ach.points >= 25) achData.tier = "silver";
            else achData.tier = "bronze";

            data.achievements.push_back(achData);
        }

        result.push_back(std::move(data));
    }

    return result;
}

// ===== Send to API =====
SyncResult sendToApi(const SyncConfig& config, const std::vector<GameData>& games) {
    SyncResult result;

    // Build JSON payload
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "user", config.user.c_str());

    cJSON* gamesObj = cJSON_CreateObject();
    for (const auto& game : games) {
        cJSON* gameObj = cJSON_CreateObject();
        cJSON_AddStringToObject(gameObj, "name", game.name.c_str());
        if (!game.iconBase64.empty()) cJSON_AddStringToObject(gameObj, "iconBase64", game.iconBase64.c_str());
        else if (!game.iconJpeg.empty()) cJSON_AddStringToObject(gameObj, "iconJpeg", game.iconJpeg.c_str());
        cJSON_AddStringToObject(gameObj, "titleId", game.titleId.c_str());
        cJSON_AddNumberToObject(gameObj, "unlocked", game.unlocked);
        cJSON_AddNumberToObject(gameObj, "total", game.total);
        cJSON_AddNumberToObject(gameObj, "points", game.points);
        cJSON_AddNumberToObject(gameObj, "maxPoints", game.maxPoints);
        cJSON_AddNumberToObject(gameObj, "playtime", game.playtime);
        cJSON_AddNumberToObject(gameObj, "launches", game.launches);
        cJSON_AddNumberToObject(gameObj, "lastPlayed", game.lastPlayed);
        cJSON_AddBoolToObject(gameObj, "hasDefinition", game.hasDefinition);

        cJSON* achsObj = cJSON_CreateObject();
        for (const auto& ach : game.achievements) {
            cJSON* achObj = cJSON_CreateObject();
            cJSON_AddStringToObject(achObj, "id", ach.id.c_str());
            cJSON_AddStringToObject(achObj, "name", ach.name.c_str());
            cJSON_AddStringToObject(achObj, "desc", ach.desc.c_str());
            cJSON_AddNumberToObject(achObj, "points", ach.points);
            cJSON_AddBoolToObject(achObj, "unlocked", ach.unlocked);
            cJSON_AddBoolToObject(achObj, "hidden", ach.hidden);
            cJSON_AddNumberToObject(achObj, "unlockTime", static_cast<double>(ach.unlockTime));
            cJSON_AddNumberToObject(achObj, "value", ach.value);
            cJSON_AddStringToObject(achObj, "tier", ach.tier.c_str());
            cJSON_AddItemToObject(achsObj, ach.id.c_str(), achObj);
        }
        cJSON_AddItemToObject(gameObj, "achievements", achsObj);
        cJSON_AddItemToObject(gamesObj, game.titleId.c_str(), gameObj);
    }
    cJSON_AddItemToObject(root, "games", gamesObj);

    char* jsonStr = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!jsonStr) {
        result.message = "Failed to create JSON payload";
        return result;
    }

    // Initialize curl
    CURL* curl = curl_easy_init();
    if (!curl) {
        free(jsonStr);
        result.message = "Failed to initialize curl";
        return result;
    }

    std::string response;
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, ("Authorization: Bearer " + config.token).c_str());
    headers = curl_slist_append(headers, "User-Agent: TrophyNX/1.0");

    curl_easy_setopt(curl, CURLOPT_URL, config.apiUrl.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, jsonStr);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, strlen(jsonStr));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, HTTP_TIMEOUT_MS);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_CAINFO, "sdmc:/switch/TrophyNX/cacert.pem"); // Optional: custom CA bundle

    // Use the same TLS options as update.cpp (ZL updater)
    // This ensures compatibility with Switch's TLS stack
    curl_easy_setopt(curl, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);

    free(jsonStr);

    CURLcode res = curl_easy_perform(curl);

    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        result.message = "Network error: " + std::string(curl_easy_strerror(res));
        return result;
    }

    if (httpCode == 401) {
        result.message = "Invalid sync token (check sync.json)";
        return result;
    }
    if (httpCode == 400) {
        result.message = "Invalid request: " + response;
        return result;
    }
    if (httpCode == 413) {
        result.message = "Payload too large (reduce icon sizes)";
        return result;
    }
    if (httpCode < 200 || httpCode >= 300) {
        result.message = "Server error: HTTP " + std::to_string(httpCode);
        return result;
    }

    // Parse response
    cJSON* respRoot = cJSON_Parse(response.c_str());
    if (!respRoot) {
        result.message = "Invalid server response";
        return result;
    }

    cJSON* okItem = cJSON_GetObjectItem(respRoot, "ok");
    if (cJSON_IsTrue(okItem)) {
        result.success = true;
        cJSON* msgItem = cJSON_GetObjectItem(respRoot, "message");
        if (cJSON_IsString(msgItem)) result.message = msgItem->valuestring;

        cJSON* stats = cJSON_GetObjectItem(respRoot, "stats");
        if (stats) {
            auto getInt = [&](const char* key) {
                cJSON* item = cJSON_GetObjectItem(stats, key);
                return cJSON_IsNumber(item) ? item->valueint : 0;
            };
            result.gamesSynced = getInt("gamesSynced");
            result.newUnlocks = getInt("newUnlocks");
            result.totalUnlocked = getInt("totalUnlocked");
            result.totalPoints = getInt("totalPoints");
        }
    } else {
        cJSON* errItem = cJSON_GetObjectItem(respRoot, "error");
        if (cJSON_IsString(errItem)) result.message = errItem->valuestring;
        else result.message = "Sync failed";
    }

    cJSON_Delete(respRoot);
    return result;
}

// ===== Main sync function =====
SyncResult doSync() {
    SyncResult result;

    // Load config
    SyncConfig config;
    if (!loadConfig(config)) {
        result.message = "No sync.json found at sdmc:/switch/TrophyNX/sync.json";
        return result;
    }

    // Collect game data
    std::vector<GameData> games = collectGameData();
    if (games.empty()) {
        result.message = "No games with unlocked achievements to sync";
        return result;
    }

    // Send to API
    result = sendToApi(config, games);
    return result;
}

} // namespace trophynx::sync