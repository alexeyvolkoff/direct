INCLUDEPATH += $$PWD  $$PWD/../../qhttp


HEADERS   += \
    $$PWD/httpnode.h \
    $$PWD/httprange.h \
    $$PWD/filereader.h \
    $$PWD/clientsession.h \
    $$PWD/iauthorization.h \
    $$PWD/httphandler.h \
    $$PWD/sessionauth.h \
    $$PWD/consolelogger.h \
    $$PWD/crawler.h

!contains(DEFINES, P2P_ENABLED):!contains(DEFINES, NO_WEB_UI) {
    # Web frontend (QWebChannel devices + template engine) is not part of
    # P2P builds and of the router flavour (NO_WEB_UI)
    HEADERS += \
        $$PWD/httpdevicebackend.h \
        $$PWD/templateparser.h
}

SOURCES   += \
    $$PWD/httpnode.cpp \
    $$PWD/httprange.cpp \
    $$PWD/filereader.cpp \
    $$PWD/sessionauth.cpp \
    $$PWD/clientsession.cpp \
    $$PWD/httphandler.cpp \
    $$PWD/crawler.cpp \
    $$PWD/consolelogger.cpp

!contains(DEFINES, P2P_ENABLED):!contains(DEFINES, NO_WEB_UI) {
    SOURCES += \
        $$PWD/httpdevicebackend.cpp \
        $$PWD/templateparser.cpp
}
