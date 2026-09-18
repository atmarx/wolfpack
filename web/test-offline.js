/* Tests for web/wolfpack-offline.js and sw.js — no browser, no network.
 *
 *   node web/test-offline.js
 *
 * The interesting half is Range handling. The map renderer reads the
 * basemap file in pieces, and once the file is in the cache there is no
 * server left to answer a Range request — the worker has to slice the
 * cached copy itself. Get that wrong and the map works online and shows
 * nothing on the trail, which is the exact failure this whole feature
 * exists to prevent.
 *
 * The wiring checks guard the other two: a precache entry that 404s (
 * cache.addAll is all-or-nothing, so one missing file and the worker never
 * installs), and the page and worker disagreeing about where the basemap is.
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
function test(name, fn) { current = name; return fn(); }

test("isBasemap recognises the file, and nothing else", () => {
  ok(OFF.isBasemap("https://wolfpack.example/" + OFF.BASEMAP_PATH), "absolute");
  ok(OFF.isBasemap(OFF.BASEMAP_PATH), "relative");
  ok(OFF.isBasemap("https://wolfpack.example/sub/dir/" + OFF.BASEMAP_PATH), "under a subpath");
  ok(!OFF.isBasemap("https://wolfpack.example/index.html"), "the page");
  ok(!OFF.isBasemap("https://wolfpack.example/basemap/other.pmtiles"), "some other archive");
  ok(!OFF.isBasemap("https://wolfpack.example/wolfpack-protocol.js"), "a script");
});

test("parseRange handles what a byte-range reader actually sends", () => {
  const size = 1000;
  const r = (h) => OFF.parseRange(h, size);
  eq(JSON.stringify(r("bytes=0-99")), JSON.stringify({ start: 0, end: 99 }), "closed range");
  eq(JSON.stringify(r("bytes=500-")), JSON.stringify({ start: 500, end: 999 }), "open-ended");
  eq(JSON.stringify(r("bytes=-128")), JSON.stringify({ start: 872, end: 999 }), "suffix");
  eq(JSON.stringify(r(" bytes=0-0 ")), JSON.stringify({ start: 0, end: 0 }), "single byte, padded");
  eq(JSON.stringify(r("bytes=900-99999")), JSON.stringify({ start: 900, end: 999 }), "clamped to eof");
  eq(r(null), null, "no header");
  eq(r("bytes=1000-1200"), null, "starts past eof");
  eq(r("bytes=200-100"), null, "backwards");
  eq(r("bytes=abc"), null, "nonsense");
  eq(r("bytes=0-10, 20-30"), null, "multipart is refused rather than half-answered");
  eq(JSON.stringify(OFF.parseRange("bytes=-5000", 1000)), JSON.stringify({ start: 0, end: 999 }),
     "suffix longer than the file");
});

test("human sizes", () => {
  eq(OFF.human(0), "0 MB", "nothing");
  eq(OFF.human(8600000), "8.6 MB", "the basemap");
  eq(OFF.human(4096), "4 kB", "small");
});

const BYTES = Uint8Array.from({ length: 4096 }, (_, i) => i % 251);
function cached() {
  return new Response(new Blob([BYTES]), { status: 200, headers: {
    "Content-Type": "application/octet-stream", "Content-Length": String(BYTES.length),
  } });
}

const slicing = test("sliceCached answers Range out of the cached file", async () => {
  const res = await OFF.sliceCached(cached(), "bytes=100-199");
  eq(res.status, 206, "partial content");
  eq(res.headers.get("content-range"), `bytes 100-199/${BYTES.length}`, "content-range");
  eq(res.headers.get("content-length"), "100", "content-length");
  const got = new Uint8Array(await res.arrayBuffer());
  eq(got.length, 100, "body length");
  ok(got.every((b, i) => b === BYTES[100 + i]), "body is the right slice");

  const tail = await OFF.sliceCached(cached(), "bytes=4090-");
  eq(tail.status, 206, "open-ended is partial too");
  eq((await tail.arrayBuffer()).byteLength, 6, "to the end of the file");

  const whole = await OFF.sliceCached(cached(), null);
  eq(whole.status, 200, "no Range header means the whole file");
  eq((await whole.arrayBuffer()).byteLength, BYTES.length, "all of it");

  const bogus = await OFF.sliceCached(cached(), "bytes=99999-");
  eq(bogus.status, 200, "an unsatisfiable range falls back to the whole file rather than failing");
});

test("the page and the worker agree", () => {
  const html = fs.readFileSync(path.join(__dirname, "index.html"), "utf8");
  ok(/protomapsL\.leafletLayer/.test(html), "page renders vector tiles");
  ok(/url:\s*OFFLINE\.BASEMAP_PATH/.test(html), "page gets the basemap path from the shared module");
  ok(/maxDataZoom:\s*OFFLINE\.MAX_DATA_ZOOM/.test(html), "and the data zoom");
  ok(!/cartocdn|unpkg\.com|jsdelivr|cdnjs/.test(html), "nothing loaded from a CDN");

  const sw = fs.readFileSync(path.join(__dirname, "sw.js"), "utf8");
  const list = /const SHELL = \[([\s\S]*?)\];/.exec(sw);
  ok(list, "found SHELL in sw.js");
  if (!list) return;
  const files = [...list[1].matchAll(/"([^"]+)"/g)].map(x => x[1]).filter(f => f !== "./");
  for (const f of files) ok(fs.existsSync(path.join(__dirname, f)), `${f} exists`);
  for (const src of html.matchAll(/(?:src|href)="((?!https?:)[^"#]+)"/g)) {
    ok(files.includes(src[1]), `page loads ${src[1]}, so the shell must precache it`);
  }
  ok(!files.includes(OFF.BASEMAP_PATH),
     "the basemap is NOT precached — 8.6 MB is opt-in, not something a watching parent pays for");
});

test("the basemap file is a PMTiles archive covering the right zooms", () => {
  const file = path.join(__dirname, OFF.BASEMAP_PATH);
  ok(fs.existsSync(file), "basemap is committed");
  if (!fs.existsSync(file)) return;
  const head = Buffer.alloc(128);
  const fd = fs.openSync(file, "r");
  fs.readSync(fd, head, 0, 128, 0);
  fs.closeSync(fd);
  eq(head.subarray(0, 7).toString("latin1"), "PMTiles", "magic");
  eq(head[7], 3, "spec version 3");
  eq(head[101], OFF.MAX_DATA_ZOOM, "max zoom matches what the renderer overzooms from");
  const mb = fs.statSync(file).size / 1e6;
  ok(mb < 25, `small enough to save on a phone (${mb.toFixed(1)} MB)`);
});

(async () => {
  await slicing;   // the Range tests are async; don't count before they land
  console.log(`\n${checks} checks, ${failures} failures`);
  process.exit(failures ? 1 : 0);
})();
