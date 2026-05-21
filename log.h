//
//  log.h
//  Master
//
//  Created by J Matthew Sebastian on 5/4/19.
//  Copyright © 2019 J Matthew Sebastian. All rights reserved.
//

#ifndef log_h
#define log_h

#define MAXLINE 2048
extern char szError[1024];

//#include <string.h>

/*
#ifdef WINDOWS
#define __FILENAME__ (strrchr(__FILE__, '//') ? strrchr(__FILE__, '//') + 1 : __FILE__)
#else
#define __FILENAME__ (strrchr(__FILE__, '/') ? strrchr(__FILE__, '/') + 1 : __FILE__)
#endif
*/
 
#define LOG(x)  logFile( __FILE__, __LINE__, x)

int logFile(const char *pszFile, long lLine, const char *pszString);
int logFile(char *pszFile, long lLine, const char *pszString);
char *getTimestamp(char *pszBuffer );
char *getFileNameOnly(char *filename, char *pszBuffer );

#endif /* log_h */
