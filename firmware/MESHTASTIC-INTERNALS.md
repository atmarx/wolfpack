# Meshtastic internals — the Wolfpack field guide

Everything we learned the hard way about how Meshtastic works, written down so
future-us doesn't re-derive it packet by packet.  Each section ends with what it
means for Wolfpack.  We ship on the **v2.7.26** stable tag (`54e0d8d`); every
mechanism below is verified present there, but the specific `file:line` references
were read on `develop` (`ec5d230`) and can drift a few lines against 2.7.26 —
**trust the symbol names over the line numbers.**

---

## 1. How a position actually travels between two radios

The pipeline, end to end:

```
 GPS chip (L76K, serial NMEA)
   │  ~1 Hz fixes
   ▼
 GPS thread → nodeDB->updatePosition(self)          ← self entry: FULL precision
   │
   ▼
 PositionModule (periodic broadcast)
   │  gate 1: baseline interval  — position_broadcast_secs, default 1 HOUR
   │  gate 2: "smart" broadcast  — fires on ≥100 m movement, BUT never more
   │          often than broadcast_smart_minimum_interval_secs = 5 MINUTES
   │  gate 3: channel utilization — skipped when airtime is scarce
   ▼
 applyPositionPrecision()                            ← THE TRUNCATION
   │  per-channel position_precision, default 13 bits
   ▼
 LoRa air → receiving node → PositionModule::handleReceived
   │
   ▼
 nodeDB peer entry                                   ← stores the TRUNCATED value
   │
   ▼
 anything that reads nodeDB->copyNodePosition()      ← including our HUD (pre-v3)
```

Two facts fall out of this that killed us in the field:

**Fact 1 — positions are deliberately coarse.**  `Channels.cpp:156` sets
`position_precision = 13` on every default channel ("approximate location", a
privacy feature, fail-closed since upstream #10509).  The truncation
(`PositionPrecision.cpp:31-43`) keeps the top 13 bits of the lat/lon int32 and
reports the **center** of the resulting cell:

```cpp
truncated = coordinateBits & (UINT32_MAX << (32 - precision));
truncated += (1UL << (31 - precision));   // center of the cell, deterministic
```

Cell size = `2^(32-p) × 1e-7°`.  At Philadelphia's latitude (40°N):

| precision | cell (lat × lon) | max error from center |
|---|---|---|
| 13 (default) | **5.8 km × 4.5 km** | ~3.7 km |
| 14 | 2.9 km × 2.2 km | ~1.8 km |
| 16 | 730 m × 560 m | ~460 m |
| 19 | 91 m × 70 m | ~57 m |
| 32 | exact (11 mm) | — |

**This was the "distances never settle" bug.**  Your own GPS position (nodeDB
self entry) is full-precision; your teammates' positions arrive cell-center
snapped.  HUD distance = |my exact spot → their cell center| = a stable 1–3 km
offset that no amount of open sky fixes.  Three radios in one cell all report
the *same* center, so all HUDs read ~the same wrong number.  It only jumps when
someone crosses a cell boundary.

**Fact 2 — positions are deliberately slow.**  `Default.h`: baseline broadcast
1 hour; smart (movement) broadcasts at most every **5 minutes**.  A rider at
5 m/s covers 1.5 km between smart updates.  Even at precision 32 the native
pipeline is minutes stale for a moving pack.

**The out:**  `applyPositionPrecision(packet, …)` short-circuits unless
`portnum == POSITION_APP` (`PositionPrecision.cpp:70-73`).  A `PRIVATE_APP`
payload is never truncated — it's just bytes, encrypted with the channel PSK
like everything else.  **So Wolfpack v3 carries its own full-precision
position inside the beacon and controls its own cadence.**  We stop borrowing
the native position pipeline entirely; nodeDB positions remain only as a
fallback for legacy peers.

Config-only alternative (no reflash, useful as a hypothesis test on stock
firmware): `--ch-index 0 --ch-set module_settings.position_precision 32` plus
cranking `position.broadcast_smart_minimum_interval_secs` down — but that's
three fragile knobs per radio, and the beacon fix makes us immune to all of it.

## 2. Airtime budget — why cadence and modem preset matter

LoRa is slow.  Approximate airtime for a ~30-byte frame (our 12-byte beacon +
headers + MAC):

| preset | airtime/beacon | 3 nodes @ 15 s | 6 nodes @ 15 s |
|---|---|---|---|
| LongFast (default) | ~1.1 s | ~22 % util | ~44 % util |
| MediumFast | ~250 ms | ~5 % | ~10 % |
| ShortFast | ~70 ms | ~1.4 % | ~2.8 % |

Meshtastic's own tooling treats >25 % channel utilization as "polite" ceiling
(`AirTime::isTxAllowedChannelUtil(polite=true)`, `airtime.h:71`) and ~40 % as
the hard one.  On LongFast a 3-rider pack at 15 s cadence sits *at* the polite
ceiling; 6 riders blow through it.

**Wolfpack does three things about it:**
1. Beacon cadence is adaptive: 5 s tick, but only *send* when moved past the
   resend threshold since the last sent fix, with a 60 s heartbeat floor.
   Stationary pack ≈ 1 beacon/min/node.  The threshold is **runtime-tunable**
   via `position.broadcast_smart_minimum_distance` (our default 25 m; set ~5 m
   for walking tests — near the GPS noise floor, so it jitters — 25 m+ for
   riding).  No reflash to change it.
2. Movement sends are gated on the polite util check; heartbeats on the hard
   one.  Overload sheds the *extra* fidelity first, never liveness.
3. Beacons go out with `hop_limit = 1` — one relay tier (mid can bridge
   lead↔sweep), instead of the default 3 hops of rebroadcast flood.

**Recommendation for real rides: set the pack to `MediumFast`.**  Range at
bike-pack distances is fine and there's 4× the airtime headroom.  (Config, not
firmware: `--set lora.modem_preset MEDIUM_FAST` on every radio.)

## 3. The banner/overlay system (the picker's hard lessons)

- Banners are **one-shot**: `NotificationRenderer` runs
  `alertBannerCallback(curSelected); resetBanner();` back-to-back
  (`NotificationRenderer.cpp:646-654`).  Anything a callback opens is wiped on
  the same tick.  **Chaining banners requires deferral**: callbacks record the
  choice; a state machine in `runOnce()` opens the next banner on a later tick,
  gated on `!isOverlayBannerShowing()`.
- While a banner is up, `Screen::handleInputEvent` (`Screen.cpp:2010`) feeds
  events to the banner and returns — but **module input observers can still
  fire** depending on order, and the underlying UI frame *keeps drawing beneath
  the overlay*, so "did my frame draw recently" is NOT "is my frame focused".
  Guard any click-to-act on your own flow state, not just the overlay check.
- `Observable::notifyObservers` (`Observer.h:66`) **stops at the first observer
  returning non-zero**, in registration order.  Screen registers at boot;
  modules that attach later are last in line.  Returning 1 = consumed.

## 3b. Heading without a magnetometer (why you must roll to get a bearing)

The Wio Tracker L1 has **no magnetometer and no IMU** — nothing that senses which
way the device points while still.  Meshtastic derives heading from **GPS
course-over-ground** (`Screen::estimatedHeading()`, wrapped by
`CompassRenderer::getHeadingRadians()`), which only exists when you're *moving*:
course is computed from the vector between successive fixes.  Stand still and
there is no course, so `getHeadingRadians()` returns false.

What the HUD does with that (slice 3, deliberately honest):

- **Moving** → real device-relative arrow ("teammate is 30° to your left").
- **Stopped / no course** → `?` in the compass rose plus an **absolute** cardinal
  and distance ("NE 142 m").  Distance is always correct; only the *relative*
  arrow needs motion.  We never draw a relative arrow we can't justify — a
  confident arrow pointing the wrong way is worse than an honest `?`.

So the field rule is real: **roll a few meters before trusting the arrow.**  A
stopped rider still gets range + an absolute compass bearing, but must know where
north is to use it.

Possible future softening (not built): cache the last valid heading for N seconds
after stopping.  For a bar-mounted radio on a stopped-but-not-turned bike the
cached course is still true, so a glance at a stop sign would keep the arrow.
The catch is it lies the moment the bike (or a handheld) rotates — so it'd want
a visual "stale" treatment (dashed/dimmed arrow) and a short timeout.  Flag for
discussion; the honest `?` is correct until then.

## 4. Module scheduling

- `OSThread`-based modules stagger their first `runOnce()` via
  `setStartDelay()` = `MIN_BROADCAST_DELAY + numPeriodicModules × SPACING`
  (`MeshModule.cpp:40`) — **~15 s** by the time Wolfpack registers.  Anything
  that must be alive early (input observers, auto-UI) needs an explicit short
  first interval (we use 1.5 s + a warm-up retry).
- `setIntervalFromNow(0)` from any context (callbacks, handleReceived) wakes
  the thread promptly — the safe way to poke your own state machine.

## 5. Power / battery on the Wio Tracker L1

- `Power::setup()` picks ONE battery provider by priority (`Power.cpp:786`):
  AXP PMU → CW2015 → **MAX17048** → lipo-charger → serial → meshSolar →
  analog ADC **last**.  If an I2C fuel gauge answers, the analog path — and
  with it `power.adc_multiplier_override` — is **dead code**.  (We proved this
  the dumb way: override 0 / 1.0 / 2.54 all displayed the same voltage.)
- Fuel-gauge path defines its own truth: `isBatteryConnect()` =
  `gauge->isBatteryConnected()`, `isVbusIn()` = `gauge->isExternallyPowered()`
  (`Power.cpp:1538/1543`).
- Analog path (no gauge) has **no real USB detection**: `isVbusIn()` is
  literally `getBattVoltage() > 4200 mV` (`Power.cpp:562`) — "impossibly high
  reading must mean a charger".
- `battery_level: 101` in telemetry = `MAGIC_USB_BATTERY_LEVEL`
  (`DeviceTelemetry.cpp:18,105`): sent when `!getHasBattery() || isCharging`.
  Not 101 % — a sentinel for "externally powered / no cell".
- The UI's USB glyph = `getHasUSB() && !isCharging` (`SharedUIDisplay.cpp:251`);
  percent hard-codes to 0 when `getHasBattery()` is false (`PowerStatus.h:65`).
- **Open bug (L1):** gauge reports battery-not-connected + externally-powered
  while running on a 3.93 V cell (shows 3.10 V).  Readout-only; radios run
  fine.  Next probe = raw serial boot capture (see §6).

## 6. Debug visibility gotchas

- The `meshtastic` **CLI hides firmware text logs** (`LOG_INFO/LOG_DEBUG`); it
  shows decoded protobufs.  A `LOG_*` you added is invisible over
  `meshtastic --debug`.  Raw serial (`tio /dev/ttyACM0`, 115200) shows
  everything, including boot lines — provider selection, I2C scan, etc.
- Boot-time provider logs are `LOG_DEBUG` and scroll away instantly: connect
  raw serial *first*, then single-tap reset (double-tap = bootloader).
- Config persists in LittleFS `/prefs/*.proto`, a flash region UF2 app-flashes
  don't touch.  Removing code that *wrote* a config value does not *unwrite*
  it — stale values (e.g. our old 2.54 override) survive until explicitly
  cleared or factory reset.

## 7. Wolfpack protocol summary (v3)

12-byte little-endian beacon on `PRIVATE_APP` (256), immune to position
truncation, PSK-encrypted like all channel traffic:

```
[0] version = 3        [1] color (1-6 rainbow)   [2] role (1-3 L/M/S)
[3] flags (bit0 = has_position)
[4..7]  int32 latitude_i  (1e-7 °)
[8..11] int32 longitude_i (1e-7 °)
```

v2 (4-byte, slice-4) beacons are still accepted as color/role-only — a v2 peer
shows on the HUD via the nodeDB fallback.  v1 is rejected (color enum was
renumbered).  All pack/parse/geo logic is pure and host-tested in
`WolfpackProtocol.h`; the HUD prefers beacon positions and falls back to
nodeDB only when a peer has never sent one.
