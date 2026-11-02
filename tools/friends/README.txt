wotlane: War of the Monsters on PC, online with friends
========================================================

This package is the recompiled PC version of War of the Monsters (PS2) from the wotlane project. It does not contain the game's disc
files: you need your own copy of the game as an ISO of the USA disc (SCUS-97197). Please keep this package private; it is for friends
who own the game.

Needs: Windows 10 or 11 (64-bit), a CPU from roughly 2015 or newer (AVX2), a controller or the keyboard.


1. Setup (once)
---------------
1. Unzip this folder somewhere you can write to, for example your Desktop or Documents (not Program Files).
2. Put your War of the Monsters (USA) .iso in the ISO folder. You can also leave it where it is: Setup asks for it.
3. Double-click Setup.bat. It reads one file (SCUS_971.97) from your ISO and remembers where the ISO is. Do not move or rename the
   ISO afterwards (if you do, run Setup.bat again).
4. Windows may warn that WotM_Launcher.exe is from an unknown publisher: press "More info", then "Run anyway".


2. Join a friend's game
-----------------------
1. Open WotM_Launcher.exe.
2. In the Match group, pick "Your monster".
3. In the Online group:
   - Server: the address the host gave you, for example 203.0.113.7:7777 or 100.101.2.3:7777.
   - Your name: any short name (letters, digits, _ or -).
   - Code: the 4-letter lobby code from the host, then press "Join by code".
     (Or press "Refresh list", pick the lobby and press "Join selected".)
   - Password: only if the host set one.
4. Wait for the host to press "Start match". The game opens by itself and the launcher minimizes. When the match is over (QUIT in the
   game) the launcher comes back, still in the lobby.
   Do not press "Play": that is the offline game on this PC, and it closes the launcher (you leave the lobby). While you are in a
   lobby the launcher asks before doing it.


3. Host a game
--------------
1. Open WotM_Launcher.exe, pick your monster and the Stage in the Match group, type your name in the Online group.
2. Press "Host on this PC". A box appears with the lobby code and the addresses your friends can use. Copy it to your friends.
   Keep the launcher open while you play: closing it stops the server.
3. If Windows asks whether wotm_server may use the network, press Allow (tick both private and public networks). Under the box the
   launcher says what Windows Firewall thinks of wotm_server on your network; if it says friends get no answer, press
   "Allow in Windows Firewall" and accept the Windows permission prompt.
4. The launcher asks your router to open the port (UPnP). If the box says the router refused:
   - forward port 7777, TCP and UDP, on your router to the local address the box shows, or
   - everyone installs Tailscale (free, tailscale.com), joins the host's network, and uses the host's 100.x.y.z address.
5. Pick "Wins to end" (0 = endless) and press "Start match" when everyone is in. Not "Play": that starts the offline game and
   closes the launcher, which stops the server and the lobby for everyone (the launcher asks first).


Problems
--------
- "different build": someone has a different package. Everyone needs the same one (see BUILD.txt).
- "game configuration differs from the others": someone changed the mods folder or launcher checkboxes. Use the package as
  it came.
- "no answer from the server": wrong address, the host's port is not open, or a firewall blocks it.
  Host: read the Windows Firewall line under the box (and press "Allow in Windows Firewall" if it asks you to). If it is fine and
  the box says the router opened the port, compare the Internet address in the box with what "what is my ip" shows in a browser:
  a different address means a second router or the provider's shared address (CGNAT); use Tailscale then.
- The game opened on its own main menu instead of the match: someone pressed "Play" (offline) instead of "Start match". Close the
  game, open the launcher again and host or join the lobby again.
- Anything else: send the host runtime\live.err.log (the host also has wotm_server.log next to the launcher).
