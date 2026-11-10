# common/libdatachannel.pri
# WebRTC (libdatachannel) integration for direct P2P tunnel traffic.
#
# Defines P2P_ENABLED when a libdatachannel build is available and adds its
# include path, p2ptunnel.cpp and the linker flags. Without it the tunnel
# operates in relay mode only.
#
# Overrides:
#   qmake LDATACHANNEL_ROOT=/path   (or environment LDATACHANNEL_ROOT)
#   qmake LDATACHANNEL_BUILD=/path  (build dir containing libdatachannel.a/.so)

isEmpty(LDATACHANNEL_ROOT) {
    LDATACHANNEL_ROOT = $$(LDATACHANNEL_ROOT)
}
isEmpty(LDATACHANNEL_ROOT) {
    LDATACHANNEL_ROOT = $$PWD/../libdatachannel
}

exists($$LDATACHANNEL_ROOT/include/rtc/rtc.hpp) {
    DEFINES += P2P_ENABLED
    DEFINES += RTC_STATIC
    INCLUDEPATH += $$LDATACHANNEL_ROOT/include

    isEmpty(LDATACHANNEL_BUILD) {
        LDATACHANNEL_BUILD = $$(LDATACHANNEL_BUILD)
    }
    isEmpty(LDATACHANNEL_BUILD) {
        exists($$LDATACHANNEL_ROOT/build-static/libdatachannel.a): \
            LDATACHANNEL_BUILD = $$LDATACHANNEL_ROOT/build-static
        else: LDATACHANNEL_BUILD = $$LDATACHANNEL_ROOT/build
    }

    SOURCES += $$PWD/../OnMyDiskLink/p2ptunnel.cpp
    HEADERS += $$PWD/../OnMyDiskLink/p2ptunnel.h

    unix {
        LIBS += -L$$LDATACHANNEL_BUILD
        LIBS += -ldatachannel
        exists($$LDATACHANNEL_BUILD/deps/libjuice/libjuice.a) {
            LIBS += -L$$LDATACHANNEL_BUILD/deps/libjuice
            LIBS += -L$$LDATACHANNEL_BUILD/deps/usrsctp/usrsctplib
            LIBS += -ljuice -lusrsctp
            exists($$LDATACHANNEL_BUILD/deps/libsrtp/libsrtp2.a) {
                LIBS += -L$$LDATACHANNEL_BUILD/deps/libsrtp
                LIBS += -lsrtp2
            }
        }
        # static libdatachannel needs OpenSSL symbols resolved after its own
        # objects, so repeat -lssl -lcrypto here (link order matters)
        macx {
            OPENSSL_ROOT_ENV = $$(OPENSSL_ROOT_DIR)
            !isEmpty(OPENSSL_ROOT_ENV): LIBS += -L$$OPENSSL_ROOT_ENV/lib
        }
        LIBS += -lssl -lcrypto
        LIBS += -ldl -lpthread
    }
    mingw {
        LIBS += -L$$LDATACHANNEL_BUILD
        LIBS += -ldatachannel
        exists($$LDATACHANNEL_BUILD/deps/libjuice/libjuice.a) {
            LIBS += -L$$LDATACHANNEL_BUILD/deps/libjuice
            LIBS += -L$$LDATACHANNEL_BUILD/deps/usrsctp/usrsctplib
            LIBS += -ljuice -lusrsctp
            exists($$LDATACHANNEL_BUILD/deps/libsrtp/libsrtp2.a) {
                LIBS += -L$$LDATACHANNEL_BUILD/deps/libsrtp
                LIBS += -lsrtp2
            }
        }
        # static libdatachannel needs OpenSSL + Winsock system libs
        LIBS += -lssl -lcrypto -lws2_32 -liphlpapi -lwinmm -lbcrypt -lcrypt32
    }
    message("libdatachannel: P2P tunnel support enabled ($$LDATACHANNEL_BUILD)")
} else {
    message("libdatachannel: not found in $$LDATACHANNEL_ROOT, P2P tunnel support disabled (relay only)")
}
