#pragma once
//
// WolfpackProtocol.h — pure wire/parse logic for the Wolfpack color/role beacon.
//
// DELIBERATELY standalone: it pulls in nothing from the Meshtastic firmware so
// that host-native unit tests can exercise it without dragging in the mesh
// stack. Only freestanding C headers are allowed here. Keep it that way.
//
#include <ctype.h>
#include <stddef.h>
#include <stdint.h>

// 4 skill teams, identified by the first char of the node's short_name.
enum WolfpackColor : uint8_t { WP_COLOR_NONE = 0, WP_RED, WP_YELLOW, WP_GREEN, WP_BLUE };

// Position in the line, from the second char of the short_name.
enum WolfpackRole : uint8_t { WP_ROLE_NONE = 0, WP_LEADER, WP_MIDDLE, WP_TAIL };

// On-wire beacon. Fixed 4 bytes, no padding assumptions — we pack/unpack by hand.
static const uint8_t WP_BEACON_VERSION = 1;
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
    case 'Y':
        color = WP_YELLOW;
        break;
    case 'G':
        color = WP_GREEN;
        break;
    case 'B':
        color = WP_BLUE;
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
    case 'T':
        role = WP_TAIL;
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
