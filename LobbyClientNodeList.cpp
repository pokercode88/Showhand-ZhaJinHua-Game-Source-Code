#include "LobbyClientNodeList.h"
#include "LobbyClientNode.h"
#include "ServerDef.h"

bool LobbyClientNodeList::AddNodeToMap(int nUserID, unsigned long ulPlayIndex)
{
    if (nUserID <= 0) return false;

    hash_map<int, PlayerIndexDef>::iterator pos;
    pos = m_hmapPlayerNode.find(nUserID);
    if (pos != m_hmapPlayerNode.end())
    {
        __log(_ERROR, SERVER_NAME, "LobbyClientNodeList::AddNodeToMap: UserID[%d] have exsit in the PlayerList! ",nUserID);
        return false;
    }
    else
    {
        PlayerIndexDef Index;
        Index.ulPlayerNodeId = ulPlayIndex;
        m_hmapPlayerNode.insert(pair<int,PlayerIndexDef>(nUserID, Index));

        return true;
    }
}

bool LobbyClientNodeList::DelNodeFromMap(int nUserID)
{
    if (nUserID > 0)
    {
        hash_map<int, PlayerIndexDef>::iterator pos = m_hmapPlayerNode.find(nUserID);
        if (pos != m_hmapPlayerNode.end())
        {
            m_hmapPlayerNode.erase(pos);
            return true;
        }
    }
    __log(_ERROR, SERVER_NAME, "LobbyClientNodeList::DelNodeFromMap: UserID[%d] delete fail! ", nUserID);

    return false;
}

int LobbyClientNodeList::GetTotalPlayerCount()
{
	return (int)m_hmapPlayerNode.size();
}

LobbyClientNodeDef* LobbyClientNodeList::GetLobbyClientNode(int nUserID)
{
	hash_map<int, PlayerIndexDef>::iterator pos = m_hmapPlayerNode.find(nUserID);
	if (pos != m_hmapPlayerNode.end())
	{
		LobbyClientNodeDef *pClientNode = (LobbyClientNodeDef*)pos->second.ulPlayerNodeId;
		return pClientNode;
	}

	return NULL;
}

void LobbyClientNodeList::SendBatchData(GameProvider *pGameProvider, unsigned short type, void *pDataBuffer, long lDataSize)
{
	if (NULL == pGameProvider) return;

	for (hash_map<int, PlayerIndexDef>::iterator pos=m_hmapPlayerNode.begin(); pos!=m_hmapPlayerNode.end(); pos++)
	{
		LobbyClientNodeDef *pClientNode = (LobbyClientNodeDef*)pos->second.ulPlayerNodeId;
		if (NULL != pClientNode)
		{
			pGameProvider->SendData(pClientNode, type, pDataBuffer, lDataSize);
		}
	}
}

void LobbyClientNodeList::SendSiteBatchData(GameProvider *pGameProvider, int pSiteID[], unsigned short type, void *pDataBuffer, long lDataSize)
{
	if (NULL == pGameProvider) return;

	for (hash_map<int, PlayerIndexDef>::iterator pos=m_hmapPlayerNode.begin(); pos!=m_hmapPlayerNode.end(); pos++)
	{
		LobbyClientNodeDef *pClientNode = (LobbyClientNodeDef*)pos->second.ulPlayerNodeId;
		if (NULL != pClientNode)
		{
			for (int i=0; i<MAX_SITE_COUNT; i++)
			{
				if (pSiteID[i] == pClientNode->m_nSiteID)
				{
					pGameProvider->SendData(pClientNode, type, pDataBuffer, lDataSize);
					break;
				}
			}
		}
	}
}










