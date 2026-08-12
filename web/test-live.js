/* Tests for the LIVE path in index.html — no browser, no radio.
 *
 *   node web/test-live.js
 *
 * The live code is inline in index.html because the map is a single static
 * page. To test it anyway we extract that script, run it against stub DOM
 * and Leaflet objects, and then drive Live.onBeacon()/sample() with
 * synthetic beacons. What we're actually checking is the bit that would
 * fail in a field with no way to debug it: that the per-node timeline
 * arrays stay rectangular as riders appear late, drop out, and come back.
 */
"use strict";
const fs = require("fs");
const path = require("path");
const vm = require("vm");

let checks = 0, failures = 0, current = "";
function ok(cond, msg) {
  checks++;
  if (!cond) { failures++; console.log(`FAIL [${current}] ${msg}`); }
}
function eq(a, b, msg) { ok(a === b, `${msg}: got ${JSON.stringify(a)}, want ${JSON.stringify(b)}`); }
function test(name, fn) { current = name; fn(); }

// --- stubs ---------------------------------------------------------- //

function fakeEl() {
  return { textContent: "", className: "", innerHTML: "", value: 0, max: 0,
           disabled: false, addEventListener() {} };
}
const els = {};
const layerStub = () => ({
  addTo() { return this; }, remove() {}, setLatLng() {}, setLatLngs() {},
  setStyle() {}, bindTooltip() { return this; }, setTooltipContent() {},
});
const L = {
  map: () => ({ on() {}, fitBounds() {} }),
  tileLayer: layerStub, polyline: layerStub, circleMarker: layerStub,
  marker: layerStub, divIcon: () => ({}),
  latLngBounds: () => ({ pad() { return this; } }),
};

const sandbox = {
  console, setInterval: () => 0, clearInterval: () => {},
  Math, Date, JSON, Array, Object, String, Number, isNaN,
  L,
  document: {
    getElementById(id) { return (els[id] = els[id] || fakeEl()); },
    addEventListener() {},
  },
  navigator: {},
};
sandbox.window = sandbox;
sandbox.globalThis = sandbox;
sandbox.self = sandbox;
sandbox.window.WolfpackProtocol = require("./wolfpack-protocol.js");
sandbox.window.isSecureContext = true;

// Pull the page's inline script and expose its internals for inspection.
const html = fs.readFileSync(path.join(__dirname, "index.html"), "utf8");
const blocks = [...html.matchAll(/<script>([\s\S]*?)<\/script>/g)].map(m => m[1]);
const source = blocks[blocks.length - 1] +
  "\n;globalThis.__Live = Live; globalThis.__ride = () => RIDE;" +
  "\n;globalThis.__setRide = r => { RIDE = r; };";

vm.createContext(sandbox);
vm.runInContext(source, sandbox, { filename: "index.html:inline" });

const Live = sandbox.__Live;
const ride = () => sandbox.__ride();
const WP = sandbox.window.WolfpackProtocol;

// --- helpers -------------------------------------------------------- //

function beacon({ color, role, lat = 39.95, lon = -75.16, hasPosition = true, rideEpoch = 5 }) {
  return { version: 4, color, role, flags: hasPosition ? 1 : 0, hasPosition,
           lat: hasPosition ? lat : null, lon: hasPosition ? lon : null, rideEpoch };
}
/** Every node's timeline arrays must be the same length as each other. */
function rectangular(r) {
  const lens = new Set();
  r.nodes.forEach(n => {
    lens.add(n.truePos.length); lens.add(n.shown.length);
    lens.add(n.age.length); lens.add(n.status.length); lens.add(n.lastHeard.length);
  });
  return lens.size <= 1;
}

// --- tests ---------------------------------------------------------- //

test("mock ride loaded at startup and is self-consistent", () => {
  const r = ride();
  eq(r.nodes.length, 12, "4 teams x 3 roles");
  ok(rectangular(r), "mock arrays rectangular");
  eq(r.steps, 180, "steps");
});

test("a first beacon creates a peer and seeds the base", () => {
  Live.reset();
  const now = Date.now();
  Live.onBeacon(111, beacon({ color: 1, role: 1 }), now);
  eq(Live.peers.size, 1, "one peer");
  const p = Live.peers.get(111);
  eq(p.short, "RL", "short code");
  eq(p.role, "Lead", "role name");
  eq(p.team, "R", "team letter");
  Live.sample();
  eq(ride().nodes.length, 1, "one node on the timeline");
  ok(ride().base !== null, "base seeded from the first fix");
  ok(rectangular(ride()), "rectangular");
});

test("a rider who joins late is backfilled, not misaligned", () => {
  // This is the one that would silently corrupt the map: node B's array
  // index 0 must line up with node A's index 0, not with "when B arrived".
  Live.reset();
  const now = Date.now();
  Live.onBeacon(111, beacon({ color: 1, role: 1 }), now);
  Live.sample(); Live.sample(); Live.sample();
  const lenBefore = ride().nodes[0].shown.length;
  ok(lenBefore >= 3, "lead has history");

  Live.onBeacon(222, beacon({ color: 1, role: 3, lat: 39.96 }), Date.now());
  Live.sample();
  const r = ride();
  eq(r.nodes.length, 2, "sweep joined");
  ok(rectangular(r), "arrays realigned to equal length");
  const sweep = r.nodes.find(n => n.short === "RS");
  eq(sweep.shown[0], null, "sweep has no position before it existed");
  ok(sweep.shown[sweep.shown.length - 1] !== null, "sweep has a position now");
});

test("a stale rider greys out but keeps its last known position", () => {
  Live.reset();
  const old = Date.now() - 200 * 1000;     // older than LIVE_STALE_SECONDS (150)
  Live.onBeacon(111, beacon({ color: 4, role: 1 }), old);
  Live.sample();
  const n = ride().nodes[0];
  const i = n.shown.length - 1;
  ok(n.age[i] > 150, "aged out");
  eq(n.truePos[i], null, "no fresh fix -> nothing appended to the trail");
  ok(n.shown[i] !== null, "last known position still shown");
});

test("a position-less heartbeat never plots (0,0)", () => {
  // The Gulf of Guinea test: a radio whose GPS died still beacons.
  Live.reset();
  Live.onBeacon(111, beacon({ color: 1, role: 2, hasPosition: false }), Date.now());
  eq(Live.peers.get(111).lastPos, null, "no position recorded");
  Live.sample();
  eq(ride().nodes[0].shown[0], null, "nothing plotted");
  eq(ride().base, null, "base not seeded by a position-less beacon");
});

test("Start Ride clears the trails", () => {
  Live.reset();
  const now = Date.now();
  Live.onBeacon(111, beacon({ color: 1, role: 1, rideEpoch: 5 }), now);
  Live.sample(); Live.sample();
  ok(ride().nodes[0].truePos.some(Boolean), "trail has crumbs");

  // Lead stamps a new epoch — the same signal the radios act on.
  Live.onBeacon(111, beacon({ color: 1, role: 1, rideEpoch: 99 }), Date.now());
  ok(!ride().nodes[0].truePos.some(Boolean), "trail cleared");
  eq(Live.peers.get(111).rideEpoch, 99, "epoch recorded");
});

test("repeating the same epoch does not re-clear", () => {
  Live.reset();
  Live.onBeacon(111, beacon({ color: 1, role: 1, rideEpoch: 42 }), Date.now());
  Live.sample(); Live.sample();
  const crumbs = ride().nodes[0].truePos.filter(Boolean).length;
  ok(crumbs > 0, "has crumbs");
  Live.onBeacon(111, beacon({ color: 1, role: 1, rideEpoch: 42 }), Date.now());
  eq(ride().nodes[0].truePos.filter(Boolean).length, crumbs, "idempotent — trail untouched");
});

test("a non-lead's epoch does not wipe the map", () => {
  // Only the lead declares a ride. A sweep's epoch must be inert.
  Live.reset();
  Live.onBeacon(111, beacon({ color: 1, role: 1, rideEpoch: 5 }), Date.now());
  Live.sample(); Live.sample();
  const crumbs = ride().nodes[0].truePos.filter(Boolean).length;
  Live.onBeacon(222, beacon({ color: 1, role: 3, rideEpoch: 200 }), Date.now());
  eq(ride().nodes[0].truePos.filter(Boolean).length, crumbs, "lead's trail survived");
});

test("re-picking a team mid-ride follows the radio", () => {
  Live.reset();
  Live.onBeacon(111, beacon({ color: 1, role: 2 }), Date.now());
  Live.sample();
  eq(ride().nodes[0].short, "RM", "started as red mid");
  Live.onBeacon(111, beacon({ color: 5, role: 3 }), Date.now());
  Live.sample();
  eq(Live.peers.size, 1, "still one radio, not a ghost pair");
  eq(ride().nodes[0].short, "BS", "followed to blue sweep");
});

test("all six colours and three roles register as teams", () => {
  Live.reset();
  let n = 1;
  for (let c = 1; c <= 6; c++) {
    for (let r = 1; r <= 3; r++) Live.onBeacon(n++, beacon({ color: c, role: r }), Date.now());
  }
  Live.sample();
  eq(ride().nodes.length, 18, "6 x 3 riders");
  eq(Object.keys(ride().teams).length, 6, "six teams in the legend");
  ok(rectangular(ride()), "rectangular with a full field");
});

test("a long ride stays rectangular through churn", () => {
  Live.reset();
  Live.onBeacon(1, beacon({ color: 1, role: 1 }), Date.now());
  for (let i = 0; i < 40; i++) {
    if (i === 10) Live.onBeacon(2, beacon({ color: 1, role: 2 }), Date.now());
    if (i === 25) Live.onBeacon(3, beacon({ color: 1, role: 3 }), Date.now());
    // rider 2 goes quiet after step 30 — no beacons, should age out
    if (i < 30) Live.onBeacon(1, beacon({ color: 1, role: 1, lat: 39.95 + i * 1e-4 }), Date.now());
    Live.sample();
    ok(rectangular(ride()), `rectangular at step ${i}`);
  }
  eq(ride().nodes.length, 3, "three riders");
});

console.log(`\n${checks} checks, ${failures} failures`);
process.exit(failures ? 1 : 0);
