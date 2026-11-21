# Photoreal map (Google 3D tiles)

An optional layer that replaces the game's own map visuals with Google Photorealistic
3D Tiles of Greater Manchester, streamed through Cesium ion. Riding happens on the
photoreal surface. The game's own ground stays 60 cm underneath as a safety net, and the
minimap and GPS keep working.

## Requirements

1. **Cesium for Unreal 2.27.0 for UE 5.5** in `Plugins/CesiumForUnreal`. It isn't in git
   because it's 1.4 GB.
   - Download `CesiumForUnreal-55-v2.27.0.zip` from
     https://github.com/CesiumGS/cesium-unreal/releases/tag/v2.27.0.
   - Unpack it so that `Plugins/CesiumForUnreal/CesiumForUnreal.uplugin` exists.
   - Rebuild with `Tools\build_editor.bat`.

   The build detects the plugin and sets `WITH_MR_CESIUM=1`. Without the plugin, the game
   builds and runs as normal, and Settings says "needs the Cesium plugin".
2. **Your own Cesium ion token.** Each player makes their own; nothing is shared or
   hard-coded.
   1. Create a free account at https://ion.cesium.com.
   2. In **Asset Depot**, add **Google Photorealistic 3D Tiles** (asset 2275207) to
      **My Assets**.
   3. In **Access Tokens**, create a token, or use the default one, and copy it.
   4. In the game, open **Settings > Cesium ion token (A: paste)** and press A/Enter. The
      token is read from the clipboard; don't use Ctrl+V. It's stored in your local
      profile, and the row changes to "set (...xxxx)". If the clipboard doesn't hold a
      token, the row says so.
   5. Turn on **Settings > Photoreal map (Google 3D tiles)**.

The Cesium ion free tier is for personal, non-commercial use, and Google's terms apply
to the tiles. Cesium shows the required data credits on screen. You need to be online,
because the tiles stream and aren't cached for offline play.

## How alignment works

The game's map is a flat transverse-Mercator plane centred on the Town Hall. The
tiles sit on the curved WGS84 globe, so `AMRPhotoLayer` lines the two up in four steps:

1. It pins the globe to the game map at an anchor under the rider. The anchor's
   latitude and longitude come from the game's projection, and its height is the
   terrain height plus about 49.6 m of geoid.
2. It rotates the globe by the grid convergence at the anchor.
3. It measures the real tile surface at five points around the anchor and takes the
   median as a height correction. This ignores a parked car on one sample.
4. It re-anchors every 1.2 km, so the curvature and projection error stays below about
   15 cm where you're riding.

The log shows `Photoreal map: height offset N cm` while it settles.

## Multiplayer: one map for everyone

The photoreal map is a **session setting**. The host (or a solo player) turns it on, and
everyone who joins gets the same map automatically, so all riders ride the same surface.
Only the host needs a token:
- The host's token is sent to the friends who join, and their games use it to stream the
  tiles. Everyone's streaming counts against the host's Cesium ion allowance.
- Friends can see the token is in use, so only host for people you trust with it. You can
  always regenerate it on ion.cesium.com.
- On a friend's screen, Settings shows "Photoreal map (set by host)". They can't change it.

Every player's copy of the game needs the Cesium plugin installed, either in the same
build or in a packaged build that includes it.

## Limits

- Each machine streams its own copy of the same tiles. Small differences in level of
  detail can make a rider look a few centimetres off the scanned ground on someone else's
  screen.
- The scans include parked cars, street clutter and trees as solid lumps, and bridges
  and overpasses can be rough. Tunnels aren't modelled.
- Street lamps, night windows and markings come from the game's own map, which is
  hidden in this mode. Night therefore relies on the sky light, moonlight and
  headlights.
