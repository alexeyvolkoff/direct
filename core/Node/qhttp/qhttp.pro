QT       += core network
QT       -= gui

TARGET    = qhttp
TEMPLATE  = lib


include($$PWD/../../common/setupdirs.pri)

DEFINES  += QHTTP_MEMORY_LOG=0

win32:DEFINES += QHTTP_EXPORT

CONFIG(debug, debug|release) {
   DESTDIR         = $$PWD/../../run/lib_d
   win32:DLLDESTDIR = $$PWD/../../run/bin_d
} else {
   DESTDIR         = $$PWD/../../run/lib
   win32:DLLDESTDIR = $$PWD/../../run/bin
}


SOURCES  += \
    qhttpabstracts.cpp \
    qhttpserverconnection.cpp \
    qhttpserverrequest.cpp \
    qhttpserverresponse.cpp \
    qhttpserver.cpp \
    http_parser.c 

HEADERS  += \
    qhttpfwd.hpp \
    qhttpabstracts.hpp \
    qhttpserverconnection.hpp \
    qhttpserverrequest.hpp \
    qhttpserverresponse.hpp \
    qhttpserver.hpp \
    http_parser.h 

DISTFILES += \
    notes.txt

contains(DEFINES, QHTTP_HAS_CLIENT) {
    SOURCES += \
        qhttpclientrequest.cpp \
        qhttpclientresponse.cpp \
        qhttpclient.cpp

    HEADERS += \
        qhttpclient.hpp \
        qhttpclientresponse.hpp \
        qhttpclientrequest.hpp
}

!contains(QT_ARCH, x86_64) {
    target.path = /usr/lib
} else {
    target.path = /usr/lib64
}
INSTALLS += target
