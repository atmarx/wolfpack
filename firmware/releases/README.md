# Wolfpack — flashable firmware artifacts

Prebuilt UF2 images so you can flash without setting up the full Meshtastic
build toolchain. Built from this repo's module source dropped into upstream
**meshtastic/firmware `2.8.0` (`ec5d230`)**, target `seeed_wio_tracker_L1`.

| File | Slice | What you'll see |
|---|---|---|
| `wolfpack-slice3-seeed_wio_tracker_L1-2.8.0.uf2` | 2 + 3 | Color/role beacon **+ the two-up compass HUD** (distance + bearing arrow to your two nearest teammates) |

## Flash an L1 (nRF52, USB UF2 only)

1. Data USB-C in, then **double-tap RESET** → a USB drive named **`Tracker L1`** mounts.
2. Copy the `.uf2` onto that drive → it flashes and reboots itself.
3. Set each node's **short-name** so it parses to a team + role — first char is
   color (`R/Y/G/B`), second is role (`L/M/T`): e.g. `RL`, `RM`, `RT` for a Red
   leader/mid/tail. Wrong/blank name → the HUD just says "Set short-name".

> ⚠️ **Never flash over BLE / NRF-OTA on the L1** — it can brick the board. USB
> UF2 only. A wrong *app* UF2 is harmless (double-tap back to the drive, drop a
> good one); only a bad *bootloader* bricks, and normal flashing never touches it.

Config (names, `US_915` region) persists across a UF2 app-flash.
