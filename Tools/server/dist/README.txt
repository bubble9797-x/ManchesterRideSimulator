MANCHESTER RIDE SERVER  (Debian / Ubuntu / any 64-bit Linux)
=============================================================

The game's Multiplayer screen connects to this server. Players make an account the first time,
and the game remembers their login after that.

SET UP (once)
-------------
1. Copy manchesterride-server-linux.tar.gz to the Debian PC, then in a terminal:
       tar -xzf manchesterride-server-linux.tar.gz
       cd manchesterride-server
       ./start.sh
   You'll see "Done! Type "help" for commands." The server is running and this terminal is its console.

2. Playit.gg: run the playit agent on this same PC, with a UDP tunnel pointing at local port 9010
   (127.0.0.1:9010). Your tunnel's public address (209.25.141.16:38420) is built into the game.
   Proxy Protocol: leave it OFF (none). The server doesn't need it. If you turn it on anyway, the
   server notices and strips it, and only the address shown in the console changes.

3. Start the game, choose Multiplayer, create your account, then make yourself an operator by
   typing this in the server console:
       op YourName

KEEP IT RUNNING
---------------
Run it inside "screen" so it keeps going after you close the terminal:
       sudo apt install screen
       screen -S manchesterride ./start.sh
   Leave it running: press Ctrl+A, then D.  Get the console back: screen -r manchesterride
To start it automatically when the PC boots, see manchesterride.service (instructions inside).

CONSOLE COMMANDS  (in-game, operators type the same with a / in chat, e.g. /kick)
------------------------------------------------------------------------------
help                          every command
list                          who's online
say <message>                 announce to everyone
msg <player> <message>        private message (players can use /msg and /r too)
kick <player> [reason]        remove someone
ban <player> [reason]         ban an account          pardon <player>   unban
op <player> / deop <player>   give / take operator (admin) rights
whitelist on|off|add|remove|list
tp <player> <to player>       teleport someone        tp <player> <lat> <lon>
weather clear|cloudy|overcast|rain|storm|fog
time set 18:30                (or: time day / noon / night / midnight)
motd <message>                the welcome message
accounts list | delete <name> | password <name> <new password> | logout <name>
status                        players, pings, traffic, update on offer
update                        offer a new game version to players (below)
reload                        re-read server.properties and the lists
stop                          save and shut down

GAME UPDATES
------------
1. Build the new installer on your PC (Tools\installer\make_installer.bat), so you have a new
   ManchesterRide-Setup.exe.
2. Copy it into this server's "updates" folder.
3. Type:  update
From then on, anyone joining with an older game version gets the new one straight from this
server (through the Playit tunnel). The game downloads it, checks it, installs it and restarts.
Players online at the time get told to rejoin. "update status" shows what's on offer.

FILES
-----
server.properties     settings (port, max players, registration, voice, update speed...)
accounts.json         accounts. Passwords are stored only as salted hashes.
ops.json, banned-players.json, whitelist.json
server-identity.key   the server's identity. Keep it private, and keep it if you move the server,
                      otherwise players' games warn that the server's key has changed.
logs/latest.log       the console log
