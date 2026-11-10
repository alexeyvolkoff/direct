DEFINES += A20_GPIO

SOURCES += \
        $$PWD/A20-gpio.cpp \
        $$PWD/gpio_lib.c


HEADERS += \
        $$PWD/gpio_lib.h \
        $$PWD/A20-gpio.hpp


INCLUDEPATH += $$PWD
