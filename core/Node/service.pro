#-------------------------------------------------
#
# Embedded client for ARM boards. Build with buildroot toolchain
#
# Development and simulation builds for Linux.
# EMBEDEED is defined, but some functionality may not work without root permissions on linux desktops
#
# For Buildroot toolchans, BUILDROOT is defined. Be careful, you are root on board.
#-------------------------------------------------
DEFINES += NO_QDRIVE SERVICE


QT       += core network widgets sql
CONFIG   += rtti console
QT       -= gui


greaterThan(QT_MAJOR_VERSION, 4): {
    DEFINES += QT5
}


CONFIG(release, debug|release):DEFINES += RELEASE

TARGET = onmydisk

contains(CONFIG, DEPLOY) {
    KIT_TARGET=$$(KIT_TARGET)
    !isEmpty(KIT_TARGET) {
        TARGET = onmydisk-$$(KIT_ARCH)
    }
}

LIBS +=  -ltag

TEMPLATE = app


QMAKE_TARGET_COMPANY = "Bineon d.o.o."
QMAKE_TARGET_PRODUCT = "On My Disk"
QMAKE_TARGET_DESCRIPTION = "On My Disk Filesystem Access Service"
QMAKE_TARGET_COPYRIGHT = "Copyright \\251 2015-2021 Bineon d.o.o."



include ($$PWD/../common/setupdirs.pri)
include ($$PWD/src/ipc/ipc.pri)
include ($$PWD/src/daemon/daemon.pri)
include ($$PWD/../common/logging/logging.pri)
include ($$PWD/../OnMyDiskLink/onmydisklinkclient.pri)
include ($$PWD/../common/version/version.pri)

QMAKE_LFLAGS_SONAME  = -Wl,-install_name,@executable_path/../lib/


#Set destdir to project's filesystem target image dir
DESTDIR         = $$PWD/service/usr/bin
LIBS            += -L$$PWD/service/usr/lib
QMAKE_RPATHDIR	+= ../lib

VERSION = $$APP_VERSION

HEADERS += \
    $$PWD/src/omdnode.h \
    $$PWD/src/omdclient.h \
    $$PWD/src/omdsettings.h \
    $$PWD/src/omd_common.h \
    src/servicenode.h



SOURCES += \
    $$PWD/src/omdnode.cpp \
    $$PWD/src/omdclient.cpp \
    $$PWD/src/omdsettings.cpp \
    $$PWD/src/client_main.cpp \
    src/servicenode.cpp


RESOURCES += \
    $$PWD/resource/console.qrc \
    $$PWD/../assets/mime_icons_minimal.qrc

OTHER_FILES += \
    $$PWD/service/etc/onmydisk/onmydisk.conf


DEFINES += NO_PRIVATE_HEADERS
INCLUDEPATH += $$PWD/src


