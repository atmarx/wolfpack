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
    WolfpackBeacon in = {WP_BEACON_VERSION, WP_GREEN, WP_LEADER, 0};
    uint8_t buf[8] = {0};

    size_t n = wp_packBeacon(in, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_size_t(WP_BEACON_SIZE, n);
    TEST_ASSERT_EQUAL_HEX8(WP_BEACON_VERSION, buf[0]);
    TEST_ASSERT_EQUAL_HEX8(WP_GREEN, buf[1]);
    TEST_ASSERT_EQUAL_HEX8(WP_LEADER, buf[2]);
    TEST_ASSERT_EQUAL_HEX8(0, buf[3]);

    WolfpackBeacon out;
    TEST_ASSERT_TRUE(wp_unpackBeacon(buf, n, out));
    TEST_ASSERT_EQUAL_UINT8(in.version, out.version);
    TEST_ASSERT_EQUAL_UINT8(in.color, out.color);
    TEST_ASSERT_EQUAL_UINT8(in.role, out.role);
    TEST_ASSERT_EQUAL_UINT8(in.flags, out.flags);
}

void test_pack_rejects_short_buffer()
{
    WolfpackBeacon in = {WP_BEACON_VERSION, WP_RED, WP_TAIL, 0};
    uint8_t buf[3] = {0};
    TEST_ASSERT_EQUAL_size_t(0, wp_packBeacon(in, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_size_t(0, wp_packBeacon(in, buf, 0));
}

void test_unpack_rejects_short_buffer()
{
    uint8_t buf[3] = {WP_BEACON_VERSION, WP_RED, WP_LEADER};
    WolfpackBeacon out;
    TEST_ASSERT_FALSE(wp_unpackBeacon(buf, sizeof(buf), out));
}

void test_unpack_rejects_bad_version()
{
    WolfpackBeacon out;
    uint8_t v2[4] = {2, WP_RED, WP_LEADER, 0};
    TEST_ASSERT_FALSE(wp_unpackBeacon(v2, sizeof(v2), out));
    uint8_t v0[4] = {0, WP_RED, WP_LEADER, 0};
    TEST_ASSERT_FALSE(wp_unpackBeacon(v0, sizeof(v0), out));
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
    expect_parse("RL", WP_RED, WP_LEADER);
    expect_parse("RM", WP_RED, WP_MIDDLE);
    expect_parse("RT", WP_RED, WP_TAIL);
    expect_parse("YL", WP_YELLOW, WP_LEADER);
    expect_parse("YM", WP_YELLOW, WP_MIDDLE);
    expect_parse("YT", WP_YELLOW, WP_TAIL);
    expect_parse("GL", WP_GREEN, WP_LEADER);
    expect_parse("GM", WP_GREEN, WP_MIDDLE);
    expect_parse("GT", WP_GREEN, WP_TAIL);
    expect_parse("BL", WP_BLUE, WP_LEADER);
    expect_parse("BM", WP_BLUE, WP_MIDDLE);
    expect_parse("BT", WP_BLUE, WP_TAIL);
}

void test_parse_case_insensitive()
{
    expect_parse("rl", WP_RED, WP_LEADER);
    expect_parse("gm", WP_GREEN, WP_MIDDLE);
    expect_parse("bT", WP_BLUE, WP_TAIL);
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

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_pack_unpack_roundtrip);
    RUN_TEST(test_pack_rejects_short_buffer);
    RUN_TEST(test_unpack_rejects_short_buffer);
    RUN_TEST(test_unpack_rejects_bad_version);
    RUN_TEST(test_unpack_rejects_null);
    RUN_TEST(test_parse_full_grid);
    RUN_TEST(test_parse_case_insensitive);
    RUN_TEST(test_parse_garbage_partial_null);
    RUN_TEST(test_isSameTeam_truth_table);
    RUN_TEST(test_distance_known);
    RUN_TEST(test_bearing_cardinals);
    RUN_TEST(test_cardinal8);
    RUN_TEST(test_two_nearest);
    exit(UNITY_END());
}

void loop() {}
