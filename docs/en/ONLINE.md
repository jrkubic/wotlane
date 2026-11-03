# Playing online (wotlane)

Two to eight friends, each on their own PC and screen, play one War of the Monsters fight over the internet. Each PC runs the whole game
in deterministic mode and only the controller inputs travel (lockstep through a small server, `wotm_server`). Each friend owns the disc
and builds the game from their own ISO: **never share `wotm.exe`, `recomp/`, `disc/` or the ISO**. The server program has no game code
and can be shared freely.

## 1. Everyone: build the same game

1. Install what `tools\setup_runtime.ps1` needs: git, Python 3, CMake 3.24+, and Visual Studio 2022 Build Tools with the C++ workload
   **plus the component "MSVC v143 - VS 2022 C++ x64/x86 build tools (v14.44-17.14)"** (Visual Studio Installer, Modify, Individual
   components). Online play is tested with 14.44, so the setup insists on it.
2. Clone the fork on the same commit as your friends:
   `git clone -b wotlane/4p https://github.com/sightama/wotlane.git` (agree on the commit: `git log -1 --format=%h`).
3. Put your own ISO in `ISO\` and run `powershell -ExecutionPolicy Bypass -File tools\setup_runtime.ps1` (about 10 minutes the first
   time). Later updates: `git pull`, then `build_runtime.bat`.
4. Compare build ids in your chat: `python tools\build_id.py`. Everyone must print the same `wl-...` id. The server refuses players with
   a different build (different commit or runtime patch), because the simulation would drift apart.

## 2. The host: run the server

1. Build it once: `tools\build_net.bat` (it also runs the network tests). The program is `native\net\build\wotm_server.exe`.
2. Start it, one of:
   - **From the launcher (player as host):** section **Online**, press **Host on this PC**. The launcher runs the server hidden (it
     writes `wotm_server.log` next to the launcher and stops when you close the launcher, so keep the launcher open while you play),
     hosts a lobby on it and shows a box to copy to your friends: the addresses they can type as Server and the lobby code. It also
     asks your router to open the port (UPnP, TCP and UDP; closed again when the launcher closes). If the router refuses, the box says
     so and which local address to forward the port to. To list it publicly, type the **Master server** address and tick **List
     publicly as** (with a name) before pressing **Host on this PC** (see "Public server list" below).
   - **Standalone:** run `native\net\build\wotm_server.exe --port 7777` and leave its window open while you play; close it to stop.
   Players create the lobbies themselves from the launcher (up to 16 at a time, `--lobbies N` to change; `--players 2..8` per lobby, 8 by default).
3. The first time, Windows Firewall asks about `wotm_server.exe`: allow it on private networks (and public if your network is marked
   public).
4. If UPnP did not open the port, let your friends reach it, one of:
   - **Port forwarding (no extra software):** find your PC's local address with `ipconfig` ("IPv4 Address", for example 192.168.1.50).
     In your router's admin page (usually http://192.168.1.1 or http://192.168.0.1), add a port forwarding rule: external port **7777**,
     internal IP **your PC's IPv4 address**, internal port **7777**, protocol **TCP and UDP** (or "Both"). Friends then connect to your
     public IP (search "what is my ip"). If your provider uses CGNAT (your router's WAN address differs from the public IP), forwarding
     will not work: use the next option.
   - **Free virtual LAN (Tailscale or ZeroTier):** everyone installs the same one and joins your network; friends use your virtual
     address (for example 100.x.y.z with Tailscale). No router changes.
   - **Dedicated server:** any always-on PC can run `wotm_server` the same way (the source builds with CMake on Linux too). Friends use
     that machine's address.

### Public server list (master server)

A master server keeps the list of public servers, so players can find them without being told an address.

- **Run the master** (one always-on machine): `wotm_server --master` listens on TCP port **7778** (`--port N` to change). Forward
  that port (TCP only) the same way as above. It needs no game files.
- **List a server publicly:** start it with `--announce <master address>[:7778] --name <name>` (the name: letters, digits, `-` and
  `_`, up to 16). Every 10 s it sends the master its port and its counts (open lobbies, how many wait for players, players); the
  master lists it under the address the announce came from (behind a router, the public one) and drops it 30 s after the last
  announce. An unreachable master never holds up the server's matches.
- **See the list:** `python tools/net_lobby.py --browse <master address>` prints one row per server: name, address, lobbies (and how
  many are waiting), players and ping. `unreachable` means the server announced but nothing answers on its port from here (often a
  router that doesn't forward it); `needs another version` means it speaks another protocol. Join one with
  `--server <address> --list`, then `--code`. In the launcher: section 3.
- **From the launcher:** **List publicly as** (with a name) next to **Host on this PC** starts the hosted server with `--announce`
  to the **Master server** field.

## 3. Everyone: join and play

1. Open the launcher (`tools\launcher\build.bat` builds `WotM_Launcher.exe` in the repo root).
2. In **Match**, pick **Your monster**. The host also picks the **Stage**.
3. In **Online**, set Server = `<server address>:7777` and type your name. To find a public server instead, type the **Master
   server** address and press **Browse public servers**: one row per server with its name, address, lobbies (waiting), players and
   ping from your PC (`unreachable`: its port doesn't answer from here; `needs another version`: another protocol). Picking a row
   sets Server to it and lists its lobbies. Then:
   - **To host a lobby:** type a **Lobby name** (and a **Password** if you want only friends to get in) and press **Host lobby**.
     The server gives the lobby a 4-letter **code** (it appears in the Code box and under the buttons): tell it to your friends.
     You are the lobby's host: you pick the stage and the **Modifiers** (Super Jump, Fast Monsters, Turbo; Match group) and the
     **Wins to end** (0 = endless) and press **Start match**. Joiners' Modifiers boxes are greyed out in a lobby: everyone plays the
     host's.
   - **To join:** type the code and press **Join by code**, or press **Refresh list**, pick a lobby and press **Join selected**
     (double-click works too). Lobbies marked `[password]` need the host's password in the Password box.
   The line under the buttons lists who is in the lobby, the host, the stage, the wins and the modifiers. If the host leaves, the next
   player becomes the host (and the lobby takes the new host's settings). An empty lobby closes.
4. When everyone is in, the host presses **Start match**. The game opens on every PC, loads the stage and waits until everyone has
   arrived; the server measures the ping and picks the input delay. The launcher minimizes and stays in the lobby.
5. In the game: your controller (or keyboard) drives your monster; the screen shows only your view. Top right: ping and input delay in
   frames. A "Waiting for <name>" message means someone's input is late (everyone pauses together, nobody desyncs).
6. With a wins target, the first to reach it gets the winner's screen: **PLAY AGAIN** (same stage, everyone respawns), **REBUILD LEVEL**
   (the stage is rebuilt) or **QUIT** (and YES): the game closes on every PC and the launcher comes back, still in the same lobby, so the
   host can change the stage or wins and start again. QUIT from the pause menu does the same.

Command line alternative to the launcher (`tools\net_lobby.py`, prints the environment for `wotm.exe` when the match starts):
`--create <lobby> --name Ana --monster 3 [--password <pw>] [--stage N] [--wins N] [--mods jump,fast,turbo] --start-with <players>` hosts a lobby (the code goes
to the console) and starts once that many players are in; `--code <code> --name Bia --monster 1 [--password <pw>]` joins one;
`--list` shows the open lobbies.

## What is different online

- Gameplay modifiers (Super Jump, Fast Monsters, Turbo) are the lobby's: the host sets them (`SET mods=<list>` in the lobby
  protocol, the **Modifiers** boxes in the launcher, `--mods` in `net_lobby.py`), every PC gets them in `MATCH` (`WOTM_MODIFIERS`), and the MODIFIERS tab shows them locked. A PC
  with a different list is refused at join. Turbo doubles the fields per frame during the fight only: menus keep their speed.

- Endless free-for-all with Wins to end 0: knocked-out monsters get back up, no end-of-match screen; QUIT from the pause menu (or closing the window) leaves. With a wins
  target (the host's **Wins to end**) the first to reach it ends the match with the game's PLAY AGAIN / REBUILD LEVEL /
  QUIT screen; any player's controller moves the cursor and confirms, and every PC follows the same choice.
- Pausing works for everyone: whoever presses Start pauses every PC, and the pause menu moves the same everywhere. RECOMPILED OPTIONS
  does nothing online.
- Everyone plays with the game's default options (difficulty, volumes, controller layout): local saves are neither read nor written
  online, because they live in game memory and would make the PCs drift apart.
- Gameplay settings are fixed online: classic camera (no modern orbit camera, no mouse camera), the game's own lock-on (no lock-on
  toggle), close-up camera on, default sensitivity. Your own settings are kept and come back offline. Video settings (resolution,
  render scale, interpolation, enemy bars, damage numbers, button prompts) stay yours. Mouse clicks in menus, photo mode and switching
  targets with the keyboard are off online (they come from your PC only and would make the PCs drift apart).
- Mods: everyone needs the same active mods (same files, same contents). Otherwise the server refuses the join with
  "configuracao do jogo diferente".
- Debug keys that change the game or pause it are off: F1 (cheats), F5 (free camera), F6/F7/F8 (pause, step, photo).
- Input delay: 2 frames on a LAN, about 4 to 6 over the internet (each frame is 16.7 ms; 33 ms with 3 or more players, where the game
  runs its own 30 Hz mode). It is fixed for the whole match.
- **5 to 8 players:** the game itself only has 4 player slots and 4 cameras. Players 5 to 8 drive monsters in the computer-player slots
  (same controls and moves as players 1 to 4), and their screen uses a camera made by wotlane that follows from behind and above, since
  the game has no camera for them. It does not avoid buildings like the game's camera does, which is why it stays high. Every PC
  simulates all 8 monsters, so 8 players need PCs that run the game comfortably (on Lane's Ryzen 5 5600X a frame with 8 monsters uses
  about half of its time budget).
- 12 players are not possible: the game has exactly 8 controller inputs, the level loader has room for 12 monster files and no more,
  and the per-pair monster checks grow with the square of the count.
- If a player leaves, their monster stands still for the rest of the match (same frame on every PC).
- If the server sees two PCs with different game state ("desync"), the match stops on every PC with a message. Please report it with
  everyone's `runtime\live.err.log`.

## Troubleshooting

- "different build": someone is on another commit. Compare `python tools\build_id.py`; `git pull` and rebuild.
- "configuracao do jogo diferente": someone launched with different game options (enemy bars, cheats, `WOTM_RATEMIN`, wins target)
  or a different set of mods in `mods\`. Use the same launcher checkboxes in the Mods group and copy the same mod folders (folders whose
  name starts with `_` are off and do not count).
- "sem resposta do servidor": the server is not running, the port is not forwarded (TCP and UDP), or a firewall blocks it.
- Test everything on one PC: `powershell -File tools\net_check.ps1` starts a server and two games and checks that both stay identical;
  `powershell -File tools\net_checks.ps1` runs the online checks (per-PC settings, mouse, photo mode, pause, wins target, mods).
