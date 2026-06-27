#include "WolfpackModule.h"
#include "MeshService.h"
#include "NodeDB.h"
#include "configuration.h"
#include "main.h"

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
