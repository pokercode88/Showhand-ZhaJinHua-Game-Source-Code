#include "ReconnectService.h"
#include "LobbyServer.h"

bool ReconnectService::OnConnected(void *lpcbParam, int iType)
{
    long lResult = -1;
    LobbyServer *lpLobbyServer = (LobbyServer*)lpcbParam;

    if (RECONNECT_TYPE_DISPATCH_SVR == iType)
    {
        lResult = lpLobbyServer->ConnectServer("dispatch_server", lpLobbyServer->m_lpDispatchServer);
		if (-1 != lResult) lpLobbyServer->SendRegLobbyServerReq();
    }

    return (0 == lResult);
}
