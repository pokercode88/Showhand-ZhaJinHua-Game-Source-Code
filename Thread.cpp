#include "Thread.h"
using namespace SGLib;

u32 CBasicThreadOps::_SuspendThread( CThread *pThread )
{
#ifdef _USE_WIN32
    return (u32)SuspendThread( pThread->m_hHandle );
#else
    return 0;
#endif
}
        
u32 CBasicThreadOps::_ResumeThread( CThread *pThread )
{
#ifdef _USE_WIN32
    return (u32)ResumeThread( pThread->m_hHandle );
#else
    return 0;
#endif
}

bool CBasicThreadOps::_TerminateThread( CThread *pThread, u32 exitCode )
{
#ifdef _USE_WIN32
    return (TerminateThread( pThread->m_hHandle, exitCode )==TRUE);
#else
    return 0;
#endif
}

void CBasicThreadOps::_Sleep(u32 uMilliSec)
{
#ifdef _USE_WIN32
    Sleep( uMilliSec );
#else
    usleep( 1000 * uMilliSec );
#endif
}

u64	CBasicThreadOps::_GetThreadId()
{
#ifdef _USE_WIN32
	return (u64)GetCurrentThreadId();
#else
	return (u64)pthread_self();
#endif
}

///////////////////////////////////////////////////////////////////////////////

// auto_run param is nouse for linux
CThread::CThread( IRunnable *task, bool auto_run ) :
    m_pTask( task )
#ifndef _USE_WIN32
  , m_StartLock()
#endif
  , m_uiThreadId( 0 )
  , m_bRun( false )
{
    if( m_pTask )
    {
#ifdef _USE_WIN32
        m_hHandle = (HANDLE)_beginthreadex(
            NULL, 
            0, 
            ThreadFunc, 
            (void*)this, 
            CREATE_SUSPENDED, 
            &m_uiThreadId );

        if( NULL == m_hHandle )
        {
            m_hHandle = INVALID_HANDLE_VALUE;

            SGDEBUG( "_beginthreadex failed, err=%u\n", (u32)GetLastError() );
        }
        else
        {
            if( true == auto_run )
            {
                m_bRun = true;
                CBasicThreadOps::_ResumeThread( this );
                CBasicThreadOps::_Sleep( 0 );
            }
        }
#else
        m_StartLock.Lock();
        s32 nRet = pthread_create( 
                &m_Thread, 
                NULL,
                ThreadFunc,
                (void*)this );
        if( nRet == 0 )
        {
            CBasicThreadOps::_Sleep( 1 );
            m_uiThreadId = (u32)m_Thread;
			if( true == auto_run )
			{
                m_bRun = true;
				m_StartLock.UnLock();
			}
        }
        else
        {
            m_Thread = 0;
            SGDEBUG( "pthread_create failed, err=%u\n", errno );
        }
#endif
    }
}

CThread::~CThread()
{
    Stop();
}

bool CThread::Start()
{
#ifdef _USE_WIN32
    if( INVALID_HANDLE_VALUE == m_hHandle ) 
    {
        return false;
    }

    if( false == m_bRun )
    {
        m_bRun = true;
        CBasicThreadOps::_ResumeThread( this );
        Sleep( 0 );
    }
    return true;
#else
	if( m_uiThreadId == 0 )
	{
		return false;
	}

	if( false == m_bRun )
	{
		m_StartLock.UnLock();
	}
    return true;
#endif
}
 
void CThread::Stop(s32 waitTime)
{
    if( false==m_bRun || 
#ifdef _USE_WIN32
	    INVALID_HANDLE_VALUE==m_hHandle
#else
	    m_uiThreadId==0
#endif
    )
    {
        return;
    }


#ifdef _USE_WIN32
    if( m_uiThreadId != GetCurrentThreadId() )
    {
	    m_bRun = false;
	    if( WaitForSingleObject( m_hHandle, waitTime ) != WAIT_OBJECT_0 )
	    {
		    SGDEBUG( "WaitForSingleObject failed, now terminate thread\n" );
		    CBasicThreadOps::_TerminateThread( this );
	    }
	    CloseHandle( m_hHandle );
	    m_hHandle = INVALID_HANDLE_VALUE;
        m_uiThreadId = 0;
    }
#else
    m_StartLock.UnLock();
    if( !pthread_equal(m_Thread, pthread_self()) )
    {
	    m_bRun = false;
	    pthread_join( m_Thread, NULL );
	    m_Thread = 0;
        m_uiThreadId = 0;
    }
#endif
}

#ifdef _USE_WIN32
unsigned int CThread::ThreadFunc(void *pParam)
#else
void* CThread::ThreadFunc(void *pParam)
#endif
{
    CThread *pThread = (CThread*)pParam;
#ifndef _USE_WIN32
	pThread->m_StartLock.Lock();
#endif
    pThread->_Dispatch();
#ifndef _USE_WIN32
    pThread->m_StartLock.UnLock();
#endif

    return 0;
}

void CThread::_Dispatch()
{
    if( m_pTask )
    {
        try
        {
            m_pTask->Run();
        }
        catch( ... )
        {
            SGDEBUG( "task::Run failed.\n" );
        }
    }
}

///////////////////////////////////////////////////////////////

CAdvThread::CAdvThread( IRunnable *task, bool bWaitForEvent ) :
    m_pTask( task )
  , m_WorkThread( this )
  , m_bStop( true )
  , m_bWaitForEvent( bWaitForEvent )
{
#ifdef _USE_WIN32
#else
    if( m_WorkThread.IsRun() == true )
    {
    	m_bStop = false;
    }
#endif
}
        
CAdvThread::~CAdvThread()
{
    Stop();
}
      
bool CAdvThread::Start()
{
    if( m_bStop == true )
    {
    	m_bStop = false;
    	return m_WorkThread.Start();
    }
    return true;
}
 
void CAdvThread::Stop()
{
    m_bStop = true;
    WakeUp();
    m_WorkThread.Stop();
}
 
void CAdvThread::WakeUp()
{
    m_WorkEvent.SetEvent();
}
 
void CAdvThread::Run()
{
    while( 1 )
    {
        if( true == m_bWaitForEvent )
        {
            m_WorkEvent.Wait();
        }

        if( m_bStop == true )
        {
            break;
        }

        if( m_pTask )
        {
            m_pTask->Run();
        }
    }
}


