//
//  log.cpp
//  Master
//
//  Created by J Matthew Sebastian on 5/4/19.
//  Copyright © 2019 J Matthew Sebastian. All rights reserved.
//

#ifdef WIN32
#include <windows.h>
#endif

//#include <sys/time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "log.h"

char szError[1024];

#pragma warning(disable : 4996)

int logFile(const char *pszFile, long lLine, const char *pszString)
{
	int iRc = logFile((char *)pszFile, lLine, pszString);
	return iRc;
}

int logFile( char *pszFile, long lLine, const char *pszString )
{
    FILE *fp;
    char szReturn1[1024];
    char szReturn2[1024];
    
    if ((fp = fopen ("error.log", "a")) == NULL )
    {
        printf( "fopen error returned %d", errno );
        return -1;
    }
    
    // /usr/local/include/myheader.h
    
    fprintf(fp, "%-24s %s(%ld) %s\n",
          getTimestamp(szReturn1),
          getFileNameOnly(pszFile, szReturn2),
          lLine,
          pszString
          );

    fclose(fp);

    return 0;
}


char *getTimestamp(char *pszReturn)
{
#ifdef WIN32
	SYSTEMTIME local;
	GetLocalTime(&local);

	sprintf(pszReturn, "%04hd-%02hd-%02hd %02hd:%02hd:%02hd.%03hd",
		local.wYear,
		local.wMonth,
		local.wDay,
		local.wHour,
		local.wMinute,
		local.wSecond,
		local.wMilliseconds );
/*
	WORD wYear;
	WORD wMonth;
	WORD wDayOfWeek;
	WORD wDay;
	WORD wHour;
	WORD wMinute;
	WORD wSecond;
	WORD wMilliseconds;
*/

#endif
#ifdef __APPLE__
	struct timeval tv;
    struct tm *tm;
    char szTimestamp[256];
    
    gettimeofday(&tv, NULL);
    tm = localtime(&tv.tv_sec);

    sprintf( pszReturn, "%04d-%02d-%02d %02d:%02d:%02d.%03d",
            tm->tm_year + 1900,
            tm->tm_mon + 1,
            tm->tm_mday,
            tm->tm_hour,
            tm->tm_min,
            tm->tm_sec,
            (int) (tv.tv_usec / 1000)
            );
#endif

    return pszReturn;
}

char *getFileNameOnly(char *filename, char *pszReturn)
{
    char temp1[1024];
    char *F;
    int i = 0;
	#ifdef __APPLE__
		F=strrchr(filename,'/');
    #elif WIN32
		F=strrchr(filename,'\\');
    #endif
    strcpy(temp1,F+1);
    
    if(*F == '/')
        F++;
    
    while(F[i] != '\0')
    {
        pszReturn[i] = temp1[i];
        i++;
    }
    pszReturn[i] = '\0';
    return pszReturn;
}

