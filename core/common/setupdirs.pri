# specifying common dirs
linux: {
    DEFINES += _ISOC23_SOURCE=0 _GNU_SOURCE
    QMAKE_CFLAGS += -D_ISOC23_SOURCE=0 -D_GNU_SOURCE
    QMAKE_CXXFLAGS += -D_ISOC23_SOURCE=0 -D_GNU_SOURCE
    # Glibc compatibility wrapper
    SOURCES += $$PWD/glibc_compat.cpp
}

CONFIG         += c++1z

mac:QMAKE_CXXFLAGS += -stdlib=libc++

linux-buildroot-g++ {
    DEFINES += BUILDROOT
}

linux-openwrt-g++ {
    DEFINES += OPENWRT
}

linux-g++:QMAKE_TARGET.arch = $$QMAKE_HOST.arch
linux-g++-32:QMAKE_TARGET.arch = x86
linux-g++-64:QMAKE_TARGET.arch = x86_64
linux-buildroot-g++:QMAKE_TARGET.arch = armv7
mac:QMAKE_TARGET.arch = mac

mac {
    LIBS += -lc++ -framework Cocoa -framework Foundation -framework Security -framework SystemConfiguration
    OPENSSL_ROOT_ENV = $$(OPENSSL_ROOT_DIR)
    !isEmpty(OPENSSL_ROOT_ENV) {
        INCLUDEPATH += $$OPENSSL_ROOT_ENV/include
        LIBS += -L$$OPENSSL_ROOT_ENV/lib
    }
}

win32 {
    DEFINES        += _WINDOWS WIN32_LEAN_AND_MEAN NOMINMAX
	message("Windows build: $$QMAKE_TARGET.arch")
    win32-msvc {
        !contains(QMAKE_TARGET.arch, x86_64) {
            message("x86 build (MSVC + OpenSSL-Win32)")
            LIBS += -LC:/OpenSSL-Win32/lib -LC:/OpenSSL-Win32/lib/VC
            LIBS += -llibcrypto32MD -llibssl32MD
            INCLUDEPATH += C:/OpenSSL-Win32/include

        } else {
            message("x86_64 build (MSVC + OpenSSL-Win64)")
            LIBS += -LC:/OpenSSL-Win64/lib -LC:/OpenSSL-Win64/lib/VC
            LIBS += -llibcrypto64MD -llibssl64MD
            INCLUDEPATH += C:/OpenSSL-Win64/include
        }
    } else {
        # MinGW / MXE: OpenSSL comes from the toolchain
        message("MinGW build (toolchain OpenSSL)")
        LIBS += -lcrypto -lssl
    }
}


linux-openwrt-g++ {
	KIT_ARCH=$$(KIT_ARCH)
	!isEmpty(KIT_ARCH) {
		QMAKE_TARGET.arch = $$KIT_ARCH
	} else {
		QMAKE_TARGET.arch = mips
	}
	message("OpenWRT build: $$QMAKE_TARGET.arch")
        CONFIG += static
        QMAKE_LFLAGS += -static -static-libgcc
        #QMAKE_LFLAGS += "-Wl,-rpath,\'\$$ORIGIN/../lib/\'"
        QMAKE_CFLAGS += -static -static-libgcc -mmusl
        QMAKE_CXXFLAGS += -static -static-libgcc -mmusl
	INCLUDEPATH += $$(KIT_INCLUDE)
        KIT_TARGET=$$(KIT_TARGET)
        !isEmpty(KIT_TARGET) {
             DEFINES += $$KIT_TARGET
             message("OpenWRT target: $$KIT_TARGET")
        }
}

CONFIG(debug, debug|release) {
    win32:TEMPDIR = $$PWD/../build/win32-debug/$$TARGET-$$[QT_ARCH]
    else:TEMPDIR = $$PWD/../build/debug/$$TARGET-$$QMAKE_TARGET.arch
    DEFINES += QT_DEBUG
} else {
    win32:TEMPDIR = $$PWD/../build/win32-release/$$TARGET-$$[QT_ARCH]
    else:TEMPDIR = $$PWD/../build/release/$$TARGET-$$QMAKE_TARGET.arch
}

!CONFIG(static) {
    message("Dynamic linking")
    unix:LIBS+= -lssl -lcrypto
} else {
    message("Static linking")
}

MOC_DIR         = $$TEMPDIR
OBJECTS_DIR     = $$TEMPDIR
RCC_DIR         = $$TEMPDIR
UI_DIR          = $$TEMPDIR/Ui


