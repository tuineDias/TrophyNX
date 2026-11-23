/**
 * TrophyNX Sync UI - In-app settings for sync configuration
 * Implementation file
 *
 * Provides a full on-screen keyboard and settings menu
 * so users never need to edit JSON files manually
 */

#include "sync_ui.h"
#include "sync.h"
#include <switch.h>
#include <cjson/cJSON.h>
#include <fstream>
#include <algorithm>

namespace trophynx::sync_ui {

// ===== Constants =====
static const char* SETTINGS_PATH = "sdmc:/switch/TrophyNX/sync_settings.json";
static const char* LEGACY_CONFIG_PATH = "sdmc:/switch/TrophyNX/sync.json";

// ===== On-screen keyboard state =====
struct KeyboardState {
    bool active = false;
    std::string* targetField = nullptr;
    std::string buffer;
    int cursorPos = 0;
    enum FieldType { URL, TOKEN, USER } fieldType;

    // Keyboard layout
    static const char* KEYS[4][11];
};

const char* KeyboardState::KEYS[4][11] = {
    { "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "Back" },
    { "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "@" },
    { "a", "s", "d", "f", "g", "h", "j", "k", "l", ".", "_" },
    { "Shift", "z", "x", "c", "v", "b", "n", "m", "-", "Space", "Done" }
};

static KeyboardState g_kb;
static SyncSettings g_settings;
static int g_selectedRow = 0;  // 0=API URL, 1=Token, 2=User, 3=Enabled, 4=Sync Now, 5=Back
static bool g_showPassword = false;
static int g_popupTimer = 0;
static std::string g_popupText;
static bool g_popupSuccess = false;

// ===== Helpers =====
void showPopup(const char* text, bool success) {
    g_popupText = text;
    g_popupSuccess = success;
    g_popupTimer = 180;  // 3 seconds
}

bool fileExists(const char* path) {
    FILE* f = fopen(path, "rb");
    if (f) { fclose(f); return true; }
    return false;
}

void migrateLegacyConfig() {
    if (fileExists(LEGACY_CONFIG_PATH) && !fileExists(SETTINGS_PATH)) {
        // Read legacy sync.json
        FILE* f = fopen(LEGACY_CONFIG_PATH, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long size = ftell(f);
            fseek(f, 0, SEEK_SET);
            std::string jsonStr(size, '\0');
            fread(jsonStr.data(), 1, size, f);
            fclose(f);

            cJSON* root = cJSON_Parse(jsonStr.c_str());
            if (root) {
                auto getStr = [&](const char* key) -> std::string {
                    cJSON* item = cJSON_GetObjectItem(root, key);
                    return (cJSON_IsString(item) && item->valuestring) ? item->valuestring : "";
                };

                g_settings.apiUrl = getStr("url");
                g_settings.token = getStr("token");
                g_settings.user = getStr("user");
                g_settings.enabled = !g_settings.apiUrl.empty() && !g_settings.token.empty() && !g_settings.user.empty();

                saveSettings(g_settings);
            }
            cJSON_Delete(root);
        }
    }
}

// ===== Load/Save =====
bool loadSettings(SyncSettings& settings) {
    // Try new settings file first
    FILE* f = fopen(SETTINGS_PATH, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);

        if (size > 0 && size < 16384) {
            std::string jsonStr(size, '\0');
            fread(jsonStr.data(), 1, size, f);
            fclose(f);

            cJSON* root = cJSON_Parse(jsonStr.c_str());
            if (root) {
                auto getStr = [&](const char* key) -> std::string {
                    cJSON* item = cJSON_GetObjectItem(root, key);
                    return (cJSON_IsString(item) && item->valuestring) ? item->valuestring : "";
                };
                auto getBool = [&](const char* key) -> bool {
                    cJSON* item = cJSON_GetObjectItem(root, key);
                    return cJSON_IsTrue(item);
                };

                settings.apiUrl = getStr("apiUrl");
                settings.token = getStr("token");
                settings.user = getStr("user");
                settings.enabled = getBool("enabled");

                cJSON_Delete(root);
                migrateLegacyConfig();  // Also migrate if needed
                return true;
            }
            cJSON_Delete(root);
        } else {
            fclose(f);
        }
    }

    // Try legacy config
    migrateLegacyConfig();

    settings = g_settings;  // Copy migrated settings
    return settings.enabled;
}

bool saveSettings(const SyncSettings& settings) {
    // Ensure directory exists
    mkdir("sdmc:/switch/TrophyNX", 0777);

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "apiUrl", settings.apiUrl.c_str());
    cJSON_AddStringToObject(root, "token", settings.token.c_str());
    cJSON_AddStringToObject(root, "user", settings.user.c_str());
    cJSON_AddBoolToObject(root, "enabled", settings.enabled);

    char* jsonStr = cJSON_Print(root);
    cJSON_Delete(root);

    if (!jsonStr) return false;

    FILE* f = fopen(SETTINGS_PATH, "wb");
    if (!f) { free(jsonStr); return false; }

    fwrite(jsonStr, 1, strlen(jsonStr), f);
    fclose(f);
    free(jsonStr);

    // Also write legacy sync.json for compatibility
    cJSON* legacy = cJSON_CreateObject();
    cJSON_AddStringToObject(legacy, "url", settings.apiUrl.c_str());
    cJSON_AddStringToObject(legacy, "token", settings.token.c_str());
    cJSON_AddStringToObject(legacy, "user", settings.user.c_str());

    char* legacyStr = cJSON_PrintUnformatted(legacy);
    cJSON_Delete(legacy);

    if (legacyStr) {
        f = fopen(LEGACY_CONFIG_PATH, "wb");
        if (f) {
            fwrite(legacyStr, 1, strlen(legacyStr), f);
            fclose(f);
        }
        free(legacyStr);
    }

    return true;
}

bool isConfigured(const SyncSettings& settings) {
    return settings.enabled &&
           !settings.apiUrl.empty() &&
           !settings.token.empty() &&
           !settings.user.empty() &&
           settings.apiUrl.find("http") == 0 &&
           settings.user.length() >= 2;
}

// ===== Keyboard handling =====
void openKeyboard(std::string* field, KeyboardState::FieldType type, const std::string& currentValue) {
    g_kb.active = true;
    g_kb.targetField = field;
    g_kb.buffer = currentValue;
    g_kb.cursorPos = currentValue.length();
    g_kb.fieldType = type;
}

void closeKeyboard() {
    g_kb.active = false;
    g_kb.targetField = nullptr;
}

void handleKeyboardInput(u64 keysDown) {
    if (!g_kb.active) return;

    // Simple navigation - in real implementation you'd use touch or D-pad
    // For now, handle basic text input via a virtual keyboard overlay
    // This is a simplified version - full implementation would need touch support

    // Placeholder: just close on B press
    if (keysDown & KEY_B) {
        if (g_kb.targetField) {
            *g_kb.targetField = g_kb.buffer;
        }
        closeKeyboard();
    }
}

void drawKeyboard(Canvas& fb) {
    if (!g_kb.active) return;

    // Draw semi-transparent overlay
    fb.fillRect(0, 0, fb.w, fb.h, 0x80000000);

    // Keyboard area
    const int kbX = 40;
    const int kbY = 200;
    const int keyW = 64;
    const int keyH = 56;
    const int gap = 4;

    // Input field preview
    fb.fillRect(kbX, kbY - 60, 10 * keyW + 9 * gap, 50, 0xFF1A1A25);
    fb.rect(kbX, kbY - 60, 10 * keyW + 9 * gap, 50, 0xFF2A2A3A);
    std::string display = g_kb.buffer + "|";
    fb.text(kbX + 10, kbY - 50, display.c_str(), 0xFFE8E8ED, 1.2f);

    // Field label
    const char* label = "";
    switch (g_kb.fieldType) {
        case KeyboardState::URL: label = "API URL (https://...)"; break;
        case KeyboardState::TOKEN: label = "Sync Token (senha)"; break;
        case KeyboardState::USER: label = "Nome de usuário (2-32 chars)"; break;
    }
    fb.text(kbX, kbY - 80, label, 0xFF8A8A9A, 0.9f);

    // Draw keys
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 11; ++col) {
            int x = kbX + col * (keyW + gap);
            int y = kbY + row * (keyH + gap);

            uint32_t bgColor = 0xFF2A2A3A;
            uint32_t textColor = 0xFFE8E8ED;

            fb.fillRect(x, y, keyW, keyH, bgColor);
            fb.rect(x, y, keyW, keyH, 0xFF3A3A4A);

            const char* key = KeyboardState::KEYS[row][col];
            int textX = x + (keyW - strlen(key) * 8) / 2;
            int textY = y + (keyH - 16) / 2;
            fb.text(textX, textY, key, textColor, 1.0f);
        }
    }

    // Hint
    fb.text(kbX, kbY + 4 * (keyH + gap) + 20, "Use touch screen or D-pad + A to type. B to cancel.", 0xFF8A8A9A, 0.7f);
}

// ===== Settings Screen =====
bool drawSettingsScreen(Canvas& fb, u64 keysDown, u64 keysHeld) {
    // Load settings if not loaded
    static bool loaded = false;
    if (!loaded) {
        loadSettings(g_settings);
        loaded = true;
    }

    // Draw keyboard overlay if active
    if (g_kb.active) {
        drawKeyboard(fb);
        handleKeyboardInput(keysDown);
        return true;
    }

    // Draw popup if active
    if (g_popupTimer > 0) {
        g_popupTimer--;
        const int w = 500;
        const int h = 120;
        const int x = (fb.w - w) / 2;
        const int y = (fb.h - h) / 2;

        fb.fillRect(x, y, w, h, 0xFF1A1A25);
        fb.rect(x, y, w, h, g_popupSuccess ? 0xFF4ADE80 : 0xFFF87171);
        fb.text(x + 20, y + 30, g_popupText.c_str(), 0xFFE8E8ED, 1.1f);
        fb.text(x + 20, y + 70, g_popupSuccess ? "Sucesso!" : "Erro", g_popupSuccess ? 0xFF4ADE80 : 0xFFF87171, 0.9f);
    }

    // Background
    fb.fillRect(0, 0, fb.w, fb.h, 0xFF0D0D12);

    // Title
    fb.text(40, 30, "TrophyNX Sync - Configuracao", 0xFFFF6B35, 1.5f);
    fb.text(40, 60, "Configure o envio automatico de conquistas para o site", 0xFF8A8A9A, 0.9f);

    // Fields
    const int fieldX = 40;
    const int fieldY = 110;
    const int fieldW = 1100;
    const int fieldH = 80;
    const int gap = 16;

    const char* labels[4] = { "URL da API", "Token de Sync", "Usuario", "Ativado" };
    std::string values[4] = {
        g_settings.apiUrl,
        g_showPassword ? g_settings.token : std::string(g_settings.token.length(), '*'),
        g_settings.user,
        g_settings.enabled ? "SIM" : "NAO"
    };

    for (int i = 0; i < 4; ++i) {
        int y = fieldY + i * (fieldH + gap);
        bool selected = (g_selectedRow == i);

        // Field background
        fb.fillRect(fieldX, y, fieldW, fieldH, selected ? 0xFF1A1A2E : 0xFF16161F);
        fb.rect(fieldX, y, fieldW, fieldH, selected ? 0xFFFF6B35 : 0xFF2A2A3A);

        // Label
        fb.text(fieldX + 20, y + 10, labels[i], 0xFF8A8A9A, 0.8f);

        // Value
        uint32_t valueColor = (i == 1 && !g_showPassword) ? 0xFF8A8A9A : 0xFFE8E8ED;
        fb.text(fieldX + 20, y + 35, values[i].c_str(), valueColor, 1.1f);

        // Edit hint for text fields
        if (i < 3) {
            fb.text(fieldX + fieldW - 180, y + 35, "[A] Editar", 0xFFFF6B35, 0.8f);
        } else {
            fb.text(fieldX + fieldW - 180, y + 35, "[A] Toggle", 0xFFFF6B35, 0.8f);
        }

        // Show password toggle for token
        if (i == 1) {
            fb.text(fieldX + fieldW - 350, y + 35, g_showPassword ? "[X] Ocultar" : "[X] Mostrar", 0xFF8A8A9A, 0.8f);
        }
    }

    // Action buttons
    const int btnY = fieldY + 4 * (fieldH + gap) + 20;
    const int btnW = 250;
    const int btnH = 50;
    const int btnGap = 20;

    // Sync Now button
    bool syncSelected = (g_selectedRow == 4);
    fb.fillRect(fieldX, btnY, btnW, btnH, syncSelected ? 0xFFFF6B35 : 0xFF1A3A2A);
    fb.rect(fieldX, btnY, btnW, btnH, syncSelected ? 0xFFFF6B35 : 0xFF4ADE80);
    fb.text(fieldX + 60, btnY + 15, "Sincronizar Agora", syncSelected ? 0xFF0D0D12 : 0xFF4ADE80, 1.0f);

    // Back button
    bool backSelected = (g_selectedRow == 5);
    fb.fillRect(fieldX + btnW + btnGap, btnY, btnW, btnH, backSelected ? 0xFF3A1A1A : 0xFF1A1A2E);
    fb.rect(fieldX + btnW + btnGap, btnY, btnW, btnH, backSelected ? 0xFFF87171 : 0xFF2A2A3A);
    fb.text(fieldX + btnW + btnGap + 80, btnY + 15, "Voltar", backSelected ? 0xFFF87171 : 0xFF8A8A9A, 1.0f);

    // Status info
    if (isConfigured(g_settings)) {
        fb.text(fieldX, btnY + 70, "Configurado! Pressione A em 'Sincronizar Agora' para enviar.", 0xFF4ADE80, 0.9f);
    } else {
        fb.text(fieldX, btnY + 70, "Preencha todos os campos e ative para sincronizar.", 0xFFF87171, 0.9f);
    }

    // Help text
    fb.text(40, fb.h - 50, "D-pad: Navegar  |  A: Editar/Selecionar  |  X: Mostrar/Ocultar token  |  B: Voltar", 0xFF5A5A6A, 0.8f);

    return true;
}

bool handleSettingsInput(u64 keysDown, u64 keysHeld, SyncSettings& settings) {
    // Handle keyboard first
    if (g_kb.active) {
        handleKeyboardInput(keysDown);
        return true;
    }

    // Navigation
    if (keysDown & (KEY_UP | KEY_DUP)) {
        g_selectedRow = (g_selectedRow - 1 + 6) % 6;
    }
    if (keysDown & (KEY_DOWN | KEY_DDOWN)) {
        g_selectedRow = (g_selectedRow + 1) % 6;
    }

    // Actions
    if (keysDown & KEY_A) {
        switch (g_selectedRow) {
            case 0: // API URL
                openKeyboard(&settings.apiUrl, KeyboardState::URL, settings.apiUrl);
                break;
            case 1: // Token
                openKeyboard(&settings.token, KeyboardState::TOKEN, settings.token);
                break;
            case 2: // User
                openKeyboard(&settings.user, KeyboardState::USER, settings.user);
                break;
            case 3: // Enabled toggle
                settings.enabled = !settings.enabled;
                if (settings.enabled && !isConfigured(settings)) {
                    settings.enabled = false;
                    showPopup("Preencha todos os campos antes de ativar", false);
                } else {
                    saveSettings(settings);
                    showPopup(settings.enabled ? "Sync ativado!" : "Sync desativado", true);
                }
                break;
            case 4: // Sync Now
                if (isConfigured(settings)) {
                    showPopup("Sincronizando...", true);
                    auto result = trophynx::sync::doSync();
                    showPopup(result.message.c_str(), result.success);
                } else {
                    showPopup("Configure URL, Token e Usuario primeiro", false);
                }
                break;
            case 5: // Back
                settings = g_settings;  // Copy back
                return false;  // Exit settings screen
        }
    }

    // Toggle password visibility
    if (keysDown & KEY_X && g_selectedRow == 1) {
        g_showPassword = !g_showPassword;
    }

    // Exit on B
    if (keysDown & KEY_B) {
        settings = g_settings;
        return false;
    }

    return true;
}

trophynx::sync::SyncResult quickSync(const SyncSettings& settings) {
    if (!isConfigured(settings)) {
        trophynx::sync::SyncResult result;
        result.success = false;
        result.message = "Nao configurado";
        return result;
    }

    // Temporarily save current settings for sync.cpp to use
    SyncSettings temp = settings;
    temp.enabled = true;
    saveSettings(temp);

    // Call the main sync function
    return trophynx::sync::doSync();
}

} // namespace trophynx::sync_ui