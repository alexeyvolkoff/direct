#-------------------------------------------------
# Unified client app project for both client flavours, included from
# Node/embedded.pro (the single client entry point):
#   connector (default) - BUILDROOT STAGING + TAGLIB (disk.direct)
#   router/InnBox        - selected from the qmake command line:
#                          qmake DEFINES+=ROUTER ...
#
# A qmake subdirs wrapper (the root Node.pro) does not propagate its
# file-level variables to subprojects - only command-line assignments are
# propagated. Hence the connector flavour defines live in Node/embedded.pro
# and the router flavour is a CLI flag; EMBEDDED (client node, both flavours)
# is set in Node/embedded.pro as well. ROUTER derives below:
#   NO_WEB_UI             - no web frontend (template engine, QWebChannel,
#                           WebSockets): like a P2P-enabled embedded node the
#                           router serves the P2P tunnel and the claim page
#                           only
#   NO_QDRIVE             - no DBus/qdrive, no SSDP announcer (see the
#                           flavour section below), openwrt install layout
#
# P2P_ENABLED stays auto-detected: onmydisklinkclient.pri pulls
# libdatachannel.pri when a libdatachannel build is available.
#-------------------------------------------------

contains(DEFINES, ROUTER): DEFINES += NO_WEB_UI NO_QDRIVE

QT       += core network sql
!contains(DEFINES, ROUTER) {
    QT += dbus
    win32|macx: QT -= dbus
}
CONFIG   += rtti console
macx: CONFIG -= app_bundle

greaterThan(QT_MAJOR_VERSION, 4): {
    DEFINES += QT5
}

CONFIG(release, debug|release):DEFINES += RELEASE

TARGET = onmydisk
TEMPLATE = app

QMAKE_TARGET_COMPANY = "Bineon d.o.o."
QMAKE_TARGET_PRODUCT = "On My Disk"
QMAKE_TARGET_DESCRIPTION = "On My Disk Filesystem Access Service"
QMAKE_TARGET_COPYRIGHT = "Copyright \\251 2015-2025 Bineon d.o.o."

include ($$PWD/../common/setupdirs.pri)

# Disable _ISOC23_SOURCE/_GNU_SOURCE flags that conflict with glibc_compat.cpp
# when using libdatachannel (P2P) - they cause strtoul/strtol recursion via headers.
# The openwrt/musl toolchains (ROUTER) keep the flags from setupdirs.pri as before.
!contains(DEFINES, ROUTER) {
    DEFINES -= _ISOC23_SOURCE=0 _GNU_SOURCE
    QMAKE_CFLAGS -= -D_ISOC23_SOURCE=0 -D_GNU_SOURCE
    QMAKE_CXXFLAGS -= -D_ISOC23_SOURCE=0 -D_GNU_SOURCE
}

include ($$PWD/src/ipc/ipc.pri)
include ($$PWD/src/daemon/daemon.pri)
include ($$PWD/qhttp/qhttp.pri)
include ($$PWD/../common/logging/logging.pri)

# Included BEFORE the web frontend conditionals below: onmydisklinkclient.pri
# pulls libdatachannel.pri which auto-defines P2P_ENABLED when the libdatachannel
# build is available (if P2P_ENABLED was not passed explicitly via qmake).
include ($$PWD/../OnMyDiskLink/onmydisklinkclient.pri)

include ($$PWD/src/httpnode/http-node.pri)

# No web frontend (template engine + QWebChannel + WebSockets) in P2P builds
# and in the router flavour: both serve the P2P tunnel and the claim page only.
contains(DEFINES, P2P_ENABLED) {
    message("P2P_ENABLED: web frontend (webchannel/websockets/templateengine) excluded")
} else:contains(DEFINES, NO_WEB_UI) {
    message("NO_WEB_UI: web frontend (webchannel/websockets/templateengine) excluded (router flavour)")
}
contains(DEFINES, P2P_ENABLED)|contains(DEFINES, NO_WEB_UI) {
    QT -= webchannel websockets
} else {
    QT += webchannel websockets
    include ($$PWD/../common/websockets/websockets.pri)
    include ($$PWD/../common/templateengine/templateengine.pri)
}

include ($$PWD/../common/version/version.pri)

VERSION = $$APP_VERSION

HEADERS += \
    $$PWD/src/omdnode.h \
    $$PWD/src/omdclient.h \
    $$PWD/src/omdsettings.h \
    $$PWD/src/userevents.h \
    $$PWD/src/omd_common.h

SOURCES += \
    $$PWD/src/omdnode.cpp \
    $$PWD/src/omdclient.cpp \
    $$PWD/src/omdsettings.cpp \
    $$PWD/src/userevents.cpp \
    $$PWD/src/client_main.cpp

RESOURCES += \
    $$PWD/resource/console.qrc \
    $$PWD/../assets/mime_icons_minimal.qrc

DEFINES += NO_PRIVATE_HEADERS
INCLUDEPATH += $$PWD/src

win32:LIBS += -ladvapi32

#-------------------------------------------------
# Flavour-specific part
#-------------------------------------------------
contains(DEFINES, ROUTER) {
    # Router/InnBox: openwrt-style install layout, no qdrive, no SSDP announcer
    contains(CONFIG, DEPLOY) {
        KIT_TARGET=$$(KIT_TARGET)
        !isEmpty(KIT_TARGET) {
            TARGET = onmydisk-$$(KIT_ARCH)
        }
    }

    DESTDIR = $$PWD/router/opt/bin
    LIBS   += -lz
    LIBS   += -L$$PWD/router/opt/lib
    unix:!macx:!android:LIBS += -lcrypt
    QMAKE_RPATHDIR += ../lib

    OTHER_FILES += \
        $$PWD/router/etc/onmydisk/onmydisk.conf \
        $$PWD/router/etc/init.d/onmydisk
} else {
    HEADERS += \
        $$PWD/src/ssdpannouncer.h
    SOURCES += \
        $$PWD/src/ssdpannouncer.cpp

    INCLUDEPATH += $$PWD/libqdrive/include
    win32|macx: DEFINES += NO_QDRIVE

    !contains(DEFINES, NO_QDRIVE) {
        !win32 {
            !CONFIG(static) {
                #QMAKE_RPATHDIR += ../lib
                LIBS     += -L$$PWD/target/usr/lib  -lqdrive
            } else {
                LIBS     += -L$$PWD/target/usr/lib  $$PWD/target/usr/lib/libqdrive.a
            }

            LIBS += -ltag
        }
    }

    # keep -lcrypt/-ldl AFTER qdrive/tag: static link resolves deps in one pass
    unix:!macx:!android:LIBS += -lcrypt
    unix:LIBS += -ldl

    #Set destdir to project's filesystem target image dir
    DESTDIR         = $$PWD/target/usr/bin

    KIT_TARGET=$$(KIT_TARGET)

    isEqual(KIT_TARGET, FIREFLY) {
        message("Building for FIREFLY target")
        DEFINES += FIREFLY
        QMAKE_TARGET.arch = $$(KIT_ARCH)
        LIBS += -L/opt/firefly_qt5.15.2_arm64_20.04/ext/lib -lQt5Core -ldl -lpthread
    }

    OTHER_FILES += \
        $$PWD/target/etc/onmydisk.conf
}
