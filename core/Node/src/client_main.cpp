/******************************************************************************
**
**      main.cpp - On My Disk Daemon
**
**      Copyright 2017 On My Disk Team
**
**     Licensed under the Apache License, Version 2.0 (the "License");
**   you may not use this file except in compliance with the License.
**   You may obtain a copy of the License at
**
**       http://www.apache.org/licenses/LICENSE-2.0
**
**   Unless required by applicable law or agreed to in writing, software
**   distributed under the License is distributed on an "AS IS" BASIS,
**   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
**   See the License for the specific language governing permissions and
**   limitations under the License.
**
*******************************************************************************/

#ifdef DESKTOP
#include <QApplication>
#include <QFontDatabase>
#else
#include <QCoreApplication>
#endif
#include <QStandardPaths>
#include <QDebug>
#include <QSettings>
#include <QDir>
#include <QDataStream>
#include "version.h"
#include "daemon.h"


int main(int argc, char *argv[])
{

    QCoreApplication::setApplicationName(APP_NAME);
    QCoreApplication::setApplicationVersion(APP_VERSION);
    QCoreApplication::setOrganizationName(ORGANIZATION);

    #ifdef DESKTOP
    QApplication app(argc, argv);
    // Load the embedded font.
    int fontId = QFontDatabase::addApplicationFont(":/resource/DejaVuSans.ttf");
    if (fontId != -1)
    {
        QFont font("DejaVuSans");
        font.setPixelSize(13);
        app.setFont(font);
    }
    app.setQuitOnLastWindowClosed(false);
    #else
    QCoreApplication app(argc, argv);
    #endif

    Daemon d;
    app.exec();
}
