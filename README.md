# ScenarioEditor

**ScenarioEditor** is a **Windows C++ desktop application** (MFC, dialog-based) for authoring, recording, replaying, and streaming **DIS ([Distributed Interactive Simulation](https://en.wikipedia.org/wiki/Distributed_Interactive_Simulation))** scenarios. You define entities, motion paths, timing, and output targets through tabs and dialogs, and the app generates DIS PDUs and plays them out over the network (or to a file) in real time.

ScenarioEditor is the **authoring half** of a two-app pair. Its companion is **[DISBrowser](../DISBrowser)** (Unreal Engine 5.7), which receives the same DIS streams and renders the entities in a georeferenced 3D world. The two apps are decoupled — they communicate only over the wire (DIS PDUs over UDP) plus one small file-based handoff. See **[How it talks to DISBrowser](#how-it-talks-to-disbrowser)**.

> The authoritative design spec is **[spec.mk](spec.mk)**. This README is an orientation + build/run guide.

---

## How it talks to DISBrowser

There are **two channels**, and one optional trigger:

### 1. DIS PDUs over UDP (the live data path)

ScenarioEditor is a **UDP client / sender — it transmits only** (it opens a UDP socket and sends; it does not bind a listening server socket). DISBrowser is the receiver. During playback, `ScenarioWorker` samples each entity's motion and, per tick, builds and sends one PDU per entity:

- **What is sent:** **DIS v7 (IEEE 1278.1) Entity State PDUs**, built with the Open-DIS C++ library (`DIS::EntityStatePdu`) and marshalled to big-endian wire format (`PduBuilder`). Each PDU carries the entity's **ID**, **entity type** (SISO-REF-010 kind/domain/country/… tuple), **ECEF position**, **ECEF linear velocity** (tangent to the path, so the receiver can dead-reckon), and **orientation** (ψ/θ/φ). The scenario's **Exercise ID** (default `1`) and DIS version tag every PDU.
- **Two output modes** (set on the **Run** tab; defaults from `Scenario.h`):

  | Mode | Default destination | Notes |
  | --- | --- | --- |
  | **UDP unicast** | **`127.0.0.1:3001`** | Point-to-point to the machine running DISBrowser. |
  | **UDP multicast** | group **`239.1.2.3:3001`**, TTL `1`, interface `0.0.0.0`, loopback on | Group must be in `224.0.0.0/4`. |

- Two more output targets exist that don't hit the network: **record to `.disrec`** (a timestamped PDU-stream file) and **replay** a `.disrec` back out over UDP, plus a **preview-only** mode (no I/O).

#### Socket defaults, and how to change the port

The default DIS destination is **UDP `127.0.0.1:3001`** (unicast) — deliberately aligned with DISBrowser's listener. **Change it on the Run tab** — set the unicast IP/port (or the multicast group/port) fields; the value is stored with the scenario (`[Output]` section) and in `settings.ini`.

> ⚠️ **The port must match DISBrowser.** Both sides default to **`3001`** — ScenarioEditor's output (`OutputConfig` in `Scenario.h`) and DISBrowser's `Config/DISBrowser.ini` → `[DIS] ListenPort`. If you change one, change the other. The **Exercise ID** should match too if DISBrowser is filtering on it.

### 2. Level/origin/basemap handoff — `Config\Startup.ini` (the file path)

The **Run** tab's **"Configure Unreal"** button writes `<DISBrowserProjectDir>\Config\Startup.ini` (`StartupIniWriter`), which DISBrowser reads at boot to pick its scene. It writes:

- `[Startup] Level=` — which DISBrowser level to open (Generic, Beach, Forest, GodView, Hanger, Main).
- For the **Generic** level: `OriginLatitude` / `OriginLongitude` / `OriginAltitudeMeters` (scenario origin), `Map=` (basemap: Satellite / Topographic / Custom / …), `DynamicTiles`, the painted `TerrainBoundsLat/LonMin/Max` box, and — for self-hosted terrain — the quoted `CesiumTilesetUrl` / `CesiumRasterOverlayUrlTemplate`.

The file is **deleted and rewritten** on every "Configure Unreal", so it always reflects the current scenario.

### 3. (Optional) 3D terrain build trigger

The Preview tab's **"Build 3D Terrain"** shells out to DISBrowser's `Scripts\retile_terrain.cmd` with the painted lat/lon box, so the georeferenced 3D terrain DISBrowser streams matches the scenario area. (See DISBrowser's README → self-hosted terrain.)

---

## Features

- **Scenario authoring** — name/description, DIS **v6/v7** selection, exercise & site IDs (Setup page).
- **Entity catalog editor** — a curated SISO-REF-010 subset in `EntityTypeCatalog.ini`, edited via a tree editor (add/remove kind/domain/country/category nodes and attributes).
- **Asset / entity editor** — per-entity type, marking text, initial position and orientation.
- **Motion path authoring** — per-entity motion segments: **Stationary, Stop, Line, Ellipse, Follow**, with dead-reckoning algorithms 1/2/5 chosen per segment type.
- **2D preview canvas** — Direct2D/DirectWrite map view: projects entities to screen, draws terrain footprint and level zones, drag-to-set-start, group select, pan/zoom.
- **3D Cesium globe preview** — a WebView2-hosted CesiumJS globe for previewing entities and painting terrain boundaries on a real globe *(Visual Studio build only — see [Building](#building--compilation))*.
- **Map backdrops** — background raster tiles over WinHTTP + WIC with memory/disk cache: None / Satellite (ESRI World Imagery) / Topographic (OpenTopoMap) / Cesium 3D.
- **Named locations ("Places")** — save and recall lat/lon/alt viewpoints; drives map centering and globe framing.
- **Terrain boundary painting + Build 3D Terrain** — paint a lat/lon box and build DISBrowser's self-hosted Cesium terrain for it.
- **DISBrowser handoff** — one-click "Configure Unreal" writes `Startup.ini` (see above).
- **Scenario save/load** — human-readable `.ini` format (`ScenarioIO`).
- **DIS playback** — worker-thread playback with Start/Pause/Resume/Stop/loop; **record & replay** via `.disrec`.
- **Validation** — a validator enforces DIS rules (marking length, multicast range, timestamps…) and estimates output bandwidth.
- **Terrain-server controls** — Start/Stop the self-hosted terrain tile server and gate the boundary/build tools on it (Preview tab).

### UI structure

Top-level tabs: **Run** (output/playback + Configure Unreal), **Plays** *(scaffold)*, **Deploy** *(scaffold)*, **Preview** (2D canvas + 3D globe). A modal **Attributes** notebook hosts the **Setup**, **Asset/Entity Editor**, and **Motion Path Editor** pages.

### Conventions newcomers get wrong (read this)

| Field | Convention | Why |
|---|---|---|
| **Local coordinates** | **ENU** (East/North/Up), tangent plane at scenario origin. Not NED. | Matches DISBrowser, KML, Cesium, Unreal Georeferencing. |
| **Altitude** | **WGS-84 ellipsoidal** everywhere. Not MSL. | DIS ECEF math assumes ellipsoidal; MSL→ellipsoidal is a user-side concern. |
| **Heading** | 0° = North, clockwise, range [0, 360). | Standard sim convention; mapped to DIS `psi` at the PDU boundary. |
| **Pitch / Roll** | Pitch+ = nose up; Roll+ = right wing down. | — |
| **DIS timestamps** | **Relative** (bit 0 = 0), always. | Recordings stay valid across hosts and clocks. |
| **DIS orientation** | ψ/θ/φ in **radians**, entity→world. Editor stores degrees, converts at the PDU boundary. | IEEE 1278.1 §B.1.6.4. |
| **Marking text** | ASCII only, 11 bytes, null-padded. Non-ASCII stripped with a warning. | DIS standard. |
| **Dead reckoning** | Algorithm 1 (Static), 2 (F,P,W), or 5 (F,V,W), per motion-segment type. Analytic derivatives only. | Smooth motion at the receiver from low-Hz streams. |
| **Multicast IPs** | Must be in `224.0.0.0/4`. | RFC 5771. |
| **Logging** | Always use `LOG(x)` from `log.h`. | One log, one place. |

---

## Dependencies

- **Windows 10/11**, x64.
- **Visual Studio 2022** with the **MSVC v143** toolset and the **Windows 10 SDK**, including:
  - **Desktop development with C++** workload
  - **C++ MFC for v143 build tools (x64/x86)** component *(MFC is used dynamic + Unicode)*
  - **C++ CMake tools for Windows** *(only needed for the CMake build)*
- **Open-DIS C++** — the DIS serialization library ([`open-dis/open-dis-cpp`](https://github.com/open-dis/open-dis-cpp)), consumed as `OpenDIS7`. Vendored as a **sibling directory** `..\open-dis-cpp-master` (next to this repo, not a submodule).
- **WebView2** — NuGet package **`Microsoft.Web.WebView2`** (v1.0.4078.44), restored to `packages\`. Required only for the **Visual Studio** build's 3D globe.
- **CesiumJS** — loaded at runtime from a CDN by the globe view; needs internet access when using the 3D globe (no build-time dependency).
- **System libraries** (ship with the Windows SDK / VS): Direct2D + DirectWrite (`d2d1`, `dwrite`), WinHTTP (`winhttp`) + WIC (`windowscodecs`) for map tiles, Winsock2 (`Ws2_32`) for DIS UDP, plus `winmm` and `uxtheme`.

> Not a dependency: the `Sgp4Prop_v8.4` sibling folder is unrelated — ScenarioEditor has no SGP4 / satellite-propagation code.

---

## Installation

### 1. Install the toolchain
Install **Visual Studio 2022** with the components listed under *Dependencies* (Desktop C++, MFC v143, Windows 10 SDK, and — for the CMake build — the CMake tools component).

### 2. Clone the repositories
Clone ScenarioEditor and, **next to it**, the Open-DIS library (the build expects it as a sibling `open-dis-cpp-master`):
```bash
git clone <scenarioeditor-repo-url> ScenarioEditor
git clone https://github.com/open-dis/open-dis-cpp open-dis-cpp-master
```
Your layout should look like:
```
...\some-parent\
├── ScenarioEditor\        ← this repo
└── open-dis-cpp-master\   ← Open-DIS library (sibling)
```

### 3. Build Open-DIS (`OpenDIS7`)
- For the **CMake build** of ScenarioEditor (below), Open-DIS is added automatically as a subproject — **skip this step**.
- For the **Visual Studio build**, build Open-DIS first so `OpenDIS7.lib` exists at `..\open-dis-cpp-master\build\{Debug,Release}`:
  ```bash
  cd open-dis-cpp-master
  cmake -S . -B build -G "Visual Studio 17 2022" -A x64
  cmake --build build --config Release
  cmake --build build --config Debug
  ```

### 4. Restore the WebView2 NuGet package (Visual Studio build only)
```bash
cd ScenarioEditor
msbuild -t:restore ScenarioEditor.sln     # or: nuget restore ScenarioEditor.sln
```

### 5. Build ScenarioEditor
Pick one of the two builds below.

---

## Building / Compilation

There are **two builds**, and they are not equivalent:

| Build | 3D Cesium globe | Extras | Use it for |
| --- | --- | --- | --- |
| **Visual Studio / MSBuild** | ✅ real WebView2 globe (`HAVE_WEBVIEW2`) | — | The full, shippable app |
| **CMake** | ❌ placeholder (2D only) | builds 5 console test exes | 2D work + running the unit/round-trip tests |

### A. Visual Studio / MSBuild (full app with 3D globe)
Requires steps 3 (OpenDIS built) and 4 (WebView2 restored). From a *Developer Command Prompt for VS 2022* (or with `msbuild` on `PATH`):
```bash
cd ScenarioEditor
msbuild ScenarioEditor.vcxproj /p:Configuration=Release /p:Platform=x64
```
- Output: **`build\x64\Release\ScenarioEditor.exe`** (Debug → `build\x64\Debug\`). A post-build step copies `EntityTypeCatalog.ini` next to the exe.
- ⚠️ **Quirk:** the `.sln` maps *Release → Debug* for ActiveCfg, so building the **solution** in "Release" actually builds Debug. Build the **`.vcxproj`** directly (as above) to get a true Release binary.

### B. CMake (2D-only + test suite)
```bash
cd ScenarioEditor
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```
- Builds Open-DIS as a subproject (no separate step 3 needed) and produces the app **plus** the `coord_roundtrip`, `scenario_io_roundtrip`, `motion_sampler_test`, `disrec_roundtrip`, and `validator_test` executables.
- Output under the chosen build dir (e.g. `build\Release\ScenarioEditor.exe`). Ensure `OpenDIS7.dll` sits next to the exe at runtime.
- This build does **not** define `HAVE_WEBVIEW2`, so the Preview tab's 3D globe is a placeholder — use build **A** to work on the globe.

---

## Running

1. Launch `ScenarioEditor.exe`.
2. On the **Setup** page (Attributes), name the scenario and pick DIS v6/v7 + exercise/site IDs. Add entities and motion paths via the Asset/Entity and Motion Path editors.
3. On the **Preview** tab, place/inspect entities on the 2D map or 3D globe; optionally paint a terrain boundary and build terrain.
4. On the **Run** tab, set the output target (**match the port to DISBrowser's `ListenPort`**), then **Start** playback to stream Entity State PDUs — or **Configure Unreal** to hand the level/origin/basemap to DISBrowser for its next boot.

---

## Project structure

```
ScenarioEditor/
├── ScenarioEditor.sln / .vcxproj     Visual Studio build (WebView2 3D globe)
├── CMakeLists.txt                    CMake build (2D-only + tests)
├── packages.config                   WebView2 NuGet reference
├── spec.mk                           Authoritative specification
├── log.h / log.cpp                   LOG(x) — shared with the DISBrowser sibling
├── EntityTypeCatalog.ini             Curated SISO-REF-010 subset for pickers
└── src/
    ├── ScenarioEditorDialog.*        Main dialog + top tab control
    ├── OutputPlaybackPage.*          "Run" tab: output config, playback, Configure Unreal
    ├── PreviewPage.* / PreviewCanvas.* / CesiumView.*   2D canvas + 3D globe
    ├── AttributesDialog.* + ScenarioSetupPage.* / AssetEntityEditorPage.* / MotionPathEditorPage.*
    ├── CatalogEditorDialog.* / CatalogAddDialog.*       entity-type catalog editing
    ├── Scenario.h / ScenarioIO.*     model + .ini load/save
    ├── ScenarioWorker.* / PduBuilder.* / UdpSender.*    DIS playback + Open-DIS + Winsock
    ├── Disrec*.*                     .disrec record/replay format
    ├── StartupIniWriter.*            DISBrowser Startup.ini handoff
    ├── MapTileService.* / Direct2DContext.*             map tiles + Direct2D
    ├── CoordTransforms.* / MotionSampler.* / Validator.*
    └── SettingsIO.*                  settings.ini (UI state)
```

> **Repository hygiene:** source + config only. Build outputs (`build*/`, `bin/`, `obj/`), restored NuGet `packages/`, VS/IDE state, ad-hoc `build_*.log`, and runtime files written next to the exe (`settings.ini`, `error.log`) are excluded via `.gitignore`.

---

## License

No license has been specified yet. All rights reserved by the author unless a `LICENSE` file is added.
