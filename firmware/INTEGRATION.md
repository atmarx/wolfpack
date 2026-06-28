# Wolfpack — firmware integration (slices 2–3: broadcast + two-up compass HUD)

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
| + Wolfpack slice 3 | **88.8% — 723928 B** | 46.9% — 116596 B |

Cost of slice 3 (the HUD): **+2560 bytes flash (+0.31 pts)** over slice 2, static
RAM unchanged (the candidate arrays in `drawFrame` are stack-local). Roughly
**~89 KB of flash headroom remains**. The build's own nRF52 guards confirm it:
the image ends 73 KB clear of the warm/bootloader region.

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
