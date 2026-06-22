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

### One mesh, one key, color is app-layer

Every radio shares one primary Meshtastic channel + **one PSK** — all friendlies on
the same mesh. "Color" and "role" are **not** separate channels or keys; they're
attributes each node advertises, and the display filters by color.

Why one shared key: it keeps every radio a full participant, so every node provably
**relays for every other node** — the resilience that's the whole point. If a rider
gets separated and can't reach their own teammates directly, any nearby node *of any
color* hears them and pushes their position into the mesh until it lands with their
group. Other teams' radios are your relays, even though you never see their dots.
Per-color keys would only add complexity (and partial blindness) among people who all
trust each other anyway. Multiple teams run follow-the-leader at once on the one mesh;
the only per-color thing is what each screen chooses to show.

(This also settles the old airspace question: other people's LoRa meshes nearby share
the airwaves but can't decrypt us, and we ignore them. Coexistence is via PSK + modem
preset, not separate frequencies.)

### Display tiers (ordered for the hardware we actually have)

- **Tier 0 — Radio OLED (the goal, standalone):** the 128×64 screen on the Heltec
  draws the bearing arrow + distance; button cycles targets / marks hazards. The
  must-have, and what the custom firmware module is for.
- **Tier 1 — Phone on the mount (the enhancement you've got):** hook the radio to a
  phone and the **official Meshtastic app already gives the full map, every node's
  distance + bearing, and the node list — today, on stock firmware, zero custom
  code.** This is what shows up working at the next event. A self-hosted single-page
  app for the team-timelapse view comes later — **design + a runnable mock are staged:
  see [docs/PHONE-MAP.md](docs/PHONE-MAP.md) and [`web/`](web/).**
- **Tier 2 — Garmin watch (future / if a coach has one):** glanceable wrist HUD over
  BLE — we have a working Connect IQ prototype in the archive (see Found Assets), but
  nobody on the crew owns a watch yet, so it's parked. *(For the record: BLE direct
  from the ESP32, no phone, no ANT+ — ANT+'s shared-source license is permissive but
  adds the adopter / network-key dance for zero benefit over BLE. Skip it.)*

> **The goal is radios-alone — phone is just the safety net.** The real Wolfpack is
> the standalone radio: follow-the-leader on the OLED, no phone, multiple teams at
> once. That needs the custom firmware below, and that's the build. *If* the firmware
> isn't ride-ready by the next event, there's a zero-code break-glass: flash stock
> Meshtastic on one private channel and the official phone app shows the whole pack on
> a map — so you still show up holding radios that genuinely work. Safety net, not the
> target. We're driving for the OLED.

### The firmware: a small module, not a from-scratch HUD

Big correction from the prior-art scan — **stock Meshtastic already draws distance +
a bearing arrow to other nodes on the device OLED, no phone, using GPS
course-over-ground as your heading.** It's real and shipping today:
`src/graphics/draw/NodeListRenderer.cpp` (`drawNodeListWithCompasses`,
`drawNodeDistance`, `drawCompassArrow`) plus `Screen.cpp` (`estimatedHeading` derives
heading from GPS movement; a `hasCompass` path uses a real magnetometer when one's
present). There's even a per-node "favorite" full-screen frame — basically "point me
at THIS teammate." That's **~75% of Tier 0, free.**

So we don't build a HUD — we **fork meshtastic/firmware and add a small "Wolfpack"
module** on top of what's there:

- a `SinglePortModule` on a `PRIVATE_APP` port that broadcasts each node's
  **color + role** and reads everyone else's — the one concept stock has no notion of,
- one bespoke Screen frame: a big arrow to **your leader** + a compact roster of your
  color, reusing the stock `drawCompassArrow` / `drawNodeDistance` calls,
- the **tail-lag alert** (screen blink + buzz),
- hazard marking via native Meshtastic **waypoints** (already in stock).

Build skeleton is well-trodden: copy `ReplyModule`, register in
`src/modules/Modules.cpp`, wire the frame into the `Screen` frame list the way the
favorite-node frames do. The Module API is documented.

**Rejected alternative:** stock firmware + a companion device doing the math
re-introduces a phone/Pi/watch as a *dependency* — violates the core rule. The module
is the only path to "radios alone run the show," and it's now a small one.

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

- **The arrow needs your heading — this is the real risk, not code volume.** The
  radio always knows **distance + bearing to your leader** (pure math from both GPS
  fixes — never stale). What it *can't* know without a compass is **which way you
  face** — it infers that from motion (`estimatedHeading` = GPS course-over-ground), so
  it's good while rolling and unknown at a standstill. Critically: stop and physically
  turn the bike and a *held* arrow would be **confidently wrong** until you roll ~10m
  and it re-syncs. So v1 handles the stop honestly, no magnetometer:
  - **Flag it, don't fake it.** On stop, the arrow switches to a "heading lost — roll
    to re-sync" state and falls back to honest numbers ("Leader 200m, bearing 040°,
    North ↑"). A truthful compass-rose beats a lying arrow.
  - **Warmer/colder junction mode.** At a fork you don't need your facing at all — pick
    a branch, roll 20 ft, watch the distance to the leader. Shrinking = right way;
    growing = back up. Pure distance gradient, zero heading, and it works exactly where
    trails twist and cross. A Wolfpack original the asset-trackers never needed.
  - Label any moving arrow "direction of travel," NOT a compass-N — stock draws it like
    a magnetic compass, which misleads (Meshtastic issue #9928).
  - **Magnetometer deferred, not rejected.** It's the clean fix for the
    stopped-and-repositioned case — but the stock case has only a GNSS bay, and a mag
    jammed next to the radio/battery distorts (needs calibration) and wants
    tilt-compensation on non-level bars. So we add a tilt-compensated 9-axis IMU
    (ICM20948-class) in the 3D-printed bike case, isolated from the radio — *if* field
    testing shows warmer/colder + the honest stale-state aren't enough.
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

## Node hardware

**Bike-rider nodes (need a screen + continuous GNSS):**
- **CHOSEN: Seeed Wio Tracker L1** (~$31 bare) — nRF52840 + SX1262 + **continuous L76K
  GNSS built in**, 1.3" OLED, 2000 mAh, official Meshtastic. **3 ordered** for the first
  color team (leader/middle/tail). Built-in GNSS removes the "confirm GNSS" problem
  entirely; the 1.3" screen beats the Heltec's 0.96" for a handlebar. Bare L1, not the
  ~$47 Pro (its case gets replaced by a printed mount). No WiFi (BLE only) — non-issue:
  phone pairs over BLE, homelab map is dog-side. **Bring-up: see [BRINGUP.md](BRINGUP.md).**
- **Also fine: Heltec LoRa V4** (SX1262, ESP32-S3, 0.96" OLED) — already on the way.
  Confirm onboard GNSS or add a module. Good bench/test units regardless.
- ⚠️ **Do NOT buy the Wio Tracker *1110*** — it's the LR1110 *snapshot*-GPS board
  (periodic asset pings, not live fixes); it won't drive a smooth arrow.

**Dog-collar nodes (no screen; small, sealed, long battery):**
- **Recommended: Seeed SenseCAP T1000-E** (~$40) — credit-card size, 32 g, IP65,
  nRF52840, continuous GNSS, motion IMU, screenless. Purpose-built wearable tracker.
  (This is the nRF52-class collar the project originally spec'd — same family as the
  ThinkNode M3 / Houdini clips.)

**Both:**
- 915 MHz antenna (US_915).
- Bike nodes: 18650(s) from the salvaged 40V pack (the L1 also has its own 2000 mAh).
- 3D-printed handlebar / backpack mount (bike) — xram designing.
- Optional ~$1 piezo buzzer per bike node for the tail-lag alert if the board lacks one.

## Found assets (mined from `~/Code-archives/meshTracker`)

The old meshTracker clone has a **working Garmin Connect IQ watchapp** prototype —
`TrackerApp.mc`, `BLEManager.mc`, `TrackerView.mc` — that already does "distance +
bearing arrow to a mesh node on your wrist," offline over BLE. That's the Tier 2
watch layer most of the way built — parked until someone on the crew owns a watch.
It also has battle-tested Meshtastic config scripts (backcountry
setup, sub-GHz enablement) and the protobufs. We don't start the watch layer cold.

## Open decisions for xram

1. **Magnetometer — deferred to the custom-case phase** (was leaning yes; the stock
   case flips it). v1 runs GPS-course heading, an honest "heading lost — roll to
   re-sync" state when stopped, and a warmer/colder distance-gradient mode for
   junctions — no extra parts, fits the stock case as-is. A real compass (tilt-comp
   9-axis IMU, isolated spot) is a 3D-printed-case job. **Skip it for v1.** Still worth
   a ~$1 piezo buzzer per node now (tail-lag alert) if the board has none.
2. **Confirm onboard GNSS** on the V4s *and* the $34 kit — *moot if you standardize on
   the Wio L1*, which has continuous GNSS built in. GPS is mandatory; it computes every
   arrow. (Bare Heltec LoRa boards have none; the Wio L1 and SenseCAP T1000 both do.)
3. **Start small or full 12?** Recommend flashing **3 nodes first** (one color:
   leader + middle + tail), prove it on a real ride, then scale.
4. **Repo home?** Committed locally — point me at a Gitea remote (or say go and I'll
   create one) so CI builds firmware on push.

## Build phases

- **Phase 0 (now):** spec + repo scaffold + prior-art scan. ✅
- **Phase 1 — prove it on STOCK firmware:** flash 2–3 nodes with stock Meshtastic,
  one private channel, GPS on. Confirm the stock node-list **Distance mode already
  shows distance + bearing arrows** on the OLED. Validates ~75% of Tier 0 before we
  write a line of code.
- **Phase 2 — add the Wolfpack module:** `SinglePortModule` broadcasting color/role +
  one custom Screen frame (big arrow to your leader + team roster). Magnetometer
  decision lands here.
- **Phase 3:** tail-lag alert + hazard waypoints + button cycling within your color.
- **Phase 4:** real-ride field test with 3 nodes; tune airtime + heading UX.
- **Phase 5:** scale to 12; then phone/map/watch enhancements as wanted.
