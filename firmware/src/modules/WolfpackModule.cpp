#include "WolfpackModule.h"
#include "GPSStatus.h" // gpsStatus — the chip's own Doppler course (slice 8 heading)
#include "MeshService.h"
#include "NodeDB.h"
#include "PowerStatus.h" // powerStatus global (battery telemetry probe)
#include "airtime.h"     // airTime->isTxAllowedChannelUtil() send gates
#include "configuration.h"
#include "main.h"

#if HAS_SCREEN
#include "graphics/Screen.h"            // DegD(), graphics::Screen
#include "graphics/ScreenFonts.h"       // FONT_SMALL, FONT_HEIGHT_SMALL
#include "graphics/SharedUIDisplay.h"   // drawCommonHeader, SCREEN_WIDTH
#include "graphics/draw/CompassRenderer.h"
#include "graphics/draw/NotificationRenderer.h" // isOverlayBannerShowing()
#endif

WolfpackModule *wolfpackModule;

// Slice 8: the lead's recorded route. 8 KB of BSS, deliberately NOT a class
// member on the heap so the linker's RAM report keeps us honest about it.
static WolfpackGhostTrail wpGhostTrail;

// Beacon cadence (slice 5): evaluate every WP_TEAM_TICK_MS, send adaptively. A
// heartbeat once per WP_BEACON_IDLE_MS keeps peer liveness even when parked;
// moving past the resend threshold (wpMoveThresholdMeters) since the last *sent*
// fix re-sends early so followers track a moving pack — worst case one tick
// stale. Sends are airtime-gated (runOnce). The tick caps update freshness; on a
// busy channel the gates back sends off well before that. 3 s is the floor worth
// having: on MediumFast a 3-node pack moving continuously sits right at the
// polite 25% util ceiling (~250 ms/beacon), and on LongFast (~1.1 s/beacon) the
// gates dominate at any tick, so faster ticking buys nothing but gate-thrash.
static constexpr int32_t WP_TEAM_TICK_MS = 3 * 1000;
static constexpr uint32_t WP_BEACON_IDLE_MS = 60 * 1000;
static constexpr float WP_MOVE_RESEND_DEFAULT_M = 25.0f;

// A teammate's position is trusted for this long after their last position-fix
// beacon. A live peer refreshes at least every WP_BEACON_IDLE_MS (60 s heartbeat),
// so this must clear one fully-dropped LoRa heartbeat (~120 s) without flapping.
// Past it we no longer know where they are (out of range, or GPS lost mid-ride —
// their beacons keep coming, position-less, so posMs stops advancing) and the HUD
// falls back to an honest "?" instead of a stale pointer.
static constexpr uint32_t WP_POS_STALE_MS = 150 * 1000;

// While a pick flow is in progress, tick fast so each banner opens promptly once
// the previous overlay clears (selection callbacks also wake us immediately).
static constexpr int32_t WP_PICK_TICK_MS = 250;

// Meshtastic fixed-point (1e-7 degree) -> degrees.
static inline double wpDeg(int32_t i)
{
    return (double)i * 1e-7;
}

// How far (m) we must move before re-sending our position. Runtime-tunable with
// no reflash by reusing Meshtastic's own "smart position" distance knob:
//   meshtastic --set position.broadcast_smart_minimum_distance 5    # walking test
//   meshtastic --set position.broadcast_smart_minimum_distance 25   # riding (our default)
// The Meshtastic factory value (100 m) and 0 mean "unset" -> our 25 m default:
// above the ~3-5 m GPS noise floor, tight enough for a pack. ~5 m suits walking
// tests but is near GPS resolution, so expect the odd jitter-triggered send while
// standing still. Clamped to a sane ceiling.
static float wpMoveThresholdMeters()
{
    uint32_t d = config.position.broadcast_smart_minimum_distance;
    if (d == 0 || d == 100)
        return WP_MOVE_RESEND_DEFAULT_M;
    if (d > 500)
        d = 500;
    return (float)d;
}

WolfpackModule::WolfpackModule()
    : SinglePortModule("wolfpack", meshtastic_PortNum_PRIVATE_APP), concurrency::OSThread("Wolfpack")
{
    // Come alive quickly: the input observer attaches and the picker auto-launches
    // from the first runOnce, and setStartDelay() can be ~15s with all the stock
    // periodic modules registered ahead of us — far too long to wait for a click.
    setIntervalFromNow(1500);
}

int32_t WolfpackModule::runOnce()
{
    WolfpackColor color;
    WolfpackRole role;
    wp_parseColorRole(owner.short_name, color, role);

#if HAS_SCREEN
    // Attach to the input broker once it exists (click-to-pick re-trigger).
    if (!inputObserved && inputBroker) {
        inputObserver.observe(inputBroker);
        inputObserved = true;
    }
    // First boot with no team set: pop the picker so you can choose on-device.
    if (!autoPickerShown && screen && (color == WP_COLOR_NONE || role == WP_ROLE_NONE)) {
        autoPickerShown = true;
        launchTeamPicker();
    }

    // Deferred picker state machine. A banner's selection callback can't open the
    // next banner — NotificationRenderer calls resetBanner() right after the
    // callback (NotificationRenderer.cpp ~653), wiping anything it opened — so the
    // callbacks only record the choice + advance pickStep, and we open each banner
    // here on a later tick, once the prior overlay has cleared.
    if (pickStep != WP_PICK_IDLE) {
        const bool overlayUp = graphics::NotificationRenderer::isOverlayBannerShowing();
        switch (pickStep) {
        case WP_PICK_WANT_ACTION:
            if (!overlayUp) {
                showActionPicker();
                pickStep = WP_PICK_WAIT_ACTION;
            }
            break;
        case WP_PICK_WANT_ROLL:
            if (!overlayUp) {
                startRide();
                pickStep = WP_PICK_IDLE;
            }
            break;
        case WP_PICK_WANT_COLOR:
            if (!overlayUp) {
                showColorPicker();
                pickStep = WP_PICK_WAIT_COLOR;
            }
            break;
        case WP_PICK_WANT_POSITION:
            if (!overlayUp) {
                showPositionPicker();
                pickStep = WP_PICK_WAIT_POSITION;
            }
            break;
        case WP_PICK_WANT_APPLY:
            if (!overlayUp)
                pickStep = applyTeamSelection(pendingColor, pendingRole) ? WP_PICK_IDLE : WP_PICK_WANT_COLOR;
            break;
        case WP_PICK_WAIT_ACTION:
        case WP_PICK_WAIT_COLOR:
        case WP_PICK_WAIT_POSITION:
            // Banner closed without the callback advancing us => cancel / timeout.
            if (!overlayUp)
                pickStep = WP_PICK_IDLE;
            break;
        default:
            pickStep = WP_PICK_IDLE;
            break;
        }
        // Re-enter promptly: the next runOnce re-parses the (possibly just-set)
        // identity and beacons with current state, not the stale parse from the top
        // of THIS call. Also keeps the flow responsive between banners.
        return WP_PICK_TICK_MS;
    }

    // Warming up: if inputBroker or screen weren't ready at the first (1.5s) tick,
    // retry soon so the observer attaches and the picker auto-launches without a
    // full beacon-interval stall.
    if (!inputObserved || (!autoPickerShown && (color == WP_COLOR_NONE || role == WP_ROLE_NONE)))
        return 1000;
#endif

    // Battery telemetry probe (temporary). The L1 reads its cell through an I2C
    // fuel gauge, not the ADC (adc_multiplier_override is a no-op here), so this
    // reports the values the firmware actually acts on — to pin down the
    // 0%/USB-with-nothing-plugged-in symptom. Throttled to ~1/min, INFO level.
    if (powerStatus && (lastBattLogMs == 0 || millis() - lastBattLogMs >= 60000)) {
        lastBattLogMs = millis();
        LOG_INFO("Wolfpack batt: hasBattery=%d hasUSB=%d charging=%d mV=%d pct=%d",
                 (int)powerStatus->getHasBattery(), (int)powerStatus->getHasUSB(),
                 (int)powerStatus->getIsCharging(), powerStatus->getBatteryVoltageMv(),
                 (int)powerStatus->getBatteryChargePercent());
    }

    if (color == WP_COLOR_NONE || role == WP_ROLE_NONE) {
        LOG_INFO("Wolfpack: short_name '%s' has no color/role, skip beacon", owner.short_name);
        return (int32_t)WP_BEACON_IDLE_MS;
    }

    // --- Slice 5: position rides IN the beacon ------------------------------
    // Meshtastic truncates POSITION_APP packets to the channel position_precision
    // (default 13 bits = ~5.8 km cells) and rate-limits smart broadcasts to one
    // per 5 min — useless for a moving pack. PRIVATE_APP payloads are never
    // truncated, so we carry our own fix: full precision, straight from NodeDB's
    // self entry (updated locally by the GPS thread).
    bool havePos = false;
    int32_t latI = 0, lonI = 0;
    const meshtastic_NodeInfoLite *me = nodeDB->getMeshNode(nodeDB->getNodeNum());
    if (me && nodeDB->hasValidPosition(me)) {
        latI = me->position.latitude_i;
        lonI = me->position.longitude_i;
        havePos = true;
    }

    const uint32_t now = millis();
    // Heartbeat: at least one beacon per WP_BEACON_IDLE_MS for peer liveness,
    // fix or no fix.
    bool sendDue = (lastBeaconMs == 0) || (now - lastBeaconMs >= WP_BEACON_IDLE_MS);
    // Movement: re-send early once we've moved WP_MOVE_RESEND_M from the last
    // *sent* fix (or just got our first fix). This is extra fidelity, so it's
    // gated on the POLITE (25%) airtime ceiling — under pressure we shed these
    // first and keep heartbeats.
    if (!sendDue && havePos) {
        const bool moved = !haveSentPos || wp_distanceMeters(wpDeg(lastSentLat), wpDeg(lastSentLon), wpDeg(latI),
                                                             wpDeg(lonI)) >= wpMoveThresholdMeters();
        if (moved && airTime && airTime->isTxAllowedChannelUtil(true))
            sendDue = true;
    }
    // Hard (40%) ceiling applies to everything; a skipped heartbeat retries next
    // tick because lastBeaconMs doesn't advance.
    if (sendDue && airTime && !airTime->isTxAllowedChannelUtil(false))
        sendDue = false;

    if (sendDue) {
        WolfpackBeacon beacon = {WP_BEACON_VERSION,          (uint8_t)color, (uint8_t)role,
                                 (uint8_t)(havePos ? WP_FLAG_HAS_POSITION : 0), latI,           lonI,
                                 myRideEpoch};
        meshtastic_MeshPacket *p = allocDataPacket();
        p->decoded.payload.size = wp_packBeacon(beacon, p->decoded.payload.bytes, sizeof(p->decoded.payload.bytes));
        // Pack semantics, not mesh semantics: one relay tier is plenty (mid can
        // bridge lead<->sweep) and it cuts rebroadcast airtime vs the default 3.
        p->hop_limit = 1;
        LOG_INFO("Wolfpack: beacon color=%u role=%u pos=%d", (unsigned)color, (unsigned)role, (int)havePos);
        service->sendToMesh(p);
        lastBeaconMs = now;
        if (havePos) {
            lastSentLat = latI;
            lastSentLon = lonI;
            haveSentPos = true;
        }
    }

    // Tick fast enough to notice movement promptly; the gates above decide
    // whether anything actually hits the air.
    return WP_TEAM_TICK_MS;
}

ProcessMessage WolfpackModule::handleReceived(const meshtastic_MeshPacket &mp)
{
    WolfpackBeacon beacon;
    if (wp_unpackBeacon(mp.decoded.payload.bytes, mp.decoded.payload.size, beacon)) {
        upsertPeer(mp.from, beacon);
        LOG_DEBUG("Wolfpack: peer 0x%x color=%u role=%u pos=%d", mp.from, beacon.color, beacon.role,
                  (int)((beacon.flags & WP_FLAG_HAS_POSITION) != 0));

        // Slice 8: breadcrumb the lead's route. Every non-lead teammate records
        // the lead's position beacons (>= WP_GHOST_SPACING_M apart) so the HUD
        // can answer "when the lead was where I am now, which way did they go?"
        if (beacon.role == WP_LEADER && (beacon.flags & WP_FLAG_HAS_POSITION)) {
            WolfpackColor myColor;
            WolfpackRole myRole;
            wp_parseColorRole(owner.short_name, myColor, myRole);
            if (myRole != WP_LEADER && wp_isSameTeam((WolfpackColor)beacon.color, myColor)) {
                if (ghostLeadNum != mp.from) {
                    // New lead (first heard, or re-pick mid-ride): the old trail
                    // is another rider's history — drop it, don't splice it.
                    wp_ghostReset(wpGhostTrail);
                    ghostLeadNum = mp.from;
                }
                // Slice 9: the lead declared a new ride. Comparing against the
                // epoch we last acted on is what keeps this idempotent — every
                // beacon carries the epoch, so this fires exactly once per ride
                // no matter how many we hear or which ones we missed.
                if (beacon.rideEpoch != WP_RIDE_EPOCH_NONE && beacon.rideEpoch != heardRideEpoch) {
                    heardRideEpoch = beacon.rideEpoch;
                    wp_ghostReset(wpGhostTrail);
                    LOG_INFO("Wolfpack: ride epoch %u from lead 0x%x, trail cleared", (unsigned)beacon.rideEpoch,
                             mp.from);
#if HAS_SCREEN
                    if (screen) {
                        char msg[40];
                        snprintf(msg, sizeof(msg), "%s Team Is Rolling!", wp_colorName(myColor));
                        screen->showSimpleBanner(msg, 4000);
                    }
#endif
                }
                wp_ghostAppend(wpGhostTrail, beacon.lat_i, beacon.lon_i);
            }
        }

#if HAS_SCREEN
        // Lead-uniqueness backstop for the out-of-range case the picker can't
        // prevent: if a same-color Lead appears and outranks us (lower node-num
        // wins), warn and reopen the picker so we re-pick. Throttled.
        if (beacon.role == WP_LEADER && screen) {
            WolfpackColor myColor;
            WolfpackRole myRole;
            wp_parseColorRole(owner.short_name, myColor, myRole);
            if (myRole == WP_LEADER && myColor == (WolfpackColor)beacon.color && mp.from < nodeDB->getNodeNum() &&
                (millis() - lastConflictBannerMs > 15000)) {
                lastConflictBannerMs = millis();
                char msg[32];
                snprintf(msg, sizeof(msg), "2x %s LEAD", wp_colorName(myColor));
                screen->showSimpleBanner(msg, 5000);
                // forceRepick: this radio IS a set lead, so the default slice-9
                // flow would offer "Start Ride" — but the whole point here is
                // that two leads collided and one of them must re-pick.
                launchTeamPicker(true); // defer: opens once the warning banner clears
            }
        }
#endif
    }
    return ProcessMessage::CONTINUE;
}

void WolfpackModule::upsertPeer(NodeNum num, const WolfpackBeacon &beacon)
{
    const uint32_t now = millis();
    const bool hasPos = (beacon.flags & WP_FLAG_HAS_POSITION) != 0;

    WolfpackPeer *slot = nullptr;

    // Linear scan — the table is tiny (<= WP_MAX_PEERS) and walked rarely.
    for (uint8_t i = 0; i < peerCount; i++) {
        if (peers[i].num == num) {
            slot = &peers[i];
            break;
        }
    }

    if (!slot && peerCount < WP_MAX_PEERS)
        slot = &peers[peerCount++];

    if (!slot) {
        // Table full: evict the stalest entry so a fresh teammate still lands.
        slot = &peers[0];
        for (uint8_t i = 1; i < peerCount; i++) {
            if (peers[i].lastHeardMs < slot->lastHeardMs)
                slot = &peers[i];
        }
        slot->posMs = 0; // don't inherit the evicted node's position
    }

    slot->num = num;
    slot->color = beacon.color;
    slot->role = beacon.role;
    slot->lastHeardMs = now;
    if (hasPos) {
        slot->lat_i = beacon.lat_i;
        slot->lon_i = beacon.lon_i;
        slot->posMs = now;
    }
    // A beacon without a fix keeps the peer's previous position (if any) —
    // last-known beats nothing, and posMs still says how old it is.
}

bool WolfpackModule::getPeer(NodeNum num, WolfpackPeer &out) const
{
    for (uint8_t i = 0; i < peerCount; i++) {
        if (peers[i].num == num) {
            out = peers[i];
            return true;
        }
    }
    return false;
}

#if HAS_SCREEN

// Compact distance, honoring the user's metric/imperial display setting.
static void wpFormatDistance(float meters, char *buf, size_t buflen)
{
    if (config.display.units == meshtastic_Config_DisplayConfig_DisplayUnits_IMPERIAL) {
        float feet = meters * 3.28084f;
        if (feet < 1000.0f)
            snprintf(buf, buflen, "%.0fft", feet);
        else
            snprintf(buf, buflen, "%.1fmi", feet / 5280.0f);
    } else {
        if (meters < 1000.0f)
            snprintf(buf, buflen, "%.0fm", meters);
        else
            snprintf(buf, buflen, "%.1fkm", meters / 1000.0f);
    }
}

// Age of a peer's last position fix, rendered tiny: raw seconds under 100 (the
// range where it drives riding decisions), then minutes, then a flat "1h+" —
// past that the number is trivia, not information.
static void wpFormatAge(uint32_t secs, char *buf, size_t buflen)
{
    if (secs < 100)
        snprintf(buf, buflen, "%us", (unsigned)secs);
    else if (secs < 6000)
        snprintf(buf, buflen, "%um", (unsigned)(secs / 60));
    else
        snprintf(buf, buflen, "1h+");
}

// Our own heading, chip-first (slice 8). Upstream's estimatedHeading() only
// recomputes after 10 m of travel from a reference point — at walking speed
// that's a ~7 s old *average* direction, which is the "slow to notice I turned"
// lag from field testing. The L76K computes course-over-ground from Doppler on
// every fix (1 Hz, no displacement needed — same reason car dashboards feel
// instant). So: upstream keeps deciding the moving/stopped CLASSIFICATION
// (slice 6 semantics untouched), but while "moving" the heading VALUE comes
// from the chip when it has one. FREEZE_HEADING mode is respected — the user
// asked for a pinned compass, don't fight them.
static bool wpOwnHeadingRadians(double myLat, double myLon, float &out)
{
    if (!graphics::CompassRenderer::getHeadingRadians(myLat, myLon, out))
        return false;
    if (uiconfig.compass_mode != meshtastic_CompassMode_FREEZE_HEADING && gpsStatus && gpsStatus->getHasLock()) {
        const uint32_t track = gpsStatus->getHeading(); // chip course, degrees * 1e-5
        if (track < 36000000U)
            out = (float)((double)track * 1e-5 * WP_DEG2RAD);
    }
    return true;
}

// One teammate's cell: name on top, compass in the middle, distance on the
// bottom. Three states, gated on two INDEPENDENT questions — do we know where
// they are (posFresh), and can we make it body-relative (haveHeading)?
//   posFresh + moving  -> rotating rose + relative arrow  ("walk toward it")
//   posFresh + stopped -> absolute cardinal in the rose   ("they're NE, 200 m")
//   !posFresh          -> "?" + "~" last-known distance    ("lost their fix")
// The "?" means *unknown location*, never *I'm standing still* — a stopped rider
// still gets a true cardinal, because distance and bearing are the same two
// coordinates; only the rotation into a body arrow needs GPS course.
// ageSecs (slice 7) is how long ago this position arrived — drawn as a small
// counter in the cell's top-left, "resetting" to 0s whenever a beacon lands
// because it's just rendered age, not state. -1 hides it (no fix ever heard).
// ghostDir (slice 8, lead's cell only) is the cardinal the lead DEPARTED from
// where the viewer now stands — drawn top-right as ">NE" when the viewer is on
// the lead's recorded trail. NULL hides it.
// Drawn inside the column [colX, colX+colW).
static void wpDrawCell(OLEDDisplay *display, int16_t colX, int16_t colW, int16_t top, int16_t bottom, const char *name,
                       double myLat, double myLon, double peerLat, double peerLon, float distMeters, bool haveHeading,
                       float myHeadingRad, bool posFresh, int32_t ageSecs, const char *ghostDir)
{
    const int16_t cx = colX + colW / 2;
    const int16_t nameY = top;
    const int16_t distY = bottom - FONT_HEIGHT_SMALL;
    const int16_t cyc = (int16_t)((nameY + FONT_HEIGHT_SMALL + distY) / 2);
    int16_t rad = (int16_t)((distY - (nameY + FONT_HEIGHT_SMALL)) / 2 - 1);
    if (rad > colW / 2 - 3)
        rad = colW / 2 - 3;
    if (rad < 6)
        rad = 6;

    char dist[16];
    wpFormatDistance(distMeters, dist, sizeof(dist));
    const float absBearingDeg = wp_bearingDegrees(myLat, myLon, peerLat, peerLon);

    display->setFont(FONT_SMALL);
    if (ageSecs >= 0) {
        char age[8];
        wpFormatAge((uint32_t)ageSecs, age, sizeof(age));
        display->setTextAlignment(TEXT_ALIGN_LEFT);
        display->drawString((int16_t)(colX + 1), nameY, age);
    }
    if (ghostDir) {
        char g[6];
        snprintf(g, sizeof(g), ">%s", ghostDir);
        display->setTextAlignment(TEXT_ALIGN_RIGHT);
        display->drawString((int16_t)(colX + colW - 1), nameY, g);
    }
    display->setTextAlignment(TEXT_ALIGN_CENTER);
    display->drawString(cx, nameY, name);
    display->drawCircle(cx, cyc, rad);

    if (!posFresh) {
        // No recent fix from this teammate — their location is unknown or gone
        // stale (out of range, or their GPS dropped mid-ride). Any pointer would
        // be guessing, so show an honest "?" plus a "~" last-known distance.
        display->setTextAlignment(TEXT_ALIGN_CENTER);
        display->drawString(cx, (int16_t)(cyc - FONT_HEIGHT_SMALL / 2), "?");
        char line[20];
        snprintf(line, sizeof(line), "~%s", dist);
        display->drawString(cx, distY, line);
    } else if (haveHeading) {
        // Moving with a fresh fix: rotating north marker + a relative arrow.
        graphics::CompassRenderer::drawCompassNorth(display, cx, cyc, myHeadingRad, rad);
        float relRad = graphics::CompassRenderer::adjustBearingForCompassMode(absBearingDeg * (float)WP_DEG2RAD, myHeadingRad);
        graphics::CompassRenderer::drawNodeHeading(display, cx, cyc, (uint16_t)(rad * 2), relRad);
        display->setTextAlignment(TEXT_ALIGN_CENTER);
        display->drawString(cx, distY, dist);
    } else {
        // Fresh fix but standing still (no GPS course): we know exactly where they
        // are, just can't rotate it to a body arrow without a compass. Show the
        // absolute cardinal in the rose + plain distance — no misleading "?".
        display->setTextAlignment(TEXT_ALIGN_CENTER);
        display->drawString(cx, (int16_t)(cyc - FONT_HEIGHT_SMALL / 2), wp_cardinal8(absBearingDeg));
        display->drawString(cx, distY, dist);
    }
}

void WolfpackModule::drawFrame(OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y)
{
    (void)state;
    lastFrameDrawMs = millis(); // focus proxy for the click-to-pick re-trigger
    display->clear();
    display->setFont(FONT_SMALL);

    WolfpackColor myColor;
    WolfpackRole myRole;
    wp_parseColorRole(owner.short_name, myColor, myRole);

    char title[24];
    if (myColor != WP_COLOR_NONE)
        snprintf(title, sizeof(title), "%s %s", wp_colorName(myColor), wp_roleName(myRole));
    else
        snprintf(title, sizeof(title), "Wolfpack");
    graphics::drawCommonHeader(display, x, y, title);

    const int16_t w = display->getWidth();
    const int16_t h = display->getHeight();
    const int16_t top = (int16_t)(y + FONT_HEIGHT_SMALL);
    const int16_t bottom = (int16_t)(y + h);

    display->setTextAlignment(TEXT_ALIGN_CENTER);

    // No team identity yet — guide setup instead of drawing meaningless arrows.
    if (myColor == WP_COLOR_NONE) {
        display->drawString(x + w / 2, top + 2, "No team set");
        display->drawString(x + w / 2, (int16_t)(top + 2 + FONT_HEIGHT_SMALL), "Click to pick");
        display->setTextAlignment(TEXT_ALIGN_LEFT);
        return;
    }

    // Our own fix — needed before any distance/bearing means anything.
    const meshtastic_NodeInfoLite *me = nodeDB->getMeshNode(nodeDB->getNodeNum());
    if (!me || !nodeDB->hasValidPosition(me)) {
        display->drawString(x + w / 2, (int16_t)(top + 4), "Waiting for GPS fix");
        display->setTextAlignment(TEXT_ALIGN_LEFT);
        return;
    }
    const double myLat = wpDeg(me->position.latitude_i);
    const double myLon = wpDeg(me->position.longitude_i);

    // One heading lookup for both cells. false => not moving / no course yet.
    // Chip-course first (slice 8) — see wpOwnHeadingRadians.
    float myHeadingRad = 0.0f;
    const bool haveHeading = wpOwnHeadingRadians(myLat, myLon, myHeadingRad);
    const uint32_t now = millis();

    // Gather same-team teammates that currently have a known position. Beacon
    // positions (v3: full precision, seconds fresh) win; NodeDB is the fallback
    // for v2 peers — but NodeDB positions are channel-truncated (default 13 bits
    // = ~5.8 km cells) and minutes stale, so they're a last resort.
    const WolfpackPeer *cand[WP_MAX_PEERS];
    float candDist[WP_MAX_PEERS];
    int32_t candLatI[WP_MAX_PEERS];
    int32_t candLonI[WP_MAX_PEERS];
    uint8_t nCand = 0;
    const NodeNum myNum = nodeDB->getNodeNum();
    for (const WolfpackPeer *p = peersBegin(); p != peersEnd(); ++p) {
        if (p->num == myNum || !wp_isSameTeam((WolfpackColor)p->color, myColor))
            continue;
        int32_t plat = 0, plon = 0;
        bool have = false;
        if (p->posMs != 0) { // beacon-carried position (never truncated)
            plat = p->lat_i;
            plon = p->lon_i;
            have = true;
        } else { // v2 peer: NodeDB position or nothing
            const meshtastic_NodeInfoLite *pn = nodeDB->getMeshNode(p->num);
            if (pn && nodeDB->hasValidPosition(pn)) {
                plat = pn->position.latitude_i;
                plon = pn->position.longitude_i;
                have = true;
            }
        }
        if (!have)
            continue;
        cand[nCand] = p;
        candLatI[nCand] = plat;
        candLonI[nCand] = plon;
        candDist[nCand] = wp_distanceMeters(myLat, myLon, wpDeg(plat), wpDeg(plon));
        if (++nCand >= WP_MAX_PEERS)
            break;
    }

    if (nCand == 0) {
        display->drawString(x + w / 2, (int16_t)(top + 4), "No teammates yet");
        display->setTextAlignment(TEXT_ALIGN_LEFT);
        return;
    }

    uint8_t pick[2];
    const uint8_t nShow = wp_twoNearest(candDist, nCand, pick);
    const int16_t colW = (int16_t)(w / 2);

    for (uint8_t c = 0; c < nShow; c++) {
        const WolfpackPeer *peer = cand[pick[c]];
        const meshtastic_NodeInfoLite *pn = nodeDB->getMeshNode(peer->num);
        // NodeDB may not know this node yet (beacons flow before NodeInfo does);
        // synthesize the team code from the beacon so the cell is never nameless.
        char synth[3] = {wp_colorChar((WolfpackColor)peer->color), wp_roleChar((WolfpackRole)peer->role), '\0'};
        const char *nm = (pn && pn->has_user && pn->user.short_name[0]) ? pn->user.short_name : synth;
        // Freshness gates the "?": trust a beacon position (posMs) first; fall back
        // to last-heard for v2 peers that never carried one. A peer whose GPS died
        // keeps a non-zero (frozen) posMs, so it ages out here and reads "?".
        const uint32_t fMs = peer->posMs ? peer->posMs : peer->lastHeardMs;
        const bool posFresh = (fMs != 0) && (now - fMs < WP_POS_STALE_MS);
        const int32_t ageSecs = fMs ? (int32_t)((now - fMs) / 1000) : -1;
        // Slice 8: the ghost — when this cell is the lead whose trail we hold and
        // the viewer is standing on that trail, show the lead's departure cardinal.
        const char *ghostDir = NULL;
        if ((WolfpackRole)peer->role == WP_LEADER && peer->num == ghostLeadNum) {
            float gDeg;
            if (wp_ghostQuery(wpGhostTrail, myLat, myLon, gDeg))
                ghostDir = wp_cardinal8(gDeg);
        }
        wpDrawCell(display, (int16_t)(x + c * colW), colW, top, bottom, nm, myLat, myLon, wpDeg(candLatI[pick[c]]),
                   wpDeg(candLonI[pick[c]]), candDist[pick[c]], haveHeading, myHeadingRad, posFresh, ageSecs, ghostDir);
    }

    // Vertical divider between the two cells.
    if (nShow == 2)
        display->drawLine((int16_t)(x + colW), top, (int16_t)(x + colW), (int16_t)(bottom - 1));

    // Honest "there are more than two" marker — no silent cap (future 4th node).
    if (nCand > nShow) {
        char more[6];
        snprintf(more, sizeof(more), "+%u", (unsigned)(nCand - nShow));
        display->setTextAlignment(TEXT_ALIGN_RIGHT);
        display->drawString((int16_t)(x + w - 1), top, more);
    }

    display->setTextAlignment(TEXT_ALIGN_LEFT);
}

// --- Slice 4: on-device team picker ----------------------------------------

void WolfpackModule::launchTeamPicker(bool forceRepick)
{
    if (!screen)
        return;
    // Don't open a banner straight from here — we may be inside a banner callback
    // or handleReceived(). Just arm the state machine; runOnce() opens the first
    // banner on the next tick, once any current overlay has cleared.
    //
    // Slice 9: a coach riding lead has one thing they want from a click mid-ride
    // ("we're rolling") and one thing they want roughly never ("change my team"),
    // so a lead gets an action menu first. Everyone else drops straight into the
    // color picker exactly as before — no new gesture, no new input plumbing.
    WolfpackColor c;
    WolfpackRole r;
    wp_parseColorRole(owner.short_name, c, r);
    const bool amSetLead = (r == WP_LEADER && c != WP_COLOR_NONE);
    pickStep = (amSetLead && !forceRepick) ? WP_PICK_WANT_ACTION : WP_PICK_WANT_COLOR;
    setIntervalFromNow(0);
}

// Lead-only first stop. Selecting "Start Ride" cannot call startRide() from the
// callback — NotificationRenderer wipes any banner opened from inside one — so
// it records WANT_ROLL and runOnce() fires it on a later tick.
void WolfpackModule::showActionPicker()
{
    if (!screen)
        return;
    static const char *opts[] = {"Start Ride", "Change Team", "Cancel"};

    graphics::BannerOverlayOptions o;
    o.message = "Lead";
    o.optionsArrayPtr = opts;
    o.optionsCount = 3;
    o.durationMs = 30000;
    o.InitialSelected = 0;
    o.bannerCallback = [this](int idx) {
        if (idx == 0)
            pickStep = WP_PICK_WANT_ROLL;
        else if (idx == 1)
            pickStep = WP_PICK_WANT_COLOR;
        else
            pickStep = WP_PICK_IDLE;
    };
    screen->showOverlayBanner(o);
}

// Declare a new ride: stamp a fresh epoch, drop any trail this radio was
// holding, and announce it. The epoch propagates in every subsequent beacon, so
// there is nothing to retransmit and nothing to acknowledge.
void WolfpackModule::startRide()
{
    WolfpackColor c;
    WolfpackRole r;
    wp_parseColorRole(owner.short_name, c, r);

    myRideEpoch = wp_nextRideEpoch(millis(), myRideEpoch);

    // A lead records no trail of its own, but this radio may have been a
    // follower earlier today — start the ride genuinely empty either way.
    wp_ghostReset(wpGhostTrail);
    ghostLeadNum = 0;
    heardRideEpoch = myRideEpoch;

    if (screen) {
        char msg[40];
        snprintf(msg, sizeof(msg), "%s Team Is Rolling!", wp_colorName(c));
        screen->showSimpleBanner(msg, 4000);
    }
    LOG_INFO("Wolfpack: ride start, color=%u epoch=%u", (unsigned)c, (unsigned)myRideEpoch);

    // Put the new epoch on the air now rather than waiting out the heartbeat —
    // the coaches are looking at their screens at exactly this moment.
    lastBeaconMs = 0;
    setIntervalFromNow(0);
}

void WolfpackModule::showColorPicker()
{
    if (!screen)
        return;
    // Rainbow order; last entry is an explicit Cancel for click-only devices.
    static const char *opts[] = {"Red", "Orange", "Yellow", "Green", "Blue", "Violet", "Cancel"};

    WolfpackColor cur;
    WolfpackRole curRole;
    wp_parseColorRole(owner.short_name, cur, curRole);

    graphics::BannerOverlayOptions o;
    o.message = "Team color";
    o.optionsArrayPtr = opts;
    o.optionsCount = WP_NUM_COLORS + 1; // colors + Cancel
    o.durationMs = 30000;
    o.InitialSelected = (cur != WP_COLOR_NONE) ? (int8_t)(cur - WP_RED) : 0;
    o.bannerCallback = [this](int idx) {
        if (idx < 0 || idx >= (int)WP_NUM_COLORS) { // Cancel / dismissed
            this->pickStep = WP_PICK_IDLE;
            return;
        }
        // Only record + advance; runOnce() opens the position picker next tick.
        this->pendingColor = wp_colorFromIndex(idx);
        this->pickStep = WP_PICK_WANT_POSITION;
        this->setIntervalFromNow(0);
    };
    screen->showOverlayBanner(o);
}

void WolfpackModule::showPositionPicker()
{
    if (!screen)
        return;
    static const char *opts[] = {"Lead", "Mid", "Sweep", "Cancel"};

    graphics::BannerOverlayOptions o;
    o.message = "Position";
    o.optionsArrayPtr = opts;
    o.optionsCount = WP_NUM_ROLES + 1; // roles + Cancel
    o.durationMs = 30000;
    o.InitialSelected = 0;
    o.bannerCallback = [this](int idx) {
        if (idx < 0 || idx >= (int)WP_NUM_ROLES) { // Cancel / dismissed
            this->pickStep = WP_PICK_IDLE;
            return;
        }
        // Only record + advance; runOnce() applies it next tick (pendingColor was
        // set by the color picker).
        this->pendingRole = wp_roleFromIndex(idx);
        this->pickStep = WP_PICK_WANT_APPLY;
        this->setIntervalFromNow(0);
    };
    screen->showOverlayBanner(o);
}

bool WolfpackModule::applyTeamSelection(WolfpackColor color, WolfpackRole role)
{
    if (color == WP_COLOR_NONE || role == WP_ROLE_NONE || !screen)
        return true; // nothing to apply; don't reopen the picker

    const NodeNum me = nodeDB->getNodeNum();

    // Lead is exclusive per color: if one is already on the air, refuse + reopen.
    if (role == WP_LEADER && teamHasLeader(color, me)) {
        char msg[40];
        snprintf(msg, sizeof(msg), "%s already has a Lead", wp_colorName(color));
        screen->showSimpleBanner(msg, 4000);
        return false; // tell runOnce() to reopen the color picker
    }

    // Short-name = color char + role char + (Mid/Sweep) sequential collision suffix.
    char code[5];
    const char cc = wp_colorChar(color);
    const char rc = wp_roleChar(role);
    if (role == WP_LEADER) {
        snprintf(code, sizeof(code), "%c%c", cc, rc);
    } else {
        const uint8_t existing = countTeamRole(color, role, me);
        if (existing == 0)
            snprintf(code, sizeof(code), "%c%c", cc, rc);
        else
            snprintf(code, sizeof(code), "%c%c%u", cc, rc, (unsigned)(existing + 1)); // RM -> RM2 -> RM3
    }

    strncpy(owner.short_name, code, sizeof(owner.short_name));
    owner.short_name[sizeof(owner.short_name) - 1] = '\0';
    snprintf(owner.long_name, sizeof(owner.long_name), "%s %s", wp_colorName(color), wp_roleName(role));
    nodeDB->saveToDisk(SEGMENT_DEVICESTATE); // owner is a reference to devicestate.owner

    char done[24];
    snprintf(done, sizeof(done), "You are %s", code);
    screen->showSimpleBanner(done, 3000);
    LOG_INFO("Wolfpack: team set to %s (%s %s)", code, wp_colorName(color), wp_roleName(role));

    setIntervalFromNow(0); // beacon the new identity right away
    return true;
}

bool WolfpackModule::teamHasLeader(WolfpackColor color, NodeNum exclude) const
{
    for (const WolfpackPeer *p = peersBegin(); p != peersEnd(); ++p) {
        if (p->num != exclude && (WolfpackColor)p->color == color && (WolfpackRole)p->role == WP_LEADER)
            return true;
    }
    return false;
}

uint8_t WolfpackModule::countTeamRole(WolfpackColor color, WolfpackRole role, NodeNum exclude) const
{
    uint8_t n = 0;
    for (const WolfpackPeer *p = peersBegin(); p != peersEnd(); ++p) {
        if (p->num != exclude && (WolfpackColor)p->color == color && (WolfpackRole)p->role == role)
            n++;
    }
    return n;
}

int WolfpackModule::handleInputEvent(const InputEvent *event)
{
    if (!event || event->inputEvent != INPUT_BROKER_SELECT)
        return 0;
    // A pick flow is already running: the banner owns all input until it resolves.
    // Never relaunch from a click here — that clobbers pickStep back to the color
    // step and makes the picker "reprompt" (the real cause of the cycle).
    if (pickStep != WP_PICK_IDLE)
        return 0;
    // A banner/picker is already up — that SELECT belongs to it, not us.
    if (graphics::NotificationRenderer::isOverlayBannerShowing())
        return 0;
    // Only fire when our HUD frame is the one on screen (it drew very recently),
    // so we don't hijack carousel navigation on other frames.
    if (millis() - lastFrameDrawMs > 1500)
        return 0;
    launchTeamPicker();
    return 1; // consumed
}

#endif // HAS_SCREEN
