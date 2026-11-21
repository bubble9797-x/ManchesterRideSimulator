# Development status

_Last updated: 2026-11-12 (session 3)_

## Read this first

- **The game now compiles and runs in Unreal Engine 5.5.4** (session 3). Build with
  `Tools/build_editor.bat`. It maps the project to drive `M:` because the project
  folder's path pushes MSVC past 260 characters, and it pins MSVC 14.44 because UE
  5.5 picks up the unsupported VS 2026 compiler otherwise. The build has 0 errors
  and 0 warnings.
- **Headless run** (`UnrealEditor-Cmd -game -nullrhi`, 2.5 minutes): the game boots
  to MRGameMode on the Entry map and loads the manifest (21,136 cells) and the
  terrain. It builds the 3×3 cells under the spawn synchronously, about 20 ms
  each, then streams the rest (27 cells, 294 components). It holds a steady
  60 fps with no "below the ground" warnings and no errors from our code.
  Cell finishing is now time-budgeted (8 ms per frame) instead of "2 per frame",
  which caused ~45 ms frames while streaming.
  **Correction:** that run's player was actually at the map origin, not
  Piccadilly (see the spawn bug below), so its ground check only covered the
  origin.
- **Checked with rendering (session 3, editor PIE and standalone `-game`):**
  - The first launch compiles shaders for about 3 minutes.
  - The main menu shows, and Ride (free roam) starts a ride.
  - The rider and bike render and pedal up to 15 mph. The chase camera follows,
    buildings stream in, and lighting and the day cycle work. Faces render the
    right way round (`bFlipWinding` is correct).
  - At Piccadilly the grass, paths, tram lines and buildings line up with the
    real layout, and the GPS readout says 53.48099 N 2.23697 W. Screenshots:
    `docs/screenshot_piccadilly_day.png`, `docs/screenshot_piccadilly_night.png`,
    `docs/screenshot_vehicles.png`.
- **Rider: a real person instead of blocks** (`Vehicles/MRRiderBuilder.cpp`), checked on screen
  on an eMTB, an aero road bike, a superbike, a 450 MX and a dual-motor scooter:
  - Built from adult body proportions scaled by the character editor's height and build: shaped
    torso, shoulders, neck, arms, hands, thighs, shins and feet. The face has eyes, nose, lips,
    ears and eyebrows.
  - One mesh per body segment, posed every frame with two-bone IK:
    - Hands close round the grips and follow the steering.
    - Feet go on the pedals, footpegs or scooter deck; scooter riders stand with one foot ahead.
    - The torso leans from the hips to reach the bars, so riders crouch on road bikes and
      superbikes and sit up on commuters.
    - The head turns into corners.
  - **Pedalling:** cranks and pedals are now separate parts. They turn with the rear wheel
    through the gearing (40-130 rpm) while you pedal, settle level when you freewheel, and the
    legs follow them round the whole stroke.
  - Every character-editor option is modelled:
    - hair: short, curly, long, ponytail, braids, afro, mohawk
    - stubble and beard
    - five helmets: open-face with visor, full-face with face port and visor, motocross with
      peak and goggles, skate, and aero road with vents and glasses
    - six tops: jacket with collar, zip and armour; hoodie with hood, pocket and drawstrings;
      jersey with band and rear pockets; leathers with hump, stripes, knee and elbow sliders;
      hi-vis vest with reflective bands; T-shirt with bare arms
    - bags: backpack, courier bag and hydration pack
    - trainers on bicycles and scooters; buckled boots and gauntlets on motorbikes
  - In first person the head is hidden for the local player, and the camera sits at eye height.
  - Test-only launch options: `-RiderLook=<hex look code>` and `-CrankTest` (cranks turn on the
    spot). Screenshot: `docs/screenshot_riders.png`.
  - **Body movement** (`UpdateRiderPose` / `FMRRiderRagdoll`), for local and remote riders:
    - Weight shifts: leans forward under acceleration and in wheelies, slides back on the seat
      when braking and in stoppies, follows the lean-back / lean-forward input.
    - Bumps and landings: a damped spring between rider and bike (about 1.6 Hz), driven by the
      bike's vertical acceleration. Knees and elbows bend on landings, and the body folds
      forward a little.
    - Cornering: road motorbike riders hang off (hips to the inside, body further into the turn,
      inside knee out). Dirt bike, MTB, downhill and BMX riders stay upright and push the bike
      down. The head counter-rolls so the eyes stay nearer level.
    - Foot down when stopped (under 0.6 m/s): the left foot, or a scooter's back foot, swings to
      the ground and the bike leans onto it. Cyclists slide off the saddle if the ground is too
      far to reach on tiptoe.
    - Standing in the attack position: dirt bikes on gravel, grass, mud, dirt or trail surfaces
      and in the air; MTB, downhill and BMX when coasting off-road or in the air (BMX also on
      tarmac); other bikes over jumps. The stance travels over the network as a new OffRoad
      flag.
    - Crash ragdoll: the rider is thrown off at the bike's speed with a forward tumble, then
      falls, bounces and slides to a stop on the real ground (22 Verlet point masses with
      distance links and rough joint limits, ground found by line traces). They're back on the
      bike at respawn.
    - Checked on screen: foot down (superbike, road bike), standing (450 MX, MTB) and a 30 km/h
      crash. Weight shifts, cornering and the bump spring are coded but haven't been watched
      while riding, because screenshots are taken standing still.
    - Test-only launch options: `-StandTest` (standing pose) and `-CrashTest=<seconds>`
      (thrown off at 30 km/h, repeating). Screenshot: `docs/screenshot_rider_moves.png`.
  - **Surface detail** (`M_MRRider` and `M_MRSkin`, built by `Tools/make_materials.bat`). There
    are no image textures: the patterns are computed in the shader from each body segment's own
    space, so they move with the body:
    - Skin uses subsurface lighting, with pores, slight blotchiness and smoother lips.
    - Each outfit gets its own cloth: cotton knit on hoodies and T-shirts, a polyester pinhole mesh
      on jerseys, gloves and hi-vis, and Cordura weave on riding jackets, textile moto trousers
      and bags. Leathers, boots and gauntlets get pebbled leather grain, and trousers get twill.
      Denim fades on the thighs.
    - Hair has strands and clumps, stubble shows as dots, trainers have an engineered mesh,
      soles are rubber, and helmet paint has orange peel, metal flake and light scuffs.
    - Fabric creases are soft folds that gather at the elbows, knees and waist, and are
      lighter on tight leathers and lycra. Seams with stitching run under the limbs and
      down the sides of the torso.
    - Detail smaller than about two pixels fades out, so it doesn't shimmer at a distance.
      Without the new assets, the rider falls back to plain colours.
    - Checked on screen in close-up and at normal distance (hoodie and jeans, race leathers,
      jersey, riding jacket and beard). Test-only launch option: `-ShowcaseClose` (close-up
      showcase camera). Screenshot: `docs/screenshot_rider_textures.png`.
  - Not done yet: first person hasn't been checked on screen, the ragdoll doesn't collide with the bike or walls (only the ground), and there is
    no getting-up animation.
- **Landmarks (checked on screen from real viewpoints with `-ViewCam=lat,lon,heightM,yaw,pitch`):**
  - **Old Trafford** and the **Etihad** are built on the real pitches, measured
    from the OSM pitch polygons (105 x 68 m at the real centre and bearing).
    Both have two-tier bowls, cladding, a cantilever roof with trusses, a mown
    and lined pitch, red seats at United and sky-blue at City, and the Etihad's
    12 cable masts. The OSM stand blocks on both sites are replaced.
  - **On their real footprints:**
    - Beetham Tower: 169 m, with the cantilever above the 23rd floor and the
      roof blade.
    - Manchester Town Hall: sandstone, with an 85 m clock tower on the Albert
      Square front, clock faces lit at night, pinnacles and a lead spire.
    - Central Library: rotunda, colonnade, entablature, dome and lantern.
    - Manchester Cathedral: gabled nave and a battlemented west tower.
    - CIS Tower: dark photovoltaic slab and its 118 m service tower.
    - John Rylands Library: red sandstone with corner turrets.
    - The Lowry and Imperial War Museum North: metal cladding, the Air Shard.
    - Trafford Centre: cream stone with a copper-green central dome and four
      smaller domes.
    - Borough town halls: stone, each with a clock tower.
  - **Bridges:** brick piers and a deck under raised railways, and concrete
    piers under road bridges, wherever the deck is more than 4.5 m up. Not yet
    confirmed on screen: the Stockport Viaduct viewpoint missed it.
  - Note: with the Cesium plugin installed, each launch logs a harmless
    `LogClass: Error ... FCesiumMetadataPropertyStatisticValue` from the plugin
    itself. Removing `Plugins/CesiumForUnreal` silences it.
- **Photo textures (checked on screen at street level and from rooftops):**
  - 13 CC0 textures from ambientCG (list in `Tools/textures_src/CREDITS.txt`, credited in game).
    Only the colour, normal and roughness maps are kept, 192 MB in total.
    `Tools/import_textures.py` imports them, and `make_materials.bat` runs it first.
  - Ground (`M_MRGround` Tex):
    - worn tarmac on main roads and cycle lanes, cracked tarmac on side streets
    - concrete flags with 60 cm joints on pavements and urban ground
    - granite setts, grass, woodland floor, dirt and gravel
    - water stays procedural
    - each surface is sampled twice, at two scales and rotations, to hide repeats
  - Walls (`M_MRFacade` Wall): red brick, sandstone (render on pale buildings), concrete and
    corrugated steel, with photo bump maps. Windows and roofs are still procedural.
  - Each surface keeps its own colour: the photo adds detail relative to its average colour,
    so cycle lanes stay red and every building keeps its palette shade.
  - The shared engine sampler (`View.MaterialTextureBilinearWrapedSampler`) lets one material
    use 24 textures.
  - The bark texture is downloaded but not used yet.
- **Photoreal map fixed and checked:**
  - The tileset now uses our georeference explicitly. Before, it picked up a default one
    centred on Denver, so the first test showed Colorado mountains.
  - Detail is raised (screen-space error 6, cache 1 GB). The height calibration settles within
    a few seconds (the log shows the fix converging).
  - Checked on screen: from rooftops it is real Manchester (Town Hall in its renovation
    wrapping, Central Library, the skyline). At riding height the scan is melted and lumpy,
    the physics mesh is coarse, and the front wheel sinks into lumps. It's best for looking
    around, not for riding.
  - Test-only launch option: `-PhotoMap` turns it on with the saved token.
  - **Hybrid (checked on screen):** in photoreal mode our photo-textured ground and road markings
    now stay visible and you ride on them. Our buildings, trees and street furniture are hidden
    but keep their collision, and the scan is sunk 25 cm so our ground covers its melted ground.
    The scan no longer blocks wheels or the camera. In open places (Piccadilly Gardens) this
    looks good: smooth ground, real buildings behind. In narrow streets the scanned buildings
    are still blurry and melted within about 20 m, and scanned street clutter pokes through
    the ground. Walls are solid where our footprints are, which may not match the scan exactly.
- **Waypoints and sat-nav (checked on screen):**
  - On the full map (M), left-click (or Enter / gamepad X on the crosshair) sets a waypoint;
    right-click, Backspace or gamepad Y clears it. Console: `MRWaypoint <lat> <lon>`.
  - The route follows the real road network (`Content/GeoData/routes.bin`, built by
    `Tools/geo_pipeline/make_routes.mjs`): 352,458 junctions and 406,134 road pieces. 97.4% of
    the network is one connected piece, and waypoints snap only to that piece. It covers roads,
    cycleways, tracks and shared paths, but not motorways, footways or steps, and obeys one-way
    streets except on cycleways and paths. A* on a worker thread takes 3-15 ms across the city.
  - A blue line with a white edge runs from where you are to the pin, on the full map and the
    minimap. The full map shows the distance by road and a time at 20 km/h. Under the minimap
    is the distance left, and "You have arrived" shows within 25 m.
  - The route is recalculated when you are more than 40 m off it, at most every 3 s.
  - Checked: Piccadilly Gardens to the Etihad gives 3.1 km by road, drawn on both maps.
    Rerouting and arriving haven't been ridden yet.
  - Test-only launch option: `-Waypoint=<lat>,<lon>`.
- **Session 3, latest pass (checked on screen):**
  - **Full map (M):** all of Greater Manchester from `Content/GeoData/overview.json`,
    built by `Tools/geo_pipeline/make_overview.mjs` (5.4 MB). It holds 127,865
    lines (motorways in blue, A/B roads in orange, streets, rail, Metrolink,
    rivers, canals), 566 place names and 45,824 street-name labels.
    - Zoom runs from 0.4 m to 200 m per pixel. You can pan, and Space recentres
      on you.
    - Building outlines show close up, and other riders appear on the map.
    - Riding input pauses while the map is open.
    - Checked at four zoom levels with `-MapZoom=`.
  - **Reverse (S):** S still brakes while rolling. Once you're stopped, holding it
    backs up at walking pace (up to 1.9 m/s on bicycles and scooters, 1.6 m/s on
    motorbikes), and letting go stops you. Sim test `[reverse]` covers four
    vehicle types: 2,161 checks, 0 failures.
  - **Buildings:**
    - Real gabled and hipped roofs from OSM `roof` tags, with ridges along the
      long side, eaves and gable walls.
    - Per-building walls from Manchester palettes: red, dark and blackened
      Victorian brick, render and pebble-dash on semis, sandstone and
      terracotta on commercial buildings, tinted glass on offices.
    - Brick coursing, white window frames, and slate or red-tile roofs.
    - Shadow casters are now split 3 x 3 per cell. That held 60 fps in Chorlton
      and Levenshulme with no VSM warning. `-StartLatLon=lat,lon` was added for
      testing.
  - **Bikes:**
    - Brake hoses and gear cables, a red rear reflector, a bottle and cage on
      road, gravel and MTB bikes, and a kickstand on commuters and e-bikes.
    - Red tail lights.
    - Road motorbikes have a UK yellow rear number plate and amber indicators.
  - The photoreal (Cesium) layer is still in the code but off by default; the
    base game doesn't use it.
- **Realism pass (session 3, second half), checked on screen:**
  - **Detailed vehicles** (`Vehicles/MRVehicleMeshBuilder.cpp`): every vehicle is
    built from its own wheelbase, wheel sizes, travel and powertrain, plus
    real-world geometry for its type (head and seat angles, chainstay, BB drop,
    fork length, rake). Parts include:
    - round tubes and spoked or cast wheels with knobbly or slick tyres
    - cassette, chainring, chain and derailleur
    - suspension forks (lowers follow the axle), shocks with coil springs, and a
      swingarm that pivots with the suspension
    - drop or flat or BMX bars, saddles and pedals
    - e-bike batteries and motors
    - motorcycle engines with cylinders, fins and exhausts, plus tanks, fairings,
      upside-down forks, number plates and mudguards
    - scooter decks, stems and hub motors
    - working headlights at night
    Checked side-on for an eMTB, enduro, DH, aero road, BMX, 450 MX, superbike,
    cruiser and dual-motor scooter (`-Showcase=90 -NoRider -Vehicle=<id>`).
  - **Real names:** all 429 vehicles carry real production model names (for
    example Trek Fuel EX, Honda CRF450R, Segway Ninebot Max G30). Ids are
    unchanged so saves still work. Handling figures are modelled per type, not
    taken from manufacturer data. Credits carry a trademark notice. Sim tests:
    2,149 checks, 0 failures, names unique.
  - **Materials** (`Tools/make_materials.bat` creates them in
    `Content/MR/Materials`; no textures, the patterns are computed in the shader):
    - rider: skin (subsurface, pores), cloth weaves, leather, denim, hair, helmet paint
    - ground: asphalt grain, paving flags, setts, grass, water and gravel
    - façades: windows per storey for brick, stone, glass and industrial
      buildings; some windows light up at night
    - vehicles: lacquered metallic paint with flake, brushed alloy, scuffed tyre rubber,
      textured plastic and seat covers (micro detail from each part's local position)
    - trees (`M_MRFoliage`): two-sided foliage lighting, so sun glows through the leaves; leaf
      clusters with dark gaps; bark with furrows and lichen. Crowns are now a core plus seven
      irregular leaf masses instead of three smooth blobs.
    - ground relief through the normal: tarmac aggregate, recessed paving joints, domed setts,
      grass tufts, water ripples and gravel. Grass uses real turf colour (it was cartoon green),
      with dry patches and clumps.
  - **Camera finish** (`MRWorldEnvironment`): slightly muted grade with cool shadows and warm
    highlights, softer highlight roll-off, bloom, lens vignette and fringing, fine grain, motion
    blur, and higher Lumen lighting and reflection quality. Real-life graphics would need
    scanned or hand-made models and textures. This pass makes the procedural world less
    cartoony, but it does not look like a photo.
  - **Named electric vehicles** from the makers' published figures (power, battery, weight, top
    speed, wheel sizes): Sur-Ron Light Bee X and Ultra Bee, Talaria Sting R MX4 and XXX, Segway
    Dirt eBike X260, Tuttio Soleil01, Stark Varg MX and EX, and KuKirin G2 Pro, G2 Max, G3,
    G3 Pro, G4 and G4 Max. The Sur-Ron-style bikes have their own look: forged beam frame,
    battery in the frame with painted side covers, mid motor, no tank. The Stark Vargs get
    radiators and shrouds. The Tuttio's specs vary between sellers, so its figures are
    approximate.
  - **Street detail:**
    - UK centre and edge lines
    - zebra crossings with Belisha beacons
    - street lamps (OSM nodes plus lamps every 32 m on roads tagged lit)
    - traffic signals, bus shelters, benches, post boxes, K6 phone boxes and
      cycle stands
    - trees in woodland and parks
    Props and tree trunks have collision; canopies don't.
  - **Night:** the 24 nearest street lamps cast real light, plus moonlight and an
    eye-adaptation floor.
  - **Streaming:**
    - Shadow casters are split into quarter-cell pieces, which stopped the VSM
      overflow coming back.
    - Cells finish one section at a time on a 6 ms budget, instead of 60–77 ms
      in one go.
    - Measured: 60 fps by day and by night at Piccadilly (one daytime run read
      42 fps while a screen recorder and other apps were busy, so treat daytime
      as 42–60).
  - Not checked yet: riding into props and trees (collision), dense or hilly
    areas, and the garage menu with the new names.
- **Fixed after the visual check:**
  - **Spawn bug:** the local player was spawned before `StartPlay` created the
    geo world. Every ride therefore started at the Entry map's PlayerStart (the
    Town Hall origin, on the editor's checker floor, facing a wall) instead of
    Piccadilly Gardens. `AMRGameMode::EnsureWorldActors` now runs from
    `RestartPlayer` too. The log line `Spawning player N at ... (geo world)`
    confirms it.
  - **Minimap:** it only drew the test district, so it was blank on the real map.
    It now draws roads (major in yellow, minor in white), paths, cycle lanes,
    rail/tram, water and building outlines from the loaded cells
    (`AMRGeoWorld::ForEachMapLine`).
  - New `-NoMenu` launch option: starts straight in free roam, for automated
    screenshot and soak runs.
  - **Shadow overflow:** UE showed "VSM Non-Nanite Marking Job Queue overflow"
    within seconds at Piccadilly, because every cell's flat ground, road, path,
    grass and water sheets were non-Nanite shadow casters. Those sections no
    longer cast shadows; buildings still cast and everything still receives.
    In a 90 s standalone run with rendering, including a ride towards
    Chinatown, the warning didn't appear on screen or in the log. It held
    59–60 fps (51 fps in the first 5 s while loading), with 24–25 cells and 271
    components. There's no frame-rate measurement from before the change, so
    the speed-up itself isn't quantified.
- **Still to check on screen:**
  - Riding feel over a longer ride.
  - The garage and rider menus.
  - Frame rate in denser or hillier areas away from Piccadilly.
- **What *was* verified this session:**
  - Sim: **2,149 checks, 0 failures**, no warnings with g++ `-Wall -Wextra`
    (`Tests/SimTests`). This covers all **429 vehicles**: each is stable at rest,
    accelerates, respects its governor, and holds a casual-assist wheelie without
    crashing. IDs and names are unique.
  - Terrain: `node Tools/geo_pipeline/test_terrain.mjs` passes 8/8 against
    published spot heights (Black Chew Head 539 m vs 542 m; Winter Hill 456 m vs
    456 m; Albert Square 39 m vs 39 m).
  - No-phasing checks: `node Tools/geo_pipeline/test_ground.mjs --cells 3000`
    passes 4/4 on real cells. It covers terrain covering the whole boundary with a
    ≥ 500 m margin, watertight cell seams, **0 of 18.3 million sampled road
    points** below the ground, and all 69,887 sampled buildings reaching into the
    ground. It's a line-for-line JS port of the C++ rules in `MRGeoWorld.cpp`, so
    it verifies the algorithm. The C++ still needs an in-engine check.
- **Map coverage: the whole county's data is imported.** That's 21,136 cells:
  275,908 roads and paths (22,904 km), 531,911 buildings, 146,828 parks, woods,
  water and other areas, all 10 boroughs, every motorway (M6, M56, M58, M60,
  M602, M61, M62, M66, M67), 4,494 bridges, and real terrain heights (1–627 m).
  Source: the Geofabrik extract of 2026-11-11 and SRTM-based terrain tiles.
  **What's still not "finished-game" quality:**
  - buildings are extruded footprints with flat roofs and block colours
  - landmarks are blocks, not hand-made models
  - no traffic, pedestrians, street furniture models or vegetation
  - tunnels aren't built (you ride over the hill above them)
  - courtyard holes in multipolygon buildings are filled in
  - World Partition/HLOD baking isn't done (the game streams cells at runtime instead)
- **Multiplayer:** implemented, **not yet tested** (needs UE plus two machines plus
  the Playit agent). See docs/PLAYIT_NETWORKING.md §1.

## Milestones

| # | Milestone | State | Verified how |
|---|---|---|---|
| 1 | Project foundation | Code complete | Sim tests; UE build pending |
| 2 | Vehicle physics | Physics done and tested; cameras coded | Sim checks; in-engine feel untested |
| 3 | Garage and vehicles | **429 free vehicles** across 8 categories; garage category filter; character editor | Every vehicle sim-tested; UI untested |
| 4 | Greater Manchester map | **Whole county imported**: roads, buildings, areas, bridges, real terrain, watertight ground with collision, anti-fall-through safety net. Missing: tunnels, roof shapes, World Partition/HLOD bake | Terrain + no-phasing tests on real data; UE pending |
| 5 | Environment | Weather/time and street lights done. Traffic, pedestrians, landmarks, vegetation **not started** | – |
| 6 | Multiplayer | Code complete; Playit researched | **Untested** |
| 7 | Final gameplay | Trick scoring, competitions, HUD, menus, audio coded | Tricks tested in sim; rest untested |
| 8 | Optimisation and release | Graphics presets coded | – |

## What makes the map solid (no phasing)

1. **Ground everywhere.** Every streamed cell builds a terrain mesh on an 8 m
   lattice from `terrain.bin`, even cells with no OSM data, and it always has
   collision. Neighbouring cells share identical edge vertices, so there are no
   cracks. Water and parks are painted onto that ground (so they're solid) rather
   than cut out of it.
2. **Roads sit on the ground, never under it.** Ribbons are resampled every
   ≤ 4 m, split into ≤ 4 m columns across, and placed 10–16 cm above the ground
   mesh. A crest pass then lifts any triangle where the ground comes within 6 cm.
3. **Buildings reach into the ground.** Walls start 1 m below the lowest ground
   point under the footprint, and the roof is never lower than 2.5 m above the
   highest one.
4. **Bridges are solid slabs.** The deck is interpolated between the abutments
   and is 80 cm thick with sides and an underside.
5. **Loading can't open a hole.** The 3 × 3 cells under the spawn point and the
   rider's own cell are built synchronously, with collision cooked immediately for
   ground, roads and paths. Other cells load nearest first.
6. **Safety net.** If a rider is ever more than 1.5 m below the real terrain
   surface, `KeepAboveGround()` respawns them at their last safe spot (corrected to
   ground height) and logs a warning, so a remaining gap would be found rather
   than hidden.

## Feature checklist (✓ done, ~ partial, ✗ not started)

**Vehicles and physics:** ✓ weight transfer ✓ suspension ✓ tyre grip ✓ brakes/ABS
✓ pedal, assist, hub motor, engine + clutch + gears, electric motorcycles ✓ steering
and lean ✓ airborne and landing ✓ wall collision ✓ 8 surfaces × 5 tyres + wetness
✓ battery, fuel, regen ✓ pedalling animation (cranks turn with the wheel) ✓ rider
posing and body movement (IK, weight shifts, foot down, standing, crash ragdoll) ✓ detailed vehicle visuals

**Vehicle list:** ✓ 429 vehicles, all free:
- 34 e-scooters
- 75 e-bikes (commuter, cargo, folding, gravel, road, hardtail, trail, enduro, SL, fat)
- 50 BMX/freestyle (park, street, race, flatland, cruiser, junior, dirt jump, slopestyle)
- 49 MTB (XC, trail, all-mountain, enduro, fat, kids)
- 49 road/gravel/urban
- 31 downhill/freeride
- 59 dirt bikes (50 cc to 500 cc, trials, pit bike, electric)
- 68 motorcycles (125 cc to 1300 cc, cruiser, supermoto, adventure, electric)

✓ garage filtered by category

**Character editor:** ✓ build ✓ height (150–200 cm) ✓ 10 skin tones ✓ 8 hair styles
✓ 11 hair colours ✓ facial hair ✓ 5 helmet styles + colour ✓ 5 visors ✓ 6 tops +
colour ✓ trim ✓ trousers ✓ gloves ✓ shoes ✓ 4 bags ✓ randomise/reset ✓ live preview
✓ saved in profile ✓ replicated to other players (14-byte look, sanitised on the
server) ✓ rider is a modelled person with every option shown (procedural skin, cloth and leather surfaces)

**Wheelies and stunts:** ✓ dedicated rebindable button ✓ 3 assist levels ✓ scoring
✓ manuals ✓ bunny hops ✓ jumps/spins/flips ✓ stoppies ✓ slides ✓ combos
✓ competitions ~ freestyle tricks (no barspins/tailwhips yet)

**Input:** ✓ Enhanced Input ✓ gamepads ✓ keyboard/mouse ✓ rebinding ✓ vibration
✓ sensitivity settings

**Multiplayer:** ✓ listen server ✓ host/join ✓ public/private/friends ✓ favourites
✓ names ✓ player list ✓ sync, interpolation and validation ✓ error messages
✓ auto-reconnect ✓ rider look sync ~ dedicated server ✗ voice/chat ✗ emotes

**World:**
- ✓ whole-county OSM import
- ✓ real terrain
- ✓ watertight ground and collision
- ✓ bridges
- ✓ painted parks, woods and water
- ✓ surfaces per road class and OSM tag
- ✓ building heights
- ✓ test district
- ✓ day/night and weather
- ✗ tunnels
- ✗ roof shapes
- ✗ rain particles
- ✗ traffic, pedestrians and traffic lights
- ✗ landmark models
- ✗ vegetation
- ✗ World Partition bake

## Session log

**Session 1:** requirements; Playit research; Sim core plus tests; UE project, pawn,
input, menus, HUD, networking, world, audio; geo pipeline plus Piccadilly import; docs.

**Session 3:**
- Installed UE 5.5.4 through the Epic Launcher.
- First compile. Fixed: `PI` macro clash, `Pi` shadowing, `Ground` enum versus
  function name, a non-literal `Printf` format, and the missing `HTTP` module
  (UrlEncode). Renamed `JoinSession` to `JoinByAddress` because it hid
  `UGameInstance::JoinSession`.
- Added `Tools/build_editor.bat` (short-path mapping, MSVC pin).
- Ran a headless smoke test, added streaming timing logs, and time-budgeted cell
  finishing.
- Realism pass: detailed procedural vehicles, real model names, shader
  materials, road markings, street furniture, trees, night lighting, split
  shadow casters and incremental cell finishing. Launch options `-Showcase=<yaw>`,
  `-NoRider` and `-Vehicle=<id>` were added for screenshot runs.
- First visual check in the editor. Fixed the spawn order bug (rides started at
  the origin, not Piccadilly). The minimap now draws the real streets. Added
  `-NoMenu`.

**Session 2:**
- Installed VS 2022 Build Tools and the Windows SDK.
- Full-county import via a new `.osm.pbf` reader (`pbf_to_tiles.mjs`, with
  multipolygon support).
- Terrain pipeline (`fetch_terrain.mjs`) plus spot-height test.
- Ground mesh, draped roads, bridges, building footing and the fall-through safety
  net in `MRGeoWorld`.
- No-phasing test. It found terrain poking up to 12 cm through roads on hill
  crests; fixed with a crest pass and verified at 0 in 18.3 million samples.
- Character editor (`FMRRiderLook`, Rider menu).
- 429-vehicle lineup with a per-vehicle wheelie test and garage category filter.
- Fixed `process_osm.mjs` wiping `Content/GeoData` (it deleted `terrain.bin`).
- Tightened the landmark name matching (it flagged every Co-op shop).

## Kerbs, road-only routes, teleport, stray blocks (2026-11-14)

- **Kerbs (checked on screen near Deansgate and Oxford Road):** streets (major, minor, residential,
  4.5 m or wider) get a raised pavement on both sides: 12 cm kerb face, kerb stone, 2 m of flags,
  bevel back to the ground (`Kerbs` in `MRGeoWorld.cpp`). The kerb drops to road level where another
  carriageway meets it (`FSegGrid::Clearance`). No pavements in farmland, woodland or water.
  Not checked: ride feel over kerbs at speed, cell borders, performance in dense areas.
- **Stray blocks (causes fixed from the map data, not all seen on screen):** canopies
  (`roof`/`canopy`/`carport`) are a slab on posts; kiosks, huts and footprints under 16 m2 with no
  tagged height are 3 m; bridge piers, lamps, signals, shelters, benches, post and phone boxes are
  not built on a carriageway; a building with a public street through it is lifted 4.5 m.
- **Routes:** `make_routes.mjs` leaves out tunnels and makes tracks (x3), pedestrian streets (x5)
  and paths (x8) last-resort only. `routes.bin` rebuilt (352,331 nodes, 405,466 edges).
  Checked: Piccadilly to Circle Square follows streets.
- **Teleport to waypoint (checked: Piccadilly to the Etihad):** T or left-stick click on the map.
  Lands on the nearest road to the marker, facing along it. Test flag: `-TeleportWaypoint`.
  Not checked with a friend connected (client asks the host through `ServerTeleportTo`).
- **Bridge slabs across roads fixed (checked on screen near Deansgate-Castlefield, 53.4746 N 2.2481 W):**
  the pipeline lifts every bridge deck 50 cm per OSM layer, which left a slab wall across the road at
  both ends of almost every bridge. `Ribbon` now lays a deck on the ground while it is under 2.5 m
  above it and blends to deck height over the next 2 m; no slab is built on the ground part.
  `Tools/geo_pipeline/scan_bridges.mjs` checks all 7,048 bridge-to-road joins: 6,561 steps over
  30 cm before, 0 after. Side effect: low bridges (canals, small rivers) are now at ground level,
  so the road crosses the water on the flat. Not checked: tall bridges and viaducts on screen.
