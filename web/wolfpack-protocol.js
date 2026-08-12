/* ------------------------------------------------------------------ *
 * Wolfpack — wire decoding for the phone map.
 *
 * Pure functions, no DOM, no Bluetooth: the same split the firmware uses
 * (WolfpackProtocol.h is dependency-free so it can be host-tested; the
 * module shell does the hardware talking). Run the tests with:
 *
 *   node web/test-protocol.js
 *
 * Why hand-rolled protobuf instead of @meshtastic/js: we need exactly five
 * fields out of two nested messages. The wire format is varints and
 * length-delimited blobs, so the decoder below is smaller than the import
 * statement's dependency tree — and it keeps the map a single static page
 * with no build step and nothing fetched from a CDN at runtime.
 *
 * Field numbers below are read off meshtastic/protobufs at v2.7.26, not
 * from memory. If a future firmware renumbers them this file is the one
 * place to fix.
 * ------------------------------------------------------------------ */
(function (root, factory) {
  const api = factory();
  if (typeof module === "object" && module.exports) module.exports = api;
  else root.WolfpackProtocol = api;
})(typeof self !== "undefined" ? self : this, function () {
  "use strict";

  // --- Meshtastic BLE (src/BluetoothCommon.h) ---
  const BLE = {
    SERVICE: "6ba1b218-15a8-461f-9fa8-5dcae273eafd",
    TORADIO: "f75c76d2-129e-4dad-a1dd-7866124401e7",
    FROMRADIO: "2c55e69e-4993-11ed-b878-0242ac120002",
    FROMNUM: "ed9da18c-a800-4f66-a670-aa7547e34453",
  };

  const PORT_PRIVATE_APP = 256; // portnums.proto — what the Wolfpack beacon rides on

  // --- Wolfpack beacon (firmware/src/modules/WolfpackProtocol.h) ---
  const BEACON = {
    V4: 4, V3: 3, V2: 2,
    SIZE_V4: 13, SIZE_V3: 12, SIZE_V2: 4,
    FLAG_HAS_POSITION: 0x01,
    RIDE_EPOCH_NONE: 0,
  };

  // Rainbow order, matching enum WolfpackColor. Index 0 is "none".
  const COLORS = [null, "Red", "Orange", "Yellow", "Green", "Blue", "Violet"];
  const COLOR_LETTERS = [null, "R", "O", "Y", "G", "B", "V"];
  // enum WolfpackRole. Slice 4 renamed "tail" -> "sweep"; wire value unchanged.
  const ROLES = [null, "Lead", "Mid", "Sweep"];
  const ROLE_LETTERS = [null, "L", "M", "S"];

  // ---------------------------------------------------------------- //
  // Minimal protobuf reader. Only what MeshPacket/Data need.
  // ---------------------------------------------------------------- //

  function Reader(bytes) {
    this.b = bytes;
    this.p = 0;
  }
  Reader.prototype.eof = function () { return this.p >= this.b.length; };

  Reader.prototype.varint = function () {
    let result = 0, shift = 0;
    while (this.p < this.b.length) {
      const byte = this.b[this.p++];
      // Multiply rather than shift: >>> 0 breaks past 32 bits, and a
      // corrupt frame can hand us a 10-byte varint.
      result += (byte & 0x7f) * Math.pow(2, shift);
      if (!(byte & 0x80)) return result;
      shift += 7;
      if (shift > 63) throw new Error("varint too long");
    }
    throw new Error("truncated varint");
  };

  Reader.prototype.fixed32 = function () {
    if (this.p + 4 > this.b.length) throw new Error("truncated fixed32");
    const v = this.b[this.p] | (this.b[this.p + 1] << 8) |
              (this.b[this.p + 2] << 16) | (this.b[this.p + 3] << 24);
    this.p += 4;
    return v >>> 0;
  };

  Reader.prototype.bytes = function () {
    const len = this.varint();
    if (this.p + len > this.b.length) throw new Error("truncated length-delimited field");
    const out = this.b.subarray(this.p, this.p + len);
    this.p += len;
    return out;
  };

  // Skip a field we don't care about, by wire type.
  Reader.prototype.skip = function (wireType) {
    switch (wireType) {
      case 0: this.varint(); break;
      case 1: this.p += 8; break;
      case 2: this.bytes(); break;
      case 5: this.p += 4; break;
      default: throw new Error("unknown wire type " + wireType);
    }
    if (this.p > this.b.length) throw new Error("truncated field");
  };

  // Walk a message, calling visit(fieldNumber, wireType, reader).
  // visit returns true if it consumed the field, false to let us skip it.
  function walk(bytes, visit) {
    const r = new Reader(bytes);
    while (!r.eof()) {
      const tag = r.varint();
      const field = tag >>> 3, wire = tag & 7;
      if (field === 0) throw new Error("invalid field number 0");
      if (!visit(field, wire, r)) r.skip(wire);
    }
  }

  // ---------------------------------------------------------------- //
  // Meshtastic frames
  // ---------------------------------------------------------------- //

  /**
   * Decode a FromRadio frame far enough to find a mesh packet's payload.
   * Returns null for frames that aren't packets (config, node info, log
   * records...) or for packets we can't read — an encrypted packet, for
   * instance, which is what you get if the phone's radio isn't on the same
   * channel with the same PSK.
   *
   * Field numbers: FromRadio.packet = 2; MeshPacket.from = 1 (fixed32),
   * .decoded = 4, .rx_time = 7 (fixed32); Data.portnum = 1, .payload = 2.
   */
  function decodeFromRadio(bytes) {
    let packet = null;
    walk(bytes, (field, wire, r) => {
      if (field === 2 && wire === 2) { packet = r.bytes(); return true; }
      return false;
    });
    if (!packet) return null;

    let from = null, rxTime = 0, decoded = null, encrypted = false;
    walk(packet, (field, wire, r) => {
      if (field === 1 && wire === 5) { from = r.fixed32(); return true; }
      if (field === 4 && wire === 2) { decoded = r.bytes(); return true; }
      if (field === 5 && wire === 2) { encrypted = true; r.bytes(); return true; }
      if (field === 7 && wire === 5) { rxTime = r.fixed32(); return true; }
      return false;
    });
    if (from === null) return null;
    if (!decoded) return encrypted ? { from, rxTime, encrypted: true } : null;

    let portnum = null, payload = null;
    walk(decoded, (field, wire, r) => {
      if (field === 1 && wire === 0) { portnum = r.varint(); return true; }
      if (field === 2 && wire === 2) { payload = r.bytes(); return true; }
      return false;
    });
    if (portnum === null) return null;
    return { from, rxTime, portnum, payload: payload || new Uint8Array(0) };
  }

  /** Encode ToRadio{want_config_id}. Field 3, varint. */
  function encodeWantConfig(id) {
    const out = [0x18]; // (3 << 3) | 0
    let v = id >>> 0;
    do {
      let byte = v & 0x7f;
      v = Math.floor(v / 128);
      if (v) byte |= 0x80;
      out.push(byte);
    } while (v);
    return new Uint8Array(out);
  }

  /** Encode ToRadio{heartbeat}. Field 7, an empty Heartbeat message. */
  function encodeHeartbeat() {
    return new Uint8Array([0x3a, 0x00]); // (7 << 3) | 2, length 0
  }

  // ---------------------------------------------------------------- //
  // The Wolfpack beacon itself
  // ---------------------------------------------------------------- //

  function readI32LE(b, o) {
    return (b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24)) | 0;
  }

  /**
   * Parse a Wolfpack beacon payload. Mirrors wp_unpackBeacon(): accepts v4
   * (13 bytes), v3 (12, ride epoch reads as "none"), and legacy v2 (4,
   * colour/role only). Returns null on anything else — including v1, whose
   * colour palette was renumbered and would decode to the wrong team.
   */
  function parseBeacon(payload) {
    if (!payload || payload.length < BEACON.SIZE_V2) return null;
    const version = payload[0];

    const isV4 = version === BEACON.V4 && payload.length >= BEACON.SIZE_V4;
    const isV3 = version === BEACON.V3 && payload.length >= BEACON.SIZE_V3;

    if (isV4 || isV3) {
      const flags = payload[3];
      const hasPosition = (flags & BEACON.FLAG_HAS_POSITION) !== 0;
      return {
        version,
        color: payload[1],
        role: payload[2],
        flags,
        hasPosition,
        // 1e-7 degree fixed point, same as Meshtastic native. Zeroed when the
        // flag is clear so a position-less heartbeat can't read as (0,0) —
        // which is a real place in the Gulf of Guinea.
        lat: hasPosition ? readI32LE(payload, 4) * 1e-7 : null,
        lon: hasPosition ? readI32LE(payload, 8) * 1e-7 : null,
        rideEpoch: isV4 ? payload[12] : BEACON.RIDE_EPOCH_NONE,
      };
    }

    if (version === BEACON.V2 && payload.length >= BEACON.SIZE_V2) {
      return {
        version, color: payload[1], role: payload[2], flags: 0,
        hasPosition: false, lat: null, lon: null,
        rideEpoch: BEACON.RIDE_EPOCH_NONE,
      };
    }
    return null;
  }

  /** "RL", "GS" — the same two-character handle the device HUD shows. */
  function shortCode(beacon) {
    const c = COLOR_LETTERS[beacon.color] || "?";
    const r = ROLE_LETTERS[beacon.role] || "?";
    return c + r;
  }

  function colorName(i) { return COLORS[i] || "Unknown"; }
  function roleName(i) { return ROLES[i] || "Unknown"; }

  return {
    BLE, PORT_PRIVATE_APP, BEACON, COLORS, COLOR_LETTERS, ROLES, ROLE_LETTERS,
    decodeFromRadio, encodeWantConfig, encodeHeartbeat,
    parseBeacon, shortCode, colorName, roleName,
    _Reader: Reader, _walk: walk,
  };
});
