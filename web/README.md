# Wolfpack web — team map &amp; timelapse

Phase A: runs on **mock data**, no hardware, no build step.

## Run it
Open `index.html` in Chrome / Edge / Firefox. Or serve it:
```bash
cd web && python3 -m http.server 8080   # then http://localhost:8080
```
(Needs internet for the Leaflet library + OpenStreetMap tiles.)

## What you're seeing
A simulated follow-the-leader practice: two teams (Red, Blue) looping the Wissahickon,
each with Lead / Mid / Tail strung out by realistic lag. Press ▶ to play the timelapse,
drag the scrubber to scan the ride, watch each team's **spread** (lead→tail distance)
update live.

## Where real data plugs in
One seam — `DataSource` in `index.html`:
- `replay(ride)` — **implemented**; drives the map from a full ride (mock now, a logged
  ride later).
- `connectLive()` — **stubbed**; wire `@meshtastic/js` (BLE / Serial) here when hardware
  lands. Nothing else in the app changes.

Teams come from the device short-name prefix (R/B/G/Y) — stable Meshtastic API, so it
works before the firmware module exists. Design notes: [`../docs/PHONE-MAP.md`](../docs/PHONE-MAP.md).
