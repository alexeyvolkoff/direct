#-------------------------------------------------
#
# omd-tunnel - TCP tunnel over OMD gateway, used as ssh ProxyCommand
#
# Standalone minimal client: no OMD profile is required, a self-signed
# device identity is generated in the config dir on first run.
#
#-------------------------------------------------
QT += core network
CONFIG   += rtti console
macx: CONFIG -= app_bundle

greaterThan(QT_MAJOR_VERSION, 4): {
    DEFINES += QT5
}

CONFIG(release, debug|release):DEFINES += RELEASE

TARGET = omd-tunnel
TEMPLATE = app

include ($$PWD/../common/setupdirs.pri)
include ($$PWD/../common/libdatachannel.pri)
include ($$PWD/../common/version/version.pri)

INCLUDEPATH += $$PWD/../OnMyDiskLink

SOURCES += \
    $$PWD/main.cpp \
    $$PWD/../OnMyDiskLink/linkmessage.cpp \
    $$PWD/../OnMyDiskLink/sslhelper.cpp \
    $$PWD/../OnMyDiskLink/tunnelcrypto.cpp

HEADERS += \
    $$PWD/../OnMyDiskLink/linkmessage.h \
    $$PWD/../OnMyDiskLink/onmydisklink_global.h \
    $$PWD/../OnMyDiskLink/sslhelper.h \
    $$PWD/../OnMyDiskLink/tunnelcrypto.h

LIBS += -lcrypto

DESTDIR = $$PWD/../bin
