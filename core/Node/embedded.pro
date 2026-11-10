#-------------------------------------------------
#
# Embedded/connector client for ARM boards. Build with buildroot toolchain.
#
# Development and simulation builds for Linux.
# EMBEDDED is defined, but some functionality may not work without root permissions on linux desktops
#
# For Buildroot toolchans, BUILDROOT is defined. Be careful, you are root on board.
#-------------------------------------------------
# Single entry point for both client flavours; all shared build logic lives in
# Node/client.pri:
#   connector (default) - BUILDROOT STAGING + TAGLIB, staging.onmydisk.net;
#                          web UI, optional P2P (auto-detected libdatachannel)
#   router/InnBox        - selected from the qmake command line:
#                          qmake DEFINES+=ROUTER ... (P2P tunnel and claim page
#                          only, openwrt install layout, no web UI/qdrive/SSDP)
#
# Flavour defines must live in this subproject file: a qmake subdirs wrapper
# does not propagate its file-level variables to subprojects - only
# command-line assignments are propagated (that is why DEFINES+=ROUTER works).
#-------------------------------------------------
DEFINES += EMBEDDED
!contains(DEFINES, ROUTER) {
    DEFINES += BUILDROOT STAGING
    !win32:!macx: DEFINES += TAGLIB_SUPPORTED
}

include ($$PWD/client.pri)
