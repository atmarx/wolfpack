#pragma once
//
// WolfpackProtocol.h — pure wire/parse logic for the Wolfpack color/role beacon.
//
// DELIBERATELY standalone: it pulls in nothing from the Meshtastic firmware so
// that host-native unit tests can exercise it without dragging in the mesh
// stack. Only freestanding C headers are allowed here. Keep it that way.
//
#include <ctype.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>

// 6 teams (rainbow order), identified by the first char of the node's short_name.
enum WolfpackColor : uint8_t { WP_COLOR_NONE = 0, WP_RED, WP_ORANGE, WP_YELLOW, WP_GREEN, WP_BLUE, WP_VIOLET };
static const uint8_t WP_NUM_COLORS = 6;

// Position in the line, from the second char of the short_name.
// (Slice 4 renamed "tail" -> "sweep"; the wire value is unchanged.)
enum WolfpackRole : uint8_t { WP_ROLE_NONE = 0, WP_LEADER, WP_MIDDLE, WP_SWEEP };
static const uint8_t WP_NUM_ROLES = 3;

// On-wire beacon, packed by hand (no padding assumptions), little-endian ints.
//
// Version 3 (slice 5) carries the sender's position INSIDE the beacon. Why:
// Meshtastic truncates POSITION_APP packets to the channel's position_precision
// (default 13 bits = ~5.8 km cells, PositionPrecision.cpp) and rate-limits smart
// position broadcasts to one per 5 minutes — useless for a moving pack. Private
// application payloads like this one are never truncated, so the beacon is the
// position channel now. NodeDB positions remain only a fallback for v2 peers.
//
// A v2 (4-byte, slice-4) beacon is still accepted as color/role-only. v1 is
// rejected (the color enum was renumbered into rainbow order for v2).
static const uint8_t WP_BEACON_VERSION = 3;
static const uint8_t WP_BEACON_VERSION_V2 = 2;
static const size_t WP_BEACON_SIZE = 12;
static const size_t WP_BEACON_SIZE_V2 = 4;

// flags bitfield. bit0 = lat_i/lon_i carry a real fix.
static const uint8_t WP_FLAG_HAS_POSITION = 0x01;

struct WolfpackBeacon {
    uint8_t version;
    uint8_t color;  // WolfpackColor
    uint8_t role;   // WolfpackRole
    uint8_t flags;  // WP_FLAG_*
    int32_t lat_i;  // latitude  * 1e7 (Meshtastic native fixed-point), valid iff HAS_POSITION
    int32_t lon_i;  // longitude * 1e7, valid iff HAS_POSITION
};

static inline void wp_writeI32LE(uint8_t *p, int32_t v)
{
    const uint32_t u = (uint32_t)v; // defined behavior for negative values
    p[0] = (uint8_t)(u & 0xFF);
    p[1] = (uint8_t)((u >> 8) & 0xFF);
    p[2] = (uint8_t)((u >> 16) & 0xFF);
    p[3] = (uint8_t)((u >> 24) & 0xFF);
}

static inline int32_t wp_readI32LE(const uint8_t *p)
{
    const uint32_t u = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    return (int32_t)u;
}

// Serialize a v3 beacon into buf. Returns bytes written (WP_BEACON_SIZE) or 0 if
// buf is null or too small. No allocation.
inline size_t wp_packBeacon(const WolfpackBeacon &b, uint8_t *buf, size_t buflen)
{
    if (buf == NULL || buflen < WP_BEACON_SIZE)
        return 0;
    buf[0] = b.version;
    buf[1] = b.color;
    buf[2] = b.role;
    buf[3] = b.flags;
    wp_writeI32LE(buf + 4, b.lat_i);
    wp_writeI32LE(buf + 8, b.lon_i);
    return WP_BEACON_SIZE;
}

// Deserialize a beacon. Accepts v3 (12 bytes) and legacy v2 (4 bytes, parsed as
// color/role with no position — flags are zeroed since v2's bit0 meant something
// else). Returns false (leaving out untouched) on null buffer, short buffer, or
// a version we don't speak.
inline bool wp_unpackBeacon(const uint8_t *buf, size_t len, WolfpackBeacon &out)
{
    if (buf == NULL)
        return false;
    if (len >= WP_BEACON_SIZE && buf[0] == WP_BEACON_VERSION) {
        out.version = buf[0];
        out.color = buf[1];
        out.role = buf[2];
        out.flags = buf[3];
        out.lat_i = wp_readI32LE(buf + 4);
        out.lon_i = wp_readI32LE(buf + 8);
        if (!(out.flags & WP_FLAG_HAS_POSITION)) {
            out.lat_i = 0;
            out.lon_i = 0;
        }
        return true;
    }
    if (len >= WP_BEACON_SIZE_V2 && buf[0] == WP_BEACON_VERSION_V2) {
        out.version = buf[0];
        out.color = buf[1];
        out.role = buf[2];
        out.flags = 0; // v2 flag bits are not ours; no position on the wire
        out.lat_i = 0;
        out.lon_i = 0;
        return true;
    }
    return false;
}

// Derive (color, role) from a node short_name. char0 -> color (R/Y/G/B),
// char1 -> role (L/M/T), both case-insensitive. Each field independently falls
// back to NONE on a non-match or when the string is too short / null. Pure, no
// allocation, no global state read.
inline void wp_parseColorRole(const char *shortName, WolfpackColor &color, WolfpackRole &role)
{
    color = WP_COLOR_NONE;
    role = WP_ROLE_NONE;

    if (shortName == NULL || shortName[0] == '\0')
        return;

    switch (toupper((unsigned char)shortName[0])) {
    case 'R':
        color = WP_RED;
        break;
    case 'O':
        color = WP_ORANGE;
        break;
    case 'Y':
        color = WP_YELLOW;
        break;
    case 'G':
        color = WP_GREEN;
        break;
    case 'B':
        color = WP_BLUE;
        break;
    case 'V':
        color = WP_VIOLET;
        break;
    default:
        color = WP_COLOR_NONE;
        break;
    }

    if (shortName[1] == '\0')
        return; // single char: role stays NONE

    switch (toupper((unsigned char)shortName[1])) {
    case 'L':
        role = WP_LEADER;
        break;
    case 'M':
        role = WP_MIDDLE;
        break;
    case 'S':
    case 'T': // legacy: slice-3 named the rear rider "tail"
        role = WP_SWEEP;
        break;
    default:
        role = WP_ROLE_NONE;
        break;
    }
}

// Two nodes are teammates only when both carry a real color and it matches.
inline bool wp_isSameTeam(WolfpackColor a, WolfpackColor b)
{
    return a != WP_COLOR_NONE && b != WP_COLOR_NONE && a == b;
}

// ----------------------------------------------------------------------------
// Geo math for the screen frame (slice 3). Kept pure and host-tested so the
// distance/bearing the rider sees is exactly what the unit tests pin down — and
// so it links identically on-device and on the native runner (no firmware
// GeoCoord dependency dragged into the test build).
// ----------------------------------------------------------------------------

static const double WP_DEG2RAD = 0.017453292519943295; // pi / 180
static const double WP_EARTH_R_M = 6371000.0;          // mean Earth radius, meters

// Great-circle (haversine) distance in meters between two lat/lon points (deg).
inline float wp_distanceMeters(double lat1, double lon1, double lat2, double lon2)
{
    const double rlat1 = lat1 * WP_DEG2RAD;
    const double rlat2 = lat2 * WP_DEG2RAD;
    const double dLat = (lat2 - lat1) * WP_DEG2RAD;
    const double dLon = (lon2 - lon1) * WP_DEG2RAD;
    const double s1 = sin(dLat * 0.5);
    const double s2 = sin(dLon * 0.5);
    const double a = s1 * s1 + cos(rlat1) * cos(rlat2) * s2 * s2;
    const double c = 2.0 * atan2(sqrt(a), sqrt(1.0 - a));
    return (float)(WP_EARTH_R_M * c);
}

// Initial great-circle bearing in degrees [0,360) from point 1 to point 2.
// 0 = due north, 90 = east, 180 = south, 270 = west.
inline float wp_bearingDegrees(double lat1, double lon1, double lat2, double lon2)
{
    const double rlat1 = lat1 * WP_DEG2RAD;
    const double rlat2 = lat2 * WP_DEG2RAD;
    const double dLon = (lon2 - lon1) * WP_DEG2RAD;
    const double y = sin(dLon) * cos(rlat2);
    const double x = cos(rlat1) * sin(rlat2) - sin(rlat1) * cos(rlat2) * cos(dLon);
    double deg = atan2(y, x) / WP_DEG2RAD;
    if (deg < 0.0)
        deg += 360.0;
    if (deg >= 360.0)
        deg -= 360.0;
    return (float)deg;
}

// 8-point compass label for an absolute bearing in degrees. Used when the node
// has no fresh heading (standstill, no magnetometer) — an honest "NE" beats a
// lying relative arrow.
inline const char *wp_cardinal8(float bearingDeg)
{
    static const char *const DIRS[8] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    int idx = (int)((bearingDeg + 22.5f) / 45.0f);
    return DIRS[idx & 7];
}

// Indices of the two smallest values in dists[0..n) -> out[0..ret). Ties resolve
// to the lower index. Returns 0, 1, or 2. Pure, no allocation, NULL-safe at n=0.
inline uint8_t wp_twoNearest(const float *dists, uint8_t n, uint8_t out[2])
{
    int best1 = -1, best2 = -1;
    for (uint8_t i = 0; i < n; i++) {
        if (best1 < 0 || dists[i] < dists[best1]) {
            best2 = best1;
            best1 = (int)i;
        } else if (best2 < 0 || dists[i] < dists[best2]) {
            best2 = (int)i;
        }
    }
    uint8_t count = 0;
    if (best1 >= 0)
        out[count++] = (uint8_t)best1;
    if (best2 >= 0)
        out[count++] = (uint8_t)best2;
    return count;
}

// ----------------------------------------------------------------------------
// Slice 8: ghost trail — the lead's route as breadcrumbs, recorded by every
// follower from the lead's own position beacons. The radio never needs to know
// what a fork is; the trail always answers ONE question: "when the lead was
// where I am now, which way did they go?" On plain trail that's just "onward";
// at a fork it's the answer the sweep came for. Ring buffer, oldest overwritten:
// 1024 crumbs at >=20 m spacing covers ~20+ km of route in 8 KB.
// ----------------------------------------------------------------------------

#define WP_GHOST_MAX 1024
static const float WP_GHOST_SPACING_M = 20.0f; // min distance between stored crumbs
static const float WP_GHOST_NEAR_M = 30.0f;    // "the lead was here" match radius

struct WolfpackGhostTrail {
    int32_t lat_i[WP_GHOST_MAX]; // 1e-7 degrees, same fixed-point as the beacon
    int32_t lon_i[WP_GHOST_MAX];
    uint16_t count; // valid crumbs (saturates at WP_GHOST_MAX)
    uint16_t head;  // ring slot the NEXT crumb will be written to
};

inline void wp_ghostReset(WolfpackGhostTrail &t)
{
    t.count = 0;
    t.head = 0;
}

// Ring slot of the logical i-th crumb (0 = oldest surviving).
inline uint16_t wp_ghostSlot(const WolfpackGhostTrail &t, uint16_t logical)
{
    return (uint16_t)((t.head + (uint32_t)WP_GHOST_MAX - t.count + logical) % WP_GHOST_MAX);
}

// Append a crumb unless it's within WP_GHOST_SPACING_M of the newest one — that
// dedupes stationary heartbeats and keeps successive crumbs far enough apart
// that a crumb->next bearing means something. Returns whether it was stored.
inline bool wp_ghostAppend(WolfpackGhostTrail &t, int32_t lat_i, int32_t lon_i)
{
    if (t.count > 0) {
        const uint16_t newest = wp_ghostSlot(t, (uint16_t)(t.count - 1));
        if (wp_distanceMeters(t.lat_i[newest] * 1e-7, t.lon_i[newest] * 1e-7, lat_i * 1e-7, lon_i * 1e-7) <
            WP_GHOST_SPACING_M)
            return false;
    }
    t.lat_i[t.head] = lat_i;
    t.lon_i[t.head] = lon_i;
    t.head = (uint16_t)((t.head + 1) % WP_GHOST_MAX);
    if (t.count < WP_GHOST_MAX)
        t.count++;
    return true;
}

// The ghost lookup: nearest crumb to (myLat, myLon); if it's within
// WP_GHOST_NEAR_M *and* has a successor, out = bearing crumb -> successor (the
// direction the lead departed from this spot) and return true. Nearest wins on
// switchbacks — the closest leg is almost always the leg you're standing on.
// The newest crumb has no successor (the lead just left it; the live arrow
// covers that), and an empty/one-crumb trail can't answer at all.
inline bool wp_ghostQuery(const WolfpackGhostTrail &t, double myLat, double myLon, float &bearingDegOut)
{
    if (t.count < 2)
        return false;
    int best = -1;
    float bestDist = 0.0f;
    for (uint16_t i = 0; i + 1 < t.count; i++) {
        const uint16_t s = wp_ghostSlot(t, i);
        const float d = wp_distanceMeters(myLat, myLon, t.lat_i[s] * 1e-7, t.lon_i[s] * 1e-7);
        if (best < 0 || d < bestDist) {
            best = (int)i;
            bestDist = d;
        }
    }
    if (best < 0 || bestDist > WP_GHOST_NEAR_M)
        return false;
    const uint16_t from = wp_ghostSlot(t, (uint16_t)best);
    const uint16_t to = wp_ghostSlot(t, (uint16_t)(best + 1));
    bearingDegOut = wp_bearingDegrees(t.lat_i[from] * 1e-7, t.lon_i[from] * 1e-7, t.lat_i[to] * 1e-7, t.lon_i[to] * 1e-7);
    return true;
}

// ----------------------------------------------------------------------------
// Display + short-name code helpers (slice 4 picker). The short-name encodes the
// team: char0 = color, char1 = role, optional trailing digit(s) = collision suffix.
// ----------------------------------------------------------------------------

inline char wp_colorChar(WolfpackColor c)
{
    switch (c) {
    case WP_RED:    return 'R';
    case WP_ORANGE: return 'O';
    case WP_YELLOW: return 'Y';
    case WP_GREEN:  return 'G';
    case WP_BLUE:   return 'B';
    case WP_VIOLET: return 'V';
    default:        return '?';
    }
}

inline char wp_roleChar(WolfpackRole r)
{
    switch (r) {
    case WP_LEADER: return 'L';
    case WP_MIDDLE: return 'M';
    case WP_SWEEP:  return 'S';
    default:        return '?';
    }
}

inline const char *wp_colorName(WolfpackColor c)
{
    switch (c) {
    case WP_RED:    return "Red";
    case WP_ORANGE: return "Orange";
    case WP_YELLOW: return "Yellow";
    case WP_GREEN:  return "Green";
    case WP_BLUE:   return "Blue";
    case WP_VIOLET: return "Violet";
    default:        return "--";
    }
}

inline const char *wp_roleName(WolfpackRole r)
{
    switch (r) {
    case WP_LEADER: return "Lead";
    case WP_MIDDLE: return "Mid";
    case WP_SWEEP:  return "Sweep";
    default:        return "-";
    }
}

// Map a picker list index (0..WP_NUM_COLORS-1) to a color, in rainbow order.
inline WolfpackColor wp_colorFromIndex(int i)
{
    static const WolfpackColor ORDER[WP_NUM_COLORS] = {WP_RED, WP_ORANGE, WP_YELLOW, WP_GREEN, WP_BLUE, WP_VIOLET};
    return (i >= 0 && i < (int)WP_NUM_COLORS) ? ORDER[i] : WP_COLOR_NONE;
}

// Map a picker list index (0..WP_NUM_ROLES-1) to a role.
inline WolfpackRole wp_roleFromIndex(int i)
{
    static const WolfpackRole ORDER[WP_NUM_ROLES] = {WP_LEADER, WP_MIDDLE, WP_SWEEP};
    return (i >= 0 && i < (int)WP_NUM_ROLES) ? ORDER[i] : WP_ROLE_NONE;
}
