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
#endif

WolfpackModule *wolfpackModule;

// One beacon per minute. Cheap, and slow enough not to perturb channel util.
static constexpr int32_t WP_BROADCAST_INTERVAL_MS = 60 * 1000;

WolfpackModule::WolfpackModule()
    : SinglePortModule("wolfpack", meshtastic_PortNum_PRIVATE_APP), concurrency::OSThread("Wolfpack")
{
    // Stagger the first broadcast like the other periodic modules do.
    setIntervalFromNow(setStartDelay());
}

int32_t WolfpackModule::runOnce()
{
    WolfpackColor color;
    WolfpackRole role;
    wp_parseColorRole(owner.short_name, color, role);

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
    display->clear();
    display->setFont(FONT_SMALL);

    WolfpackColor myColor;
    WolfpackRole myRole;
    wp_parseColorRole(owner.short_name, myColor, myRole);

    static const char *const COLW[] = {"--", "RED", "YEL", "GRN", "BLU"};
    static const char *const ROLEW[] = {"-", "Lead", "Mid", "Tail"};
    char title[20];
    if (myColor != WP_COLOR_NONE)
        snprintf(title, sizeof(title), "Wolf %s-%s", COLW[myColor], ROLEW[myRole]);
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
        display->drawString(x + w / 2, top + 2, "Set short-name");
        display->drawString(x + w / 2, (int16_t)(top + 2 + FONT_HEIGHT_SMALL), "color+role e.g. RL");
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

#endif // HAS_SCREEN
