#include "LobbyServer.h"
#include "LobbyPlayer.h"
#include "Odao_LobbySvr_Msg.h"
#include "Odao_DispatchSvr_Msg.h"
#include "ServerDef.h"
using namespace SGLib;

CIniFile g_cfg;

LobbyServer::LobbyServer()
{
    m_lpLobbyServer = NULL;
    m_bSendRegSvrReq = false;
    m_dbWorker = NULL;
 
    m_bIsOpenDB = false;

    g_cfg.InitFile(CONFIG_FILE);

    int iInterval = g_cfg.GetValueInt("network", "interval", 20);
    int iTimeout = g_cfg.GetValueInt("network", "timeout", 120);
    int iMaxSendCount = g_cfg.GetValueInt("network", "MAX_SEND_COUNT", 0);
    int iMaxRecvCount = g_cfg.GetValueInt("network", "MAX_RECV_COUNT", 0);

	m_nServerID = g_cfg.GetValueInt("init", "server_id", 0);

	char szIP[16]={0};
	g_cfg.GetValueStr("init", "server_ip", szIP, 16);

	m_ulServerIP = inet_addr(szIP);
	m_usServerPort = 0;

    printf( "GameProvider::Initialize begin\n" );

    if (true == GameProvider::Initialize(iMaxRecvCount, iMaxSendCount, iInterval, iTimeout))
    {
        __log(_DEBUG, "LobbyServer::LobbyServer", "interval[%d], timeout[%d]", iInterval, iTimeout);
    }
    else
    {
        __log(_ERROR, "LobbyServer::LobbyServer", "GameProvider::Initialize() error !");
    }
}

LobbyServer::~LobbyServer()
{

}

long LobbyServer::Start()
{
	long lResult = -1;
	char szBuffer[32];

    m_dbWorker = new CDBWorker( *this );
    if( !m_dbWorker )
    {
		__log(_ERROR, "LobbyServer::Start","new m_dbWorker failed!");
		return lResult;
    }
    if( !m_dbWorker->Start() )
    {
		__log(_ERROR, "LobbyServer::Start","m_dbWorker start failed!");
		return lResult;
    }

    m_db = new DBOperation();
    if( !m_db )
    {
		__log(_ERROR, "LobbyServer::Start","new m_db failed!");
		return lResult;
    }
	if (!m_db->Open())
	{
		__log(_ERROR, "LobbyServer::Start","m_db.Open failed!");
		return lResult;
	}

    //连接分发服务器
	if (-1 == ConnectServer("dispatch_server", m_lpDispatchServer))
	{
		__log(_ERROR, "LobbyServer::Start:","DispatchServer connect failed!");
		return lResult;
	}
	
	if (0 != g_cfg.GetValueStr("listen", "addr", szBuffer, sizeof(szBuffer)))
	{
		int iMaxConn = g_cfg.GetValueInt("listen", "maxconnection", 10000);

		char szAddr[32] = {0};
		int iPort = 0;
		sscanf(szBuffer, "%[^:]:%d", szAddr, &iPort);

		m_lpLobbyServer = GameProvider::Create(true, szAddr, iPort, 0, iMaxConn);

		if (NULL != m_lpLobbyServer)
		{
			if (true == GameProvider::RecvData(m_lpLobbyServer))
			{
				__log(_DEBUG, "LobbyServer::Start", "listen and start service success, host[%s], port[%d], node = %08x !", szAddr, iPort, m_lpLobbyServer);

				m_Reconnect.Start();

				m_usServerPort = iPort;
				SendRegLobbyServerReq();

				m_bIsOpenDB = true;

				lResult = 0;
			}
			else
			{
				__log(_ERROR, "LobbyServer::Start", "RecvData() error !, node = %08x", m_lpLobbyServer);

				GameProvider::Close(m_lpLobbyServer);
			}
		}
		else
		{
			__log(_ERROR, "LobbyServer::Start", "GameProvider::Create() error, host[%s], port[%d], errno[%d], info[%s] !", szAddr, iPort, errno, strerror(errno));
		}
	}
	else
	{
		__log(_ERROR, "LobbyServer::Start", "g_cfg.GetValueStr() 'addr' error !");
	}

    return lResult;
}

void LobbyServer::OnNetMessage(IPlayerNode *lpPlayerNode, unsigned short type, void *buffer, long length)
{
	if (lpPlayerNode == m_lpDispatchServer)
	{
		HandleDispatchServerNetMsg(lpPlayerNode, type, buffer, length);
	} 
	else
	{
		HandleClientNetMsg(lpPlayerNode, type, buffer, length);
	}
}

void LobbyServer::OnClosed(IPlayerNode *lpPlayerNode, bool bClosed)
{
	if (lpPlayerNode == m_lpDispatchServer)
	{
		__log(_ERROR, "LobbyServer::OnClosed", "dispatch server has been closed !, bClosed[%d]", bClosed);
		m_lpDispatchServer = NULL;
		m_Reconnect.SetParameter(this, RECONNECT_TYPE_DISPATCH_SVR);
		m_bSendRegSvrReq = false;
	}
	else if (lpPlayerNode == m_lpLobbyServer)
	{
		__log(_ERROR, "LobbyServer::OnClosed", "Lobby server has been closed !, bClosed[%d]", bClosed);
		m_lpLobbyServer = NULL;
	}
	else
	{
		LobbyClientNodeDef *pClientNode = (LobbyClientNodeDef *)lpPlayerNode;
		m_LobbyClientNodeList.DelNodeFromMap(pClientNode->m_nUserID);

		__log(_DEBUG, SERVER_NAME, "OnClosed: UserID[%d] has been closed !", pClientNode->m_nUserID);
	}
	
    GameProvider::OnClosed(lpPlayerNode, bClosed);
}

void LobbyServer::OnTimer(long tmNow)
{
	TimerKeepAliveServer(tmNow);
	TimerSendUpdateLobbySvrOnlineCount(tmNow);
	TimerUpdateClientNodeItemsInfo (tmNow);
    OnProcDBWorkDone( tmNow );
}

void LobbyServer::TimerKeepAliveServer(long tmNow)
{
	static time_t tLastSend = 0;

	//心跳连接发送
	if (tmNow - tLastSend >= KEEP_ALVIE_TIME)
	{
		tLastSend = tmNow;
		if (NULL != m_lpDispatchServer)
		{
			GameProvider::SendData(m_lpDispatchServer, NM_KEEP_ALIVE, NULL, 0);
		}
	}
}

void LobbyServer::TimerSendUpdateLobbySvrOnlineCount(long tmNow)
{
	static time_t tLastSend = 0;
	if (NULL == m_lpDispatchServer) return;

	if (m_nServerID == 0 || m_usServerPort == 0) 
	{
		__log(_ERROR, SERVER_NAME, "TimerSendUpdateLobbySvrOnlineCount: server_id=%d, server_port=%d", m_nServerID, m_usServerPort);
		return;
	}

	if (tmNow - tLastSend < 60) return;
	tLastSend = tmNow;

	tagDSUpdateLobbySvrOnlineCountReq OnlineCount = {0};

	OnlineCount.nServerID = m_nServerID;
	OnlineCount.nOnlineCount = m_LobbyClientNodeList.GetTotalPlayerCount();

	SendData(m_lpDispatchServer, DISPATCH_SVR_UPDATE_LOBBYSVR_ONLINE_COUNT_REQ, &OnlineCount, sizeof(OnlineCount));	

	__log(_DEBUG, SERVER_NAME, "TimerSendUpdateLobbySvrOnlineCount: server_id=%d, OnlineCount=%d", m_nServerID, OnlineCount.nOnlineCount);
}

void LobbyServer::TimerUpdateClientNodeItemsInfo (long tmNow)
{
	static time_t tLastUpdate = 0;
	if (tmNow - tLastUpdate < 60) return;
	tLastUpdate = tmNow;

	time_t updateTime = 0;
	time (&updateTime);

	for (LobbyClientNodeItemsInfo::iterator it=m_LobbyClientNodeItemsInfo.begin(); it!=m_LobbyClientNodeItemsInfo.end(); it++)
	{
		if ((*it)->nTimeLimited && updateTime > (*it)->nAvalibleTime && (*it)->nHasNoticed == 0)
		{
			(*it)->nHasNoticed = 1;

			LobbyClientNodeDef *pOldClientNode = m_LobbyClientNodeList.GetLobbyClientNode((*it)->nUserID);
			if (NULL != pOldClientNode)
			{
				LobbyGetUserItemsOutOfDateDspData ItemsOutOfDataInfo = {0};
				ItemsOutOfDataInfo.nItemID = (*it)->nItemsID;
				ItemsOutOfDataInfo.OutOfDataTime = (*it)->nAvalibleTime;

				if (true == GameProvider::SendData(pOldClientNode, LOBBYCLIENT_GET_USER_ITEMS_OUTOF_DATE_RSP, &ItemsOutOfDataInfo, sizeof(ItemsOutOfDataInfo)))
				{
					__log(_DEBUG, SERVER_NAME, "TimerUpdateClientNodeItemsInfo: send LOBBYCLIENT_GET_USER_ITEMS_OUTOF_DATE_RSP to ClientNode success! UserID [%d], ItemsID[%d]", (*it)->nUserID, ItemsOutOfDataInfo.nItemID);
				}
			}
		}
	}
}

void LobbyServer::TimerRemoveClientNodeItemsInfo (long tmNow)
{
	static time_t tLastUpdate = 0;
	if (tmNow - tLastUpdate < 3600) return;
	tLastUpdate = tmNow;

	for (LobbyClientNodeItemsInfo::iterator it = m_LobbyClientNodeItemsInfo.begin(); it != m_LobbyClientNodeItemsInfo.end(); it++)
	{
		delete (*it);
	}
	m_LobbyClientNodeItemsInfo.clear ();
}

IPlayerNode* LobbyServer::OnCreatePlayer()
{
    return new LobbyClientNodeDef();
}

long LobbyServer::ConnectServer(const char *lpszServerName, IPlayerNode*& lpPlayerNode)
{
	long lResult = -1;

	char szBuffer[32];

	if (0 != g_cfg.GetValueStr("connect", lpszServerName, szBuffer, sizeof(szBuffer)))
	{
		char szAddr[32] = {0};
		int iPort = 0;

		sscanf(szBuffer, "%[^:]:%d", szAddr, &iPort);

		lpPlayerNode = GameProvider::Create(false, szAddr, iPort, 5000);

		if (NULL != lpPlayerNode)
		{
			if (true == GameProvider::RecvData(lpPlayerNode))
			{
				__log(_DEBUG, "LobbyServer::ConnectServer", "connect '%s' success, host[%s], port[%d], node = %08x !", lpszServerName, szAddr, iPort, lpPlayerNode);

				lResult = 0;
			}
			else
			{
				__log(_ERROR, "LobbyServer::ConnectServer", "connect '%s', host[%s], port[%d], RecvData() error !, node = %08x", lpszServerName, szAddr, iPort, lpPlayerNode);

				GameProvider::Close(lpPlayerNode);
			}
		}
		else
		{
			__log(_ERROR, "LobbyServer::ConnectServer", "Create() error, '%s', host[%s], port[%d], errno[%d], info[%s] !", lpszServerName, szAddr, iPort, errno, strerror(errno));			
		}
	}
	else
	{
		__log(_ERROR, "LobbyServer::ConnectServer", "g_cfg.GetValueStr() '%s' error !", lpszServerName);
	}

	return lResult;
}

void LobbyServer::SendRegLobbyServerReq()
{
	if (m_bSendRegSvrReq==true) return;

	if (m_nServerID == 0 || m_usServerPort == 0) 
	{
		__log(_ERROR, "LobbyServer::SendRegLobbyServerReq", "server_id=%d, server_port=%d", m_nServerID, m_usServerPort);
		return;
	}

	if (m_lpDispatchServer != NULL)
	{
		tagDSRegLobbyServerReq RegServer = {0};

		RegServer.nServerID = m_nServerID;
		RegServer.ulServerIP = m_ulServerIP;
		RegServer.usServerPort = m_usServerPort;

		SendData(m_lpDispatchServer, DISPATCH_SVR_REG_LOBBYSVR_REQ, &RegServer, sizeof(tagDSRegLobbyServerReq));	

		m_bSendRegSvrReq = true;
	}
}

//分发服务器的消息处理
void LobbyServer::HandleDispatchServerNetMsg(IPlayerNode *lpPlayerNode, unsigned short type, void *buffer, int length)
{
	switch (type)
	{
	case DISPATCH_SVR_GET_GAME_ONLINE_COUNT_RSP:
		OnDispatchServerGetGameOnlineCountRspMsg(lpPlayerNode, buffer, length);
		break;

	case DISPATCH_SVR_CLIENT_GET_SYS_BROADCAST_MSG_RSP:
		OnDispatchServerClientGetSysBroadcastMsgRsp(lpPlayerNode, buffer, length);
		break;

	case DISPATCH_SVR_BATCHSEND_SYS_BROADCAST_MSG:
		OnDispatchServerBatchSendSysBroadcastMsg(lpPlayerNode, buffer, length);
		break;

	case DISPATCH_SVR_NOTICE_LOBBYSVR_CHECK_LOGIN_RSP:
		OnDispatchServerNoticeLobbySvrCheckLoginRsp(lpPlayerNode, buffer, length);
		break;

	case DISPATCH_SVR_LOBBYCLIENT_GET_GQ_CAIPIAOINFO_RSP:
		OnDispatchServerGetGQCaiPiaoInfoRspMsg(lpPlayerNode, buffer, length);
		break;

	case DISPATCH_SVR_LOBBYCLIENT_IS_CAIPIAO_ACTIVITY_ACCOUNT_RSP:
		OnDispatchServerIsCaiPiaoActivityAccountRsp(lpPlayerNode, buffer, length);
		break;

	case DISPATCH_SVR_LOBBYCLIENT_ACTIVITY_INFO_RSP:
		OnDispatchServerGetLobbyActivityInfoRspMsg(lpPlayerNode, buffer, length);
		break;

	case DISPATCH_SVR_LOBBYCLIENT_REFRESH_GAMECOIN_INFO_RSP:
		//OnDispatchServerLobbyRefreshGameCoinInfoRspMsg(lpPlayerNode, buffer, length);
		break;

	case DISPATCH_SVR_LOBBYCLIENT_OLDPLAYER_GET_CAIPIAO_RSP:
		OnDispatchServerOldPlayerGetCaiPiaoRspMsg(lpPlayerNode, buffer, length);
		break;

	case DISPATCH_SVR_LOBBYCLIENT_ADD_GQ_LOTTERYTICKET_RSP:
		OnDispatchServerLobbyAddGQLotteryTicketRspMsg(lpPlayerNode, buffer, length);
		break;
		
	case DISPATCH_SVR_LOBBYCLIENT_SEND_PLAYER_MESSAGE_RSP:
		//OnDispatchServerClientGetSysBroadcastMsgRsp(lpPlayerNode, buffer, length);
		OnDispatchServerLobbySendPlayerMessageRspMsg (lpPlayerNode, buffer, length);
		break;

	case DISPATCH_SVR_LOBBYCLIENT_BUY_ITEMS_RSP:
		OnDispatchServerLobbyPlayerBuyItems (lpPlayerNode, buffer, length);
		break;

	case DISPATCH_SVR_LOBBYCLIENT_GETSINGLE_ONLINECOUNTS_RSP:
		OnDispatchServerLobbyGetSingleGameOnlineCounts (lpPlayerNode, buffer, length);
		break;

	case DISPATCH_SVR_LOBBYCLIENT_GET_ANNIVERSARY_SENDCOIN_TIMES_RSP:
		OnDispatchServerLobbyGetAnniversarySendCoinTimes (lpPlayerNode, buffer, length);
		break;

	case DISPATCH_SVR_LOBBYCLIENT_SET_HEAD_IMAGE_RSP:
		OnDispatchServerLobbySetHeadImageRspMsg (lpPlayerNode, buffer, length);
		break;

	case DISPATCH_SVR_LOBBYCLIENT_BUY_HEAD_IMAGE_RSP:
		OnDispatchServerLobbyBuyHeadImageRspMsg (lpPlayerNode, buffer, length);
		break;

	case DISPATCH_SVR_LOBBYCLIENT_GET_ITEMS_INFO_RSP:
		OnDispatchServerLobbyGetItemsInfoRspMsg (lpPlayerNode, buffer, length);
		break;

	case DISPATCH_SVR_LOBBYCLIENT_GET_ONSALE_INFO_RSP:
		OnDispatchServerLobbyGetOnSaleInfoRspMsg (lpPlayerNode, buffer, length);
		break;
	case DISPATCH_SVR_LOBBYCLIENT_BROADCAST_PLAYER_MESSAGE_RSP:
		OnDispatchServerLobbyGetPlayeBroadcastMessagePspMsg (lpPlayerNode, buffer,  length);
		break;
	case DISPATCH_SVR_LOBBYCLIENT_GET_ITEMS_OUTOFDATE_RSP:
		OnDispatchServerLobbyGetItemsOutOfDateRspMsg (lpPlayerNode, buffer, length);
		break;

	case DISPATCH_SVR_BATCHSEND_PLAYER_BROADCAST_MSG:
		OnDispatchServerBatchPlayerBroadcastMsg (lpPlayerNode, buffer, length);
		break;

	default:
		__log(_ERROR, SERVER_NAME, "HandleDispatchServerNetMsg: type invalid, type [%d] !", type);
		GameProvider::Close(lpPlayerNode);
		break;
	}
}

void LobbyServer::OnDispatchServerGetGameOnlineCountRspMsg(IPlayerNode *lpPlayerNode, void *buffer, long length)
{
	char *pReadBuffer = (char*)buffer;
	int nHeadSize = sizeof(MsgHeader);
	int nReadSize = nHeadSize;
	int nSendSize = 0;

	int nSendUserID = 0;
	memcpy(&nSendUserID, pReadBuffer+nReadSize, sizeof(int));
	nReadSize += sizeof(int);

	__log(_DEBUG, SERVER_NAME, "OnDispatchServerGetGameOnlineCountRspMsg: SendUserID[%d]", nSendUserID);

	if (0 == nSendUserID)
	{
		nSendSize = length-nReadSize+nHeadSize;
		memcpy(pReadBuffer+nHeadSize, pReadBuffer+nReadSize, length-nReadSize);

		m_LobbyClientNodeList.SendBatchData(this, LOBBYCLIENT_GET_GAME_ONLINE_COUNT_RSP, pReadBuffer, nSendSize);
	} 
	else
	{
		unsigned long ulPlayerNode = 0;
		memcpy(&ulPlayerNode, pReadBuffer+nReadSize, sizeof(unsigned long));
		nReadSize += sizeof(unsigned long);

		unsigned long ulPlayerNodeId = 0;
		memcpy(&ulPlayerNodeId, pReadBuffer+nReadSize, sizeof(unsigned long));
		nReadSize += sizeof(unsigned long);

		nSendSize = length-nReadSize+nHeadSize;
		memcpy(pReadBuffer+nHeadSize, pReadBuffer+nReadSize, length-nReadSize);

		LobbyClientNodeDef *pClientNode = NULL;
		pClientNode = m_LobbyClientNodeList.GetLobbyClientNode(nSendUserID);
		
		if (NULL != pClientNode 
			&& pClientNode == (LobbyClientNodeDef *)ulPlayerNode
			&& *pClientNode == ulPlayerNodeId)
		{
			GameProvider::SendData(pClientNode, LOBBYCLIENT_GET_GAME_ONLINE_COUNT_RSP, pReadBuffer, nSendSize);
		}
	}
}

void LobbyServer::OnDispatchServerClientGetSysBroadcastMsgRsp(IPlayerNode *lpPlayerNode, void *buffer, long length)
{
	char *pReadBuffer = (char*)buffer;
	int nHeadSize = sizeof(MsgHeader);
	int nReadSize = nHeadSize;

	int nSendUserID = 0;
	memcpy(&nSendUserID, pReadBuffer+nReadSize, sizeof(int));
	nReadSize += sizeof(int);

	unsigned long ulPlayerNode = 0;
	memcpy(&ulPlayerNode, pReadBuffer+nReadSize, sizeof(unsigned long));
	nReadSize += sizeof(unsigned long);

	unsigned long ulPlayerNodeId = 0;
	memcpy(&ulPlayerNodeId, pReadBuffer+nReadSize, sizeof(unsigned long));
	nReadSize += sizeof(unsigned long);

	int nSendSize = length-nReadSize+nHeadSize;
	memcpy(pReadBuffer+nHeadSize, pReadBuffer+nReadSize, length-nReadSize);

	LobbyClientNodeDef *pClientNode = NULL;
	pClientNode = m_LobbyClientNodeList.GetLobbyClientNode(nSendUserID);

	__log(_DEBUG, SERVER_NAME, "OnDispatchServerClientGetSysBroadcastMsgRsp: SendUserID[%d], ClientNode[%u]", nSendUserID, pClientNode);

	if (NULL != pClientNode 
		&& pClientNode == (LobbyClientNodeDef *)ulPlayerNode
		&& *pClientNode == ulPlayerNodeId)
	{
		if (false == GameProvider::SendData(pClientNode, LOBBYCLIENT_GET_SYS_BROADCAST_MSG_RSP, pReadBuffer, nSendSize))
		{
			__log(_ERROR, SERVER_NAME, "OnDispatchServerClientGetSysBroadcastMsgRsp: send LOBBYCLIENT_GET_SYS_BROADCAST_MSG_RSP failed! SendUserID[%d]", nSendUserID);
		}
		else
		{
			__log(_DEBUG, SERVER_NAME, "OnDispatchServerClientGetSysBroadcastMsgRsp: send LOBBYCLIENT_GET_SYS_BROADCAST_MSG_RSP success! SendUserID[%d]", nSendUserID);
		}
	}
}

void LobbyServer::OnDispatchServerBatchSendSysBroadcastMsg(IPlayerNode *lpPlayerNode, void *buffer, long length)
{
	if (length != sizeof(tagDSBatchSendSysBroadcastMsg))
	{
		__log(_ERROR, SERVER_NAME, "OnDispatchServerBatchSendSysBroadcastMsg: length[%d] not match, expect[%d] !", length, sizeof(tagDSBatchSendSysBroadcastMsg));
		GameProvider::Close(lpPlayerNode);
		return;
	}

	tagDSBatchSendSysBroadcastMsg *pSysBroadcast = (tagDSBatchSendSysBroadcastMsg*)buffer;

	int nSendSize=0;
	char cSendBuffer[4096]={0};

	MsgHeader head={0};
	int nHeadSize = sizeof(MsgHeader);
	memcpy(cSendBuffer, &head, nHeadSize);
	nSendSize += nHeadSize;

	tagLobbyClientGetSysBroadcastMsgRsp SysBroadcast={0};

	SysBroadcast.nMsgID = pSysBroadcast->nMsgID;
	SysBroadcast.nMsgTypeID = pSysBroadcast->nMsgTypeID;
	strcpy(SysBroadcast.szMsgContext, pSysBroadcast->szMsgContext);

	memcpy(cSendBuffer+nSendSize, &SysBroadcast, sizeof(SysBroadcast));
	nSendSize += sizeof(SysBroadcast);

	m_LobbyClientNodeList.SendSiteBatchData(this, pSysBroadcast->nSiteID, LOBBYCLIENT_GET_SYS_BROADCAST_MSG_RSP, cSendBuffer, nSendSize);
}

void LobbyServer::OnDispatchServerBatchPlayerBroadcastMsg (IPlayerNode *lpPlayerNode, void *buffer, long length)
{
	if (length != sizeof (LobbyPlayerBroadCastMsg))
	{
		__log(_ERROR, SERVER_NAME, "OnDispatchServerBatchPlayerBroadcastMsg: length[%d] not match, expect[%d] !", length, sizeof(LobbyPlayerBroadCastMsg));
		GameProvider::Close(lpPlayerNode);
		return;
	}

	LobbyPlayerBroadCastMsg *pSysBroadcast = (LobbyPlayerBroadCastMsg*)buffer;

	int nSendSize=0;
	char cSendBuffer[4096]={0};

	MsgHeader head={0};
	int nHeadSize = sizeof(MsgHeader);
	memcpy(cSendBuffer, &head, nHeadSize);
	nSendSize += nHeadSize;

	LobbyPlayerBroadCastMsg SysBroadcast={0};

	SysBroadcast.nMsgID = pSysBroadcast->nMsgID;
	SysBroadcast.nGameID = pSysBroadcast->nGameID;
	SysBroadcast.nPlatformID = pSysBroadcast->nPlatformID;
	SysBroadcast.nSendMessageTime = pSysBroadcast->nSendMessageTime;
	SysBroadcast.nSiteID = pSysBroadcast->nSiteID;
	SysBroadcast.nUserID = pSysBroadcast->nUserID;
	strncpy (SysBroadcast.szContent, pSysBroadcast->szContent, sizeof (SysBroadcast.szContent));
	strncpy(SysBroadcast.szNickName, pSysBroadcast->szNickName, sizeof (SysBroadcast.szNickName));

	memcpy(cSendBuffer+nSendSize, &SysBroadcast, sizeof(SysBroadcast));
	nSendSize += sizeof(SysBroadcast);

	m_LobbyClientNodeList.SendBatchData(this, LOBBYCLIENT_BATCH_PLAYER_BROADCAST_MSG, cSendBuffer, nSendSize);

}

void LobbyServer::OnDispatchServerNoticeLobbySvrCheckLoginRsp(IPlayerNode *lpPlayerNode, void *buffer, long length)
{
	if (length != sizeof(tagDSNoticeLobbySvrCheckLoginRsp))
	{
		__log(_ERROR, SERVER_NAME, "OnDispatchServerNoticeLobbySvrCheckLoginRsp: length[%d] not match, expect[%d] !", length, sizeof(tagDSNoticeLobbySvrCheckLoginRsp));
		GameProvider::Close(lpPlayerNode);
		return;
	}

	tagDSNoticeLobbySvrCheckLoginRsp *pCheckLogin = (tagDSNoticeLobbySvrCheckLoginRsp*)buffer;

	//检验该玩家是否已与大厅服务器连接
	LobbyClientNodeDef *pOldClientNode = m_LobbyClientNodeList.GetLobbyClientNode(pCheckLogin->nLoginUserID);
	if (NULL != pOldClientNode)
	{
		tagLobbyClientLogoutByLobbySvrRsp Logout={0};

		Logout.nUserID = pCheckLogin->nLoginUserID;
		snprintf(Logout.szDescribe, 127, "您的账号在其他地方登录，您被踢下线！");

		if (true == GameProvider::SendData(pOldClientNode, LOBBYCLIENT_LOGOUT_BY_LOBBYSVR_RSP, &Logout, sizeof(Logout)))
		{
			__log(_DEBUG, SERVER_NAME, "OnDispatchServerNoticeLobbySvrCheckLoginRsp: send LOBBYCLIENT_LOGOUT_BY_LOBBYSVR_RSP to OldClientNode success!");
		}

		for (LobbyClientNodeItemsInfo::iterator it = m_LobbyClientNodeItemsInfo.begin(); it != m_LobbyClientNodeItemsInfo.end();)
		{
			if ((*it)->nUserID == pCheckLogin->nLoginUserID)
			{
				delete (*it);
				it = m_LobbyClientNodeItemsInfo.erase (it);
				__log (_DEBUG, SERVER_NAME, "OnDispatchServerLobbyGetItemsInfoRspMsg: Del item node itemID [%d], UserID [%d], hasNoticed[%d]", (*it)->nItemsID, (*it)->nUserID, (*it)->nHasNoticed);
			}
			else
			{
				it++;
			}
		}
		GameProvider::Close(pOldClientNode);
	}
}

void LobbyServer::OnDispatchServerGetGQCaiPiaoInfoRspMsg(IPlayerNode *lpPlayerNode, void *buffer, long length)
{
	if (length != sizeof(tagDSGetGQCaiPiaoInfoRsp))
	{
		__log(_ERROR, SERVER_NAME, "OnDispatchServerGetGQCaiPiaoInfoRspMsg: length[%d] not match, expect[%d] !", length, sizeof(tagDSGetGQCaiPiaoInfoRsp));
		GameProvider::Close(lpPlayerNode);
		return;
	}

	tagDSGetGQCaiPiaoInfoRsp *pCaiPiaoInfoRsp = (tagDSGetGQCaiPiaoInfoRsp*)buffer;

	LobbyClientNodeDef *pClientNode = NULL;
	pClientNode = m_LobbyClientNodeList.GetLobbyClientNode(pCaiPiaoInfoRsp->nUserID);

	if (NULL != pClientNode 
		&& pClientNode == (LobbyClientNodeDef *)pCaiPiaoInfoRsp->ulPlayerNode
		&& *pClientNode == pCaiPiaoInfoRsp->ulPlayerNodeId)
	{
		tagLobbyClientGetGQCaiPiaoInfoRsp CaiPiaoInfo = {0};

		CaiPiaoInfo.nUserID = pCaiPiaoInfoRsp->nUserID;
		CaiPiaoInfo.nShareNumber = pCaiPiaoInfoRsp->nShareNumber;
		CaiPiaoInfo.nNewLotteryRecord = pCaiPiaoInfoRsp->nNewLotteryRecord;
		CaiPiaoInfo.nNewShareRecord = pCaiPiaoInfoRsp->nNewShareRecord;
		CaiPiaoInfo.nNewLotteryNumber = pCaiPiaoInfoRsp->nNewLotteryNumber;
		CaiPiaoInfo.nOldPlayerAddStatus = pCaiPiaoInfoRsp->nOldPlayerAddStatus;
		CaiPiaoInfo.nNewPlayerAddStatus = pCaiPiaoInfoRsp->nNewPlayerAddStatus;
		CaiPiaoInfo.nTipPlayerStatus = pCaiPiaoInfoRsp->nTipPlayerStatus;

		if (false == GameProvider::SendData(pClientNode, LOBBYCLIENT_GET_GQ_CAIPIAOINFO_RSP, &CaiPiaoInfo, sizeof(CaiPiaoInfo)))
		{
			__log(_ERROR, SERVER_NAME, "OnDispatchServerGetGQCaiPiaoInfoRspMsg: send LOBBYCLIENT_GET_GQ_CAIPIAOINFO_RSP failed! UserID[%d]", pCaiPiaoInfoRsp->nUserID);
		}
		else
		{
			__log(_DEBUG, SERVER_NAME, "OnDispatchServerGetGQCaiPiaoInfoRspMsg: send LOBBYCLIENT_GET_GQ_CAIPIAOINFO_RSP success! UserID[%d]", pCaiPiaoInfoRsp->nUserID);
		}
	}
	else
	{
		__log(_ERROR, SERVER_NAME, "OnDispatchServerGetGQCaiPiaoInfoRspMsg: ClientNode error! UserID[%d]", pCaiPiaoInfoRsp->nUserID);
	}
}

void LobbyServer::OnDispatchServerIsCaiPiaoActivityAccountRsp(IPlayerNode *lpPlayerNode, void *buffer, long length)
{
	if (length != sizeof(tagDSIsCaiPiaoActivityAccountRsp))
	{
		__log(_ERROR, SERVER_NAME, "OnDispatchServerIsCaiPiaoActivityAccountRsp: length[%d] not match, expect[%d] !", length, sizeof(tagDSIsCaiPiaoActivityAccountRsp));
		GameProvider::Close(lpPlayerNode);
		return;
	}

	tagDSIsCaiPiaoActivityAccountRsp *pIsCPAccountRsp = (tagDSIsCaiPiaoActivityAccountRsp*)buffer;

	LobbyClientNodeDef *pClientNode = NULL;
	pClientNode = m_LobbyClientNodeList.GetLobbyClientNode(pIsCPAccountRsp->nUserID);

	if (NULL != pClientNode 
		&& pClientNode == (LobbyClientNodeDef *)pIsCPAccountRsp->ulPlayerNode
		&& *pClientNode == pIsCPAccountRsp->ulPlayerNodeId)
	{
		tagLobbyClientIsCaiPiaoActivityAccountRsp CPAccountRsp = {0};

		CPAccountRsp.nUserID = pIsCPAccountRsp->nUserID;
		CPAccountRsp.nAccountStatus = pIsCPAccountRsp->nAccountStatus;
		CPAccountRsp.nAccountTypeID = pIsCPAccountRsp->nAccountTypeID;

		if (false == GameProvider::SendData(pClientNode, LOBBYCLIENT_IS_CAIPIAO_ACTIVITY_ACCOUNT_RSP, &CPAccountRsp, sizeof(CPAccountRsp)))
		{
			__log(_ERROR, SERVER_NAME, "OnDispatchServerIsCaiPiaoActivityAccountRsp: send LOBBYCLIENT_GET_GQ_CAIPIAOINFO_RSP failed! UserID[%d]", pIsCPAccountRsp->nUserID);
		}
		else
		{
			__log(_DEBUG, SERVER_NAME, "OnDispatchServerIsCaiPiaoActivityAccountRsp: send LOBBYCLIENT_GET_GQ_CAIPIAOINFO_RSP success! UserID[%d]", pIsCPAccountRsp->nUserID);
		}
	}
	else
	{
		__log(_ERROR, SERVER_NAME, "OnDispatchServerIsCaiPiaoActivityAccountRsp: ClientNode error! UserID[%d]", pIsCPAccountRsp->nUserID);
	}
}

void LobbyServer::OnDispatchServerGetLobbyActivityInfoRspMsg(IPlayerNode *lpPlayerNode, void *buffer, long length)
{
	__log(_DEBUG, SERVER_NAME, "OnDispatchServerGetLobbyActivityInfoRspMsg: enter");

	char *pReadBuffer = (char*)buffer;
	int nHeadSize = sizeof(MsgHeader);
	int nReadSize = nHeadSize;

	int nSendUserID = 0;
	memcpy(&nSendUserID, pReadBuffer+nReadSize, sizeof(int));
	nReadSize += sizeof(int);

	unsigned long ulPlayerNode = 0;
	memcpy(&ulPlayerNode, pReadBuffer+nReadSize, sizeof(unsigned long));
	nReadSize += sizeof(unsigned long);

	unsigned long ulPlayerNodeId = 0;
	memcpy(&ulPlayerNodeId, pReadBuffer+nReadSize, sizeof(unsigned long));
	nReadSize += sizeof(unsigned long);

	int nSendSize = length-nReadSize+nHeadSize;
	memcpy(pReadBuffer+nHeadSize, pReadBuffer+nReadSize, length-nReadSize);

	LobbyClientNodeDef *pClientNode = NULL;
	pClientNode = m_LobbyClientNodeList.GetLobbyClientNode(nSendUserID);

	__log(_DEBUG, SERVER_NAME, "OnDispatchServerGetLobbyActivityInfoRspMsg: SendUserID[%d], ClientNode[%u]", nSendUserID, pClientNode);

	if (NULL != pClientNode 
		&& pClientNode == (LobbyClientNodeDef *)ulPlayerNode
		&& *pClientNode == ulPlayerNodeId)
	{
		if (false == GameProvider::SendData(pClientNode, LOBBYCLIENT_LOBBY_ACTIVITY_INFO_RSP, pReadBuffer, nSendSize))
		{
			__log(_ERROR, SERVER_NAME, "OnDispatchServerGetLobbyActivityInfoRspMsg: send LOBBYCLIENT_LOBBY_ACTIVITY_INFO_RSP to client failed! SendUserID[%d]", nSendUserID);
		}
		else
		{
			__log(_DEBUG, SERVER_NAME, "OnDispatchServerGetLobbyActivityInfoRspMsg: send LOBBYCLIENT_LOBBY_ACTIVITY_INFO_RSP to client success! SendUserID[%d]", nSendUserID);
		}
	}

	__log(_DEBUG, SERVER_NAME, "OnDispatchServerGetLobbyActivityInfoRspMsg: leave");

}

void LobbyServer::OnDispatchServerLobbyRefreshGameCoinInfoRspMsg(IPlayerNode *lpPlayerNode, void *buffer, long length)
{
	if (length != sizeof(tagDSRefreshGameCoinInfoRsp))
	{
		__log(_ERROR, SERVER_NAME, "OnDispatchServerLobbyRefreshGameCoinInfoRspMsg: length[%d] not match, expect[%d] !", length, sizeof(tagDSRefreshGameCoinInfoRsp));
		GameProvider::Close(lpPlayerNode);
		return;
	}

	tagDSRefreshGameCoinInfoRsp *pRefreshCoinInfoRsp = (tagDSRefreshGameCoinInfoRsp*)buffer;

	LobbyClientNodeDef *pClientNode = NULL;
	pClientNode = m_LobbyClientNodeList.GetLobbyClientNode(pRefreshCoinInfoRsp->nUserID);

	if (NULL != pClientNode 
		&& pClientNode == (LobbyClientNodeDef *)pRefreshCoinInfoRsp->ulPlayerNode
		&& *pClientNode == pRefreshCoinInfoRsp->ulPlayerNodeId)
	{
		tagLobbyRefreshGameCoinInfoRsp GameCoinInfo = {0};
		GameCoinInfo.nUserID = pRefreshCoinInfoRsp->nUserID;
		GameCoinInfo.lErrorCode = pRefreshCoinInfoRsp->lErrorCode;
		GameCoinInfo.llGameCoin = pRefreshCoinInfoRsp->llGameCoin;
		GameCoinInfo.nCoin2Award = pRefreshCoinInfoRsp->nCoin2Award;
		GameCoinInfo.nAwardScore = pRefreshCoinInfoRsp->nAwardScore;
		GameCoinInfo.nLabaCounts = pRefreshCoinInfoRsp->nLabaCounts;
		GameCoinInfo.nFirstCharge = pRefreshCoinInfoRsp->nFirstCharge;
		GameCoinInfo.llBankCoin = pRefreshCoinInfoRsp->llBankCoin;

		if (false == GameProvider::SendData(pClientNode, LOBBYCLIENT_REFRESH_GAMECOIN_INFO_RSP, &GameCoinInfo, sizeof(GameCoinInfo)))
		{
			__log(_ERROR, SERVER_NAME, "OnDispatchServerLobbyRefreshGameCoinInfoRspMsg: send LOBBYCLIENT_REFRESH_GAMECOIN_INFO_RSP failed! UserID[%d]", pRefreshCoinInfoRsp->nUserID);
		}
		else
		{
			__log(_DEBUG, SERVER_NAME, "OnDispatchServerLobbyRefreshGameCoinInfoRspMsg: send LOBBYCLIENT_REFRESH_GAMECOIN_INFO_RSP success! UserID[%d]", pRefreshCoinInfoRsp->nUserID);
		}
	}
	else
	{
		__log(_ERROR, SERVER_NAME, "OnDispatchServerLobbyRefreshGameCoinInfoRspMsg: ClientNode error! UserID[%d]", pRefreshCoinInfoRsp->nUserID);
	}
}

void LobbyServer::OnDispatchServerOldPlayerGetCaiPiaoRspMsg(IPlayerNode *lpPlayerNode, void *buffer, long length)
{
	if (length != sizeof(tagDSOldPlayerGetCaiPiaoRsp))
	{
		__log(_ERROR, SERVER_NAME, "OnDispatchServerOldPlayerGetCaiPiaoRspMsg: length[%d] not match, expect[%d] !", length, sizeof(tagDSOldPlayerGetCaiPiaoRsp));
		GameProvider::Close(lpPlayerNode);
		return;
	}

	tagDSOldPlayerGetCaiPiaoRsp *pGetCaiPiaoRsp = (tagDSOldPlayerGetCaiPiaoRsp*)buffer;

	LobbyClientNodeDef *pClientNode = NULL;
	pClientNode = m_LobbyClientNodeList.GetLobbyClientNode(pGetCaiPiaoRsp->nUserID);

	if (NULL != pClientNode 
		&& pClientNode == (LobbyClientNodeDef *)pGetCaiPiaoRsp->ulPlayerNode
		&& *pClientNode == pGetCaiPiaoRsp->ulPlayerNodeId)
	{
		tagLobbyOldPlayerGetCaiPiaoRsp OldPlayerGetCaiPiaoRsp = {0};

		OldPlayerGetCaiPiaoRsp.nUserID = pGetCaiPiaoRsp->nUserID;
		OldPlayerGetCaiPiaoRsp.bIsGetCaiPiao = pGetCaiPiaoRsp->bIsGetCaiPiao;
		strcpy(OldPlayerGetCaiPiaoRsp.szErrorDescription, pGetCaiPiaoRsp->szErrorDescription);

		if (false == GameProvider::SendData(pClientNode, LOBBYCLIENT_OLDPLAYER_GET_CAIPIAO_RSP, &OldPlayerGetCaiPiaoRsp, sizeof(OldPlayerGetCaiPiaoRsp)))
		{
			__log(_ERROR, SERVER_NAME, "OnDispatchServerOldPlayerGetCaiPiaoRspMsg: send LOBBYCLIENT_OLDPLAYER_GET_CAIPIAO_RSP failed! UserID[%d]", pGetCaiPiaoRsp->nUserID);
		}
		else
		{
			__log(_DEBUG, SERVER_NAME, "OnDispatchServerOldPlayerGetCaiPiaoRspMsg: send LOBBYCLIENT_OLDPLAYER_GET_CAIPIAO_RSP success! UserID[%d]", pGetCaiPiaoRsp->nUserID);
		}
	}
	else
	{
		__log(_ERROR, SERVER_NAME, "OnDispatchServerOldPlayerGetCaiPiaoRspMsg: ClientNode error! UserID[%d]", pGetCaiPiaoRsp->nUserID);
	}
}

void LobbyServer::OnDispatchServerLobbyAddGQLotteryTicketRspMsg(IPlayerNode *lpPlayerNode, void *buffer, long length)
{
	if (length != sizeof(tagDSAddGQLotteryTicketRsp))
	{
		__log(_ERROR, SERVER_NAME, "OnDispatchServerLobbyAddGQLotteryTicketRspMsg: length[%d] not match, expect[%d] !", length, sizeof(tagDSAddGQLotteryTicketRsp));
		GameProvider::Close(lpPlayerNode);
		return;
	}

	tagDSAddGQLotteryTicketRsp *pAddLotteryTicketRsp = (tagDSAddGQLotteryTicketRsp*)buffer;

	LobbyClientNodeDef *pClientNode = NULL;
	pClientNode = m_LobbyClientNodeList.GetLobbyClientNode(pAddLotteryTicketRsp->nUserID);

	if (NULL != pClientNode 
		&& pClientNode == (LobbyClientNodeDef *)pAddLotteryTicketRsp->ulPlayerNode
		&& *pClientNode == pAddLotteryTicketRsp->ulPlayerNodeId)
	{
		tagLobbyClientAddGQLotteryTicketRsp AddLotteryTicketRsp = {0};

		AddLotteryTicketRsp.nUserID = pAddLotteryTicketRsp->nUserID;
		AddLotteryTicketRsp.bIsAddSuccess = pAddLotteryTicketRsp->bIsAddSuccess;
		strcpy(AddLotteryTicketRsp.szErrorDescription, pAddLotteryTicketRsp->szErrorDescription);

		if (false == GameProvider::SendData(pClientNode, LOBBYCLIENT_ADD_GQ_LOTTERYTICKET_RSP, &AddLotteryTicketRsp, sizeof(AddLotteryTicketRsp)))
		{
			__log(_ERROR, SERVER_NAME, "OnDispatchServerLobbyAddGQLotteryTicketRspMsg: send LOBBYCLIENT_ADD_GQ_LOTTERYTICKET_RSP failed! UserID[%d]", pAddLotteryTicketRsp->nUserID);
		}
		else
		{
			__log(_DEBUG, SERVER_NAME, "OnDispatchServerLobbyAddGQLotteryTicketRspMsg: send LOBBYCLIENT_ADD_GQ_LOTTERYTICKET_RSP success! UserID[%d]", pAddLotteryTicketRsp->nUserID);
		}
	}
	else
	{
		__log(_ERROR, SERVER_NAME, "OnDispatchServerLobbyAddGQLotteryTicketRspMsg: ClientNode error! UserID[%d]", pAddLotteryTicketRsp->nUserID);
	}
}


void LobbyServer::OnDispatchServerLobbySendPlayerMessageRspMsg (IPlayerNode *lpPlayerNode, void *buffer, long length)
{
	if (length != sizeof(LobbySendMessageRspData_Trans))
	{
		__log(_ERROR, SERVER_NAME, "OnDispatchServerLobbySendPlayerMessageRspMsg: length[%d] not match, expect[%d] !", length, sizeof(LobbySendMessageRspData_Trans));
		GameProvider::Close(lpPlayerNode);
		return;
	}

	LobbySendMessageRspData_Trans * sendMessageRspData = (LobbySendMessageRspData_Trans*)buffer;
	LobbyClientNodeDef *pClientNode = NULL;
	pClientNode = m_LobbyClientNodeList.GetLobbyClientNode(sendMessageRspData->nUserID);

	if (NULL != pClientNode 
		&& pClientNode == (LobbyClientNodeDef *)sendMessageRspData->ulPlayerNode
		&& *pClientNode == sendMessageRspData->ulPlayerNodeId)
	{
		tagLobbySendMessageRsp sendMessage = {0};
		sendMessage.nResult = sendMessageRspData->nResult;
		sendMessage.nUserID = sendMessageRspData->nUserID;

		if (false == GameProvider::SendData(pClientNode, LOBBYCLIENT_SEND_PLAYERS_MESSAGE_RSP, &sendMessage, sizeof(tagLobbySendMessageRsp)))
		{
			__log(_ERROR, SERVER_NAME, "OnDispatchServerLobbySendPlayerMessageRspMsg: send LOBBYCLIENT_SEND_PLAYERS_MESSAGE_RSP failed! UserID[%d]", sendMessageRspData->nUserID);
		}
		else
		{
			__log(_DEBUG, SERVER_NAME, "OnDispatchServerLobbySendPlayerMessageRspMsg: send LOBBYCLIENT_SEND_PLAYERS_MESSAGE_RSP success! UserID[%d]", sendMessageRspData->nUserID);
		}
	}
	else
	{
		__log(_ERROR, SERVER_NAME, "OnDispatchServerLobbySendPlayerMessageRspMsg: ClientNode error! UserID[%d]", sendMessageRspData->nUserID);
	}
}

void LobbyServer::OnDispatchServerLobbyPlayerBuyItems (IPlayerNode *lpPlayerNode, void *buffer, long length)
{
	if (length != sizeof(LobbyPlayerBuyItemsRsp_trans))
	{
		__log(_ERROR, SERVER_NAME, "OnDispatchServerLobbyPlayerBuyItems: length[%d] not match, expect[%d] !", length, sizeof(LobbyPlayerBuyItemsRsp_trans));
		GameProvider::Close(lpPlayerNode);
		return;
	}

	LobbyPlayerBuyItemsRsp_trans * buyItemsInfoRsp_Trans = (LobbyPlayerBuyItemsRsp_trans*)buffer;
	LobbyClientNodeDef *pClientNode = NULL;
	pClientNode = m_LobbyClientNodeList.GetLobbyClientNode(buyItemsInfoRsp_Trans->nUserID);

	if (NULL != pClientNode 
		&& pClientNode == (LobbyClientNodeDef *)buyItemsInfoRsp_Trans->ulPlayerNode
		&& *pClientNode == buyItemsInfoRsp_Trans->ulPlayerNodeId)
	{
		LobbyPlayerBuyItemsRspData buyItemRspData = {0};
		buyItemRspData.nUserID = buyItemsInfoRsp_Trans->nUserID;
		buyItemRspData.llGameCoin = buyItemsInfoRsp_Trans->llGameCoin;
		buyItemRspData.nItemsBuyCounts = buyItemsInfoRsp_Trans->nItemsBuyCounts;
		buyItemRspData.nRealCost = buyItemsInfoRsp_Trans->nRealCost;
		buyItemRspData.nItemsCounts = buyItemsInfoRsp_Trans->nItemsCounts;
		buyItemRspData.IsTimeLimited = buyItemsInfoRsp_Trans->IsTimeLimited;
		buyItemRspData.nItemsID = buyItemsInfoRsp_Trans->nItemsID;
		strncpy (buyItemRspData.szLimitedTime, buyItemsInfoRsp_Trans->szLimitedTime, sizeof (buyItemRspData.szLimitedTime));
		buyItemRspData.nResult = buyItemsInfoRsp_Trans->nResult;
		if (false == GameProvider::SendData(pClientNode, LOBBYCLIENT_SEND_BUY_ITEMS_RSP, &buyItemRspData, sizeof(LobbyPlayerBuyItemsRspData)))
		{
			__log(_ERROR, SERVER_NAME, "OnDispatchServerLobbyPlayerBuyItems: send LOBBYCLIENT_SEND_BUY_ITEMS_RSP failed! UserID[%d]", buyItemsInfoRsp_Trans->nUserID);
		}
		else
		{
			__log(_DEBUG, SERVER_NAME, "OnDispatchServerLobbyPlayerBuyItems: send LOBBYCLIENT_SEND_BUY_ITEMS_RSP success! UserID[%d], GameCoin[%lld], ItemCount[%d], ItemBuyCount[%d], szLimitedTime[%s], sendDataLen[%d]",
				buyItemsInfoRsp_Trans->nUserID, buyItemsInfoRsp_Trans->llGameCoin, buyItemsInfoRsp_Trans->nItemsCounts, buyItemsInfoRsp_Trans->nItemsBuyCounts,buyItemsInfoRsp_Trans->szLimitedTime, sizeof(LobbyPlayerBuyItemsRspData));
		}
	}
	else
	{
		__log(_ERROR, SERVER_NAME, "OnDispatchServerLobbyPlayerBuyItems: ClientNode error! UserID[%d]", buyItemsInfoRsp_Trans->nUserID);
	}
}

void LobbyServer::OnDispatchServerLobbyGetAnniversarySendCoinTimes (IPlayerNode *lpPlayerNode, void *buffer, long length)
{
	if (length != sizeof(tagDSGetAnniversarySendCoinTimeRsp))
	{
		__log(_ERROR, SERVER_NAME, "OnDispatchServerLobbyGetAnniversarySendCoinTimes: length[%d] not match, expect[%d] !", length, sizeof(tagDSGetAnniversarySendCoinTimeRsp));
		GameProvider::Close(lpPlayerNode);
		return;
	}

	tagDSGetAnniversarySendCoinTimeRsp * buyItemsInfo = (tagDSGetAnniversarySendCoinTimeRsp*)buffer;
	LobbyClientNodeDef *pClientNode = NULL;
	pClientNode = m_LobbyClientNodeList.GetLobbyClientNode(buyItemsInfo->nUserID);

	if (NULL != pClientNode 
		&& pClientNode == (LobbyClientNodeDef *)buyItemsInfo->ulPlayerNode
		&& *pClientNode == buyItemsInfo->ulPlayerNodeId)
	{
		tagLobbyGetAnniversarySendCoinTimesRsp sendCoinRsp = {0};

		sendCoinRsp.nUserID = buyItemsInfo->nUserID;
		sendCoinRsp.nAddTimes = buyItemsInfo->nTimes;
		sendCoinRsp.nHasSendToday = buyItemsInfo->nHasSendToday;

		if (false == GameProvider::SendData(pClientNode, LOBBYCLIENT_GET_ANNIVERSARY_SEND_COIN_TIMES_RSP, &sendCoinRsp, sizeof(tagDSGetAnniversarySendCoinTimeRsp)))
		{
			__log(_ERROR, SERVER_NAME, "OnDispatchServerLobbyGetAnniversarySendCoinTimes: send LOBBYCLIENT_GET_ANNIVERSARY_SEND_COIN_TIMES_RSP failed! UserID[%d]", buyItemsInfo->nUserID);
		}
		else
		{
			__log(_DEBUG, SERVER_NAME, "OnDispatchServerLobbyGetAnniversarySendCoinTimes: send LOBBYCLIENT_GET_ANNIVERSARY_SEND_COIN_TIMES_RSP success! UserID[%d]", buyItemsInfo->nUserID);
		}
	}
	else
	{
		__log(_ERROR, SERVER_NAME, "OnDispatchServerLobbyGetAnniversarySendCoinTimes: ClientNode error! UserID[%d]", buyItemsInfo->nUserID);
	}
}

void LobbyServer::OnDispatchServerLobbySetHeadImageRspMsg (IPlayerNode * lpPlayerNode, void * buffer, long length)
{
	if (length != sizeof(tagLobbySetUserHeadImageRsp_Trans))
	{
		__log(_ERROR, SERVER_NAME, "OnDispatchServerLobbySetHeadImageRspMsg: length[%d] not match, expect[%d] !", length, sizeof(tagLobbySetUserHeadImageRsp_Trans));
		GameProvider::Close(lpPlayerNode);
		return;
	}
	tagLobbySetUserHeadImageRsp_Trans * buyItemsInfo = (tagLobbySetUserHeadImageRsp_Trans*)buffer;
	LobbyClientNodeDef *pClientNode = NULL;
	pClientNode = m_LobbyClientNodeList.GetLobbyClientNode(buyItemsInfo->nUserID);

	if (NULL != pClientNode 
		&& pClientNode == (LobbyClientNodeDef *)buyItemsInfo->ulPlayerNode
		&& *pClientNode == buyItemsInfo->ulPlayerNodeId)
	{
		tagLobbySetUserHeadImageRsp setImageRsp = {0};
		setImageRsp.nUserID = buyItemsInfo->nUserID;
		setImageRsp.nResult = buyItemsInfo->nResult;
		setImageRsp.nSettingHeadImageIndex = buyItemsInfo->nSettingHeadImageIndex;

		if (false == GameProvider::SendData(pClientNode, LOBBYCLIENT_SET_USER_HEAD_IMAGE_RSP, &setImageRsp, sizeof(tagLobbySetUserHeadImageRsp)))
		{
			__log(_ERROR, SERVER_NAME, "OnDispatchServerLobbySetHeadImageRspMsg: send LOBBYCLIENT_SET_USER_HEAD_IMAGE_RSP failed! UserID[%d]", buyItemsInfo->nUserID);
		}
		else
		{
			__log(_DEBUG, SERVER_NAME, "OnDispatchServerLobbySetHeadImageRspMsg: send LOBBYCLIENT_SET_USER_HEAD_IMAGE_RSP success! UserID[%d], HeadImageID[%d]", buyItemsInfo->nUserID, buyItemsInfo->nSettingHeadImageIndex);
		}
	}
	else
	{
		__log(_ERROR, SERVER_NAME, "OnDispatchServerLobbySetHeadImageRspMsg: ClientNode error! UserID[%d]", buyItemsInfo->nUserID);
	}
}

void LobbyServer::OnDispatchServerLobbyBuyHeadImageRspMsg (IPlayerNode * lpPlayerNode, void * buffer, long length)
{
	if (length != sizeof(tageLobbyBuyUserHeadImageRsp_Trans))
	{
		__log(_ERROR, SERVER_NAME, "OnDispatchServerLobbyBuyHeadImageRspMsg: length[%d] not match, expect[%d] !", length, sizeof(tageLobbyBuyUserHeadImageRsp_Trans));
		GameProvider::Close(lpPlayerNode);
		return;
	}
	tageLobbyBuyUserHeadImageRsp_Trans * buyItemsInfo = (tageLobbyBuyUserHeadImageRsp_Trans*)buffer;
	LobbyClientNodeDef *pClientNode = NULL;
	pClientNode = m_LobbyClientNodeList.GetLobbyClientNode(buyItemsInfo->nUserID);

	if (NULL != pClientNode 
		&& pClientNode == (LobbyClientNodeDef *)buyItemsInfo->ulPlayerNode
		&& *pClientNode == buyItemsInfo->ulPlayerNodeId)
	{
		tageLobbyBuyUserHeadImageRsp buyItemRsp = {0};
		buyItemRsp.nUserID = buyItemsInfo->nUserID;
		buyItemRsp.nResult = buyItemsInfo->nResult;
		buyItemRsp.nUserScore = buyItemsInfo->nUserScore;
		buyItemRsp.nCost = buyItemsInfo->nCostScore;

		if (false == GameProvider::SendData(pClientNode, LOBBYCLIENT_BUY_USER_HEAD_IMAGE_RSP, &buyItemRsp, sizeof(tageLobbyBuyUserHeadImageRsp)))
		{
			__log(_ERROR, SERVER_NAME, "OnDispatchServerLobbyBuyHeadImageRspMsg: send LOBBYCLIENT_BUY_USER_HEAD_IMAGE_RSP failed! UserID[%d]", buyItemsInfo->nUserID);
		}
		else
		{
			__log(_DEBUG, SERVER_NAME, "OnDispatchServerLobbyBuyHeadImageRspMsg: send LOBBYCLIENT_BUY_USER_HEAD_IMAGE_RSP success! UserID[%d], CostScore[%d], len[%d], len2[%d], longSize[%d], headSize[%d]", buyItemsInfo->nUserID, buyItemsInfo->nCostScore, sizeof (tageLobbyBuyUserHeadImageRsp), sizeof (long long), sizeof (long), sizeof (MsgHeader));
		}
	}
	else
	{
		__log(_ERROR, SERVER_NAME, "OnDispatchServerLobbyBuyHeadImageRspMsg: ClientNode error! UserID[%d]", buyItemsInfo->nUserID);
	}
}

void LobbyServer::OnDispatchServerLobbyGetItemsInfoRspMsg (IPlayerNode * lpPlayerNode, void * buffer, long length)
{
	char *pReadBuffer = (char*)buffer;
	int nHeadSize = sizeof(MsgHeader);
	int nReadSize = nHeadSize;
	int nSendSize = 0;

	int nSendUserID = 0;
	memcpy(&nSendUserID, pReadBuffer+nReadSize, sizeof(int));
	nReadSize += sizeof(int);

	__log(_DEBUG, SERVER_NAME, "OnDispatchServerLobbyGetItemsInfoRspMsg: GameID[%d]", nSendUserID);

	unsigned long ulPlayerNode = 0;
	memcpy(&ulPlayerNode, pReadBuffer+nReadSize, sizeof(unsigned long));
	nReadSize += sizeof(unsigned long);

	unsigned long ulPlayerNodeId = 0;
	memcpy(&ulPlayerNodeId, pReadBuffer+nReadSize, sizeof(unsigned long));
	nReadSize += sizeof(unsigned long);

	nSendSize = length-nReadSize+nHeadSize;
	memcpy(pReadBuffer+nHeadSize, pReadBuffer+nReadSize, length-nReadSize);

	LobbyClientNodeDef *pClientNode = (LobbyClientNodeDef *)ulPlayerNode;

	char * pSendDateBuff = pReadBuffer + nHeadSize;

	if (NULL != pClientNode 
		&& pClientNode == (LobbyClientNodeDef *)ulPlayerNode
		&& *pClientNode == ulPlayerNodeId)
	{
		tagLobbyPlayerItemsInfoRsp * rspInfo = (tagLobbyPlayerItemsInfoRsp*)pSendDateBuff;
		for (LobbyClientNodeItemsInfo::iterator it = m_LobbyClientNodeItemsInfo.begin(); it != m_LobbyClientNodeItemsInfo.end();)
		{
			if ((*it)->nHasNoticed == 0)
			{
				delete (*it);
				it = m_LobbyClientNodeItemsInfo.erase (it);
			}
			else
			{
				it ++;
			}
		}

		while (nReadSize < length)
		{
			LobbyPlayerItemsInfoData * itemsInfo = new LobbyPlayerItemsInfoData;
			itemsInfo->nUserID = nSendUserID;
			itemsInfo->nItemsID = rspInfo->nItemID;
			itemsInfo->nAvalibleTime = rspInfo->nAvalibleTime;
			itemsInfo->nTimeLimited = rspInfo->nTimeLimited;
			itemsInfo->nHasNoticed = 0;
			m_LobbyClientNodeItemsInfo.push_back (itemsInfo);
			rspInfo ++;
			nReadSize += sizeof (tagLobbyPlayerItemsInfoRsp);
		}

		GameProvider::SendData(pClientNode, LOBBYCLIENT_GET_USER_ITEMS_INFO_RES, pReadBuffer, nSendSize);
	}
	else
	{
		__log (_ERROR, SERVER_NAME, "OnDispatchServerLobbyGetItemsInfoRspMsg: ClientNode error! GameID[%d]", nSendUserID);
	}
}

void LobbyServer::OnDispatchServerLobbyGetOnSaleInfoRspMsg (IPlayerNode * lpPlayerNode, void * buffer, long length)
{
	char *pReadBuffer = (char*)buffer;
	int nHeadSize = sizeof(MsgHeader);
	int nReadSize = nHeadSize;
	int nSendSize = 0;

	int nSendUserID = 0;
	memcpy(&nSendUserID, pReadBuffer+nReadSize, sizeof(int));
	nReadSize += sizeof(int);

	__log(_DEBUG, SERVER_NAME, "OnDispatchServerLobbyGetOnSaleInfoRspMsg: UserID[%d]", nSendUserID);

	unsigned long ulPlayerNode = 0;
	memcpy(&ulPlayerNode, pReadBuffer+nReadSize, sizeof(unsigned long));
	nReadSize += sizeof(unsigned long);

	unsigned long ulPlayerNodeId = 0;
	memcpy(&ulPlayerNodeId, pReadBuffer+nReadSize, sizeof(unsigned long));
	nReadSize += sizeof(unsigned long);

	nSendSize = length-nReadSize+nHeadSize;
	memcpy(pReadBuffer+nHeadSize, pReadBuffer+nReadSize, length-nReadSize);

	LobbyClientNodeDef *pClientNode = (LobbyClientNodeDef *)ulPlayerNode;

	if (NULL != pClientNode 
		&& pClientNode == (LobbyClientNodeDef *)ulPlayerNode
		&& *pClientNode == ulPlayerNodeId)
	{
		GameProvider::SendData(pClientNode, LOBBYCLIENT_GET_ONSALE_ITEMS_INFO_RES, pReadBuffer, nSendSize);
	}
	else
	{
		__log (_ERROR, SERVER_NAME, "OnDispatchServerLobbyGetOnSaleInfoRspMsg: ClientNode error! UserID[%d]", nSendUserID);
	}
}

void LobbyServer::OnDispatchServerLobbyGetSingleGameOnlineCounts (IPlayerNode *lpPlayerNode, void *buffer, long length)
{
	char *pReadBuffer = (char*)buffer;
	int nHeadSize = sizeof(MsgHeader);
	int nReadSize = nHeadSize;
	int nSendSize = 0;

	int nSendUserID = 0;
	memcpy(&nSendUserID, pReadBuffer+nReadSize, sizeof(int));
	nReadSize += sizeof(int);

	__log(_DEBUG, SERVER_NAME, "OnDispatchServerLobbyGetSingleGameOnlineCounts: GameID[%d]", nSendUserID);

	unsigned long ulPlayerNode = 0;
	memcpy(&ulPlayerNode, pReadBuffer+nReadSize, sizeof(unsigned long));
	nReadSize += sizeof(unsigned long);

	unsigned long ulPlayerNodeId = 0;
	memcpy(&ulPlayerNodeId, pReadBuffer+nReadSize, sizeof(unsigned long));
	nReadSize += sizeof(unsigned long);

	nSendSize = length-nReadSize+nHeadSize;
	memcpy(pReadBuffer+nHeadSize, pReadBuffer+nReadSize, length-nReadSize);

	LobbyClientNodeDef *pClientNode = (LobbyClientNodeDef *)ulPlayerNode;
	pClientNode = m_LobbyClientNodeList.GetLobbyClientNode(nSendUserID);
		
	if (NULL != pClientNode 
		&& pClientNode == (LobbyClientNodeDef *)ulPlayerNode
		&& *pClientNode == ulPlayerNodeId)
	{
		GameProvider::SendData(pClientNode, LOBBYCLIENT_GET_PLAYER_ONLINE_COUNTS_RSP, pReadBuffer, nSendSize);
	}
	else
	{
		__log (_ERROR, SERVER_NAME, "OnDispatchServerLobbyGetSingleGameOnlineCounts: ClientNode error! UserID[%d]", nSendUserID);
	}
}

void LobbyServer::OnDispatchServerLobbyGetPlayeBroadcastMessagePspMsg (IPlayerNode * lpPlayerNode, void * buffer, long length)
{
	char *pReadBuffer = (char*)buffer;
	int nHeadSize = sizeof(MsgHeader);
	int nReadSize = nHeadSize;
	int nSendSize = 0;

	int nSendUserID = 0;
	memcpy(&nSendUserID, pReadBuffer+nReadSize, sizeof(int));
	nReadSize += sizeof(int);

	__log(_DEBUG, SERVER_NAME, "OnDispatchServerLobbyGetPlayeBroadcastMessagePspMsg: UserID[%d]", nSendUserID);

	unsigned long ulPlayerNode = 0;
	memcpy(&ulPlayerNode, pReadBuffer+nReadSize, sizeof(unsigned long));
	nReadSize += sizeof(unsigned long);

	unsigned long ulPlayerNodeId = 0;
	memcpy(&ulPlayerNodeId, pReadBuffer+nReadSize, sizeof(unsigned long));
	nReadSize += sizeof(unsigned long);

	nSendSize = length-nReadSize+nHeadSize;
	memcpy(pReadBuffer+nHeadSize, pReadBuffer+nReadSize, length-nReadSize);

	LobbyClientNodeDef *pClientNode = (LobbyClientNodeDef *)ulPlayerNode;

	if (NULL != pClientNode 
		&& pClientNode == (LobbyClientNodeDef *)ulPlayerNode
		&& *pClientNode == ulPlayerNodeId)
	{
		GameProvider::SendData(pClientNode, LOBBYCLIENT_BROADCAST_PLAYER_MESSAGE_RSP, pReadBuffer, nSendSize);
	}
	else
	{
		__log (_ERROR, SERVER_NAME, "OnDispatchServerLobbyGetPlayeBroadcastMessagePspMsg: ClientNode error! UserID[%d]", nSendUserID);
	}
}

void LobbyServer::OnDispatchServerLobbyGetItemsOutOfDateRspMsg (IPlayerNode * lpPlayerNode, void * buffer, long length)
{
	char *pReadBuffer = (char*)buffer;
	int nHeadSize = sizeof(MsgHeader);
	int nReadSize = nHeadSize;
	int nSendSize = 0;

	int nSendUserID = 0;
	memcpy(&nSendUserID, pReadBuffer+nReadSize, sizeof(int));
	nReadSize += sizeof(int);

	__log(_DEBUG, SERVER_NAME, "OnDispatchServerLobbyGetItemsOutOfDateRspMsg: UserID[%d]", nSendUserID);

	unsigned long ulPlayerNode = 0;
	memcpy(&ulPlayerNode, pReadBuffer+nReadSize, sizeof(unsigned long));
	nReadSize += sizeof(unsigned long);

	unsigned long ulPlayerNodeId = 0;
	memcpy(&ulPlayerNodeId, pReadBuffer+nReadSize, sizeof(unsigned long));
	nReadSize += sizeof(unsigned long);

	nSendSize = length-nReadSize+nHeadSize;
	memcpy(pReadBuffer+nHeadSize, pReadBuffer+nReadSize, length-nReadSize);

	LobbyClientNodeDef *pClientNode = (LobbyClientNodeDef *)ulPlayerNode;

	if (NULL != pClientNode 
		&& pClientNode == (LobbyClientNodeDef *)ulPlayerNode
		&& *pClientNode == ulPlayerNodeId)
	{
		GameProvider::SendData(pClientNode, LOBBYCLIENT_GET_USER_ITEMS_OUTOF_DATE_RSP, pReadBuffer, nSendSize);
	}
	else
	{
		__log (_ERROR, SERVER_NAME, "OnDispatchServerLobbyGetItemsOutOfDateRspMsg: ClientNode error! UserID[%d]", nSendUserID);
	}
}

//客户端的消息处理
void LobbyServer::HandleClientNetMsg(IPlayerNode *lpPlayerNode, unsigned short type, void *buffer, int length)
{
    switch (type)
    {
	case LOBBYCLIENT_LOGIN_REQ:
		OnLobbyClientLoginReqMsg(lpPlayerNode, buffer, length);
		break;

	case LOBBYCLIENT_GET_GQ_CAIPIAOINFO_REQ:
		OnLobbyClientGetGQCaiPiaoInfoReqMsg(lpPlayerNode, buffer, length);
		break;

	case LOBBYCLIENT_SET_GQ_CAIPIAOINFO_REQ:
		OnLobbyClientSetGQCaiPiaoInfoReqMsg(lpPlayerNode, buffer, length);
		break;

	case LOBBYCLIENT_ADD_GQ_LOTTERYTICKET_REQ:
		OnLobbyClientAddGQLotteryTicketReqMsg(lpPlayerNode, buffer, length);
		break;

	case LOBBYCLIENT_IS_CAIPIAO_ACTIVITY_ACCOUNT_REQ:
		OnLobbyClientIsCaiPiaoActivityAccountReqMsg(lpPlayerNode, buffer, length);
		break;

	case LOBBYCLIENT_REFRESH_GAMECOIN_INFO_REQ:
		OnLobbyClientRefreshGameCoinInfoReqMsg(lpPlayerNode, buffer, length);
		break;

	case LOBBYCLIENT_OLDPLAYER_GET_CAIPIAO_REQ:
		OnLobbyClientOldPlayerGetCaiPiaoReqMsg(lpPlayerNode, buffer, length);
		break;

	case LOBBYCLIENT_GET_PLAYER_ONLINE_COUNTS_REQ:
		OnLobbyClientGetSingleGameOnlineCountsReqMsg (lpPlayerNode, buffer, length);
		break;

	case LOBBYCLIENT_SEND_PLAYERS_MESSAGE_REQ:
		OnLobbyClientSendPlayerMessagesReqMsg (lpPlayerNode, buffer, length);
		break;

	case LOBBYCLIENT_SEND_BUY_ITEMS_REQ:
		OnLobbyClientBuyItemsReqMsg (lpPlayerNode, buffer, length);
		break;

	case LOBBYCLIENT_GET_ANNIVERSARY_SEND_COIN_TIMES_REQ:
		OnLobbyClientGetAnniversarySendCoinTimes (lpPlayerNode, buffer, length);
		break;

	case LOBBYCLIENT_SET_USER_HEAD_IMAGE_REQ:
		OnLobbyClientSetHeadImageReqMsg (lpPlayerNode, buffer, length);
		break;

	case LOBBYCLIENT_BUY_USER_HEAD_IMAGE_REQ:
		OnLobbyClientBuyHeadImageReqMsg (lpPlayerNode, buffer, length);
		break;

	case LOBBYCLIENT_GET_USER_ITEMS_INFO_REQ:
		OnLobbyClientSendGetItemsInfoReqMsg (lpPlayerNode, buffer, length);
		break;

	case LOBBYCLIENT_GET_ONSALE_ITEMS_INFO_REQ:
		OnLobbyClientGetOnSaleItemsInfoReqMsg (lpPlayerNode, buffer, length);
		break;

	case LOBBYCLIENT_BROADCAST_PLAYER_MESSAGE_REQ:
		OnLobbyClientBroadCastPlayerMessageReqMsg (lpPlayerNode,buffer, length);
		break;

	case LOBBYCLIENT_GET_USER_ITEMS_OUTOF_DATE_REQ:
		OnLobbyClientGetUserItemsOutOfDateMessageReqMsg (lpPlayerNode, buffer, length);
		break;
    case KEEP_ALIVE_MSG:
		//__log(_ERROR, SERVER_NAME, "HandleClientNetMsg: recv KEEP_ALIVE_MSG msg !");
        break;

    default:
        __log(_ERROR, SERVER_NAME, "HandleClientNetMsg: type invalid, type [%d] !", type);
        GameProvider::Close(lpPlayerNode);
        break;
    }
}

void LobbyServer::OnLobbyClientLoginReqMsg(IPlayerNode *lpPlayerNode, void *buffer, long length)
{
	if (sizeof(tagLobbyClientLoginReq) != length)
	{
		__log(_ERROR, SERVER_NAME, "OnLobbyClientLoginReqMsg: length[%d] not match, expect[%d] !", length, sizeof(tagLobbyClientLoginReq));
		return;
	}

	tagLobbyClientLoginReq *pClientLogin = (tagLobbyClientLoginReq*)buffer;
	LobbyClientNodeDef *pClientNode = (LobbyClientNodeDef*)lpPlayerNode;

	pClientNode->m_nUserID = pClientLogin->nUserID;
	pClientNode->m_nSiteID = pClientLogin->nSiteID;

	snprintf(pClientNode->m_szUserName, NAME_LEN-1, "%s", pClientLogin->szUserName);
	snprintf(pClientNode->m_szNickName, NAME_LEN-1, "%s", pClientLogin->szNickname);

	//检验该玩家是否已与大厅服务器连接
	LobbyClientNodeDef *pOldClientNode = m_LobbyClientNodeList.GetLobbyClientNode(pClientLogin->nUserID);
	if (NULL != pOldClientNode)
	{
		tagLobbyClientLogoutByLobbySvrRsp Logout={0};

		Logout.nUserID = pClientLogin->nUserID;
		snprintf(Logout.szDescribe, 127, "您的账号在其他地方登录，您被踢下线！");

		if (true == GameProvider::SendData(pOldClientNode, LOBBYCLIENT_LOGOUT_BY_LOBBYSVR_RSP, &Logout, sizeof(Logout)))
		{
			__log(_DEBUG, SERVER_NAME, "OnLobbyClientLoginReqMsg: send LOBBYCLIENT_LOGOUT_BY_LOBBYSVR_RSP to OldClientNode success!");
		}

		for (LobbyClientNodeItemsInfo::iterator it = m_LobbyClientNodeItemsInfo.begin(); it != m_LobbyClientNodeItemsInfo.end();)
		{
			if ((*it)->nUserID == pClientLogin->nUserID)
			{
				delete (*it);
				it = m_LobbyClientNodeItemsInfo.erase (it);
				__log (_DEBUG, SERVER_NAME, "OnDispatchServerLobbyGetItemsInfoRspMsg: Del item node itemID [%d], UserID [%d], hasNoticed[%d]", (*it)->nItemsID, (*it)->nUserID, (*it)->nHasNoticed);
			}
			else
			{
				it++;
			}
		}

		m_LobbyClientNodeList.DelNodeFromMap(pOldClientNode->m_nUserID);
		pOldClientNode->m_nUserID = 0;
		GameProvider::Close(pOldClientNode);
	}
	else
	{
		tagDSCheckLobbyUserLoginReq CheckLogin={0};

		CheckLogin.nLobbyServerID = m_nServerID;
		CheckLogin.nLoginUserID = pClientLogin->nUserID;

		if (true == GameProvider::SendData(m_lpDispatchServer, DISPATCH_SVR_LOBBYCLIENT_CHECK_LOGIN_REQ, &CheckLogin, sizeof(CheckLogin)))
		{
			__log(_DEBUG, SERVER_NAME, "OnLobbyClientLoginReqMsg: send DISPATCH_SVR_LOBBYCLIENT_CHECK_LOGIN_REQ to DispatchServer success!");
		}
	}

	__log(_DEBUG, SERVER_NAME, "OnLobbyClientLoginReqMsg: UserName[%s], UserID[%d] request login !", pClientNode->m_szUserName, pClientNode->m_nUserID);

	if (true == m_LobbyClientNodeList.AddNodeToMap(pClientLogin->nUserID, (unsigned long)pClientNode))
	{
		tagDSGetGameOnlineCountReq GetGameOnline={0};

		GetGameOnline.ulPlayerNode = (unsigned long)pClientNode;
		GetGameOnline.ulPlayerNodeId = *pClientNode;
		GetGameOnline.nUserID = pClientNode->m_nUserID;

		if (true == GameProvider::SendData(m_lpDispatchServer, DISPATCH_SVR_GET_GAME_ONLINE_COUNT_REQ, &GetGameOnline, sizeof(GetGameOnline)))
		{
			__log(_DEBUG, SERVER_NAME, "OnLobbyClientLoginReqMsg: send DISPATCH_SVR_GET_GAME_ONLINE_COUNT_REQ success!");
		}

		//请求大厅活动列表信息
		tagDSLobbyActivityInfoReq LobbyActivityInfo={0};

		LobbyActivityInfo.nUserID = pClientLogin->nUserID;

		LobbyActivityInfo.ulPlayerNode = (unsigned long)pClientNode;
		LobbyActivityInfo.ulPlayerNodeId = *pClientNode;

		if (true == GameProvider::SendData(m_lpDispatchServer, DISPATCH_SVR_LOBBYCLIENT_ACTIVITY_INFO_REQ, &LobbyActivityInfo, sizeof(LobbyActivityInfo)))
		{
			__log(_DEBUG, SERVER_NAME, "OnLobbyClientLoginReqMsg: send DISPATCH_SVR_LOBBYCLIENT_ACTIVITY_INFO_REQ success!");
		}

		//请求系统广播消息
		tagDSGetSysBroadcastMsgReq GetSysBroadcast={0};

		GetSysBroadcast.ulPlayerNode = (unsigned long)pClientNode;
		GetSysBroadcast.ulPlayerNodeId = *pClientNode;
		GetSysBroadcast.nUserID = pClientNode->m_nUserID;

		if (true == GameProvider::SendData(m_lpDispatchServer, DISPATCH_SVR_CLIENT_GET_SYS_BROADCAST_MSG_REQ, &GetSysBroadcast, sizeof(GetSysBroadcast)))
		{
			__log(_DEBUG, SERVER_NAME, "OnLobbyClientLoginReqMsg: send DISPATCH_SVR_CLIENT_GET_SYS_BROADCAST_MSG_REQ success!");
		}

		LobbyOnSaleItemsInfoReqData_Trans getItemsInfo = {0};
		getItemsInfo.nUserID = pClientNode->m_nUserID;
		getItemsInfo.ulPlayerNode = (unsigned long)pClientNode;
		getItemsInfo.ulPlayerNodeId = *pClientNode;
		if (true == GameProvider::SendData(m_lpDispatchServer, DISPATCH_SVR_LOBBYCLIENT_GET_ONSALE_INFO_REQ, &getItemsInfo, sizeof(getItemsInfo)))
		{
			__log(_DEBUG, SERVER_NAME, "OnLobbyClientLoginReqMsg: send DISPATCH_SVR_LOBBYCLIENT_GET_ONSALE_INFO_REQ success!");
		}

		tagLobbyPlayerItemsInfoReq_Trans itemsInfo_Trans = {0};
		itemsInfo_Trans.nUserID = pClientNode->m_nUserID;
		itemsInfo_Trans.ulPlayerNode = (unsigned long)pClientNode;
		itemsInfo_Trans.ulPlayerNodeId = *pClientNode;
		if (true == GameProvider::SendData (m_lpDispatchServer, DISPATCH_SVR_LOBBYCLIENT_GET_ITEMS_INFO_REQ, &itemsInfo_Trans, sizeof (itemsInfo_Trans)))
		{
			__log(_DEBUG, SERVER_NAME, "OnLobbyClientLoginReqMsg: send DISPATCH_SVR_LOBBYCLIENT_GET_ITEMS_INFO_REQ success!");
		}
	}
	else
	{
		__log(_ERROR, SERVER_NAME, "OnLobbyClientLoginReqMsg: AddNodeToMap failed! UserID[%d], UserName[%s]", pClientNode->m_nUserID, pClientNode->m_szUserName);
		GameProvider::Close(lpPlayerNode);
	}
}

void LobbyServer::OnLobbyClientGetGQCaiPiaoInfoReqMsg( IPlayerNode *lpPlayerNode, void *buffer, long length )
{
	if (sizeof(tagLobbyClientGetGQCaiPiaoInfoReq) != length)
	{
		__log(_ERROR, SERVER_NAME, "OnLobbyClientGetGQCaiPiaoInfoReqMsg: length[%d] not match, expect[%d] !", length, sizeof(tagLobbyClientGetGQCaiPiaoInfoReq));
		return;
	}

	tagLobbyClientGetGQCaiPiaoInfoReq *pGetGQCaiPiaoInfo = (tagLobbyClientGetGQCaiPiaoInfoReq*)buffer;
	tagDSGetGQCaiPiaoInfoReq DSGetGQCaiPiaoInfo = {0};

	LobbyClientNodeDef *pClientNode = (LobbyClientNodeDef*)lpPlayerNode;

	DSGetGQCaiPiaoInfo.nUserID = pGetGQCaiPiaoInfo->nUserID;
	DSGetGQCaiPiaoInfo.nSiteID = pGetGQCaiPiaoInfo->nSiteID;

	DSGetGQCaiPiaoInfo.ulPlayerNode = (unsigned long)pClientNode;
	DSGetGQCaiPiaoInfo.ulPlayerNodeId = *pClientNode;

	if (true == GameProvider::SendData(m_lpDispatchServer, DISPATCH_SVR_LOBBYCLIENT_GET_GQ_CAIPIAOINFO_REQ, &DSGetGQCaiPiaoInfo, sizeof(DSGetGQCaiPiaoInfo)))
	{
		__log(_DEBUG, SERVER_NAME, "OnLobbyClientGetGQCaiPiaoInfoReqMsg: send DISPATCH_SVR_LOBBYCLIENT_GET_GQ_CAIPIAOINFO_REQ to DispatchServer success ! UserID[%d], SiteID[%d]", pGetGQCaiPiaoInfo->nUserID, pGetGQCaiPiaoInfo->nSiteID);
	} 
	else
	{
		__log(_ERROR, SERVER_NAME, "OnLobbyClientGetGQCaiPiaoInfoReqMsg: send DISPATCH_SVR_LOBBYCLIENT_GET_GQ_CAIPIAOINFO_REQ to DispatchServer failed ! UserID[%d], SiteID[%d]", pGetGQCaiPiaoInfo->nUserID, pGetGQCaiPiaoInfo->nSiteID);
	}
}

void LobbyServer::OnLobbyClientSetGQCaiPiaoInfoReqMsg( IPlayerNode *lpPlayerNode, void *buffer, long length )
{
	if (sizeof(tagLobbyClientSetGQCaiPiaoInfoReq) != length)
	{
		__log(_ERROR, SERVER_NAME, "OnLobbyClientLoginReqMsg: length[%d] not match, expect[%d] !", length, sizeof(tagLobbyClientSetGQCaiPiaoInfoReq));
		return;
	}

	tagLobbyClientSetGQCaiPiaoInfoReq *pSetCaiPiaoInfo = (tagLobbyClientSetGQCaiPiaoInfoReq*)buffer;

	tagDSSetGQCaiPiaoInfoReq SetCaiPiaoInfo = {0};

	SetCaiPiaoInfo.nUserID = pSetCaiPiaoInfo->nUserID;
	SetCaiPiaoInfo.nSiteID = pSetCaiPiaoInfo->nSiteID;
	SetCaiPiaoInfo.bSetShareNumber = pSetCaiPiaoInfo->bSetShareNumber;
	SetCaiPiaoInfo.bSetNewLotteryRecord = pSetCaiPiaoInfo->bSetNewLotteryRecord;
	SetCaiPiaoInfo.bSetNewShareRecord = pSetCaiPiaoInfo->bSetNewShareRecord;
	SetCaiPiaoInfo.bSetNewLotteryNumber = pSetCaiPiaoInfo->bSetNewLotteryNumber;
	SetCaiPiaoInfo.nShareNumber = pSetCaiPiaoInfo->nShareNumber;
	SetCaiPiaoInfo.nNewLotteryRecord = pSetCaiPiaoInfo->nNewLotteryRecord;
	SetCaiPiaoInfo.nNewShareRecord = pSetCaiPiaoInfo->nNewShareRecord;
	SetCaiPiaoInfo.nNewLotteryNumber = pSetCaiPiaoInfo->nNewLotteryNumber;

	if (true == GameProvider::SendData(m_lpDispatchServer, DISPATCH_SVR_LOBBYCLIENT_SET_GQ_CAIPIAOINFO_REQ, &SetCaiPiaoInfo, sizeof(SetCaiPiaoInfo)))
	{
		__log(_DEBUG, SERVER_NAME, "OnLobbyClientSetGQCaiPiaoInfoReqMsg: send DISPATCH_SVR_LOBBYCLIENT_SET_GQ_CAIPIAOINFO_REQ to DispatchServer success ! UserID[%d], SiteID[%d]", pSetCaiPiaoInfo->nUserID, pSetCaiPiaoInfo->nSiteID);
	} 
	else
	{
		__log(_ERROR, SERVER_NAME, "OnLobbyClientSetGQCaiPiaoInfoReqMsg: send DISPATCH_SVR_LOBBYCLIENT_SET_GQ_CAIPIAOINFO_REQ to DispatchServer failed ! UserID[%d], SiteID[%d]", pSetCaiPiaoInfo->nUserID, pSetCaiPiaoInfo->nSiteID);
	}
}

void LobbyServer::OnLobbyClientAddGQLotteryTicketReqMsg( IPlayerNode *lpPlayerNode, void *buffer, long length )
{
	if (sizeof(tagLobbyClientAddGQLotteryTicketReq) != length)
	{
		__log(_ERROR, SERVER_NAME, "OnLobbyClientAddGQLotteryTicketReqMsg: length[%d] not match, expect[%d] !", length, sizeof(tagLobbyClientAddGQLotteryTicketReq));
		return;
	}

	LobbyClientNodeDef *pClientNode = (LobbyClientNodeDef *)lpPlayerNode;

	tagLobbyClientAddGQLotteryTicketReq *pAddLotteryTicket = (tagLobbyClientAddGQLotteryTicketReq*)buffer;

	tagDSAddGQLotteryTicketReq AddLotteryTicket = {0};

	AddLotteryTicket.nUserID = pAddLotteryTicket->nUserID;
	AddLotteryTicket.nSiteID = pAddLotteryTicket->nSiteID;

	AddLotteryTicket.ulPlayerNode = (unsigned long)pClientNode;
	AddLotteryTicket.ulPlayerNodeId = *pClientNode;

	if (true == GameProvider::SendData(m_lpDispatchServer, DISPATCH_SVR_LOBBYCLIENT_ADD_GQ_LOTTERYTICKET_REQ, &AddLotteryTicket, sizeof(AddLotteryTicket)))
	{
		__log(_DEBUG, SERVER_NAME, "OnLobbyClientAddGQLotteryTicketReqMsg: send DISPATCH_SVR_LOBBYCLIENT_ADD_GQ_LOTTERYTICKET_REQ to DispatchServer success ! UserID[%d], SiteID[%d]", pAddLotteryTicket->nUserID, pAddLotteryTicket->nSiteID);
	} 
	else
	{
		__log(_ERROR, SERVER_NAME, "OnLobbyClientAddGQLotteryTicketReqMsg: send DISPATCH_SVR_LOBBYCLIENT_ADD_GQ_LOTTERYTICKET_REQ to DispatchServer failed ! UserID[%d], SiteID[%d]", pAddLotteryTicket->nUserID, pAddLotteryTicket->nSiteID);
	}
}

void LobbyServer::OnLobbyClientIsCaiPiaoActivityAccountReqMsg(IPlayerNode *lpPlayerNode, void *buffer, long length)
{
	if (sizeof(tagLobbyClientIsCaiPiaoActivityAccountReq) != length)
	{
		__log(_ERROR, SERVER_NAME, "OnLobbyClientIsCaiPiaoActivityAccountReqMsg: length[%d] not match, expect[%d] !", length, sizeof(tagLobbyClientIsCaiPiaoActivityAccountReq));
		return;
	}

	LobbyClientNodeDef *pClientNode = (LobbyClientNodeDef *)lpPlayerNode;

	tagLobbyClientIsCaiPiaoActivityAccountReq *pIsCPAccount = (tagLobbyClientIsCaiPiaoActivityAccountReq*)buffer;

	tagDSIsCaiPiaoActivityAccountReq CPAccount = {0};

	CPAccount.nUserID = pIsCPAccount->nUserID;
	CPAccount.nSiteID = pIsCPAccount->nSiteID;

	CPAccount.ulPlayerNode = (unsigned long)pClientNode;
	CPAccount.ulPlayerNodeId = *pClientNode;

	if (true == GameProvider::SendData(m_lpDispatchServer, DISPATCH_SVR_LOBBYCLIENT_IS_CAIPIAO_ACTIVITY_ACCOUNT_REQ, &CPAccount, sizeof(CPAccount)))
	{
		__log(_DEBUG, SERVER_NAME, "OnLobbyClientIsCaiPiaoActivityAccountReqMsg: send DISPATCH_SVR_LOBBYCLIENT_IS_CAIPIAO_ACTIVITY_ACCOUNT_REQ to DispatchServer success ! UserID[%d], SiteID[%d]", pIsCPAccount->nUserID, pIsCPAccount->nSiteID);
	} 
	else
	{
		__log(_ERROR, SERVER_NAME, "OnLobbyClientIsCaiPiaoActivityAccountReqMsg: send DISPATCH_SVR_LOBBYCLIENT_IS_CAIPIAO_ACTIVITY_ACCOUNT_REQ to DispatchServer failed ! UserID[%d], SiteID[%d]", pIsCPAccount->nUserID, pIsCPAccount->nSiteID);
	}
}

void LobbyServer::OnLobbyClientRefreshGameCoinInfoReqMsg(IPlayerNode *lpPlayerNode, void *buffer, long length)
{
	if (sizeof(tagLobbyRefreshGameCoinInfoReq) != length)
	{
		__log(_ERROR, SERVER_NAME, "OnLobbyClientRefreshGameCoinInfoReqMsg: length[%d] not match, expect[%d] !", length, sizeof(tagLobbyRefreshGameCoinInfoReq));
		return;
	}

	/*
    // old
	LobbyClientNodeDef *pClientNode = (LobbyClientNodeDef *)lpPlayerNode;

	tagLobbyRefreshGameCoinInfoReq *pRefreshCoinInfo = (tagLobbyRefreshGameCoinInfoReq*)buffer;

	tagDSRefreshGameCoinInfoReq RefreshCoinInfo = {0};

	RefreshCoinInfo.nUserID = pRefreshCoinInfo->nUserID;

	RefreshCoinInfo.ulPlayerNode = (unsigned long)pClientNode;
	RefreshCoinInfo.ulPlayerNodeId = *pClientNode;

	if (true == GameProvider::SendData(m_lpDispatchServer, DISPATCH_SVR_LOBBYCLIENT_REFRESH_GAMECOIN_INFO_REQ, &RefreshCoinInfo, sizeof(RefreshCoinInfo)))
	{
		__log(_DEBUG, SERVER_NAME, "OnLobbyClientRefreshGameCoinInfoReqMsg: send DISPATCH_SVR_LOBBYCLIENT_REFRESH_GAMECOIN_INFO_REQ to DispatchServer success ! UserID[%d]", pRefreshCoinInfo->nUserID);
	} 
	else
	{
		__log(_ERROR, SERVER_NAME, "OnLobbyClientRefreshGameCoinInfoReqMsg: send DISPATCH_SVR_LOBBYCLIENT_REFRESH_GAMECOIN_INFO_REQ to DispatchServer failed ! UserID[%d]", pRefreshCoinInfo->nUserID);
	}
	//*/

    // 同步模式
    /*
    tagDSRefreshGameCoinInfoRsp GameCoinInfo = {0};
    GameCoinInfo.nUserID = pRefreshCoinInfo->nUserID;
    GameCoinInfo.ulPlayerNode = pRefreshCoinInfo->ulPlayerNode;
    GameCoinInfo.ulPlayerNodeId = pRefreshCoinInfo->ulPlayerNodeId;
    GameCoinInfo.lErrorCode = m_db.SPRefreshGameCoinInfo( &RefreshCoinInfo, GameCoinInfo );
    __log( _DEBUG, SERVER_NAME, "OnLobbyClientRefreshGameCoinInfoReqMsg UserID[%d] DBRet[%d]", pRefreshCoinInfo->nUserID, GameCoinInfo.lErrorCode );
    OnDispatchServerLobbyRefreshGameCoinInfoRspMsg( lpPlayerNode, (void*)&GameCoinInfo, sizeof(tagDSRefreshGameCoinInfoRsp) );
    //*/

    // 线程模式
	LobbyClientNodeDef *pClientNode = (LobbyClientNodeDef *)lpPlayerNode;
	tagLobbyRefreshGameCoinInfoReq *pRefreshCoinInfo = (tagLobbyRefreshGameCoinInfoReq*)buffer;

    printf( "OnLobbyClientRefreshGameCoinInfoReqMsg, userid=%d\n", pRefreshCoinInfo->nUserID);

	tagDSRefreshGameCoinInfoReq *RefreshCoinInfo = new tagDSRefreshGameCoinInfoReq();
    if( !RefreshCoinInfo )
    {
		__log( _ERROR, SERVER_NAME, "OnLobbyClientRefreshGameCoinInfoReqMsg new obj failed!" );
        return;
    }
	RefreshCoinInfo->nUserID = pRefreshCoinInfo->nUserID;
	RefreshCoinInfo->ulPlayerNode = (unsigned long)pClientNode;
	RefreshCoinInfo->ulPlayerNodeId = *pClientNode;
    m_dbWorker->PushWorkData( E_DBType_RefreshGameCoin_Req, sizeof(tagDSRefreshGameCoinInfoReq), RefreshCoinInfo );
}

void LobbyServer::OnLobbyClientOldPlayerGetCaiPiaoReqMsg(IPlayerNode *lpPlayerNode, void *buffer, long length)
{
	if (sizeof(tagLobbyOldPlayerGetCaiPiaoReq) != length)
	{
		__log(_ERROR, SERVER_NAME, "OnLobbyClientOldPlayerGetCaiPiaoReqMsg: length[%d] not match, expect[%d] !", length, sizeof(tagLobbyOldPlayerGetCaiPiaoReq));
		return;
	}

	LobbyClientNodeDef *pClientNode = (LobbyClientNodeDef *)lpPlayerNode;

	tagLobbyOldPlayerGetCaiPiaoReq *pOldPlayerGetCaiPiao = (tagLobbyOldPlayerGetCaiPiaoReq*)buffer;

	tagDSOldPlayerGetCaiPiaoReq OldPlayerGetCaiPiao = {0};

	OldPlayerGetCaiPiao.nUserID = pOldPlayerGetCaiPiao->nUserID;
	OldPlayerGetCaiPiao.nSiteID = pOldPlayerGetCaiPiao->nSiteID;

	OldPlayerGetCaiPiao.ulPlayerNode = (unsigned long)pClientNode;
	OldPlayerGetCaiPiao.ulPlayerNodeId = *pClientNode;

	if (true == GameProvider::SendData(m_lpDispatchServer, DISPATCH_SVR_LOBBYCLIENT_OLDPLAYER_GET_CAIPIAO_REQ, &OldPlayerGetCaiPiao, sizeof(OldPlayerGetCaiPiao)))
	{
		__log(_DEBUG, SERVER_NAME, "OnLobbyClientOldPlayerGetCaiPiaoReqMsg: send DISPATCH_SVR_LOBBYCLIENT_OLDPLAYER_GET_CAIPIAO_REQ to DispatchServer success ! UserID[%d], SiteID[%d]", pOldPlayerGetCaiPiao->nUserID, pOldPlayerGetCaiPiao->nSiteID);
	} 
	else
	{
		__log(_ERROR, SERVER_NAME, "OnLobbyClientOldPlayerGetCaiPiaoReqMsg: send DISPATCH_SVR_LOBBYCLIENT_OLDPLAYER_GET_CAIPIAO_REQ to DispatchServer failed ! UserID[%d], SiteID[%d]", pOldPlayerGetCaiPiao->nUserID, pOldPlayerGetCaiPiao->nSiteID);
	}
}

void LobbyServer::OnLobbyClientSendPlayerMessagesReqMsg (IPlayerNode *lpPlayerNode, void * buffer, long length)
{
	if (sizeof(tagLobbySendMessageReq) != length)
	{
		__log(_ERROR, SERVER_NAME, "OnLobbyClientSendPlayerMessagesReqMsg: length[%d] not match, expect[%d] !", length, sizeof(tagLobbySendMessageReq));
		return;
	}
	LobbyClientNodeDef *pClientNode = (LobbyClientNodeDef *)lpPlayerNode;
	tagLobbySendMessageReq * sendMessage = (tagLobbySendMessageReq *) buffer; 
	LobbySendMessageReqData_Trans transData = {0};
	transData.nGameID = sendMessage->nGameID;
	transData.nPlatformID = sendMessage->nPlatformID;
	transData.nSiteID = sendMessage->nSiteID;
	transData.nUserID = sendMessage->nUserID;
	transData.nMessageType = 0; //player message
	strcpy (transData.szAccount, sendMessage->szAccount);
	strcpy (transData.szContent, sendMessage->szContent);
	strcpy (transData.szNickname, sendMessage->szNickname);

	transData.ulPlayerNode = (unsigned long)pClientNode;
	transData.ulPlayerNodeId = *pClientNode;

	if (true == GameProvider::SendData(m_lpDispatchServer, DISPATCH_SVR_LOBBYCLIENT_SEND_PLAYER_MESSAGE_REQ, &transData, sizeof(LobbySendMessageReqData_Trans)))
	{
		__log(_DEBUG, SERVER_NAME, "OnLobbyClientSendPlayerMessagesReqMsg: send DISPATCH_SVR_LOBBYCLIENT_SEND_PLAYER_MESSAGE_REQ to DispatchServer success ! UserID[%d], SiteID[%d], NickName[%s]", sendMessage->nUserID, sendMessage->nSiteID, sendMessage->szNickname);
	} 
	else
	{
		__log(_ERROR, SERVER_NAME, "OnLobbyClientSendPlayerMessagesReqMsg: send DISPATCH_SVR_LOBBYCLIENT_SEND_PLAYER_MESSAGE_REQ to DispatchServer failed ! UserID[%d], SiteID[%d], NickName[%s]", sendMessage->nUserID, sendMessage->nSiteID, sendMessage->szNickname);
	}
}


void LobbyServer::OnLobbyClientBuyItemsReqMsg (IPlayerNode *lpPlayerNode, void * buffer, long length)
{
	if (sizeof(LobbyPlayerBuyItemsReqData) != length)
	{
		__log(_ERROR, SERVER_NAME, "OnLobbyClientBuyItemsReqMsg: length[%d] not match, expect[%d] !", length, sizeof(LobbyPlayerBuyItemsReqData));
		return;
	}

	LobbyClientNodeDef *pClientNode = (LobbyClientNodeDef *)lpPlayerNode;
	LobbyPlayerBuyItemsReqData * itemBuyMessage = (LobbyPlayerBuyItemsReqData*)buffer;
	LobbyPlayerBuyItemsReq_trans itemBuyMessageTrans = {0};
	itemBuyMessageTrans.nGameID = itemBuyMessage->nGameID;
	itemBuyMessageTrans.nItemsID = itemBuyMessage->nItemsID;
	itemBuyMessageTrans.nUserID = itemBuyMessage->nUserID;
	itemBuyMessageTrans.nItemsCounts = itemBuyMessage->nItemsCounts;
	itemBuyMessageTrans.nPlatformID = itemBuyMessage->nPlatformID;
	itemBuyMessageTrans.nBuyType = itemBuyMessage->nBuyType;
	itemBuyMessageTrans.ulPlayerNode = (unsigned long)pClientNode;
	itemBuyMessageTrans.ulPlayerNodeId = *pClientNode;

	if (true == GameProvider::SendData(m_lpDispatchServer, DISPATCH_SVR_LOBBYCLIENT_BUY_ITEMS_REQ, &itemBuyMessageTrans, sizeof(LobbyPlayerBuyItemsReq_trans)))
	{
		__log(_DEBUG, SERVER_NAME, "OnLobbyClientBuyItemsReqMsg: send DISPATCH_SVR_LOBBYCLIENT_BUY_ITEMS_REQ to DispatchServer success ! nUserID[%d], nItemID[%d], nGameID[%d], nItemsCounts[%d]", itemBuyMessageTrans.nUserID, itemBuyMessageTrans.nItemsID, itemBuyMessageTrans.nGameID, itemBuyMessageTrans.nItemsCounts);
	} 
	else
	{
		__log(_ERROR, SERVER_NAME, "OnLobbyClientBuyItemsReqMsg: send DISPATCH_SVR_LOBBYCLIENT_BUY_ITEMS_REQ to DispatchServer failed ! nUserID[%d], nItemID[%d], nGameID[%d], nItemsCounts[%d]", itemBuyMessageTrans.nUserID, itemBuyMessageTrans.nItemsID, itemBuyMessageTrans.nGameID, itemBuyMessageTrans.nItemsCounts);
	}
}


void LobbyServer::OnLobbyClientGetAnniversarySendCoinTimes (IPlayerNode *lpPlayerNode, void * buffer, long length)
{
	if (sizeof(tagLobbyGetAnniversarySendCoinTimesReq) != length)
	{
		__log(_ERROR, SERVER_NAME, "OnLobbyClientGetAnniversarySendCoinTimes: length[%d] not match, expect[%d] !", length, sizeof(tagLobbyGetAnniversarySendCoinTimesReq));
		return;
	}

	LobbyClientNodeDef *pClientNode = (LobbyClientNodeDef *)lpPlayerNode;
	tagLobbyGetAnniversarySendCoinTimesReq * sendTimesReq = (tagLobbyGetAnniversarySendCoinTimesReq*)buffer;
	tagLobbyGetAnniversarySendTimes_trans sendTimesReqTrans = {0};

	sendTimesReqTrans.nGameID = sendTimesReq->nGameID;
	sendTimesReqTrans.nUserID = sendTimesReq->nUserID;
	sendTimesReqTrans.nAddScore = sendTimesReq->nAddScore;
	sendTimesReqTrans.ulPlayerNode = (unsigned long)pClientNode;
	sendTimesReqTrans.ulPlayerNodeId = *pClientNode;

	if (true == GameProvider::SendData(m_lpDispatchServer, DISPATCH_SVR_LOBBYCLIENT_GET_ANNIVERSARY_SENDCOIN_TIMES_REQ, &sendTimesReqTrans, sizeof(tagLobbyGetAnniversarySendTimes_trans)))
	{
		__log(_DEBUG, SERVER_NAME, "OnLobbyClientBuyItemsReqMsg: send DISPATCH_SVR_LOBBYCLIENT_GET_ANNIVERSARY_SENDCOIN_TIMES_REQ to DispatchServer success ! nUserID[%d], nGameID[%d], addScores[%d]", sendTimesReqTrans.nUserID, sendTimesReqTrans.nGameID, sendTimesReqTrans.nAddScore);
	} 
	else
	{
		__log(_ERROR, SERVER_NAME, "OnLobbyClientBuyItemsReqMsg: send DISPATCH_SVR_LOBBYCLIENT_GET_ANNIVERSARY_SENDCOIN_TIMES_REQ to DispatchServer failed ! nUserID[%d], nGameID[%d], addScores[%d]", sendTimesReqTrans.nUserID, sendTimesReqTrans.nGameID, sendTimesReqTrans.nAddScore);
	}
}


void LobbyServer::OnLobbyClientGetSingleGameOnlineCountsReqMsg (IPlayerNode *lpPlayerNode, void * buffer, long length)
{
	if (sizeof(tagLobbyGetSingleGameOnlineCountReq) != length)
	{
		__log(_ERROR, SERVER_NAME, "OnLobbyClientGetSingleGameOnlineCountsReqMsg: length[%d] not match, expect[%d] !", length, sizeof(tagLobbyGetSingleGameOnlineCountReq));
		return;
	}

	LobbyClientNodeDef *pClientNode = (LobbyClientNodeDef *)lpPlayerNode;
	tagLobbyGetSingleGameOnlineCountReq * sendTimesReq = (tagLobbyGetSingleGameOnlineCountReq*)buffer;
	tagLobbyGetSingleOnlineCountsReq_trans GetGameOnline = {0};

	GetGameOnline.nGameID = sendTimesReq->nGameID;
	GetGameOnline.ulPlayerNode = (unsigned long)pClientNode;
	GetGameOnline.ulPlayerNodeId = *pClientNode;

	if (true == GameProvider::SendData(m_lpDispatchServer, DISPATCH_SVR_LOBBYCLIENT_GETSINGLE_ONLINECOUNTS_REQ, &GetGameOnline, sizeof(tagLobbyGetSingleOnlineCountsReq_trans)))
	{
		__log(_DEBUG, SERVER_NAME, "OnLobbyClientGetSingleGameOnlineCountsReqMsg: send DISPATCH_SVR_LOBBYCLIENT_GETSINGLE_ONLINECOUNTS_REQ to DispatchServer success ! nGameID[%d]", GetGameOnline.nGameID);
	} 
	else
	{
		__log(_ERROR, SERVER_NAME, "OnLobbyClientGetSingleGameOnlineCountsReqMsg: send DISPATCH_SVR_LOBBYCLIENT_GETSINGLE_ONLINECOUNTS_REQ to DispatchServer failed ! nGameID[%d]", GetGameOnline.nGameID);
	}
}

void LobbyServer::OnLobbyClientSetHeadImageReqMsg (IPlayerNode * lpPlayerNode, void * buffer, long length)
{
	if (sizeof (LobbySetUserHeadImageReqData) != length)
	{
		__log(_ERROR, SERVER_NAME, "OnLobbyClientSetHeadImageReqMsg: length[%d] not match, expect[%d] !", length, sizeof(LobbySetUserHeadImageReqData));
		return;
	}

	LobbyClientNodeDef *pClientNode = (LobbyClientNodeDef *)lpPlayerNode;
	LobbySetUserHeadImageReqData * setImageReq = (LobbySetUserHeadImageReqData*)buffer;
	LobbySetUserHeadImageReqData_Trans setImageReqData_Trans = {0};

	setImageReqData_Trans.nGameID = setImageReq->nGameID;
	setImageReqData_Trans.nSiteID = setImageReq->nSiteID;
	setImageReqData_Trans.nUserID = setImageReq->nUserID;
	setImageReqData_Trans.nSettingHeadImageIndex = setImageReq->nSettingHeadImageIndex;
	setImageReqData_Trans.ulPlayerNode = (unsigned long)pClientNode;
	setImageReqData_Trans.ulPlayerNodeId = *pClientNode;

	if (true == GameProvider::SendData(m_lpDispatchServer, DISPATCH_SVR_LOBBYCLIENT_SET_HEAD_IMAGE_REQ, &setImageReqData_Trans, sizeof(LobbySetUserHeadImageReqData_Trans)))
		__log(_DEBUG, SERVER_NAME, "OnLobbyClientSetHeadImageReqMsg: send DISPATCH_SVR_LOBBYCLIENT_SET_HEAD_IMAGE_REQ to DispatchServer success ! nGameID[%d]", setImageReqData_Trans.nGameID);
	else
		__log(_ERROR, SERVER_NAME, "OnLobbyClientSetHeadImageReqMsg: send DISPATCH_SVR_LOBBYCLIENT_SET_HEAD_IMAGE_REQ to DispatchServer failed ! nGameID[%d]", setImageReqData_Trans.nGameID);
}

void LobbyServer::OnLobbyClientBuyHeadImageReqMsg (IPlayerNode * lpPlayerNode, void * buffer, long length)
{
	if (sizeof (LobbyBuyUserHeadImageReqData) != length)
	{
		__log(_ERROR, SERVER_NAME, "OnLobbyClientBuyHeadImageReqMsg: length[%d] not match, expect[%d] !", length, sizeof(LobbyBuyUserHeadImageReqData));
		return;
	}

	LobbyClientNodeDef *pClientNode = (LobbyClientNodeDef *)lpPlayerNode;
	LobbyBuyUserHeadImageReqData * buyImageReq = (LobbyBuyUserHeadImageReqData*)buffer;
	LobbyBuyUserHeadImageReqData_Trans buyImageReqData_Trans = {0};

	buyImageReqData_Trans.nGameID = buyImageReq->nGameID;
	buyImageReqData_Trans.nSiteID = buyImageReq->nSiteID;
	buyImageReqData_Trans.nUserID = buyImageReq->nUserID;
	buyImageReqData_Trans.nBuyHeadImageIndex = buyImageReq->nBuyHeadImageIndex;
	buyImageReqData_Trans.nCost = buyImageReq->nCost;
	buyImageReqData_Trans.nPlatformID = buyImageReq->nPlatformID;
	buyImageReqData_Trans.ulPlayerNode = (unsigned long)pClientNode;
	buyImageReqData_Trans.ulPlayerNodeId = *pClientNode;

	if (true == GameProvider::SendData(m_lpDispatchServer, DISPATCH_SVR_LOBBYCLIENT_BUY_HEAD_IMAGE_REQ, &buyImageReqData_Trans, sizeof(LobbyBuyUserHeadImageReqData_Trans)))
		__log(_DEBUG, SERVER_NAME, "OnLobbyClientBuyHeadImageReqMsg: send DISPATCH_SVR_LOBBYCLIENT_BUY_HEAD_IMAGE_REQ to DispatchServer success ! nGameID[%d]", buyImageReqData_Trans.nGameID);
	else
		__log(_ERROR, SERVER_NAME, "OnLobbyClientBuyHeadImageReqMsg: send DISPATCH_SVR_LOBBYCLIENT_BUY_HEAD_IMAGE_REQ to DispatchServer failed ! nGameID[%d]", buyImageReqData_Trans.nGameID);
}


void LobbyServer::OnLobbyClientSendGetItemsInfoReqMsg (IPlayerNode * lpPlayerNode, void * buffer, long length)
{
	if (sizeof (tagLobbyPlayerItemsInfoReqData) != length)
	{
		__log(_ERROR, SERVER_NAME, "OnLobbyClientSendGetItemsInfoReqMsg: length[%d] not match, expect[%d] !", length, sizeof(tagLobbyPlayerItemsInfoReqData));
		return;
	}

	LobbyClientNodeDef *pClientNode = (LobbyClientNodeDef *)lpPlayerNode;
	tagLobbyPlayerItemsInfoReqData * itemsInfo = (tagLobbyPlayerItemsInfoReqData*)buffer;
	tagLobbyPlayerItemsInfoReq_Trans itemsInfo_Trans = {0};
	itemsInfo_Trans.nUserID = itemsInfo->nUserID;
	itemsInfo_Trans.ulPlayerNode = (unsigned long)pClientNode;
	itemsInfo_Trans.ulPlayerNodeId = *pClientNode;

	if (true == GameProvider::SendData(m_lpDispatchServer, DISPATCH_SVR_LOBBYCLIENT_GET_ITEMS_INFO_REQ, &itemsInfo_Trans, sizeof(tagLobbyPlayerItemsInfoReq_Trans)))
		__log(_DEBUG, SERVER_NAME, "OnLobbyClientSendGetItemsInfoReqMsg: send DISPATCH_SVR_LOBBYCLIENT_GET_ITEMS_INFO_REQ to DispatchServer success ! nGameID[%d]", itemsInfo_Trans.nUserID);
	else
		__log(_ERROR, SERVER_NAME, "OnLobbyClientSendGetItemsInfoReqMsg: send DISPATCH_SVR_LOBBYCLIENT_GET_ITEMS_INFO_REQ to DispatchServer failed ! nGameID[%d]", itemsInfo_Trans.nUserID);
}

void LobbyServer::OnLobbyClientGetOnSaleItemsInfoReqMsg (IPlayerNode * lpPlayerNode, void * buffer, long length)
{
	if (sizeof (LobbyOnSaleItemsInfoReqData)  != length)
	{
		__log(_ERROR, SERVER_NAME, "OnLobbyClientGetOnSaleItemsInfoReqMsg: length[%d] not match, expect[%d] !", length, sizeof(LobbyOnSaleItemsInfoReqData));
		return;
	}

	LobbyClientNodeDef *pClientNode = (LobbyClientNodeDef *)lpPlayerNode;
	LobbyOnSaleItemsInfoReqData * itemsInfo = (LobbyOnSaleItemsInfoReqData*)buffer;
	tagLobbyOnSaleItemsInfoReq_Trans itemsInfo_Trans = {0};
	itemsInfo_Trans.nUserID = itemsInfo->nUserID;
	itemsInfo_Trans.ulPlayerNode = (unsigned long)pClientNode;
	itemsInfo_Trans.ulPlayerNodeId = *pClientNode;

	if (true == GameProvider::SendData(m_lpDispatchServer, DISPATCH_SVR_LOBBYCLIENT_GET_ONSALE_INFO_REQ, &itemsInfo_Trans, sizeof(tagLobbyOnSaleItemsInfoReq_Trans)))
		__log(_DEBUG, SERVER_NAME, "OnLobbyClientGetOnSaleItemsInfoReqMsg: send DISPATCH_SVR_LOBBYCLIENT_GET_ONSALE_INFO_REQ to DispatchServer success ! nGameID[%d]", itemsInfo_Trans.nUserID);
	else
		__log(_ERROR, SERVER_NAME, "OnLobbyClientGetOnSaleItemsInfoReqMsg: send DISPATCH_SVR_LOBBYCLIENT_GET_ONSALE_INFO_REQ to DispatchServer failed ! nGameID[%d]", itemsInfo_Trans.nUserID);
}


void LobbyServer::OnLobbyClientUserInfoRegisterReqMsg (IPlayerNode * lpPlayerNode, void * buffer, long length)
{
}

void LobbyServer::OnLobbyClientBroadCastPlayerMessageReqMsg (IPlayerNode * lpPlayerNode, void * buffer, long length)
{
	if (sizeof (LobbyPlayerBroadCastMsgData) != length)
	{
		__log(_ERROR, SERVER_NAME, "OnLobbyClientBroadCastPlayerMessageReqMsg: length[%d] not match, expect[%d] !", length, sizeof(LobbyPlayerBroadCastMsgData));
		return;
	}

	LobbyClientNodeDef *pClientNode = (LobbyClientNodeDef *)lpPlayerNode;
	LobbyPlayerBroadCastMsgData * itemsInfo = (LobbyPlayerBroadCastMsgData*)buffer;
	LobbyPlayerBroadCastMsgData_Trans itemsInfo_Trans = {0};
	itemsInfo_Trans.nUserID = itemsInfo->nUserID;
	itemsInfo_Trans.nStart = itemsInfo->nStart;
	itemsInfo_Trans.nCounts = itemsInfo->nCounts;
	itemsInfo_Trans.ulPlayerNode = (unsigned long)pClientNode;
	itemsInfo_Trans.ulPlayerNodeId = *pClientNode;

	if (true == GameProvider::SendData(m_lpDispatchServer, DISPATCH_SVR_LOBBYCLIENT_BROADCAST_PLAYER_MESSAGE_REQ, &itemsInfo_Trans, sizeof(LobbyPlayerBroadCastMsgData_Trans)))
		__log(_DEBUG, SERVER_NAME, "OnLobbyClientBroadCastPlayerMessageReqMsg: send DISPATCH_SVR_LOBBYCLIENT_GET_ONSALE_INFO_REQ to DispatchServer success ! nGameID[%d]", itemsInfo_Trans.nUserID);
	else
		__log(_ERROR, SERVER_NAME, "OnLobbyClientBroadCastPlayerMessageReqMsg: send DISPATCH_SVR_LOBBYCLIENT_GET_ONSALE_INFO_REQ to DispatchServer failed ! nGameID[%d]", itemsInfo_Trans.nUserID);
}

void LobbyServer::OnLobbyClientGetUserItemsOutOfDateMessageReqMsg (IPlayerNode * lpPlayerNode, void * buffer, long length)
{
	if (sizeof (LobbyGetUserItemsOutOfDateReqData) != length)
	{
		__log(_ERROR, SERVER_NAME, "OnLobbyClientGetUserItemsOutOfDateMessageReqMsg: length[%d] not match, expect[%d] !", length, sizeof(LobbyGetUserItemsOutOfDateReqData));
		return;
	}

	LobbyClientNodeDef *pClientNode = (LobbyClientNodeDef *)lpPlayerNode;
	LobbyGetUserItemsOutOfDateReqData * reqInfo = (LobbyGetUserItemsOutOfDateReqData*)buffer;
	LobbyGetUserItemsOutOfDateReqData_Trans reqInfo_Trans = {0};
	reqInfo_Trans.nUserID = reqInfo->nUserID;
	reqInfo_Trans.ulPlayerNode = (unsigned long)pClientNode;
	reqInfo_Trans.ulPlayerNodeId = *pClientNode;

	if (true == GameProvider::SendData(m_lpDispatchServer, DISPATCH_SVR_LOBBYCLIENT_GET_ITEMS_OUTOFDATE_REQ, &reqInfo_Trans, sizeof(LobbyGetUserItemsOutOfDateReqData_Trans)))
		__log(_DEBUG, SERVER_NAME, "OnLobbyClientBroadCastPlayerMessageReqMsg: send DISPATCH_SVR_LOBBYCLIENT_GET_ITEMS_OUTOFDATE_REQ to DispatchServer success ! nUserID[%d]", reqInfo_Trans.nUserID);
	else
		__log(_ERROR, SERVER_NAME, "OnLobbyClientBroadCastPlayerMessageReqMsg: send DISPATCH_SVR_LOBBYCLIENT_GET_ITEMS_OUTOFDATE_REQ to DispatchServer failed ! nUserID[%d]", reqInfo_Trans.nUserID);
}

void LobbyServer::RefreshGameCoinInfo(SWorkData &data)
{
    tagDSRefreshGameCoinInfoReq *RefreshCoinInfo = (tagDSRefreshGameCoinInfoReq*)data.ptr;
    if( !RefreshCoinInfo )
    {
        __log( _ERROR, SERVER_NAME, "LobbyServer::RefreshGameCoinInfo NULL ptr" );
        return;
    }

    tagDSRefreshGameCoinInfoRsp *GameCoinInfo = new tagDSRefreshGameCoinInfoRsp();
    if( !GameCoinInfo )
    {
        __log( _ERROR, SERVER_NAME, "LobbyServer::RefreshGameCoinInfo new obj failed" );
        return;
    }

    printf( "LobbyServer::RefreshGameCoinInfo UserId[%d]\n", RefreshCoinInfo->nUserID );

	GameCoinInfo->nUserID = RefreshCoinInfo->nUserID;
	GameCoinInfo->ulPlayerNode = RefreshCoinInfo->ulPlayerNode;
	GameCoinInfo->ulPlayerNodeId = RefreshCoinInfo->ulPlayerNodeId;
	GameCoinInfo->lErrorCode = m_db->SPRefreshGameCoinInfo( RefreshCoinInfo, *GameCoinInfo );
    PushDBDoneEvent( E_DBType_RefreshGameCoin_Rsp, sizeof(tagDSRefreshGameCoinInfoRsp), GameCoinInfo );
	__log( _DEBUG, SERVER_NAME, "RefreshGameCoinInfo UserID[%d] DBRet[%d]", RefreshCoinInfo->nUserID, GameCoinInfo->lErrorCode );
    delete RefreshCoinInfo;
}

void LobbyServer::ProcDBWorkData(SWorkData &data)
{
    printf( "LobbyServer::ProcDBWorkData type[%d]\n", data.type );
    switch( data.type )
    {
    case E_DBType_RefreshGameCoin_Req:
        RefreshGameCoinInfo( data );
        break;
    default:
        __log( _ERROR, SERVER_NAME, "LobbyServer::ProcDBWorkData unknown type[%d]", data.type );
        break;
    };
}

void LobbyServer::PushDBDoneEvent(int type, int len, void *evt)
{
    if( evt == NULL )
    {
        __log( _ERROR, SERVER_NAME, "PushDBDoneEvent NULL event" );
        return;
    }
    SWorkData data;
    data.type = type;
    data.len = len;
    data.ptr = evt;
    CGuardLock<CLock> g(m_queLock);
    m_dbDoneQueue.push_back( data );
}

void LobbyServer::OnProcDBWorkDone(long tmNow)
{
    int procCount = 0;
    int limitCount = 50;
    while( true )
    {
        SWorkData data = {0};
        {
            CGuardLock<CLock> g(m_queLock);
            if( !m_dbDoneQueue.empty() )
            {
                data = m_dbDoneQueue.front();
                m_dbDoneQueue.pop_front();
            }
            else
            {
                break;
            }
        }
        if( data.ptr == NULL )
        {
		    __log( _ERROR, SERVER_NAME, "OnProcRefreshDoneEvent get NULL event" );
            continue;
        }
        DBWorkDone( data );
        ++procCount;
        if( procCount >= limitCount )
        {
            break;
        }
    }

    if( procCount >= limitCount )
    {
        __log( _WARN, SERVER_NAME, "OnProcRefreshDoneEvent proc max event[%d/%d]", procCount, limitCount );
    }
}

void LobbyServer::DBWorkDone(SWorkData &data)
{
    switch( data.type )
    {
        case E_DBType_RefreshGameCoin_Rsp:
            {
                OnDispatchServerLobbyRefreshGameCoinInfoRspMsg( NULL, data.ptr, data.len );
                if( data.ptr ) 
                {
                    tagDSRefreshGameCoinInfoRsp *ptr = (tagDSRefreshGameCoinInfoRsp*)data.ptr;
                    delete ptr;
                }
            }
            break;
        default:
            break;
    };
}

