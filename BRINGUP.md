# Phase 1 Bring-Up — 3× Wio Tracker L1

Goal: prove ~75% of Wolfpack on **stock Meshtastic**, before we write a line of code.
Three L1s = one color team (**leader / middle / tail**). When you can walk them apart
and watch distance + a bearing arrow update on each OLED — no phone — Tier 0 is
validated and we start the module.

## 0. Before they arrive
- Chrome or Edge (WebSerial) for the web flasher, **or** `pip install --upgrade meshtastic` for the CLI.
- A clear-sky spot for first GPS fix (cold start can take a few minutes).
- The 915 MHz antennas on hand.

## 1. Flash stock Meshtastic (each of the 3)
- The L1 is nRF52840 → **UF2 flow**: double-tap reset, a USB drive named **`Tracker L1`** appears, drag the L1 Meshtastic `.uf2` onto it. It reboots installed.
- ⚠️ **USB UF2 only — never flash over BLE / NRF-OTA.** Seeed's own warning: OTA can brick the board dead. A wrong *app* `.uf2` is harmless (double-tap back to the `Tracker L1` drive, drop a good one); only a bad *bootloader* bricks, and normal flashing never touches it.
- Firmware + exact steps on the device page: <https://meshtastic.org/docs/hardware/devices/seeed-studio/wio-series/tracker-l1/>
- (Or use <https://flasher.meshtastic.org> and pick the Wio Tracker L1 target. On nRF52 the flasher only triggers DFU — you still drag the `.uf2`, so double-tap+drag is the reliable path either way.)

## 2. Set region FIRST — or it won't transmit
- Region = **US** (US_915).  `meshtastic --set lora.region US`
- **Screw the 915 MHz antenna on before powering.** Never TX without an antenna.

## 3. One shared mesh (the Wolfpack channel)
- On node 1, set a primary channel + random PSK (name it `wolfpack`).
- **Clone it to the other two via the channel URL/QR** — guarantees an identical PSK so
  all three are one encrypted mesh and relay for each other.
  - CLI: grab node 1's URL from `meshtastic --info`, then on nodes 2 & 3: `meshtastic --seturl "<url>"`
  - App: Channel → Share → scan the QR on nodes 2 & 3.

## 4. Turn on position
- GPS + position broadcast on all three. For testing a fixed ~30 s interval is fine
  (smart-broadcast later).
- Name them so you can tell them apart on the OLED: **Red-Lead / Red-Mid / Red-Tail**.

## 5. Prove the HUD
- Take all three outside; wait for GPS fix (the GPS icon stops blinking).
- On each OLED, cycle to the **node list → Distance mode**.
- Walk them apart. You should see **distance + a bearing arrow** to the other two,
  updating live.
- Heading caveat (expected): the arrow points right **while you're walking** (GPS
  course). Standing still it can't know your facing — that's exactly what the module's
  honest "heading lost" state + warmer/colder mode fix later.

## 6. Measure your range (terrain test)
Range here is set by hills, not leaves — so test the shadows, not just the flat:
- Two nodes, walk apart on open trail until the link drops — note the distance (best-case
  per-hop).
- Now put a **ridge or hill between you** and watch it die far sooner — that's the real
  limiter. Note where.
- Drop one in a **nook/gully** and see how close the other has to be to hold the link.
- Try the base **high vs. low** — hold one up on a high point vs. down in the lot and
  compare how much trail it reaches. That's how you decide where the center node lives.

## Done looks like
Three radios, no phone, each showing how far and which way to the others as you move
them around the yard or trail. That's Tier 0's core on stock firmware. **Next:** the
Wolfpack module — color/role broadcast + leader-centric screen + warmer/colder +
tail-lag alert — built on top of what you just watched work.

---

### CLI cheat-sheet (one node at a time over USB)
> Flag names track your installed `meshtastic` version; if one errors, check `meshtastic --help`.

```bash
# region (do this first)
meshtastic --set lora.region US

# node 1: name + a private wolfpack channel with a random key
meshtastic --set-owner "Red-Lead" --set-owner-short "RL"
meshtastic --ch-set name wolfpack --ch-index 0
meshtastic --ch-set psk random --ch-index 0
meshtastic --info          # copy the channel URL it prints

# nodes 2 & 3: clone the exact channel/PSK, then name them
meshtastic --seturl "<paste node 1's URL>"
meshtastic --set-owner "Red-Mid"  --set-owner-short "RM"   # node 2
meshtastic --set-owner "Red-Tail" --set-owner-short "RT"   # node 3

# all three: GPS on, broadcast position every 30s for testing
meshtastic --set position.gps_mode ENABLED
meshtastic --set position.position_broadcast_secs 30
meshtastic --set position.position_broadcast_smart_enabled false
```
