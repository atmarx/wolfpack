/* ------------------------------------------------------------------ *
 * Wolfpack service worker — the map keeps working with no signal.
 *
 *   app shell (page, scripts, Leaflet)  → cache, refreshed in the background
 *   basemap (one PMTiles file)          → cache first, Range requests sliced
 *                                          out of the cached copy
 *
 * The shell is stale-while-revalidate rather than network-first on
 * purpose: at a trailhead with one bar, network-first means staring at a
 * blank page while a request times out. Cost: a new version of the page
 * shows up on the second load after it deploys, not the first.
 *
 * The basemap is deliberately NOT precached — it's 8.6 MB, and a parent
 * who just wants to watch dots on cell data shouldn't pay for it. The
 * "Save map offline" button is the opt-in.
 * ------------------------------------------------------------------ */
importScripts("wolfpack-offline.js");
const OFF = self.WolfpackOffline;

const SHELL_CACHE = "wolfpack-shell-v2";
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
  "vendor/protomaps/protomaps-leaflet.js",
];

self.addEventListener("install", event => {
  event.waitUntil(caches.open(SHELL_CACHE).then(c => c.addAll(SHELL)).then(() => self.skipWaiting()));
});

self.addEventListener("activate", event => {
  // Drop old shell versions. Never the basemap — that was a deliberate
  // 8.6 MB download and a code deploy shouldn't throw it away.
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

  if (OFF.isBasemap(req.url)) { event.respondWith(basemap(req)); return; }
  if (new URL(req.url).origin === self.location.origin) event.respondWith(shell(event));
});

async function basemap(req) {
  const cache = await caches.open(OFF.BASEMAP_CACHE);
  const hit = await cache.match(OFF.BASEMAP_PATH);
  // The renderer reads the file in pieces (Range), so a saved map has to be
  // sliced here — the cache stores one whole response, not each range.
  if (hit) return OFF.sliceCached(hit, req.headers.get("range"));
  try {
    return await fetch(req);
  } catch (e) {
    return new Response("", { status: 504, statusText: "offline, map not saved" });
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
