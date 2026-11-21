# Multiplayer through Playit.gg

Playit.gg is the only supported tunnelling provider. No port forwarding, and no
other tunnel or VPN service is needed or used.

## 1. What I verified, and what still needs a live test

**Researched (Nov 2026)** from Playit's support pages and community guides:

| Question | Finding | Source |
|---|---|---|
| Tunnel protocols | TCP, UDP, and combined TCP+UDP tunnels exist. | playit.gg/support (setup guides list "Type: TCP/UDP") |
| Address format | A public `hostname:port` is assigned, e.g. `name.gl.at.ply.gg:51795`. The **public port usually differs from your local port.** | Playit unofficial UDP guide (discuss.playit.gg/t/109) |
| Free vs paid | Premium raises ports "from 4 to 16" and adds region choice and custom domains. Free tunnels use global anycast. | playit.gg/support/playit-premium |
| Free UDP availability | **Unclear.** Some 2025 reviews say free accounts lost UDP or are capped at 4 UDP ports; others report free UDP working. | Trustpilot reviews, Dec 2025 |
| Unreal Engine | No official guide. Other UE-based games (e.g. SCP:SL on UDP 7777) are hosted through Playit UDP tunnels. Multi-port games (Conan, Ark) have trouble. | discuss.playit.gg threads |
| Proxy protocol | Optional. It passes the real client IP to TCP servers. We don't need it. | playit.gg/support |

**Conclusion and design decision.** Unreal's IpNetDriver needs **one UDP port**
and nothing else, as long as you avoid Steam/EOS sessions, NAT punch-through,
beacons and query ports. The project is configured for exactly that:

- `DefaultPlatformService=Null`: no Steam, no EOS
- `GameNetDriver = IpNetDriver` on UDP **7777** (configurable)
- `MaxPortCountToTry=0`: if 7777 is busy, hosting fails with a clear error instead
  of silently moving to 7778, which would break the tunnel
- No beacons, no server-query port, no LAN broadcast. Everything (version check,
  full server, password, friends list) happens in `AMRGameMode::PreLogin` on the
  game port.
- Clients connect with `ClientTravel("host:port?...")` to the Playit address and
  public port. Hostnames are resolved off the game thread.

**Not yet tested live.** This dev machine has no Unreal Engine install and no
second PC, so these checks remain (see the test plan below):

1. UDP traffic flows through a Playit UDP tunnel to `127.0.0.1:7777`.
2. Three or more clients at once work. Unreal identifies clients by
   source `IP:port`; the Playit agent should give each remote client its own
   local source port. If it doesn't, only one client could connect, and the fix
   would be a small client-ID handshake in our own NetDriver subclass. This is
   documented as the fallback, not implemented.
3. Long idle periods don't drop the tunnel mapping. Unreal sends keep-alives
   several times a second, so this should be fine.

If the free tier turns out not to include UDP, the supported options are a
Playit Premium account, or LAN/direct IP play. TCP-only tunnels can't carry
Unreal's UDP netcode.

## 2. Hosting (host PC)

1. Start the game, then **Menu → Multiplayer → Host a session**. Set:
   - UDP port (default 7777), max players, who can join (Public / Private
     password / Friends list)
2. Choose **Start hosting**. The game reopens the world as a listen server on
   `UDP <port>`.
3. Install and run the **Playit.gg agent** (https://playit.gg/download), sign in
   and link the agent.
4. In the Playit dashboard: **Add Tunnel → type UDP → local address
   `127.0.0.1`, local port = your game port (7777)**.
5. Copy the tunnel's public address (e.g. `name.gl.at.ply.gg:51795`).
6. In the game: **Host a session → Playit public address → A/Enter to paste**.
   Then **Multiplayer → Copy invite address** puts it on your clipboard to send to
   friends. You can also use the console: `MRPlayit name.gl.at.ply.gg:51795`.

The game never hardcodes tunnel addresses. Everything lives in your profile
(`Saved/SaveGames/Profile.sav`) and can be changed at any time.

## 3. Joining (friends)

1. Copy the host's address to your clipboard.
2. **Multiplayer → Join by address → Address (A/Enter pastes) → Connect**.
   You can also use the console: `MRJoin name.gl.at.ply.gg:51795 [password]`.
3. Servers you've joined appear under **Server browser → Recent**. Press Right to
   favourite one.

## 4. Error messages

| Situation | Message the player sees |
|---|---|
| Typo or unknown host | "Could not find "…". Check the address for typos (Playit addresses look like name.gl.at.ply.gg:12345)." |
| Malformed address | "… is not a valid address. Use host:port …" |
| Server unavailable (no reply) | "Server not responding. Check: the address and port are right, the host is in a session, and the host's Playit.gg agent shows the UDP tunnel as online." |
| Connection timeout mid-session | "Lost contact with the host (timed out). Reconnecting..." (up to 3 automatic attempts) |
| Version mismatch | "Version mismatch: host protocol X, yours Y. Everyone needs the same game build." |
| Full server | "The session is full. Ask the host to raise the player limit, or try again later." |
| Wrong password | "Incorrect password for this private session." |
| Not on friends list | "This is a friends-only session and your player name is not on the host's friends list." |
| Host closed / connection lost | "Disconnected from the host. Reconnecting..." |
| Hosting port busy | "Could not open UDP port 7777 for hosting. Close anything else using it or pick another port (and update the Playit tunnel)." |

## 5. Limitations (honest list)

- **No public session discovery.** Playit has no lobby or master-server API, so
  the "server browser" lists your favourites and recent servers. A community
  session list (a JSON file at a URL) is a possible later addition (TODO.md).
- **Passwords are not encryption.** Unreal login options travel unencrypted.
  The password keeps strangers out of a casual session; it isn't account security.
- **Friends-only checks player names.** Without Steam/EOS there are no verified
  identities, so someone using a friend's name could get in.
- **Client-simulated movement.** Each player simulates their own bike, so riding
  stays smooth at Playit latencies. The server rejects impossible speeds and
  teleports, but it is not cheat-proof (ARCHITECTURE.md §4).
- **Dedicated server** builds need a source build of Unreal Engine. A listen
  server (one player hosts) works with the Epic Launcher engine.
- **Tested capacity:** none yet. Default max 8, hard ceiling 16 (`DefaultGame.ini`)
  until real sessions have been measured.

## 6. Test plan (run before calling multiplayer done)

1. **Local:** Editor → Play → Net Mode *Play As Listen Server*, 2–3 clients.
   Check movement smoothness, garage swaps, tricks scoring, competition timer.
2. **LAN/direct:** two PCs on one network: host, then `MRJoin 192.168.x.x:7777`.
3. **Playit, 1 client:** host behind Playit; a friend on mobile data joins via
   the tunnel address. Measure ping on the Tab player list.
4. **Playit, 3+ clients:** confirms per-client source ports (see §1 item 2).
5. **Failure paths:** wrong port, stopped agent, full server (set max 2), wrong
   password, version mismatch (bump `ProtocolVersion` on one build), host
   alt-F4 mid-ride (expect auto-reconnect attempts, then an error).

Record results in DEVELOPMENT_STATUS.md.
