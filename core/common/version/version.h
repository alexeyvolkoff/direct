#ifndef VERSION_H
#define VERSION_H
#include <QtCore/qglobal.h>
#include "build_version.h"

/** Name of this application */
#define APP_NAME "On My Disk"
#define APP_GENERIC_NAME "onmydisk"

/** Publisher of this application */
#define ORGANIZATION "Bineon"

/** Short description of the Windows service */
#define DESCRIPTION "On My Disk personal storage"


#define STR(value) #value
#define STRIGIFY(value) STR(value)

//omd protocol version
#define PROTOCOL_VERSION  STRIGIFY(VERSION_MAJOR) "." STRIGIFY(VERSION_MINOR)


//omd protocol version
#if QT_VERSION >= 0x050000
#define PROTOCOL_STREAM_VERSION 5
#else
#define PROTOCOL_STREAM_VERSION 4
#endif

#ifdef INNBOX
#define DEFAULT_PORT   9000
#define DEFAULT_CLOUD "storage.innbox.app"
#define DEFAULT_SECURE true
#define DEFAULT_SEARCH_URL "http://localhost:9090"
#define DEFAULT_AI_URL "http://localhost:4000"
#else
#ifdef P2P_ENABLED
#define DEFAULT_PORT   9010
#define DEFAULT_CLOUD "direct.onmydisk.net"
#elif defined(STAGING)
#define DEFAULT_PORT   9000
#define DEFAULT_CLOUD "staging.onmydisk.net"
#else
#define DEFAULT_PORT   9000
#ifdef QT_DEBUG
#define DEFAULT_CLOUD "localhost"
#else
#define DEFAULT_CLOUD "onmydisk.net"
#endif
#endif
#define DEFAULT_SECURE true
#define DEFAULT_SEARCH_URL "http://localhost:9090"
#define DEFAULT_AI_URL "http://localhost:4000"
#endif

#endif // VFS_VERSION_H
