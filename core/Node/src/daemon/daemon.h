/******************************************************************************
**
**      daemon.h - On My Disk Daemon
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
#ifndef DAEMON_H
#define DAEMON_H

#include <QObject>
#ifdef DESKTOP
#include <QApplication>
#else
#include <QCoreApplication>
#endif
#include <QSettings>
#include <QLocalServer>
#include <QSslSocket>
#include <filelogger.h>
#include <ipcserver.h>


#ifdef EMBEDDED
#include "httpnode.h"
#endif


#ifdef SERVICE
#include "servicenode.h"
#endif


#ifdef DESKTOP
#include "desktopnode.h"
#ifdef DEPLOY
#define LOGGING
#include "filelogger.h"
using namespace stefanfrings;
#endif
#endif


class Daemon: public QObject
{
 Q_OBJECT
public:
    /** Constructor */
    Daemon(QObject* parent = Q_NULLPTR);
    /** Destructor */
    ~Daemon();

protected:
    void start();
    void stop();

private:
    bool m_isService;

    QString  m_configDir, m_logDir;
    IPCServer *m_ipcServer;

#ifdef DESKTOP
    OnMyDiskDesktopNode *m_omdNode;
#endif

#ifdef EMBEDDED
    OnMyDiskHttpNode *m_omdNode;
#endif

#ifdef SERVICE
    ServiceNode *m_omdNode;
#endif


#ifdef LOGGING
    FileLogger* m_logger;
#endif

    void performTest(const QString &fileName);
};

#endif // DAEMON_H
