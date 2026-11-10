QT          += core network sql  webchannel websockets
CONFIG      += console rtti force_debug_info
osx:CONFIG  -= app_bundle
# SVAR is defined here, not in Gateway.pro: qmake does not propagate DEFINES
# of a subdirs project to its subprojects.
DEFINES += SERVER SVAR



include ($$PWD/qhttp/qhttp.pri)
include ($$PWD/../OnMyDiskLink/onmydisklinkserver.pri)
include ($$PWD/../common/libdatachannel.pri)
include ($$PWD/../common/templateengine/templateengine.pri)
include ($$PWD/../common/websockets/websockets.pri)
include ($$PWD/../common/logging/logging.pri)
include ($$PWD/../common/version/version.pri)
include ($$PWD/../common/setupdirs.pri)
include ($$PWD/../SMTPClient/SMTPClient.pri)


TARGET       = omd-server
TEMPLATE     = app

CONFIG(debug, debug|release) {
     DESTDIR         = $$PWD/../run/bin_d
     #unix:QMAKE_RPATHDIR += ../lib_d
     #unix:QMAKE_LFLAGS_SONAME  = -Wl,-install_name,@executable_path/../lib_d/
} else {
     DESTDIR         = $$PWD/../run/bin
     #unix:QMAKE_RPATHDIR += ../lib
     #unix:QMAKE_LFLAGS_SONAME  = -Wl,-install_name,@executable_path/../lib/
}

QMAKE_LFLAGS += -export-dynamic
LIBS += -ldl  -ltag
unix:!macx:!android:LIBS += -lcrypt

INCLUDEPATH += $$PWD/qhttp
#INCLUDEPATH += $$PWD/../../qfcgi
INCLUDEPATH += $$PWD/../OnMyDiskLink
INCLUDEPATH += $$PWD/..

INCLUDEPATH += $$PWD/src $$PWD/src/httpnode
DEPENDPATH += $$PWD/src

INCLUDEPATH += $$PWD/../common/templateengine $$PWD/../common/websockets
DEPENDPATH += $$PWD/../common/templateengine $$PWD/../common/websockets

HEADERS   += \
    $$PWD/src/httpnode/httpserverbackend.h \
    $$PWD/src/httpnode/httprange.h \
    $$PWD/src/httpnode/filereader.h \
    $$PWD/src/httpnode/clientsession.h \
    $$PWD/src/httpnode/iauthorization.h \
    $$PWD/src/httpnode/httphandler.h \
    $$PWD/src/httpnode/digestauth.h \
    $$PWD/src/httpnode/basicauth.h \
    $$PWD/src/httpnode/deviceauth.h \
    $$PWD/src/httpnode/sessionauth.h \
    $$PWD/src/httpnode/clientnotifier.h \
    $$PWD/src/httpnode/templateparser.h \
    $$PWD/src/omdsettings.h \
    $$PWD/src/watchdog.h \
    $$PWD/src/onmydiskmailer.h \
    $$PWD/src/userevents.h \
    $$PWD/src/notificationmanager.h

unix:HEADERS += $$PWD/src/unixdaemon.h

DEFINES += NO_PRIVATE_HEADERS

SOURCES   += \
    $$PWD/src/httpnode/httpserverbackend.cpp \
    $$PWD/src/httpnode/httprange.cpp \
    $$PWD/src/httpnode/filereader.cpp \
    $$PWD/src/httpnode/clientsession.cpp \
    $$PWD/src/httpnode/httphandler.cpp \
    $$PWD/src/httpnode/deviceauth.cpp \
    $$PWD/src/httpnode/digestauth.cpp \
    $$PWD/src/httpnode/basicauth.cpp \
    $$PWD/src/httpnode/sessionauth.cpp \
    $$PWD/src/httpnode/clientnotifier.cpp \
    $$PWD/src/httpnode/templateparser.cpp \
    $$PWD/src/omdsettings.cpp \
    $$PWD/src/onmydiskmailer.cpp \
    $$PWD/src/watchdog.cpp \
    $$PWD/src/server_main.cpp \
    $$PWD/src/userevents.cpp \
    $$PWD/src/notificationmanager.cpp


unix:SOURCES += $$PWD/src/unixdaemon.cpp

defined(USE_EPOLL, var) {
    linux:DEFINES += USE_EPOLL
    linux:HEADERS += $$PWD/src/qeventdispatcher_epoll.h
    linux:SOURCES += $$PWD/src/qeventdispatcher_epoll.cpp
}


# OMD 3.0 (direct): legacy filemanager frontend removed.
# Only the SVAR assets (favicon + service-worker fallback) are packaged.
# Mime icons are served from /srv/www/mimetypes on disk (SVAR uses the CDN icon set).
message("OMD 3.0: legacy filemanager QRC resources removed, packaging svar assets only")
RESOURCES += \
   $$PWD/../../web/svar_assets.qrc


target.path = /usr/bin

config.files = $$PWD/../run/etc/config/server.conf.example
config.path = /etc/onmydisk


INSTALLS += target config
