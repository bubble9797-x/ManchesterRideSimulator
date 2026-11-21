# Manchester Riding Simulator

An open-world multiplayer riding simulator set in Greater Manchester, built in Unreal
Engine 5: e-scooters, e-bikes, BMX, mountain, road and downhill bikes, dirt bikes
and motorcycles. **Every vehicle is free.** Wheelies are easy, physics is convincing,
and friends join through Playit.gg.

> **Status:** early development. See [DEVELOPMENT_STATUS.md](DEVELOPMENT_STATUS.md)
> for exactly what works and what has been verified. It compiles and runs in UE 5.5.4,
> and riding at Piccadilly Gardens has been checked on screen
> ([day](docs/screenshot_piccadilly_day.png), [night](docs/screenshot_piccadilly_night.png),
> [vehicles](docs/screenshot_vehicles.png)). The whole county's roads, buildings, areas and
> real terrain are imported, with windowed façades, road markings, street furniture and trees.
> Buildings are still flat-roofed extrusions, and there's no traffic, pedestrians or landmark
> models yet. Vehicle names are real models; their handling is modelled per type.

## Quick start

1. Install Unreal Engine **5.5** (Epic Games Launcher → Unreal Engine → Library → +)
   and the **Visual Studio 2022** C++ build tools with a Windows SDK (UE 5.5 can't
   use the VS 2026 compiler).
2. Run `Tools\build_editor.bat`. If `Content\GeoData\routes.bin` is missing, run `node Tools\geo_pipeline\make_routes.mjs` (map waypoints need it).
   If `Content\MR\Materials` is missing (or has no `M_MRRider`), run
   `Tools\make_materials.bat` once, with the editor closed. It builds **ManchesterRideEditor Win64 Development**
   from drive `M:` (a `subst` mapping that keeps MSVC paths under 260 characters)
   with MSVC 14.44 pinned.
3. Open `M:\ManchesterRide.uproject` in UE 5.5 and press Play. You spawn at Piccadilly Gardens on a hardtail
   eMTB. The main menu has the Garage (415 free vehicles), Rider (character editor),
   Multiplayer and the test district.

Optional photoreal map (Google 3D tiles via Cesium ion, your own free token):
[docs/PHOTOREAL_MAP.md](docs/PHOTOREAL_MAP.md).

Controls: [docs/CONTROLS.md](docs/CONTROLS.md). Multiplayer setup:
[docs/PLAYIT_NETWORKING.md](docs/PLAYIT_NETWORKING.md).

## Tests that run without Unreal

```bash
powershell -ExecutionPolicy Bypass -File Tests/SimTests/build_and_run_gcc.ps1
```

```bash
node Tools/geo_pipeline/test_geo.mjs
```

The first builds the riding physics with g++ (CLion's bundled MinGW works) and runs
2,149 checks over all 415 vehicles. `Tests/SimTests/build_and_run.bat` does the same with MSVC. The second
validates the map projection.

## Rebuilding the map

The whole county is already in `Content/GeoData`. To refresh it from newer
OpenStreetMap data, see [Tools/geo_pipeline/README.md](Tools/geo_pipeline/README.md)
(about 5 minutes). Map checks:

```bash
node Tools/geo_pipeline/test_terrain.mjs
```

```bash
node --max-old-space-size=4096 Tools/geo_pipeline/test_ground.mjs --cells 3000
```

## Documentation

- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md): code layout, physics model, networking model, world pipeline
- [docs/PLAYIT_NETWORKING.md](docs/PLAYIT_NETWORKING.md): Playit compatibility findings, hosting and joining, errors, test plan
- [docs/CONTROLS.md](docs/CONTROLS.md)
- [docs/DEPENDENCIES.md](docs/DEPENDENCIES.md)
- [TODO.md](TODO.md) and [DEVELOPMENT_STATUS.md](DEVELOPMENT_STATUS.md)

Map data © OpenStreetMap contributors, ODbL.
