# Wolfpack — flashable firmware artifacts

Prebuilt UF2 images so you can flash without setting up the full Meshtastic
build toolchain. Built from this repo's module source dropped into upstream
**meshtastic/firmware `v2.7.26` (`54e0d8d`)** — the current stable release tag —
target `seeed_wio_tracker_L1`.

| File | Slice | What you'll see |
|---|---|---|
| `wolfpack-slice6-seeed_wio_tracker_L1-2.7.26.uf2` | 2–6 | Team picker + two-up compass HUD, positions **carried in the beacon** (real meter-scale distances), and an **honest compass** — relative arrow while moving, absolute cardinal (`NE 200m`) while stopped, `?` only when a teammate's fix has actually gone stale |

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

## Reading the compass (slice 6)

The L1 has no magnetometer, so it can't sense which way it points while still —
it borrows heading from your GPS course-over-ground. The HUD stays honest about
that, gating on two separate facts:

- **Moving, teammate's fix fresh** → a **relative arrow**: point yourself at it
  and ride.
- **Stopped, teammate's fix fresh** → an **absolute cardinal** in the rose plus
  distance (`NE 200m`). We know exactly where they are; you just have to know
  where north is, because we can't rotate it to your body without a compass.
  Roll a few meters and it becomes a relative arrow.
- **Teammate's fix stale** (no position beacon in ~150 s — out of range, or their
  GPS dropped) → a `?` and a `~`-prefixed last-known distance. The `?` means
  *"I've lost track of where they are,"* never *"you're standing still."*

That last state is also a **loose-GPS-cable detector**: a radio that boots with a
fix then loses its antenna keeps beaconing (LoRa's fine) but stops sending
positions, so its teammates' HUDs flip it to `?`. Reseat the connector and it
snaps back to a cardinal/arrow.

> ⚠️ **Never flash over BLE / NRF-OTA on the L1** — it can brick the board. USB
> UF2 only. A wrong *app* UF2 is harmless (double-tap back to the drive, drop a
> good one); only a bad *bootloader* bricks, and normal flashing never touches it.

Config (names, `US_915` region) persists across a UF2 app-flash.

Battery reads correctly on this build. The earlier **0% + USB icon** was a
regression in upstream `develop`'s reworked `Power.cpp`, not a hardware quirk —
building on the `v2.7.26` stable tag reads the L1's fuel gauge fine.
