/* ------------------------------------------------------------------ *
 * Wolfpack — offline map tiles.
 *
 * The positions come over Bluetooth and never need a network. The basemap
 * does. On a trail with no cell service that used to mean live dots on a
 * blank grey square — the one failure that makes the whole map pointless.
 *
 * Two halves share this file:
 *   - pure tile math (which tiles cover an area, what URL a tile lives at),
 *     tested in node:  node web/test-offline.js
 *   - the browser side: "Save this area" pulls every tile for the visible
 *     area into the Cache API, and sw.js serves them from there first.
 *
 * sw.js loads this same file with importScripts(), so the page and the
 * service worker can't disagree about tile URLs or cache names.
 * ------------------------------------------------------------------ */
(function (root, factory) {
  const api = factory();
  if (typeof module === "object" && module.exports) module.exports = api;
  else root.WolfpackOffline = api;
})(typeof self !== "undefined" ? self : this, function () {
  "use strict";

  const TILE_URL = "https://{s}.basemaps.cartocdn.com/dark_all/{z}/{x}/{y}.png";
  const SUBDOMAINS = "abcd";
  const ATTRIBUTION = "© OpenStreetMap © CARTO";
  const TILE_CACHE = "wolfpack-tiles-v1";

  // Deepest zoom we'll save. z18 is ~2 m/px — past that the tiles are
  // upscaled z18 anyway, and every extra level costs 4× the tiles.
  const MAX_SAVE_ZOOM = 18;
  // Zoom out this far below the current view too, so pinching out on the
  // trail shows context instead of grey.
  const ZOOM_OUT_LEVELS = 4;
  // Politeness and storage cap for one save. Dark tiles run ~5–20 kB, so
  // this tops out in the tens of MB.
  const MAX_TILES = 3000;
  const CONCURRENCY = 6;

  // --- tile math (slippy-map / Web Mercator) --- //

  function lonToX(lon, z) {
    const n = 2 ** z;
    return clamp(Math.floor((lon + 180) / 360 * n), 0, n - 1);
  }
  function latToY(lat, z) {
    const n = 2 ** z;
    const r = lat * Math.PI / 180;
    const y = Math.floor((1 - Math.log(Math.tan(r) + 1 / Math.cos(r)) / Math.PI) / 2 * n);
    return clamp(y, 0, n - 1);
  }
  function clamp(v, lo, hi) { return Math.min(hi, Math.max(lo, v)); }

  /** Tiles at zoom z covering {south, west, north, east}. North is the smaller y. */
  function tileRange(b, z) {
    return { z, x0: lonToX(b.west, z), x1: lonToX(b.east, z),
             y0: latToY(b.north, z), y1: latToY(b.south, z) };
  }
  function rangeCount(r) { return (r.x1 - r.x0 + 1) * (r.y1 - r.y0 + 1); }

  /**
   * Decide which zooms to save for this view without blowing the cap:
   * from a few levels above the current zoom down to the deepest level
   * that still fits. Returns { ranges, count, zMin, zMax }.
   */
  function planSave(bounds, zoom, cap = MAX_TILES) {
    const zNow = Math.round(zoom);
    const zMin = Math.max(0, zNow - ZOOM_OUT_LEVELS);
    const ranges = [];
    let count = 0;
    for (let z = zMin; z <= MAX_SAVE_ZOOM; z++) {
      const r = tileRange(bounds, z);
      const c = rangeCount(r);
      if (count + c > cap) break;
      ranges.push(r); count += c;
    }
    const zMax = ranges.length ? ranges[ranges.length - 1].z : null;
    return { ranges, count, zMin, zMax };
  }

  function* tilesOf(ranges) {
    for (const r of ranges)
      for (let x = r.x0; x <= r.x1; x++)
        for (let y = r.y0; y <= r.y1; y++) yield { z: r.z, x, y };
  }

  /** One URL per tile regardless of which CDN subdomain served it. */
  function tileUrl(z, x, y, s = SUBDOMAINS[0]) {
    return TILE_URL.replace("{s}", s).replace("{z}", z).replace("{x}", x).replace("{y}", y);
  }

  const TILE_RE = /^https:\/\/[a-d]\.basemaps\.cartocdn\.com\/dark_all\/(\d+)\/(\d+)\/(\d+)(@2x)?\.png$/;
  /** Canonical cache key for a tile request URL, or null if it isn't one of ours. */
  function tileKey(url) {
    const m = TILE_RE.exec(String(url).split("?")[0]);
    if (!m) return null;
    return tileUrl(+m[1], +m[2], +m[3]).replace(".png", (m[4] || "") + ".png");
  }

  // --- browser side --- //

  async function fetchIntoCache(cache, url) {
    if (await cache.match(url)) return "had";
    const res = await fetch(url, { mode: "cors", credentials: "omit" });
    if (!res.ok) throw new Error("HTTP " + res.status);
    await cache.put(url, res);
    return "got";
  }

  /** Pull every planned tile into the cache. onProgress({done, total, failed}). */
  async function saveTiles(plan, onProgress) {
    const cache = await caches.open(TILE_CACHE);
    const queue = tilesOf(plan.ranges);
    const stats = { done: 0, got: 0, had: 0, failed: 0, total: plan.count };
    async function worker() {
      for (let next = queue.next(); !next.done; next = queue.next()) {
        const t = next.value;
        try { stats[await fetchIntoCache(cache, tileUrl(t.z, t.x, t.y))]++; }
        catch (e) { stats.failed++; }
        stats.done++;
        if (onProgress) onProgress(stats);
      }
    }
    await Promise.all(Array.from({ length: CONCURRENCY }, worker));
    return stats;
  }

  async function savedCount() {
    if (typeof caches === "undefined") return 0;
    const cache = await caches.open(TILE_CACHE);
    return (await cache.keys()).length;
  }

  /**
   * Wire the page: register the service worker and hook up the save button.
   * `button` and `label` are DOM elements; the map is a Leaflet map.
   */
  function attach(map, button, label) {
    const say = t => { label.textContent = t; };
    const supported = typeof caches !== "undefined" && "serviceWorker" in navigator &&
                      window.isSecureContext;
    if (!supported) {
      button.disabled = true;
      say(location.protocol === "file:" ? "Offline maps need the page served over HTTPS."
                                        : "This browser can't save maps offline.");
      return;
    }

    navigator.serviceWorker.register("sw.js").catch(e => say("Offline setup failed: " + e.message));
    const refresh = () => savedCount().then(n => say(n ? `${n} tiles saved offline` : "No offline tiles yet"));
    refresh();

    button.addEventListener("click", async () => {
      const b = map.getBounds();
      const plan = planSave({ south: b.getSouth(), west: b.getWest(),
                              north: b.getNorth(), east: b.getEast() }, map.getZoom());
      if (!plan.count) {
        say("Zoom in closer — this area is too big to save.");
        return;
      }
      button.disabled = true;
      // Ask the browser not to evict the cache under storage pressure.
      // Best effort: Chrome grants it silently for installed/engaged sites.
      if (navigator.storage && navigator.storage.persist) navigator.storage.persist().catch(() => {});
      try {
        const s = await saveTiles(plan, p => say(`Saving map… ${p.done}/${p.total}`));
        const deep = plan.zMax < MAX_SAVE_ZOOM ? ` (zoom in and save again for detail past z${plan.zMax})` : "";
        say(s.failed ? `Saved ${s.done - s.failed}/${s.total} tiles — ${s.failed} failed, try again with signal`
                     : `Saved z${plan.zMin}–${plan.zMax}: ${s.got} new, ${s.had} already had${deep}`);
      } catch (e) {
        say("Save failed: " + e.message);
      } finally {
        button.disabled = false;
      }
    });
  }

  return {
    TILE_URL, SUBDOMAINS, ATTRIBUTION, TILE_CACHE, MAX_SAVE_ZOOM, ZOOM_OUT_LEVELS, MAX_TILES,
    lonToX, latToY, tileRange, rangeCount, planSave, tilesOf, tileUrl, tileKey,
    saveTiles, savedCount, attach,
  };
});
