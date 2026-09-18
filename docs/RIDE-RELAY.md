# Ride Relay — phones as the second network

**Status:** design, agreed in #meshtastic 2026-09-16. Not built.

The radios are the network on the trail. This is what happens when a coach also
has a bar of cell signal: their phone forwards what their radio heard, so a
parent at the lot — or at home — watches the same ride. It adds nothing to the
LoRa channel, because it rides the cell network instead.

```
radio ─LoRa─ radio ─BLE─ coach's phone ─cell─ ride relay ─ parent's phone
                              └─ that phone's own GPS (opt-in)
```

## The ride code is a night

A ride code is generated at the trailhead and stands for one evening's practice.

- A coach opens the map and taps **I'm riding**. The first coach of the night
  creates the code; everyone else enters it. Their phone starts relaying every
  beacon its radio hears, and (opt-in, separately) its own GPS.
- A parent opens the share link, or taps **I'm watching** and enters the code.
  They see the dots. Nothing they do can put anything on the map.
- **Afterwards the code still works.** It opens that night's ride in the
  timelapse the map already has: scrub the whole practice, watch the pack string
  out and come back together.

Most parents won't look. The ones who do get to see the ride, and that is the
whole feature.

## Two codes, because writing is not reading

| | Who holds it | What it allows |
|---|---|---|
| **Ride code** (`PNPK-4K2C`) | coaches | relay, and read |
| **Share link** (`/r/<view token>`) | parents of riders present | read, live and afterwards |

Possession is the permission — there are no accounts. That's the right trade for
a volunteer coach in a parking lot at dusk, and it's why the two are separate: a
share link that leaked can never inject a fake rider onto the map.

Codes are random, not guessable, and short enough to read aloud. The share link
is what actually gets texted.

## What the relay learns about a kid

Less than you'd think, and this is deliberate.

- A beacon carries **color, role, position, ride epoch** — that's the whole
  payload. The relay stores those. **No names.** On the wire a rider is `R-Mid`,
  not a child's name, so the server never holds one.
- **Coach radios and coach phones only.** No rider's phone runs the relay.
- Positions live in the ride and die with it: **30 days by default**, then
  deleted. Any coach can delete a ride immediately from the map.
- Self-hosted next to the map. No third party, no analytics, no ad SDK — nothing
  on the page that we didn't put there.

## How it works

**Relay (coach's phone).** Already connected to a radio over Web Bluetooth, so
it's already decoding beacons. It batches them and POSTs every ~5 s. When cell
drops it queues (a bounded ~30 min ring) and flushes when signal comes back —
the ride fills in behind them as they climb out of the creek valley.

**Dedupe.** Three coaches hear the same beacon and all three forward it. The
Meshtastic packet id plus the sending node is the key; the relay keeps the first
and drops the rest. Their phone GPS is a separate source: if a phone is paired to
its coach's own radio, its fixes fill that radio's gaps rather than appearing as
a second dot.

**Viewer.** A live stream (SSE — survives a reverse proxy, reconnects itself)
into the same timeline the map already builds, so the scrubber works on a live
relayed ride exactly as it does on a Bluetooth one. Replay afterwards is one GET
of the night's samples.

**Server.** Small: ride create, sample POST, event stream, ride GET, delete. One
SQLite file. Behind the same origin as the map so there is no CORS and no second
hostname. Rate-limited per relay, because a coach's phone that gets stuck in a
loop shouldn't fill the disk.

## What it does not do

- **Background GPS.** A browser tab gets location only while it's in the
  foreground with the screen on. A bar-mounted phone works (the page holds a wake
  lock); a phone in a jersey pocket goes quiet. Real background tracking means
  wrapping the map in an Android shell — later, if ever.
- **Replace the radios.** No cell, no relay; the mesh carries on exactly as it
  does today and the coaches' screens never notice.
- **Public sharing.** There is no discoverable list of rides, ever.

## Open

- Retention: 30 days is a proposal, not a decision.
- Does the base station want a "who's missing" roll call, given the relay finally
  knows the whole pack in one place?
