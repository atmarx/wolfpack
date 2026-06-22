# Phone Map (Tier 1/2 enhancement)

The colored team map + ride **timelapse** — the part the official Meshtastic app
*doesn't* do. Build-while-we-wait, because it leans entirely on the stable Meshtastic
client API and **has zero dependency on the Wolfpack firmware module.**

## Why a custom app at all (vs the official one)

The official Meshtastic app already gives a generic live map + node list over BLE —
**use it day one, free.** What it lacks, and why we build our own:

1. **Color-team grouping** — red/blue/green/yellow as first-class, not a flat node list.
2. **Timelapse** — scrub/replay the whole team's practice ride. "3 data points per group
   gets us a lot about lag." The thing xram got excited about.
3. A single-purpose, glanceable view we control.

So: official app for generic live viewing; this SPA for the team-colored map + timelapse.

## The load-bearing fact: it's all stable API

Everything the map needs comes from Meshtastic's **`Position`** + **`NodeInfo`**
messages, which are stable and shipping today. Per node we can count on:

- node id, long/short name
- latitude / longitude / altitude
- position timestamp
- SNR / RSSI, hops away, battery

From that alone we render dots, trails, per-node distance, last-seen, and the full
timelapse. **None of it needs our firmware module.**

- **Teams today:** derive color/role from the **short-name prefix** (e.g. `R-Lead`,
  `R-Mid`, `R-Tail`) — short names are in the stable API, so colored teams work *now*.
- **Teams later:** when the Wolfpack module ships a real color/role field, the map reads
  that instead. One small adapter swap; the rest is untouched.

## Transports (how the browser talks to a radio)

Via the official **`@meshtastic/js`** client library, which handles the protobufs:

- **BLE (Web Bluetooth)** — live, direct phone↔radio. The L1 is nRF52 = BLE only, so
  this is the live path. Works in Chrome/Edge on Android + desktop. **→ DECIDED:
  Android.** Web Bluetooth is our live path — the SPA talks straight to the radio, no
  app middleman. **iOS deferred** ("eventually we'll cover iPhone"): Apple blocks Web
  Bluetooth in Safari, so when we get there the timelapse/replay view already works on
  any browser (no live link needed), and the live side becomes a native or PWA wrapper —
  or it reuses the dog-map server feed. None of that gates what we build now.
- **Serial** — a radio on USB to a laptop (handy for dev/testing).
- **Replay** — positions logged to a file / MQTT / the homelab, fed to the map after the
  ride. This is the timelapse path and needs no live link at all.

## Two modes, one app

- **Live field view** — dots move as positions arrive (BLE on Android, or official app on iOS).
- **After-ride timelapse** — the exciting one. Log positions, scrub the ride, watch the
  pack string out and the tail lag. Shares its whole foundation (ingest → store → map)
  with the **dog-tracker web map** (`liamcottle/meshtastic-map`, MIT) — build once, reuse.

## What's buildable NOW (no hardware)

The entire front end, against mock data:

- map + team-colored dots + per-node trails
- timeline scrubber + play (the timelapse)
- a clean **data source** seam: `replay(ride)` (done) vs `connectLive()` (stubbed, wired
  when hardware lands + we know the phone OS)

→ **Staged in [`/web`](../web/) — open `web/index.html` in a browser and it runs on a
mock follow-the-leader ride right now.** Mock ride loops the Wissahickon, because of
course it does.

## When hardware lands

1. Wire the real `@meshtastic/js` BLE/Serial source into the `connectLive()` stub.
2. Point it at a Phase-1 node; confirm real positions render.
3. Add position logging → the timelapse runs on real rides.
4. Swap the short-name-prefix team hack for the module's color/role field once it ships.

## Build phases

- **Phase A (now, no hardware):** SPA shell — map, colored teams, trails, timelapse,
  mock data, transport seam. ← staged in `/web`.
- **Phase B (hardware in hand):** wire live BLE/Serial; render real Phase-1 nodes.
- **Phase C:** position logging + replay → real-ride timelapse; share the ingest layer
  with the dog map.
- **Phase D:** read color/role from the firmware module instead of name prefixes.
