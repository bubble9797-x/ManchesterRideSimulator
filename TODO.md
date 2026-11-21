# TODO

## Next session (blocking everything else)

1. ~~Install UE 5.5~~ (5.5.4, session 3).
2. ~~Build and fix compile errors~~: clean build with `Tools/build_editor.bat` (session 3).
3. Play in editor (Entry map). Check that the county streams around Piccadilly
   Gardens with hills, that roads sit on the ground, and that buildings block you.
   Ride up to Saddleworth and the Pennine edges; watch the log for "below the
   ground" warnings from `KeepAboveGround()` (there should be none). Then switch
   World → test district from the menu. If buildings or roads render inside-out,
   set `bFlipWinding=True` in DefaultGame.ini.
   Profile cell build time (8 m ground lattice + crest pass + buildings).
   If hitches appear, lower `LoadRadiusCm` or move more collision cooking async.
4. Tune feel with a controller: steering curve, camera, wheelie assist targets.
5. PIE multiplayer test (Listen Server + 2 clients), then the Playit test plan
   in docs/PLAYIT_NETWORKING.md §6. Record results in DEVELOPMENT_STATUS.md.

## Milestone 2: physics polish
- [ ] Per-wheel suspension pitch DOF (body pitch from front/rear spring difference;
      currently only visual compression differs). Rigid-rear bikes over-weight the
      rear spring visually.
- [ ] Rolling backwards / walking the bike when stopped against a wall.
- [ ] Engine stall when the clutch is dumped at idle with auto clutch off.
- [ ] Rider foot-down animation at very low speed.

## Milestone 3: vehicles
- [x] Detailed procedural vehicle meshes from each model's own geometry (`MRVehicleMeshBuilder`):
      frames, spoked/cast wheels, knobbly tyres, drivetrain, forks, shocks, engines, exhausts.
- [x] Real brand/model names for all 415 vehicles (ids unchanged; trademark notice in Credits).
- [ ] Per-model shape cues beyond type (e.g. a Stumpjumper's SWAT down tube); real decals/logos are NOT
      planned without licences.
- [x] Rider as a modelled person: per-segment meshes posed with IK to bars, pedals, pegs and deck;
      pedalling with turning cranks; every character-editor option modelled (`MRRiderBuilder`).
- [x] Rider weight shifts (acceleration, braking, wheelies, bumps), cornering style, foot down when
      stopped, standing off-road and in the air, crash ragdoll.
- [x] Rider surface detail: subsurface skin, cloth weaves, leather, denim, hair, helmet paint, creases
      and seams (`M_MRRider` / `M_MRSkin`).
- [ ] Watch weight shifts and cornering while riding; ragdoll collision with the bike and walls;
      check first person on screen.
- [x] Map waypoints with a blue road route on the full map and minimap (`MRRouter`, `make_routes.mjs`).
- [x] CC0 photo textures on ground and walls (ambientCG); photoreal map shows the right place.
- [ ] Photo textures for roofs, window frames and bark; decals (drain covers, road patches, litter).
- [ ] Photoreal: hybrid (our streets close up, Google scan for the distant skyline) or a smoother ride surface.
- [x] Kerbs and raised pavements; teleport to waypoint; routes keep to streets.
- [ ] Ride over kerbs at speed, check kerbs at cell borders, test teleport in multiplayer.
- [ ] Ride a waypoint route end to end (rerouting, arrival); turn-by-turn arrows in the world.
- [ ] Per-brand liveries/decals; garage search box and favourites for the 429-model list.
- [ ] More customisation: wheels, tyres (changes `ETyre`), suspension setup,
      gearing. All free.

## Milestone 4: Greater Manchester map
- [x] Whole-county import (Geofabrik PBF → `pbf_to_tiles.mjs`), 21,136 cells; all motorways present.
- [x] Terrain (Terrarium/SRTM, 16 m) + draped roads + bridge decks + watertight ground.
- [ ] Higher-res terrain: EA LIDAR Composite DTM 1–2 m (OGL) to replace the ~30 m source (sharper embankments, cuttings).
- [ ] Tunnels and portals (currently skipped: riders go over the top); embankments/cuttings for rail.
- [ ] Multipolygon inner rings (courtyards): dropped by `pbf_to_tiles.mjs` for now.
- [ ] Compress cell files (314 MB of JSON → binary/zlib) before shipping.
- [ ] Editor importer: bake cells into a World Partition map (256 m grid) with
      HLOD layers; Nanite for building kits.
- [x] Centre/edge lines, zebra crossings with Belisha beacons. Still to do: roundabout markings, bus lanes,
      cycle-lane markings; UK signage from `maxspeed`, `ref`, `name`.
- [x] Shader façades: windows per storey, brick/stone/glass/industrial, lit windows at night.
- [ ] Gabled/hipped roofs for terraces and semis (roof:shape); real texture sets (Megascans via Fab).
- [x] Street furniture from OSM (lamps + lit roads, signals, bus shelters, benches, post boxes, K6 boxes,
      cycle stands) and trees in woods/parks. Still to do: OSM individual trees/tree rows (needs a pipeline re-run).
- [x] Optional Google Photorealistic 3D Tiles layer (`AMRPhotoLayer`, docs/PHOTOREAL_MAP.md). Still to do: verify on screen      with a real token; tune height alignment on hills; (was: needs the Cesium plugin from Fab
      and a Google Map Tiles API key with billing, which the player sets up themselves).
- [ ] Landmarks: Beetham Tower, Town Hall, Central Library, Old Trafford, Etihad,
      MediaCityUK, the Lowry, Trafford Centre, Stockport Viaduct, town halls of each
      borough (flagged by the pipeline already).
- [x] Minimap drawn from road data. Still to do: full-screen map.

## Milestone 5: environment
- [ ] AI traffic on the road graph (left-hand), using ZoneGraph or a custom lane
      graph from OSM; pooled vehicles; traffic lights from `highway=traffic_signals`.
- [ ] Pedestrians (Mass crowd) on footways.
- [ ] Rain/spray Niagara effects; wet material layer and puddles; wind.
- [ ] Recorded audio to replace or blend with the synth (engines, tyres, ambience).

## Milestone 6: multiplayer hardening
- [ ] Live Playit results (see the test plan). If the agent maps all clients to one
      source port, add a client-ID handshake NetDriver subclass.
- [ ] Optional community session list (JSON at a URL) for "public sessions".
- [ ] Text chat, emotes/horn/bell, group ride waypoint.
- [ ] Measure bandwidth and CPU with 8 and 16 riders; raise the ceiling if stable.

## Milestone 8: release
- [ ] Performance capture on the target PC (RTX 3060 / 5600G / 16 GB) at each preset.
- [ ] Shipping package; installation guide; first-run controller detection.
