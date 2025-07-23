#include "DBWorker.h"
#include "LobbyServer.h"

using namespace std;
using namespace SGLib;

CDBWorker::CDBWorker(LobbyServer &server) : 
    m_lobbyServer(server),
    m_stop(false)
{
}

CDBWorker::~CDBWorker()
{
    if( !m_stop )
    {
        Stop();
    }
}

void CDBWorker::Run()
{
    printf( "CDBWorker::Run\n" );
    __log( _DEBUG, SERVER_NAME, "CDBWorker Start!" );
    while( !m_stop )
    {
        m_workEvt.Wait();
        SWorkData data = {0};
        bool bget = false;
        {
            CGuardLock<CLock> g(m_queLock);
            if( !m_dataQueue.empty() )
            {
                data = m_dataQueue.front();
                m_dataQueue.pop_front();
                bget = true;
            }
        }
        if( bget )
        {
            m_lobbyServer.ProcDBWorkData( data );
        }
    }

    int remainCount = 0;
    {
        CGuardLock<CLock> g(m_queLock);
        remainCount = m_dataQueue.size();
    }
    __log( _WARN, SERVER_NAME, "CDBWorker stoped! Unprocess work data=%d", remainCount );
    printf( "CDBWorker::End\n" );
}

bool CDBWorker::Start()
{
    m_thread = new CThread( this );
    return ((m_thread!=NULL) && m_thread->Start());
}

void CDBWorker::Stop()
{
    m_stop = true;
    m_workEvt.SetEvent();
    m_thread->Stop();
}

void CDBWorker::PushWorkData(int type, int len, void *data)
{
    if( data == NULL )
    {
        __log(_ERROR, SERVER_NAME, "CDBWorker::PushWorkData NULL data !");
        return;
    }
    SWorkData wd;
    wd.type = type;
    wd.len = len;
    wd.ptr = data;
    CGuardLock<CLock> g(m_queLock);
    m_dataQueue.push_back( wd );
    m_workEvt.SetEvent();
}


