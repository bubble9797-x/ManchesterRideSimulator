# Dependencies

| Dependency | Version | Used for | Licence / notes |
|---|---|---|---|
| Unreal Engine | 5.5 | Engine | Epic EULA (royalties apply above the revenue threshold) |
| UE plugins | bundled | EnhancedInput, ProceduralMeshComponent, OnlineSubsystem/Null/Utils (IpNetDriver) | Part of UE |
| Visual Studio 2022 Build Tools | 17.x (MSVC 14.38+; installed: 14.44) + **Windows SDK** 10.0.22621 | Building UE 5.5 C++ | Microsoft licence. UE 5.5 does **not** support the VS 2026 (v18) compiler |
| Playit.gg agent | current | Tunnelling for hosts (UDP) | Playit terms. Free and Premium tiers; see PLAYIT_NETWORKING.md |
| Node.js | ≥ 18 (uses built-in `fetch`) | Geo pipeline | MIT. The pipeline has **no npm packages** |
| OpenStreetMap data | Geofabrik Greater Manchester extract (2026-11-11) + Overpass for the boundary | Roads, buildings, land use, boundary | **ODbL 1.0**: "© OpenStreetMap contributors" must be credited (done in Credits and the manifest) |
| Terrain Tiles (Tilezen, AWS Open Data) | Terrarium PNG, zoom 12 | Ground heights (`terrain.bin`) | Open data; UK coverage derived from SRTM (public domain) / EU-DEM; attribution: https://github.com/tilezen/joerd/blob/master/docs/attribution.md |
| Overpass API (overpass-api.de) | public instance | Downloading OSM data | Usage policy: keep requests modest. The fetcher is single-threaded with delays and backoff |
| g++ / MSVC | any C++17 | Running `Tests/SimTests` without UE | – |

Planned (not yet used): EA LIDAR Composite DTM (Open Government Licence) for
higher-resolution terrain.

Engine content referenced at runtime: `/Engine/BasicShapes/*`,
`/Engine/Maps/Entry`, and optionally
`/Engine/EngineSky/VolumetricClouds/m_SimpleVolumetricCloud_Inst`. All are
cooked through DefaultGame.ini.
