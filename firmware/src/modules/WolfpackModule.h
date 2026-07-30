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
// STL, no heap — so the screen frame can iterate it cheaply.
struct WolfpackPeer {
    NodeNum num;          // node number of the sender (mp.from)
    uint8_t color;        // WolfpackColor
    uint8_t role;         // WolfpackRole
    uint32_t lastHeardMs; // millis() of last beacon
    // Full-precision position carried in a v3 beacon (never channel-truncated,
    // unlike NodeDB positions). posMs == 0 => never received one (v2 peer / no
    // fix yet) and the HUD falls back to NodeDB.
    int32_t lat_i;  // latitude  * 1e7
    int32_t lon_i;  // longitude * 1e7
    uint32_t posMs; // millis() when the beacon position was received
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
    void upsertPeer(NodeNum num, const WolfpackBeacon &beacon);

    // --- Slice 5: position-in-beacon cadence state ---
    uint32_t lastBeaconMs = 0;  // millis() of the last beacon actually sent
    int32_t lastSentLat = 0;    // position carried in that beacon...
    int32_t lastSentLon = 0;
    bool haveSentPos = false;   // ...and whether it carried one at all
    uint32_t lastBattLogMs = 0; // throttle for the battery telemetry probe

#if HAS_SCREEN
    // --- Slice 4: on-device team picker (color + position) ---
    void launchTeamPicker(); // entry point: auto on first boot, or on a click
    void showColorPicker();
    void showPositionPicker(); // uses pendingColor
    // Apply the chosen identity. Returns false if it was refused (same-color Lead
    // already exists) so runOnce() reopens the picker; true otherwise.
    bool applyTeamSelection(WolfpackColor color, WolfpackRole role);
    // Lead is exclusive per color; Mid/Sweep are not (they collision-suffix).
    bool teamHasLeader(WolfpackColor color, NodeNum exclude) const;
    uint8_t countTeamRole(WolfpackColor color, WolfpackRole role, NodeNum exclude) const;

    CallbackObserver<WolfpackModule, const InputEvent *> inputObserver =
        CallbackObserver<WolfpackModule, const InputEvent *>(this, &WolfpackModule::handleInputEvent);
    // The picker is a deferred state machine driven from runOnce(): a banner's
    // selection callback can't open the next banner (NotificationRenderer calls
    // resetBanner() right after the callback returns, wiping anything it opened),
    // so callbacks only record the choice + advance this; runOnce() opens the next
    // banner once the current overlay clears. WANT_* = "open next"; WAIT_* = "up".
    enum PickStep : uint8_t {
        WP_PICK_IDLE = 0,
        WP_PICK_WANT_COLOR,
        WP_PICK_WAIT_COLOR,
        WP_PICK_WANT_POSITION,
        WP_PICK_WAIT_POSITION,
        WP_PICK_WANT_APPLY,
    };

    WolfpackColor pendingColor = WP_COLOR_NONE; // color chosen, awaiting position
    WolfpackRole pendingRole = WP_ROLE_NONE;    // position chosen, awaiting apply
    PickStep pickStep = WP_PICK_IDLE;           // deferred picker flow state
    uint32_t lastFrameDrawMs = 0;               // when our HUD frame last drew (focus proxy)
    uint32_t lastConflictBannerMs = 0;          // throttle the 2x-Lead warning
    bool inputObserved = false;                 // attached to inputBroker yet?
    bool autoPickerShown = false;               // auto-launch the picker once when unset
#endif

    WolfpackPeer peers[WP_MAX_PEERS] = {};
    uint8_t peerCount = 0;

    // --- Slice 8: whose ghost trail we're recording (0 = no lead heard yet).
    // The trail itself is a file-scope static in the .cpp (8 KB — keep it in BSS
    // where the linker's RAM accounting can see it, not on the heap).
    NodeNum ghostLeadNum = 0;
};

extern WolfpackModule *wolfpackModule;
