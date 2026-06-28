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

// On-wire beacon. Fixed 4 bytes, no padding assumptions — we pack/unpack by hand.
// Version 2 = slice-4 palette (6 colors). A v1 (slice-3) beacon is rejected rather
// than mis-parsed, since the color enum was renumbered into rainbow order.
static const uint8_t WP_BEACON_VERSION = 2;
static const size_t WP_BEACON_SIZE = 4;

// flags bitfield (reserved). bit0 = is_leader, held at 0 for this slice.
static const uint8_t WP_FLAG_IS_LEADER = 0x01;

struct WolfpackBeacon {
    uint8_t version;
    uint8_t color; // WolfpackColor
    uint8_t role;  // WolfpackRole
    uint8_t flags; // reserved (WP_FLAG_*)
};

// Serialize a beacon into buf. Returns bytes written (WP_BEACON_SIZE) or 0 if
// buf is null or too small. No allocation.
inline size_t wp_packBeacon(const WolfpackBeacon &b, uint8_t *buf, size_t buflen)
{
    if (buf == NULL || buflen < WP_BEACON_SIZE)
        return 0;
    buf[0] = b.version;
    buf[1] = b.color;
    buf[2] = b.role;
    buf[3] = b.flags;
    return WP_BEACON_SIZE;
}

// Deserialize a beacon. Returns false (leaving out untouched) if the buffer is
// null, shorter than WP_BEACON_SIZE, or carries a version we don't speak.
inline bool wp_unpackBeacon(const uint8_t *buf, size_t len, WolfpackBeacon &out)
{
    if (buf == NULL || len < WP_BEACON_SIZE)
        return false;
    if (buf[0] != WP_BEACON_VERSION)
        return false;
    out.version = buf[0];
    out.color = buf[1];
    out.role = buf[2];
    out.flags = buf[3];
    return true;
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
