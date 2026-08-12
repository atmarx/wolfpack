/* Host tests for web/wolfpack-protocol.js — no browser, no radio.
 *
 *   node web/test-protocol.js
 *
 * The encoders here are written independently of the decoder (and mirror
 * the firmware's wp_packBeacon byte-for-byte) so a round-trip actually
 * proves something rather than just proving the decoder agrees with itself.
 */
"use strict";
const WP = require("./wolfpack-protocol.js");

let checks = 0, failures = 0, current = "";
function ok(cond, msg) {
  checks++;
  if (!cond) { failures++; console.log(`FAIL [${current}] ${msg}`); }
}
function eq(a, b, msg) { ok(a === b, `${msg}: got ${a}, want ${b}`); }
function near(a, b, tol, msg) { ok(Math.abs(a - b) <= tol, `${msg}: got ${a}, want ~${b}`); }
function test(name, fn) { current = name; fn(); }

// --- independent encoders (the firmware's side of the wire) ---

function varint(v) {
  const out = [];
  do { let byte = v & 0x7f; v = Math.floor(v / 128); if (v) byte |= 0x80; out.push(byte); } while (v);
  return out;
}
function tag(field, wire) { return varint((field << 3) | wire); }
function lenDelim(field, bytes) { return [...tag(field, 2), ...varint(bytes.length), ...bytes]; }
function fixed32(field, v) {
  return [...tag(field, 5), v & 0xff, (v >>> 8) & 0xff, (v >>> 16) & 0xff, (v >>> 24) & 0xff];
}

/** Mirror of wp_packBeacon(): 13 bytes, little-endian int32 position. */
function packBeacon({ version = 4, color, role, hasPosition = true, latI = 0, lonI = 0, rideEpoch = 0 }) {
  const b = [version, color, role, hasPosition ? 0x01 : 0x00];
  for (const v of [latI, lonI]) {
    b.push(v & 0xff, (v >>> 8) & 0xff, (v >>> 16) & 0xff, (v >>> 24) & 0xff);
  }
  if (version === 4) b.push(rideEpoch);
  return b.slice(0, version === 4 ? 13 : 12);
}

/** Wrap a payload the way the radio does: FromRadio{ packet{ decoded{} } }. */
function fromRadioPacket({ from, portnum = 256, payload, rxTime = 0 }) {
  const data = [...tag(1, 0), ...varint(portnum), ...lenDelim(2, payload)];
  let packet = [...fixed32(1, from), ...lenDelim(4, data)];
  if (rxTime) packet = packet.concat(fixed32(7, rxTime));
  return new Uint8Array(lenDelim(2, packet));
}

// ---------------------------------------------------------------- //

test("beacon v4 round-trip", () => {
  // Philadelphia-ish: positive lat, negative lon — the sign must survive.
  const payload = packBeacon({ color: 1, role: 1, latI: 399500000, lonI: -751600000, rideEpoch: 200 });
  eq(payload.length, 13, "v4 is 13 bytes");
  const b = WP.parseBeacon(new Uint8Array(payload));
  ok(b !== null, "parsed");
  eq(b.version, 4, "version");
  eq(b.color, 1, "color");
  eq(b.role, 1, "role");
  eq(b.hasPosition, true, "hasPosition");
  near(b.lat, 39.95, 1e-6, "lat");
  near(b.lon, -75.16, 1e-6, "lon");
  eq(b.rideEpoch, 200, "rideEpoch");
  eq(WP.shortCode(b), "RL", "short code");
});

test("negative longitude is not read unsigned", () => {
  // The bug this guards: >>> 0 on the assembled int would put us in China.
  const b = WP.parseBeacon(new Uint8Array(packBeacon({ color: 5, role: 3, latI: 399500000, lonI: -751600000 })));
  ok(b.lon < 0, "longitude stayed negative");
  eq(WP.shortCode(b), "BS", "blue sweep");
});

test("position-less heartbeat does not read as (0,0)", () => {
  // A radio whose GPS dropped still beacons; null must not become the
  // Gulf of Guinea, or a dead GPS puts a dot off the coast of Africa.
  const b = WP.parseBeacon(new Uint8Array(packBeacon({ color: 4, role: 2, hasPosition: false, latI: 123456789, lonI: -987654321 })));
  eq(b.hasPosition, false, "flag clear");
  eq(b.lat, null, "lat null");
  eq(b.lon, null, "lon null");
});

test("legacy v3 decodes with no ride epoch", () => {
  const payload = packBeacon({ version: 3, color: 4, role: 1, latI: 399500000, lonI: -751600000 });
  eq(payload.length, 12, "v3 is 12 bytes");
  const b = WP.parseBeacon(new Uint8Array(payload));
  ok(b !== null, "parsed");
  eq(b.rideEpoch, WP.BEACON.RIDE_EPOCH_NONE, "epoch reads as none");
  near(b.lat, 39.95, 1e-6, "position still lands");
});

test("legacy v2 decodes as colour/role only", () => {
  const b = WP.parseBeacon(new Uint8Array([2, 5, 3, 0x01]));
  ok(b !== null, "parsed");
  eq(b.color, 5, "color");
  eq(b.hasPosition, false, "v2 flag bits are not ours");
});

test("junk and truncation are rejected, not guessed at", () => {
  eq(WP.parseBeacon(null), null, "null");
  eq(WP.parseBeacon(new Uint8Array([])), null, "empty");
  eq(WP.parseBeacon(new Uint8Array([4, 1, 1])), null, "too short for any version");
  eq(WP.parseBeacon(new Uint8Array([1, 1, 1, 0])), null, "v1 rejected — palette renumbered");
  eq(WP.parseBeacon(new Uint8Array([9, 1, 1, 0])), null, "unknown version");
  // 12 bytes claiming v4: exactly a v3's length, epoch byte absent.
  eq(WP.parseBeacon(new Uint8Array(new Array(12).fill(0).map((_, i) => (i === 0 ? 4 : 1)))), null,
     "v4 claim at v3 length must not be salvaged");
});

test("FromRadio unwraps to our payload", () => {
  const payload = packBeacon({ color: 2, role: 3, latI: 399500000, lonI: -751600000, rideEpoch: 7 });
  const frame = fromRadioPacket({ from: 0xdeadbeef, payload, rxTime: 1786420800 });
  const pkt = WP.decodeFromRadio(frame);
  ok(pkt !== null, "decoded");
  eq(pkt.from, 0xdeadbeef, "from (fixed32, unsigned)");
  eq(pkt.portnum, WP.PORT_PRIVATE_APP, "portnum 256");
  eq(pkt.rxTime, 1786420800, "rx_time");
  const b = WP.parseBeacon(pkt.payload);
  eq(WP.shortCode(b), "OS", "orange sweep");
  eq(b.rideEpoch, 7, "epoch survived the whole stack");
});

test("portnum 256 needs a two-byte varint", () => {
  // 256 is the first portnum that doesn't fit in one byte — an off-by-one
  // in the varint reader shows up here and nowhere else.
  const frame = fromRadioPacket({ from: 1, payload: packBeacon({ color: 1, role: 1 }) });
  eq(WP.decodeFromRadio(frame).portnum, 256, "portnum");
});

test("non-packet and foreign frames are ignored", () => {
  // FromRadio{config_complete_id} — field 7 varint, no packet.
  eq(WP.decodeFromRadio(new Uint8Array([...tag(7, 0), ...varint(42)])), null, "config frame");
  // Someone else's traffic on a port we don't speak.
  const pkt = WP.decodeFromRadio(fromRadioPacket({ from: 9, portnum: 1, payload: [104, 105] }));
  eq(pkt.portnum, 1, "portnum surfaces");
  ok(pkt.portnum !== WP.PORT_PRIVATE_APP, "caller filters it out");
});

test("encrypted packets are surfaced, not silently dropped", () => {
  // Wrong channel or PSK: the radio hands up ciphertext. We want to be able
  // to TELL the user that, rather than show an empty map and no reason why.
  const packet = [...fixed32(1, 5), ...lenDelim(5, [1, 2, 3, 4])];
  const pkt = WP.decodeFromRadio(new Uint8Array(lenDelim(2, packet)));
  ok(pkt !== null, "returned");
  eq(pkt.encrypted, true, "flagged encrypted");
});

test("malformed frames throw rather than return nonsense", () => {
  let threw = false;
  try { WP.decodeFromRadio(new Uint8Array([0x12, 0x50, 0x01])); } catch (e) { threw = true; }
  ok(threw, "truncated length-delimited field throws");
});

test("want_config_id and heartbeat encode correctly", () => {
  const c = WP.encodeWantConfig(1);
  eq(c[0], 0x18, "field 3, varint");
  eq(c[1], 1, "value");
  const big = WP.encodeWantConfig(300);
  eq(big[0], 0x18, "tag");
  eq(big[1], 0xac, "300 low byte");
  eq(big[2], 0x02, "300 high byte");
  const h = WP.encodeHeartbeat();
  eq(h[0], 0x3a, "field 7, length-delimited");
  eq(h[1], 0x00, "empty message");
});

test("colour and role tables match the firmware enums", () => {
  eq(WP.colorName(1), "Red", "1 = Red");
  eq(WP.colorName(6), "Violet", "6 = Violet");
  eq(WP.roleName(3), "Sweep", "3 = Sweep, renamed from Tail");
  eq(WP.COLOR_LETTERS[2], "O", "2 = O");
  // Every colour and role must round-trip to a two-char code.
  for (let c = 1; c <= 6; c++) {
    for (let r = 1; r <= 3; r++) {
      const code = WP.shortCode({ color: c, role: r });
      eq(code.length, 2, `code ${c}/${r} is two chars`);
      ok(!code.includes("?"), `code ${c}/${r} fully resolved`);
    }
  }
});

console.log(`\n${checks} checks, ${failures} failures`);
process.exit(failures ? 1 : 0);
