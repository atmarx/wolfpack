# Wolfpack — firmware integration (slice 2: color/role broadcast)

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
  table (`NodeNum`, color, role, `lastHeardMs`) for the upcoming screen frame.

All wire/parse logic is in `WolfpackProtocol.h`, which is dependency-free and
unit-tested on the host.

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
and the `wp_isSameTeam` truth table.

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
| + Wolfpack slice 2 | **88.5% — 721368 B** | 46.9% — 116596 B |

Cost of this slice: **+896 bytes flash (+0.11 pts)**, static RAM unchanged (the
32-entry peer table is heap-allocated inside the `new`'d module, not static).
Roughly **~92 KB of flash headroom remains** — keep the screen-frame slice lean.

## Notes / decisions

- **PortNum:** uses `meshtastic_PortNum_PRIVATE_APP` (256) directly, so no
  protobuf regen is needed. If a stable distinct port is wanted later, pick an
  unused value in 258–511 and run `bin/regen-protos.sh` (note 257 = ATAK_FORWARDER).
- **Heading/compass** is out of scope for slice 2; this board has no
  magnetometer, so the screen-frame slice will rely on GPS-course heading via
  `Screen::estimatedHeading()` (see the slice-1 recon).
- `wp_parseColorRole` resolves color and role **independently** — e.g. `"XL"`
  yields (NONE, LEADER) and `"RX"` yields (RED, NONE).
