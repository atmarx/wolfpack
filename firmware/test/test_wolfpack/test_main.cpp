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
    exit(UNITY_END());
}

void loop() {}
