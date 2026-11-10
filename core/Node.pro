TEMPLATE = subdirs
CONFIG += force_qmake

# STAGING is defined in Node/embedded.pro: a subdirs wrapper propagates only
# command-line assignments to subprojects, not its file-level variables.
# The router/InnBox flavour is selected from the CLI:
#   qmake DEFINES+=ROUTER Node.pro
SUBDIRS += OnMyDisk

OnMyDisk.file = $$PWD/Node/embedded.pro

# No LibQDrive for the router flavour or macOS (no DBus/qdrive there)
unix:!macx:!contains(DEFINES, ROUTER) {
    SUBDIRS += LibQDrive
    INCLUDEPATH += $$PWD/Node/libqdrive/include
    DEPENDPATH  += $$PWD/Node/libqdrive
    LibQDrive.file = $$PWD/Node/libqdrive/qdrive.pro
    OnMyDisk.depends = LibQDrive
}
