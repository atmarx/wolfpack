/* ------------------------------------------------------------------ *
 * Wolfpack — offline basemap.
 *
 * The positions come over Bluetooth and never need a network. The basemap
 * does. On a trail with no cell service that used to mean live dots on a
 * blank grey square — the one failure that makes the whole map pointless.
 *
 * The basemap is one file per place we ride, hosted ourselves: a small
 * PMTiles extract cut from the OpenStreetMap-derived Protomaps build. No
 * tile server, no API key, no per-tile crawl of somebody else's CDN — and
 * "save the map for the trail" is just "download one file".
 *
 * Shared by the page and sw.js (which loads this with importScripts), so
 * the two can't disagree about what's cached or where it lives.
 * ------------------------------------------------------------------ */
(function (root, factory) {
  const api = factory();
  if (typeof module === "object" && module.exports) module.exports = api;
  else root.WolfpackOffline = api;
})(typeof self !== "undefined" ? self : this, function () {
  "use strict";

  // One extract per riding area. `bounds` is the box the file was cut to
  // ([[south, west], [north, east]]; `pmtiles extract --bbox=west,south,east,north`)
  // — outside it the map is blank, so the page uses it to pick the right file.
  const REGIONS = [
    { id: "pennypack", name: "Pennypack", path: "basemap/pennypack.pmtiles",
      bounds: [[40.01, -75.10], [40.12, -74.99]] },
    { id: "bluemountain", name: "Blue Mountain", path: "basemap/bluemountain.pmtiles",
      bounds: [[40.75, -75.65], [40.88, -75.40]] },
  ];
  const BASEMAP_CACHE = "wolfpack-basemap-v1";
  const ATTRIBUTION = '© <a href="https://www.openstreetmap.org/copyright">OpenStreetMap</a> · ' +
                      '<a href="https://protomaps.com">Protomaps</a>';
  // The extract stops at z15; the renderer draws past that from the same
  // vector data, so zooming in stays sharp instead of going blank.
  const MAX_DATA_ZOOM = 15;

  function region(id) { return REGIONS.find(r => r.id === id) || REGIONS[0]; }

  /** The region whose basemap file this request is for, or null. */
  function regionFor(url) {
    let p;
    try { p = new URL(url, "http://x/").pathname; } catch (e) { return null; }
    return REGIONS.find(r => p.endsWith("/" + r.path)) || null;
  }

  /** Is this request for a basemap file? */
  function isBasemap(url) { return !!regionFor(url); }

  /** The region whose map covers this spot, or null. */
  function regionAt(lat, lon) {
    return REGIONS.find(({ bounds: [[s, w], [n, e]] }) =>
      lat >= s && lat <= n && lon >= w && lon <= e) || null;
  }

  /** "bytes=100-199" → {start, end}; null if absent or unsupported. */
  function parseRange(header, size) {
    if (!header) return null;
    const m = /^bytes=(\d*)-(\d*)$/.exec(header.trim());
    if (!m) return null;
    let start, end;
    if (m[1] === "") {                       // suffix: last N bytes
      if (m[2] === "") return null;
      start = Math.max(0, size - Number(m[2]));
      end = size - 1;
    } else {
      start = Number(m[1]);
      end = m[2] === "" ? size - 1 : Math.min(Number(m[2]), size - 1);
    }
    if (!(start >= 0 && end >= start && start < size)) return null;
    return { start, end };
  }

  function human(bytes) {
    if (!bytes) return "0 MB";
    return bytes >= 1e6 ? (bytes / 1e6).toFixed(1) + " MB" : Math.round(bytes / 1e3) + " kB";
  }

  // --- browser side --- //

  async function savedBytes(r) {
    if (typeof caches === "undefined") return 0;
    const cache = await caches.open(BASEMAP_CACHE);
    const hit = await cache.match(r.path);
    if (!hit) return 0;
    return Number(hit.headers.get("content-length")) || (await hit.blob()).size;
  }

  /** Download a region's basemap into the cache. onProgress({done, total}). */
  async function saveBasemap(r, onProgress) {
    const cache = await caches.open(BASEMAP_CACHE);
    const res = await fetch(r.path, { cache: "reload" });
    if (!res.ok) throw new Error("HTTP " + res.status);
    const total = Number(res.headers.get("content-length")) || 0;

    // Read it through so there's a progress bar for an 8 MB download on a
    // parking-lot connection, then hand the assembled body to the cache.
    const reader = res.body && res.body.getReader ? res.body.getReader() : null;
    let body, size;
    if (reader) {
      const chunks = [];
      let done = 0;
      for (;;) {
        const r = await reader.read();
        if (r.done) break;
        chunks.push(r.value);
        done += r.value.length;
        if (onProgress) onProgress({ done, total });
      }
      body = new Blob(chunks);
      size = done;
    } else {
      body = await res.blob();
      size = body.size;
    }

    await cache.put(r.path, new Response(body, {
      status: 200,
      headers: {
        "Content-Type": "application/octet-stream",
        "Content-Length": String(size),
        "Accept-Ranges": "bytes",
      },
    }));
    return { bytes: size };
  }

  /** Serve a Range request out of an already-cached full response. */
  async function sliceCached(cached, rangeHeader) {
    const blob = await cached.blob();
    const range = parseRange(rangeHeader, blob.size);
    if (!range) {
      return new Response(blob, { status: 200, headers: {
        "Content-Type": "application/octet-stream",
        "Content-Length": String(blob.size),
        "Accept-Ranges": "bytes",
      } });
    }
    const part = blob.slice(range.start, range.end + 1);
    return new Response(part, { status: 206, statusText: "Partial Content", headers: {
      "Content-Type": "application/octet-stream",
      "Content-Length": String(part.size),
      "Accept-Ranges": "bytes",
      "Content-Range": `bytes ${range.start}-${range.end}/${blob.size}`,
    } });
  }

  /**
   * Register the service worker and wire the save button, which saves
   * whichever region currentRegion() returns. Returns refresh(), for the
   * page to call when it switches region.
   */
  function attach(button, label, currentRegion) {
    const say = t => { label.textContent = t; };
    const supported = typeof caches !== "undefined" && "serviceWorker" in navigator &&
                      self.isSecureContext;
    if (!supported) {
      button.disabled = true;
      say(location.protocol === "file:" ? "Offline maps need the page served over HTTPS."
                                        : "This browser can't save the map offline.");
      return () => {};
    }

    navigator.serviceWorker.register("sw.js").catch(e => say("Offline setup failed: " + e.message));
    const refresh = () => {
      const r = currentRegion();
      return savedBytes(r).then(n =>
        say(n ? `${r.name} map saved (${human(n)})` : `${r.name} map not saved yet`));
    };
    refresh();

    button.addEventListener("click", async () => {
      button.disabled = true;
      // Ask the browser not to evict it under storage pressure. Best effort.
      if (navigator.storage && navigator.storage.persist) navigator.storage.persist().catch(() => {});
      try {
        const r = currentRegion();
        say(`Saving ${r.name} map…`);
        const { bytes } = await saveBasemap(r, p => say(p.total
          ? `Saving map… ${Math.round(100 * p.done / p.total)}%`
          : `Saving map… ${human(p.done)}`));
        say(`${r.name} map saved (${human(bytes)}) — good with no signal`);
      } catch (e) {
        say("Save failed: " + e.message + " — try again with signal");
      } finally {
        button.disabled = false;
      }
    });
    return refresh;
  }

  return {
    REGIONS, BASEMAP_CACHE, ATTRIBUTION, MAX_DATA_ZOOM,
    region, regionFor, regionAt, isBasemap, parseRange, human, savedBytes, saveBasemap, sliceCached, attach,
  };
});
