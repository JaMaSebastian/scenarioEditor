# The NPC `Behavior` Line — dis-service Contract & ScenarioEditor Integration

**Purpose:** everything ScenarioEditor needs to author a `Behavior=` line in `scenario.ini`
that `dis-service` will read and honour, without guessing.

**Source of truth:** `noda-ai/planeswalker`, branch
`feat/URZA-11265/taiwan-update-npc-dynamic-authoring-capability`
(checkout `~/projects/dev/planeswalker`). Ticket **URZA-11265**.

Reference files:

| File | Role |
|---|---|
| `dis-service/src/scenario/Scenario.h` | `enum class NpcBehavior`, `Entity::behavior` |
| `dis-service/src/scenario/ScenarioReader.cpp` | `ParseBehavior()`, entity key read order |
| `dis-service/src/playback/BehaviorOverride.h/.cpp` | where a held entity is placed |
| `dis-service/src/playback/PlaybackEngine.cpp` | load / freeze / resume / rewind |
| `dis-service/src/api/RestController.cpp` | `GET /npcs`, `POST /npcs/behavior` |
| `dis-service/src/config/IniFile.cpp` | INI parsing rules the file must satisfy |
| `dis-service/README.md` §10 | operator-facing description |

---

## 1. The line itself

```ini
[Entity.13]
Enabled=1
Name=Vanilla-2
Description=
Behavior=Directed          ; or Stationary
ForceID=Friendly
SiteID=1
...
```

**The whole contract:**

| Property | Value |
|---|---|
| Section | `[Entity.<EntityID>]` — the entity section, **not** `Entity.<N>.Motion.<M>` |
| Key | `Behavior` |
| Legal values | `Directed`, `Stationary` — **these two only** |
| Default when key absent | `Directed` |
| Value matching | **case-insensitive** (`stationary`, `STATIONARY`, `Stationary` all work) |
| Unrecognised value | falls back to `Directed`; the load **does not fail** |
| Schema version | additive optional key — `[Format] Version` stays `1` on both sides |

`ScenarioReader.cpp` reads it immediately after `Description` and before `ForceID`:

```cpp
e.behavior = ParseBehavior(ini.GetString(sec, "Behavior", ""), e.behavior);
```

INI key/section lookup is case-insensitive and the **first** value for a duplicated key
wins (Win32 `GetPrivateProfile` semantics), so the editor's existing
`WritePrivateProfileString` round-trip is already compatible — no writer changes beyond
adding the key.

### `Resume` is not a value

`POST /npcs/behavior` accepts `resume` as a **command verb** meaning "back to Directed".
It is deliberately not a member of `NpcBehavior` and **must never be written to a file** —
a persisted `Resume` cannot say what it resumes to. ScenarioEditor writes only `Directed`
or `Stationary`.

---

## 2. What dis-service does with it

### At scenario load (`PlaybackEngine::BuildTracksLocked`)

An entity authored `Behavior=Stationary` starts **frozen**, at the pose it would have held
at the start of its own emission window (`max(0, BeginSecond)`):

```cpp
if (e.behavior == NpcBehavior::Stationary) {
    tr.frozen     = true;
    tr.frozenPose = BehaviorOverride::Resolve(e, scn, tr.windowStart, FreezeRequest{}).pose;
}
if (!tr.frozen) BuildRingLocked(tr);   // held entities skip path precompute
```

Two consequences worth knowing when authoring:

- **Motion segments are not consumed but not discarded.** The authored `Motion.<M>` legs stay
  exactly as written; freezing is a runtime override on the track. An operator resuming the
  entity rejoins the path *at the current scenario time* — not where it stopped.
- **A frozen entity gets no arc-length ring.** Resume builds one on demand. This only matters
  for single-Ellipse orbits, where the ring is what gives editor/service parity.

### Where a held entity is actually placed (`BehaviorOverride::Resolve`)

This is the part most likely to surprise an author. For `Behavior=Stationary` with no
explicit coordinates:

| Component | Rule |
|---|---|
| Latitude | live sampled position at freeze time |
| Longitude | live sampled position at freeze time |
| Altitude | **start altitude of the motion leg active at that instant** — *not* the live altitude |
| Heading / Pitch / Roll | always live sampled (a frozen entity keeps pointing where it was) |

The altitude rule has three documented exceptions where the live altitude is used instead,
reported as `altitudeSource: "liveSampled"`:

1. **`StopHold` legs** — carry no authored coordinates by design; taking their start yields
   the scenario origin (a hole in the ground).
2. **`Ellipse` with `FollowEntityId >= 0`** — re-centres on a moving target every frame, so
   its "start" is wherever the target was when the leg began.
3. **`Line` legs that change altitude by > 1.0 m** (compared in the leg's own `coordMode`,
   Local `Z` vs geodetic `Alt`; ECEF legs are exempt from this check) — freezing a climb-out
   at 1500 m would otherwise resolve to the 100 m runway.

**Authoring implication:** an entity you intend to author as `Behavior=Stationary` is placed
most predictably when its window start sits on a level `Line` or a static `Ellipse`, or when
it has no motion segments at all (then it simply holds its initial pose).

### On Stop → Play (`ResetTracksLocked`)

Behavior **rewinds with the clock**. On restart every track is reset to its *authored* value:
an entity an operator froze at t=200 is released, and one whose file says
`Behavior=Stationary` is re-frozen even if an operator had released it during the previous
run. The file is the only thing "start from the beginning" can honestly mean.

**So the authored `Behavior=` line is the durable state; REST changes are session-scoped.**

---

## 3. The runtime REST surface (what operators see)

Authoring must stay consistent with these, since sim-ui drives them.

### `GET /npcs`

Served from the **loaded scenario in memory, never the `.ini` on disk** — the file carries
authored values only and cannot say where an entity is now.

```json
{"success":true,"npcs":[
  {"entityId":13,"siteId":1,"applicationId":1,"forceId":1,
   "name":"Vanilla-2","marking":"VAN-2","enabled":true,
   "behavior":"directed",
   "latitudeDeg":30.4275,"longitudeDeg":-86.6893,"altitudeMeters":1200.0}
]}
```

`behavior` is reported **from the live track**, not from `Entity::behavior` — after a freeze
the two disagree. A disabled entity has no track and reports its authored value.

Note the JSON values are **lower-case** (`"directed"` / `"stationary"`) while the INI values
are capitalised. Both are accepted on input; match the case of the surface you are writing to.

### `POST /npcs/behavior`

```json
{"entityId":13,"behavior":"stationary",
 "latitudeDeg":30.43,"longitudeDeg":-86.69,"altitudeMeters":1500}
```

- The entity rides in the **body**, not the path (the HTTP router matches method+path exactly
  and has no path parameters). The body is **flat** — the JSON reader is a substring search
  with no structural awareness, so nesting only works by accident.
- `behavior` ∈ `{directed, resume, stationary}` (case-**sensitive** here, unlike the INI key).
- `latitudeDeg` / `longitudeDeg` / `altitudeMeters` are each **independently optional**.
  There is **no sentinel** for "keep current" — omit the field. `0` and `-1` are real
  altitudes and are honoured as positions.
- Bounds: lat ±90, lon ±180, altitude −12 000 … 5e7 m. `entityId` must be a whole number
  0…65535.

Errors: `INVALID_ENTITY_ID`, `INVALID_BEHAVIOR`, `INVALID_POSITION` (400);
`ENTITY_NOT_FOUND` (404, deliberately distinct from the router's `NOT_FOUND` for a bad path);
Conflict when no scenario is loaded, or when the entity is `Enabled=0` and therefore has no
playback track.

---

## 4. ScenarioEditor integration

The editor currently has **no** behavior support — `grep -n "NpcBehavior\|Behavior" src/Scenario.h
src/ScenarioIO.cpp` returns nothing. Four edits, all mirroring the existing `ForceID` pattern.

### 4.1 `src/Scenario.h` — add the enum and the field

Add near the other enums (the service declares it after `EllipseDirection`):

```cpp
// What an entity does with its authored motion at runtime (URZA-11265).
// Directed = play the motion segments as written (the default, and the only
// value a scenario authored before this key existed can have).
// Stationary = hold one fixed pose; the segments stay untouched.
// There is deliberately no Resume member: Resume is a *command* meaning
// "back to Directed", never a state an entity rests in.
enum class NpcBehavior : uint8_t
{
    Directed   = 0,
    Stationary = 1,
};
```

In `struct Entity` (currently `src/Scenario.h:263`), immediately after `description`
(line 268) so the field order matches dis-service's `Entity`:

```cpp
    // Runtime motion behavior (URZA-11265). Directed unless the file says
    // otherwise, so every scenario written before this key existed keeps
    // playing exactly as it did.
    NpcBehavior behavior       = NpcBehavior::Directed;
```

### 4.2 `src/ScenarioIO.cpp` — name/parse helpers

Alongside `ForceIdName` / `ParseForceId` (`src/ScenarioIO.cpp:68-95`):

```cpp
    //
    // BehaviorName — map NpcBehavior to the INI token dis-service reads.
    //
    const wchar_t* BehaviorName(NpcBehavior b)
    {
        return b == NpcBehavior::Stationary ? L"Stationary" : L"Directed";
    }
    //
    // ParseBehavior — case-INSENSITIVE, matching dis-service's reader. That is
    // deliberate, not an oversight: for a key that decides whether an entity
    // moves at all, a spelling mismatch is a scene that looks authored and
    // plays wrong. Unknown values fall back rather than failing the load.
    //
    NpcBehavior ParseBehavior(const std::wstring& v, NpcBehavior fallback)
    {
        std::wstring low;
        low.reserve(v.size());
        for (wchar_t c : v) low.push_back(static_cast<wchar_t>(::towlower(c)));
        if (low == L"directed")   return NpcBehavior::Directed;
        if (low == L"stationary") return NpcBehavior::Stationary;
        return fallback;
    }
```

Do **not** add a numeric fallback the way `ParseForceId` does — dis-service does not accept
`Behavior=1`, so accepting it in the editor would let you author a file the service silently
reads as `Directed`.

### 4.3 `src/ScenarioIO.cpp` — Save

In the entity write loop, after the `Description` line (`src/ScenarioIO.cpp:548`) and before
`ForceID`:

```cpp
        WriteStr   (S, L"Behavior",       BehaviorName(e.behavior), path);
```

Writing the key unconditionally (including `Directed`) is safe: older dis-service builds
never query it and ignore it entirely; current builds read `Directed` as the default anyway.

### 4.4 `src/ScenarioIO.cpp` — Load

In the entity read loop, after `Description` (`src/ScenarioIO.cpp:897`):

```cpp
        e.behavior       = ParseBehavior(ReadStr(sec, L"Behavior",
                                                 BehaviorName(e.behavior), path),
                                         e.behavior);
```

### 4.5 UI surface

Wherever `ForceID` is offered on the entity page (`AssetEntityEditorPage` /
`ScenarioEditorDialog`), a two-item combo — **Directed** / **Stationary** — is the whole
control. Sensible affordances, given §2:

- Label it *starting* behavior. It is the value playback rewinds to, not a live command.
- If you preview, mirror the service: a `Stationary` entity holds the pose at
  `max(0, BeginSecond)` and its motion path is not walked. `src/MotionSampler.cpp` in the
  editor is the same sampler dis-service ports, so `EvaluateSegment(seg, 0.0, &scenario)` at
  the active leg reproduces the altitude rule exactly.
- Consider warning when `Behavior=Stationary` is set on an entity whose window start lands on
  a `StopHold` leg, a follow-`Ellipse`, or a climbing `Line` — those are the three cases where
  the held altitude comes from the live sample rather than the authored leg, which is the one
  place authored intent and runtime result can differ.

---

## 5. Compliance checklist

- [ ] Key is spelled `Behavior`, in `[Entity.<EntityID>]`, never in a `Motion` subsection.
- [ ] Only `Directed` or `Stationary` is ever written. Never `Resume`, never a number.
- [ ] `[Format] Version` is **not** bumped — this is an additive optional key.
- [ ] No duplicate `Behavior` keys in one section (first wins; a stale second line is invisible).
- [ ] File is `;`/`#` full-line comments only, `key=value`, CRLF tolerated — the service's
      `IniFile` skips blank lines and trims surrounding whitespace.
- [ ] Round-trip test: Save → `ScenarioReader::Load` in dis-service → `GET /npcs` reports
      `"behavior":"stationary"` for the entities you marked, before any REST command is sent.
- [ ] Rewind test: Play, freeze a *Directed* entity via `POST /npcs/behavior`, Stop, Play —
      it must come back Directed. Freeze nothing on a *Stationary* entity, resume it, Stop,
      Play — it must come back Stationary.
- [ ] Disabled-entity test: `Enabled=0` + `Behavior=Stationary` loads without error, has no
      track, and `POST /npcs/behavior` against it answers Conflict — not a crash.

---

## 6. Caveat on provenance

The URZA-11265 work above lives on an **unpushed local branch** in
`~/projects/dev/planeswalker`; it is not yet on `origin`. Re-verify against the merged
`develop` before treating any line number here as final — the semantics are stable, the
line references are a snapshot.
