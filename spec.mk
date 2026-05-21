# ScenarioEditor - Technical Specification

## 1. Project Summary

**Project Name:** ScenarioEditor  
**Solution Name:** ScenarioEditor.sln  
**Project File:** ScenarioEditor.vcxproj  
**Executable Name:** ScenarioEditor.exe  
**Application Type:** Native Windows desktop application  
**Primary Language:** C++  
**UI Framework:** Microsoft Foundation Classes (MFC) with Win32 SDK integration  
**Networking:** Winsock2 directly  
**Primary Purpose:** Form-based DIS scenario authoring, generation, recording, playback, and network streaming

ScenarioEditor is a form-based C++ desktop application that lets users define simulated entities, movement paths, timing, coordinate systems, and output destinations. The application generates, records, and replays Distributed Interactive Simulation (DIS) Protocol Data Unit (PDU) streams over UDP unicast, UDP multicast, TCP direct connection, or custom timed file playback.

The application is not a question-and-answer wizard. All scenario data is entered, edited, validated, previewed, saved, generated, recorded, and replayed through structured forms, tables, trees, and tabbed dialogs.

### 1.1 Companion Application - DISBrowser

ScenarioEditor is the **authoring half** of a two-application system. Its companion is **DISBrowser**, a separate Unreal Engine application (sibling project at `..\DISBrowser`) that consumes DIS PDU streams and renders the resulting scenario in 3D. ScenarioEditor produces scenarios; DISBrowser visualizes them.

```text
+------------------+    DIS PDUs over UDP/TCP    +-----------------+
|                  |   or .disrec file replay    |                 |
|  ScenarioEditor  | --------------------------> |   DISBrowser    |
|   (this app)     |                             | (Unreal Engine) |
|   MFC / C++      |                             |    UE 5.7       |
+------------------+                             +-----------------+
```

The two applications communicate **only via on-the-wire DIS PDUs and the `.disrec` file format**. There is no shared library, no shared process, no editor-specific extension to the protocol, and no private side-channel. The wire format is the contract.

This split is deliberate:

- DISBrowser can receive DIS streams from any third-party DIS source (Open-DIS publishers, VR-Forces, CIGI gateways, etc.), not just ScenarioEditor.
- ScenarioEditor can drive any third-party DIS-compliant viewer, not just DISBrowser.
- Any feature that requires a private channel between editor and viewer is out of scope for this spec.

### 1.2 Implications for this Spec

Because DISBrowser is the primary visual consumer of ScenarioEditor's output, the following decisions are weighted accordingly:

- **DIS v7 is the default and primary target** (§6.3), because that is what DISBrowser is expected to consume. DIS v6 is supported but secondary.
- **ECEF is the internal canonical position frame** (§15.5), because Unreal Engine large-world coordinates and DISBrowser's georeferencing pipeline are easier to align against ECEF than against arbitrary local origins.
- **Entity Type fields must be valid SISO-REF-010 values** (§7.3, §20.2), because DISBrowser uses Kind/Domain/Country/Category to look up visual assets. Garbage type fields produce missing or wrong models in the viewer, not validation errors at the editor.
- **Entity Marking text** (§14.5) is used by DISBrowser for on-screen labels. Truncation or character-set surprises here are user-visible.
- **The `.disrec` recording format** (§13) must be readable by DISBrowser's replay path, not just by ScenarioEditor itself. Format decisions that benefit only the editor are non-goals.

The ScenarioEditor Preview tab (§10) is intentionally a lightweight 2D sanity-check view, not a replacement for DISBrowser. Full 3D visualization is DISBrowser's responsibility — the editor's job is to produce a correct stream, not to render it.

### 1.3 Cross-Project Dependencies and Tracking Items

The following items live outside this spec but the editor's behavior depends on them. They are listed here so they cannot be forgotten when ScenarioEditor ships:

- **DISBrowser dead-reckoning implementation.** DISBrowser currently does not implement DIS dead reckoning (verified by code scan: `DISPDUParser.cpp` skips the 40-byte DR block; `DISEntityActor::Tick` lerps toward the last received position without extrapolation). ScenarioEditor's motion engine (§16.4) emits algorithms 1, 2, and 5 with correct linear velocity and acceleration on the assumption that DISBrowser will dead-reckon between PDUs. Until DISBrowser implements DR algorithms 1/2/5, low-Hz output streams will look polygonal in DISBrowser even though the wire format is correct. **Owner:** DISBrowser project. **Scope estimate:** 1-2 days (algorithms 1, 2, 5 + tick-loop extrapolation + new-PDU blend).
- **DISBrowser parser migration to Open-DIS.** DISBrowser uses a hand-rolled parser; ScenarioEditor uses Open-DIS C++ (§14.1). Two implementations of the same wire format is a known drift risk. The §14.5.3 conformance tests include a cross-implementation byte round-trip through DISBrowser's parser to detect divergence. Long-term goal: migrate DISBrowser to Open-DIS so a single library defines "what the bytes mean" for both apps.

### 1.4 Localization

English (`en-US`) only. All UI strings, menu labels, validation messages, and log output are hard-coded English. No resource-DLL localization layer in v1/v2. If localization becomes a requirement later it is a structural refactor, not a future enhancement bullet.

---

## 2. Core Goals

The application shall allow users to:

1. Create complete DIS simulation scenarios using forms.
2. Select DIS protocol version per scenario.
3. Define scenario-level metadata, timing, coordinate system, and exercise information.
4. Define multiple simulated assets/entities.
5. Organize assets/entities in a tree view by force, type, domain, or user-defined grouping.
6. Edit selected asset/entity details in a detail panel.
7. Define motion paths using timeline-based motion segments.
8. Preview scenario motion visually before sending or recording.
9. Generate DIS PDUs using internal C++ serialization classes.
10. Send generated PDUs live over UDP unicast, UDP multicast, or TCP.
11. Record generated PDUs to a custom `.disrec` file.
12. Replay `.disrec` files to UDP, multicast, or TCP outputs.
13. Support playback controls such as start, pause, resume, stop, loop, and playback speed.
14. Validate scenario data before generation or playback.
15. Bulk-generate and bulk-read ephemeral PDU stream data for efficient recording and playback.

---

## 3. Target Platform

### 3.1 Operating System

- Windows 10, 64-bit
- Windows 11, 64-bit

### 3.2 Development Environment

- Visual Studio 2022
- C++17 minimum
- C++20 acceptable if project constraints allow
- MFC enabled Visual C++ project
- Windows SDK
- Winsock2

### 3.3 Build System

The project shall use **Visual Studio 2022 `.sln` / `.vcxproj` files only**.

CMake is out of scope for the initial project specification. The application shall be created as a traditional Visual Studio 2022 **MFC dialog-based application** with a tab control hosting child dialog pages. This avoids the SDI Document/View overhead — there is no meaningful "document" to serialize through `CDocument` (scenarios already round-trip through INI), and there is no "view" abstraction earning its keep (the entire UI is forms and tabs). A dialog-based shell gives the same access to the MFC resource editor, message maps, menus, toolbars, and status bars without the SDI lifecycle.

MFC dialog-based provides:

- Resource editor dialogs
- Dialog pages hosted in a tab control
- Menus and accelerators on the main dialog
- Toolbar and status bar on the main dialog
- Message maps
- Win32 SDK integration

Recommended solution structure:

```text
ScenarioEditor.sln
└── ScenarioEditor.vcxproj
    ├── MFC Dialog-Based Application
    ├── Use Unicode Character Set
    ├── x64 Debug configuration
    ├── x64 Release configuration
    ├── Windows SDK
    ├── MFC enabled (Shared DLL or Static, project preference)
    ├── Winsock2 linked through Ws2_32.lib
    ├── Direct2D linked through D2d1.lib and Dwrite.lib (Preview tab, §10.5)
    └── Open-DIS C++ linked as static library (§14)
```

---

## 4. Application Architecture

### 4.1 High-Level Architecture

```text
ScenarioEditor
│
├── MFC UI Layer
│   ├── Main Dialog (CDialogEx)
│   ├── Tab Control
│   ├── Child Dialog Pages (one per tab)
│   ├── Menu Bar / Toolbar / Status Bar
│   ├── Tree Views
│   ├── List Controls
│   ├── Property Editors
│   └── Direct2D Preview Canvas
│
├── Scenario Model Layer
│   ├── Scenario Definition
│   ├── Asset / Entity Definitions
│   ├── Motion Segment Definitions
│   ├── Coordinate Definitions
│   ├── Output Definitions
│   └── Validation Results
│
├── Scenario Storage Layer
│   ├── INI Scenario Reader
│   ├── INI Scenario Writer
│   └── Scenario Import/Export Helpers
│
├── Motion Engine
│   ├── Stationary Motion
│   ├── Linear Motion
│   ├── Circular Motion
│   ├── Ellipse Motion
│   ├── Waypoint Motion
│   ├── Stop/Hold Segments
│   └── Orientation/Velocity Calculation
│
├── Coordinate Engine
│   ├── Lat/Lon/Alt
│   ├── Local X/Y/Z Relative to Origin
│   ├── ECEF
│   └── Coordinate Conversion Utilities
│
├── DIS PDU Layer
│   ├── DIS v6 Serializer
│   ├── DIS v7 Serializer
│   ├── PDU Base Classes
│   ├── Entity State PDU
│   ├── Create Entity PDU
│   ├── Remove Entity PDU
│   └── Future Event PDUs
│
├── Ephemeral Stream Layer
│   ├── Generated PDU Buffer
│   ├── Timed PDU Records
│   ├── Bulk Write Support
│   └── Bulk Read Support
│
├── Recording / Replay Layer
│   ├── .disrec Writer
│   ├── .disrec Reader
│   ├── Playback Clock
│   ├── Replay Scheduler
│   └── Loop / Speed Control
│
├── Network Layer
│   ├── Winsock2 Initialization
│   ├── UDP Unicast Sender
│   ├── UDP Multicast Sender
│   ├── TCP Client Sender
│   ├── TCP Server Sender
│   └── Socket Configuration
│
└── Worker Thread Layer
    ├── Live Generation Thread
    ├── File Recording Thread
    ├── Replay Thread
    ├── Network Send Queue
    └── UI Status Notifications
```

---

## 5. User Interface Design

### 5.1 Application Style

The application shall be an **MFC dialog-based application** (`CDialogEx`-derived main dialog) hosting a tab control with one child dialog per tab.

The main dialog shall include:

- Menu bar (attached to the dialog via `SetMenu`)
- Toolbar (this is also the command strip — see §5.3)
- Status bar (multi-pane: state, time, validation counts, bandwidth)
- Tab control
- Active child dialog page

### 5.2 Main Tab Control

The main tab control shall contain five tabs:

```text
1. Scenario Setup
2. Asset / Entity Editor
3. Motion Path Editor
4. Output / Playback
5. Preview
```

Each tab shall be implemented as an MFC dialog page hosted inside the main form view.

Recommended class structure:

```cpp
class CScenarioEditorDialog : public CDialogEx
{
    CMenu       m_menu;
    CToolBar    m_toolBar;        // also serves as the command strip (§5.3)
    CStatusBar  m_statusBar;      // multi-pane: state, time, errors, bw
    CTabCtrl    m_tabCtrl;
    CScenarioSetupPage      m_scenarioSetupPage;
    CAssetEntityEditorPage  m_assetEntityEditorPage;
    CMotionPathEditorPage   m_motionPathEditorPage;
    COutputPlaybackPage     m_outputPlaybackPage;
    CPreviewPage            m_previewPage;
};
```

Each child page is a `CDialogEx`-derived dialog with `WS_CHILD | WS_VISIBLE`, positioned inside the tab control's display rect. Switching tabs hides the outgoing page and shows the incoming one.

---

### 5.3 Command Toolbar

The main dialog shall have **one** command bar — the MFC toolbar — that combines what some specifications call a "toolbar" and a "command strip" into a single row. There is no separate command/control strip. The toolbar remains visible regardless of which tab is selected.

The toolbar is organized into logical groups separated by separators:

```text
[New][Open][Save] | [Validate][Preview][Generate] | [Record][Replay File][Send Live] | [Start][Pause][Resume][Stop] | [ ]Loop  Speed:[1x v]
```

Toolbar button groups:

```text
Group 1 - File:          New | Open | Save
Group 2 - Scenario ops:  Validate | Preview | Generate
Group 3 - Output:        Record | Replay File | Send Live
Group 4 - Transport:     Start | Pause | Resume | Stop
Group 5 - Loop / Speed:  Loop checkbox | Speed combo (0.25x, 0.5x, 1x, 2x, 10x)
```

The toolbar should be implemented as a `CMFCToolBar` (modern MFC) or `CToolBar` with custom controls hosted in dummy buttons for the Loop checkbox and Speed combo box.

### 5.4 Status Bar - Always-Visible State

Scenario state, time, validation counts, and bandwidth live in the **multi-pane status bar** rather than in a separate strip. The status bar is visible at all times regardless of the active tab.

Status bar panes (left to right):

```text
| State: Idle | Time: 000.000s | Progress: 12% | Errors: 0 | Warnings: 0 | Info: 0 | PDUs: 0 | BW: 0.00 Mbps |
```

State values: `Idle / Running / Paused / Stopped / Completed / Error` (matches §19.4 `PlaybackState` enum).

Clicking the Errors/Warnings/Info pane shall open the full Validation Panel (§5.5).

### 5.5 Main Window Layout

The initial UI shall be laid out before any code-behind logic is implemented. The first implementation milestone shall focus on resources, dialog pages, control placement, tab switching, and placeholder command buttons only.

Final main-dialog layout:

```text
+--------------------------------------------------------------------------------+
| Menu Bar: File | Scenario | Playback | Network | View | Help                   |
+--------------------------------------------------------------------------------+
| Toolbar (= command bar)                                                        |
| [New][Open][Save] | [Validate][Preview][Generate] | [Record][Replay][Send]     |
| [Start][Pause][Resume][Stop] | [ ]Loop  Speed[1x v]                            |
+--------------------------------------------------------------------------------+
| Tab Control                                                                    |
| | Scenario Setup | Asset / Entity Editor | Motion Path Editor |               |
| | Output / Playback | Preview |                                                |
| +----------------------------------------------------------------------------+ |
| |                                                                            | |
| | Active child dialog page                                                   | |
| |                                                                            | |
| +----------------------------------------------------------------------------+ |
+--------------------------------------------------------------------------------+
| Status Bar (multi-pane)                                                        |
| State:Idle | Time:000.000s | Prog:0% | Err:0 | Warn:0 | Info:0 | BW:0.00Mbps  |
+--------------------------------------------------------------------------------+
```

### 5.6 Full Validation Panel Placement

The full validation panel shall be accessible from the status bar's Errors/Warnings/Info pane and may be implemented as either:

1. A collapsible panel docked above the status bar.
2. A modeless validation window launched from the status-bar pane click.

Recommended v1 UI layout:

```text
+--------------------------------------------------------------------------------+
| Validation Results                                                              |
| Severity | Source | Message | Suggested Fix                                  |
| Error    | Entity | Duplicate Entity ID 1.1.101 detected.                         |
| Warning  | Output | Update rate may generate high bandwidth.                       |
| Info     | Output | Estimated PDU count: 4500.                                      |
+--------------------------------------------------------------------------------+
```

The validation panel shall include columns:

```text
Severity
Source Tab
Entity / Segment / Output Name
Message
Suggested Fix
```

---

## 6. Tab 1 - Scenario Setup

### 6.1 Purpose

The Scenario Setup tab captures global scenario metadata, DIS version selection, timing, coordinate mode, and scenario origin.

### 6.2 Controls

Recommended MFC/Win32 controls:

```text
Group Box: Scenario Identification
    Static: Scenario Name (REQUIRED - maps to output/<Name>/ directory, see §11.1)
    Edit: IDC_EDIT_SCENARIO_NAME (filesystem-safe; rename here renames directory on Save)
    Static: Description
    Multiline Edit: IDC_EDIT_SCENARIO_DESCRIPTION

Group Box: DIS Protocol
    Radio Button: IDC_RADIO_DIS_V6
    Radio Button: IDC_RADIO_DIS_V7
    Static: Default selected value shall be DIS v7.

Group Box: Exercise / Application IDs
    Static + Numeric Edit: Exercise ID
    Static + Numeric Edit: Site ID
    Static + Numeric Edit: Application ID

Group Box: Timing
    Date Time Picker: Scenario Start Time
    Static + Numeric Edit: Duration Seconds
    Static + Numeric Edit: Default Update Rate Hz

Group Box: Default Coordinate Mode
    Combo Box: Lat/Lon/Alt, Local X/Y/Z, ECEF

Group Box: Scenario Origin
    Static + Edit: Origin Latitude Degrees
    Static + Edit: Origin Longitude Degrees
    Static + Edit: Origin Altitude Meters
    Button: Use Current Entity As Origin
    Button: Clear Origin

Group Box: Validation Options
    Check Box: Validate Before Send
    Check Box: Validate Before Record
    Check Box: Validate Before Replay
```

Suggested page layout:

```text
+--------------------------------------------------------------+
| Scenario Identification                                      |
| Name [______________________________]                        |
| Description                                                  |
| [__________________________________________________________] |
| [__________________________________________________________] |
+--------------------------------------------------------------+
| DIS Protocol        | Exercise / Application IDs             |
| ( ) DIS v6          | Exercise ID [___] Site [___] App [___] |
| (o) DIS v7          |                                         |
+--------------------------------------------------------------+
| Timing                                                       |
| Start Time [ date/time picker ] Duration [____] Rate [____]  |
+--------------------------------------------------------------+
| Coordinate Mode / Origin                                     |
| Mode [Lat/Lon/Alt v]                                         |
| Lat [________] Lon [________] Alt [________]                 |
| [Use Current Entity As Origin] [Clear Origin]                |
+--------------------------------------------------------------+
| Validation Options                                           |
| [x] Validate Before Send [x] Before Record [x] Before Replay |
+--------------------------------------------------------------+
```

### 6.3 DIS Version Behavior

The user shall select DIS protocol version per scenario using radio buttons.

Initial recommended behavior:

```text
Default selected: DIS v7
Alternative: DIS v6
```

The selected DIS version determines:

- PDU field layout
- Serialization behavior
- Header format
- Supported PDU types
- Version field values

### 6.4 Scenario Origin

The scenario origin is required when any entity or motion segment uses local coordinates.

The origin shall be defined as:

```text
Origin Latitude Degrees
Origin Longitude Degrees
Origin Altitude Meters
```

Local X/Y/Z values shall be interpreted relative to this origin.

---

## 7. Tab 2 - Asset / Entity Editor

### 7.1 Purpose

The Asset / Entity Editor tab lets users define simulated DIS entities/assets.

The tab shall use a split layout with a left tree and right entity detail panel.

Required MFC/Win32 controls:

```text
Tree Control: Asset hierarchy
Button: Add Asset
Button: Delete Asset
Button: Duplicate Asset
Button: Move Asset
Button: Validate Asset

Right-side tab or grouped detail area:
    Group Box: General
    Group Box: DIS Entity Type
    Group Box: Identity
    Group Box: Initial State
    Group Box: Timing
    Group Box: Appearance / Marking
```

Suggested page layout:

```text
+----------------------------------------------------------+
| Asset Tree                         | Entity Detail Panel |
|                                    |                     |
| Friendly                           | Name                |
|   Air                              | Entity IDs          |
|     Drone 1                        | Entity Type         |
|     Fighter 1                      | Force ID            |
|   Ground                           | Marking             |
|     Radar Truck                    | Appearance          |
| Opposing                           | Timing              |
|   Ground                           | Update Rate         |
|     Target Tank                    | Initial State       |
+----------------------------------------------------------+
```

### 7.2 Asset Tree

The left side shall use a tree control.

Recommended grouping options:

- Force
- Domain
- Entity kind
- User-defined group

Example tree:

```text
Scenario
├── Friendly
│   ├── Air
│   │   ├── Drone 1
│   │   └── Fighter 1
│   └── Ground
│       └── Radar Truck
└── Opposing
    └── Ground
        └── Target Tank
```

### 7.3 Entity Detail Panel

The right side shall show editable fields for the selected entity.

Recommended fields:

#### General

- Enabled
- Name
- Description
- Marking Text
- Force ID
- Entity Kind
- Domain
- Country
- Category
- Subcategory
- Specific
- Extra

#### Identity

- Site ID
- Application ID
- Entity ID

#### Initial State

- Coordinate Mode
  - Lat/Lon/Alt
  - Local X/Y/Z
  - ECEF
- Latitude
- Longitude
- Altitude meters
- Local X meters
- Local Y meters
- Local Z meters
- ECEF X meters
- ECEF Y meters
- ECEF Z meters
- Heading degrees     (see §7.3.1 for sign convention)
- Pitch degrees       (see §7.3.1 for sign convention)
- Roll degrees        (see §7.3.1 for sign convention)
- Initial Speed meters/second

### 7.3.1 Euler Angle Conventions (Heading / Pitch / Roll)

All user-facing heading, pitch, and roll fields use the **standard DIS simulation conventions**, expressed in the **local ENU tangent plane** at the entity's current position:

```text
Heading   0° = North, 90° = East, 180° = South, 270° = West
          Increases clockwise when viewed from above (compass convention).
          Valid range: [0, 360).

Pitch     0° = level, positive = nose up, negative = nose down.
          Valid range: [-90, +90].

Roll      0° = wings level, positive = right wing down (right roll),
          negative = left wing down (left roll).
          Valid range: [-180, +180].
```

These map to the DIS Euler triple (psi, theta, phi) at the PDU boundary via the local-ENU-to-ECEF rotation. The mapping is performed by the coordinate engine (§15.5) and the DIS serializer (§14.5.2):

```text
psi   (DIS yaw, world-Z rotation)        = headingDeg, converted to radians, sign-aligned with local North
theta (DIS pitch, intermediate-Y rotation) = pitchDeg, converted to radians
phi   (DIS roll, body-X rotation)        = rollDeg, converted to radians
```

The Tait-Bryan rotation order is the DIS-standard ZYX entity-to-world rotation per IEEE 1278.1 §B.1.6.4. The user never sees psi/theta/phi; they only see Heading/Pitch/Roll in the editor forms.

#### Timing

- Begin Time Seconds
- End Time Seconds
- Update Rate Hz

### 7.4 Entity ID Validation

Entity identity shall be unique across the scenario:

```text
Site ID + Application ID + Entity ID
```

Duplicate entity IDs shall be validation errors.

### 7.5 Entity Type Catalog - EntityTypeCatalog.ini

The combo-box pickers for **Entity Kind, Domain, Country, Category, and Subcategory** (§7.3 General) are populated from a flat INI file named **`EntityTypeCatalog.ini`** located at the project root (next to the executable). The file is a curated subset of SISO-REF-010 — it is **not** a full enumeration of the standard; it covers the entity types the user is most likely to need, and the user can extend it by editing the file.

Behavior:

- Loaded once at application startup, after `settings.ini` (§11.5) and before the Asset/Entity Editor tab is initialized.
- Missing file → log a warning via `LOG` (§17.6) and continue with empty pickers. The user can still type raw numeric values into the Kind/Domain/Country/Category/Subcategory fields; only the dropdowns are empty.
- Malformed entries → skip the bad entry, log it, keep loading. Never abort on a bad line.
- Unknown sections (anything not matching `Kind.*`, `Domain.*.*`, `Country.*`, `Category.*.*.*`, or `Subcategory.*.*.*.*`) → ignored, preserved on round-trip-write if the app ever writes it back (v1/v2 does not write this file).
- Hot-reload is **not** supported — restart the app to pick up edits.

Format:

```ini
[Format]
Version=1
Source=SISO-REF-010 subset (curated for ScenarioEditor v1)

[Kind.<id>]                Name=...
[Domain.<kind>.<id>]       Name=...
[Country.<id>]             Name=...
[Category.<kind>.<domain>.<id>]                          Name=...
[Subcategory.<kind>.<domain>.<category>.<id>]            Name=...
```

The numeric IDs match the DIS Entity Type wire fields (§14.5). A sample catalog ships at `EntityTypeCatalog.ini` in the project root with representative entries for U.S. / NATO / Russian / Chinese aircraft, tanks, ships, and UAVs.

Picker UX behavior:

- Each combo-box shows entries filtered by the parent selection. Selecting Kind=Platform filters the Domain combo to {Land, Air, Surface, Subsurface, Space}. Selecting Domain=Air filters Category to air-platform categories.
- Each combo-box is a `CBS_DROPDOWN` (not `CBS_DROPDOWNLIST`) so the user can also type a numeric value that isn't in the catalog. A typed value bypasses the catalog and is stored raw; validation (§20) accepts it but emits an info-level note that the value is not in the catalog.

Cross-reference: §1.2 requires entity types to be valid SISO-REF-010 values because DISBrowser uses Kind/Domain/Country/Category to look up visual assets. The catalog file is how ScenarioEditor enforces this without the user having to know the numeric encoding.

---

## 8. Tab 3 - Motion Path Editor

### 8.1 Purpose

The Motion Path Editor tab lets users define movement behavior for each entity using a timeline of motion segments.

Each asset shall support multiple motion segments.

Example:

```text
0-30 sec: stationary
30-120 sec: line
120-240 sec: ellipse
240-300 sec: stop
```

### 8.2 Motion Editor Layout

Required MFC/Win32 controls:

```text
Combo Box: Entity Selector
List Control: Motion Segment Timeline
Button: Add Segment
Button: Delete Segment
Button: Duplicate Segment
Button: Move Up
Button: Move Down
Button: Validate Segment
Dynamic Detail Area: selected segment fields
```

Recommended layout:

```text
+----------------------------------------------------------+
| Entity Selector: [Drone 1 v]                             |
+----------------------------------------------------------+
| Motion Segment Timeline                                  |
|----------------------------------------------------------|
| # | Enabled | Start | End | Type       | Description      |
| 1 | Yes     | 0     | 30  | Stationary | Hold at start    |
| 2 | Yes     | 30    | 120 | Line       | Fly to waypoint  |
| 3 | Yes     | 120   | 240 | Ellipse    | Orbit target     |
| 4 | Yes     | 240   | 300 | Stop       | Hold final pos   |
+----------------------------------------------------------+
| Selected Segment Detail                                  |
| ... dynamic form changes based on selected motion type ...|
+----------------------------------------------------------+
| [Add Segment] [Delete] [Move Up] [Move Down] [Validate]  |
+----------------------------------------------------------+
```

### 8.3 Supported Motion Segment Types

Initial supported motion types:

1. Stationary
2. Stop/Hold
3. Straight Line
4. Circle
5. Ellipse
6. Waypoints

Future motion types:

1. Random Patrol
2. Formation Follow
3. Intercept
4. Terrain-following
5. Scripted custom function
6. Imported telemetry track

### 8.4 Common Motion Segment Fields

Each motion segment shall include:

```text
Segment ID
Enabled
Start Time Seconds
End Time Seconds
Motion Type
Coordinate Mode
Description
```

### 8.5 Stationary / Stop Segment Fields

```text
Position Coordinate Mode
Lat/Lon/Alt or Local X/Y/Z or ECEF
Heading
Pitch
Roll
Velocity = 0
```

### 8.6 Straight Line Segment Fields

```text
Start Position
End Position
Start Time
End Time
Speed Mode:
    - Calculate from segment time
    - Use fixed speed
Heading Mode:
    - Calculate from path
    - User-defined
Altitude Mode:
    - Linear interpolation
    - Constant start altitude
    - Constant end altitude
```

### 8.7 Circle Segment Fields

```text
Center Position
Radius Meters
Start Angle Degrees
Direction:
    - Clockwise
    - Counterclockwise
Period Seconds
Altitude Mode:
    - Constant
    - Linear climb/descent
```

### 8.8 Ellipse Segment Fields

```text
Center Position
Radius X Meters
Radius Y Meters
Rotation Degrees
Start Angle Degrees
Direction:
    - Clockwise
    - Counterclockwise
Period Seconds
Altitude Mode:
    - Constant
    - Linear climb/descent
    - Sinusoidal
```

### 8.9 Waypoint Segment Fields

Waypoints shall be editable in a list control.

```text
# | Time Offset | Coordinate Mode | Lat | Lon | Alt | X | Y | Z | Speed | Hold Seconds
```

Recommended buttons:

```text
Add Waypoint
Delete Waypoint
Move Up
Move Down
Import CSV
Export CSV
```

### 8.10 Motion Sampling

The motion engine shall sample each entity's active motion segment at a specific scenario time.

```cpp
struct EntityPose
{
    double scenarioTimeSeconds;
    CoordinateMode coordinateMode;

    double latitudeDeg;
    double longitudeDeg;
    double altitudeMeters;

    double localXMeters;
    double localYMeters;
    double localZMeters;

    double ecefXMeters;
    double ecefYMeters;
    double ecefZMeters;

    double headingDeg;
    double pitchDeg;
    double rollDeg;

    double velocityXMetersPerSecond;
    double velocityYMetersPerSecond;
    double velocityZMetersPerSecond;
};
```

**Note:** the above is the v1 positional contract only. The full `EntityPose` used by the motion engine (§16.5) additionally carries the dead-reckoning algorithm, ECEF acceleration, and angular velocity — fields the receiver needs to extrapolate smoothly between PDUs. See §16.4 / §16.5 for the complete definition.

---

## 9. Tab 4 - Output / Playback

### 9.1 Purpose

The Output / Playback tab configures live output, recording, replay, timing, transport, and playback controls.

Required MFC/Win32 controls:

```text
Group Box: Output Mode
    Radio Button: UDP Unicast
    Radio Button: UDP Multicast
    Radio Button: TCP Direct Connection
    Radio Button: File Recording
    Radio Button: Preview Only

Group Box: UDP Unicast Settings
    Edit: Destination IP Address
    Edit: Destination Port
    Edit: Local Bind Address
    Edit: Local Bind Port
    Edit: Send Buffer Size
    Edit: TTL

Group Box: UDP Multicast Settings
    Edit: Multicast Group Address
    Edit: Port
    Combo Box: Network Interface
    Edit: TTL
    Check Box: Loopback Enabled

Group Box: TCP Settings
    Radio Button: Connect to Remote Server
    Radio Button: Listen for Client
    Edit: Remote Host
    Edit: Remote Port
    Edit: Local Listen Port

Group Box: File Recording / Replay
    Edit: Recording File Path
    Button: Browse Recording File
    Edit: Replay File Path
    Button: Browse Replay File
    Check Box: Bulk Write Enabled
    Check Box: Validate File Before Replay

Group Box: Playback Controls
    Button: Start
    Button: Pause
    Button: Resume
    Button: Stop
    Check Box: Loop
    Combo Box: Playback Speed
```

### 9.2 Output Modes

Supported output modes:

```text
UDP Unicast
UDP Multicast
TCP Direct Connection
File Recording
Preview Only
```

### 9.3 UDP Unicast Settings

```text
Destination IP Address
Destination Port
Local Bind Address
Local Bind Port
Send Buffer Size
TTL
```

### 9.4 UDP Multicast Settings

```text
Multicast Group Address
Port
Network Interface
TTL
Loopback Enabled
Local Bind Address
Send Buffer Size
```

### 9.5 TCP Settings

TCP mode shall support both:

```text
Connect to remote server
Listen for client
```

Fields:

```text
Remote Host
Remote Port
Local Listen Port
Reconnect Enabled
Connection Timeout
Send Buffer Size
```

### 9.6 File Recording Settings

```text
Recording File Path
Recording Format: .disrec
Bulk Write Enabled
Flush Interval
Include Timing Offsets
Include Scenario Metadata
```

### 9.7 File Replay Settings

```text
Replay File Path
Output Target
Replay Speed
Loop Enabled
Validate File Before Replay
```

### 9.8 Playback Controls

The application shall provide controls for both real-time scenario generation and file replay.

Required controls:

```text
Start
Pause
Resume
Stop
Loop
Playback Speed
```

Playback speed options:

```text
0.25x
0.5x
1x
2x
10x
```

### 9.9 Playback State Machine

```text
Idle
  └── Start → Running
Running
  ├── Pause → Paused
  ├── Stop → Stopped
  └── Complete → Completed or Looping
Paused
  ├── Resume → Running
  └── Stop → Stopped
Stopped
  └── Start → Running
Completed
  ├── Start → Running
  └── Loop → Running
```

---

## 10. Tab 5 - Preview

### 10.1 Purpose

The Preview tab provides a visual representation of the scenario before live sending or recording.

### 10.2 Initial Preview Scope

Initial preview shall support:

- 2D top-down view
- Local X/Y grid
- Entity icons or markers
- Motion paths
- Current scenario time marker
- Playback controls linked to scenario clock
- Optional labels for entity names
- Optional trail display

### 10.3 Preview Modes

The preview tab should eventually support multiple display modes:

```text
Local X/Y Grid
Lat/Lon Map Approximation
ECEF Diagnostic View
Timeline View
```

Initial version may implement Local X/Y grid first, while still storing and converting Lat/Lon/Alt and ECEF in the data model.

### 10.4 Preview Controls

```text
Preview Start
Preview Pause
Preview Resume
Preview Stop
Time Slider
Zoom In
Zoom Out
Fit Scenario
Show Labels
Show Trails
Show Paths
Show Orientation Vectors
```

### 10.5 Preview Rendering Technology

The Preview canvas shall use **Direct2D** from V1 onwards.

Rationale: Direct2D is roughly the same line count as GDI/GDI+ once the device/render-target boilerplate is in place, but it gives anti-aliased lines and curves, hardware acceleration, and smooth pan/zoom for free — all of which the preview will want immediately. Sticking with GDI now and migrating later is more total work than starting with Direct2D.

V1 implementation:

- Custom `CWnd`-derived `CPreviewCanvasWnd` hosting an `ID2D1HwndRenderTarget`
- DirectWrite (`IDWriteFactory`) for entity labels and axis text
- Render target re-created on `WM_SIZE` (Direct2D requires resize handling)
- Lazy redraw on `WM_PAINT`; invalidate on scenario-clock tick from worker thread via `PostMessage`

Link dependencies (already listed in §3.3):

```text
D2d1.lib       - Direct2D core
Dwrite.lib     - DirectWrite (text)
```

Possible future rendering options:

- Direct3D 11 (3D preview)
- Embedded map control (Bing Maps, Esri, etc.)
- Defer full 3D visualization to DISBrowser via live UDP send (§1.1) — this is the intended path

Note: The ScenarioEditor preview is a 2D sanity-check view. It is **not** a replacement for DISBrowser. If a user wants full 3D, the correct workflow is to use Send Live mode and watch in DISBrowser.

---

## 11. Scenario Storage Format

### 11.1 On-Disk Layout

All files belonging to a scenario — the scenario definition, ephemeral generated PDU buffers, recordings, and exports — live together under a single directory named after the scenario. Scenario directories are siblings under the project's `output/` root.

```text
<ProjectRoot>/
└── output/
    ├── DronePatrolDemo/
    │   ├── scenario.ini                  (canonical scenario definition - §11.2)
    │   ├── ephemerals/
    │   │   ├── generated_pdus.bin        (PduBulkBuffer cache - §12.3)
    │   │   └── pdu_index.bin             (timed index)
    │   ├── recordings/
    │   │   ├── 2026-05-17_1305.disrec    (recorded streams - §13)
    │   │   └── 2026-05-17_1432.disrec
    │   └── exports/
    │       ├── waypoints.csv             (CSV import/export - §8.9)
    │       └── scenario.kml              (future map export)
    │
    └── HighwayConvoy/
        ├── scenario.ini
        ├── ephemerals/
        └── recordings/
```

Rules:

- The **Scenario Name** field (§6.2) is the directory name. The application shall sanitize it to a filesystem-safe form (strip path separators, control chars, trailing dots/spaces; map illegal chars to `_`; reject empty after sanitization).
- The scenario file inside the directory is always named `scenario.ini` — not `<ScenarioName>.ini`. This way the directory name is the only place the scenario name appears on disk, and renaming the scenario is a directory rename.
- `ephemerals/` is reproducible from `scenario.ini` and may be deleted at any time. The app shall regenerate as needed.
- `recordings/` is user-precious output and shall never be auto-deleted.
- The `output/` root path is configurable via a project preference but defaults to `<ProjectRoot>/output/` relative to the executable.

**New / Open / Save semantics:**

```text
New Scenario   - prompts for Scenario Name; creates output/<Name>/ and scenario.ini
Open Scenario  - file dialog scoped to output/, shows scenario directories
Save Scenario  - writes output/<currentName>/scenario.ini
Save As        - prompts for new name; creates new directory; copies contents
Delete         - moves directory to OS recycle bin (never hard-delete)
```

Renaming a scenario in the UI shall rename the directory on disk. If the new name collides, the operation is rejected with a validation error.

### 11.2 Scenario Definition File

The scenario definition file shall use an `.ini` format. It is always named `scenario.ini` inside its scenario directory.

Every `scenario.ini` shall begin with a `[Format]` section identifying the schema version. Readers shall reject files whose `Version` is newer than the reader understands. Adding new optional keys is a non-breaking change; renaming or removing keys requires a version bump.

```ini
[Format]
Version=1
```

Example:

```ini
[Format]
Version=1

[Scenario]
Name=Drone Patrol Demo
Description=Demo scenario with drone orbit and target vehicle
DISVersion=7
ExerciseID=1
SiteID=1
ApplicationID=1
StartTimeUtc=2026-05-17T13:00:00Z
DurationSeconds=300
DefaultUpdateRateHz=5
DefaultCoordinateMode=LatLonAlt

[Origin]
LatitudeDeg=39.0458
LongitudeDeg=-76.6413
AltitudeMeters=0

[Entity.101]
Enabled=1
Name=Drone 1
Description=UAV orbiting target area
ForceID=Friendly
SiteID=1
ApplicationID=1
EntityID=101
Kind=Platform
Domain=Air
Country=USA
Category=UAV
Subcategory=Generic
Specific=0
Extra=0
Marking=DRONE1
BeginSecond=0
EndSecond=300
UpdateRateHz=5
InitialCoordinateMode=LatLonAlt
InitialLatitudeDeg=39.0458
InitialLongitudeDeg=-76.6413
InitialAltitudeMeters=500
InitialHeadingDeg=90
InitialPitchDeg=0
InitialRollDeg=0

[Entity.101.Motion.1]
Enabled=1
Type=Stationary
StartSecond=0
EndSecond=30
CoordinateMode=LatLonAlt
LatitudeDeg=39.0458
LongitudeDeg=-76.6413
AltitudeMeters=500
HeadingDeg=90
PitchDeg=0
RollDeg=0

[Entity.101.Motion.2]
Enabled=1
Type=Line
StartSecond=30
EndSecond=120
CoordinateMode=LatLonAlt
StartLatitudeDeg=39.0458
StartLongitudeDeg=-76.6413
StartAltitudeMeters=500
EndLatitudeDeg=39.0550
EndLongitudeDeg=-76.6300
EndAltitudeMeters=500
SpeedMode=CalculateFromTime
HeadingMode=CalculateFromPath
AltitudeMode=Linear

[Entity.101.Motion.3]
Enabled=1
Type=Ellipse
StartSecond=120
EndSecond=240
CoordinateMode=LatLonAlt
CenterLatitudeDeg=39.0550
CenterLongitudeDeg=-76.6300
CenterAltitudeMeters=500
RadiusXMeters=2000
RadiusYMeters=1000
RotationDeg=0
StartAngleDeg=0
Direction=Clockwise
PeriodSeconds=120
AltitudeMode=Constant

[Output]
Mode=UdpMulticast
DestinationIP=239.1.2.3
DestinationPort=3000
LocalBindAddress=0.0.0.0
TTL=1
LoopbackEnabled=1
PlaybackSpeed=1.0
LoopEnabled=0
```

### 11.3 INI Parser Requirements

The INI reader/writer shall support:

- Sections
- Key/value pairs
- Comments
- Repeated entity sections
- Repeated motion sections
- Default values
- Validation of missing required fields
- Preservation of user comments is optional

### 11.4 Scenario Save/Load Behavior

The application shall support the operations defined in §11.1 (New / Open / Save / Save As / Delete). Recent files shall display scenario directory names (not paths).

**Dirty state.** The main-window title shall display an asterisk (`*`) suffix whenever the editable scenario document has unsaved changes relative to its `scenario.ini` on disk. The asterisk clears on successful save. Save-As resets dirty state on the new document. Closing or opening another scenario while the current one is dirty prompts: Save / Discard / Cancel.

**Dirty does not block Run.** A dirty document may still be sent live, recorded, previewed, or replayed — the worker runs from a Runtime Scenario Snapshot (§18.0) built from the current in-memory state, which need not have been saved. The user sees the asterisk and can save independently of running.

### 11.5 Application Settings - settings.ini

UI-state and per-user preferences live in **`settings.ini` at the project root** (next to the executable). This is a single flat INI file, loaded once at application start, saved once at application close.

```ini
; settings.ini - ScenarioEditor user preferences and UI state.
; Loaded on startup; rewritten on clean shutdown.

[Format]
Version=1

[Window]
PositionX=120
PositionY=80
Width=1280
Height=900
Maximized=0

[Paths]
LastScenarioDirectory=output/DronePatrolDemo
OutputRoot=output

[Recent]
Count=4
File1=output/DronePatrolDemo
File2=output/HighwayConvoy
File3=output/ApproachDemo
File4=output/SmokeTest

[Defaults]
DisVersion=7
DefaultCoordinateMode=LatLonAlt
DefaultUpdateRateHz=10
LastOutputMode=UdpMulticast

[Logging]
LogFilePath=ScenarioEditor.log
```

Load behavior:

- Read on `CScenarioEditorApp::InitInstance` before the main dialog is created (so window position can be applied).
- Missing file → use built-in defaults; do not error.
- Missing keys → use defaults; do not error.
- Unknown sections / keys → preserve on save (round-trip) so a future version's keys aren't destroyed by an old version.

Save behavior:

- Write on `WM_CLOSE` of the main dialog, before destruction.
- Atomic write: write to `settings.ini.tmp`, rename over `settings.ini`. Prevents corruption on crash mid-write.
- A crash before save means the next start loses the most recent window position / recent files — that's an acceptable loss; never persist UI state continuously.

`settings.ini` is **not** version-controlled with scenarios — it is per-installation, per-user state. The directory layout (§11.1) keeps scenario data under `output/<Name>/`, which IS version-controllable; `settings.ini` is at the executable root and lives independently.

---

## 12. Ephemeral Generated Data Format

### 12.1 Purpose

The application shall generate ephemeral PDU data from the scenario definition before or during playback.

This ephemeral format exists to allow:

- Bulk writing generated PDUs
- Bulk reading generated PDUs
- Efficient `.disrec` creation
- Efficient replay
- Separation of scenario definition from generated network data

### 12.2 In-Memory Timed PDU Record

```cpp
struct TimedPduRecord
{
    uint64_t timeOffsetMicroseconds;
    uint32_t pduLengthBytes;
    uint16_t pduType;
    uint16_t disVersion;
    std::vector<uint8_t> pduBytes;
};
```

### 12.3 Bulk Buffer

For performance, the application may use a bulk buffer representation:

```cpp
struct PduBulkBuffer
{
    std::vector<uint8_t> bytes;
    std::vector<TimedPduIndexEntry> index;
};

struct TimedPduIndexEntry
{
    uint64_t timeOffsetMicroseconds;
    uint32_t byteOffset;
    uint32_t pduLengthBytes;
    uint16_t pduType;
    uint16_t disVersion;
};
```

This allows PDU bytes to be written and read in larger chunks instead of writing one PDU at a time.

---

## 13. Recording File Format - `.disrec`

### 13.1 Purpose

The `.disrec` format stores generated timed DIS PDUs for later replay.

The file shall contain:

1. File header
2. Optional scenario metadata
3. Timed PDU records
4. Optional index

### 13.2 File Header

```cpp
#pragma pack(push, 1)
struct DisrecFileHeader
{
    char magic[8];              // "DISREC\0\0"
    uint16_t fileVersion;       // 1
    uint16_t disVersion;        // 6 or 7
    uint32_t headerLengthBytes;
    uint64_t createdUnixTime;
    uint64_t scenarioDurationMicroseconds;
    uint32_t recordCount;
    uint32_t flags;
};
#pragma pack(pop)
```

### 13.3 Timed PDU Record Header

```cpp
#pragma pack(push, 1)
struct DisrecPduRecordHeader
{
    uint64_t timeOffsetMicroseconds;
    uint32_t pduLengthBytes;
    uint16_t pduType;
    uint16_t disVersion;
};
#pragma pack(pop)
```

### 13.4 Record Layout

```text
[DisrecFileHeader]
[DisrecPduRecordHeader][PDU Bytes]
[DisrecPduRecordHeader][PDU Bytes]
[DisrecPduRecordHeader][PDU Bytes]
...
```

**V1/V2 scope:** only the file header (§13.2) followed by a flat sequence of timed PDU records (§13.3). No metadata block, no index block.

**Reserved for future versions (deferred):**

```text
[DisrecFileHeader]
[Optional Metadata Block]      reserved - flags bit 0 indicates presence
[Optional Index Block]         reserved - flags bit 1 indicates presence
```

The `flags` field in `DisrecFileHeader` (§13.2) reserves:

```text
bit 0   HasMetadataBlock   - reserved; v1/v2 writers MUST set 0
bit 1   HasIndexBlock      - reserved; v1/v2 writers MUST set 0
bits 2..31  reserved; writers MUST set 0; readers MUST ignore
```

Format of the metadata block and index block is **TBD** and will be defined when those features land. V1/V2 readers shall reject files where flag bits 0 or 1 are set (unknown format).

### 13.5 Replay Behavior

During replay:

1. Open `.disrec` file.
2. Validate file header.
3. Load index or scan records.
4. Initialize playback clock.
5. Send each PDU when its scheduled offset is reached.
6. Apply playback speed multiplier.
7. If loop is enabled, restart from beginning after completion.

---

## 14. DIS PDU Serialization

### 14.1 Design Choice - Use Open-DIS C++

The application shall use the **Open-DIS C++ library** (https://github.com/open-dis/open-dis-cpp) for DIS PDU serialization and deserialization. ScenarioEditor will not implement its own marshalling layer.

Rationale:

- Open-DIS C++ already implements every PDU type in IEEE 1278.1, both v6 and v7, with correct byte ordering, dead-reckoning records, articulation parameters, and PDU Status (v7) byte handling.
- The fiddly parts (DRM algorithm variants, marking text encoding, Annex A appearance bitfields) are pre-debugged.
- MIT-licensed, header-rich, no runtime dependencies beyond the standard library — links cleanly into a static lib that the `.vcxproj` consumes.
- The Open-DIS data model maps directly to the wire layouts in §14.4 / §14.5, so the documentation in this spec continues to serve as the contract; Open-DIS is the implementation of that contract.

What ScenarioEditor still owns:

- The **scenario model → Open-DIS PDU** boundary: mapping `EntityDefinition` + sampled `EntityPose` (§16) into `DIS::EntityStatePdu` field values, including unit conversions (degrees → radians) and frame conversions (ENU → ECEF, §15).
- The **playback timing and network send layer** (§17, §19) — Open-DIS produces bytes; we still own the socket and the clock.
- The **`.disrec` recording format** (§13) — Open-DIS does not provide a recording container. We write the byte buffers Open-DIS marshals, framed with our own timed record header.

**Cross-project note:** DISBrowser currently uses a hand-rolled DIS parser (`DISBrowser/Source/DISBrowser/Public/DISPDUParser.cpp`), not Open-DIS. Two independent implementations consuming the same wire format is a known drift risk. The recommended long-term path is to migrate DISBrowser to Open-DIS as well, so a single library defines "what the bytes mean" for both apps. Until then, the §14.5.3 conformance tests in this spec must include a byte-for-byte round-trip through DISBrowser's parser as well as Open-DIS.

### 14.2 Open-DIS Integration

The DIS layer in ScenarioEditor shall wrap Open-DIS behind a thin façade that hides the library's namespace from the rest of the codebase. This keeps the option open to swap libraries later without touching scenario or UI code.

```cpp
// dis/DisSerializer.h - facade over Open-DIS C++
#include "dis6/EntityStatePdu.h"   // Open-DIS, v6 family
#include "dis7/EntityStatePdu.h"   // Open-DIS, v7 family
#include "utils/DataStream.h"      // Open-DIS marshalling stream

namespace scn::dis {

class Serializer
{
public:
    explicit Serializer(DisVersion version);

    // Marshal one Entity State PDU built from a sampled entity pose.
    // Returns the wire bytes in network order; throws on validation failure.
    std::vector<uint8_t> SerializeEntityState(const EntityDefinition& entity,
                                              const EntityPose&       pose,
                                              uint8_t                 exerciseId,
                                              uint32_t                timestamp) const;

    // Future: SerializeCreateEntity, SerializeRemoveEntity, etc.

private:
    DisVersion m_version;
};

} // namespace scn::dis
```

The internal implementation populates the Open-DIS PDU object, calls `pdu.marshal(stream)` with a `BIG` endian `DataStream`, then copies the stream's byte vector out. Open-DIS's `DataStream` already enforces network byte order.

For deserialization on the replay path:

```cpp
// dis/DisReader.h - facade over Open-DIS C++
namespace scn::dis {

class Reader
{
public:
    // Identify PDU type by reading header byte 2, then dispatch to the correct
    // Open-DIS PDU class and unmarshal. Returns variant of decoded PDU.
    DecodedPdu Read(const uint8_t* bytes, size_t length) const;
};

} // namespace scn::dis
```

The rest of the application (motion engine, network sinks, recorder) sees only `std::vector<uint8_t>` and never includes an Open-DIS header.

### 14.3 DIS Version Selector

```cpp
enum class DisVersion
{
    DIS6 = 6,
    DIS7 = 7
};
```

### 14.4 PDU Header - Wire Layout

All multi-byte integer and floating-point fields are written in **network byte order (big-endian)**. Floats are IEEE 754 in BE byte order. The serializer shall write through a `BinaryWriter` (see §14.7) rather than `memcpy` of a packed struct, because packing rules vary across compilers and several fields (notably ECEF `float64` location at offset 30 of EntityStatePDU) are not naturally aligned.

PDU Header wire layout (12 bytes):

```text
Offset  Size  Field               Type        Notes
------  ----  ------------------  ----------  -----------------------------------------
 0      1     Protocol Version    uint8       6 or 7
 1      1     Exercise ID         uint8
 2      1     PDU Type            uint8       1 = EntityState, 11 = CreateEntity, 12 = RemoveEntity
 3      1     Protocol Family     uint8       1 = Entity Information/Interaction, 5 = SimMgmt
 4      4     Timestamp           uint32 BE   See §14.4.1
 8      2     Length              uint16 BE   Total PDU length in bytes (header + body)
10      2     v6: Padding         uint16      Reserved, write zero
10      1     v7: PDU Status      uint8       See §14.4.2
11      1     v7: Padding         uint8       Reserved, write zero
```

#### 14.4.1 Timestamp encoding

DIS timestamps are 32-bit fields per IEEE 1278.1, encoded as:

```text
bit 0           Time-type flag:
                  0 = relative timestamp (host-local, no external sync required)
                  1 = absolute timestamp (synced to UTC; resets every hour)
bits 1..31      Time value in units of (3600 / 2^31) seconds since the top of
                the current hour. One unit is approximately 1.676 microseconds.
                Range covers exactly one hour, then wraps.
```

**ScenarioEditor shall produce relative timestamps (bit 0 = 0).** This decision is fixed for both live-send and recording paths.

Rationale:

- The same timestamps must be valid whether the scenario is being sent live or replayed from a `.disrec` file hours or days later. Relative timestamps have no dependency on wall-clock time, so a recording produced today plays back identically tomorrow.
- DISBrowser, the primary visual consumer (§1.1), uses arrival ordering plus dead-reckoning state — it does not require absolute UTC sync.
- Removes a class of failure mode (clock skew between editor and viewer producing visible jitter).

The 31-bit timestamp value shall be computed as:

```cpp
uint32_t MakeRelativeTimestamp(double scenarioTimeSeconds)
{
    // Modulo one hour, encode as units of (3600 / 2^31) seconds.
    const double hoursMod = std::fmod(scenarioTimeSeconds, 3600.0);
    const uint32_t ticks  = static_cast<uint32_t>(hoursMod * (2147483648.0 / 3600.0));
    return (ticks << 1);   // bit 0 = 0 means relative
}
```

If a future receiver requires absolute timestamps, that is a per-output-target setting added later; it is not a v1/v2 capability.

#### 14.4.2 PDU Status byte (v7 only)

The v7 PDU Status byte occupies the first padding byte of the v6 header. Bit assignments per IEEE 1278.1-2012 §6.2.67:

```text
bit 0       TEI   Transferred Entity Indicator
bit 1       LVC   Live/Virtual/Constructive (with bit 2)
bit 2       LVC   Live/Virtual/Constructive (with bit 1)
bit 3       CEI   Coupled Extension Indicator
bits 4..7   FTI/DTI/RAI/IAI/ISM/AII  (PDU-type-specific; see standard table)
```

Getting v6/v7 padding wrong shifts every subsequent byte by one — a failure that looks like total corruption rather than a one-field error. A header-only round-trip unit test catches this cheaply.

### 14.5 EntityStatePDU - Wire Layout

Fixed body is 132 bytes; total fixed PDU size is 144 bytes (header + body), plus 16 bytes per articulation parameter.

```text
Offset  Size  Field                       Type           Units / Notes
------  ----  --------------------------  -------------  -------------------------------
  0     12    PDU Header                  (see §14.4)
 12      6    Entity ID                   3 × uint16 BE  site, application, entity
 18      1    Force ID                    uint8          1=Friendly, 2=Opposing, 3=Neutral, ...
 19      1    Articulation Param Count    uint8          N (count of trailing records)
 20      8    Entity Type                 (see below)    "what this entity actually is"
 28      8    Alternative Entity Type     (see below)    "what this entity appears to be"
 36     12    Entity Linear Velocity      3 × float32 BE meters/second, frame depends on DR algo
 48     24    Entity Location (ECEF)      3 × float64 BE meters, WGS-84 ECEF
 72     12    Entity Orientation          3 × float32 BE RADIANS, order (psi, theta, phi)
                                                         = (yaw, pitch, roll), entity-to-world
 84      4    Entity Appearance           uint32 BE      Bitfield, layout depends on entity kind
 88     40    Dead Reckoning Parameters   (see below)
128     12    Entity Marking              (see below)    charset byte + 11 bytes text
140      4    Entity Capabilities         uint32 BE      Bitfield
144   N×16    Articulation Parameters     (variable)     Repeats N times
```

Entity Type / Alternative Entity Type (8 bytes each):

```text
+0  1  Kind          uint8        0=Other, 1=Platform, 2=Munition, 3=LifeForm, ...
+1  1  Domain        uint8        Per Kind; e.g. for Platform: 1=Land, 2=Air, 3=Surface, 4=Subsurface, 5=Space
+2  2  Country       uint16 BE    SISO-REF-010 country code (e.g. 225 = USA)
+4  1  Category      uint8
+5  1  Subcategory   uint8
+6  1  Specific      uint8
+7  1  Extra         uint8
```

Dead Reckoning Parameters (40 bytes):

```text
+ 0   1   Algorithm                 uint8           1..9, see §14.5.1
+ 1  15   Other Parameters          15 bytes        Algorithm-specific; write zero unless used
+16  12   Linear Acceleration       3 × float32 BE  meters/second^2, frame depends on algo
+28  12   Angular Velocity          3 × float32 BE  radians/second, body frame
```

Entity Marking (12 bytes):

```text
+0  1   Character Set    uint8        1 = ASCII (recommended), 2 = Army Marking, 3 = Digram
+1 11   Marking Text     11 bytes     Null-padded, NOT null-terminated; 12-char names truncate
```

#### 14.5.1 Frame-of-reference and units - gotchas

These are field-level pitfalls that a "just port the struct" pass will miss:

- **Orientation is radians, not degrees.** The scenario model (§7.3) uses degrees. The serializer must convert at the model→PDU boundary. Use unit-tagged types (`Radians`, `Degrees`) in the C++ model rather than raw `double` so the compiler enforces it.
- **Orientation order is (psi, theta, phi)** = (yaw, pitch, roll), describing the **entity-to-world** rotation per IEEE 1278.1 §B.1.6.4. Documenting the convention next to the struct is mandatory; readers will otherwise guess Tait-Bryan ZYX vs XYZ wrong.
- **Linear velocity frame depends on the dead-reckoning algorithm:**

  ```text
  Algorithm 1  Static               velocity must be zero
  Algorithm 2  DRM(F,P,W)           WORLD coords (ECEF), no rotation update
  Algorithm 3  DRM(R,P,W)           WORLD coords, with rotation update
  Algorithm 4  DRM(R,V,W)           WORLD coords, with rotation update + accel
  Algorithm 5  DRM(F,V,W)           WORLD coords, with accel
  Algorithm 6  DRM(F,P,B)           BODY coords, no rotation update
  Algorithm 7  DRM(R,P,B)           BODY coords, with rotation update
  Algorithm 8  DRM(R,V,B)           BODY coords, with rotation update + accel
  Algorithm 9  DRM(F,V,B)           BODY coords, with accel
  ```

  The serializer must know which frame the motion engine produced and either match the algorithm choice to that frame or transform. V1/V2 should default to **Algorithm 2** (world coords, simplest) unless a specific receiver requires otherwise.
- **Marking text is ASCII-only, null-padded, not null-terminated.** Per IEEE 1278.1, the 11-byte marking field is ASCII (character set = 1). At the PDU boundary the serializer shall:
  1. Strip any non-ASCII characters (codepoint > 0x7E or < 0x20, except permitted control chars) and emit a validation warning naming the entity.
  2. Truncate the resulting ASCII string to 11 characters and emit a validation warning if data was lost.
  3. Null-pad the remainder of the 11 bytes (do not null-terminate; the field has no terminator).

  The form layer (§7.3) shall display the Marking input as a fixed-width 11-character ASCII edit control; the model layer accepts up to 11 ASCII chars with non-ASCII rejected at field-validation time so the user sees the warning while editing rather than at Start time.
- **Entity Appearance is a kind-specific bitfield**, not opaque. Air, ground, surface, and life-form platforms each have different bit assignments per Annex A. V1 may write zero; later versions should validate bits against entity kind.

#### 14.5.2 C++ representation - Open-DIS usage

ScenarioEditor does not define its own EntityStatePdu class. It uses Open-DIS's `DIS::EntityStatePdu` directly and populates it from the scenario model. Sample population code (DIS v7):

```cpp
#include "dis7/EntityStatePdu.h"
#include "utils/DataStream.h"

std::vector<uint8_t> Serializer::SerializeEntityState(
    const EntityDefinition& entity,
    const EntityPose&       pose,
    uint8_t                 exerciseId,
    uint32_t                timestamp) const
{
    DIS::EntityStatePdu pdu;

    // Header
    pdu.setProtocolVersion(static_cast<unsigned char>(m_version));   // 6 or 7
    pdu.setExerciseID(exerciseId);
    pdu.setTimestamp(timestamp);                                     // §14.4.1 relative

    // Entity ID (site, app, entity)
    DIS::EntityID id;
    id.setSite(entity.siteId);
    id.setApplication(entity.applicationId);
    id.setEntity(entity.entityId);
    pdu.setEntityID(id);

    pdu.setForceId(static_cast<unsigned char>(entity.forceId));

    // Entity Type
    DIS::EntityType et;
    et.setEntityKind(entity.kind);
    et.setDomain(entity.domain);
    et.setCountry(entity.country);
    et.setCategory(entity.category);
    et.setSubcategory(entity.subcategory);
    et.setSpecific(entity.specific);
    et.setExtra(entity.extra);
    pdu.setEntityType(et);

    // Location - ECEF doubles (§15)
    DIS::Vector3Double loc;
    loc.setX(pose.ecefX);
    loc.setY(pose.ecefY);
    loc.setZ(pose.ecefZ);
    pdu.setEntityLocation(loc);

    // Orientation - degrees -> radians at the boundary (§14.5.1 gotcha)
    DIS::Orientation orient;
    orient.setPsi  (DegreesToRadians(pose.headingDeg));
    orient.setTheta(DegreesToRadians(pose.pitchDeg));
    orient.setPhi  (DegreesToRadians(pose.rollDeg));
    pdu.setEntityOrientation(orient);

    // Velocity (ECEF world frame). Algorithm and acceleration are chosen
    // per motion segment type by the motion engine - see §16.4.
    DIS::Vector3Float vel;
    vel.setX(static_cast<float>(pose.velocityEcefXMps));
    vel.setY(static_cast<float>(pose.velocityEcefYMps));
    vel.setZ(static_cast<float>(pose.velocityEcefZMps));
    pdu.setEntityLinearVelocity(vel);

    // Dead Reckoning: algorithm + acceleration come from the sampled pose
    // (populated by the motion segment, §16.5).
    DIS::DeadReckoningParameter dr;
    dr.setDeadReckoningAlgorithm(static_cast<unsigned char>(pose.drAlgorithm));

    DIS::Vector3Float accel;
    accel.setX(static_cast<float>(pose.accelerationEcefXMps2));
    accel.setY(static_cast<float>(pose.accelerationEcefYMps2));
    accel.setZ(static_cast<float>(pose.accelerationEcefZMps2));
    dr.setEntityLinearAcceleration(accel);
    // Angular velocity is zero for v1/v2 algorithms (1, 2, 5) - §16.4.
    pdu.setDeadReckoningParameters(dr);

    // Marking - truncate to 11 chars, null-pad
    DIS::Marking marking;
    marking.setCharacterSet(1);                                  // ASCII
    marking.setByStringCharacters(entity.markingText.c_str());   // Open-DIS handles padding
    pdu.setMarking(marking);

    // Marshal to network byte order
    DIS::DataStream stream(DIS::BIG);
    pdu.marshal(stream);

    return std::vector<uint8_t>(stream.begin(), stream.end());
}
```

The scenario model side defines its own POD types (`EntityDefinition`, `EntityPose`, etc., per §22.2) — Open-DIS types are confined to the `dis::` namespace and never leak into the UI or motion layers.

#### 14.5.3 Conformance tests

The wire layout in §14.4–§14.5 is now a **specification of what Open-DIS produces**, not what we produce. Conformance tests verify the integration boundary — the scenario model → Open-DIS → bytes path — and the cross-implementation contract with DISBrowser's parser.

Required tests:

1. **Default round-trip.** Build a `DIS::EntityStatePdu` with default-constructed fields, marshal to bytes via Open-DIS. Verify the result is exactly 144 bytes and byte 2 (PDU Type) is `0x01`.
2. **Location preservation.** Populate `setEntityLocation(1.0, 2.0, 3.0)`, marshal, verify bytes 48..71 are the big-endian IEEE 754 encoding of those three doubles.
3. **Marking encoding.** Set marking to `"HELLO"`, marshal, verify byte 128 is `0x01` (ASCII charset), bytes 129..133 are `"HELLO"`, bytes 134..139 are zero.
4. **v6 vs v7 header.** Marshal the same logical PDU with `protocolVersion = 6` and `protocolVersion = 7`. Verify byte 10 differs (padding vs PDU Status). Verify byte 0 differs accordingly.
5. **Round-trip through Open-DIS.** Marshal → unmarshal via Open-DIS into a fresh PDU object. All fields must compare equal.
6. **Cross-implementation contract with DISBrowser.** Marshal via Open-DIS, parse via `DISPDUParser::TryParseEntityStatePDU()` from DISBrowser. The decoded fields must match the scenario input. This test is the canary for the drift risk called out in §14.1.
7. **Wireshark sanity.** Capture one Send Live session and decode in Wireshark with the DIS dissector. Verify entity location, orientation, and marking match the scenario inputs.

Test 6 should run in CI; test 7 is a manual acceptance check at integration time.

### 14.6 Initial PDU Types

Initial scope (Open-DIS C++ classes shown for v7; v6 family uses parallel `dis6::` headers):

```text
Entity State PDU      - Type 1,  Family 1   DIS::EntityStatePdu
Create Entity PDU     - Type 11, Family 5   DIS::CreateEntityPdu
Remove Entity PDU     - Type 12, Family 5   DIS::RemoveEntityPdu
```

Future scope:

```text
Fire PDU                       - Type 2,  Family 2   DIS::FirePdu
Detonation PDU                 - Type 3,  Family 2   DIS::DetonationPdu
Collision PDU                  - Type 4,  Family 1   DIS::CollisionPdu
Transmitter PDU                - Type 25, Family 4   DIS::TransmitterPdu
Signal PDU                     - Type 26, Family 4   DIS::SignalPdu
Receiver PDU                   - Type 27, Family 4   DIS::ReceiverPdu
Electromagnetic Emission PDU   - Type 23, Family 6   DIS::ElectronicEmissionsPdu
Logistics PDUs                            Family 3
Simulation Management PDUs                Family 5
```

### 14.7 Open-DIS Build Integration

Open-DIS C++ ships as a CMake project. To consume it from a Visual Studio 2022 `.vcxproj`:

```text
1. Clone open-dis-cpp at a pinned tag into ScenarioEditor/external/open-dis-cpp/
2. Generate Visual Studio project files: cmake -G "Visual Studio 17 2022" -A x64
3. Build the static library configuration (OpenDIS6_static.lib, OpenDIS7_static.lib)
4. Reference output libs in ScenarioEditor.vcxproj:
   - Additional Include Directories: $(ProjectDir)external\open-dis-cpp\src
   - Additional Library Directories: $(ProjectDir)external\open-dis-cpp\build\$(Configuration)
   - Additional Dependencies:        OpenDIS7_static.lib;OpenDIS6_static.lib
5. Pin the Open-DIS commit hash in a top-level README or submodule. Bumping versions
   is a deliberate action with a re-run of §14.5.3 conformance tests.
```

`Ws2_32.lib` is still required for the network layer (§17) — Open-DIS does not provide sockets.

For the **`.disrec` reader/writer (§13)** and any other binary file framing we own, a small `BinaryReader` helper is still useful (Open-DIS's `DataStream` is scoped to PDU marshalling, not arbitrary file I/O):

```cpp
class BinaryReader
{
public:
    explicit BinaryReader(const uint8_t* data, size_t length);
    uint8_t  ReadUInt8();
    uint16_t ReadUInt16LE();   // .disrec headers are LE per §13.2
    uint32_t ReadUInt32LE();
    uint64_t ReadUInt64LE();
    void     ReadBytes(void* out, size_t length);
    size_t   BytesRead() const;
};
```

Note: `.disrec` file-header fields are little-endian (host-order on x64). The DIS PDU payload bytes inside each record are big-endian as produced by Open-DIS — those bytes are written as opaque blobs and never re-byte-swapped.

---

## 15. Coordinate Systems

### 15.1 Required Coordinate Modes

The application shall support these coordinate modes from the beginning:

```text
Lat / Lon / Alt    WGS-84 geodetic, degrees + meters ELLIPSOIDAL (see §15.1.1)
Local X / Y / Z    ENU tangent plane anchored at the scenario origin (see §15.2)
ECEF               WGS-84 Earth-Centered, Earth-Fixed, meters
```

#### 15.1.1 Altitude Reference - WGS-84 Ellipsoidal

All altitude fields in ScenarioEditor (entity initial altitude, motion segment altitudes, scenario origin altitude) are **WGS-84 ellipsoidal height** (height above the WGS-84 reference ellipsoid), not mean sea level (MSL).

Rationale:

- DIS Entity State PDU location is ECEF (§14.5), and the ECEF math used by the coordinate engine (§15.5) assumes WGS-84 ellipsoidal heights. Using MSL anywhere internally would introduce a non-trivial conversion through a geoid model.
- The two references differ by the local **geoid undulation**, which can be as much as ±100 meters in some regions (e.g. roughly -30m in the U.S. Midwest, +60m around Iceland). Quietly accepting an MSL value as if it were ellipsoidal produces a vertically-misplaced entity that the user never sees in the form.
- DISBrowser's coordinate converter (§15.5) and Unreal's georeferencing pipeline are also WGS-84 ellipsoidal. Picking the same reference end-to-end eliminates a class of "why is the aircraft floating 60m above the runway" bug.

**Conversion at the boundary:** If a user has altitude data sourced as MSL (most aviation, most marine, most consumer GPS displays) the conversion to ellipsoidal must happen *before* the value enters ScenarioEditor's data model:

```text
height_ellipsoidal  =  height_msl  +  geoid_undulation(lat, lon)
```

For v1/v2 the application does **not** ship a geoid model. The form field is labeled "Altitude (m, WGS-84 ellipsoidal)" and the user is responsible for converting. A future enhancement (§24) may add an EGM96/EGM2008 geoid lookup with an MSL-input toggle. Until then, MSL input is treated as ellipsoidal silently — the validation panel shows an info-level note when altitude values look suspicious (e.g. a Pacific Ocean entity at altitude +30m might actually be sea-level MSL).

### 15.2 Local Frame Convention - ENU

The user-facing "Local X/Y/Z" coordinate mode is **ENU (East-North-Up)**, not NED.

```text
+X  =  East   (along the local parallel of latitude, increasing eastward)
+Y  =  North  (along the local meridian, increasing northward)
+Z  =  Up     (along the local outward normal, away from Earth's center)
```

The ENU tangent plane is anchored at the **Scenario Origin** defined in §6.4 (origin latitude, longitude, altitude). The origin's altitude is the Z=0 reference for local altitude.

Why ENU:

- Matches standard simulation authoring tools (CIGI, KML, Cesium, Unreal Engine's Georeferencing plugin all use ENU).
- DISBrowser's `DISCoordinateConverter` (the sibling app, §1.1) already implements ENU ↔ ECEF — using the same convention means a user's "1000m north" displays in DISBrowser exactly where the editor placed it.
- Heading 0° = North in the ENU convention is intuitive and matches DIS Marking/HUD conventions.

NED (North-East-Down) is **explicitly not supported** as a user input mode. Aviation users who think in NED must convert at the boundary (swap X/Y, negate Z) or use Lat/Lon/Alt directly.

### 15.3 Coordinate Mode Enumeration

```cpp
enum class CoordinateMode
{
    LatLonAlt,   // WGS-84 geodetic
    LocalENU,    // East-North-Up tangent plane at scenario origin
    ECEF         // WGS-84 Earth-Centered, Earth-Fixed
};
```

The enum value name `LocalENU` (not `LocalXYZ`) makes the convention unmistakable in code. UI labels may still read "Local X/Y/Z" for user readability.

### 15.4 Coordinate Structures

```cpp
struct LatLonAlt
{
    double latitudeDeg;     // WGS-84
    double longitudeDeg;    // WGS-84
    double altitudeMeters;  // ellipsoidal height
};

struct LocalENU
{
    double eastMeters;      // +X
    double northMeters;     // +Y
    double upMeters;        // +Z (up from local tangent plane)
};

struct EcefXYZ
{
    double xMeters;
    double yMeters;
    double zMeters;
};
```

### 15.5 Conversion Requirements

The coordinate engine shall provide:

```cpp
EcefXYZ  LatLonAltToEcef(const LatLonAlt& lla);
LatLonAlt EcefToLatLonAlt(const EcefXYZ& ecef);
EcefXYZ  LocalEnuToEcef(const LocalENU& enu, const LatLonAlt& origin);
LocalENU EcefToLocalEnu(const EcefXYZ& ecef, const LatLonAlt& origin);
LatLonAlt LocalEnuToLatLonAlt(const LocalENU& enu, const LatLonAlt& origin);
LocalENU LatLonAltToLocalEnu(const LatLonAlt& lla, const LatLonAlt& origin);
```

Implementation note: DISBrowser already contains a working WGS-84 ECEF↔LLA + ECEF↔ENU implementation in `DISBrowser/Source/DISBrowser/Public/DISCoordinateConverter.cpp`. The math is the same; the only adaptation needed for ScenarioEditor is replacing Unreal's `FMath::Sin/Cos/Atan2/DegreesToRadians` with `std::sin/cos/atan2` and the `FVector` return types with the structs above. **Recommended action: port that converter rather than re-derive the formulas.** This is the only piece of DISBrowser code worth direct reuse identified so far.

### 15.6 Internal Normalization

For PDU generation, entity positions are normalized to **ECEF** internally — the DIS wire format requires ECEF (§14.5), and ECEF is the only frame that is global, unambiguous, and origin-independent.

```text
User input coordinate mode (LatLonAlt | LocalENU | ECEF)
        ↓
Coordinate engine (single conversion to ECEF at the model boundary)
        ↓
ECEF EntityPose (canonical, used by motion + serializer)
        ↓
DIS::EntityStatePdu.setEntityLocation(ecef) via Open-DIS (§14.5.2)
```

The Preview tab (§10) renders in **LocalENU** projected to 2D (drop Z, or shade by Z), with the origin at the canvas center.

---

## 16. Motion Engine

### 16.1 Motion Model Interface

```cpp
class IMotionSegment
{
public:
    virtual ~IMotionSegment() = default;

    virtual double GetStartSecond() const = 0;
    virtual double GetEndSecond() const = 0;
    virtual bool ContainsTime(double scenarioTimeSeconds) const = 0;
    virtual EntityPose Sample(double scenarioTimeSeconds) const = 0;
};
```

### 16.2 Entity Timeline

```cpp
class EntityMotionTimeline
{
public:
    std::vector<std::unique_ptr<IMotionSegment>> segments;

    const IMotionSegment* FindSegment(double scenarioTimeSeconds) const;
    EntityPose Sample(double scenarioTimeSeconds) const;
};
```

### 16.3 Sampling Rules

- If no segment exists at time `t`, entity is inactive.
- If multiple enabled segments overlap, validation should report an error.
- If a segment gap exists, validation should report a warning.
- Segment boundaries shall be deterministic.
- End time is exclusive except for the final segment's end time. The first PDU of segment N is sampled with segment N's dead-reckoning algorithm and fields (§16.4), even when wall-clock `t` equals segment N-1's end time.

### 16.4 Dead-Reckoning Algorithm Per Motion Type

Each motion segment type emits a fixed DIS dead-reckoning algorithm and is responsible for populating the auxiliary fields (linear acceleration, angular velocity) that algorithm requires. This is what lets DISBrowser (or any DIS receiver) extrapolate smoothly between PDUs at the entity's update rate, instead of seeing a polygonal sequence of position snaps.

Algorithm assignment:

```text
Motion Type      DR Algo  Name           Fields populated by Sample()
---------------  -------  -------------  ---------------------------------------
Stationary       1        Static         velocity = 0, accel = 0
Stop / Hold      1        Static         velocity = 0, accel = 0
Straight Line    2        DRM(F,P,W)     velocity (ECEF), accel = 0
Circle           5        DRM(F,V,W)     velocity (ECEF, tangent),
                                         accel (ECEF, centripetal toward center)
Ellipse          5        DRM(F,V,W)     velocity (ECEF, analytic tangent),
                                         accel (ECEF, analytic centripetal)
Waypoints        2        DRM(F,P,W)     leg-wise constant velocity (ECEF), accel = 0
```

Algorithms 1, 2, and 5 are the only algorithms ScenarioEditor v1/v2 emits. They cover every motion type defined in §8.3.

Algorithms not used in v1/v2 — and the reasons:

```text
3, 4, 7, 8   Include rotation update via angular velocity.
             Reserved for later motion types that change attitude smoothly
             (e.g., banked turns, terrain-following with pitch).
6, 9         Body-frame velocity. Reserved for imported telemetry tracks
             (§8.3 future scope) where the source data is body-relative.
```

The motion engine's `IMotionSegment::Sample()` shall populate these fields by **analytic differentiation of the segment's closed-form path**, never by finite differences. Finite differences (`(pos(t+ε) − pos(t)) / ε`) introduce per-step noise that defeats the receiver's smoothing — the editor must hand the receiver clean derivatives, not numerically-estimated ones.

Per-segment derivation rules:

- **StationarySegment / StopMotionSegment** — `velocity = (0,0,0)`, `accel = (0,0,0)`. Algorithm = 1.
- **LineMotionSegment** — `velocity = (endEcef − startEcef) / durationSeconds`, `accel = (0,0,0)`. Algorithm = 2.
- **CircleMotionSegment** — At sample time `t`, compute the local-ENU tangent and centripetal vectors analytically from radius `r`, period `T`, and angular position. Magnitude of velocity is `2πr/T`; magnitude of centripetal acceleration is `v²/r` directed toward the center. Convert both into ECEF using the scenario origin's rotation matrix. Algorithm = 5.
- **EllipseMotionSegment** — Differentiate the parametric ellipse `(rₓ·cos θ, r_y·sin θ)` at the current angular position to get the tangent (velocity direction) and second derivative for centripetal acceleration. Velocity magnitude varies along the arc; acceleration magnitude varies as well. Convert both into ECEF. Algorithm = 5.
- **WaypointMotionSegment** — Within a single leg between two waypoints, treat as LineMotionSegment (velocity from `(next − prev) / legDuration`, accel = 0, Algorithm = 2). At waypoint boundaries the velocity vector jumps discontinuously; the receiver absorbs this at the next PDU.

### 16.5 Extended Pose Sample

The `EntityPose` struct (§8.10) carries the position/orientation fields the receiver needs as ground truth. For dead reckoning to work, `Sample()` must also emit the derivative fields the receiver needs to extrapolate. Extend `EntityPose` with:

```cpp
enum class DeadReckoningAlgorithm : uint8_t
{
    Static = 1,
    Fpw    = 2,   // DRM(F,P,W) - velocity only, world frame
    Fvw    = 5,   // DRM(F,V,W) - velocity + accel, world frame
    // 3, 4, 6, 7, 8, 9 reserved (§16.4)
};

struct EntityPose
{
    // Existing positional fields (§8.10) - ECEF is the canonical frame:
    double ecefXMeters,  ecefYMeters,  ecefZMeters;
    double headingDeg,   pitchDeg,     rollDeg;

    // Dead-reckoning fields used by the DIS serializer (§14.5.2):
    DeadReckoningAlgorithm drAlgorithm;
    double velocityEcefXMps,      velocityEcefYMps,      velocityEcefZMps;        // m/s
    double accelerationEcefXMps2, accelerationEcefYMps2, accelerationEcefZMps2;   // m/s^2
    // Angular velocity reserved for future algorithms 3/4/7/8; always (0,0,0) in v1/v2:
    double angularVelocityXRps,   angularVelocityYRps,   angularVelocityZRps;     // rad/s
};
```

`Serializer::SerializeEntityState` (§14.5.2) reads `drAlgorithm`, `velocityEcef*`, and `accelerationEcef*` from the sampled pose and writes them into the `DIS::DeadReckoningParameter` block — no per-segment branching needed at the serializer layer.

### 16.6 Update Rate Selection

Each entity has an `UpdateRateHz` (§7.3) defaulting to the scenario-level `DefaultUpdateRateHz` (§6.2). The motion engine samples the entity's active segment at exactly this rate, producing **time-equispaced** PDUs (not distance-equispaced — equidistant-in-space sampling would break dead reckoning, since DR assumes uniform `dt`).

Recommended default rates:

```text
Entity class                             Rate     Notes
---------------------------------------  -------  --------------------------------------
Stationary entity                        1-2 Hz   Mostly Create + refresh; DR algo 1.
Slow ground vehicles                     5 Hz     Algo 2 is sufficient.
Fast ground / surface ships              10 Hz    Algo 2.
Aircraft, straight cruise                5-10 Hz  Algo 2.
Aircraft on circle / ellipse / turn      10 Hz    Algo 5 with analytic accel makes
                                                  10 Hz visually smooth at the receiver.
Maneuvering / waypoint transitions       20 Hz    Compensates for accel discontinuity
                                                  at segment boundaries (§16.3).
```

Higher rates are recommended at segment boundaries because the analytic acceleration is only correct *within* the current segment. At a boundary, the acceleration vector can jump discontinuously, and the receiver needs a fresh PDU promptly to update its extrapolation parameters before overshoot becomes visible.

Validation (§20) uses each entity's `UpdateRateHz` × scenario duration to compute the estimated PDU count and bandwidth shown in the status bar (§5.4). Rates above 30 Hz on many entities trigger the high-bandwidth warning.

---

## 17. Network Layer

### 17.1 Winsock2 Usage

The application shall use Winsock2 directly.

Startup/shutdown:

```cpp
WSADATA wsaData;
WSAStartup(MAKEWORD(2, 2), &wsaData);
WSACleanup();
```

### 17.2 UDP Unicast

UDP unicast sender shall support:

- Destination IP
- Destination port
- Optional local bind address
- Optional local port
- Send buffer size
- TTL

### 17.3 UDP Multicast

UDP multicast sender shall support:

- Multicast group address
- Destination port
- Outgoing interface
- TTL
- Loopback enabled/disabled
- Send buffer size

### 17.4 TCP Direct Connection

TCP shall support:

1. Client mode: connect to remote host/port
2. Server mode: listen for one or more clients

The first implementation may support one connected client, with multi-client support added later.

### 17.5 Network Send Interface

```cpp
class INetworkSink
{
public:
    virtual ~INetworkSink() = default;
    virtual bool Open() = 0;
    virtual bool Send(const uint8_t* data, size_t length) = 0;
    virtual void Close() = 0;
};
```

Implementations:

```cpp
class UdpUnicastSink : public INetworkSink;
class UdpMulticastSink : public INetworkSink;
class TcpClientSink : public INetworkSink;
class TcpServerSink : public INetworkSink;
```

### 17.6 Logging

The project provides a single logging macro in `log.h` / `log.cpp` (already present at the project root). All ScenarioEditor code that needs to record errors, warnings, or notable events shall use this macro — no `printf`, no `std::cerr`, no `OutputDebugString`, no `TRACE`.

```cpp
// From log.h
#define LOG(x)  logFile(__FILE__, __LINE__, x)

int logFile(const char* pszFile, long lLine, const char* pszString);
```

Usage rules:

- `LOG(x)` takes a `const char*`. For formatted messages, format into the static `szError[1024]` buffer (declared `extern` in `log.h`) and pass that to `LOG`:

  ```cpp
  snprintf(szError, sizeof(szError),
           "UDP send failed: dst=%s:%d errno=%d", dstIp, dstPort, WSAGetLastError());
  LOG(szError);
  ```

- Every catch-able error path that returns failure or marks state as Error shall `LOG(...)` first.
- Every validation result added to the panel (§20) shall also `LOG(...)` so the log preserves the history across runs.
- Worker threads may call `LOG` directly — it is the project's responsibility to make `logFile` thread-safe internally (mutex around file write). Do not invent thread-local buffers.

The log destination is whatever `log.cpp` implements (file under the executable's directory by default). Do not bypass this — the user expects one log, in one place, for everything.

### 17.7 Error-Handling Policy for Output and Playback

The network layer (§17), the file recording layer (§13), and the replay scheduler (§18.3) shall never silently hide failures. Every failure is classified into one of two policies:

**Fail-fast (unrecoverable):**

- Socket cannot be opened (bind failed, address in use, multicast join failed).
- Replay file cannot be opened, has a bad magic, or has a flags bit the v1/v2 reader does not understand (§13.4).
- Recording file cannot be opened or initial header write fails.
- Out-of-disk during recording.
- TCP connection lost in the middle of a recording (no auto-reconnect in v1; that is a future enhancement).

Action: `LOG(reason)`, transition the playback state to `Error` (§19.5), post `WM_APP_PLAYBACK_ERROR` to the UI thread, stop the worker. The UI surfaces a non-modal banner with the error text from the log and offers a "View Log" button.

**Drop-and-count (recoverable):**

- A single `sendto()` returns `WSAEWOULDBLOCK`, `WSAENOBUFS`, or any other non-fatal error.
- A multicast send fails on one of multiple interfaces (when v2 adds multi-interface support).
- A `.disrec` record-write returns a short write that completes on retry.

Action: increment a per-sink drop counter, `LOG` the first occurrence and then again every Nth occurrence (e.g. 1, 10, 100, 1000) to avoid log flooding, continue running. The drop counter is exposed in the status bar (§5.4) next to PDUs sent: `PDUs: 1234 (3 dropped)`.

Forbidden:

- Swallowing an error and returning success.
- Logging an error but continuing as if nothing happened, when the error means the next PDU will also fail (this is a fail-fast case, not a drop-and-count case).
- `try { ... } catch (...) { /* nothing */ }`.

When in doubt about which policy applies, fail-fast. A loud stop is recoverable by the user; a silent half-running scenario is not.

---

## 18. Generation, Recording, and Replay

### 18.0 Runtime Scenario Snapshot (Copy-on-Start)

Whenever the user triggers a long-running scenario operation — **Send Live, Record, Preview Playback, or Replay** — the application shall not hand the editable scenario document to the worker thread. Instead, it shall build an **immutable Runtime Scenario Snapshot** and pass that snapshot to the worker. The editable document remains the user's UI-thread copy.

This prevents the entire class of races where the user edits an entity field while the motion engine on a worker thread is reading the same field. There is no shared mutable state between UI and worker — the snapshot is read-only for its entire lifetime.

**Start-time sequence:**

```text
User clicks [Send Live] | [Record] | [Preview] | [Replay File]
        ↓
a. Validate the editable scenario (§20). Block if errors exist.
b. Build a Runtime Scenario Snapshot (deep copy of the scenario model,
   resolved coordinates, expanded waypoint lists, sampled-friendly form).
c. Pass the snapshot pointer to the worker thread (shared_ptr<const Snapshot>).
d. UI re-enables (or partially re-enables) editing of the editable document.
e. Status bar shows: "Running snapshot from <timestamp>".
```

**Lifetime and ownership:**

```cpp
struct RuntimeScenarioSnapshot {
    // Everything the worker needs to run the scenario, frozen at Start time.
    ScenarioDefinition          scenario;          // deep-copied
    std::vector<EntityRuntime>  entities;          // resolved, ECEF-canonical
    OutputDefinition            output;            // network/file target
    DisVersion                  disVersion;
    uint8_t                     exerciseId;
    std::chrono::steady_clock::time_point capturedAt;
};

// Built on UI thread, immutable after construction:
std::shared_ptr<const RuntimeScenarioSnapshot>
BuildSnapshot(const ScenarioDefinition& editable);

// Worker receives const ref - cannot mutate:
void ScenarioWorkerBase::Run(std::shared_ptr<const RuntimeScenarioSnapshot> snap);
```

The snapshot is reference-counted (`shared_ptr<const T>`). The worker holds a strong reference for its entire run; the UI may discard its reference immediately after Start. The snapshot is destroyed when the worker thread joins.

**UI editability while running:**

- The user **may** continue to edit the editable scenario document. Edits go into the editable model only.
- The user **may** save the editable document to disk while a scenario is running.
- The user **may not** start a second scenario operation while one is in flight (§19.2 — single-worker rule).
- A non-modal banner near the toolbar shows: **"Edits will not affect the running scenario. Stop and Start again to apply."**
- The Preview tab during a live operation shows the **snapshot**, not the editable document. Editing entities while live does not change the preview until the next Start.

**Stop and re-Start:**

- When the user clicks Stop, the worker joins, the snapshot is released.
- Clicking Start again builds a fresh snapshot from the now-edited document and begins a new run. There is no "resume with new edits" path — that would re-introduce the mutation race.

This pattern is non-negotiable: any future feature that crosses the UI/worker boundary must use the snapshot, not the editable document.

### 18.1 Live Real-Time Generation

For live sending:

```text
Scenario model
  → Motion engine samples entity poses in real time
  → DIS serializer creates PDUs
  → Network sink sends PDUs immediately
```

### 18.2 Timed Recording

For file recording:

```text
Scenario model
  → Generator computes timed PDU records
  → Records are bulk written to .disrec
```

### 18.3 File Replay

For replay:

```text
.disrec file
  → Bulk read timed PDU records
  → Replay scheduler waits for record offsets
  → Network sink sends PDU bytes
```

### 18.4 Playback Speed

Playback speed shall modify scheduled send time.

Example:

```text
1.0x  → normal timing
2.0x  → twice as fast
0.5x  → half speed
10.0x → ten times faster
```

If original timestamp offset is `T`, effective send time is:

```text
T / playbackSpeed
```

---

## 19. Threading Model

### 19.1 Required Behavior

Network generation, recording, and replay shall run on worker threads so the MFC UI remains responsive.

### 19.2 Threading Primitive - std::thread

The application shall use **`std::thread` from the C++ Standard Library**, not MFC's `CWinThread`/`AfxBeginThread`.

Rationale:

- `std::thread` is the modern primitive; `CWinThread` predates `<thread>` by 15+ years and offers nothing the standard library doesn't.
- Standard synchronization (`std::mutex`, `std::condition_variable`, `std::atomic`, `std::jthread` if C++20 is enabled per §3.2) composes cleanly with non-MFC code, which keeps the option open to extract subsystems into a non-MFC test harness.
- Avoids the gotcha where MFC requires `AfxBeginThread` for any code that touches MFC objects from a worker — we'll keep workers MFC-free.

**Workers shall not touch MFC objects directly.** All UI updates flow back to the main thread via `::PostMessage` to the main dialog's HWND (see §19.4).

Recommended worker threads:

```text
LiveGenerationThread    samples motion, marshals via Open-DIS, sends via socket
RecordingThread         same as Live, but writes to .disrec instead of sending
ReplayThread            reads .disrec, schedules sends against playback clock
```

Only one scenario operation should run at a time in v1.

### 19.3 Worker Skeleton

```cpp
class ScenarioWorkerBase
{
public:
    void Start(HWND uiHwnd) {
        m_uiHwnd = uiHwnd;
        m_stop.store(false);
        m_thread = std::thread([this]{ Run(); });
    }

    void RequestStop() { m_stop.store(true); }

    void Join() {
        if (m_thread.joinable()) m_thread.join();
    }

protected:
    virtual void Run() = 0;

    void PostStatusToUI(PlaybackState state) {
        ::PostMessage(m_uiHwnd, WM_APP_PLAYBACK_STATUS,
                      static_cast<WPARAM>(state), 0);
    }

    HWND              m_uiHwnd = nullptr;
    std::atomic<bool> m_stop{false};
    std::thread       m_thread;
};
```

### 19.4 UI Communication

Worker threads shall communicate with the UI thread using:

- `::PostMessage` to the main dialog HWND (never `SendMessage` — that blocks the worker until the UI handles it)
- A thread-safe command queue for UI-to-worker commands (start/pause/resume/stop), implemented with `std::mutex` + `std::condition_variable` or a lock-free SPSC queue
- `std::atomic<bool>` stop/pause flags polled by the worker
- `std::mutex` where shared state needs more than atomic access

Example custom Windows messages:

```cpp
#define WM_APP_PLAYBACK_STATUS   (WM_APP + 100)   // wParam = PlaybackState
#define WM_APP_PLAYBACK_PROGRESS (WM_APP + 101)   // wParam = scenarioTime ms, lParam = pdusSent
#define WM_APP_PLAYBACK_ERROR    (WM_APP + 102)   // wParam = error code
#define WM_APP_VALIDATION_RESULT (WM_APP + 103)   // lParam = ValidationReport*
```

For posted messages carrying pointer payloads (validation reports, error structs), allocate on the heap in the worker and free in the message handler. Document ownership at the message-id site.

### 19.5 Cancellation

Start, pause, resume, and stop shall be thread-safe.

Recommended state variables:

```cpp
enum class PlaybackState
{
    Idle,
    Running,
    Paused,
    Stopping,
    Stopped,
    Completed,
    Error
};
```

---

## 20. Validation

### 20.1 Validation Panel

The application shall include a validation results panel. The validation summary shall be visible from the command/control area where the user launches and controls the scenario. The user shall not need to switch tabs to see whether the scenario has errors, warnings, informational messages, estimated PDU count, or estimated bandwidth.

Required always-visible summary fields near launch controls:

```text
Errors
Warnings
Info
Estimated PDU Count
Estimated Bandwidth
```

Validation messages shall have severity:

```text
Error
Warning
Info
```

The full validation panel shall be accessible by clicking the summary, pressing the Validate button, or selecting Scenario > Validate.

### 20.2 Required Validation Rules

Scenario validation:

- Scenario name is not empty.
- DIS version is v6 or v7.
- Exercise ID is valid.
- Duration is greater than zero.
- Default update rate is greater than zero.
- Scenario origin exists if local coordinates are used.

Entity validation:

- Entity IDs are unique.
- Entity name is not empty.
- Entity begin time is before end time.
- Entity timing is within scenario duration.
- Update rate is greater than zero.
- DIS entity type fields are valid.
- Marking text contains only ASCII characters (warning if non-ASCII would be stripped at PDU boundary, §14.5.1).
- Marking text is 11 characters or fewer (warning if truncation would occur).

Motion validation:

- Each motion segment has start time before end time.
- Motion segments do not overlap for the same entity.
- Segment coordinates are valid.
- Circle and ellipse radius values are positive.
- Waypoint lists contain at least two points for moving segments.
- Speed values are non-negative.

Output validation:

- UDP destination address is a valid IPv4 dotted-quad.
- **Multicast address is in the valid IPv4 multicast range `224.0.0.0/4`** (first octet 224–239). Addresses outside this range shall be **rejected as errors**.
- **Multicast address in the link-local block `224.0.0.0/24`** (224.0.0.0–224.0.0.255) shall produce a **warning**. This block is reserved by IANA for routing protocols (224.0.0.1 = all hosts, 224.0.0.2 = all routers, 224.0.0.5 = OSPF, etc.) and is not forwarded by routers, which usually surprises users expecting cross-subnet delivery.
- Multicast port shall be in the user-assignable range (recommend 1024–49151); ports below 1024 produce a warning (privileged) and ports in the dynamic range 49152–65535 are accepted without comment.
- Multicast TTL of 0 produces a warning (host-local only, frequently a misconfiguration). TTL > 32 produces an info-level note (crosses many hops, may flood unintended segments).
- TCP host resolves to a valid IPv4 address; port is 1–65535.
- File path is writable for recording (probe by creating and immediately deleting a temp file in the target directory).
- Replay file exists, is readable, and has a valid `.disrec` magic + file-version (§13.2).

Reference table for the multicast checks:

```text
224.0.0.0/4         valid multicast range          accept
224.0.0.0/24        link-local control block       warning (not routed)
224.0.1.0 - 238.255.255.255  globally-scoped       accept
239.0.0.0/8         administratively-scoped        accept (private/site-scope)
< 224.0.0.0 or > 239.255.255.255   not multicast   error
```

Performance validation:

- Estimated PDU count is displayed.
- Estimated bandwidth is displayed.
- High update rates produce warnings.
- Very large recordings produce warnings.

---

## 21. Menus and Commands

### 21.1 File Menu

```text
New Scenario
Open Scenario
Save Scenario
Save Scenario As
Open Recording
Exit
```

### 21.2 Scenario Menu

```text
Validate
Generate Recording
Preview
Reset Scenario Clock
```

### 21.3 Playback Menu

```text
Start
Pause
Resume
Stop
Loop
Playback Speed
```

### 21.4 Network Menu

```text
Open Network Output
Close Network Output
Test UDP Send
Test Multicast Send
Test TCP Connection
```

### 21.5 Help Menu

```text
About
Protocol Notes
File Format Notes
```

---

## 22. Suggested Class List

### 22.1 UI Classes

```cpp
class CScenarioEditorApp;          // CWinApp-derived
class CScenarioEditorDialog;       // CDialogEx-derived, main window
class CScenarioSetupPage;          // CDialogEx child
class CAssetEntityEditorPage;      // CDialogEx child
class CMotionPathEditorPage;       // CDialogEx child
class COutputPlaybackPage;         // CDialogEx child
class CPreviewPage;                // CDialogEx child
class CValidationPanel;
class CPreviewCanvasWnd;           // Direct2D drawing surface
```

Note: no `CDocument`/`CView`/`CFrameWnd` classes — this is a dialog-based app per §3.3 / §5.1.

### 22.2 Scenario Model Classes

```cpp
class ScenarioDefinition;
class ScenarioOrigin;
class EntityDefinition;
class EntityTypeDefinition;
class MotionTimeline;
class MotionSegmentDefinition;
class OutputDefinition;
class ValidationResult;
class ValidationReport;
```

### 22.3 Motion Classes

```cpp
class IMotionSegment;
class StationaryMotionSegment;
class StopMotionSegment;
class LineMotionSegment;
class CircleMotionSegment;
class EllipseMotionSegment;
class WaypointMotionSegment;
class MotionEngine;
```

### 22.4 Coordinate Classes

```cpp
class CoordinateEngine;
struct LatLonAlt;
struct LocalENU;        // East-North-Up tangent plane at origin (§15.2)
struct EcefXYZ;
struct EntityPose;
```

### 22.5 DIS Classes

```cpp
// ScenarioEditor-owned (thin facade over Open-DIS, §14.2)
namespace scn::dis {
    class Serializer;     // facade: scenario model -> bytes via Open-DIS
    class Reader;         // facade: bytes -> decoded variant via Open-DIS
    class BinaryReader;   // for .disrec file framing (§14.7)
}

// External (Open-DIS C++, namespace DIS::)
// We use these directly from Serializer/Reader; they do not leak elsewhere:
//   DIS::EntityStatePdu, DIS::CreateEntityPdu, DIS::RemoveEntityPdu
//   DIS::DataStream, DIS::EntityID, DIS::EntityType, DIS::Marking, ...
```

### 22.6 Recording Classes

```cpp
class TimedPduRecord;
class PduBulkBuffer;
class DisrecWriter;
class DisrecReader;
class ReplayScheduler;
class PlaybackClock;
```

### 22.7 Network Classes

```cpp
class WinsockRuntime;
class INetworkSink;
class UdpUnicastSink;
class UdpMulticastSink;
class TcpClientSink;
class TcpServerSink;
```

### 22.8 Worker Classes

```cpp
class ScenarioWorkerBase;
class LiveGenerationWorker;
class RecordingWorker;
class ReplayWorker;
```

---

## 23. Versioned Implementation Scope

### 23.1 Version 1 Scope - UI Layout Only

Version 1 shall create the MFC/Win32 user interface shell and lay out all tab controls and child window controls. Version 1 shall not implement scenario generation, DIS serialization, networking, file recording, file replay, or motion math.

V1 purpose:

```text
Build the Visual Studio 2022 MFC dialog-based application shell.
Create the main dialog (CScenarioEditorDialog : CDialogEx).
Attach a menu, toolbar, and multi-pane status bar.
Create the tab control.
Create all five child tab dialog pages.
Lay out all controls.
Wire only basic UI navigation and placeholder command buttons.
Do not add code-behind business logic yet.
```

V1 required tabs:

```text
1. Scenario Setup
2. Asset / Entity Editor
3. Motion Path Editor
4. Output / Playback
5. Preview
```

V1 main window controls:

```text
Menu bar
Toolbar
Status bar
Main form view
Tab control
Always-visible command/control strip
Validation summary area
Optional full validation panel placeholder
```

V1 Scenario Setup tab controls:

```text
Scenario Name edit
Description multiline edit
DIS v6 radio button
DIS v7 radio button, selected by default
Exercise ID numeric edit
Site ID numeric edit
Application ID numeric edit
Scenario Start Time date/time picker
Duration Seconds numeric edit
Default Update Rate Hz numeric edit
Default Coordinate Mode combo box
Scenario Origin Latitude edit
Scenario Origin Longitude edit
Scenario Origin Altitude edit
Validate Before Send checkbox
Validate Before Record checkbox
Validate Before Replay checkbox
```

V1 Asset / Entity Editor tab controls:

```text
Asset tree control on the left
Add Asset button
Delete Asset button
Duplicate Asset button
Move Asset button
Validate Asset button
Entity detail panel on the right
Enabled checkbox
Name edit
Description edit
Marking Text edit
Force ID combo box
Entity Kind combo box
Domain combo box
Country edit
Category edit
Subcategory edit
Specific edit
Extra edit
Site ID edit
Application ID edit
Entity ID edit
Coordinate Mode combo box
Lat/Lon/Alt edits
Local X/Y/Z edits
ECEF X/Y/Z edits
Heading/Pitch/Roll edits
Initial Speed edit
Begin Time edit
End Time edit
Update Rate edit
```

V1 Motion Path Editor tab controls:

```text
Entity Selector combo box
Motion Segment Timeline list control
Add Segment button
Delete Segment button
Duplicate Segment button
Move Up button
Move Down button
Validate Segment button
Selected Segment Detail group box
Segment Enabled checkbox
Segment Start Time edit
Segment End Time edit
Motion Type combo box
Coordinate Mode combo box
Description edit
Dynamic placeholder area for Stationary, Stop, Line, Circle, Ellipse, and Waypoint fields
Waypoint list control placeholder
Import CSV button placeholder
Export CSV button placeholder
```

V1 Output / Playback tab controls:

```text
Output Mode radio buttons:
    UDP Unicast
    UDP Multicast
    TCP Direct Connection
    File Recording
    Preview Only
UDP Unicast settings group
UDP Multicast settings group
TCP settings group
File Recording settings group
File Replay settings group
Playback controls group
Start button
Pause button
Resume button
Stop button
Loop checkbox
Playback Speed combo box with 0.25x, 0.5x, 1x, 2x, 10x
```

V1 Preview tab controls:

```text
Preview canvas placeholder
Preview Start button
Preview Pause button
Preview Resume button
Preview Stop button
Time slider
Zoom In button
Zoom Out button
Fit Scenario button
Show Labels checkbox
Show Trails checkbox
Show Paths checkbox
Show Orientation Vectors checkbox
```

V1 command/control strip controls:

```text
Validate button
Preview button
Generate button
Record button
Replay File button
Send Live button
Start button
Pause button
Resume button
Stop button
Loop checkbox
Playback Speed combo box
Scenario Time display
Progress bar
Playback State display
Errors count display
Warnings count display
Info count display
Estimated PDU Count display
Estimated Bandwidth display
```

V1 deliverable:

```text
A compilable Visual Studio 2022 MFC dialog-based application with the complete window and tab layout, resource IDs, placeholder controls, and no scenario-generation code behind the controls.
```

### 23.2 Version 2 Scope - Scenario, DIS, Network, Recording, and Replay

Version 2 shall implement the first working functional scenario composer and player.

Version 2 should include:

```text
.ini scenario save/load
Entity table/model
Asset tree population from scenario data
Stationary motion
Stop/Hold motion
Line motion
Ellipse motion
Entity State PDU generation
DIS v7 implementation first
DIS v6/v7 scenario selection retained
Internal DIS serialization classes
Lat/Lon/Alt support
Local X/Y/Z support
ECEF support
UDP unicast output
UDP multicast output
Timed .disrec file recording
Timed .disrec playback to UDP/multicast
Validation panel
Estimated PDU count
Estimated bandwidth
Worker-thread playback/generation/recording
Start/Pause/Resume/Stop/Loop controls
Playback speed: 0.25x, 0.5x, 1x, 2x, 10x
```

TCP support may be started in Version 2 if time permits, but UDP unicast, UDP multicast, file recording, and file replay should be prioritized first.

### 23.3 Later Version Scope

Later versions may add:

```text
TCP direct connection support completion
Create Entity PDU
Remove Entity PDU
Fire PDU
Detonation PDU
Circle motion
Waypoint motion
Full map preview
PCAP export
Receive mode
Scenario comparison tools
Advanced visual preview
```

---

## 24. Future Enhancements

Possible future features:

```text
Undo / redo for form-based edits
Full map preview
3D preview mode
PCAP export
Telemetry CSV import
KML import/export
EGM96 / EGM2008 geoid model for MSL-to-ellipsoidal altitude conversion (§15.1.1)
More DIS PDU types
Fire and detonation events
Formation movement
Scripted entity behavior
Network receive mode
Scenario comparison tool
Recording trim/split/merge
Unreal Engine visualizer bridge
HLA support
Plugin architecture for custom motion generators
Absolute (UTC-synced) DIS timestamps as a per-output-target option (§14.4.1)
Dead-reckoning algorithms 3/4/6/7/8/9 in the motion engine (§16.4)
```

---

## 25. Design Principle

The key design principle is:

```text
The user defines the scenario declaratively through forms.
The application generates time-based DIS behavior from that scenario.
The user should not have to manually construct packets or answer a long prompt wizard.
```

Scenario authoring, PDU generation, network sending, recording, and replay should remain separate subsystems.

This separation will make the program easier to test, easier to extend, and easier to connect later to a DIS viewer, Unreal Engine visualization, or other simulation tools.
