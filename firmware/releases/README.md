# Wolfpack — flashable firmware artifacts

Prebuilt UF2 images so you can flash without setting up the full Meshtastic
build toolchain. Built from this repo's module source dropped into upstream
**meshtastic/firmware `2.8.0` (`ec5d230`)**, target `seeed_wio_tracker_L1`.

| File | Slice | What you'll see |
|---|---|---|
| `wolfpack-slice5-seeed_wio_tracker_L1-2.8.0.uf2` | 2–5 | Team picker + two-up compass HUD, now with **positions carried in the beacon** — real meter-scale distances instead of the ~1.4 km floor |

## Flash an L1 (nRF52, USB UF2 only)

1. Data USB-C in, then **double-tap RESET** → a USB drive named **`Tracker L1`** mounts.
2. Copy the `.uf2` onto that drive → it flashes and reboots itself.
3. On first boot with no team set, the **team picker pops up**: pick a color
   (Red/Orange/Yellow/Green/Blue/Violet), then a position (Lead/Mid/Sweep). Click
   the HUD frame any time to change it. Lead is one-per-color; extra Mid/Sweep get
   auto-numbered (`RM`, `RM2`, …).

## Why distances read ~1.4 km before slice 5

Meshtastic truncates native position packets to the channel's
`position_precision` — default **13 bits ≈ 5.8 km cells** — and rate-limits
movement broadcasts to one per 5 minutes. Slice 5 sidesteps both: the Wolfpack
beacon carries its own full-precision fix, re-sent whenever you move past the
threshold (evaluated every 5 s) with a 60 s heartbeat floor. No channel config
needed. Full story: `../MESHTASTIC-INTERNALS.md`.

**Tuning the resend distance (no reflash):**
```bash
meshtastic --set position.broadcast_smart_minimum_distance 5    # walking tests (jittery, near GPS floor)
meshtastic --set position.broadcast_smart_minimum_distance 25   # riding (also the default)
```

For rides with 3+ radios, `meshtastic --set lora.modem_preset MEDIUM_FAST` on
every radio buys 4× the airtime headroom (optional but recommended).

> ⚠️ **Never flash over BLE / NRF-OTA on the L1** — it can brick the board. USB
> UF2 only. A wrong *app* UF2 is harmless (double-tap back to the drive, drop a
> good one); only a bad *bootloader* bricks, and normal flashing never touches it.

Config (names, `US_915` region) persists across a UF2 app-flash.

Known cosmetic issue: the battery shows **0% + a USB icon** — the L1's fuel
gauge misreports; radios run fine on battery. Diagnosis in progress.
