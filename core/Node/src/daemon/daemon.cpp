/******************************************************************************
**
**      daemon.cpp - On My Disk Daemon
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

#include "daemon.h"
#include <version.h>
#include <QSettings>
#include <QStandardPaths>
#include <QCommandLineParser>
#include <QLocalSocket>
#include <QUrl>
#include <QDir>
#include <QDebug>
#include <QFileInfo>

#ifdef DESKTOP
#include <QDesktopServices>
#endif
#include <ipcclient.h>
#include "localdb.h"
#include "omdupdater.h"

#include <iostream>
#include <string>

#ifdef Q_OS_WIN32
#include <windows.h>
QString secureInput()
{
    HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    GetConsoleMode(hStdin, &mode);
    SetConsoleMode(hStdin, mode & (~ENABLE_ECHO_INPUT));

    std::string s;
    getline(std::cin, s);
    SetConsoleMode(hStdin, mode | ENABLE_ECHO_INPUT);
    QString result = QString::fromStdString(s);
    return result;
}
#else
#include <termios.h>
#include <unistd.h>
QString secureInput()
{
    termios oldt;
    tcgetattr(STDIN_FILENO, &oldt);
    termios newt = oldt;
    newt.c_lflag &= ~ECHO;
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);

    std::string s;
    getline(std::cin, s);

    newt.c_lflag |= ECHO;
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);


    QString result = QString::fromStdString(s);
    return result;
}
#endif

Daemon::Daemon(QObject* parent): QObject (parent)
{
    m_isService = isService();
    start();
}

Daemon::~Daemon()
{
    stop();
}

//start - connect client
void Daemon::start()
{
#ifdef Q_OS_WIN
#ifdef EMBEDDED
    // user-level autostart: HKCU Run key, so the node starts at login
    // for the installing user. No service, no admin rights required.
    {
        QSettings autoRun("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                          QSettings::NativeFormat);
        const QString appFile = QCoreApplication::applicationFilePath();
        const QString runKey = QString("\"%1\"").arg(QDir::toNativeSeparators(appFile));
        const QString curVal = autoRun.value("OnMyDiskNode").toString();
        if (curVal.isEmpty() || (!curVal.contains("wscript", Qt::CaseInsensitive) && curVal != runKey)) {
            autoRun.setValue("OnMyDiskNode", runKey);
            autoRun.sync();
        }
        // Remove legacy desktop client Run key if it points to a different binary that would steal IPC pipe
        const QString legacyVal = autoRun.value("onmydisk").toString();
        if (!legacyVal.isEmpty() && !legacyVal.contains(QDir::toNativeSeparators(appFile), Qt::CaseInsensitive)) {
            autoRun.remove("onmydisk");
            autoRun.sync();
        }
    }
    // Detach console when launched directly by Explorer / Run key without CLI flags
    // so no black console window stays on screen (closing which would kill the daemon).
    if (qApp->arguments().size() <= 1) {
        DWORD procList[2] = {0, 0};
        if (GetConsoleProcessList(procList, 2) == 1) {
            FreeConsole();
        }
    }
    // Disable the console QuickEdit selection mode: while text is selected in
    // the console, every WriteFile/console write to stdout blocks forever and
    // the daemon appears to freeze for good (only Ctrl+C and restart help).
    {
        HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
        if (hOut != INVALID_HANDLE_VALUE && hOut != nullptr) {
            DWORD outMode = 0;
            if (GetConsoleMode(hOut, &outMode)) {
                SetConsoleMode(hOut, (outMode | ENABLE_EXTENDED_FLAGS)
                                     & ~ENABLE_QUICK_EDIT_MODE);
            }
        }
        HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
        if (hIn != INVALID_HANDLE_VALUE && hIn != nullptr) {
            DWORD inMode = 0;
            if (GetConsoleMode(hIn, &inMode)) {
                SetConsoleMode(hIn, (inMode | ENABLE_EXTENDED_FLAGS)
                                    & ~ENABLE_QUICK_EDIT_MODE);
            }
        }
    }
#endif
#endif
    //parse command line
    QCommandLineParser parser;

    parser.setApplicationDescription(DESCRIPTION);

    QCommandLineOption addShareOption(QStringList() << "a" << "add-share",
            QCoreApplication::translate("main", "Add share."),
            QCoreApplication::translate("main", "path"));

    QCommandLineOption removeShareOption(QStringList() << "r" << "remove-share",
            QCoreApplication::translate("main", "Remove share."),
            QCoreApplication::translate("main", "path"));

    QCommandLineOption connectOption(QStringList() << "c" << "connect",
            QCoreApplication::translate("main", "Connect."));

    QCommandLineOption disconnectOption(QStringList() << "d" << "disconnect",
            QCoreApplication::translate("main", "Disconnect."));

    QCommandLineOption statusOption(QStringList() << "i" << "status",
            QCoreApplication::translate("main", "Get status info."));

    QCommandLineOption authOption(QStringList() << "l" << "login",
            QCoreApplication::translate("main", "User authentication."));


    #ifdef DESKTOP
    QCommandLineOption trayOption(QStringList() << "t" << "tray",
            QCoreApplication::translate("main", "Show as tray icon."));

    QCommandLineOption showOption(QStringList() << "s" << "show",
            QCoreApplication::translate("main", "Show."));


    QCommandLineOption urlOption(QStringList() << "u" << "url",
            QCoreApplication::translate("main", "Parse command from url scheme."),
            QCoreApplication::translate("main", "url"));
    #endif

    QCommandLineOption quitOption(QStringList() << "q" << "quit",
            QCoreApplication::translate("main", "Quit."));



    parser.addHelpOption();
    parser.addVersionOption();

    parser.addOption(addShareOption);
    parser.addOption(removeShareOption);
    parser.addOption(connectOption);
    parser.addOption(disconnectOption);
    parser.addOption(statusOption);
    parser.addOption(authOption);
#ifdef DESKTOP
    parser.addOption(showOption);
    parser.addOption(trayOption);
    parser.addOption(urlOption);
#endif
    parser.addOption(quitOption);


   parser.process(qApp->arguments());

   IPCClient omdService;
   //#ifdef EMBEDDED
   QString omdServiceSocketName = getIPCSocketName(true); // embedded client runs as service
   QString omdClientSocketName = getIPCSocketName(false); //desktop client runs as user process

   //try to connect to system-wide service
   omdService.connectToServer(omdServiceSocketName);

   if (!m_isService && !omdService.waitForConnected(100))  //try user service
       omdService.connectToServer(omdClientSocketName);
   #if !defined(QT_DEBUG) && !defined(STAGING)

   if (omdService.waitForConnected(100)) //if service is running, send command to service
   {
      qInfo() << qPrintable(QString("%1 v.%2").arg(APP_NAME, APP_VERSION));
      if (parser.optionNames().count() == 0)
        qInfo() << "Service is up and running.\nPlease run with -h for help.";
      if (parser.isSet(addShareOption))
      {
          ServiceRequest request(RequestType::AddShare, parser.value(addShareOption));
          qDebug() << "Adding" << parser.value(addShareOption);
          omdService.request(request);
      } else
      if (parser.isSet(removeShareOption))
      {
          ServiceRequest request(RequestType::RemoveShare, parser.value(removeShareOption));
          qDebug() << "Removing" << parser.value(removeShareOption);
          omdService.request(request);
      } else
      if (parser.isSet(connectOption))
      {
           ServiceRequest request(RequestType::Connect, parser.value(connectOption));
           qDebug() << "Connecting";
           omdService.request(request);
      } else
      if (parser.isSet(disconnectOption))
      {
           ServiceRequest request(RequestType::Disconnect, parser.value(connectOption));
           qDebug() << "Disconnecting";
           omdService.request(request);
      } else
      if (parser.isSet(authOption))
      {
           qInfo() << "Authenticating user";
           std::cout << "User: ";
           QTextStream s(stdin);
           QString user = s.readLine();
           std::cout << "Password: ";
           QString passwod = secureInput();

           ServiceRequest request(RequestType::Auth, user + ":" + passwod);
           ServiceResponse response = omdService.service(request);
           QStringList statusData = QString::fromUtf8(response.msg_data).split(";");
           Q_FOREACH(QString line, statusData)
               qInfo() << qPrintable(line);
      } else
      if (parser.isSet(quitOption))
      {
           ServiceRequest request(RequestType::Quit, parser.value(quitOption));
           qDebug() << "Quit application";
           omdService.request(request);
      } else
      #if (SERVICE || EMBEDDED)
      if (parser.isSet(statusOption) || parser.optionNames().count() == 0)
      {
           ServiceRequest request(RequestType::GetStatus);
           qDebug() << "Connection status:";
           ServiceResponse response = omdService.service(request);
           QStringList statusData = QString::fromUtf8(response.msg_data).split(";");
           Q_FOREACH(QString line, statusData)
                qInfo() << qPrintable(line);
      }
      #endif
      #ifdef DESKTOP
      if (parser.isSet(urlOption) )
      {
          ServiceRequest request(RequestType::Url, parser.value(urlOption));
          omdService.request(request);
          qInfo() << "URL scheme request sent:" << parser.value(urlOption);
      }
      else
      if (parser.isSet(showOption) || parser.optionNames().count() == 0)
      {
          ServiceRequest request(RequestType::Show);
          omdService.request(request);
          qInfo() << "Show GUI request sent.";
      }
      #endif
      exit(0); // command executed - exiting
    } else // not connected
    {
        if (! parser.optionNames().isEmpty())
        {
           qCritical() << qPrintable(tr("No %1 service is running.\nPlease start the service first").arg(APP_NAME));
           exit(0);
        }
    }
    #endif

   //Running single instance app
    #ifdef Q_OS_LINUX
    //configFile is a system-wide config file where the system configuration is defined. No user-editable settings are saved there.
    #ifdef QT_DEBUG
    QString configFile = QFileInfo(QCoreApplication::applicationDirPath() + "/../../etc/onmydisk/onmydisk.conf").absoluteFilePath();
    m_configDir = QFileInfo(configFile).absolutePath();
    #else
    #ifdef DEPLOY
    QString configFile =  QString("/etc/onmydisk/onmydisk.conf");
    m_configDir = m_isService ? QString("/etc/onmydisk") :  QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    #else
    QString configFile = QFileInfo(QCoreApplication::applicationDirPath() + "/../../etc/onmydisk/onmydisk.conf").absoluteFilePath();
    m_configDir = QFileInfo(configFile).absolutePath();
    #endif
    #endif
    //config dir is directory where ssl keys, local database and other pesistent user-specific data is saved.
    m_logDir = m_isService ? QString("/var/onmydisk/log") : m_configDir + QString("/log");
	#else
    QString configFile = QCoreApplication::applicationDirPath() + "/../etc/onmydisk.conf";
    m_configDir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    m_logDir = m_configDir + QString("/log");
    #endif

    qInfo() << "Starting" << APP_NAME;
    qInfo() << "Version:" << APP_VERSION;

    configFile = QFileInfo(configFile).absoluteFilePath();

    QFile file(configFile);
    if ( !file.exists() )
    {
        qWarning() << configFile << "not found, default settings will be used";
    }

    //copy config file from ../etc to user's profile to allow writing user settings
    qInfo() << "System config:" << configFile;

    qInfo() << "SSL Support: " << QSslSocket::supportsSsl();
    qInfo() << "Qt compiled with: " << QSslSocket::sslLibraryBuildVersionString();
    qInfo() << "Runtime: " << QSslSocket::sslLibraryVersionString();

    LocalDB::instance().createDb(m_configDir);

    //Start IPC
    m_ipcServer = new IPCServer(m_isService);

    //Create OMD client

    /* It is complicated.
       Events flow:
       m_ipcServer (shell requests)
                                  \
                                    ---> m_omdClient --->  m_ipcServer ---> IPC clients in other processes
                                  /
       m_requestMapper (HTTP requests)
     */

#ifdef DESKTOP
    m_omdNode = new OnMyDiskDesktopNode(configFile);
    QObject::connect(m_ipcServer, &IPCServer::addShare, m_omdNode, &OnMyDiskDesktopNode::addShare);
    QObject::connect(m_ipcServer, &IPCServer::removeShare, m_omdNode, &OnMyDiskDesktopNode::removeShare);
    QObject::connect(m_ipcServer, &IPCServer::connectClient, m_omdNode, &OnMyDiskDesktopNode::connectCloud);
    QObject::connect(m_ipcServer, &IPCServer::disconnectClient, m_omdNode, &OnMyDiskDesktopNode::disconnectCloud);
    QObject::connect(m_ipcServer, &IPCServer::showStorage, m_omdNode, &OnMyDiskDesktopNode::showStorage);
    QObject::connect(m_ipcServer, &IPCServer::showCloud, m_omdNode, &OnMyDiskDesktopNode::showCloud);
    QObject::connect(m_ipcServer, &IPCServer::showShare, m_omdNode, &OnMyDiskDesktopNode::showShare);
    QObject::connect(m_ipcServer, &IPCServer::linkStorage, m_omdNode, &OnMyDiskDesktopNode::linkStorage);
    QObject::connect(m_ipcServer, &IPCServer::parseUrl, m_omdNode, &OnMyDiskDesktopNode::parseUrl);
    QObject::connect(m_ipcServer, &IPCServer::quit, m_omdNode, &OnMyDiskDesktopNode::quit);
    QObject::connect(m_ipcServer, &IPCServer::resetLink, m_omdNode, &OnMyDiskNode::resetLink);
    QObject::connect(m_omdNode, &OnMyDiskNode::stateChanged, m_ipcServer, &IPCServer::stateChanged);
    QObject::connect(m_omdNode, &OnMyDiskNode::certLoaded, m_ipcServer, &IPCServer::certLoaded);
#endif

#ifdef SERVICE
    m_omdNode = new ServiceNode(configFile);
    QObject::connect(m_ipcServer, &IPCServer::addShare, m_omdNode, &ServiceNode::addShare);
    QObject::connect(m_ipcServer, &IPCServer::removeShare, m_omdNode, &ServiceNode::removeShare);
    QObject::connect(m_ipcServer, &IPCServer::connectClient, m_omdNode, &ServiceNode::connectCloud);
    QObject::connect(m_ipcServer, &IPCServer::disconnectClient, m_omdNode, &ServiceNode::disconnectCloud);
    QObject::connect(m_ipcServer, &IPCServer::showCloud, m_omdNode, &ServiceNode::showCloud);
    QObject::connect(m_ipcServer, &IPCServer::quit, m_omdNode, &ServiceNode::quit);
    QObject::connect(m_ipcServer, &IPCServer::authRequest, m_omdNode, &OnMyDiskNode::authRequest);
    QObject::connect(m_ipcServer, &IPCServer::resetLink, m_omdNode, &OnMyDiskNode::resetLink);
    QObject::connect(m_omdNode, &OnMyDiskNode::stateChanged, m_ipcServer, &IPCServer::stateChanged);
    QObject::connect(m_omdNode, &OnMyDiskNode::certLoaded, m_ipcServer, &IPCServer::certLoaded);
#endif


#ifdef EMBEDDED
    m_omdNode = new OnMyDiskHttpNode(configFile);
    QObject::connect(m_ipcServer, &IPCServer::addShare, m_omdNode, &OnMyDiskHttpNode::addShare);
    QObject::connect(m_ipcServer, &IPCServer::removeShare, m_omdNode, &OnMyDiskHttpNode::removeShare);
    QObject::connect(m_ipcServer, &IPCServer::connectClient, m_omdNode, &OnMyDiskHttpNode::connectCloud);
    QObject::connect(m_ipcServer, &IPCServer::disconnectClient, m_omdNode, &OnMyDiskHttpNode::disconnectCloud);
    QObject::connect(m_ipcServer, &IPCServer::quit, m_omdNode, &OnMyDiskHttpNode::quit);
    QObject::connect(m_ipcServer, &IPCServer::resetLink, m_omdNode, &OnMyDiskNode::resetLink);
    QObject::connect(m_omdNode, &OnMyDiskNode::stateChanged, m_ipcServer, &IPCServer::stateChanged);
    QObject::connect(m_omdNode, &OnMyDiskNode::certLoaded, m_ipcServer, &IPCServer::certLoaded);
#endif

    // Initialize OTA updater
#if !defined(Q_OS_WIN) && !defined(Q_OS_MAC)
    OMDUpdater::instance().init();
#endif

	//QCommandLineParser *p = new QCommandLineParser();


    //Check parameters

    if (parser.isSet(addShareOption))
    {
        QString share = parser.value(addShareOption);
        if ( share != "" )
        {
            emit m_omdNode->addShare(share);
            qDebug() << "Adding" << share;
            emit m_omdNode->showShare(share);
        }
    } else
    if (parser.isSet(removeShareOption))
    {
        QString share = parser.value(removeShareOption);
        if (share != "")
        {
            qDebug() << "Removing" << share;
            emit m_omdNode->removeShare(share);
        }
    }
#ifdef DESKTOP
    else
    if (parser.isSet(urlOption))
    {
           QString url = parser.value(urlOption);
           qDebug() << "Parsing url";
           emit m_omdNode->parseUrl(url);
    } else
    if (!parser.isSet(trayOption) &&  (parser.isSet(showOption) || parser.optionNames().count() == 0))
    {
       emit m_omdNode->showStorage();
    }
#endif

#ifdef LOGGING
    QString logFile = m_logDir + QString("/onmydisk.log");
    QDir d;
    d.mkpath(m_logDir);
    m_logger = new FileLogger(configFile, logFile, 10000);
    m_logger->installMsgHandler();
#endif
}

//stop service - disconnect client
void Daemon::stop()
{
   qInfo() << "Stopping On My Disk Daemon.";

  if (m_ipcServer)
  {
     m_ipcServer->deleteLater();
     m_ipcServer = nullptr;
  }

  if (m_omdNode)
  {
      m_omdNode->close();
      m_omdNode->deleteLater();
      m_omdNode = nullptr;
  }

  #ifdef LOGGING
  if (m_logger)
  {
        m_logger->deleteLater();
        m_logger = nullptr;
  }
#endif
}


QString formatBytes(qulonglong bytes)
{
    const char *sizes[5] = { "B", "KB", "MB", "GB", "TB" };

    int i;
    double dblByte = bytes;
    for (i = 0; i < 5 && bytes >= 1024; i++, bytes /= 1024)
        dblByte = bytes / 1024.0;

    QString sSize = QString::number(dblByte, 'f', 2);
    return QString("%1 %2").arg(sSize, sizes[i]);
}

void Daemon::performTest(const QString& fileName)
{
    QFile file(fileName);
    if (! file.open(QFile::ReadOnly)) {
        qDebug() << "Can not open" << fileName;
        return;
    }
    QByteArray data;
    int pos = 0;
    qint64 s1 = QDateTime::currentMSecsSinceEpoch();
    while (! file.atEnd()) {
        file.seek(pos);
        data = file.read(PACKET_SIZE);
        pos += PACKET_SIZE;
    }
    qint64 s2 = QDateTime::currentMSecsSinceEpoch();
    file.close();
    uint rate = (file.size() / ((s2 - s1) / 1000));
    qDebug() << "Size:" << qPrintable(QString("%1").arg(formatBytes(file.size())));
    qDebug() << "Time:" << qPrintable(QString("%1 s").arg((s2 - s1) / 1000));
    qDebug() << "Read rate:" << qPrintable(QString("%1/s").arg(formatBytes(rate)));
}


