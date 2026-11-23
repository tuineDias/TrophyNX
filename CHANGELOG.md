# Changelog

Each version has its own section. The release workflow copies it into the GitHub release, and the
in-app updater (ZL) shows its first line and its bullets before you install.

How to write a section (see `tools/release/changelog.py`):
- `## X.Y.Z` heading (the APP_VERSION in `include/version.h`), optional date after it.
- First line: one sentence for players, shown as the update's title in the app (max ~100 characters).
- Then `### New`, `### Fixed`, `### Changed`, `### Live module`, `### Known issues`... with `- ` bullets
  written for players: what changed for them, and what they need to do, if anything.
- A release without a section for its version fails in CI.

## 0.11.4 (2026-11-23)

Fixes the console shutting down (error 2001-0132) when a game or sphaira starts.

### Fixed
- On consoles with several background modules, the console could shut down with error 2001-0132 right after booting, or when starting a game, sphaira or TrophyNX. All background modules share one block of system memory, and TrophyNX kept 4 MB of it reserved the whole time. It now keeps 2 MB, and only takes more for a moment when there is room to spare.

### Live module 0.8.6
- The health check now really reads the shared system resources (it always said "resource limit unreadable"), and logs them at every game start and close in `live_log.txt`.
- Heap sized on demand: 2 MB while idle, 4 MB only while a popup is shown or a save is read, and only if at least 6 MB stay free for the other modules. Without room, popups and save checks run in the 2 MB heap.
- Safe mode no longer turns on for threads (the module never creates any after boot), and the scan on close checks resources again right before it runs.
- Tip: if the console still crashes, turn off background modules you don't use (Ultrahand), e.g. emuiibo or sys-clk.

## 0.11.3

Turning the live module off now really keeps it off, even when you open TrophyNX.

### Fixed
- The app started the live module every time it opened, even after you turned it off (Ultrahand, or by renaming/removing `flags/boot2.flag`). Now, without `boot2.flag`, the app never starts it, and an update installs the new module without starting it.

## 0.11.2

Safer live module, a beginner guide for game files, and THEATRHYTHM's password found again.

### New
- Beginner guide for the two games that need a file from your own copy (THEATRHYTHM, DELTARUNE): `docs/dumping-guide.md`.
- The update screen (ZL) now lists what each update changes, like these notes.

### Fixed
- THEATRHYTHM: the app now finds `global-metadata.dat` where nxdumptool really saves it (`sdmc:/nxdumptool/...`, up to 16 folders deep).

### Live module 0.8.2
- Ignores homebrew started like a game (Homebrew Menu, forwarders, launchers such as homeNX, title takeover with R): no live mode, capture or scan on close for them.
- The raw NAND save probes are off. Create an empty `sdmc:/switch/TrophyNX/probe.flag` to turn them back on.
- Fixed a display layer that could stay open if a popup failed to start.
- New health check: `health:` lines in `live_log.txt` show how much of the shared system resources background modules use. Near a limit, the module switches to safe mode (toasts instead of popups, no save reading) until it recovers.

## 0.11.1 (2026-11-21)

THEATRHYTHM FINAL BAR LINE: 19 of the game's own feats added as achievements.

### New
- THEATRHYTHM: 19 feats (first summon, ProfiCard edit, 24/100 hours, 200/500 stages, a song 100 times, 300 Perfect Chains, 32 characters Lv99, 4 characters Star Level 9, 5/20 Series Quests, 1,500 chain in Endless World, every airship and moogle style, 9,800,000+ scores).

### Live module 0.8.1
- Also reads THEATRHYTHM high scores on close, within its memory limit (a save too big is skipped, never a crash).

## 0.11.0 (2026-11-21)

THEATRHYTHM popups when you close the game, and its save password found from the game's own file.

### New
- THEATRHYTHM: popups about 3 seconds after closing the game, like the other save-based games.
- No `keys/theatrhythm.txt`? The app finds the password in the game's `global-metadata.dat` (in `keys/` or nxdumptool's folder) and writes the key file. The Save Lab can do it too.
- THEATRHYTHM FINAL BAR LINE: save reader and 32 of the 42 PS4 trophies (needs the save password, see above).

### Live module 0.8.0
- Reads only the parts of THEATRHYTHM's save that the achievements need (~2.3 MB instead of ~11 MB).

## 0.10.2 (2026-11-21)

DELTARUNE: the 30 PS4/PS5 trophies.

### New
- DELTARUNE: 30 trophies read from `deltarune.sav`. Some unlock on an unpatched save; all of them need the trophy patch in `tools/deltarune/` (you patch your own dump on a PC).

## 0.10.1 (2026-11-20)

The in-app update now really replaces the app and restarts the live module.

### Fixed
- Updating with ZL downloaded everything but left the old app and module running. The app now restarts itself after an update, and the module is restarted once its old process is gone.
- New `update_log.txt`: what happened to each file and to the module.

### Live module 0.7.1
- At boot, installs an app update an older version left behind as `TrophyNX.nro.new`. Coming from 0.10.0: update, then reboot once.

## 0.10.0 (2026-11-20)

Save captures shared over Wi-Fi, Hades achievements and save-based achievements for 7 more games.

### New
- Save captures: ZR on a game, A to record. Each time the game closes, its save is copied; Y shares the captures to a phone over Wi-Fi (Save Lab at `http://<console IP>:8080`).
- Hades: 41 achievements from the save (38 Steam + 3 escape milestones).
- Save-based achievements: DAVE THE DIVER (56), Blasphemous (45), Slay the Spire (46), Vampire Survivors (243), Vampire Crawlers (161), OCTOPATH TRAVELER (22), Paper Mario: The Origami King (22).
- Play-time achievements for Hades and DELTARUNE.

### Live module 0.7.0
- Copies the save of recorded games when they close (keeps the last 20 per game).

## Older versions

See the [GitHub releases](https://github.com/Imp3r1us/TrophyNX/releases) (0.9.1 and before have no detailed notes).
