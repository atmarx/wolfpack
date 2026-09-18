# Wolfpack web — team map &amp; timelapse

A single static page. No build step, no bundler, no npm install: open it, or
serve it, and it runs.

## Two modes

**Mock ride (default).** Opens on a simulated follow-the-leader practice: all
four skill teams leave the one parking-lot 🏁 base, fan out on different
bearings, loop their own terrain, and converge back — beginners stay close, the
advanced group ranges farthest and drops off-mesh mid-ride so you can see the
greyed last-known behaviour. Needs no hardware.

```bash
cd web && python3 -m http.server 8080   # then http://localhost:8080
```

**Live (Web Bluetooth).** Press **Connect radio** and pick one of your L1s. The
page reads Wolfpack beacons straight off the radio and plots the pack in real
time. The same scrubber then works on the live ride, so you get replay of
today's practice for free — rewind while you're still standing in the parking
lot.

## What the live path needs

| Requirement | Why |
|---|---|
| **Chrome or Edge on Android** (or desktop) | Web Bluetooth. Safari and iOS don't implement it — the mock and replay views still work there, the live link doesn't. |
| **HTTPS**, or `localhost` | `navigator.bluetooth` is refused outside a secure context. This is why the map needs a real origin like `wolfpack.xram.net`, not a file:// path. |
| A radio **on the pack's channel and PSK** | Packets from another channel arrive encrypted. The page detects that case and says so rather than showing an empty map for no visible reason. |
| Radios running **slice 9** firmware | v4 beacons carry the ride epoch. v3 and v2 beacons still plot; they just never trigger a Start Ride trail clear. |

## Why it reads our beacon, not standard position packets

Meshtastic truncates native position packets to the channel's
`position_precision` — 13 bits by default, about **5.8 km cells** — and
rate-limits movement broadcasts to one per five minutes. That's the wall slice 5
hit on the device.

The Wolfpack beacon is a `PRIVATE_APP` payload, and private payloads are exempt
from that truncation. It carries full-precision latitude and longitude every few
seconds. So this map sees **better data than a generic Meshtastic client can** —
real metre-scale positions at the pack's actual update rate.

It also means the map understands things a generic client can't: team colour and
role come from the beacon, and **Start Ride** clears the on-screen trails at the
same moment it clears them on the radios.

## Offline: the map works with no signal

The page is a service worker app. After one visit with a connection, the page,
its scripts and Leaflet (vendored under `vendor/`, nothing from a CDN) load with
no network at all.

The **basemap** is the part that needs planning. Press **⬇ Save map offline**
while you still have signal, with the practice area on screen: it downloads
every tile for that view from a few zoom levels out down to z18 (capped at 3000
tiles), and the worker serves them from cache first after that. Tiles you simply
panned past with signal are kept too. Positions always arrive over Bluetooth and
never needed a network.

Needs HTTPS (or `localhost`) — same rule as Web Bluetooth.

⚠ **The current tile source is watermarked.** CARTO now stamps "API KEY
REQUIRED" across keyless basemap tiles. The map is still readable underneath,
but the tile source has to change — and the ones that allow bulk offline
download are the ones we host ourselves.

## Tests

The wire decoding and the live timeline logic are tested off-hardware:

```bash
node web/test-protocol.js   # protobuf + beacon decoding (88 checks)
node web/test-live.js       # live peer tracking and timeline (74 checks)
node web/test-offline.js    # tile math + page/worker wiring (88 checks)
```

`test-protocol.js` writes its own encoders, independently of the decoder and
mirroring the firmware's `wp_packBeacon` byte for byte, so a round-trip proves
something rather than proving the decoder agrees with itself.

`test-live.js` extracts the inline script from `index.html` and runs it against
stub DOM and Leaflet objects. What it actually guards is the failure you could
never debug in a field: that every rider's timeline arrays stay aligned as
people join late, drop out of range, and come back.

## What isn't built yet

- **Ride storage / the ride relay.** Everything lives in the tab. Close it and
  the ride is gone. The design for coaches' phones relaying over cell — ride
  codes, share links, replay of a night's ride — is in
  [`docs/RIDE-RELAY.md`](../docs/RIDE-RELAY.md). Not built yet.
- **Status pills** (injury / mechanical). The renderer supports them and the mock
  demonstrates them; nothing on the wire sets them yet.

## Files

| File | What |
|---|---|
| `index.html` | The whole app — map, timeline, mock ride, and the live Bluetooth path. |
| `wolfpack-protocol.js` | Pure decoding: Meshtastic BLE UUIDs, a minimal protobuf reader, and the Wolfpack beacon parser. No DOM, no Bluetooth. |
| `test-protocol.js` | Decoder tests. |
| `test-live.js` | Live-path tests. |
| `wolfpack-offline.js` | Tile math and "Save map offline". Shared with the service worker. |
| `sw.js` | Service worker: app shell and saved tiles from cache. |
| `test-offline.js` | Offline tests. |
| `vendor/leaflet/` | Leaflet 1.9.4, byte-identical to the npm release (BSD-2-Clause). |
