# Wolfpack — firmware integration (slices 2–4: broadcast + HUD + team picker)

This `firmware/` subtree holds the **canonical, hand-written** Wolfpack module
source. It is *not* a fork of the Meshtastic tree — it's the set of files you
drop into a real `meshtastic/firmware` checkout, plus the one registration line.

Built and verified against upstream **meshtastic/firmware `2.8.0` (git `ec5d230`)**,
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

Still only the slice-2 `Modules.cpp` registration — everything else is contained
in `WolfpackModule` + the pure helpers in `WolfpackProtocol.h`.

## L1 battery calibration (upstream regression — important)

Stock ec5d230 ships the `seeed_wio_tracker_L1` variant with `ADC_MULTIPLIER 2.0`,
which **reads the battery ~21% low** on our boards (measured 3.93 V at the cell,
firmware reported 3.10 V). Older Meshtastic firmware read it correctly, so this is
an upstream calibration regression for the L1. The board's true divider is ~**2.54**
(`2.0 × 3.93 / 3.10`).

Fix without reflashing — per node, persists in config across firmware updates:

```bash
meshtastic --set power.adc_multiplier_override 2.54
```

(Optional permanent bake: patch the variant's `ADC_MULTIPLIER 2.0 → 2.54`. Worth
reporting upstream.)

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

1. Add the include alongside the other module includes (e.g. just after the
   `ReplyBotModule.h` block near the top):

   ```cpp
   #include "WolfpackModule.h"
   ```

2. Register it inside **`setupModules()`** — drop it next to the
   `// new ReplyModule();` example line (in the upstream 2.8.0 tree this is in
   the `setupModules()` body, right after the `PowerStressModule` block):

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

| Build | Flash | RAM (static) |
|---|---|---|
| Stock 2.8.0 | 88.4% — 720472 B | 46.9% — 116596 B |
| + Wolfpack slice 2 | 88.5% — 721368 B | 46.9% — 116596 B |
| + Wolfpack slice 3 | 88.8% — 723928 B | 46.9% — 116596 B |
| + Wolfpack slice 4 | **89.0% — 725400 B** | 46.9% — 116644 B |

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
