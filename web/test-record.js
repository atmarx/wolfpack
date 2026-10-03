/* Tests for web/wolfpack-record.js — the GPX half. No browser.
 *
 *   node web/test-record.js
 *
 * IndexedDB isn't here to test; what is here is the file a coach will
 * open in Strava or Gaia, and the ways it could quietly lie: a greyed-out
 * last-known position exported as somewhere the rider was, a dropout
 * drawn as a straight line across the woods, or times that don't match
 * the fixes.
 */
"use strict";
const R = require("./wolfpack-record.js");

let checks = 0, failures = 0, current = "";
function ok(cond, msg) {
  checks++;
  if (!cond) { failures++; console.log(`FAIL [${current}] ${msg}`); }
}
function eq(a, b, msg) { ok(a === b, `${msg}: got ${JSON.stringify(a)}, want ${JSON.stringify(b)}`); }
function test(name, fn) { current = name; fn(); }

const T0 = Date.UTC(2026, 9, 3, 15, 0, 0);
const A = [40.8166, -75.5098], B = [40.8170, -75.5090], C = [40.8180, -75.5080];
function ride() {
  return {
    startedAt: T0, times: [0, 1, 2, 3, 4, 5].map(k => T0 + k * 5000), isLive: true,
    teams: { R: { name: "Red" }, B: { name: "Blue" } },
    nodes: [
      // lead: moves, parks (repeat), drops out, comes back
      { short: "RL", team: "R", role: "Lead", truePos: [A, B, B, null, null, C] },
      // a radio that never had a fix
      { short: "BS", team: "B", role: "Sweep", truePos: [null, null, null, null, null, null] },
    ],
  };
}
const gpx = R.toGPX(ride());

test("GPX shape", () => {
  ok(gpx.startsWith('<?xml version="1.0" encoding="UTF-8"?>'), "xml declaration");
  ok(/<gpx version="1.1" creator="Wolfpack" xmlns="http:\/\/www.topografix.com\/GPX\/1\/1">/.test(gpx), "GPX 1.1 root");
  ok(gpx.trim().endsWith("</gpx>"), "closed");
  eq((gpx.match(/<trk>/g) || []).length, 1, "one track — the radio with no fixes is left out");
  ok(gpx.includes("<name>RL — Red Lead</name>"), "track named for the radio");
});

test("a dropout is a new segment, not a line across the woods", () => {
  eq((gpx.match(/<trkseg>/g) || []).length, 2, "two segments either side of the gap");
});

test("only fresh fixes, repeats dropped, times match the step", () => {
  const pts = [...gpx.matchAll(/<trkpt lat="([-\d.]+)" lon="([-\d.]+)"><time>([^<]+)<\/time>/g)];
  eq(pts.length, 3, "A, B (once), C");
  eq(pts[0][3], new Date(T0).toISOString(), "first point at step 0");
  eq(pts[1][3], new Date(T0 + 5000).toISOString(), "parked: the first time it was there, not the last");
  eq(pts[2][3], new Date(T0 + 25000).toISOString(), "after the gap, at its real time");
  eq(Number(pts[2][1]), C[0], "lat");
  eq(Number(pts[2][2]), C[1], "lon");
});

test("names are escaped", () => {
  const r = ride();
  r.teams.R.name = "R&D <crew>";
  ok(R.toGPX(r).includes("R&amp;D &lt;crew&gt;"), "no raw markup in a track name");
});

test("hasFixes", () => {
  ok(R.hasFixes(ride()), "a ride with fixes");
  const empty = ride(); empty.nodes = [empty.nodes[1]];
  ok(!R.hasFixes(empty), "nothing but a fixless radio");
  ok(!R.hasFixes({ nodes: [] }), "a mock ride has no times");
  ok(!R.hasFixes(null), "no ride");
});

test("ride name is local date and time", () => {
  ok(/^Wolfpack ride \d{4}-\d\d-\d\d \d\d:\d\d$/.test(R.rideName(ride())), R.rideName(ride()));
});

console.log(`\n${checks} checks, ${failures} failures`);
process.exit(failures ? 1 : 0);
