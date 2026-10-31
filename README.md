# Game Presence Mirror

Shows the games you play on your **gaming PC** as your Discord activity on your **streaming PC**, where Discord is installed. Friends see the real "Playing <game>" card with the game's name, icon and an elapsed timer.

Windows only. Single static executable (`game_presence_mirror.exe`), nothing to install.

## How it works

```
 Gaming PC                                    Streaming PC
 ┌──────────────────────┐   TCP 47831 (LAN)   ┌────────────────────────────┐
 │ send  (chosen game)  │ ──────────────────▶ │ receive                    │
 │ auto  (running game) │  "SET <appId> ..."  │  - checks token            │
 │  - looks up app ID   │  every 1 s          │  - talks to Discord via    │
 │                      │                     │    \\.\pipe\discord-ipc-0  │
 └──────────────────────┘                     └─────────────┬──────────────┘
                                                            ▼
                                                  Discord desktop app
                                                  "Playing <game>"
```

Nothing is faked and no game files are copied. The streaming PC publishes a normal Rich Presence activity under the game's official Discord application ID.

There are two ways to choose what is shown:

- **`send`**: you save games with `pick`, choose one from a menu when you start `send`, and that game is shown for as long as `send` runs. Nothing is detected; the running processes are not looked at.
- **`auto`**: the program detects games by itself, as described in section 1 below.

### 1. The game list and how `auto` detects games (gaming PC)

The program does not decide what is "a game". It uses Discord's own list, which `pick` searches by name and `auto` matches against running process names:

1. It downloads Discord's public **detectable applications** list (about 25,000 games; only about 10,000 of them list a Windows exe), the same list Discord's own game detection uses:
   `https://discord.com/api/v9/applications/detectable` (no login or key needed, via Windows WinHTTP).
2. The list is cached as `detectable.json` next to the exe. It is re-downloaded when missing, older than 7 days, or when you run `update`.
3. Each entry looks like this (Dying Light 2 shown, trimmed):

   ```json
   {
     "id": "1124351684131442728",
     "name": "Dying Light 2: Stay Human",
     "executables": [
       { "name": "x64/dyinglightgame_x64_rwdi.exe", "os": "win32", "is_launcher": false }
     ],
     "icon_hash": "b265e59b13543037af8e0f553b621a68"
   }
   ```

   Only three fields are used:
   - `executables[].name` for `os: win32`: the exe name to match. A leading `>` and any folder prefix such as `x64/` are stripped, and the name is lowercased.
   - `id`: the Discord application ID.
   - `name`: the display name, used in `pick` and in log messages.
4. `pick <name>` searches the `name` and `aliases` fields of every entry. `auto` detects games in two ways (see "How `auto` detects games" below).

**Limits:** a game that is not in Discord's list cannot be found by `pick`. `auto` also can't detect Xbox / Microsoft Store games that have neither a listed exe nor a `StoreId` in their `MicrosoftGame.config`, games with no exe and no id at all (about 4,500, mostly mobile, console or defunct), or a game whose running exe name differs from the listed one. Use `pick` and `send` for those.

### How `auto` detects games

Discord's list identifies games in different ways, and about 14,600 of its 25,000 entries have **no `executables` at all**. All of `auto`'s knowledge comes from the list (exe names and folders, store ids, game names), so a new game or launcher title is supported just by running `update`; no code changes are needed. Each cycle (every second) it uses five methods, and the console shows which one found the game (`Playing: <game>  (detected via ...)`):

1. **Steam app ID (checked first).** Steam records the game it is running in the registry (`HKCU\Software\Valve\Steam\RunningAppID`, only trusted while `steam.exe` is running). `auto` looks that ID up in the `third_party_skus` entries with `"distributor": "steam"` in Discord's list. This detects the roughly 9,300 games that list no exe (for example WARDOGS, Steam ID 1867240) and tells apart games that share an exe name. Steam doesn't record a start time, so the Discord timer starts when `auto` first sees the game; start `auto` before the game for an accurate timer.
2. **Exe name plus folder.** For each running process, `auto` looks up its exe name, then checks the folder part of the listed name against the process's real path, the way Discord does. For example `x64/dyinglightgame_x64_rwdi.exe` matches only a process whose path ends in `\x64\dyinglightgame_x64_rwdi.exe`. If the game is also found through Steam, the exe's real start time is used for the timer. This covers games from any launcher (Epic, Ubisoft Connect, EA, Battle.net, GOG, standalone launchers such as Delta Force's or Escape from Tarkov's) that list an exe: the game is detected when its exe runs, not when the launcher does.
3. **Epic Games Launcher.** Epic keeps a manifest for every installed game in `%ProgramData%\Epic\EpicGamesLauncher\Data\Manifests\*.item`, with its install folder and catalog ID. Discord's Epic IDs are those same IDs (`third_party_skus` with `"distributor": "epic"`). `auto` reads the manifests (refreshed every 60 seconds) and reports the game when a process runs from its install folder. This detects the roughly 1,000 Epic games that list no exe, and uses the real process start time.
4. **Xbox app / Microsoft Store games.** Xbox (GDK) games, for example those installed under `C:\XboxGames`, ship a `MicrosoftGame.config` file in their install folder that can contain the game's Store ID (`<StoreId>9NRMZXGKP4NM</StoreId>`). Discord's Xbox IDs (`third_party_skus` with `"distributor": "xbox"`) are those same Store IDs. For each running process, `auto` looks for that file in the exe's folder and up to 6 parent folders (once per process, cached) and looks the ID up in the list. This detects Xbox-only games that list no exe, for example Delta Force from the Xbox app. Games whose config has no `StoreId`, and older UWP-style Store games that ship no such file, can't be identified this way (they are still detected if the list gives their exe name).
5. **Install-folder name (other launchers).** If a process runs from a known game-library folder (`steamapps\common`, `Epic Games`, `GOG Galaxy\Games`, `Ubisoft Game Launcher\games`, `EA Games`, `Origin Games`, `XboxGames`) and the game's folder name exactly matches the name of a game that lists no exe (ignoring case, spaces and punctuation, and requiring a unique match of at least 4 characters), that game is reported. This is a conservative fallback and also covers Steam games when Steam isn't running.

Some exe names are shared by many games (`game.exe` by about 190 games, `hl2.exe` by about 36). If an exe can't be narrowed down to one game, `auto` reports nothing for it and prints a one-time warning, instead of showing the wrong game. Steam detection still handles those games when launched from Steam.

**Known quirk (shared with Discord itself):** the list claims some generic exe names for a single game with no folder to check. For example `javaw.exe` belongs to Minecraft, so any Java program (such as Angry IP Scanner) running on the gaming PC is reported as Minecraft. Use the `detected via` text in the console to see why a game was reported, and stop `auto` (or use `send`) while such a program runs.

**Updating the list:** run `game_presence_mirror.exe update` on the gaming PC. A running `auto` notices the new `detectable.json` within 30 seconds and prints `Game list updated`; no restart or new exe is needed.

### 2. How it reaches the streaming PC

While there is a game to show, the sender connects over TCP and sends one line, repeated every second (`auto` scans running processes every second to decide this; `send` always has a game). A game starting, closing or switching therefore reaches the streaming PC within about a second:

```
GPR2 <token> SET <applicationId> <unixStartTime> <game name>
GPR2 <token> CLEAR
```

- The start time is the real creation time of the game process in `auto`, so the Discord timer is accurate. In `send` it is the time `send` started, minus any `--hours` / `--minutes` / `--seconds` you give.
- The receiver replies `OK` or `NO`. Reports with a wrong token are rejected.
- When the game closes (`auto`) or the sender is stopped with Ctrl+C, the sender sends `CLEAR`.

### 3. How Discord displays it (streaming PC)

The Discord desktop app opens a local named pipe `\\.\pipe\discord-ipc-0` (up to `-9`). Messages are a binary header (int32 opcode, int32 length) followed by JSON.

1. **Handshake** (opcode 0): `{"v":1,"client_id":"<applicationId>"}`. Discord replies `READY`.
2. **Set activity** (opcode 1):
   ```json
   {"cmd":"SET_ACTIVITY","args":{"pid":1234,
     "activity":{"instance":false,"timestamps":{"start":1730000000}}},"nonce":"1"}
   ```
3. Discord looks the application ID up on its servers and shows "Playing <that app's name>" with its icon and the elapsed timer. The program never sends a name or icon itself.

Details:
- `pid` is the receiver's own process ID (the game's PID does not exist on this PC). Discord drops the activity if that process dies.
- Switching to a different game means reconnecting with the new application ID, since the ID is fixed per connection.
- The receiver does **not** forward every one-second report to Discord. Discord counts the elapsed time itself from the start timestamp (so the timer ticks every second without any updates), and it rate-limits presence updates to roughly 5 per 20 seconds. The receiver sends to Discord only when the game or start time changes, plus a refresh every 5 seconds (which also recovers from a Discord restart).
- On `CLEAR`, or if no report arrives for 20 seconds, the receiver clears the activity and closes the pipe.
- Connection problems are announced once (`Receiver did not accept the report...` / `Could not reach Discord...`) and again as `Receiver reachable again.` when they clear, instead of every second.
- If the pipe dies (Discord restarted), the receiver reconnects once on the next report.

## Commands

Run `game_presence_mirror.exe` with no arguments to print a short usage summary. Commands are run from a terminal (cmd or PowerShell) in the folder containing the exe. There are seven forms:

| Command | Run on | What it does |
|---|---|---|
| `receive --token <secret>` | Streaming PC | Listens on TCP 47831 and publishes reports to the local Discord app. |
| `pick <game name>` | Gaming PC | Searches Discord's game list by name and adds the game you choose to your picked list (`picked.txt`). |
| `pick --id <application id> <name>` | Gaming PC | Adds a custom game to the picked list, using an application you created in the Discord developer portal. |
| `send <receiver IPv4> --token <secret>` | Gaming PC | Asks which picked game to show, then shows it on Discord continuously, whether or not it is running on the gaming PC. |
| `auto <receiver IPv4> --token <secret>` | Gaming PC | Detects any game from Discord's list that is actually running and reports it; stops when it closes. |
| `list` | Gaming PC | Prints all picked games. |
| `update` | Gaming PC | Force re-downloads Discord's game list. |

### `receive --token <secret>`

Run on the **streaming PC**, the one where the Discord desktop app is installed.

```
game_presence_mirror.exe receive --token mysecret
```

- Discord must be open and signed in on this PC.
- Allow it through Windows Firewall (private network), or add an inbound TCP rule for port 47831.
- Output: `Listening on TCP port 47831...`, then `Now showing on Discord: <game>` when a game starts, and `Game closed; clearing Discord presence.` when it ends.
- Errors: `Could not reach Discord...` means the Discord app isn't running or isn't signed in. `Rejected a report with a wrong token.` means the token differs from the sender's.
- Runs until you close the window (Ctrl+C).

### `pick <game name>`

Run on the **gaming PC**. Searches Discord's game list by name; the game does not need to be running.

```
game_presence_mirror.exe pick dying light
```

```
6 match(es) for "dying light":
  1) Dying Light  [dyinglightgame.exe]
  2) Dying Light 2: Stay Human  [dyinglightgame_x64_rwdi.exe]
  3) Dying Light 2: Stay Human - Reloaded Edition
  ...
Enter the number of the game to show on Discord, or blank to cancel:
```

- Search matches the typed words against each game's name and aliases, ignoring case, spaces and punctuation. Word order doesn't matter, so `light dying` also works. Quotes aren't needed.
- Variations of a game (editions, sequels, DLC) are separate entries; pick the exact one you want, since its name and icon are what friends see.
- Enter one number, or press Enter to cancel. The game is added to `picked.txt` next to the exe; earlier picks are kept. Picking a game that is already saved just moves it to the top of the `send` menu. Only one game can be shown at a time.
- All matches are shown, with no limit, so a short search can print thousands of lines. Add more words to narrow it.
- If nothing matches, Discord doesn't list the game; use `pick --id` below.

### `pick --id <application id> <name>`

Run on the **gaming PC**. For a game Discord doesn't list, use your own Discord application.

```
game_presence_mirror.exe pick --id 123456789012345678 My Indie Game
```

1. Open the Discord Developer Portal (discord.com/developers/applications) and click **New Application**.
2. Name it after the game. **That name is what friends see** as "Playing <name>". Optionally upload an icon on the application's page.
3. Copy the **Application ID** (digits) and run the command above.

- The ID is required: Discord needs it to know which application's name and icon to show. Games from Discord's list get it automatically through `pick <game name>`.
- `<name>` is only used in this program's own messages and `list`; Discord shows the name set in the portal. Quotes aren't needed.
- It is added to `picked.txt` like any other pick; `send` then offers it in its menu. `auto` can't detect custom games.
- Custom applications show the name and icon you gave them, with no store page or official game artwork.

### `send <receiver IPv4> --token <secret>`

Run on the **gaming PC**. Asks which of your picked games to show, then shows it on Discord.

```
game_presence_mirror.exe send 10.2.0.12 --token mysecret
```

```
Which game do you want to show on Discord?
  1) Dying Light 2: Stay Human
  2) My Indie Game
  3) Dying Light
Enter a number (blank to cancel):
```

- `<receiver IPv4>` is the streaming PC's numeric IPv4 address (find it with `ipconfig` on that PC). Host names are not accepted.
- The menu lists every game saved by `pick`, most recently picked first. Enter a number to start, or press Enter to cancel. If only one game is saved, it starts without asking.
- It does **not** check whether the game is running on the gaming PC. As long as `send` is running, Discord shows the chosen game, with the elapsed time counted from when `send` started, unless you set a starting point (below).
- **Time already played:** `--hours <n>`, `--minutes <n>` and/or `--seconds <n>` start the Discord timer that long ago, for when you've been playing a while before running `send`. They can be combined and placed anywhere on the command line:

  ```
  game_presence_mirror.exe send 10.2.0.12 --token mysecret --hours 2 --minutes 30
  game_presence_mirror.exe send 10.2.0.12 --token mysecret --minutes 45
  game_presence_mirror.exe send 10.2.0.12 --token mysecret --hours 1 --minutes 2 --seconds 3
  ```

  The first shows 2h 30m elapsed immediately, then keeps counting; the last starts at 1h 2m 3s. Whole numbers only; the total can be at most 1000 hours. Minutes and seconds can exceed 59 (`--minutes 90` is 1h 30m, `--seconds 90` is 1m 30s). They only work with `send` (`auto` already uses the game's real start time). The timer is fixed when `send` starts, so to change it, restart `send`.
- To switch games, stop `send` (Ctrl+C) and run it again to get the menu.
- Stopping with Ctrl+C or closing the window clears the Discord activity right away. If the program is killed instead, the receiver clears it after 20 seconds.
- Output: `Playing: <game>` on the first successful report.
- Errors: `No games picked yet...` means run `pick` first. `Receiver did not accept the report...` means a wrong IP, wrong token, firewall block, or the receiver isn't running; it retries automatically.

### `auto <receiver IPv4> --token <secret>`

Run on the **gaming PC**. Auto-detect mode: no need to pick games first.

```
game_presence_mirror.exe auto 10.2.0.12 --token mysecret
```

- Every second it checks Steam's running game and all running processes against Discord's game list (see "How `auto` detects games") and reports the first match.
- When the game closes it stops reporting and clears the Discord activity. When a game (the same or another) starts again, reporting resumes automatically.
- Executables Discord marks as launchers (for example game launchers or stores) are ignored, so they aren't shown as games.
- If several known games run at once, it keeps reporting the one it already had until it closes, then switches to the next.
- Output: `Playing: <game>` when a game is detected, `Stopped playing: <exe>` when it closes. Errors are the same as `send`.
- Ctrl+C or closing the window clears the Discord activity.
- It does not use `picked.txt`. Use `auto` when you want whatever is running to be shown, and `send` when you want one specific game shown.
- Caveat: it trusts Discord's list, so any non-game program the list contains will also be reported when it runs.

### `list`

Prints every game saved by `pick` with its application ID, most recent first, in the same order as the `send` menu.

```
game_presence_mirror.exe list
```

### `update`

Force re-downloads Discord's game list and prints how many known games were loaded. The list is otherwise refreshed automatically when missing or older than 7 days. A running `auto` picks up the new list within 30 seconds, so keeping game support current only ever needs this command.

```
game_presence_mirror.exe update
```

### Options and limits

- `--token <secret>` is used by `receive`, `send` and `auto`. It is any password you invent (no spaces, up to 64 characters) and must be identical on both PCs. It can be placed anywhere on the command line.
- `--hours <n>`, `--minutes <n>` and `--seconds <n>` are used by `send` only (see above).
- There are no other flags. These are fixed in the source ([src/main.cpp](src/main.cpp)): port 47831, report and scan interval 1 second, receiver-to-Discord refresh 5 seconds, receiver expiry 20 seconds.
- There is no command to remove a picked game: delete its line from `picked.txt` by hand.
- Games not in Discord's list can only be shown through `send`, using `pick --id` (not detected by `auto`).
- Only one game is shown at a time. In `auto`, if two known games run at once, only one is reported.
- No background or tray mode; both sides run in a console window.

### picked.txt

Written by `pick` and read by `send` and `list`. Next to the exe, one game per line as `applicationId|Game name`, oldest first. To remove a game, delete its line.

## Quick start

Show one specific game:

1. Gaming PC: run `game_presence_mirror.exe pick <game name>` and choose the game (repeat for any other games you want in your list).
2. Streaming PC: run `game_presence_mirror.exe receive --token mysecret` (allow inbound TCP 47831 in Windows Firewall on your private network).
3. Gaming PC: run `game_presence_mirror.exe send <streaming-PC-IPv4> --token mysecret` and choose a game from the menu.

Or show whatever game is running, automatically: do step 2, then on the gaming PC run `game_presence_mirror.exe auto <streaming-PC-IPv4> --token mysecret`.

Expected output: the sender prints `Playing: <game>`; the receiver prints `Now showing on Discord: <game>`.

## Notes

- The Discord activity privacy setting must allow sharing your activity on the streaming PC's account.
- Close Discord on the gaming PC (if installed) to avoid two activities.
- Traffic is plain TCP guarded only by the token; use it on a trusted LAN.
- Status: confirmed working between a real gaming PC and streaming PC (Discord showed the game with its name, icon and timer).

## Build from source

The result is **one self-contained exe with no dependencies**. Everything it needs is linked in, and the only DLLs it loads are ones every Windows 10/11 install already has (`kernel32`, `advapi32`, `ws2_32`, `winhttp`, `msvcrt`). The gaming and streaming PCs need no compiler, no runtime and no extra DLLs: copy the exe and run it. The compiler below is only needed on the PC where you build.

### 1. Install a compiler (once, on the build PC)

Use MSYS2's MinGW-w64 GCC (this is what the exe was built and tested with; GCC 15.2, anything with C++17 works):

1. Install MSYS2 from msys2.org (default location `C:\msys64`).
2. Open the **MSYS2 MINGW64** shell from the Start menu and run:

   ```
   pacman -S mingw-w64-x86_64-gcc
   ```

3. Add `C:\msys64\mingw64\bin` to your Windows `PATH` so `g++` works in PowerShell or cmd. Check with `g++ --version`.

### 2. Compile

From the project folder (the one containing `src\main.cpp`), in PowerShell or cmd:

```powershell
g++ -std=c++17 -municode -static -static-libgcc -static-libstdc++ -O2 -Wall -Wextra src/main.cpp -o game_presence_mirror.exe -lws2_32 -lwinhttp -ladvapi32
```

What each part does (**keep the static flags**, they are what make the exe dependency-free):

| Flag | Purpose |
|---|---|
| `-std=c++17` | The source uses C++17 (`std::filesystem`, structured bindings). |
| `-municode` | The program starts at `wmain` (Unicode command line). Without it you get `undefined reference to wWinMain`. |
| `-static -static-libgcc -static-libstdc++` | Link the C++ runtime into the exe, so no `libstdc++-6.dll`, `libgcc_s_seh-1.dll` or `libwinpthread-1.dll` is needed at runtime. |
| `-O2` | Optimise. |
| `-Wall -Wextra` | Show warnings. The build should print none. |
| `-lws2_32 -lwinhttp -ladvapi32` | Windows system libraries: sockets, downloading Discord's list, and the Steam registry lookup. |

The build takes a few seconds and produces `game_presence_mirror.exe` (about 3.5 MB) in the project folder.

### 3. Check it has no extra dependencies (optional)

```powershell
objdump -p game_presence_mirror.exe | findstr /C:"DLL Name:"
```

You should see only `ADVAPI32.dll`, `KERNEL32.dll`, `msvcrt.dll`, `WINHTTP.dll` and `WS2_32.dll`. If you see `libstdc++-6.dll`, `libgcc_s_seh-1.dll` or `libwinpthread-1.dll`, the static flags were missed and the exe will not run on a PC without MinGW.

### 4. Deploy

Copy `game_presence_mirror.exe` to both PCs (any folder). `picked.txt` and `detectable.json` are created next to it when needed. After editing the source, rebuild and copy the new exe to both PCs: the sender and receiver should be the same version. If the exe's timestamp is older than `src\main.cpp`, it is stale.

### Troubleshooting

| Problem | Fix |
|---|---|
| `g++` is not recognised | `C:\msys64\mingw64\bin` is not on `PATH` (step 1.3), or open the MSYS2 MINGW64 shell instead. |
| `undefined reference to wWinMain` | Add `-municode`. |
| `cannot find -lwinhttp` or `-ladvapi32` | You are using an old or non-MinGW-w64 toolchain; install the MSYS2 one above. |
| Exe says a `.dll` is missing on the other PC | The `-static` flags were left out; rebuild with the command above and check with step 3. |
| `error: ... filesystem` or other C++17 errors | The compiler is too old; update it (`pacman -Syu`). |

### Alternative: CMake

[CMakeLists.txt](CMakeLists.txt) builds the same exe. With MinGW it applies the same flags (`-municode -static -static-libgcc -static-libstdc++`) and links `ws2_32`, `winhttp` and `advapi32`:

```powershell
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

The exe is then `build\game_presence_mirror.exe`. The CMake route has not been tested here (CMake isn't installed on the development PC); the direct `g++` command above is the verified one. Source is a single file: [src/main.cpp](src/main.cpp).

## Project layout

- `src/main.cpp`: everything (JSON parser, Discord list loader, process scanning, sender, receiver, Discord pipe client, CLI).
- `CMakeLists.txt`: build definition.
- `picked.txt` (the game `send` shows), `detectable.json` (Discord's game list cache): created next to the exe at runtime.

## Changelog

Add a line here for each change you make.

- **build instructions:** added step-by-step compile instructions (MSYS2 GCC), an explanation of each flag, a dependency check, deployment notes and troubleshooting. Fixed `CMakeLists.txt` to link statically (`-municode -static ...`) so a CMake build is also dependency-free.
- **per-second reporting:** `send` and `auto` now report to the streaming PC every second (and `auto` scans every second) instead of every 5 / 2 seconds, so starting, closing or switching a game shows up within about a second. The receiver forwards to Discord only on a change plus a 5-second refresh, because Discord rate-limits presence updates and computes the elapsed timer itself from the start time. Connection errors are now printed once per outage rather than repeatedly.
- **launcher support in auto:** after checking which launchers Discord's list covers, `auto` also detects Epic Games Launcher games (from Epic's install manifests, matched to Epic ids in the list) and, as a conservative fallback, games running from a known game-library folder whose folder name exactly matches a game that lists no exe. `auto` reloads the game list when `detectable.json` changes (after `update`), shows how each game was detected, and warns once about ambiguous exe names. Xbox / Microsoft Store games are detected through the Store ID in their `MicrosoftGame.config` when the game ships one.
- **auto detection fixes:** `auto` missed games such as WARDOGS because about 14,600 of Discord's entries list no exe. It now also detects games through Steam's running app ID (matched to the Steam IDs in Discord's list). Exe matching now checks the listed folder against the process's real path instead of the bare exe name, and when an exe name is shared by several games (`game.exe`, `hl2.exe`...) it reports nothing and warns once, instead of showing whichever game was listed first.
- **search limit removed, played time:** `pick` now shows every match instead of the first 40. `send` accepts `--hours`, `--minutes` and `--seconds` to start the Discord timer that long ago.
- **picked list + send menu:** `pick` (and `pick --id`) now add to a saved list instead of replacing one game. `send` shows a numbered menu of the list (newest first) and starts the one you choose. Removed the live re-read of `picked.txt`; to switch games, restart `send`. `list` shows all picks.
- **`pick --id`:** `pick --id <application id> <name>` picks a custom game from an application created in the Discord developer portal, so games Discord doesn't list can be shown by `send`.
- **removed games.txt and `add`:** `auto` now uses only Discord's game list, and `send` only the picked game. `list` shows just the picked game.
- **searchable pick / pinned send:** `pick <game name>` now searches Discord's game list by name and aliases and saves one game to `picked.txt`. `send` now shows that picked game continuously without checking whether it is running; detection moved to `auto`. Ctrl+C / closing the window clears the Discord activity. `list` shows the picked game and `games.txt`.
- **auto command:** `auto <ip> --token <secret>` reports any running exe found in Discord's list (launchers excluded) and stops/starts reporting as the game closes/reopens.
- **v2 rewrite:** replaced the old process/executable mirroring with Discord Rich Presence over IPC, using Discord's detectable-games list. Added `pick`, `add`, `list`, `update`, token authentication, and automatic presence expiry.
