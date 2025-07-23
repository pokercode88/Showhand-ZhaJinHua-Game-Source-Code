#include "DBOperation.h"
#include "Common/Common.h"
#include "aes/aeslib.h"
#include <errno.h>
#include <arpa/inet.h>   //IP地址转换函数
#include <sys/socket.h>
#include <netinet/in.h>

extern CIniFile g_cfg;
const long RETRY_COUNT = 1;

#define EXEC_NONE_FLAG          0x00000000
#define EXEC_RETURN_VALUE_FLAG  0x00000001
#define EXEC_SELECT_MORE_FLAG   0x00000002

unsigned char hex2byte(unsigned char hex)
{
    const char *const lpszTable = "0123456789ABCDEF";

    return (0x0F >= hex) ? lpszTable[hex] : -1;
}

unsigned char str2byte(unsigned char szchar)
{
    unsigned char cResult = 0xFF;

    if ((0x30 <= szchar) && (0x39 >= szchar))
    {
        cResult = szchar - 0x30;
    }
    else if ((0x41 <= szchar) && (0x5A >= szchar))
    {
        cResult = szchar - 0x41 + 0x0A;
    }
    else if ((0x61 <= szchar) && (0x7A >= szchar))
    {
        cResult = szchar - 0x61 + 0x0A;
    }

    return cResult;
}

long str2byte(char *lpszStr)
{
    long lResult = -1;

    if (NULL != lpszStr)
    {
        unsigned long len = strlen(lpszStr);

        if (0 == (len % 2))
        {
            lResult = len / 2;

            for (int i = 0; i < lResult; i++)
            {
                lpszStr[i] = str2byte(lpszStr[i * 2]);
                lpszStr[i] = (lpszStr[i] << 4) | str2byte(lpszStr[i * 2 + 1]);
            }

            lpszStr[lResult] = 0;
        }
    }

    return lResult;
}

bool decrypt(char *lpszEncode, char *lpszBuffer, int len)
{
    bool bResult = false;
    if ((NULL != lpszEncode) && (NULL != lpszBuffer) && (len > 0))
    {
        long lStrLen = str2byte(lpszEncode);
        if (-1 != lStrLen)
        {
            if (0 == aes_dec_r(lpszEncode, lStrLen, "", 0, lpszBuffer, &len))
            {
                lpszBuffer[len] = 0;
                bResult = true;
            }
        }
    }
    return bResult;
}


bool ConvertTime(long tmTime, TIMESTAMP_STRUCT& stamp)
{
    tm *ptm = localtime(&tmTime);

    if (NULL != ptm)
    {
        stamp.year   = ptm->tm_year + 1900;
        stamp.month  = ptm->tm_mon + 1;
        stamp.day    = ptm->tm_mday;
        stamp.hour   = ptm->tm_hour;
        stamp.minute = ptm->tm_min;
        stamp.second = ptm->tm_sec;
    }
    return (NULL != ptm);
}

bool ConvertToSysTime(long& tmTime, TIMESTAMP_STRUCT stamp)
{
	struct tm tmTmp={0};

	tmTmp.tm_year = stamp.year-1900;
	tmTmp.tm_mon = stamp.month-1;
	tmTmp.tm_mday = stamp.day;
	tmTmp.tm_hour = stamp.hour;
	tmTmp.tm_min = stamp.minute;
	tmTmp.tm_sec = stamp.second;
	tmTime = mktime(&tmTmp);

	return true;
}

DBOperation::DBOperation()
{
    m_bOpen = false;
}

DBOperation::~DBOperation()
{
}

bool DBOperation::OpenDSN(const char *lpName, ODBCExt& db, ODBCContext& ctx)
{
    bool bResult = false;

    char szDSN[32], szUser[32], szPwd[128], szDecode[1024];

    if (0 != g_cfg.GetValueStr(lpName, "dsn", szDSN, sizeof(szDSN)))
    {
        if (0 != g_cfg.GetValueStr(lpName, "usr", szUser, sizeof(szUser)))
        {
            if (0 != g_cfg.GetValueStr(lpName, "pwd", szPwd, sizeof(szPwd)))
            {
                int len = sizeof(szDecode);
                if (true == decrypt(szPwd, szDecode, len))
                {
                    if (SQL_SUCCEEDED(db.Open(szDSN, szUser, szDecode)))
                    {
                        if (true == ctx.Initialize(&db))
                        {
                            long l = ctx.SQLExec("set ansi_warnings on");

                            if (SQL_SUCCEEDED(l) || (SQL_NO_DATA == l))
                            {
                                __log(_DEBUG, "DBOperation::OpenDSN", "name[%s], dsn[%s], db.Open() success !", lpName, szDSN);
                                int iQueryTimeout = g_cfg.GetValueInt(lpName, "query_timeout", 0);
                                ctx.SetStmtAttr(SQL_ATTR_QUERY_TIMEOUT, (void*)iQueryTimeout, 0);

                                bResult = true;
                            }
                            else
                            {
                                if (false == m_bOpen) //重连不打印log
                                {
                                    __log(_ERROR, "DBOperation::OpenDSN", "name[%s], 'set ansi_warnings on' error, %s !", lpName, ctx.GetErrorInfo());
                                }
                            }
                        }
                        else
                        {
                            if (false == m_bOpen) //重连不打印log
                            {
                                __log(_ERROR, "DBOperation::OpenDSN", "name[%s], ctx.Initialize() error, %s !", lpName, ctx.GetErrorInfo());
                            }
                        }
                    }
                    else
                    {
                        if (false == m_bOpen) //重连不打印log
                        {
                            __log(_ERROR, "DBOperation::OpenDSN", "name[%s], db.Open() error, %s !", lpName, db.GetErrorInfo());
                        }
                    }
                }
                else
                {
                    if (false == m_bOpen) //重连不打印log
                    {
                        __log(_ERROR, "DBOperation::OpenDSN", "decrypt() error !");
                    }
                }
            }
            else
            {
                __log(_ERROR, "DBOperation::OpenDSN", "name[%s], g_cfg.GetValueStr() 'pwd' error !", lpName);
            }
        }
        else
        {
            __log(_ERROR, "DBOperation::OpenDSN", "name[%s], g_cfg.GetValueStr() 'usr' error !", lpName);
        }
    }
    else
    {
        __log(_ERROR, "DBOperation::OpenDSN", "name[%s], g_cfg.GetValueStr() 'dsn' error !", lpName);
    }

    return bResult;
}


bool DBOperation::Open()
{
    m_ctxQPGameData.Close();
    m_dbQPGameData.Close();

    if (true == OpenDSN("qp_gametest_db", m_dbQPGameData, m_ctxQPGameData))
    {
        m_bOpen = true;
    }

    return m_bOpen;
}

bool DBOperation::ExecSP(ODBCContext& ctx, unsigned long ulExecFlag, const char *lpszSP, long argc, ...)
{
    bool bResult = false;

    if (NULL != lpszSP)
    {
        long lRetryCount = 0;

        char szParamters[512] = {0};
        char szCallSP[1024];
        long lArgCount = (ulExecFlag == EXEC_RETURN_VALUE_FLAG) ? (argc - 1) : argc;

        if (lArgCount > 0)
        {
            strcat(szParamters, "(?");

            for (int i = 1; i < lArgCount; i++)
            {
                strcat(szParamters, ",?");
            }

            strcat(szParamters, ")");
        }

        if (EXEC_RETURN_VALUE_FLAG == ulExecFlag)
        {
            sprintf(szCallSP, "{? = call %s%s}", lpszSP, szParamters);
        }
        else
        {
            sprintf(szCallSP, "{call %s%s}", lpszSP, szParamters);
        }

        while (lRetryCount <= RETRY_COUNT)
        {
            if (SQL_SUCCEEDED(ctx.SQLPrepare((LPTSTR)szCallSP)))
            {
                va_list va;
                va_start(va, argc);

                SQLLEN cbLen = SQL_NTS;
                long i = 1;

                for (; i <= argc; i++)
                {
                    void *arg_addr = (void*)va_arg(va, int);
                    long arg_len = va_arg(va, int);
                    long *arg_cblen = (long*)va_arg(va, int);
                    long column_len = va_arg(va, int);
                    long decimal_len = va_arg(va, int);
                    SQLSMALLINT arg_sqltype = va_arg(va, int);
                    SQLSMALLINT arg_ctype = va_arg(va, int);
                    long arg_paramtype = va_arg(va, int);

                    if (!SQL_SUCCEEDED(ctx.BindParam(i, arg_addr, arg_len, arg_cblen, column_len, decimal_len, arg_sqltype, arg_ctype, arg_paramtype)))
                    {
                        return false;
                    }
                }

                long l = ctx.SQLExec();

                if (SQL_SUCCEEDED(l))
                {
                    if (EXEC_SELECT_MORE_FLAG == ulExecFlag)
                    {
                        bResult = true;
                        break;
                    }

                    while (false == bResult)
                    {
                        l = ctx.SQLMoreResults();

                        if (SQL_NO_DATA == l)
                        {
                            bResult = true;
                        }
                        else if (!SQL_SUCCEEDED(l))
                        {
                            break;
                        }
                    }
                }
                else if (SQL_NO_DATA == l)
                {
                    bResult = true;
                }
            }

            if ((false == bResult) && (true == m_dbQPGameData.IsDisconnect())) 
			{
				/*if (&ctx == &m_ctxQPGameData)
				{
					__log(_ERROR, "DBOperation::ExecSP", "reconnect QPGameTestDB, ulExecFlag[%d] !", ulExecFlag);

					OpenDSN("qp_gametest_db", m_dbQPGameData, m_ctxQPGameData);
				}

                lRetryCount++;*/

				break;
            }
            else
            {
                break;
            }
        }
    }

    return bResult;
}

long DBOperation::SPRefreshGameCoinInfo(tagDSRefreshGameCoinInfoReq *pReq, tagDSRefreshGameCoinInfoRsp &info)
{
	long lResult = -1;

	if (false == ExecSP(m_ctxQPGameData, EXEC_RETURN_VALUE_FLAG, "sp_mobile_lobby_refresh_gamecoin_info", 8,
		MAKEPARAM_OUT(&lResult, 0, 0, 0, 0, SQL_INTEGER, SQL_INTEGER),
		MAKEPARAM_IN(&pReq->nUserID, 0, 0, 0, 0, SQL_INTEGER, SQL_INTEGER),
		MAKEPARAM_OUT(&info.llGameCoin, 0, 0, 0, 0, SQL_DOUBLE, SQL_C_SBIGINT),
		MAKEPARAM_OUT(&info.nCoin2Award, 0, 0, 0, 0, SQL_INTEGER, SQL_INTEGER),
		MAKEPARAM_OUT(&info.nAwardScore, 0, 0, 0, 0, SQL_INTEGER, SQL_INTEGER),
		MAKEPARAM_OUT(&info.nLabaCounts, 0, 0, 0, 0, SQL_INTEGER, SQL_INTEGER),
		MAKEPARAM_OUT(&info.nFirstCharge, 0, 0, 0, 0, SQL_INTEGER, SQL_INTEGER),
		MAKEPARAM_OUT(&info.llBankCoin, 0, 0, 0, 0,SQL_DOUBLE, SQL_C_SBIGINT)
		))
	{
		__log(_ERROR, "DBOperation::SPRefreshGameCoinInfo", "ExecSP() error ! UserID[%d], info: %s", pReq->nUserID, m_ctxQPGameData.GetErrorInfo());
	}
	else
	{
		__log(_DEBUG, "DBOperation::SPRefreshGameCoinInfo", "ExecSP() success ! UserID[%d], result[%d], GameCoin[%lld], Coin2Award[%d], AwardScore[%d], LabaCounts[%d], nFirstCharge[%d], BankCoin[%lld]", 
			pReq->nUserID, lResult, info.llGameCoin, info.nCoin2Award, info.nAwardScore, info.nLabaCounts, info.nFirstCharge, info.llBankCoin);
	}

	return lResult;
}



