/* ------------------------------------------------------------------ *
 * Wolfpack — keeping the ride.
 *
 * The live map records every 5 s from the moment a radio connects. That
 * recording used to live only in the tab, so a reload, a closed tab, or
 * Android freezing Chrome with the screen off threw the ride away. Now the
 * page saves it to the phone (IndexedDB) as it goes, picks it back up on
 * the next load, and can hand it over as a GPX file.
 *
 * Nothing leaves the phone. The ride relay (docs/RIDE-RELAY.md) is the
 * design for sharing; this is just not losing it.
 *
 * toGPX is pure and tested in node (test-record.js); the storage half is a
 * thin wrapper over IndexedDB that only runs in the browser.
 * ------------------------------------------------------------------ */
(function (root, factory) {
  const api = factory();
  if (typeof module === "object" && module.exports) module.exports = api;
  else root.WolfpackRecord = api;
})(typeof self !== "undefined" ? self : this, function () {
  "use strict";

  const DB_NAME = "wolfpack-rides";
  const STORE = "rides";
  const KEEP_DAYS = 7;          // matches the ride relay's retention

  const esc = s => String(s).replace(/[<>&"']/g, c =>
    ({ "<": "&lt;", ">": "&gt;", "&": "&amp;", '"': "&quot;", "'": "&apos;" }[c]));

  /**
   * The ride as GPX 1.1: one track per radio, a new segment wherever that
   * radio went quiet. Only fresh fixes (truePos) count — a greyed-out
   * last-known position is not somewhere the rider was at that time.
   * A radio parked in one spot repeats its fix every step; those repeats
   * are dropped, so a long stop is one point rather than hundreds.
   */
  function toGPX(ride) {
    const when = k => new Date(ride.times[k]).toISOString();
    const tracks = ride.nodes.map(node => {
      const team = (ride.teams[node.team] && ride.teams[node.team].name) || node.team;
      const segs = [];
      let seg = null, prev = null;
      node.truePos.forEach((p, k) => {
        if (!p || ride.times[k] == null) { seg = null; prev = null; return; }
        if (!seg) segs.push(seg = []);
        if (prev && prev[0] === p[0] && prev[1] === p[1]) return;
        seg.push(`<trkpt lat="${p[0].toFixed(7)}" lon="${p[1].toFixed(7)}"><time>${when(k)}</time></trkpt>`);
        prev = p;
      });
      if (!segs.length) return "";
      return `  <trk><name>${esc(node.short + " — " + team + " " + node.role)}</name>\n` +
        segs.map(s => "    <trkseg>\n      " + s.join("\n      ") + "\n    </trkseg>\n").join("") +
        "  </trk>\n";
    }).join("");
    return '<?xml version="1.0" encoding="UTF-8"?>\n' +
      '<gpx version="1.1" creator="Wolfpack" xmlns="http://www.topografix.com/GPX/1/1">\n' +
      `  <metadata><name>${esc(rideName(ride))}</name><time>${new Date(ride.startedAt).toISOString()}</time></metadata>\n` +
      tracks + "</gpx>\n";
  }

  /** "Wolfpack ride 2026-10-03 11:42", in the phone's local time. */
  function rideName(ride) {
    const d = new Date(ride.startedAt);
    const p = n => String(n).padStart(2, "0");
    return `Wolfpack ride ${d.getFullYear()}-${p(d.getMonth() + 1)}-${p(d.getDate())} ` +
           `${p(d.getHours())}:${p(d.getMinutes())}`;
  }

  /** Has anything actually been recorded — a single fresh fix? */
  function hasFixes(ride) {
    return !!(ride && ride.times && ride.nodes.some(n => n.truePos.some(Boolean)));
  }

  // --- browser side: IndexedDB --- //

  function open() {
    return new Promise((resolve, reject) => {
      const req = indexedDB.open(DB_NAME, 1);
      req.onupgradeneeded = () => req.result.createObjectStore(STORE, { keyPath: "startedAt" });
      req.onsuccess = () => resolve(req.result);
      req.onerror = () => reject(req.error);
    });
  }

  function tx(mode, fn) {
    return open().then(db => new Promise((resolve, reject) => {
      const t = db.transaction(STORE, mode);
      const out = fn(t.objectStore(STORE));
      t.oncomplete = () => { db.close(); resolve(out && out.result); };
      t.onerror = () => { db.close(); reject(t.error); };
    }));
  }

  /** Save (or overwrite) a ride. Keyed by when it started. */
  function save(ride) {
    if (typeof indexedDB === "undefined" || !hasFixes(ride)) return Promise.resolve();
    return tx("readwrite", s => s.put(ride));
  }

  /** The most recent saved ride, or null. Prunes anything past KEEP_DAYS. */
  async function latest() {
    if (typeof indexedDB === "undefined") return null;
    const all = (await tx("readonly", s => s.getAll())) || [];
    const cutoff = Date.now() - KEEP_DAYS * 86400e3;
    const stale = all.filter(r => r.startedAt < cutoff);
    if (stale.length) await tx("readwrite", s => stale.forEach(r => s.delete(r.startedAt)));
    const keep = all.filter(r => r.startedAt >= cutoff).sort((a, b) => b.startedAt - a.startedAt);
    return keep[0] || null;
  }

  /** Hand the ride to the phone as a .gpx download. */
  function download(ride) {
    const blob = new Blob([toGPX(ride)], { type: "application/gpx+xml" });
    const a = document.createElement("a");
    a.href = URL.createObjectURL(blob);
    a.download = rideName(ride).replace(/[ :]/g, "-").toLowerCase() + ".gpx";
    document.body.appendChild(a);
    a.click();
    a.remove();
    setTimeout(() => URL.revokeObjectURL(a.href), 10000);
  }

  return { KEEP_DAYS, toGPX, rideName, hasFixes, save, latest, download };
});
