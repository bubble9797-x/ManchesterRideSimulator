# Dedicated server

The game's **Multiplayer** screen joins one always-on server (`DedicatedServer` in `Config/DefaultGame.ini`,
default `209.25.141.16:38420`, a Playit.gg UDP tunnel to port 9010 on the host's Debian PC).

The old ways (host a listen session / join by address, Unreal replication) are still under
*Multiplayer > Other ways to play*.

## Why it isn't an Unreal dedicated server

The Epic Launcher build of UE 5.5 can't build `Server` targets, and has no Linux toolchain. So the
server is a small standalone program in Go (`Server/`), built for Linux x64/ARM64 and Windows by
`Tools/server/build_server.bat`. It relays ride states (each game still simulates its own rider, like
the listen-server design), and adds accounts, chat, voice and updates.

## Pieces

| Where | What |
|---|---|
| `Server/*.go` | the server: protocol, accounts, console commands, update serving |
| `Source/ManchesterRide/Net/MRServerLink.*` | the game's side of the link (plain C++, Winsock + Windows CNG) |
| `Source/ManchesterRide/Net/MRServerClient.*` | game subsystem: login, saved login (DPAPI), rider puppets, chat, voice, update download |
| `Source/ManchesterRide/Net/MRVersion.h` | `MR_GAME_BUILD` / `MR_GAME_VERSION`, stamped into the installer |
| `Tools/server/linktest.cpp` | test bot: the game's link code outside Unreal (`build_linktest.bat`) |
| `Tools/installer/Setup.cs` | `/update` mode: waits for the game to exit, updates in place, restarts with `-JoinServer` |

## Protocol (UDP, one port)

`Server/protocol.go` is the reference. In short:

- **Handshake.** ECDH P-256 with the server's long-term key plus fresh ephemeral keys, then HKDF-SHA256,
  then AES-256-GCM for every packet after that, with a replay window. The game pins the server key on
  first connect (`ServerKeys` in the profile) and warns if it changes.
- **Reliability.** A small reliable/ordered layer (cumulative acks, resends) carries logins, chat and
  player info. Ride states (35 bytes, 30 Hz), voice and update chunks go unreliable.
- **Accounts.** PBKDF2-SHA256 (120k iterations) password hashes. "Stay logged in" is a random token,
  stored hashed on the server and DPAPI-encrypted in `Saved/Server/login.bin` on the PC.
- **Voice.** The engine's Opus voice codec at 16 kHz mono. The server relays it; each game plays it
  2D ("everyone") or attached to the rider ("nearby", 150 m).
- **Updates.** The installer carries a version block (`MRVER001`, build, version) before the
  `MRPAYLD2` trailer. A game with a lower build gets `LoginNeedUpdate`, pulls 1 KB chunks with a
  sliding window, checks the SHA-256, and runs `Setup.exe /update`.

## Releasing an update

1. Bump `MR_GAME_BUILD` (and `MR_GAME_VERSION`) in `MRVersion.h`.
2. `Tools\package_game.bat`, then `Tools\installer\make_installer.bat`.
3. Copy `Saved\Installer\ManchesterRide-Setup.exe` into the server's `updates/` folder and type `update`.

Bump `ProtoVersion` in both `protocol.go` and `MRServerLink.h` only when the wire format changes:
older games are then refused at the handshake with a "network protocol" message.

## Testing locally

`-Server=127.0.0.1:9011` points the game at a local server.
`-ServerLogin=name:password [-ServerRegister]` logs in at start.
`-UserDir=<folder>` keeps the test's profile and saved login away from the real one.
