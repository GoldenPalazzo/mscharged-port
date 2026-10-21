// The network numbers of the performance overlay (LAT, DLY, RB), read for the
// host after a game frame. Read only: the game is unchanged.
#if !defined(MSCHARGED_NATIVE) || !defined(MSCHARGED_GAME_MODULE)
#error The netplay snapshot belongs to the isolated original game module
#endif
#include "platform/netplay_snapshot.h"

#include "platform/rollback_state.h"

#include "Game/NetworkDebug.h"
#include "NL/plat/TransportConnection.h"

#include <algorithm>
#include <cmath>

extern "C" __attribute__((visibility("default"))) bool charged_netplay_snapshot(ChargedNetplaySnapshot* snapshot,
                                                                                unsigned size)
{
    if (!snapshot || size != sizeof(ChargedNetplaySnapshot)) return false;
    ChargedNetplaySnapshot s{};
    s.round_trip_ms = -1;
    NetworkSessionBase* session = g_pNetworkSessionBase;
    if (session && session->GetSessionMode() != NET_MODE_LOCAL)
    {
        s.network = 1;
        // The game's reliable transport pings each connection (LAN and
        // online alike): the slowest is what the match waits for.
        if (NetworkSocket* socket = session->GetDirectSocket())
            for (const TransportConnection* connection : socket->mReliableSocket.mConnections)
                if (connection && !const_cast<TransportConnection*>(connection)->IsClosed())
                    s.round_trip_ms = std::max(s.round_trip_ms, connection->mRoundTripTimeMS);
    }
    int delay = 0, predicted = 0;
    if (mscharged::rollback::OverlayNumbers(delay, predicted))
    {
        s.rollback = 1;
        s.input_delay_ms = static_cast<int>(std::lround(delay * g_fFixedUpdateTick * 1000.0f));
        s.predicted_steps = predicted;
    }
    *snapshot = s;
    return true;
}
