#include "Lock.h"
using namespace SGLib;

#ifdef _USE_WIN32
CFastLock::CFastLock() : m_Lock(0)
{}

CFastLock::~CFastLock()
{
    m_Lock = 0;
}

void CFastLock::Lock()
{
    while( ::InterlockedCompareExchange( &m_Lock, 1, 0 ) != 0 )
    {
        ::Sleep( 0 );
    }
}

void CFastLock::UnLock() 
{
    ::InterlockedExchange( &m_Lock, 0 );
}

bool CFastLock::TryLock() 
{
    if( ::InterlockedCompareExchange( &m_Lock, 1, 0 ) == 0 )
    {
        return true;
    }
    return false;
}

#endif

////////////////////////////////////////////////////////////////////

CLock::CLock()
{
#ifdef _USE_WIN32
    InitializeCriticalSection( &m_Lock );
#else
    pthread_mutexattr_t attr;
    s32 nRet = pthread_mutexattr_init( &attr );
    __ASSERT( nRet == 0 );
    nRet = pthread_mutexattr_settype( &attr, PTHREAD_MUTEX_RECURSIVE_NP );
    __ASSERT( nRet == 0 );
    nRet = pthread_mutex_init( &m_Lock, &attr );
    __ASSERT( nRet == 0 );
#endif
}

CLock::~CLock()
{
#ifdef _USE_WIN32
    DeleteCriticalSection( &m_Lock );
#else
    pthread_mutex_destroy( &m_Lock );
#endif
}

void CLock::Lock()
{
#ifdef _USE_WIN32
    EnterCriticalSection( &m_Lock );
#else
    pthread_mutex_lock( &m_Lock );
#endif
}

void CLock::UnLock()
{
#ifdef _USE_WIN32
    LeaveCriticalSection( &m_Lock );
#else
    pthread_mutex_unlock( &m_Lock );
#endif
}

bool CLock::TryLock()
{
#ifdef _USE_WIN32
    return (TryEnterCriticalSection( &m_Lock ) == TRUE);
#else
    return (pthread_mutex_trylock( &m_Lock ) == 0);
#endif
}

