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
  | **UDP multicast** | group **`224.252.0.1:3001`**, TTL `1`, interface `0.0.0.0`, loopback on | Group must be in `224.0.0.0/4`. See [Using UDP multicast](#using-udp-multicast). |
  | **TCP direct connection** | connect to **`127.0.0.1:3002`** | Reliable, ordered, one peer. See [Using TCP direct connection](#using-tcp-direct-connection). |

- Two more output targets exist that don't hit the network: **record to `.disrec`** (a timestamped PDU-stream file) and **replay** a `.disrec` back out over UDP, plus a **preview-only** mode (no I/O).

#### Socket defaults, and how to change the port

The default DIS destination is **UDP `127.0.0.1:3001`** (unicast) — deliberately aligned with DISBrowser's listener. **Change it on the Run tab** — set the unicast IP/port (or the multicast group/port) fields; the value is stored with the scenario (`[Output]` section) and in `settings.ini`.

> ⚠️ **The port must match DISBrowser.** Both sides default to **`3001`** — ScenarioEditor's output (`OutputConfig` in `Scenario.h`) and DISBrowser's `Config/DISBrowser.ini` → `[DIS] ListenPort`. If you change one, change the other. The **Exercise ID** should match too if DISBrowser is filtering on it.

#### Using UDP multicast

Multicast sends one copy of each PDU to a group address; every receiver that has *joined* that group gets it. Use it when more than one viewer needs the same exercise, or when you don't want to hard-code a receiver's IP.

**Turn it on (ScenarioEditor side).** Everything is on the **Run** tab, in the **UDP Multicast Settings** group box:

| Field | Default | What it does |
| --- | --- | --- |
| **Group** | `224.252.0.1` | Destination group. Must be in `224.0.0.0/4`. |
| **Port** | `3001` | Must equal DISBrowser's `[DIS] ListenPort`. |
| **TTL** | `1` | Router hops. **`1` never leaves the local LAN segment** — raise it to cross a router. `0` never leaves the host. |
| **Interface** | `0.0.0.0` | Which NIC to send from. `0.0.0.0` lets the OS pick; set an explicit local IP on a multi-homed machine. |
| **Loopback** | on | Deliver our own PDUs back to sockets on this machine. **Keep this on when ScenarioEditor and DISBrowser run on the same PC.** |

Pick the **UDP Multicast** radio button, then **Start**. Settings are saved per scenario in the `[Output]` section and in `settings.ini`.

**Check it before a run.** **Network → Test Multicast Send** transmits a single Entity State PDU to the configured group and reports the result — bytes sent and the full destination on success, or the actual Winsock error on failure (bad interface IP, TTL out of range, group unreachable). It also refuses a group outside `224.0.0.0/4` rather than silently degrading to a unicast send.

**Why `224.252.0.1`.** The default sits in the DIS administrative scope `224.252.0.0 – 224.255.255.255` (RFC 2365 §6.3), and its low octet matches the default **Exercise ID** of `1` — the convention DISBrowser checks and warns about (IEEE 1278.2 §6.4.7a). Any group in `224.0.0.0/4` works, but staying in that range keeps DISBrowser quiet and matches its documented default. If you change the Exercise ID, change the last octet to match.

**Set up the receiving side.** DISBrowser ships configured for **broadcast**, not multicast — it will not receive a multicast stream until you edit `DISBrowser/Config/DISBrowser.ini`:

```ini
[DIS]
ListenAddress=0.0.0.0          ; not 127.0.0.1 - must bind all interfaces
ListenPort=3001                ; match ScenarioEditor
Profile=IPv4Multicast          ; was IPv4Broadcast
MulticastGroups=224.252.0.1    ; uncomment; match ScenarioEditor's group
MulticastInterface=0.0.0.0
MulticastLoopback=true         ; needed when both apps are on one machine
```

Without `Profile=IPv4Multicast` **and** a non-empty `MulticastGroups`, DISBrowser never calls `JoinMulticastGroup`, so it binds the right port and receives nothing — which looks exactly like "the exercise isn't running."

**When nothing arrives**, check in this order: loopback off while both apps share a machine → TTL too low for the hops involved → port mismatch → receiver never joined the group → an explicit **Interface** naming a NIC that isn't on the multicast path → firewall. `Test Multicast Send` distinguishes "we couldn't even transmit" from "we transmitted and nobody listened."

#### Using TCP direct connection

UDP is fire-and-forget: a dropped datagram is simply gone. TCP gives you a reliable, ordered stream to **one** peer — useful across a flaky link, or when you need to know the receiver actually got every PDU. The trade-off is that it is point-to-point, so only one viewer can consume it.

**Wire format.** The connection carries raw DIS PDUs back to back with **no extra framing** — each PDU's header Length field (bytes 8–9) delimits it. The bytes are identical to what the UDP modes emit, so switching transports changes nothing about the PDUs themselves.

**Set it up.** Pick **TCP Direct Connection** on the Run tab, then fill in **TCP Settings**:

| Field | Default | What it does |
| --- | --- | --- |
| **Connect to Remote Server** / **Listen for Client** | Connect | Which end dials. Connect = ScenarioEditor is the client (the normal pairing with DISBrowser). |
| **Remote host / Remote port** | `127.0.0.1` / `3002` | Client mode: where the receiver is listening. Must match DISBrowser's `[DIS] TcpListenPort`. |
| **Listen port** | `3002` | Server mode: local port ScenarioEditor accepts one client on. |
| **Timeout (ms)** | `3000` | How long to wait for the connect (client) or for a client to arrive (server). |
| **Reconnect** | on | Client mode: re-dial once if the link drops mid-run. |

**Enable the receiving side.** DISBrowser's TCP feed is off by default. In `DISBrowser/Config/DISBrowser.ini`:

```ini
[DIS]
TcpEnabled=true
TcpListenPort=3002     ; must match ScenarioEditor's Remote port
```

DISBrowser is the **server** — start it first, then start ScenarioEditor. Its TCP listener runs *alongside* the UDP one, so a viewer can take multicast and a direct TCP feed at the same time; the Exercise ID filter applies to both.

**Check it** with **Network → Test TCP Connection**: it establishes the link, sends one Entity State PDU, and reports the outcome — including the real Winsock error if the connect is refused (nothing listening on that port) or times out.

**Ordering matters, unlike UDP.** With TCP the receiver must be listening *before* the sender connects; there is no equivalent of shouting into a multicast group nobody joined. A refused connection at Start is reported as an error and playback stops rather than running silently into nowhere.

**Ready-made scenario:** `plays/TCP-harburtField4.ini` is the Harburt Field 4 play preconfigured for TCP client mode against `127.0.0.1:3002`.

#### Choosing a terrain server — `settings.ini` `[Terrain]`

Three modes, switched by hand in `settings.ini` next to the executable:

```ini
[Terrain]
Mode=Local              ; Legacy | Local | Remote  (unknown values fall back to Local)
RemoteHost=127.0.0.1    ; Remote mode only
RemotePort=8088         ; Remote mode only
```

| Mode | Server | Builds | Tiles |
| --- | --- | --- | --- |
| **Local** (default) | `TerrainServer.exe`, supervised — no WSL, no console window | supervised: piped output, real exit codes, no orphans | published to `%LOCALAPPDATA%\DISBrowser\terrain\tiles` after each build |
| **Legacy** | `serve_terrain.cmd` → `serve.py` in WSL, in its own console window | original fire-and-forget `ShellExecute` | served straight from ext4; no publish step |
| **Remote** | none — another machine serves them | supervised | not published locally |
| **Service** | `terrainserver`, an independent container on **8089** — serves tiles *and* builds them | submitted to its HTTP job API; progress polled back into the status line | held by the service; no publish step |

**Legacy is a true fallback**, not a half-measure: it restores the original code path wholesale, including the fire-and-forget builds. That means failures become invisible again and children can outlive the editor — the things Local mode exists to fix. Use it only if the newer path misbehaves.

**Remote** runs nothing locally. Start/Stop Terrain Server explain that and do nothing; the editor only checks the remote address answers, and **`RemoteHost:RemotePort` is what gets written into DISBrowser's `Startup.ini`** as `CesiumTilesetUrl`, so the viewer fetches terrain from that machine. (That URL is only emitted when the basemap is *Cesium 3D (self-hosted)*.)

Legacy and Local both use `localhost:8088`. `RemoteHost`/`RemotePort` are consulted in **Remote** and **Service** modes; Service defaults the port to **8089** so the container and a native `TerrainServer.exe` can run side by side during the transition.

#### Service mode — the containerized terrain service

```ini
[Terrain]
Mode=Service
RemoteHost=127.0.0.1
RemotePort=8089
```

The server is [`terrainserver`](../../../home/sebas/projects/planeswalker/terrainserver) running in WSL Docker (or on an EC2 Ubuntu box). It both **serves** tiles and **builds** them: **Build 3D Terrain** and **Purge Terrain** POST a job to `/api/v1/jobs` instead of running the WSL `.cmd` scripts here, and the editor polls `/api/v1/jobs/{id}?since=N` so the status line and the failure dialog behave exactly as they do for a local build.

The editor never starts or stops it — the container has `restart: unless-stopped` and outlives every editor session. Start it with `./scripts/up.sh` in that directory; **Start/Stop Terrain Server** say so rather than pretending.

Because the service holds its own tiles, there is no publish step, and **Purge** does *not* touch `%LOCALAPPDATA%\DISBrowser	errain	iles` — that copy belongs to the Local fallback, and wiping it would destroy the thing you fall back to.

#### Terrain and foliage builds

These used to be fire-and-forget `ShellExecute` calls into `.cmd` files: output went to a console window you had to find, a script that failed looked identical to one that succeeded, and nothing could stop a child that was left running. In **Local and Remote** modes it now goes through **`ProcessSupervisor`** (`src/ProcessSupervisor.{h,cpp}`) — Legacy mode keeps the old behaviour:

- **You see what happened.** stdout and stderr are piped back line by line into the status line under the preview and into the app log. A non-zero exit code raises a dialog containing the script's own last output — failures can no longer hide.
- **Nothing is orphaned.** Builds run inside a Job Object with `KILL_ON_JOB_CLOSE`, so the whole tree (`wsl.exe`, `docker`, python) dies with the editor even if it crashes.
- **One at a time.** The Build/Purge buttons disable while a job runs; they all write the same tile tree, so concurrent runs would corrupt it.

**Where tiles live.** Tiling always happens on ext4 inside WSL — that is where Docker and `ctb-tile` are fastest. In **Local** mode the finished tiles are then published to `%LOCALAPPDATA%\DISBrowser\terrain\tiles` by `DISBrowser/Scripts/wsl/publish_tiles.sh`, which the editor runs automatically after a successful build; they are **gunzipped during the copy**, so the server just streams bytes. Legacy mode skips the publish (its `serve.py` reads ext4 directly), and Remote mode skips it too (those tiles live on another machine).

This copy exists because a Windows process cannot read `/root/terrain/tiles`: the `\\wsl$` share depends on `P9RdrService`, which is disabled-by-default and needs admin to start — precisely the kind of fragility this rework removes.

**The terrain server survives the editor.** `TerrainServer.exe` is deliberately started *outside* the job object, so closing the editor leaves a running DISBrowser with its terrain intact. Stop it explicitly with **Stop Terrain Server**, or from Task Manager.

> The legacy `serve_terrain.cmd` / `serve.py` still work if you want to run the server by hand; the editor no longer uses them.

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
- **Validation** — a validator enforces DIS rules (marking length, timestamps…) and estimates output bandwidth. Note it does **not** yet check the `[Output]` section; the multicast group range is checked by **Network → Test Multicast Send**, not at Start.
- **Terrain-server controls** — Start/Stop the self-hosted terrain tile server and gate the boundary/build tools on it (Preview tab). The server is **`TerrainServer.exe`**, a native tile server built alongside the editor — no WSL and no console window. See [Terrain and foliage builds](#terrain-and-foliage-builds).

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
