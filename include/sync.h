// Send your achievements to the TrophyNX Hub site (see docs/site-sync.md).
#pragma once
#include <string>
#include "achievements.h"

// Reads sdmc:/switch/TrophyNX/sync.json  {"url": "https://.../api/sync", "token": "...", "user": "..."}
// and POSTs every game that has unlocked achievements (names, points, unlock times, play time, icon).
// Blocks until the server answers (connection timeout 15 s). Returns true on success;
// `msg` is a short line for the toast on screen.
bool syncToSite(const Library& lib, std::string& msg);
