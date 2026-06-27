#pragma once
//
// WolfpackModule — periodically broadcasts this node's Wolfpack color/role and
// keeps a small table of teammates heard on the mesh. All wire/parse logic lives
// in WolfpackProtocol.h (pure, unit-tested on the host); this file is the thin
// Meshtastic-facing shell.
//
#include "SinglePortModule.h"
#include "WolfpackProtocol.h"
#include "concurrency/OSThread.h"
#include "mesh/MeshTypes.h" // NodeNum

// Snapshot of a teammate we've heard. Intentionally a fixed-size POD table — no
// STL, no heap — so the screen frame (next slice) can iterate it cheaply.
struct WolfpackPeer {
    NodeNum num;         // node number of the sender (mp.from)
    uint8_t color;       // WolfpackColor
    uint8_t role;        // WolfpackRole
    uint32_t lastHeardMs; // millis() of last beacon
};

#define WP_MAX_PEERS 32

class WolfpackModule : public SinglePortModule, private concurrency::OSThread
{
  public:
    WolfpackModule();

    // --- Accessors for the future "arrow to leader" screen frame ---
    // Copy out the peer with the given node number; false if not tracked.
    bool getPeer(NodeNum num, WolfpackPeer &out) const;
    // Half-open range over the live peer table.
    const WolfpackPeer *peersBegin() const { return peers; }
    const WolfpackPeer *peersEnd() const { return peers + peerCount; }
    uint8_t peerCountValue() const { return peerCount; }

  protected:
    // Periodic broadcast of our own beacon. Returns the next interval (ms).
    int32_t runOnce() override;

    // Ingest a teammate's beacon into the peer table.
    ProcessMessage handleReceived(const meshtastic_MeshPacket &mp) override;

  private:
    void upsertPeer(NodeNum num, uint8_t color, uint8_t role);

    WolfpackPeer peers[WP_MAX_PEERS] = {};
    uint8_t peerCount = 0;
};

extern WolfpackModule *wolfpackModule;
