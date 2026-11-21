# Technical architecture

## 1. Layout

```
ManchesterRide/
  ManchesterRide.uproject          UE 5.5, plugins: EnhancedInput, ProceduralMeshComponent, OnlineSubsystemNull/Utils
  Config/                          engine, game, input settings (networking & surfaces live here)
  Content/GeoData/                 streamed map cells produced by Tools/geo_pipeline (plain JSON)
  Source/ManchesterRide/
    Sim/        engine-independent C++17: riding physics, tricks, vehicle catalogue, geo projection
    Vehicles/   AMRRideVehicle (pawn wrapping the sim), UMRVehicleLibrary (catalogue for UI/BP)
    Core/       game instance, game mode/state, player controller (input + menus), player state, HUD, save
    Input/      UMRInputConfig - Enhanced Input actions/contexts built in code, runtime rebinding
    Net/        compact replicated ride state, network settings
    World/      world environment (time/weather), test district, geo world streaming
    Audio/      procedural synth audio component
  Tests/SimTests/                  standalone tests for Sim/ (no Unreal needed)
  Tools/geo_pipeline/              OSM download -> projection -> per-cell JSON
```

**Why the physics lives outside Unreal.** `Sim/` uses no Unreal types, so it can
be compiled and tested anywhere (`Tests/SimTests`, 2,149 checks over 415 vehicles). It runs
identically on every machine and is deterministic for identical inputs, which
matters for networking. The pawn only samples the world (wheel traces), feeds the
sim and moves the actor.

**No binary assets yet.** Input actions, mapping contexts, materials (dynamic
instances of an engine material), meshes (engine basic shapes and procedural
meshes), audio (synthesised) and the map (procedural, or streamed from GeoData)
are all created in code. The project opens and plays from source alone. Authored
assets (bike meshes, rider skeletal mesh, landmarks, recorded sounds) replace
the placeholders later without changing the gameplay code.

## 2. Riding simulation (Sim/RideSim.cpp)

One model covers every two-wheeler. The behaviour comes from data
(`FVehicleParams` in `VehicleCatalog.cpp`), not from per-vehicle classes.

| Subsystem | Model |
|---|---|
| Suspension | Ray-cast spring/damper per wheel. Stiffness and damping are derived from mass, weight distribution and a target sag; separate compression/rebound damping; bump stop. |
| Load transfer | Total support from the springs, split front/rear by COM position minus `m·a·h/L` (filtered acceleration). |
| Traction | Per-wheel `μ(surface, tyre, wetness) × load`, with a friction circle shared by braking/driving and cornering. 8 surfaces × 5 tyre types; wet multiplier per surface (wet setts are the worst). |
| Powertrains | Pedal (rider power and force limit); UK EAPC pedal assist (×0.5–3 assist, 25 km/h cut-off, battery drain); hub-motor scooters (throttle, governor, regen braking); combustion (torque curve, idle governor, rev limiter, stick/slip friction clutch, manual or auto clutch, manual or auto gears, fuel use from BSFC). |
| Brakes | Per-wheel limits; optional ABS on the front; a locked rear starts slides. |
| Pitch | Rigid-body rotation about the rear contact (wheelie) or front contact (stoppie), driven by inertial load transfer, gravity, rider weight shift and the wheelie button. The balance point is `atan(a/h)`. Loop-out or endo past it plus a margin. |
| Wheelie assist | PD controller toward 82 % of the balance point. Casual (strong, no loop-outs), Balanced (partial, needs throttle on heavy bikes), Realistic (off). |
| Steering & lean | Speed-sensitive steering lock, kinematic yaw, equilibrium lean `atan(v·ω/g)` with first-order response. Grip saturation causes understeer; realistic mode can lowside. |
| Slides | A sliding rear (skid or wheelspin) adds yaw and a sideways component. |
| Air | Ballistic. Rider controls pitch (flips) and spin; landing assist follows the trajectory; landing checks impact speed, pitch mismatch, unfinished spin and lean. |

Units are SI. The sim steps at 120 Hz (fixed sub-steps in the pawn).

**Frames.** The actor origin is the collision box centre. The *contact frame*
origin is the rear tyre contact at full extension. Actor rotation =
(ground pitch, yaw, 0). Wheelie pitch and lean only rotate the visual
`BodyPivot`, so the collision box stays stable.

**Tricks** (`TrickTracker.cpp`): wheelie / manual (unpowered), stoppie, bunny hop,
air (+180° spins, flips), slide, bail. Combos multiply when banked after 2 s.
The server validates reported points against per-trick caps.

## 3. Vehicles: the free-vehicle policy

The data model has **no price, ownership, unlock or currency field anywhere**.
`AMRGameMode::ChangeVehicle` spawns any valid catalogue id for any player at any
time, and invalid ids fall back to the default. Paint and rider colours are free,
and remembered per vehicle.

The catalogue has **415 vehicles**. Seventeen are hand-tuned reference models; the
rest come from `AddLineup()` in `Sim/VehicleCatalog.cpp`. That generator combines
~120 real-world model types with fictional North-West brands:

- 29 pedal types × 6 brands
- 12 e-bike types × 6 brands
- 8 scooter types × 4 brands
- 30 motor types × 4 brands (12 dirt, 14 road, 4 electric)

Each type has realistic mass (vehicle + 75 kg rider; lighter riders for youth
models), geometry, travel, power, battery or fuel, and gearing. Combustion gearing
is derived so that redline in top gear meets the target top speed. Brands apply
small deterministic differences in mass, power, CoM height and wheelbase, so two
brands' "Enduro 170" ride slightly differently. Every vehicle goes through the same
constructors (sag-derived springs, mass-scaled brakes) and is tested
(`TestWholeCatalogue`, `TestLineup`). Ids are stable (`cyc_kinder_enduro_170`),
so saves and replication don't depend on list order.

**Character editor (`Core/MRRiderLook`):** `FMRRiderLook` is 14 byte-sized indices:
build, height, skin, hair, facial hair, helmet style, visor, top style, trim,
trousers, gloves, shoes and bag. Jacket and helmet colours keep their own fields.
It's saved in the profile, sent in the login URL (`?look=<hex>`), clamped by
`Sanitise()` on the server, and replicated on the PlayerState and the pawn.
`ServerSetRiderLook` changes it mid-session without a respawn. The pawn rebuilds
only the rider when the look changes.

## 4. Networking

**Transport:** Unreal IpNetDriver over a single UDP port (Playit-compatible, see
PLAYIT_NETWORKING.md). Listen server by default; a dedicated-server target exists.

**Movement model: owner-simulated, server-validated.**

1. The owning client runs the full sim locally, so there is no input latency and
   no rubber-banding even at 150 ms+ tunnel pings.
2. About 30 times a second it sends `FMRNetRideState` (~32 bytes: quantised
   position, yaw, pitch, lean, speed, suspension, rpm, flags, timestamp) by
   unreliable RPC.
3. The server validates it: no out-of-order packets, horizontal and vertical
   distance limits from the vehicle's top speed, a speed cap. It rejects bad states
   with a `ClientTeleport` correction and logs them. Respawns open a one-shot
   acceptance window.
4. The server replicates the accepted state to everyone else (`COND_SkipOwner`) and
   tracks a safe respawn point per rider.
5. Remote riders are rendered from a 32-entry snapshot buffer. A sender-clock offset
   is estimated as the minimum observed (receive − send), with slow drift
   correction. Rendering interpolates `InterpolationDelay` (120 ms) in the past and
   extrapolates up to 250 ms on packet loss.

*Why not server-authoritative with client prediction and replay?* A bicycle's
pitch and lean are very sensitive, so mispredictions show up as visible wobbles.
The sim is deterministic, so this remains possible (re-simulating from input
history). For a co-operative freeride game, owner authority with validation gives
the best feel. Trade-off: a modified client can cheat within the validation
bounds. Trick scores are capped server-side.

**Bandwidth:** ~32 B × 30 Hz ≈ 1 KB/s per rider upstream, and the same per
visible rider downstream, before Unreal's own header overhead. Riders more than
1.5 km away stop replicating (`NetCullDistanceSquared`).

**Sessions:** visibility (public / private password / friends list), max players
(clamped to the config ceiling), protocol version, all checked in `PreLogin`.
The client gets the error code through `ENetworkFailure::FailureReceived`.
Auto-reconnect makes 3 attempts with backoff after a timeout or lost connection.

## 5. World

**Coordinates:** WGS84 → Transverse Mercator centred on Manchester Town Hall,
mapped to Unreal +X east, +Y south. `Sim/GeoProjection.cpp` (game) and
`Tools/geo_pipeline/geo.mjs` (pipeline) implement identical maths and are
cross-checked. Distances match WGS84 ellipsoidal distance to < 0.001 % across the
county (`test_geo.mjs`). The projected county boundary is 1,276.9 km² against the
official 1,276 km².

**Pipeline → game:** `pbf_to_tiles.mjs` (Geofabrik .osm.pbf, whole county, ~16 s) or `fetch_osm.mjs` (Overpass, tiled, cached) →
`process_osm.mjs` (classify roads, surfaces, buildings with heights and styles,
areas; boundary clip; 256 m cells; validation report) →
`Content/GeoData/cell_X_Y.json`.

**Terrain:** `fetch_terrain.mjs` writes `Content/GeoData/terrain.bin`. It's a 16 m
int16 grid covering the county plus 1 km, resampled from SRTM-based Terrarium
tiles, with Z = 0 at the Town Hall (39.4 m above sea level). `FMRTerrain::GroundZ` is
the exact height of the triangulated 8 m ground lattice the game builds, and roads,
buildings, spawns and the fall-through check all use it. Bridge deck heights
(interpolated between the abutments) are baked into the cell data as [x, y, z].
DEVELOPMENT_STATUS.md, "What makes the map solid", lists the no-phasing rules;
`test_ground.mjs` verifies them on real cells.

**Streaming now (`AMRGeoWorld`):** each machine streams cells within 700 m of its
own viewer (unloading beyond 950 m). Cells are parsed and meshed on worker
threads and turned into procedural mesh components on the game thread (max 2 cells
per frame). Each surface/material group becomes a component with the correct
physical surface, so tyre grip follows the real OSM `surface` tag. The server
streams nothing for remote riders.

**Planned (Milestone 4 completion):** an editor importer bakes the same cells into
a World Partition map (`/Game/Maps/GreaterManchester`, 256 m grid, HLOD layers for
buildings), upgrades terrain to EA LIDAR DTM (1–2 m), and replaces
procedural meshes with instanced modular UK building kits and landmark models.

## 6. Performance budget (target: RTX 3060 / Ryzen 5 5600G / 16 GB)

- Graphics presets map to engine scalability 0–3. Lumen GI and reflections only
  run at High and Ultra; Low adds 75 % screen percentage and no volumetric fog or
  VSM. Frame cap is 60 by default.
- Repeated props use instanced static meshes. Vehicles are ~40 primitive parts
  (to be replaced by a single skeletal and static mesh).
- Streaming radius 700 m keeps ~25–40 cells resident.
- Sim cost: ~120 Hz × (2 line traces + arithmetic) per locally controlled rider
  only. Remote riders cost interpolation only.

## 7. Input

Enhanced Input actions and contexts are created in `UMRInputConfig`. There are two
contexts, riding and menu, swapped when the menu opens. All single-key actions are
rebindable separately for keyboard and gamepad (Settings → Controls, or
`MRBind`). Overrides are stored in the save game. Steering has a progressive curve
plus ramping for keyboard input; sensitivities are configurable. Vibration uses
dynamic force feedback (landings, crashes, slides) and can be toggled.
