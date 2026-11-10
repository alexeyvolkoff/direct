TEMPLATE = lib

QT *= dbus
QT -= gui
TARGET = qdrive


include($$PWD/../../common/setupdirs.pri)


DESTDIR = $$PWD/../target/usr/lib


INCLUDEPATH *= $$PWD/include

HEADERS += \
        $$PWD/include/AvailableSpace.h \
        $$PWD/include/BlockDevice.h \
        $$PWD/include/DBusObject.h \
        $$PWD/include/Drive.h \
        $$PWD/include/FileSystem.h \
        $$PWD/include/Monitor.h \
        $$PWD/include/Partition.h \
        $$PWD/include/QDrive.h \
        $$PWD/include/Version.h

SOURCES += \
        $$PWD/src/AvailableSpace.cpp \
        $$PWD/src/BlockDevice.cpp \
        $$PWD/src/DBusObject.cpp \
        $$PWD/src/Drive.cpp \
        $$PWD/src/FileSystem.cpp \
        $$PWD/src/Monitor.cpp \
        $$PWD/src/Partition.cpp \
        $$PWD/src/QDrive.cpp




