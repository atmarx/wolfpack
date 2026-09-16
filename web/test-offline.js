/* Tests for web/wolfpack-offline.js and sw.js — no browser, no network.
 *
 *   node web/test-offline.js
 *
 * The tile math is checked against an independently written formula (the
 * asinh form of Web Mercator, not the log/tan form the module uses), and the
 * wiring checks guard the two mistakes that would only show up on a trail:
 * the page and the worker disagreeing about tile URLs, and a precache entry
 * that 404s — cache.addAll() is all-or-nothing, so one missing file means
 * the service worker never installs and nothing works offline at all.
 */
"use strict";
const fs = require("fs");
const path = require("path");
const OFF = require("./wolfpack-offline.js");

let checks = 0, failures = 0, current = "";
function ok(cond, msg) {
  checks++;
  if (!cond) { failures++; console.log(`FAIL [${current}] ${msg}`); }
}
function eq(a, b, msg) { ok(a === b, `${msg}: got ${JSON.stringify(a)}, want ${JSON.stringify(b)}`); }
function test(name, fn) { current = name; fn(); }

// Independent reference: y from asinh(tan φ).
function refX(lon, z) { return Math.floor((lon + 180) / 360 * 2 ** z); }
function refY(lat, z) {
  const r = lat * Math.PI / 180;
  return Math.floor((1 - Math.asinh(Math.tan(r)) / Math.PI) / 2 * 2 ** z);
}

const BASE = [40.0520, -75.2110];   // the mock ride's trailhead

test("tile coordinates match the reference formula", () => {
  for (const z of [0, 1, 5, 10, 14, 15, 17, 18]) {
    eq(OFF.lonToX(BASE[1], z), refX(BASE[1], z), `x at z${z}`);
    eq(OFF.latToY(BASE[0], z), refY(BASE[0], z), `y at z${z}`);
  }
  for (const [lat, lon] of [[-33.86, 151.21], [64.84, -147.72], [0.001, 0.001], [-0.001, -0.001]]) {
    eq(OFF.lonToX(lon, 12), refX(lon, 12), `x ${lat},${lon}`);
    eq(OFF.latToY(lat, 12), refY(lat, 12), `y ${lat},${lon}`);
  }
});

test("world edges clamp into the grid", () => {
  eq(OFF.lonToX(180, 4), 15, "lon 180");
  eq(OFF.lonToX(-180, 4), 0, "lon -180");
  eq(OFF.latToY(89.9, 4), 0, "near north pole");
  eq(OFF.latToY(-89.9, 4), 15, "near south pole");
  eq(OFF.lonToX(0, 0), 0, "z0 is one tile");
  eq(OFF.latToY(0, 0), 0, "z0 is one tile");
});

// A park-sized view: ~2.2 km × 1.7 km around the trailhead.
const PARK = { south: 40.044, west: -75.224, north: 40.060, east: -75.198 };

test("a range covers the bounds, north on top", () => {
  const r = OFF.tileRange(PARK, 15);
  ok(r.x0 <= r.x1 && r.y0 <= r.y1, "ranges are ordered");
  eq(r.y0, refY(PARK.north, 15), "north edge is the smaller y");
  eq(r.x1, refX(PARK.east, 15), "east edge");
  eq(OFF.rangeCount(r), (r.x1 - r.x0 + 1) * (r.y1 - r.y0 + 1), "count");
});

test("planSave: contiguous zooms from a few levels out, under the cap", () => {
  const plan = OFF.planSave(PARK, 15);
  eq(plan.zMin, 15 - OFF.ZOOM_OUT_LEVELS, "starts zoomed out");
  eq(plan.ranges[0].z, plan.zMin, "first range is zMin");
  plan.ranges.forEach((r, i) => eq(r.z, plan.zMin + i, `range ${i} is the next zoom`));
  ok(plan.count <= OFF.MAX_TILES, `under cap (${plan.count})`);
  eq(plan.count, plan.ranges.reduce((n, r) => n + OFF.rangeCount(r), 0), "count is the sum");
  eq(plan.zMax, OFF.MAX_SAVE_ZOOM, "a park fits all the way to max zoom");

  const tiles = [...OFF.tilesOf(plan.ranges)];
  eq(tiles.length, plan.count, "tilesOf yields every planned tile");
  eq(new Set(tiles.map(t => `${t.z}/${t.x}/${t.y}`)).size, tiles.length, "no duplicates");
});

test("planSave: a big area stops at the deepest zoom that fits", () => {
  const county = { south: 39.8, west: -75.6, north: 40.3, east: -74.9 };
  const plan = OFF.planSave(county, 11);
  ok(plan.zMax < OFF.MAX_SAVE_ZOOM, `stopped early (z${plan.zMax})`);
  ok(plan.count <= OFF.MAX_TILES, "under cap");
  const next = OFF.rangeCount(OFF.tileRange(county, plan.zMax + 1));
  ok(plan.count + next > OFF.MAX_TILES, "the next zoom really wouldn't have fit");
  eq(OFF.planSave(PARK, 15, 0).count, 0, "zero cap saves nothing");
  eq(OFF.planSave(PARK, 15, 0).zMax, null, "and says so");
});

test("planSave: zoomed out near z0 doesn't go negative", () => {
  eq(OFF.planSave(PARK, 2).zMin, 0, "zMin floors at 0");
  eq(OFF.planSave(PARK, 14.6).zMin, 15 - OFF.ZOOM_OUT_LEVELS, "fractional zoom rounds");
});

test("tileKey: one cache key per tile, whatever subdomain served it", () => {
  const want = "https://a.basemaps.cartocdn.com/dark_all/15/9538/12383.png";
  eq(OFF.tileUrl(15, 9538, 12383), want, "canonical url");
  for (const s of "abcd") {
    eq(OFF.tileKey(`https://${s}.basemaps.cartocdn.com/dark_all/15/9538/12383.png`), want, `subdomain ${s}`);
  }
  eq(OFF.tileKey(want + "?v=2"), want, "query string ignored");
  eq(OFF.tileKey("https://b.basemaps.cartocdn.com/dark_all/3/1/2@2x.png"),
     "https://a.basemaps.cartocdn.com/dark_all/3/1/2@2x.png", "retina tiles keep @2x");
  eq(OFF.tileKey("https://a.basemaps.cartocdn.com/light_all/15/1/1.png"), null, "other style");
  eq(OFF.tileKey("https://e.basemaps.cartocdn.com/dark_all/15/1/1.png"), null, "other subdomain");
  eq(OFF.tileKey("https://evil.example/a.basemaps.cartocdn.com/dark_all/1/1/1.png"), null, "other host");
  eq(OFF.tileKey("https://wolfpack.example/index.html"), null, "app shell");
});

test("the page and the worker agree on tiles", () => {
  const html = fs.readFileSync(path.join(__dirname, "index.html"), "utf8");
  const m = /L\.tileLayer\("([^"]+)",\s*\{([\s\S]*?)\}\)/.exec(html);
  ok(m, "found the tile layer in index.html");
  if (!m) return;
  eq(m[1], OFF.TILE_URL, "tile URL template");
  ok(m[2].includes(`subdomains: "${OFF.SUBDOMAINS}"`), "subdomains");
  ok(/crossOrigin:\s*true/.test(m[2]), "tiles load CORS so the worker can cache them");
  ok(!/unpkg\.com|jsdelivr|cdnjs/.test(html), "no script or stylesheet from a CDN");
});

test("every precached shell file exists", () => {
  const sw = fs.readFileSync(path.join(__dirname, "sw.js"), "utf8");
  const list = /const SHELL = \[([\s\S]*?)\];/.exec(sw);
  ok(list, "found SHELL in sw.js");
  if (!list) return;
  const files = [...list[1].matchAll(/"([^"]+)"/g)].map(x => x[1]).filter(f => f !== "./");
  ok(files.length >= 5, "shell list isn't empty");
  for (const f of files) ok(fs.existsSync(path.join(__dirname, f)), `${f} exists`);
  const html = fs.readFileSync(path.join(__dirname, "index.html"), "utf8");
  for (const src of html.matchAll(/(?:src|href)="((?!https?:)[^"#]+)"/g)) {
    ok(files.includes(src[1]), `page loads ${src[1]}, so the shell must precache it`);
  }
});

console.log(`\n${checks} checks, ${failures} failures`);
process.exit(failures ? 1 : 0);
