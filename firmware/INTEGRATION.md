# Wolfpack — firmware integration (slices 2–5: broadcast + HUD + picker + position-in-beacon)

This `firmware/` subtree holds the **canonical, hand-written** Wolfpack module
source. It is *not* a fork of the Meshtastic tree — it's the set of files you
drop into a real `meshtastic/firmware` checkout, plus the one registration line.

Built and verified against upstream **meshtastic/firmware `v2.7.26` (git `54e0d8d`)**
— the current stable release tag. (Earlier slices were built on `develop`/`ec5d230`,
which self-versions as an unreleased "2.8.0"; we rebased onto the tag for
reproducibility. Two develop-only NodeDB conveniences had to be adapted:
`copyNodePosition()` → direct `node->position`, and flattened `node->short_name`
→ `node->user.short_name`.)
target **`seeed_wio_tracker_L1`** (nRF52840, S140 v7).

## What slice 2 does

- Derives this node's **team color** (R/Y/G/B) and **role** (L/M/T) from the
  first two chars of its owner `short_name`.
- Broadcasts a 4-byte beacon `{version, color, role, flags}` on **`PRIVATE_APP`
  (portnum 256)** once a minute.
- Listens for the same beacon from other nodes and keeps a fixed 32-entry peer
  table (`NodeNum`, color, role, `lastHeardMs`) the screen frame reads.

All wire/parse logic is in `WolfpackProtocol.h`, which is dependency-free and
unit-tested on the host.

## What slice 3 adds (the two-up compass HUD)

A `MeshModule` UI frame (`wantUIFrame()` + `drawFrame()`) that renders, side by
side on the 128×64 OLED, the **two nearest same-team teammates** — each cell is
short-name on top, a compass rose in the middle, distance below. Symmetric: every
node shows the others on its team (no leader/follower asymmetry). >2 teammates →
nearest two + a `+N` marker (no silent cap; future 4th-node paging is a follow-up).

- **Positions** come from the standard Meshtastic `NodeDB` (every node already
  shares Position natively); the Wolfpack peer table only decides *who is on my
  team*. A teammate with no known position is skipped.
- **Heading** has no magnetometer on the L1, so it comes from GPS course via
  `CompassRenderer::getHeadingRadians()`. When that returns false (standstill /
  no course), the cell draws `?` in the rose and falls back to an **absolute
  cardinal + distance** ("NE 142m") — it never renders a relative arrow that
  would lie about which way you face.
- New pure, host-tested geo math lives in `WolfpackProtocol.h`:
  `wp_distanceMeters` (haversine), `wp_bearingDegrees`, `wp_cardinal8`,
  `wp_twoNearest`. Tested code == shipped code (no firmware `GeoCoord` dep).

No new files and **no `Modules.cpp` change** beyond slice 2 — the frame is
overrides on the existing `WolfpackModule`, and `wantUIFrame()` returning true is
enough because modules are constructed before Screen builds its frameset.

## What slice 4 adds (on-device team picker)

A coach sets their team on the device — no phone, no CLI. The picker rides on
Meshtastic's native banner overlay (`screen->showOverlayBanner`), so it owns input
routing, rendering, and timeout; we just supply options + a callback.

- **6 colors** (Red/Orange/Yellow/Green/Blue/Violet) then **3 positions**
  (Lead/Mid/**Sweep** — renamed from "tail"). Beacon version bumped to **2** (the
  color enum was renumbered into rainbow order; a v1 beacon is now rejected).
- **Auto-launches** from `runOnce()` on a node with no team set; **re-opens** on a
  click on the HUD frame (an `InputBroker` observer, gated by a last-draw "is my
  frame current?" proxy so it never hijacks carousel navigation).
- On confirm it **sets the owner short-name** (`owner` is a reference to
  `devicestate.owner`; persisted via `saveToDisk(SEGMENT_DEVICESTATE)`):
  - **Lead is exclusive per color** — refused if a same-color Lead is already
    heard; the picker reopens.
  - **Mid/Sweep auto-suffix** on collision: `RM` → `RM2` → `RM3`.
- **Lead-uniqueness backstop** in `handleReceived()`: if a same-color Lead appears
  from a *lower* node-number (deterministic tiebreak — they keep it), we banner
  "2x <COLOR> LEAD" and reopen the picker. Covers the out-of-range case the
  pick-time check can't.

**Picker chaining is deferred** (a bugfix over the first slice-4 build): Meshtastic
dismisses a banner via `resetBanner()` *immediately after* its selection callback
returns (`NotificationRenderer.cpp` ~653), so a callback can't open the next banner
— it gets wiped on the same tick. That made the color picker "re-prompt" forever.
Instead the color/position callbacks only *record* the choice and advance a small
`pickStep` state machine; `runOnce()` opens each next banner on a later tick, gated
on `!isOverlayBannerShowing()`, ticking at 250 ms while a pick is in flight.

Still only the slice-2 `Modules.cpp` registration — everything else is contained
in `WolfpackModule` + the pure helpers in `WolfpackProtocol.h`.

## What slice 5 adds (position rides in the beacon)

Field testing exposed that HUD distances **never settled below ~1 km**. Root
cause (full story in `MESHTASTIC-INTERNALS.md`): Meshtastic truncates every
`POSITION_APP` packet to the channel's `position_precision` — **default 13 bits
= ~5.8 km cells, center-snapped** (`Channels.cpp:156`,
`PositionPrecision.cpp:31-43`) — and rate-limits movement-triggered position
broadcasts to **one per 5 minutes** (`Default.h`). Your own GPS stays precise
locally, so the HUD was computing (my exact spot) → (teammate's cell center):
a stable km-scale error that open sky can never fix.

Private-app payloads are exempt from truncation (`PositionPrecision.cpp:70-73`),
so the **v3 beacon (12 bytes) now carries the sender's full-precision fix**:

```
[0] version=3  [1] color  [2] role  [3] flags(bit0=has_position)
[4..7] int32 latitude_i   [8..11] int32 longitude_i   (little-endian, 1e-7°)
```

- **Adaptive cadence**: 3 s tick; send when moved past the resend threshold
  since the last *sent* fix, with a 60 s heartbeat floor. Movement sends gate on
  the polite (25%) airtime ceiling, heartbeats on the hard (40%) one — pressure
  sheds fidelity first, never liveness. Beacons go out `hop_limit=1` (one relay
  tier; mid can bridge lead↔sweep).
- **Resend threshold is runtime-tunable** (no reflash) via the native smart-
  position distance knob: `meshtastic --set
  position.broadcast_smart_minimum_distance 5` for walking tests, `25` (or the
  factory `100`, treated as unset) for our riding default. See
  `wpMoveThresholdMeters()`.
- **HUD prefers beacon positions**; NodeDB is only the fallback for v2 peers.
  Peers unknown to NodeDB get a synthesized name from the beacon (`RM`), so
  cells are never nameless.
- **v2 (4-byte) beacons are still accepted** as color/role-only; v1 rejected.
- For rides with 3+ radios on LongFast, consider `--set lora.modem_preset
  MEDIUM_FAST` — 4× the airtime headroom (see the internals doc for the table).

## What slice 6 adds (honest compass states)

Field testing surfaced a UX conflation: the HUD drew a `?` whenever the *viewer*
was stationary, even though it knew exactly where the teammate was. Distance and
bearing come from the same two coordinates — only the *rotation* into a body arrow
needs GPS course. Slice 6 splits the two axes in `wpDrawCell`:

- **fresh fix + moving** → rotating rose + relative arrow + distance.
- **fresh fix + stopped** → absolute cardinal in the rose + distance (`NE 200m`),
  no `?`. (No absolute north-up *arrow* — on a handheld not held north-up that's
  the same wrong-way lie we avoid; text cardinal only.)
- **stale fix** → `?` + `~`last-known distance. `?` now means only "we've lost
  their position," never "you're standing still."

Freshness (`WP_POS_STALE_MS = 150 s`) keys on `posMs` — the last *position*-
carrying beacon — falling back to `lastHeardMs` for v2 peers. Because a
position-less heartbeat advances `lastHeardMs` but not `posMs`, a radio whose GPS
drops mid-ride ages out to `?`: a free loose-antenna detector. No new files or
Modules.cpp edits; the change is contained to `WolfpackModule.cpp`.

## What slice 7 adds (fix-age counter + 3 s tick)

Field feedback: distances "felt fuzzy" on the move. They were honest but their
*age* was invisible — the reading is your live position against the teammate's
last received fix, and at riding speed every second of beacon lag is 5–8 m of
phantom distance. Slice 7 makes the age visible instead of leaving the rider to
guess:

- **Fix-age counter**, top-left of each teammate cell: seconds since their last
  position-carrying beacon landed (`7s` → `42s` → `3m`, capped at `1h+`). Pure
  rendered age off `posMs` — no new state; it "resets" because a fresh beacon
  updates the timestamp. Hidden only when no fix was ever heard. The idle screen
  refresh is 1 fps (`IDLE_FRAMERATE`, Screen.cpp), so it visibly ticks.
- **Beacon tick 5 s → 3 s** (`WP_TEAM_TICK_MS`): the movement-resend check runs
  more often, so cadence tracks speed more tightly. 3 s is the floor worth
  having — on MediumFast a 3-node moving pack sits right at the polite 25% util
  ceiling; on LongFast the airtime gates dominate at any tick.

Contained to `WolfpackModule.cpp` (`wpFormatAge`, `wpDrawCell`); no new files,
no Modules.cpp edits, no protocol change (still v3 beacons).

## What slice 8 adds (ghost of the lead + chip-course heading)

Two field asks from the coaches, one release:

**Ghost trail.** Every non-lead teammate records the lead's position beacons
into an 8 KB ring (`WolfpackGhostTrail`, 1024 crumbs at ≥20 m spacing ≈ 20+ km
of route — pure logic in `WolfpackProtocol.h`, host-tested). When the viewer
stands within 30 m of the lead's recorded track, the lead's cell shows `>NE`
(top-right): the direction the lead **departed from this spot**. No fork
detection — on plain trail the ghost just says "onward"; at a fork it's the
answer the sweep came for. Nearest crumb wins, so on a switchback the leg
you're on beats the leg 50 m below. Honest limits: the trail only exists where
your radio *heard* the lead (mid bridging via `hop_limit=1` patches most gaps);
a lead change (re-pick) resets the ring rather than splicing two riders'
histories.

**Chip-course heading.** Upstream's `Screen::estimatedHeading` only recomputes
after 10 m of travel from a reference point — at walking speed a ~7 s-old
*average* direction, which read as "it takes too long to notice I turned." The
L76K computes course-over-ground from Doppler on every fix (1 Hz, no
displacement needed — why car dashboards feel instant), and the firmware
already captures it (`gpsStatus->getHeading()`, degrees ×1e-5).
`wpOwnHeadingRadians()` keeps upstream as the moving/stopped *gate* (slice-6
semantics untouched, `FREEZE_HEADING` respected) but takes the heading *value*
from the chip while moving. Turns now track within a fix or two.

+976 B flash, +8 KB RAM (the ring is deliberately file-scope BSS so the linker
reports it honestly). New pure functions get 4 more unity tests in
`test_wolfpack`.

## L1 battery: it's an I2C fuel gauge, not the ADC (corrected 2026-06-29)

Earlier builds set `config.power.adc_multiplier_override = 2.54` on boot, on the
theory that the L1's stock `ADC_MULTIPLIER 2.0` read the cell ~21% low (measured
3.93 V, firmware showed 3.10 V). **That theory was wrong, and the override has been
removed.**

A field test settled it: setting the override to 0, 1.0, *and* 2.54 all displayed
the same 3.10 V — the multiplier does nothing on this board. `Power::setup()` chooses
a battery provider by priority (`Power.cpp` ~786: AXP → CW2015 → MAX17048 →
lipoCharger → serial → meshSolar → **analog last**); the L1 carries an I2C **fuel
gauge** that wins, so the analog / `adc_multiplier_override` path is never reached.

That reframes the real symptom (**0% + a USB icon with nothing plugged in**): the
gauge path has its own `isBatteryConnect()` = `isBatteryConnected()` and `isVbusIn()`
= `isExternallyPowered()` (`Power.cpp` ~1538/1543). The gauge is reporting
*battery-not-connected* (→ percent hard-codes to 0, `PowerStatus.h`) and
*externally-powered* (→ USB glyph), and reads 3.10 V against a 3.93 V multimeter — an
~0.8 V error a healthy fuel gauge shouldn't have, pointing at a gauge that isn't
initializing/configured correctly on this variant.

`WolfpackModule::runOnce()` now logs the live values at INFO level
(`Wolfpack batt: hasBattery=.. hasUSB=.. charging=.. mV=.. pct=..`) to characterize
it without depending on boot-time DEBUG logs. **Open: confirm which gauge (MAX17048
vs CW2015) and whether it just needs a quickstart/reset — then it's a legitimate
upstream report.** We never filed the ADC-multiplier "regression"; it has zero effect
here and would have been noise.

## Files and destinations

| This subtree | Copy into firmware checkout at |
|---|---|
| `src/modules/WolfpackProtocol.h` | `src/modules/WolfpackProtocol.h` |
| `src/modules/WolfpackModule.h`   | `src/modules/WolfpackModule.h` |
| `src/modules/WolfpackModule.cpp` | `src/modules/WolfpackModule.cpp` |
| `test/test_wolfpack/test_main.cpp` | `test/test_wolfpack/test_main.cpp` |

`build_src_filter` for the nRF52 envs already globs `src/modules/*.cpp`, so
`WolfpackModule.cpp` is picked up automatically — no `platformio.ini` edit.

## The only edit to an existing file: `src/modules/Modules.cpp`

1. Add the include **unconditionally** near the top — do NOT nest it inside the
   `#if !MESHTASTIC_EXCLUDE_REPLYBOT` guard that wraps `ReplyBotModule.h`. The L1
   build *excludes* ReplyBot to save flash, so an include placed in that guard gets
   compiled out while the (unconditional) registration below still references the
   class → `'wolfpackModule' was not declared`. Put it right after
   `#include "modules/StatusLEDModule.h"`:

   ```cpp
   #include "WolfpackModule.h"
   ```

2. Register it inside **`setupModules()`** — drop it right after the
   `// new ReplyModule();` example line, which sits unconditionally between the
   `EXCLUDE_POWERSTRESS` and `EXCLUDE_CANNEDMESSAGES` guards:

   ```cpp
   wolfpackModule = new WolfpackModule();
   ```

That's the entire wiring. `wolfpackModule` is declared `extern` in
`WolfpackModule.h` so the future screen frame can reach the peer table.

## Build (device)

```bash
pio run -e seeed_wio_tracker_L1
# UF2 -> .pio/build/seeed_wio_tracker_L1/firmware-seeed_wio_tracker_L1-<ver>.<sha>.uf2
```

## Test (host-native, pure logic)

```bash
pio test -e native -f test_wolfpack
```

Covers: pack/unpack round-trip, short-buffer + bad-version + null rejection,
`wp_parseColorRole` across the full R/Y/G/B x L/M/T grid plus garbage/partial/null,
the `wp_isSameTeam` truth table, and (slice 3) the geo math — `wp_distanceMeters`
against a known 1°-latitude reference, `wp_bearingDegrees` on the four cardinals,
`wp_cardinal8`, and `wp_twoNearest` selection. 13 cases total.

> The `native` env compiles the full portduino firmware (`test_build_src = true`),
> so it needs the standard Meshtastic native prerequisites installed system-wide
> — notably `libyaml-cpp-dev` (plus `libi2c-dev`, `libgpiod-dev`, etc., per the
> Meshtastic "Build for Linux-native" docs). The `WolfpackProtocol.h` logic
> itself has zero such dependencies; it also compiles and passes standalone with
> a plain `g++` + Unity if the native platform isn't provisioned.

## Flash-ceiling note (important)

The `seeed_wio_tracker_L1` image is already tight against its 815104-byte app
region (the S140 v7 SoftDevice + bootloader eat the rest of the 1 MB).

Rows through slice 5 were measured on `develop` (2.8.0); the final row is the
current build on the **v2.7.26 stable base** — the numbers shift because the base
firmware differs, not our module.

| Build | Flash | RAM (static) |
|---|---|---|
| Stock develop (2.8.0) | 88.4% — 720472 B | 46.9% — 116596 B |
| + Wolfpack slice 2 (develop) | 88.5% — 721368 B | 46.9% — 116596 B |
| + Wolfpack slice 3 (develop) | 88.8% — 723928 B | 46.9% — 116596 B |
| + Wolfpack slice 4 + picker fix (develop) | 89.0% — 725720 B | 46.9% — 116644 B |
| + Wolfpack slice 5 (develop) | 89.1% — 726368 B | 46.9% — 116644 B |
| + Wolfpack slice 5 on v2.7.26 | 90.3% — 735720 B | 44.4% — 110516 B |
| + Wolfpack slice 6 on v2.7.26 | 90.3% — 735816 B | 44.4% — 110516 B |
| + Wolfpack slice 7 on v2.7.26 | 90.3% — 736040 B | 44.4% — 110516 B |
| **+ Wolfpack slice 8 on v2.7.26 (shipping)** | **90.4% — 737016 B** | **47.7% — 118708 B** |

Cost of slice 4 (the picker): **+1472 bytes flash** over slice 3 (the banner
overlay is stock — we only add options + callbacks), +48 bytes static RAM (the new
picker-state members). Roughly **~87 KB of flash headroom remains**.

## Notes / decisions

- **PortNum:** uses `meshtastic_PortNum_PRIVATE_APP` (256) directly, so no
  protobuf regen is needed. If a stable distinct port is wanted later, pick an
  unused value in 258–511 and run `bin/regen-protos.sh` (note 257 = ATAK_FORWARDER).
- **Heading/compass:** the L1 has no magnetometer, so slice 3 takes heading from
  GPS course via `CompassRenderer::getHeadingRadians()` (which wraps
  `Screen::estimatedHeading()`). No course → no arrow; we show `?` + an absolute
  cardinal instead of faking a relative bearing.
- `wp_parseColorRole` resolves color and role **independently** — e.g. `"XL"`
  yields (NONE, LEADER) and `"RX"` yields (RED, NONE).
