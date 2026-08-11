// Host-native unit tests for the pure Wolfpack protocol logic.
//
// Only WolfpackProtocol.h is exercised here — it has no Meshtastic dependencies,
// so these tests are fast and isolated. Run with:
//   pio test -e native -f test_wolfpack
//
#include "Arduino.h"
#include "TestUtil.h"
#include "modules/WolfpackProtocol.h"
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

// --- pack / unpack round-trip ---

void test_pack_unpack_roundtrip()
{
    // Philadelphia-ish fix: positive lat, NEGATIVE lon — signs must survive.
    WolfpackBeacon in = {WP_BEACON_VERSION, WP_GREEN, WP_LEADER, WP_FLAG_HAS_POSITION, 399500000, -751600000, 0};
    uint8_t buf[16] = {0};

    size_t n = wp_packBeacon(in, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_size_t(WP_BEACON_SIZE, n);
    TEST_ASSERT_EQUAL_HEX8(WP_BEACON_VERSION, buf[0]);
    TEST_ASSERT_EQUAL_HEX8(WP_GREEN, buf[1]);
    TEST_ASSERT_EQUAL_HEX8(WP_LEADER, buf[2]);
    TEST_ASSERT_EQUAL_HEX8(WP_FLAG_HAS_POSITION, buf[3]);

    WolfpackBeacon out;
    TEST_ASSERT_TRUE(wp_unpackBeacon(buf, n, out));
    TEST_ASSERT_EQUAL_UINT8(in.version, out.version);
    TEST_ASSERT_EQUAL_UINT8(in.color, out.color);
    TEST_ASSERT_EQUAL_UINT8(in.role, out.role);
    TEST_ASSERT_EQUAL_UINT8(in.flags, out.flags);
    TEST_ASSERT_EQUAL_INT32(in.lat_i, out.lat_i);
    TEST_ASSERT_EQUAL_INT32(in.lon_i, out.lon_i);
}

void test_unpack_zeroes_position_without_flag()
{
    // flags=0 but junk where lat/lon live: the parser must not surface it.
    WolfpackBeacon in = {WP_BEACON_VERSION, WP_RED, WP_MIDDLE, 0, 123456789, -987654321, 0};
    uint8_t buf[16] = {0};
    size_t n = wp_packBeacon(in, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_size_t(WP_BEACON_SIZE, n);

    WolfpackBeacon out;
    TEST_ASSERT_TRUE(wp_unpackBeacon(buf, n, out));
    TEST_ASSERT_EQUAL_UINT8(0, out.flags);
    TEST_ASSERT_EQUAL_INT32(0, out.lat_i);
    TEST_ASSERT_EQUAL_INT32(0, out.lon_i);
}

void test_unpack_accepts_legacy_v2()
{
    // A slice-4 radio still on v2 sends 4 bytes: color/role must land, position
    // must not be invented, and v2's foreign flag bits must be dropped.
    uint8_t v2[4] = {WP_BEACON_VERSION_V2, WP_BLUE, WP_SWEEP, 0x01};
    WolfpackBeacon out;
    TEST_ASSERT_TRUE(wp_unpackBeacon(v2, sizeof(v2), out));
    TEST_ASSERT_EQUAL_UINT8(WP_BEACON_VERSION_V2, out.version);
    TEST_ASSERT_EQUAL_UINT8(WP_BLUE, out.color);
    TEST_ASSERT_EQUAL_UINT8(WP_SWEEP, out.role);
    TEST_ASSERT_EQUAL_UINT8(0, out.flags); // no HAS_POSITION from a v2 peer
    TEST_ASSERT_EQUAL_INT32(0, out.lat_i);
    TEST_ASSERT_EQUAL_INT32(0, out.lon_i);
}

void test_pack_rejects_short_buffer()
{
    WolfpackBeacon in = {WP_BEACON_VERSION, WP_RED, WP_SWEEP, 0, 0, 0, 0};
    uint8_t buf[12] = {0}; // one byte short of a v4 beacon
    TEST_ASSERT_EQUAL_size_t(0, wp_packBeacon(in, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_size_t(0, wp_packBeacon(in, buf, 0));
}

void test_unpack_rejects_short_buffer()
{
    // 11 bytes claiming v4: too short for v4, wrong version for v3/v2 -> reject.
    uint8_t buf[11] = {WP_BEACON_VERSION, WP_RED, WP_LEADER, WP_FLAG_HAS_POSITION};
    WolfpackBeacon out;
    TEST_ASSERT_FALSE(wp_unpackBeacon(buf, sizeof(buf), out));
    // 12 bytes claiming v4: exactly a v3's length, but the version says v4 and
    // the epoch byte isn't there. Must NOT be salvaged as a v3.
    uint8_t v4short[12] = {WP_BEACON_VERSION, WP_RED, WP_LEADER, WP_FLAG_HAS_POSITION};
    TEST_ASSERT_FALSE(wp_unpackBeacon(v4short, sizeof(v4short), out));
    // 3 bytes claiming v2: also too short.
    uint8_t v2short[3] = {WP_BEACON_VERSION_V2, WP_RED, WP_LEADER};
    TEST_ASSERT_FALSE(wp_unpackBeacon(v2short, sizeof(v2short), out));
}

void test_unpack_rejects_bad_version()
{
    WolfpackBeacon out;
    uint8_t v1[13] = {1, WP_RED, WP_LEADER, 0}; // slice-3 beacon: rejected (palette renumbered)
    TEST_ASSERT_FALSE(wp_unpackBeacon(v1, sizeof(v1), out));
    uint8_t v5[13] = {5, WP_RED, WP_LEADER, 0}; // from the future: rejected
    TEST_ASSERT_FALSE(wp_unpackBeacon(v5, sizeof(v5), out));
    uint8_t v0[13] = {0, WP_RED, WP_LEADER, 0};
    TEST_ASSERT_FALSE(wp_unpackBeacon(v0, sizeof(v0), out));
}

// --- slice 9: ride epoch ---

void test_ride_epoch_roundtrip()
{
    WolfpackBeacon in = {WP_BEACON_VERSION, WP_VIOLET, WP_LEADER, WP_FLAG_HAS_POSITION, 399500000, -751600000, 200};
    uint8_t buf[16] = {0};
    size_t n = wp_packBeacon(in, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_size_t(WP_BEACON_SIZE, n);
    TEST_ASSERT_EQUAL_size_t(13, n);
    TEST_ASSERT_EQUAL_HEX8(200, buf[12]);

    WolfpackBeacon out;
    TEST_ASSERT_TRUE(wp_unpackBeacon(buf, n, out));
    TEST_ASSERT_EQUAL_UINT8(200, out.rideEpoch);
    TEST_ASSERT_EQUAL_INT32(399500000, out.lat_i); // epoch must not disturb the fix
    TEST_ASSERT_EQUAL_INT32(-751600000, out.lon_i);
}

void test_unpack_accepts_legacy_v3_as_no_ride()
{
    // A radio still on slice 8 sends 12 bytes. Position must still land; the
    // epoch must read as "none" so it never spuriously wipes a trail.
    uint8_t v3[12] = {WP_BEACON_VERSION_V3, WP_GREEN, WP_LEADER, WP_FLAG_HAS_POSITION};
    v3[4] = 0x40;
    v3[5] = 0xE3;
    v3[6] = 0xD3;
    v3[7] = 0x17; // 399500096-ish, exact value irrelevant
    WolfpackBeacon out;
    TEST_ASSERT_TRUE(wp_unpackBeacon(v3, sizeof(v3), out));
    TEST_ASSERT_EQUAL_UINT8(WP_BEACON_VERSION_V3, out.version);
    TEST_ASSERT_EQUAL_UINT8(WP_GREEN, out.color);
    TEST_ASSERT_EQUAL_UINT8(WP_LEADER, out.role);
    TEST_ASSERT_EQUAL_UINT8(WP_RIDE_EPOCH_NONE, out.rideEpoch);
    TEST_ASSERT_NOT_EQUAL(0, out.lat_i);
}

void test_unpack_v2_has_no_ride_epoch()
{
    uint8_t v2[4] = {WP_BEACON_VERSION_V2, WP_BLUE, WP_SWEEP, 0x01};
    WolfpackBeacon out;
    TEST_ASSERT_TRUE(wp_unpackBeacon(v2, sizeof(v2), out));
    TEST_ASSERT_EQUAL_UINT8(WP_RIDE_EPOCH_NONE, out.rideEpoch);
}

void test_next_ride_epoch_never_zero_never_repeats()
{
    // The sentinel and the previous value are both forbidden, at every input.
    for (uint32_t t = 0; t < 5000; t++) {
        for (unsigned prev = 0; prev < 256; prev += 17) {
            uint8_t e = wp_nextRideEpoch(t * 7u, (uint8_t)prev);
            TEST_ASSERT_NOT_EQUAL(WP_RIDE_EPOCH_NONE, e);
            TEST_ASSERT_NOT_EQUAL((uint8_t)prev, e);
        }
    }
}

void test_next_ride_epoch_wraps_past_255()
{
    // prev=255 forces the +1 to carry into 0 — which is the sentinel, so it must
    // step again rather than emit "no ride".
    uint8_t e = wp_nextRideEpoch(255u << 4, 255);
    TEST_ASSERT_NOT_EQUAL(WP_RIDE_EPOCH_NONE, e);
    TEST_ASSERT_NOT_EQUAL(255, e);
}

void test_next_ride_epoch_varies_with_press_time()
{
    // Two presses a few seconds apart must not collide — that is the whole
    // reason the epoch is drawn from millis() instead of a 1,2,3 counter.
    TEST_ASSERT_NOT_EQUAL(wp_nextRideEpoch(1000, 0), wp_nextRideEpoch(9000, 0));
}

void test_unpack_rejects_null()
{
    WolfpackBeacon out;
    TEST_ASSERT_FALSE(wp_unpackBeacon(NULL, 4, out));
}

// --- wp_parseColorRole: full R/Y/G/B x L/M/T grid + garbage/partial/null ---

static void expect_parse(const char *s, WolfpackColor ec, WolfpackRole er)
{
    WolfpackColor c;
    WolfpackRole r;
    wp_parseColorRole(s, c, r);
    TEST_ASSERT_EQUAL_UINT8(ec, c);
    TEST_ASSERT_EQUAL_UINT8(er, r);
}

void test_parse_full_grid()
{
    // 6 colors x 3 roles (sweep = 'S').
    const char *codes[18] = {"RL", "RM", "RS", "OL", "OM", "OS", "YL", "YM", "YS",
                             "GL", "GM", "GS", "BL", "BM", "BS", "VL", "VM", "VS"};
    const WolfpackColor cols[6] = {WP_RED, WP_ORANGE, WP_YELLOW, WP_GREEN, WP_BLUE, WP_VIOLET};
    const WolfpackRole roles[3] = {WP_LEADER, WP_MIDDLE, WP_SWEEP};
    for (int c = 0; c < 6; c++)
        for (int r = 0; r < 3; r++)
            expect_parse(codes[c * 3 + r], cols[c], roles[r]);
}

void test_parse_legacy_tail()
{
    // Slice-3 named the rear rider "tail" ('T'); it must still resolve to sweep.
    expect_parse("RT", WP_RED, WP_SWEEP);
    expect_parse("BT", WP_BLUE, WP_SWEEP);
}

void test_color_role_helpers()
{
    TEST_ASSERT_EQUAL_CHAR('R', wp_colorChar(WP_RED));
    TEST_ASSERT_EQUAL_CHAR('O', wp_colorChar(WP_ORANGE));
    TEST_ASSERT_EQUAL_CHAR('V', wp_colorChar(WP_VIOLET));
    TEST_ASSERT_EQUAL_CHAR('S', wp_roleChar(WP_SWEEP));
    TEST_ASSERT_EQUAL_STRING("Orange", wp_colorName(WP_ORANGE));
    TEST_ASSERT_EQUAL_STRING("Sweep", wp_roleName(WP_SWEEP));
    // index round-trip through the rainbow order
    TEST_ASSERT_EQUAL_UINT8(WP_RED, wp_colorFromIndex(0));
    TEST_ASSERT_EQUAL_UINT8(WP_VIOLET, wp_colorFromIndex(5));
    TEST_ASSERT_EQUAL_UINT8(WP_COLOR_NONE, wp_colorFromIndex(9));
    TEST_ASSERT_EQUAL_UINT8(WP_SWEEP, wp_roleFromIndex(2));
}

void test_parse_case_insensitive()
{
    expect_parse("rl", WP_RED, WP_LEADER);
    expect_parse("gm", WP_GREEN, WP_MIDDLE);
    expect_parse("bs", WP_BLUE, WP_SWEEP);
    expect_parse("Yl", WP_YELLOW, WP_LEADER);
}

void test_parse_garbage_partial_null()
{
    expect_parse("XL", WP_COLOR_NONE, WP_LEADER); // bad color, good role
    expect_parse("RX", WP_RED, WP_ROLE_NONE);     // good color, bad role
    expect_parse("ZZ", WP_COLOR_NONE, WP_ROLE_NONE);
    expect_parse("R", WP_RED, WP_ROLE_NONE);  // single char -> role NONE
    expect_parse("G", WP_GREEN, WP_ROLE_NONE);
    expect_parse("", WP_COLOR_NONE, WP_ROLE_NONE);
    expect_parse(NULL, WP_COLOR_NONE, WP_ROLE_NONE);
    expect_parse("RLX", WP_RED, WP_LEADER); // trailing junk ignored
}

// --- wp_isSameTeam truth table ---

void test_isSameTeam_truth_table()
{
    TEST_ASSERT_TRUE(wp_isSameTeam(WP_RED, WP_RED));
    TEST_ASSERT_TRUE(wp_isSameTeam(WP_BLUE, WP_BLUE));
    TEST_ASSERT_TRUE(wp_isSameTeam(WP_GREEN, WP_GREEN));
    TEST_ASSERT_TRUE(wp_isSameTeam(WP_YELLOW, WP_YELLOW));

    TEST_ASSERT_FALSE(wp_isSameTeam(WP_RED, WP_BLUE));
    TEST_ASSERT_FALSE(wp_isSameTeam(WP_GREEN, WP_YELLOW));

    TEST_ASSERT_FALSE(wp_isSameTeam(WP_COLOR_NONE, WP_COLOR_NONE));
    TEST_ASSERT_FALSE(wp_isSameTeam(WP_RED, WP_COLOR_NONE));
    TEST_ASSERT_FALSE(wp_isSameTeam(WP_COLOR_NONE, WP_GREEN));
}

// --- geo math: distance / bearing / cardinal / two-nearest (slice 3) ---

void test_distance_known()
{
    // One degree of latitude ~ 111.19 km anywhere.
    TEST_ASSERT_FLOAT_WITHIN(300.0f, 111195.0f, wp_distanceMeters(0.0, 0.0, 1.0, 0.0));
    // Identical points -> zero.
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 0.0f, wp_distanceMeters(40.0, -75.0, 40.0, -75.0));
    // A bike-scale hop: 0.001 deg of longitude at lat 40 ~ 85 m.
    TEST_ASSERT_FLOAT_WITHIN(3.0f, 85.3f, wp_distanceMeters(40.0, -75.0, 40.0, -74.999));
}

void test_bearing_cardinals()
{
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 0.0f, wp_bearingDegrees(0.0, 0.0, 1.0, 0.0));    // due north
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 90.0f, wp_bearingDegrees(0.0, 0.0, 0.0, 1.0));   // due east
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 180.0f, wp_bearingDegrees(0.0, 0.0, -1.0, 0.0)); // due south
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 270.0f, wp_bearingDegrees(0.0, 0.0, 0.0, -1.0)); // due west
}

void test_cardinal8()
{
    TEST_ASSERT_EQUAL_STRING("N", wp_cardinal8(0.0f));
    TEST_ASSERT_EQUAL_STRING("N", wp_cardinal8(359.0f));
    TEST_ASSERT_EQUAL_STRING("NE", wp_cardinal8(45.0f));
    TEST_ASSERT_EQUAL_STRING("E", wp_cardinal8(90.0f));
    TEST_ASSERT_EQUAL_STRING("SE", wp_cardinal8(135.0f));
    TEST_ASSERT_EQUAL_STRING("S", wp_cardinal8(180.0f));
    TEST_ASSERT_EQUAL_STRING("SW", wp_cardinal8(225.0f));
    TEST_ASSERT_EQUAL_STRING("W", wp_cardinal8(270.0f));
    TEST_ASSERT_EQUAL_STRING("NW", wp_cardinal8(315.0f));
}

void test_two_nearest()
{
    uint8_t out[2];

    float d3[3] = {300.0f, 100.0f, 200.0f};
    TEST_ASSERT_EQUAL_UINT8(2, wp_twoNearest(d3, 3, out));
    TEST_ASSERT_EQUAL_UINT8(1, out[0]); // 100m is nearest
    TEST_ASSERT_EQUAL_UINT8(2, out[1]); // 200m is next

    float d1[1] = {42.0f};
    TEST_ASSERT_EQUAL_UINT8(1, wp_twoNearest(d1, 1, out));
    TEST_ASSERT_EQUAL_UINT8(0, out[0]);

    // n == 0 must be safe even with a null array.
    TEST_ASSERT_EQUAL_UINT8(0, wp_twoNearest(NULL, 0, out));

    float d2[2] = {5.0f, 9.0f};
    TEST_ASSERT_EQUAL_UINT8(2, wp_twoNearest(d2, 2, out));
    TEST_ASSERT_EQUAL_UINT8(0, out[0]);
    TEST_ASSERT_EQUAL_UINT8(1, out[1]);
}

// --- slice 8: ghost trail ---

// ~40N latitude. 1e-7 deg of latitude ≈ 1.11 cm; ~25 m ≈ 2247 units; ~25 m of
// longitude at this latitude ≈ 2933 units.
static const int32_t GT_LAT0 = 399500000;
static const int32_t GT_LON0 = -751600000;
static const int32_t GT_M25_LAT = 2247;
static const int32_t GT_M25_LON = 2933;

void test_ghost_append_spacing_dedupe()
{
    WolfpackGhostTrail t;
    wp_ghostReset(t);
    TEST_ASSERT_TRUE(wp_ghostAppend(t, GT_LAT0, GT_LON0));
    // ~5 m away: swallowed (heartbeats while parked must not spam the ring).
    TEST_ASSERT_FALSE(wp_ghostAppend(t, GT_LAT0 + 450, GT_LON0));
    TEST_ASSERT_EQUAL_UINT16(1, t.count);
    // ~25 m away: stored.
    TEST_ASSERT_TRUE(wp_ghostAppend(t, GT_LAT0 + GT_M25_LAT, GT_LON0));
    TEST_ASSERT_EQUAL_UINT16(2, t.count);
}

void test_ghost_query_departure_bearing()
{
    WolfpackGhostTrail t;
    wp_ghostReset(t);
    float deg = -1.0f;
    // Too little trail to answer.
    TEST_ASSERT_FALSE(wp_ghostQuery(t, GT_LAT0 * 1e-7, GT_LON0 * 1e-7, deg));
    // Due-north trail, crumbs every ~25 m.
    for (int i = 0; i < 10; i++)
        TEST_ASSERT_TRUE(wp_ghostAppend(t, GT_LAT0 + i * GT_M25_LAT, GT_LON0));
    // Standing on crumb 3: the lead departed due north from here.
    TEST_ASSERT_TRUE(wp_ghostQuery(t, (GT_LAT0 + 3 * GT_M25_LAT) * 1e-7, GT_LON0 * 1e-7, deg));
    TEST_ASSERT_TRUE(deg < 1.0f || deg > 359.0f);
    // ~100 m off the trail: no ghost.
    TEST_ASSERT_FALSE(wp_ghostQuery(t, (GT_LAT0 + 3 * GT_M25_LAT) * 1e-7, (GT_LON0 + 11700) * 1e-7, deg));
    // ~20 m ahead of the trail head: only the newest crumb is in range and it
    // has no successor yet — the live arrow owns that case, not the ghost.
    TEST_ASSERT_FALSE(wp_ghostQuery(t, (GT_LAT0 + 9 * GT_M25_LAT + 1800) * 1e-7, GT_LON0 * 1e-7, deg));
}

void test_ghost_switchback_nearest_leg_wins()
{
    WolfpackGhostTrail t;
    wp_ghostReset(t);
    float deg = -1.0f;
    // Lower leg runs east; upper leg (~50 m north) runs back west.
    for (int i = 0; i < 6; i++)
        TEST_ASSERT_TRUE(wp_ghostAppend(t, GT_LAT0, GT_LON0 + i * GT_M25_LON));
    TEST_ASSERT_TRUE(wp_ghostAppend(t, GT_LAT0 + 2 * GT_M25_LAT, GT_LON0 + 5 * GT_M25_LON));
    for (int i = 4; i >= 0; i--)
        TEST_ASSERT_TRUE(wp_ghostAppend(t, GT_LAT0 + 4 * GT_M25_LAT, GT_LON0 + i * GT_M25_LON));
    // On the upper leg the nearer (upper) crumbs win: ghost points west.
    TEST_ASSERT_TRUE(wp_ghostQuery(t, (GT_LAT0 + 4 * GT_M25_LAT) * 1e-7, (GT_LON0 + 3 * GT_M25_LON) * 1e-7, deg));
    TEST_ASSERT_TRUE(deg > 250.0f && deg < 290.0f);
    // On the lower leg: east.
    TEST_ASSERT_TRUE(wp_ghostQuery(t, GT_LAT0 * 1e-7, (GT_LON0 + 2 * GT_M25_LON) * 1e-7, deg));
    TEST_ASSERT_TRUE(deg > 70.0f && deg < 110.0f);
}

void test_ghost_ring_wrap()
{
    WolfpackGhostTrail t;
    wp_ghostReset(t);
    float deg = -1.0f;
    const int total = WP_GHOST_MAX + 50;
    for (int i = 0; i < total; i++)
        TEST_ASSERT_TRUE(wp_ghostAppend(t, GT_LAT0 + i * GT_M25_LAT, GT_LON0));
    TEST_ASSERT_EQUAL_UINT16(WP_GHOST_MAX, t.count);
    // The fresh end still answers...
    TEST_ASSERT_TRUE(wp_ghostQuery(t, (GT_LAT0 + (total - 5) * GT_M25_LAT) * 1e-7, GT_LON0 * 1e-7, deg));
    TEST_ASSERT_TRUE(deg < 1.0f || deg > 359.0f);
    // ...and the overwritten oldest crumbs are really gone.
    TEST_ASSERT_FALSE(wp_ghostQuery(t, GT_LAT0 * 1e-7, GT_LON0 * 1e-7, deg));
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_pack_unpack_roundtrip);
    RUN_TEST(test_unpack_zeroes_position_without_flag);
    RUN_TEST(test_unpack_accepts_legacy_v2);
    RUN_TEST(test_pack_rejects_short_buffer);
    RUN_TEST(test_unpack_rejects_short_buffer);
    RUN_TEST(test_unpack_rejects_bad_version);
    RUN_TEST(test_unpack_rejects_null);
    RUN_TEST(test_ride_epoch_roundtrip);
    RUN_TEST(test_unpack_accepts_legacy_v3_as_no_ride);
    RUN_TEST(test_unpack_v2_has_no_ride_epoch);
    RUN_TEST(test_next_ride_epoch_never_zero_never_repeats);
    RUN_TEST(test_next_ride_epoch_wraps_past_255);
    RUN_TEST(test_next_ride_epoch_varies_with_press_time);
    RUN_TEST(test_parse_full_grid);
    RUN_TEST(test_parse_legacy_tail);
    RUN_TEST(test_color_role_helpers);
    RUN_TEST(test_parse_case_insensitive);
    RUN_TEST(test_parse_garbage_partial_null);
    RUN_TEST(test_isSameTeam_truth_table);
    RUN_TEST(test_distance_known);
    RUN_TEST(test_bearing_cardinals);
    RUN_TEST(test_cardinal8);
    RUN_TEST(test_two_nearest);
    RUN_TEST(test_ghost_append_spacing_dedupe);
    RUN_TEST(test_ghost_query_departure_bearing);
    RUN_TEST(test_ghost_switchback_nearest_leg_wins);
    RUN_TEST(test_ghost_ring_wrap);
    exit(UNITY_END());
}

void loop() {}
