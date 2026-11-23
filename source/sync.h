/**
 * TrophyNX Sync - Multi-user achievement sync for TrophyNX
 * Header file
 */

#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace trophynx::sync {

struct Achievement {
    std::string id;
    std::string name;
    std::string desc;
    int points = 0;
    bool unlocked = false;
    bool hidden = false;
    uint64_t unlockTime = 0;
    double value = 0.0;
    std::string tier; // platinum, gold, silver, bronze
};

struct GameData {
    std::string titleId;
    std::string name;
    std::string iconUrl;      // HTTP URL if available
    std::string iconBase64;   // Base64 encoded JPEG (fallback)
    std::string iconJpeg;     // Raw JPEG data (fallback)
    int unlocked = 0;
    int total = 0;
    int points = 0;
    int maxPoints = 0;
    uint64_t playtime = 0;    // seconds
    int launches = 0;
    uint64_t lastPlayed = 0;  // timestamp
    bool hasDefinition = false;
    std::vector<Achievement> achievements;
};

struct SyncConfig {
    std::string apiUrl;       // e.g., "https://trophynx-hub.xxx.workers.dev/api/sync"
    std::string token;        // SYNC_TOKEN from Cloudflare
    std::string user;         // Username (2-32 chars, alphanumeric/_-)
};

struct SyncResult {
    bool success = false;
    std::string message;
    int gamesSynced = 0;
    int newUnlocks = 0;
    int totalUnlocked = 0;
    int totalPoints = 0;
};

/**
 * Load sync configuration from sdmc:/switch/TrophyNX/sync.json
 */
bool loadConfig(SyncConfig& config);

/**
 * Collect all game data from TrophyNX library for syncing
 * Only includes games with at least one unlocked achievement
 */
std::vector<GameData> collectGameData();

/**
 * Send collected game data to the API
 */
SyncResult sendToApi(const SyncConfig& config, const std::vector<GameData>& games);

/**
 * Main sync function - call this from your button handler (e.g., R3 press)
 * Returns result with success status and message for display
 */
SyncResult doSync();

/**
 * Encode binary data as base64
 */
std::string base64Encode(const uint8_t* data, size_t len);

/**
 * Encode JPEG image data as base64 with size limit
 * Returns empty string if too large or encoding fails
 */
std::string encodeIconBase64(const uint8_t* jpegData, size_t jpegSize, size_t maxSize = 50000);

} // namespace trophynx::sync