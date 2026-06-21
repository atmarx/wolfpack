# Wolfpack

Follow-the-leader for mountain bike coaches. LoRa mesh radios that show every
rider which way the pack went — no phone, no signal, no problem.

> Status: spec'd, building. Hardware inbound. This doc is the blueprint.

## The problem it solves

Convoluted trail systems. Groups that split and branch and cross. Walkie-talkies
that die or don't carry through the trees. A coach at the back needs one thing:
*which way did the pack go, and how far ahead are they?* Wolfpack puts a distance
+ direction-to-pack readout on the handlebars.

## How it works (one sentence)

12 LoRa radios in one private mesh; each one knows where every pack member is and
draws an arrow + distance to whoever you're chasing.

## The core requirement that drives everything

**The radio alone runs the show.** No phone required. Glanceable while moving.
Everything else — watch, phone map — is an enhancement layered on top, never a
dependency. This single rule is what forces a custom firmware module (below).

## Teams & roles

- 4 color teams: **red, yellow, green, blue** — ~3 coaches each.
- Roles within a color: **leader, middle, tail.**
- Leader presses "leader" → screen shows distance/direction to their followers.
- Followers see a big arrow + distance to the leader, smaller readout to the
  other follower. Button cycles targets.
- All 12 radios share ONE mesh so packets relay for max range — color is a filter
  on top, not a separate network.

## Architecture

### One mesh, color is app-layer

All 12 share one primary Meshtastic channel + PSK (private to us). That gives
maximal rebroadcast/relay range across the whole pack. "Color" and "role" are
**not** separate radio channels — they're attributes each node advertises, and the
display filters by color. (This also settles the old airspace question: other LoRa
meshes nearby share the airwaves but can't decrypt us, and we ignore them.
Coexistence is via PSK + modem preset, not separate frequencies.)

### Display tiers (ordered for the hardware we actually have)

- **Tier 0 — Radio OLED (the goal, standalone):** the 128×64 screen on the Heltec
  draws the bearing arrow + distance; button cycles targets / marks hazards. The
  must-have, and what the custom firmware module is for.
- **Tier 1 — Phone on the mount (the enhancement you've got):** hook the radio to a
  phone and the **official Meshtastic app already gives the full map, every node's
  distance + bearing, and the node list — today, on stock firmware, zero custom
  code.** This is what shows up working at the next event. A self-hosted single-page
  app for the team-timelapse view comes later.
- **Tier 2 — Garmin watch (future / if a coach has one):** glanceable wrist HUD over
  BLE — we have a working Connect IQ prototype in the archive (see Found Assets), but
  nobody on the crew owns a watch yet, so it's parked. *(For the record: BLE direct
  from the ESP32, no phone, no ANT+ — ANT+'s shared-source license is permissive but
  adds the adopter / network-key dance for zero benefit over BLE. Skip it.)*

> **Day-one fallback — this cashes the check immediately.** The radios are useful
> the moment they arrive, before any custom firmware exists: flash **stock
> Meshtastic**, one private channel + PSK, position sharing on. Your phone on the
> mount runs the Meshtastic app → the whole pack on a map, distance + bearing to
> every node. That's a real, working Wolfpack at the next bike event. The custom
> firmware below is the *upgrade* that removes the phone for coaches who'd rather
> ride without a screen out. Two horizons; the first needs zero code.

### The firmware decision (the big fork)

Stock Meshtastic does position-sharing but **not** an on-device bearing HUD with no
phone. To hit Tier 0 we **fork meshtastic/firmware and add a "Wolfpack" module**
(C++ / PlatformIO). It:

- reads the position packets every node already floods (native, free),
- adds a tiny private packet carrying color + role,
- builds a table of `nodeid → {color, role, distance, bearing, last_seen}`,
- renders custom OLED frames, handles button input, and fires the tail-lag alert.

**Rejected alternative:** keep stock firmware + a companion device doing the math.
That re-introduces a phone/Pi/watch as a *dependency* — violates the core rule. So:
firmware module it is. It's real C++ and a flash-test loop, but it's the only path
to "radios alone run the show."

### Reuse from native Meshtastic (don't reinvent)

- **Position sharing** — native, just set the interval.
- **Encryption / mesh routing / relay / power mgmt** — native.
- **Hazard marking = Waypoints** — Meshtastic already has a waypoint primitive.
  Button-hold drops a "hazard here" waypoint (downed tree, etc.) that propagates to
  the whole mesh and flashes on every screen. Free win.

## MVP scope (what ships first)

The thing that makes the embellishment true:

1. Flash V4 nodes with Wolfpack firmware (start with 3–4, scale to 12).
2. One private mesh, position sharing on.
3. OLED shows a target's **distance + direction arrow**; button cycles target.
4. Per-node color/role config.
5. Tail-lag alert: screen blink + buzz when the tail drifts past a set distance.
6. Hazard waypoint on button-hold.

**Explicitly NOT in MVP:** phone app, audio, geofencing, timelapse map. Those come
after the radios prove out on a real ride.

## Key engineering notes (the stuff that bites)

- **The arrow needs your heading.** Bearing-to-target is easy from two GPS fixes.
  But to draw an arrow that *points*, the radio must know which way YOU face. Stock
  Heltec has no magnetometer — so we derive heading from **GPS course-over-ground**,
  which is accurate *while moving* (perfect for biking) and unavailable when stopped
  (we show numeric bearing + a "stopped" hint then). This is the one place the
  Garmin watch is strictly better — it has a real compass — which is why Tier 1 is
  attractive.
- **Airtime is the scaling limit.** 12 nodes beaconing position on a shared channel
  plus multi-hop relay adds up fast. We tune three knobs on real hardware: position
  interval (smart-broadcast, faster while moving), hop limit (one trail system —
  keep hops low), and modem preset (LongFast for range vs MediumFast for less
  airtime). This is measured, not guessed.
- **Tail-lag rule (simple v1):** if the max distance between same-color nodes
  exceeds a threshold, the whole color team's screens blink + buzz. Tune on trail.
- **Power:** salvaged 18650s; GPS is the big draw. Smart-broadcast + screen sleep
  should carry a node through a full practice. Confirm a buzzer — if the board has
  none, add a tiny piezo on a GPIO for the audible alert.

## Audio? (answering the open question from 5/26)

Short version: **not for MVP.** 10s of even low-bitrate codec2 (~700–1200 bps) is
~1 KB and takes multiple seconds of airtime on LongFast — it monopolizes the shared
channel that everyone's position beacons need, and pushes duty-cycle limits. On a
short-range fast preset it drops to ~1s and becomes a fun *experiment* later, but it
trades away the range that's the whole point. For MVP the radio shows direction;
voice stays on the walkie-talkies — and Wolfpack is the backup for exactly when
those fail. Canned text/tone alerts are cheap and we can add those.

## Node BOM (per radio)

- Heltec LoRa V4 (SX1262, ESP32-S3) **with onboard GNSS** — or the $34 kit board
  **if** it has GNSS; if not, add a GNSS module. GPS on every node is mandatory —
  it's what computes distance + bearing.
- 915 MHz antenna (US_915 — confirmed).
- 18650 cell(s) from the salvaged 40V pack.
- 3D-printed handlebar / backpack mount (xram designing).
- Optional piezo buzzer for the tail alert if the board lacks one.

## Found assets (mined from `~/Code-archives/meshTracker`)

The old meshTracker clone has a **working Garmin Connect IQ watchapp** prototype —
`TrackerApp.mc`, `BLEManager.mc`, `TrackerView.mc` — that already does "distance +
bearing arrow to a mesh node on your wrist," offline over BLE. That's the Tier 2
watch layer most of the way built — parked until someone on the crew owns a watch.
It also has battle-tested Meshtastic config scripts (backcountry
setup, sub-GHz enablement) and the protobufs. We don't start the watch layer cold.

## Open decisions for xram

1. **Confirm the V4 board variant has onboard GNSS** (Wireless Tracker-class: yes;
   bare LoRa V4: no). Same question for the $34 kit.
2. **Start small or full 12?** Recommend flashing **3 nodes first** (one color:
   leader + middle + tail), prove the HUD on a real ride, then scale. Lower risk,
   faster "it works" moment to show the group.
3. **Repo home?** Committed locally now — point me at the Gitea remote (or say the
   word and I'll create one) so CI builds firmware on push.
4. Buzzer on the V4, or do we add piezos?

## Build phases

- **Phase 0 (now):** spec + repo scaffold. ✅
- **Phase 1:** fork meshtastic/firmware, Wolfpack module skeleton; position table +
  static distance/bearing readout on the OLED for 2 bench nodes.
- **Phase 2:** arrow + heading-from-course, button cycle, color/role config.
- **Phase 3:** tail-lag alert + hazard waypoints.
- **Phase 4:** real-ride field test with 3 nodes; tune airtime.
- **Phase 5:** scale to 12; then Tier 1 watch / Tier 2 map as enhancements.
