INCLUDEPATH += $$PWD/

#DEFINES += QHTTP_MEMORY_LOG=2

SOURCES  += \
    $$PWD/qhttpabstracts.cpp \
    $$PWD/qhttpserverconnection.cpp \
    $$PWD/qhttpserverrequest.cpp \
    $$PWD/qhttpserverresponse.cpp \
    $$PWD/qhttpserver.cpp \
    $$PWD/http_parser.c 

HEADERS  += \
    $$PWD/qhttpfwd.hpp \
    $$PWD/qhttpabstracts.hpp \
    $$PWD/qhttpserverconnection.hpp \
    $$PWD/qhttpserverrequest.hpp \
    $$PWD/qhttpserverresponse.hpp \
    $$PWD/qhttpserver.hpp \
    $$PWD/http_parser.h 


