# Meshtastic — Wolfpack & dog tracking

LoRa mesh projects on Heltec / ESP32 hardware. Two builds, same radio family:

- **Wolfpack** — follow-the-leader for mountain bike coaches. *The active build.*
  See **[WOLFPACK.md](WOLFPACK.md)**.
- **Dog tracking** — collar nodes + a homelab gateway feeding a self-hosted map.
  Spec to come.

Common ground: US_915 region, 915 MHz antennas, GNSS on every node. The standalone
(no-phone) HUD lives in a custom Meshtastic firmware module — that's the first code
to write.

Status: spec'd, hardware inbound.
