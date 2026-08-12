# Wolfpack

**Meshtastic firmware for mountain-bike coaches: live distance and bearing to
every teammate, on the radio's own screen.  No phone, no cell service, no cloud —
the pack is the network.**

Wolfpack started with a real problem: a youth mountain-bike team strung out along
a wooded trail, zero cell coverage, and a sweep coach who wants to know how far
ahead the lead has gotten.  The answer turned out to be a $30 LoRa tracker
zip-tied to the handlebars.  Each radio beacons its position to the pack; every
screen shows an arrow and a distance to each teammate, readable at a glance
without taking a hand off the bars.  And the compass is honest — `?` means "I've
lost them," never "you stopped moving."  When a teammate's GPS dies mid-ride they
age out to `?` on everyone else's screen, so the failure mode is its own
loose-antenna detector.

## What's on the screen

Two teammates per screen, each in their own cell:

```
 ┌──────────────────┬──────────────────┐
 │ 7s            >NE│ 42s              │
 │   RL    ◤        │   RS     NE      │
 │        210m      │        380m      │
 └──────────────────┴──────────────────┘
```

- **`RL` / `RS`** — who it is.  Color initial + role (Lead / Mid / Sweep).
- **`210m`** — real, meter-scale distance.  Full-precision position rides inside
  our own beacon, so it sidesteps the 5.8 km cells stock position packets get
  truncated to.
- **The arrow** — where they are *relative to your bars*, while you're moving.
  Stopped, it becomes an absolute cardinal (`NE`) instead of lying to you: the
  radio has no magnetometer, so heading only exists when you're rolling.
- **`7s`** — how old that teammate's fix is.  A number that keeps climbing is a
  teammate you're losing.
- **`>NE`** — the *ghost of the lead*: the direction the lead departed **from the
  spot you're standing on**.  It appears when you're within ~30 m of their
  recorded track.  On plain trail it just says "onward."  At a fork it's the
  answer the sweep came for.
- **`?`** — honestly lost.  Not "you stopped."

At the trailhead, the coach riding Lead clicks the screen and picks **Start
Ride**: every teammate's ghost trail clears and every screen reads
`Red Team Is Rolling!`, so today's route never inherits yesterday's breadcrumbs.

## Hardware

- **Seeed Wio Tracker L1** — nRF52840 + L76K GNSS, ~$30.  No magnetometer, no
  IMU (which is why the compass works the way it does).
- Meshtastic **v2.7.26** stable.
- 6 color teams × 3 roles (Lead / Mid / Sweep), picked on the device itself — no
  serial cable, no phone.

## Flash it

Prebuilt UF2s are in [`firmware/releases/`](firmware/releases/) so you don't need
the toolchain.

1. Plug in a **data** USB-C cable, then **double-tap RESET** — a drive named
   `Tracker L1` mounts.
2. Drag the `.uf2` onto it.  It flashes and reboots itself.
3. First boot with no team set pops the picker: choose a color, then a position.

> **USB UF2 only.  Never flash an L1 over BLE / NRF-OTA — it can brick the
> board.**

Full flashing notes, config knobs, and the tuning you'll actually want (timezone,
modem preset) are in [`firmware/releases/README.md`](firmware/releases/README.md).

## What's in here

This repo is **not** a fork of the Meshtastic firmware.  It's the module you drop
into one, plus everything needed to reproduce the build:

| Path | What |
|---|---|
| [`firmware/src/modules/`](firmware/src/modules/) | The module.  `WolfpackProtocol.h` is pure, dependency-free logic (wire format, geo math, ghost ring); `WolfpackModule.cpp` is the thin Meshtastic-facing shell. |
| [`firmware/INTEGRATION.md`](firmware/INTEGRATION.md) | How to graft it into an upstream checkout — two lines in `Modules.cpp` — plus what every slice added and what it cost in flash. |
| [`firmware/MESHTASTIC-INTERNALS.md`](firmware/MESHTASTIC-INTERNALS.md) | Field notes on Meshtastic itself: the position pipeline, precision truncation, airtime budgets, banner and screen internals.  Written because we had to learn it the hard way. |
| [`firmware/releases/`](firmware/releases/) | Prebuilt UF2s. |
| [`firmware/test/`](firmware/test/) | Host-native tests for the pure logic — no hardware needed. |
| [`web/`](web/) | The team map SPA.  Runs on a mock ride today; the live path is Web Bluetooth. |
| [`docs/PHONE-MAP.md`](docs/PHONE-MAP.md) | Why the phone map exists and what it does that the official app doesn't. |
| [`WOLFPACK.md`](WOLFPACK.md) | The original design blueprint. |
| [`BRINGUP.md`](BRINGUP.md) | Day-one setup for a fresh set of radios. |

## Build it yourself

```bash
git clone --depth 1 --branch v2.7.26.54e0d8d --recurse-submodules \
  --shallow-submodules https://github.com/meshtastic/firmware.git
# copy firmware/src/modules/Wolfpack* into src/modules/, apply the two
# Modules.cpp edits from INTEGRATION.md, then:
pio run -e seeed_wio_tracker_L1
```

The pure logic is host-testable without a radio — see
[`firmware/INTEGRATION.md`](firmware/INTEGRATION.md).

## The phone map

The radios are the product; the phone is the enhancement, never a dependency.
The map SPA in [`web/`](web/) talks straight to a radio over **Web Bluetooth** —
no app store, no server in the path, no cell service.  It reads the Wolfpack
beacon directly, so it sees full-precision positions at the pack's real update
rate rather than the truncated, rate-limited ones a generic client gets.

Today it runs on a mock ride.  The live connection is the next slice.

## License

**GPL-3.0.**  Wolfpack builds into
[meshtastic/firmware](https://github.com/meshtastic/firmware), which is GPL-3.0,
and the firmware images in [`firmware/releases/`](firmware/releases/) are
combined works — so the same license applies here.  See [LICENSE](LICENSE).

Meshtastic® is a registered trademark of Meshtastic LLC.  This is an independent
project and is not affiliated with or endorsed by them.

## Status

Slices 2–9 are shipped and running on real radios: on-device team picker,
two-up compass HUD, full-precision positions in the beacon, honest compass
states, per-teammate fix age, the ghost of the lead, chip-course heading, and
Start Ride.  Field validation is ongoing — the trail is the test suite that
counts.
