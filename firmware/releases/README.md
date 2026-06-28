# Wolfpack — flashable firmware artifacts

Prebuilt UF2 images so you can flash without setting up the full Meshtastic
build toolchain. Built from this repo's module source dropped into upstream
**meshtastic/firmware `2.8.0` (`ec5d230`)**, target `seeed_wio_tracker_L1`.

| File | Slice | What you'll see |
|---|---|---|
| `wolfpack-slice4-seeed_wio_tracker_L1-2.8.0.uf2` | 2 + 3 + 4 | On-device **team picker** + the **two-up compass HUD** (distance + bearing arrow to your two nearest teammates) |

## Flash an L1 (nRF52, USB UF2 only)

1. Data USB-C in, then **double-tap RESET** → a USB drive named **`Tracker L1`** mounts.
2. Copy the `.uf2` onto that drive → it flashes and reboots itself.
3. On first boot with no team set, the **team picker pops up**: pick a color
   (Red/Orange/Yellow/Green/Blue/Violet), then a position (Lead/Mid/Sweep). Click
   the HUD frame any time to change it. Lead is one-per-color; extra Mid/Sweep get
   auto-numbered (`RM`, `RM2`, …).
4. **Battery calibration:** stock ec5d230 reads the L1 battery ~21% low. Fix it
   per node (persists across flashes): `meshtastic --set power.adc_multiplier_override 2.54`

> ⚠️ **Never flash over BLE / NRF-OTA on the L1** — it can brick the board. USB
> UF2 only. A wrong *app* UF2 is harmless (double-tap back to the drive, drop a
> good one); only a bad *bootloader* bricks, and normal flashing never touches it.

Config (names, `US_915` region) persists across a UF2 app-flash.
