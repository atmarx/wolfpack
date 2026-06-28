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
#if HAS_SCREEN
#include "Observer.h"          // CallbackObserver
#include "input/InputBroker.h" // InputEvent, inputBroker
#endif

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

#if HAS_SCREEN
    // The Wolfpack HUD frame: always present on a Wolfpack node. Modules are
    // constructed before Screen builds its frameset, so returning true here is
    // enough to land us in the carousel — no regenerate dance needed.
    virtual bool wantUIFrame() override { return true; }
    // Two-up compass HUD: distance + bearing arrow to the two nearest teammates.
    virtual void drawFrame(OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y) override;

  public:
    // Input-broker callback: a click while our HUD frame is showing re-opens the
    // team picker. Returns 1 to consume the event, 0 to pass it through.
    int handleInputEvent(const InputEvent *event);
#endif

  private:
    void upsertPeer(NodeNum num, uint8_t color, uint8_t role);

#if HAS_SCREEN
    // --- Slice 4: on-device team picker (color + position) ---
    void launchTeamPicker(); // entry point: auto on first boot, or on a click
    void showColorPicker();
    void showPositionPicker(); // applies pendingColor
    void applyTeamSelection(WolfpackColor color, WolfpackRole role);
    // Lead is exclusive per color; Mid/Sweep are not (they collision-suffix).
    bool teamHasLeader(WolfpackColor color, NodeNum exclude) const;
    uint8_t countTeamRole(WolfpackColor color, WolfpackRole role, NodeNum exclude) const;

    CallbackObserver<WolfpackModule, const InputEvent *> inputObserver =
        CallbackObserver<WolfpackModule, const InputEvent *>(this, &WolfpackModule::handleInputEvent);
    WolfpackColor pendingColor = WP_COLOR_NONE; // color chosen, awaiting position
    uint32_t lastFrameDrawMs = 0;               // when our HUD frame last drew (focus proxy)
    uint32_t lastConflictBannerMs = 0;          // throttle the 2x-Lead warning
    bool inputObserved = false;                 // attached to inputBroker yet?
    bool autoPickerShown = false;               // auto-launch the picker once when unset
#endif

    WolfpackPeer peers[WP_MAX_PEERS] = {};
    uint8_t peerCount = 0;
};

extern WolfpackModule *wolfpackModule;
