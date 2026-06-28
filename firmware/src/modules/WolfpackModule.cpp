#include "WolfpackModule.h"
#include "MeshService.h"
#include "NodeDB.h"
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

// One beacon per minute. Cheap, and slow enough not to perturb channel util.
static constexpr int32_t WP_BROADCAST_INTERVAL_MS = 60 * 1000;

// While a pick flow is in progress, tick fast so each banner opens promptly once
// the previous overlay clears (selection callbacks also wake us immediately).
static constexpr int32_t WP_PICK_TICK_MS = 250;

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
    // Workaround for an upstream L1 battery regression: ec5d230 ships the variant
    // with ADC_MULTIPLIER 2.0, which reads ~21% low on this board (3.93V cell ->
    // 3.10V shown). Set the runtime override once; it's re-read live (~5s) so it
    // applies without a reboot, persists, and we leave any user-set value alone.
    if (config.power.adc_multiplier_override <= 0.0f) {
        config.power.adc_multiplier_override = 2.54f;
        nodeDB->saveToDisk(SEGMENT_CONFIG);
        LOG_INFO("Wolfpack: set L1 adc_multiplier_override=2.54 (stock reads low)");
    }

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
        case WP_PICK_WANT_COLOR:
            if (!overlayUp) {
                LOG_INFO("Wolfpack: pick -> open color picker");
                showColorPicker();
                pickStep = WP_PICK_WAIT_COLOR;
            }
            break;
        case WP_PICK_WANT_POSITION:
            if (!overlayUp) {
                LOG_INFO("Wolfpack: pick -> open position picker (color=%d)", (int)pendingColor);
                showPositionPicker();
                pickStep = WP_PICK_WAIT_POSITION;
            }
            break;
        case WP_PICK_WANT_APPLY:
            if (!overlayUp) {
                LOG_INFO("Wolfpack: pick -> apply (color=%d role=%d)", (int)pendingColor, (int)pendingRole);
                pickStep = applyTeamSelection(pendingColor, pendingRole) ? WP_PICK_IDLE : WP_PICK_WANT_COLOR;
            }
            break;
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

    if (color == WP_COLOR_NONE || role == WP_ROLE_NONE) {
        LOG_INFO("Wolfpack: short_name '%s' has no color/role, skip beacon", owner.short_name);
        return WP_BROADCAST_INTERVAL_MS;
    }

    WolfpackBeacon beacon = {WP_BEACON_VERSION, (uint8_t)color, (uint8_t)role, 0};

    meshtastic_MeshPacket *p = allocDataPacket();
    p->decoded.payload.size = wp_packBeacon(beacon, p->decoded.payload.bytes, sizeof(p->decoded.payload.bytes));
    LOG_INFO("Wolfpack: beacon color=%u role=%u", (unsigned)color, (unsigned)role);
    service->sendToMesh(p);

    return WP_BROADCAST_INTERVAL_MS;
}

ProcessMessage WolfpackModule::handleReceived(const meshtastic_MeshPacket &mp)
{
    WolfpackBeacon beacon;
    if (wp_unpackBeacon(mp.decoded.payload.bytes, mp.decoded.payload.size, beacon)) {
        upsertPeer(mp.from, beacon.color, beacon.role);
        LOG_DEBUG("Wolfpack: peer 0x%x color=%u role=%u", mp.from, beacon.color, beacon.role);

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
                launchTeamPicker(); // defer: opens once the warning banner clears
            }
        }
#endif
    }
    return ProcessMessage::CONTINUE;
}

void WolfpackModule::upsertPeer(NodeNum num, uint8_t color, uint8_t role)
{
    uint32_t now = millis();

    // Linear scan — the table is tiny (<= WP_MAX_PEERS) and walked rarely.
    for (uint8_t i = 0; i < peerCount; i++) {
        if (peers[i].num == num) {
            peers[i].color = color;
            peers[i].role = role;
            peers[i].lastHeardMs = now;
            return;
        }
    }

    if (peerCount < WP_MAX_PEERS) {
        peers[peerCount].num = num;
        peers[peerCount].color = color;
        peers[peerCount].role = role;
        peers[peerCount].lastHeardMs = now;
        peerCount++;
        return;
    }

    // Table full: evict the stalest entry so a fresh teammate still lands.
    uint8_t oldest = 0;
    for (uint8_t i = 1; i < peerCount; i++) {
        if (peers[i].lastHeardMs < peers[oldest].lastHeardMs)
            oldest = i;
    }
    peers[oldest].num = num;
    peers[oldest].color = color;
    peers[oldest].role = role;
    peers[oldest].lastHeardMs = now;
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

// One teammate's cell: name on top, compass (arrow if we have a fresh heading,
// else "?") in the middle, distance (+ absolute heading-cardinal when stale) on
// the bottom. Drawn inside the column [colX, colX+colW).
static void wpDrawCell(OLEDDisplay *display, int16_t colX, int16_t colW, int16_t top, int16_t bottom, const char *name,
                       double myLat, double myLon, double peerLat, double peerLon, float distMeters, bool haveHeading,
                       float myHeadingRad)
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
    display->setTextAlignment(TEXT_ALIGN_CENTER);
    display->drawString(cx, nameY, name);
    display->drawCircle(cx, cyc, rad);

    if (haveHeading) {
        // Moving: rotating north marker + a relative arrow pointing at the peer.
        graphics::CompassRenderer::drawCompassNorth(display, cx, cyc, myHeadingRad, rad);
        float relRad = graphics::CompassRenderer::adjustBearingForCompassMode(absBearingDeg * (float)WP_DEG2RAD, myHeadingRad);
        graphics::CompassRenderer::drawNodeHeading(display, cx, cyc, (uint16_t)(rad * 2), relRad);
        display->setTextAlignment(TEXT_ALIGN_CENTER);
        display->drawString(cx, distY, dist);
    } else {
        // Stopped / no GPS course: a relative arrow would lie. Show "?" in the
        // rose and fall back to the honest absolute compass cardinal + distance.
        display->setTextAlignment(TEXT_ALIGN_CENTER);
        display->drawString(cx, (int16_t)(cyc - FONT_HEIGHT_SMALL / 2), "?");
        char line[20];
        snprintf(line, sizeof(line), "%s %s", wp_cardinal8(absBearingDeg), dist);
        display->drawString(cx, distY, line);
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
    meshtastic_PositionLite myPos;
    if (!me || !nodeDB->hasValidPosition(me) || !nodeDB->copyNodePosition(me->num, myPos)) {
        display->drawString(x + w / 2, (int16_t)(top + 4), "Waiting for GPS fix");
        display->setTextAlignment(TEXT_ALIGN_LEFT);
        return;
    }
    const double myLat = DegD(myPos.latitude_i);
    const double myLon = DegD(myPos.longitude_i);

    // One heading lookup for both cells. false => not moving / no course yet.
    float myHeadingRad = 0.0f;
    const bool haveHeading = graphics::CompassRenderer::getHeadingRadians(myLat, myLon, myHeadingRad);

    // Gather same-team teammates that currently have a known position.
    NodeNum cand[WP_MAX_PEERS];
    float candDist[WP_MAX_PEERS];
    uint8_t nCand = 0;
    const NodeNum myNum = nodeDB->getNodeNum();
    for (const WolfpackPeer *p = peersBegin(); p != peersEnd(); ++p) {
        if (p->num == myNum || !wp_isSameTeam((WolfpackColor)p->color, myColor))
            continue;
        const meshtastic_NodeInfoLite *pn = nodeDB->getMeshNode(p->num);
        meshtastic_PositionLite pp;
        if (!pn || !nodeDB->hasValidPosition(pn) || !nodeDB->copyNodePosition(p->num, pp))
            continue;
        candDist[nCand] = wp_distanceMeters(myLat, myLon, DegD(pp.latitude_i), DegD(pp.longitude_i));
        cand[nCand] = p->num;
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
        const NodeNum num = cand[pick[c]];
        const meshtastic_NodeInfoLite *pn = nodeDB->getMeshNode(num);
        meshtastic_PositionLite pp;
        nodeDB->copyNodePosition(num, pp); // re-fetch; validity already checked above
        const char *nm = (pn && pn->short_name[0]) ? pn->short_name : "?";
        wpDrawCell(display, (int16_t)(x + c * colW), colW, top, bottom, nm, myLat, myLon, DegD(pp.latitude_i),
                   DegD(pp.longitude_i), candDist[pick[c]], haveHeading, myHeadingRad);
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

void WolfpackModule::launchTeamPicker()
{
    if (!screen)
        return;
    // Don't open a banner straight from here — we may be inside a banner callback
    // or handleReceived(). Just arm the state machine; runOnce() opens the color
    // picker on the next tick, once any current overlay has cleared.
    LOG_INFO("Wolfpack: launchTeamPicker (was step=%d)", (int)pickStep);
    pickStep = WP_PICK_WANT_COLOR;
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
        LOG_INFO("Wolfpack: color callback idx=%d", idx);
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
        LOG_INFO("Wolfpack: position callback idx=%d", idx);
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
    if (pickStep != WP_PICK_IDLE) {
        LOG_INFO("Wolfpack: SELECT ignored, pick in progress (step=%d)", (int)pickStep);
        return 0;
    }
    // A banner/picker is already up — that SELECT belongs to it, not us.
    if (graphics::NotificationRenderer::isOverlayBannerShowing())
        return 0;
    // Only fire when our HUD frame is the one on screen (it drew very recently),
    // so we don't hijack carousel navigation on other frames.
    if (millis() - lastFrameDrawMs > 1500)
        return 0;
    LOG_INFO("Wolfpack: HUD SELECT -> launch picker");
    launchTeamPicker();
    return 1; // consumed
}

#endif // HAS_SCREEN
