# ScenarioEditor

Form-based DIS scenario authoring tool. Users define entities, motion paths,
timing, and output targets through tabs and dialogs; the application generates,
records, replays, and streams **Distributed Interactive Simulation (DIS) PDUs**
over UDP, multicast, TCP, or `.disrec` files.

ScenarioEditor is the **authoring half** of a two-app pair. Its companion is
**DISBrowser** (Unreal Engine 5.7, sibling repo at `..\DISBrowser`), which
consumes the same DIS streams and renders them in 3D. The two apps communicate
only over the wire — no shared library, no private side-channel.

> Full specification: **[spec.mk](spec.mk)** — the authoritative source.
> This README is an orientation page only.

---

## Tech stack

| Concern | Choice |
|---|---|
| UI framework | MFC, **dialog-based** (not SDI). Main dialog hosts a tab control with five child dialogs. |
| DIS serialization | **Open-DIS C++** (https://github.com/open-dis/open-dis-cpp), wrapped behind a thin façade |
| Preview rendering | **Direct2D + DirectWrite** from v1 |
| Networking | Winsock2 directly (`Ws2_32.lib`) — UDP unicast/multicast, TCP client/server |
| Threading | **`std::thread`** + `::PostMessage` to the main dialog HWND. Workers never touch MFC objects. |
| Build | Visual Studio 2022, x64, `.sln` / `.vcxproj`. CMake is only used to build Open-DIS itself. |
| Language | C++17 minimum, C++20 acceptable |

---

## Project layout

```
ScenarioEditor/
├── ScenarioEditor.sln
├── ScenarioEditor.vcxproj
├── spec.mk                     ← full specification (read this)
├── README.md                   ← you are here
├── log.h / log.cpp             ← LOG(x) macro - the only logger
├── EntityTypeCatalog.ini       ← curated SISO-REF-010 subset for combo pickers
├── settings.ini                ← user UI state (window pos, recent files); written on close
├── external/
│   └── open-dis-cpp/           ← pinned-commit submodule, built as static lib
└── output/                     ← all user scenarios live here
    ├── DronePatrolDemo/
    │   ├── scenario.ini        ← canonical scenario definition
    │   ├── ephemerals/         ← cached generated PDU buffers (reproducible)
    │   ├── recordings/         ← .disrec files (user-precious; never auto-deleted)
    │   └── exports/            ← CSV / KML / etc.
    └── HighwayConvoy/
        └── scenario.ini
```

The Scenario Name typed in the editor **is** the directory name. Save = write
`scenario.ini` inside it. Save As = create a new directory and copy.

---

## Conventions newcomers get wrong (read this section)

| Field | Convention | Why |
|---|---|---|
| **Local coordinates** | **ENU** (East/North/Up), tangent plane at scenario origin. Not NED. | Matches DISBrowser, KML, Cesium, Unreal Georeferencing. NED is explicitly rejected. (spec §15.2) |
| **Altitude** | **WGS-84 ellipsoidal** everywhere. Not MSL. | DIS ECEF math assumes ellipsoidal. MSL→ellipsoidal conversion is a user-side boundary concern. (§15.1.1) |
| **Heading** | 0° = North, increases clockwise (compass). Range [0, 360). | Standard sim convention. Mapped to DIS `psi` at PDU boundary. (§7.3.1) |
| **Pitch / Roll** | Pitch+: nose up. Roll+: right wing down. | Same. (§7.3.1) |
| **DIS timestamps** | **Relative** (bit 0 = 0). Always. No absolute-time mode in v1/v2. | Recordings remain valid across hosts and clocks. (§14.4.1) |
| **DIS orientation** | psi/theta/phi in **radians**, entity-to-world rotation. Editor stores degrees; converts at PDU boundary. | IEEE 1278.1 §B.1.6.4. (§14.5.1) |
| **Marking text** | ASCII only, 11 bytes, null-padded (not null-terminated). Non-ASCII stripped with warning at PDU boundary. | DIS standard. (§14.5.1, §20.2) |
| **Dead reckoning** | Algorithm 1 (Static), 2 (DRM F,P,W), or 5 (DRM F,V,W) — chosen per motion segment type by the engine. Analytic derivatives only, never finite differences. | Smooth motion at receiver from low-Hz streams. (§16.4) |
| **Multicast IPs** | Must be in `224.0.0.0/4`. `224.0.0.0/24` is a warning (link-local, not routed). | RFC 5771. (§20.2) |
| **Logging** | **Always** use `LOG(x)` from `log.h`. Never `printf`, `cerr`, `OutputDebugString`, `TRACE`. | One log, one place. (§17.6) |
| **Error policy** | Fail-fast on unrecoverable (socket open, file open, header write, disk full); drop-and-count on transient UDP send failures. Never silent. | (§17.7) |
| **UI ↔ worker** | UI passes an immutable `RuntimeScenarioSnapshot` to the worker on Start. UI can edit during Run; edits don't affect the running scenario. Stop and Start to apply. | Eliminates the entire UI/worker race class. (§18.0) |

---

## What ScenarioEditor depends on outside this repo

- **DISBrowser** must implement DIS dead reckoning (algorithms 1, 2, 5) to render
  low-Hz streams smoothly. Currently it does not — it lerps toward the last
  received position. Tracked in spec §1.3.
- **DISBrowser parser migration to Open-DIS** is the long-term goal so a single
  library defines "what the bytes mean" for both apps. Currently DISBrowser
  uses a hand-rolled parser; the §14.5.3 conformance tests cross-validate.

---

## Versioned scope

- **V1** — UI shell only. Main dialog, menu, toolbar, tabs, child dialogs,
  status bar, placeholder buttons. No DIS code behind anything yet.
- **V2** — `.ini` scenario load/save, motion engine (stationary/stop/line/ellipse,
  circle/waypoints later), Entity State PDU generation via Open-DIS, UDP unicast +
  multicast output, `.disrec` recording and replay, validation panel,
  worker-thread playback with start/pause/resume/stop/loop.
- **Later** — TCP completion, additional PDU types, full map preview, PCAP
  export, receive mode. See §23.3.

---

## Where to read next

| Question | Section |
|---|---|
| What does each tab do? | §6 (Setup), §7 (Entity), §8 (Motion), §9 (Output), §10 (Preview) |
| How are scenarios stored on disk? | §11 |
| What does the `.disrec` format look like? | §13 |
| How is DIS serialization wired? | §14 (Open-DIS façade) |
| What dead-reckoning algorithm does each motion type use? | §16.4 |
| What validation rules fire? | §20.2 |
| What is the threading model? | §19 |
| How do I add a new entity type to the picker? | Edit `EntityTypeCatalog.ini` and restart (§7.5) |
