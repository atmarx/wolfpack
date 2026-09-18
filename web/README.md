# Wolfpack web — team map &amp; timelapse

A single static page. No build step, no bundler, no npm install: open it, or
serve it, and it runs.

## Two modes

**Mock ride (default).** Opens on a simulated follow-the-leader practice at
Pennypack, out of the Environmental Center: all four skill teams leave the one
parking-lot 🏁 base, fan out on different
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

## The basemap is ours

The map underneath the dots is **one file we host**: `basemap/pennypack.pmtiles`,
a 9 MB [PMTiles](https://docs.protomaps.com/pmtiles/) extract of Pennypack cut
from the OpenStreetMap-derived Protomaps build, rendered as vector tiles by
`protomaps-leaflet`. No tile server, no API key, no per-tile crawl of someone
else's CDN — and nothing at all is fetched from a third party at runtime.

That last part is why it's a file and not a tile URL: every free raster
basemap either watermarks you (CARTO stamps "API KEY REQUIRED" on keyless
tiles) or forbids bulk offline download in its usage policy. Hosting the region
ourselves is the only honest way to have a map that works in the woods.

Re-cut it when the trails change, or for a new area:

```bash
pmtiles extract https://build.protomaps.com/<YYYYMMDD>.pmtiles pennypack.pmtiles \
  --bbox=-75.10,40.01,-74.99,40.12 --maxzoom=15
```

(Build dates: <https://build-metadata.protomaps.dev/builds.json>. The renderer
draws past z15 from the same vector data, so z15 stays sharp on the bars.)

## Offline: the map works with no signal

The page is a service worker app. After one visit with a connection, the page,
its scripts, Leaflet and the renderer (all vendored under `vendor/`) load with
no network at all.

The basemap is the opt-in part: press **⬇ Save map offline** and the 9 MB
archive lands in the cache. After that the map works in the woods, and a parent
who just wants to watch dots on cell data never pays for it.

The renderer reads that archive in byte ranges, and a cached file has no server
left to answer a Range request — so the service worker slices the cached copy
itself. That's the piece `test-offline.js` leans on hardest, because getting it
wrong looks perfect online and blank on the trail.

Needs HTTPS (or `localhost`) — same rule as Web Bluetooth. In production the
origin must also serve byte ranges for the first, uncached visit; Caddy and
nginx do, `python3 -m http.server` does not.

## Tests

The wire decoding and the live timeline logic are tested off-hardware:

```bash
node web/test-protocol.js   # protobuf + beacon decoding (88 checks)
node web/test-live.js       # live peer tracking and timeline (74 checks)
node web/test-offline.js    # Range slicing + page/worker wiring (57 checks)
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
| `wolfpack-offline.js` | Basemap paths, Range slicing, "Save map offline". Shared with the worker. |
| `sw.js` | Service worker: app shell, and the saved basemap (Range-sliced). |
| `test-offline.js` | Offline tests. |
| `basemap/pennypack.pmtiles` | The map itself. OpenStreetMap via Protomaps (ODbL). |
| `vendor/leaflet/` | Leaflet 1.9.4, byte-identical to the npm release (BSD-2-Clause). |
| `vendor/protomaps/` | protomaps-leaflet 5.1.0, the vector renderer (BSD-3-Clause). |
