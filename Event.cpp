#include "Event.h"
using namespace SGLib;

CEvent::CEvent() 
{
#ifdef _USE_WIN32
    m_hEvent = CreateEvent( NULL, FALSE, FALSE, NULL );
#else
    pthread_mutexattr_t attr;
    s32 nRet = pthread_mutexattr_init( &attr );
    __ASSERT( nRet == 0 );
    nRet = pthread_mutexattr_settype( &attr, PTHREAD_MUTEX_RECURSIVE_NP );
    __ASSERT( nRet == 0 );
    nRet = pthread_mutex_init( &m_Mutex, &attr);
    __ASSERT( nRet == 0 );

    nRet = pthread_cond_init( &m_Cond, NULL );
    __ASSERT( nRet == 0 );
#endif
}

CEvent::~CEvent()
{
#ifdef _USE_WIN32
    if( m_hEvent )
    {
        CloseHandle( m_hEvent );
        m_hEvent = NULL;
    }
#else
    pthread_cond_destroy( &m_Cond );
    pthread_mutex_destroy( &m_Mutex );
#endif
}
 
void CEvent::Wait( s32 nWaitTime )
{
#ifdef _USE_WIN32
    if( m_hEvent )
    {
        WaitForSingleObject( m_hEvent, nWaitTime );
    }
#else
    pthread_mutex_lock( &m_Mutex );

    if( nWaitTime > 0 )
    {
        struct timeval now;
        struct timespec ts;
        gettimeofday( &now, NULL );
        ts.tv_sec = now.tv_sec + nWaitTime/1000;
        ts.tv_nsec = (now.tv_usec + nWaitTime%1000) * 1000;

        pthread_cond_timedwait( &m_Cond, &m_Mutex, &ts );
    }
    else
    {
        pthread_cond_wait( &m_Cond, &m_Mutex );
    }

    pthread_mutex_unlock( &m_Mutex );
#endif
}
        
void CEvent::SetEvent()
{
#ifdef _USE_WIN32
    if( m_hEvent )
    {
        ::SetEvent( m_hEvent );
    }
#else
    pthread_mutex_lock( &m_Mutex );
    pthread_cond_signal( &m_Cond );
    pthread_mutex_unlock( &m_Mutex );
#endif
}
 
void CEvent::ResetEvent()
{
#ifdef _USE_WIN32
    if( m_hEvent )
    {
        ::ResetEvent( m_hEvent );
    }
#else
    Wait( 1 );
#endif
}
