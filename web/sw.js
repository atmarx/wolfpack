/* ------------------------------------------------------------------ *
 * Wolfpack service worker — the map keeps working with no signal.
 *
 *   app shell (page, scripts, Leaflet)  → cache, refreshed in the background
 *   basemap tiles                       → cache first, network fills gaps
 *
 * The shell is stale-while-revalidate rather than network-first on
 * purpose: at a trailhead with one bar, network-first means staring at a
 * blank page while a request times out. Cost: a new version of the page
 * shows up on the second load after it deploys, not the first.
 * ------------------------------------------------------------------ */
importScripts("wolfpack-offline.js");
const OFF = self.WolfpackOffline;

const SHELL_CACHE = "wolfpack-shell-v1";
const SHELL = [
  "./",
  "index.html",
  "wolfpack-protocol.js",
  "wolfpack-offline.js",
  "vendor/leaflet/leaflet.js",
  "vendor/leaflet/leaflet.css",
  "vendor/leaflet/images/layers.png",
  "vendor/leaflet/images/layers-2x.png",
  "vendor/leaflet/images/marker-icon.png",
  "vendor/leaflet/images/marker-icon-2x.png",
  "vendor/leaflet/images/marker-shadow.png",
];

self.addEventListener("install", event => {
  event.waitUntil(caches.open(SHELL_CACHE).then(c => c.addAll(SHELL)).then(() => self.skipWaiting()));
});

self.addEventListener("activate", event => {
  // Drop old shell versions. Never the tile cache — those tiles were a
  // deliberate download and a code deploy shouldn't throw them away.
  event.waitUntil(
    caches.keys()
      .then(keys => Promise.all(keys
        .filter(k => k.startsWith("wolfpack-shell-") && k !== SHELL_CACHE)
        .map(k => caches.delete(k))))
      .then(() => self.clients.claim()));
});

self.addEventListener("fetch", event => {
  const req = event.request;
  if (req.method !== "GET") return;

  const key = OFF.tileKey(req.url);
  if (key) { event.respondWith(tile(key)); return; }

  if (new URL(req.url).origin === self.location.origin) event.respondWith(shell(event));
});

async function tile(key) {
  const cache = await caches.open(OFF.TILE_CACHE);
  const hit = await cache.match(key);
  if (hit) return hit;
  try {
    // Always CORS, whatever the <img> asked for: an opaque response can't
    // be checked for success, and Chrome pads each one to megabytes of quota.
    const res = await fetch(key, { mode: "cors", credentials: "omit" });
    // Anything we panned past with signal is kept, so the tiles you looked
    // at on the drive in are there on the trail too.
    if (res.ok) cache.put(key, res.clone());
    return res;
  } catch (e) {
    return new Response("", { status: 504, statusText: "offline, tile not saved" });
  }
}

async function shell(event) {
  const cache = await caches.open(SHELL_CACHE);
  // "./" and "index.html" are the same page; ignoreSearch so ?live etc. still hit.
  const hit = await cache.match(event.request, { ignoreSearch: true });
  const fresh = fetch(event.request)
    .then(res => { if (res.ok) cache.put(event.request, res.clone()); return res; })
    .catch(() => null);
  if (hit) {
    event.waitUntil(fresh);
    return hit;
  }
  return (await fresh) || new Response("Offline, and this wasn't saved.", { status: 504 });
}
