/**
 * TrophyNX Sync UI - In-app settings for sync configuration
 * Header file
 */

#pragma once

#include <string>
#include "sync.h"

namespace trophynx::sync_ui {

struct SyncSettings {
    std::string apiUrl;
    std::string token;
    std::string user;
    bool enabled = false;
};

/**
 * Load settings from sdmc:/switch/TrophyNX/sync_settings.json
 */
bool loadSettings(SyncSettings& settings);

/**
 * Save settings to sdmc:/switch/TrophyNX/sync_settings.json
 */
bool saveSettings(const SyncSettings& settings);

/**
 * Draw the sync settings screen
 * Returns true if settings were changed and need reload
 */
bool drawSettingsScreen(Canvas& fb, u64 keysDown, u64 keysHeld);

/**
 * Handle input for settings screen
 * Returns true if still in settings screen, false to exit
 */
bool handleSettingsInput(u64 keysDown, u64 keysHeld, SyncSettings& settings);

/**
 * Quick sync from settings screen (called when user presses A on "Sync Now")
 */
trophynx::sync::SyncResult quickSync(const SyncSettings& settings);

/**
 * Check if settings are valid for syncing
 */
bool isConfigured(const SyncSettings& settings);

} // namespace trophynx::sync_ui