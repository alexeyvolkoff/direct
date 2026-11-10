/******************************************************************************
**
**      omdclient.cpp - On My Disk Client
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

#include "omdclient.h"
#include <QDebug>
#include <QFileInfo>
#include <QFile>
#include <QDir>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStorageInfo>
#include <QNetworkProxy>
#include <QNetworkInterface>
#include <QNetworkAddressEntry>
#include <QHostAddress>
#include <QTcpServer>
#include <QStandardPaths>
#include <QRegularExpression>
#include <QCoreApplication>
#ifdef Q_OS_LINUX
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/fsuid.h>
#include <pwd.h>
#include <grp.h>
#endif

#ifdef DESKTOP
#include <QDesktopServices>
#include <QMessageBox>
#endif

#if defined(Q_OS_WIN) && !defined(__MINGW32__)
#include <openssl/applink.c>
#endif

#include <QTimer>
#include "onmydisklinkclient.h"
#include "localdb.h"
#include "version.h"
#include "ipcclient.h"

#define EMPTY_ID "(empty)"
#define RECONNECT_INTERVAL 6000


QString formatSize(qulonglong bytes)
{
    const char *sizes[5] = { "B", "KB", "MB", "GB", "TB" };

    int i;
    double dblByte = bytes;
    for (i = 0; i < 5 && bytes >= 1024; i++, bytes /= 1024)
        dblByte = bytes / 1024.0;

    QString sSize = QString::number(dblByte, 'f', 2);
    return QString("%1 %2").arg(sSize, sizes[i]);
}


OnMyDiskClient::OnMyDiskClient(QSettings *settings, HostResource *hostResource,  QObject *parent)
    : QObject(parent),
      m_settings(settings),
      m_state(OnMyDisk::Disconnected),
      m_statusMessage(""),
      m_deviceId(""),
      m_hostResource(hostResource)
{

	m_settings->setIniCodec("UTF-8");
    qRegisterMetaType<OnMyDisk::ConnectionState>("ConnectionState");
    qRegisterMetaType<OnMyDisk::DriveInfo>("DriveInfo");
    qRegisterMetaType<QtMsgType>("QtMsgType");

    m_service = isService();
    m_needReconnect = true;
    m_loading = true;

    m_statusMessage = tr("Disconnected.");

    m_quitOnDisconnect = false;
    m_connectorLeds = false;

    connect(&m_reconnectTimer, &QTimer::timeout, this, &OnMyDiskClient::signIn);
    #ifdef EMBEDDED
    #ifndef NO_QDRIVE
    m_reloadConfigTimer.setSingleShot(true);
    connect(&m_reloadConfigTimer, &QTimer::timeout, this, &OnMyDiskClient::doReloadConfig);
    #endif
    #endif

    m_deviceId = EMPTY_ID;

    #ifdef ROUTER
    #ifdef INNBOX
    m_keyLogin = m_settings->value("Network/keyLogin", true).toBool();
    #else
    m_keyLogin = m_settings->value("Network/keyLogin", false).toBool();
    #endif
    #elif defined(EMBEDDED)
    m_keyLogin = m_settings->value("Network/keyLogin", true).toBool();
    #else
    m_keyLogin = false;
    #endif

    #ifdef EMBEDDED

    #ifdef A20_GPIO
    //Leds on Connector board
    m_connectorLeds = m_settings->value("Devices/Leds", true).toBool();
    qInfo() << "Showing leds:" << m_connectorLeds;
    if (m_connectorLeds)
    {
        qInfo() << "Initializing A20 GPIO";

        A20::GPIO::init();
        m_redLed = A20::GPIO::get_output(A20_GPIO_LED_OFFSET + OnMyDiskClient::Red);
        m_greenLed = A20::GPIO::get_output(A20_GPIO_LED_OFFSET + OnMyDiskClient::Green);
        m_blueLed = A20::GPIO::get_output(A20_GPIO_LED_OFFSET + OnMyDiskClient::Blue);

        showLed(Red, false);
        showLed(Green, true);
        showLed(Blue, false);
    }
    #endif
    #ifndef NO_QDRIVE
    connect(&m_driveMonitor, &QDrive::Monitor::fileSystemAdded, this, &OnMyDiskClient::deviceAdded);
    connect(&m_driveMonitor, &QDrive::Monitor::fileSystemRemoved, this, &OnMyDiskClient::deviceRemoved);
    #endif
    #endif

    //restart Shared links backend on error
    connect(&LocalDB::instance(), &LocalDB::error, this,  &OnMyDiskClient::linkError);

}


#ifdef EMBEDDED
#ifndef NO_QDRIVE
void OnMyDiskClient::reloadConfig()
{
    m_reloadConfigTimer.start(300);
}

void OnMyDiskClient::updateSmbConf()
{
#ifdef Q_OS_LINUX
        // Restrict SMB share functionality to Linux client running as a daemon from root
        if (::getuid() != 0) {
            qWarning() << "SMB share configuration is only supported when running as root daemon";
            return;
        }

        // Optional system user for force user/group in SMB shares.
        // The system user is only required for file worker operations (read/write),
        // not for SMB share configuration itself.
        QString sysUser = m_settings->value("FileNode/User", "").toString();
        QString sysGroup = m_settings->value("FileNode/Group", "").toString();
        if (sysGroup.isEmpty() && !sysUser.isEmpty()) {
            sysGroup = sysUser; // default group to username
        }

        QString general_entry = "#Autogenerated by On My Disk\n\n"
            "[global]\n"
            "   workgroup = ONMYDISK\n"
            "   security = user\n"
            "   map to guest = Never\n"
            "   writable = yes\n"
            "   browseable = yes\n"
            "   read only = no\n"
            "   local master = yes\n"
            "   preferred master = yes\n"
            "   domain master = yes\n"
            "   os level = 255\n";

        // Bind nmbd to the first active non-loopback IPv4 interface to avoid
        // browser elections on virtual subnets (docker0, etc.), where the master
        // browser address is unreachable from the LAN and the device disappears
        // from network neighborhood.
        QString lanIface;
        const QList<QNetworkInterface> interfaces = QNetworkInterface::allInterfaces();
        for (const QNetworkInterface &iface : interfaces) {
            if (iface.flags().testFlag(QNetworkInterface::IsUp) &&
                iface.flags().testFlag(QNetworkInterface::IsRunning) &&
                !iface.flags().testFlag(QNetworkInterface::IsLoopBack)) {
                for (const QNetworkAddressEntry &entry : iface.addressEntries()) {
                    if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol) {
                        lanIface = iface.name();
                        break;
                    }
                }
            }
            if (!lanIface.isEmpty())
                break;
        }
        if (!lanIface.isEmpty()) {
            general_entry += "   interfaces = " + lanIface + "\n"
                             "   bind interfaces only = yes\n";
        }

        if (!sysUser.isEmpty()) {
            general_entry +=
                "   force create mode = 0660\n"
                "   force directory mode = 2770\n"
                "   force user = " + sysUser + "\n"
                "   force group = " + sysGroup + "\n";
        }

        QString share_entry = "[%1]\n"
            "   path = %2\n";

        QString config = general_entry;
        foreach(QString sharePath, m_shares)
        {
            QString shareName = m_shares.key(sharePath);
            config += "\n" + share_entry.arg(shareName, sharePath);
        }


        QFile smb_file("/etc/samba/smb.conf");
        if (smb_file.open(QFile::WriteOnly | QFile::Text)) {
            smb_file.write(config.toUtf8());
            smb_file.close();

            QProcess restart;
            qDebug() << "Reloading SMB config";
            restart.startDetached("/bin/systemctl",
            QStringList() <<  "reload" << "smbd");
            restart.waitForStarted(1000);
        } else {
            qWarning() << "Failed to write to /etc/samba/smb.conf";
        }
#else
        qWarning() << "SMB shares are only supported on Linux";
#endif
}


void OnMyDiskClient::updateSitesConf(const QString& action, const QString& site)
{
        QProcess restart;
        if (action == "restart") {
            qDebug() << "Restarting sites";
            restart.startDetached("/bin/systemctl", QStringList() <<  "restart" << "sites");
            restart.waitForStarted(1000);
        } else if (action == "remove" && !site.isEmpty()) {
            qDebug() << "Stopping sites";
            restart.startDetached("/bin/systemctl", QStringList() <<  "stop" << "sites");
            restart.waitForStarted(1000);
            restart.waitForFinished(1000);
            qDebug() << "Removing site" << site;
            QString sitePath = m_omdSettings.sitesRoot() + "/" + site;
            QDir d(sitePath);
            d.removeRecursively();

            qDebug() << "Starting sites";
            restart.startDetached("/bin/systemctl", QStringList() <<  "start" << "sites");
            restart.waitForStarted(1000);
        }
}
#endif
#endif

#ifdef A20_GPIO
void OnMyDiskClient::showLed(OnMyDiskClient::Led led, bool on)
{
    switch (led) {
        case OnMyDiskClient::Red: {
            m_redLed->set(on);
            break;
        }
        case OnMyDiskClient::Green: {
            m_greenLed->set(on);
            break;
        }
        case OnMyDiskClient::Blue: {
            m_blueLed->set(on);
            break;
        default: break;
        }
    }
}
#endif



void OnMyDiskClient::linkError(const QString& item )
{
    qCritical() << "Link error:"  << item;
}

void OnMyDiskClient::updateStorageId(QString storageId, QString linkId, QString validTo)
{
    m_deviceId = storageId;
    m_linkId = linkId;
    m_validTo = validTo;
    emit storageIdUpdated(m_deviceId, m_linkId, m_validTo);
}


void OnMyDiskClient::setState(OnMyDisk::ConnectionState state, QString statusMessage)
{
    m_statusMessage = statusMessage;
    switch (state)
    {
            case OnMyDisk::Connecting:
			case OnMyDisk::Authorization: {
                        #ifdef A20_GPIO
                        if (m_connectorLeds)
                        {
                            showLed(Red, true);
                            showLed(Green, false);
                            showLed(Blue, true);
                        }
                        #endif
                        break;
            }
            case OnMyDisk::Disconnected: {
                        m_statusMessage = tr("Offline");
                        [[fallthrough]];
            }
            case OnMyDisk::ConnectionError:
            case OnMyDisk::AuthorizationError: {
                        if (m_needReconnect)
                        {
                            state = OnMyDisk::Connecting;
                            #ifdef A20_GPIO
                            if (m_connectorLeds)
                            {
                                showLed(Red, true);
                                showLed(Green, false);
                                showLed(Blue, false);
                            }
                            #endif
                            m_reconnectTimer.start(RECONNECT_INTERVAL);
                        } else
                        {
                            #ifdef A20_GPIO
                            if (m_connectorLeds)
                            {
                                showLed(Red, false);
                                showLed(Green, true);
                                showLed(Blue, false);
                            }
                            #endif
							m_reconnectTimer.stop();
                        }
                        break;
            }
            case OnMyDisk::Connected: {
                        m_statusMessage = tr("Online");
                        #ifdef A20_GPIO
                        if (m_connectorLeds)
                        {
                            showLed(Red, false);
                            showLed(Green, false);
                            showLed(Blue, true);
                        }
                        #endif
                        m_needReconnect = true;
                        m_reconnectTimer.stop();
            }
   }

   m_state = state;
   emit stateChanged(m_state, m_statusMessage);
}

void OnMyDiskClient::addNamedShare(QString shareName, QString sharePath, bool persistent)
{
    if (!m_shares.keys().contains(shareName))
    {
        qInfo() << "Adding share" << shareName << "path:" << sharePath;
        m_shares[shareName] = sharePath;
        m_persistentShare[shareName] = persistent;
        emit shareAdded(shareName, sharePath, persistent);
        if (!m_loading && persistent) {
            saveShares();
            m_settings->sync();
        }
    } else {
        qWarning() << "Share" << shareName << "already exists!";
    }
}

void OnMyDiskClient::addShare(QString share)
{
    #ifdef Q_OS_WIN
        QString shareName = QFileInfo(share).completeBaseName();
        addNamedShare(shareName, share);
    #else
    int p = share.indexOf(":");
	if (p > 0) {
		QString shareName = share.left(p);
		QString path = share.mid(p + 1);
		addNamedShare(shareName, path);
	} else {
		QString shareName = QFileInfo(share).completeBaseName();
		addNamedShare(shareName, share);
	}
    #endif
}


void OnMyDiskClient::removeShare(QString shareName)
{
    //remove shared folder
    if (m_shares.keys().contains(shareName))
    {
        qDebug() << "Removing share" << shareName;
        m_shares.remove(shareName);
        m_hostResource->removeShare(shareName);
        emit shareRemoved(shareName);
        saveShares();
        m_settings->sync();
    }
}


void OnMyDiskClient::shareAddRequested(QString user, QString path)
{
    Q_UNUSED(user)
    QString localPath = path;
    qInfo() << "Share add requested:" << localPath;
    if (localPath.startsWith("/")) {
         // Check if it's already a valid absolute path
         QFileInfo check(localPath);
         if (!check.exists()) {
             localPath.remove(0, 1); // Treat as relative to root
         }
    }

    if (!QFileInfo(localPath).isAbsolute()) {
        QString root;
#ifdef Q_OS_LINUX
        QString fileNodeUser = m_settings ? m_settings->value("FileNode/User", "").toString() : "";
        if (!fileNodeUser.isEmpty()) {
            struct passwd *pwd = getpwnam(fileNodeUser.toUtf8().constData());
            if (pwd) root = QString::fromUtf8(pwd->pw_dir);
        }
#endif
        if (root.isEmpty() || !QFileInfo(root).exists()) {
            root = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
        }
        localPath = QDir(root).absoluteFilePath(localPath);
    }
    qInfo() << "Resolved local path:" << localPath;
    QFileInfo shareInfo(localPath);
    if (shareInfo.exists() && shareInfo.isReadable()) {
        QString shareName = shareInfo.fileName();
        addNamedShare(shareName, localPath, true);
    } else {
        qWarning() << "Path does not exist or not readable:" << localPath;
    }
}

void OnMyDiskClient::shareRemoveRequested(QString user, QString sharePath)
{
    Q_UNUSED(user)
    QString shareName = sharePath.split("/", Qt::SplitBehaviorFlags::SkipEmptyParts).last();
    removeShare(shareName);
}


void OnMyDiskClient::writeSettings()
{
   //save settings
   saveSettings();
   saveShares();
   emit settingsSaved(m_omdSettings);
   m_settings->sync();
}

void OnMyDiskClient::saveShares()
{
#ifndef INNBOX
   m_settings->beginGroup("Shares");
   m_settings->remove(""); //cleanup the section
   foreach (QString shareName, m_shares.keys())
   {
     if (m_persistentShare[shareName])
       m_settings->setValue(shareName, m_shares[shareName]);
   }
   m_settings->endGroup();
#endif
}

void OnMyDiskClient::saveSettings()
{
   //save settings
   m_settings->beginGroup("Settings");
   m_omdSettings.writeSettings(m_settings);
   m_settings->endGroup();

}

void OnMyDiskClient::readSettings()
{
    m_shares.clear();

	m_connectOnStart = m_settings->value(("Network/connect"), true).toBool();

    #ifdef EMBEDDED
    m_settings->beginGroup("Devices");
    m_ignoredDevices = m_settings->value(("IgnoredDevices"), QString("")).toString().split(',', Qt::SplitBehaviorFlags::SkipEmptyParts);
    m_settings->endGroup();
    #endif

    //loading saved user settings
    m_settings->beginGroup("Settings");
    m_omdSettings.readSettings(m_settings);
    m_settings->endGroup();

#if ((EMBEDDED || SERVICE || defined(DESKTOP) || defined(SERVER)) && !defined(ROUTER))
    if (!m_omdSettings.omdUser().isEmpty()) {
        m_omdSettings.setUser(m_omdSettings.omdUser());
    }
#endif

    #ifdef DESKTOP
    //setAutostart(m_omdSettings.autostart());
    #endif

    //Shared folders is common part
	qDebug() << "Adding static shares";
	m_settings->beginGroup("Shares");
    QStringList shareNames = m_settings->allKeys();

	foreach (QString shareName, shareNames)
    {
        if (!shareName.isEmpty()) {
           QString sharePath = m_settings->value(shareName).toString();
           if (QFileInfo(sharePath).exists())
            addNamedShare(shareName, sharePath, true);
        }
    }
    m_settings->endGroup();

    #ifdef EMBEDDED
    #ifndef NO_QDRIVE
    //Detect and mount block devices
    if (m_omdSettings.automount()) {
    }
    #endif
    #endif

    m_omdSettings.setResetId(false);

    applySettings(m_omdSettings);
    m_loading = false;


	m_plainLinkCreator = new PlainLinkCreator(m_omdSettings.linkId(), this);

#ifdef EMBEDDED
#ifdef DEPLOY_CONSOLE_LOG
    qInfo() << "Logging to web console.";
    connect(&m_consoleLogger, &ConsoleLogger::logOutput, this, &OnMyDiskClient::logOutput);
    m_consoleLogger.installMsgHandler();
#endif
#endif
    emit settingsLoaded(m_omdSettings);
}

void OnMyDiskClient::accountCreated(QString server, QString user)
{
    m_omdSettings.setUser(user);
    m_omdSettings.setServer(server);

    m_omdSettings.setResetId(false);
    writeSettings();

    emit settingsLoaded(m_omdSettings);

}

void OnMyDiskClient::deviceIdUpdated(QString deviceId)
{
    m_deviceId = deviceId;
    m_omdSettings.setStorageId(deviceId);
    emit storageIdUpdated(m_deviceId, m_omdSettings.linkId(),  m_validTo);
    #ifdef EMBEDDED
    #ifndef NO_QDRIVE
    //update SAMBA/DLNA shares
    if (m_service)
        reloadConfig();
    #endif
    #endif
}

void OnMyDiskClient::applySettings(const OnMyDiskSettings &settings)
{

   OnMyDiskSettings newSettings =  settings;
   //Check if new cert is needed
   bool reset =  newSettings.password() != "" || newSettings.resetId()  ||  m_omdSettings.server() != newSettings.server() || m_omdSettings.user() != newSettings.user();
   //Or just reconnect
   bool doConnect = reset || m_state != OnMyDisk::Connected  || m_omdSettings.useProxy() != newSettings.useProxy()
                       #if ((EMBEDDED || SERVICE) && !defined(ROUTER))
                       || m_omdSettings.sharedWith() != newSettings.sharedWith() || m_omdSettings.sitesRoot() != newSettings.sitesRoot() || m_omdSettings.sitesEnabled() != newSettings.sitesEnabled()
                       || m_omdSettings.autoReindex() != newSettings.autoReindex() || m_omdSettings.localAI() != newSettings.localAI() || m_omdSettings.localSearch() != newSettings.localSearch()
                                             || m_omdSettings.localSearchBase() != newSettings.localSearchBase()
                       #endif
                       || m_omdSettings.server() != newSettings.server() || m_omdSettings.secure() != newSettings.secure()
                        || m_omdSettings.port() != newSettings.port() || m_omdSettings.hostName() != newSettings.hostName()
#if ((EMBEDDED || SERVICE || defined(DESKTOP) || defined(SERVER)) && !defined(ROUTER))
                        || m_omdSettings.rcloneWebdavPort() != newSettings.rcloneWebdavPort()
#endif
                        ;

   if (m_loading)
   {
	   doConnect = doConnect && m_connectOnStart;
   }

   #if defined(EMBEDDED) && !defined(ROUTER)
   LocalResource::generateVidoCovers = newSettings.generateIconsFromVideos();
   #elif defined(SERVICE)
   LocalResource::generateVidoCovers = true;
   #else
   LocalResource::generateVidoCovers = false;
   #endif

    if (newSettings.resetId() )
    {
        doResetLink();
        newSettings.setResetId(false); //reset reset
    }

   m_omdSettings = newSettings;

#if ((EMBEDDED || SERVICE || defined(DESKTOP) || defined(SERVER)) && !defined(ROUTER))
   {
       QVariantList rcloneShares = m_omdSettings.rcloneShares();
       updateRcloneConfig(rcloneShares);
       updateRcloneProcesses(rcloneShares);
       m_omdSettings.setRcloneShares(rcloneShares);
   }
#endif

   //save settings
   saveSettings();
   m_settings->sync();

   // update link client settings before reset/reconnect
   emit settingsLoaded(m_omdSettings);

    if (reset)
    {
        // Keep persistent device.id (Device Key) intact, m_linkClient will handle cert update in updateSettings()
    }

    // Connect if needed and account exists
    if (doConnect)
    {
 	   if (m_state == OnMyDisk::Connected)
       {
 		   reconnect();
       }
       // connect
       signIn();
    }
   m_omdSettings.setPassword("");
}

void OnMyDiskClient::linkConnected()
{
    setState(OnMyDisk::Connected, tr("Connected"));
    qDebug() << "Link connected";
}

void OnMyDiskClient::linkConnecting()
{
    setState(OnMyDisk::Connecting, tr("Connecting..."));
    qDebug() << "Link connecting";
}


void OnMyDiskClient::linkDisconnected()
{
    //Do not loose error state
    if (m_state != OnMyDisk::AuthorizationError && m_state != OnMyDisk::ConnectionError  && m_state != OnMyDisk::Disconnected)
        setState(OnMyDisk::Disconnected, tr("Disconnected"));

    qDebug() << "Link disconnected";
    if (m_quitOnDisconnect)
    {
        qDebug() << "Exiting";

        #ifdef A20_GPIO
        A20::GPIO::free(A20_GPIO_LED_OFFSET + OnMyDiskClient::Red);
        A20::GPIO::free(A20_GPIO_LED_OFFSET + OnMyDiskClient::Green);
        A20::GPIO::free(A20_GPIO_LED_OFFSET + OnMyDiskClient::Blue);
        #endif

        writeSettings();

        qApp->quit();
    }
}



void OnMyDiskClient::signIn()
{
    if (m_state != OnMyDisk::Connected) {
        m_needReconnect = true;
        setState(OnMyDisk::Connecting, tr("Connecting..."));

        qDebug() << "Connecting...";
        emit connectCloud();

        m_reconnectTimer.start(RECONNECT_INTERVAL);
    }
}


void OnMyDiskClient::signOut()
{
	m_reconnectTimer.stop();
	m_needReconnect = false;
	setState(OnMyDisk::Disconnected, tr("Disconnected"));
	emit disconnectCloud();
}

void OnMyDiskClient::reconnect()
{
	setState(OnMyDisk::Disconnected, tr("Reconnecting"));
	qDebug() << "Reconnecting...";
    m_needReconnect = true;
    emit disconnectCloud();
}



void OnMyDiskClient::doResetLink()
{
    qDebug() << "Device id reset requested";
    m_validTo = "";
    m_deviceId = EMPTY_ID;
    m_reconnectTimer.stop();
	#ifndef ROUTER
    m_needReconnect = false;
	#endif
    //Do not loose error state
    if (m_state != OnMyDisk::AuthorizationError && m_state != OnMyDisk::ConnectionError  && m_state != OnMyDisk::Disconnected)
        setState(OnMyDisk::Disconnected, tr("Reset requested"));
    emit resetLink();
}

void OnMyDiskClient::connectionError(QAbstractSocket::SocketError error)
{
    qDebug() << "Connection error:" << error;
	if (m_needReconnect)
	{
		qDebug() << "Reconnecting...";
	}
    if (m_state != OnMyDisk::AuthorizationError && m_state != OnMyDisk::ConnectionError  && m_state != OnMyDisk::Disconnected)
        setState(OnMyDisk::ConnectionError, tr("Connection error"));
}


void OnMyDiskClient::pkiStatusChange(QString statusMessage)
{
    setState(OnMyDisk::Authorization,  statusMessage);
}

void OnMyDiskClient::pkiError(QString errorMessage)
{
    m_needReconnect = m_keyLogin;
    //m_omdSettings.setResetId(true);
    qDebug() << "PKI error:" << errorMessage << "Retrying:" << m_needReconnect;
    setState(OnMyDisk::AuthorizationError,  errorMessage);
}

void OnMyDiskClient::sslError(QList<QSslError> errorList, bool &ignore)
{
    //ignore all ssl errors except NoSslSupport / definitive certificate rejection
    Q_FOREACH(QSslError error, errorList)
    {
        if (error == QSslError::NoSslSupport || error == QSslError::CertificateExpired ||
            error == QSslError::CertificateRejected || error == QSslError::CertificateRevoked ||
            error == QSslError::CertificateSignatureFailed)
        {
            m_needReconnect = false;
            break;
        }
    }
    ignore = m_needReconnect;
}

void OnMyDiskClient::certLoaded(QString commonName, QString deviceId, QString linkId, QString validFrom, QString validTo)
{
    Q_UNUSED(validFrom)
    m_commonName = commonName;

    bool updated = (m_omdSettings.user() != commonName || m_omdSettings.storageId() != deviceId || m_omdSettings.linkId() != linkId );
    m_omdSettings.setUser(commonName);
    m_omdSettings.setStorageId(deviceId);
    m_omdSettings.setValidTo(validTo);
    if (m_omdSettings.linkId() != linkId) {
        //make link id persistent
        m_omdSettings.setLinkId(linkId);
        saveSettings();
        m_settings->sync();
    }

    updateStorageId(deviceId, linkId,  validTo);

    if (updated)
        emit settingsLoaded(m_omdSettings);

    if (m_plainLinkCreator) {
        m_plainLinkCreator->updateStorageId(m_omdSettings.linkId(), validTo);
    }

    qDebug() << "Certificate loaded for" << m_commonName << "valid to" << validTo;
    linkConnecting();
#ifdef EMBEDDED
#ifndef ROUTER
#ifndef NO_QDRIVE
    detectPartitions();
    //update SAMBA/DLNA shares
    if (m_service)
        reloadConfig();
#endif
#endif
#endif
}


void OnMyDiskClient::quit()
{
   m_quitOnDisconnect = true;
   if (m_state == OnMyDisk::Connected) {
     signOut();
   } else {
     linkDisconnected();
     qApp->quit();
   }
}




#ifdef EMBEDDED
#ifndef NO_QDRIVE
void OnMyDiskClient::doReloadConfig()
{
   if (m_omdSettings.updateSamba())
   {
     updateSmbConf();
   }

   if (m_omdSettings.sitesEnabled() && !m_omdSettings.sitesRoot().isEmpty() )
   {
     updateSitesConf("restart");
   }
}

void OnMyDiskClient::deviceAdded(const QDBusObjectPath& path)
{

    if (!m_omdSettings.automount())
        return;

    QDrive::BlockDevice device(path);

    if (! device.hasFileSystem())
    {
        qDebug() << "No filesystem on" << path.path();
        return;
    }

    if (! device.isValid())
    {
        //qDebug() << "Not valid device"  << path.path();
        return;
    }

    if (device.isSystem())
    {
        // qDebug() << "Ignoring system device"  << path.path();
        return;
    }

    QString devicePath = device.device();
    foreach(QString ignored, m_ignoredDevices)
    {
        if (devicePath.startsWith(ignored))
        {
            qDebug() << "Ignored device"  << path.path();
            return;
        }
    }

    QStringList mountPoints;

    try {

        QDrive::FileSystem fs(path);
        if (! fs.isMounted()) {
            qInfo() << "Mounting:"  << path.path();
            fs.mount();
        }

        if (! fs.isMounted()) {
            qWarning() << "Not mounted:"  << path.path();
            return;
        }

        mountPoints =  fs.mountPoints();

    } catch (const QDrive::DBusException &e) {
        qCritical() << "DBus error:"  << e.what();
    }

    qDebug() << "MountPoints" << mountPoints;

    if (mountPoints.count() == 0) //retry later
    {
        qWarning() << "Retrying to mount later";
        QTimer::singleShot(3000, this, [this] () {
             detectPartitions();
        });
        return;
    }

    if (device.drive().path().isEmpty() )
    {
        qDebug() << "No drive info"  << path.path();
        return;
    }

    QDrive::Drive drive (device.drive());

    QString deviceLabel = device.label();

    if (deviceLabel.isEmpty() && !drive.vendor().isEmpty() && !drive.model().isEmpty()) {
        deviceLabel = QString("%1-%2").arg(drive.vendor(), drive.model());
    }

    if ( deviceLabel.isEmpty() )
    {
        qDebug() << "Cannot detect drive label"  << path.path();
        return;
    }

    qDebug() << "Adding device" << deviceLabel << device.type();

    int c = 0;
    Q_FOREACH(QString mountPoint, mountPoints )
    {
        QString shareName = mountPoints.count() == 1 ? deviceLabel : QString("%1-%2").arg(deviceLabel, c++);
        //add share
        if (!m_shares.keys().contains(shareName))
            addNamedShare(shareName, mountPoint, false);
    }

    //save path for unmount
    m_mountedShares[path.path()] = mountPoints;

    //update SAMBA/DLNA share
    if (! m_loading && m_service)
    {
        reloadConfig();
    }
}

void OnMyDiskClient::deviceRemoved(const QDBusObjectPath &path)
{
    qDebug() << "Removing" << path.path();
    //QString devicePath = path.path();

    //QDrive::FileSystem fs(path);

    QStringList mountPoints =  m_mountedShares[path.path()];

    Q_FOREACH(QString mountPoint, mountPoints ) {
        QString shareName = m_shares.key(mountPoint);
        removeShare(shareName);
    }

    m_mountedShares.remove(path.path());
    reloadConfig();
}

void OnMyDiskClient::detectPartitions()
{
    try {
        qDebug() << "Detecting mounted filesystems:";
        Q_FOREACH(const QDBusObjectPath &path, QDrive::fileSystems(100))
            deviceAdded(path);
    } catch(const QDrive::DBusException &) {
        qWarning() << "DBus timeout";
    }
}
#endif


void OnMyDiskClient::addFolder(const QString &path)
{


    QString devicePath = path;
    QString deviceLabel = QFileInfo(path).fileName();
    if (!  m_shares.keys().contains(deviceLabel))
    {
      QString size =  formatSize(QStorageInfo(path).bytesTotal());
      qDebug() << "Adding:" << deviceLabel << ", path:"<< devicePath;
      addNamedShare(deviceLabel, devicePath);
    }
}

void OnMyDiskClient::removeFolder(const QString &path)
{
    QString devicePath = path;
    QString deviceLabel = QFileInfo(path).fileName();
    if (!m_shares.keys().contains(deviceLabel))
    {
      qDebug() << "Remove:" << deviceLabel << ", path:"<< devicePath;
      removeShare(deviceLabel);
    }
}

void OnMyDiskClient::restartSites(const QString &action, const QString &site)
{
#ifndef NO_QDRIVE
    updateSitesConf(action, site);
#else
    Q_UNUSED(action)
    Q_UNUSED(site)
#endif
}
#endif


OnMyDiskClient::~OnMyDiskClient()
{
    for (QProcess *proc : m_rcloneProcesses.values()) {
        proc->kill();
        proc->waitForFinished(1000);
        delete proc;
    }
    m_rcloneProcesses.clear();

    writeSettings();

	if (m_plainLinkCreator)
		delete m_plainLinkCreator;
}

#ifdef Q_OS_LINUX
static bool resolveRcloneUser(const QString &username, const QString &groupname, uid_t &uid, gid_t &gid, QString &homeDir)
{
    bool hasUid = false;
    if (!username.isEmpty()) {
        struct passwd *pwd = getpwnam(username.toUtf8().constData());
        if (pwd) {
            uid = pwd->pw_uid;
            gid = pwd->pw_gid;
            homeDir = QString::fromUtf8(pwd->pw_dir);
            hasUid = true;
        }
    }
    if (!groupname.isEmpty()) {
        struct group *grp = getgrnam(groupname.toUtf8().constData());
        if (grp) {
            gid = grp->gr_gid;
        }
    }
    return hasUid;
}
#endif

QString OnMyDiskClient::systemFileNodeValue(const QString &key) const
{
    // FileNode/User and FileNode/Group live in the SYSTEM config (e.g. /etc/onmydisk/onmydisk.conf),
    // while m_settings points to the user profile file (onmydisk.conf.user).
    QString path = m_settings->fileName();
    if (path.endsWith(QStringLiteral(".user")))
        path.chop(5);
    QSettings sys(path, QSettings::IniFormat);
    sys.setIniCodec("UTF-8");
    return sys.value(QStringLiteral("FileNode/") + key, QString()).toString();
}

QString OnMyDiskClient::rcloneConfigPath() const
{
#ifdef Q_OS_LINUX
    //When running as a service (root), use the system user's config instead of root's home
    if (isService()) {
        uid_t uid; gid_t gid; QString home;
        if (resolveRcloneUser(systemFileNodeValue("User"), QString(), uid, gid, home)) {
            return home + "/.config/rclone/rclone.conf";
        }
    }
#endif
#ifdef Q_OS_WIN
    QString appData = qEnvironmentVariable("APPDATA");
    if (!appData.isEmpty()) {
        QString winConf = QDir::fromNativeSeparators(appData) + "/rclone/rclone.conf";
        QString dotConf = QDir::homePath() + "/.config/rclone/rclone.conf";
        if (!QFileInfo::exists(winConf) && QFileInfo::exists(dotConf))
            return dotConf;
        return winConf;
    }
#endif
    return QDir::homePath() + "/.config/rclone/rclone.conf";
}

bool OnMyDiskClient::applyRcloneUserPrivileges(QProcess *proc, long &uid, long &gid)
{
#ifdef Q_OS_LINUX
    if (!isService()) return false; //running as a normal user, rclone inherits our credentials

    QString sysUser = systemFileNodeValue("User");
    QString sysGroup = systemFileNodeValue("Group");
    uid_t resolvedUid = 0; gid_t resolvedGid = 0; QString homeDir;
    if (!resolveRcloneUser(sysUser, sysGroup, resolvedUid, resolvedGid, homeDir)) {
        qWarning() << "rclone: FileNode/User not configured, running rclone with root privileges";
        return false;
    }
    uid = resolvedUid;
    gid = resolvedGid;
    qInfo() << "DBG applyRcloneUserPrivileges: resolved" << sysUser << "uid:" << uid << "gid:" << gid << "home:" << homeDir;

    //Point rclone at the user's config and cache (rclone resolves these via $HOME)
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert("HOME", homeDir);
    env.insert("XDG_CONFIG_HOME", homeDir + "/.config");
    env.insert("XDG_CACHE_HOME", homeDir + "/.cache");
    env.insert("USER", sysUser);
    env.insert("LOGNAME", sysUser);
    proc->setProcessEnvironment(env);
    return true;
#else
    Q_UNUSED(proc)
    Q_UNUSED(uid)
    Q_UNUSED(gid)
    return false;
#endif
}

QString OnMyDiskClient::obscurePassword(const QString &password)
{
    QProcess proc;
    proc.start("rclone", QStringList() << "obscure" << password);
    if (proc.waitForFinished(5000)) {
        return QString::fromUtf8(proc.readAllStandardOutput()).trimmed();
    }
    return QString();
}

void OnMyDiskClient::updateRcloneConfig(QVariantList &shares)
{
    QString configPath = rcloneConfigPath();
    qInfo() << "DBG updateRcloneConfig: shares:" << shares.size() << "configPath:" << configPath;
    QDir().mkpath(QFileInfo(configPath).absolutePath());
    QSettings rcloneSettings(configPath, QSettings::IniFormat);
    const QStringList existingGroups = rcloneSettings.childGroups();

    QHash<QString, int> typeCounts;
    for (const QVariant &shareVar : shares) {
        const QString t = shareVar.toMap().value("type").toString();
        if (!t.isEmpty())
            typeCounts[t] = typeCounts.value(t, 0) + 1;
    }

    for (int i = 0; i < shares.size(); ++i) {
        QVariantMap share = shares[i].toMap();
        QString name = share["name"].toString().trimmed();
        QString type = share["type"].toString().trimmed();
        QString remote = share["remote"].toString().trimmed();
        QString username = share["username"].toString().trimmed();
        QString password = share["password"].toString();
        QString url = share["url"].toString().trimmed();
        QString host = share["host"].toString().trimmed();
        int sftpPort = share["sftpPort"].toInt();

        if (type.isEmpty()) continue;

        if (type == "rclone") {
            QString sectionName = !remote.isEmpty() ? remote : name;
            if (sectionName.endsWith(":"))
                sectionName.chop(1);
            share["remote"] = sectionName;
            shares[i] = share;
            continue;
        }

        QString sectionName = remote;
        if (sectionName.isEmpty()) {
            if ((type == "protondrive" || type == "webdav") &&
                typeCounts.value(type) == 1 &&
                (existingGroups.contains(type) || type == "protondrive")) {
                sectionName = type;
            } else {
                QString sanitized = name;
                sanitized.replace(QRegularExpression("[^A-Za-z0-9_-]"), "_");
                sectionName = QStringLiteral("omd_") + (sanitized.isEmpty() ? type : sanitized);
            }
        }
        share["remote"] = sectionName;

        rcloneSettings.beginGroup(sectionName);
        rcloneSettings.setValue("type", type);
        if (type == "protondrive") {
            if (!username.isEmpty()) rcloneSettings.setValue("username", username);
            if (!password.isEmpty() && password != "******") {
                QString obscured = obscurePassword(password);
                if (!obscured.isEmpty()) rcloneSettings.setValue("password", obscured);
            }
        } else if (type == "webdav") {
            if (rcloneSettings.value("vendor").toString().isEmpty())
                rcloneSettings.setValue("vendor", "other");
            if (!url.isEmpty()) rcloneSettings.setValue("url", url);
            if (!username.isEmpty()) rcloneSettings.setValue("user", username);
            if (!password.isEmpty() && password != "******") {
                QString obscured = obscurePassword(password);
                if (!obscured.isEmpty()) rcloneSettings.setValue("pass", obscured);
            }
        } else if (type == "sftp") {
            QString effectiveHost = !host.isEmpty() ? host : url;
            if (effectiveHost.startsWith("sftp://", Qt::CaseInsensitive))
                effectiveHost = effectiveHost.mid(7);
            if (!effectiveHost.isEmpty()) rcloneSettings.setValue("host", effectiveHost);
            if (sftpPort > 0) rcloneSettings.setValue("port", sftpPort);
            if (!username.isEmpty()) rcloneSettings.setValue("user", username);
            if (!password.isEmpty() && password != "******") {
                QString obscured = obscurePassword(password);
                if (!obscured.isEmpty()) rcloneSettings.setValue("pass", obscured);
            }
        }
        rcloneSettings.endGroup();

        if (!password.isEmpty())
            share["password"] = QStringLiteral("******");
        shares[i] = share;
    }
    rcloneSettings.sync();
#ifdef Q_OS_LINUX
    //When running as a service, ensure the configured system user can read the config
    if (isService()) {
        uid_t uid; gid_t gid; QString home;
        if (resolveRcloneUser(systemFileNodeValue("User"),
                              systemFileNodeValue("Group"), uid, gid, home)) {
            //Own the config dir too, so rclone can refresh tokens if needed
            const int configDir = ::chown(QFile::encodeName(QFileInfo(configPath).absolutePath()).constData(), uid, gid);
            Q_UNUSED(configDir)
            const int balloon = ::chown(QFile::encodeName(configPath).constData(), uid, gid);
            Q_UNUSED(balloon)
            ::chmod(QFile::encodeName(configPath).constData(), 0640);
        }
    }
#endif
}

static bool isLocalTcpPortFree(int port)
{
    if (port <= 0 || port > 65535)
        return false;
    QTcpServer probe;
    if (probe.listen(QHostAddress::LocalHost, static_cast<quint16>(port))) {
        probe.close();
        return true;
    }
    return false;
}

void OnMyDiskClient::updateRcloneProcesses(QVariantList &shares)
{
    const QString rcloneBin = QStandardPaths::findExecutable("rclone");
    if (rcloneBin.isEmpty() && !shares.isEmpty()) {
        qWarning() << "rclone binary not found in PATH; cannot start external cloud shares";
        return;
    }

    const QString configPath = rcloneConfigPath();
    QSet<QString> activeNames;
    QSet<int> assignedPorts;

    for (int i = 0; i < shares.size(); ++i) {
        QVariantMap share = shares[i].toMap();
        QString name = share["name"].toString().trimmed();
        QString type = share["type"].toString().trimmed();
        QString remote = share["remote"].toString().trimmed();
        QString remotePath = share["remotePath"].toString().trimmed();
        if (remote.isEmpty())
            remote = type;
        if (remote.endsWith(":"))
            remote.chop(1);
        while (remotePath.startsWith("/"))
            remotePath.remove(0, 1);

        int port = share["port"].toInt();
        if (port <= 0)
            port = 19088;

        if (name.isEmpty() || remote.isEmpty()) continue;
        activeNames.insert(name);

        // If already running, check if parameters changed or process exited
        if (m_rcloneProcesses.contains(name)) {
            QProcess *proc = m_rcloneProcesses[name];
            QVariantMap runningMeta = proc->property("meta").toMap();
            bool dead = (proc->state() == QProcess::NotRunning);
            bool changed = (runningMeta["type"].toString() != type ||
                            runningMeta["remote"].toString() != remote ||
                            runningMeta["remotePath"].toString() != remotePath ||
                            runningMeta["port"].toInt() != port);
            if (dead || changed) {
                if (!dead) {
                    proc->kill();
                    proc->waitForFinished(2000);
                }
                delete proc;
                m_rcloneProcesses.remove(name);
            } else {
                assignedPorts.insert(port);
            }
        }

        // Start if not running
        if (!m_rcloneProcesses.contains(name)) {
            // Ensure chosen port is not colliding with another local process (e.g. dual-node host)
            if (assignedPorts.contains(port) || !isLocalTcpPortFree(port)) {
                int candidate = 19088;
                while (candidate < 19999 && (assignedPorts.contains(candidate) || !isLocalTcpPortFree(candidate))) {
                    ++candidate;
                }
                qInfo() << "rclone port" << port << "for" << name << "is busy; reassigned to" << candidate;
                port = candidate;
                share["port"] = port;
                shares[i] = share;
            }
            assignedPorts.insert(port);

            QProcess *proc = new QProcess(this);
            proc->setProgram(rcloneBin);
            QStringList args;
            args << "--config" << configPath
                 << "serve" << "webdav" << QString("%1:%2").arg(remote, remotePath)
                 << "--addr" << QString("127.0.0.1:%1").arg(port)
                 << "--vfs-cache-mode" << "writes";
            proc->setArguments(args);

#ifdef Q_OS_LINUX
            long rcloneUid = 0, rcloneGid = 0;
            bool asSystemUser = applyRcloneUserPrivileges(proc, rcloneUid, rcloneGid);
#endif

            QVariantMap meta;
            meta["name"] = name;
            meta["type"] = type;
            meta["remote"] = remote;
            meta["remotePath"] = remotePath;
            meta["port"] = port;
            proc->setProperty("meta", meta);

            connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this, [name, proc](int exitCode, QProcess::ExitStatus exitStatus) {
                qWarning() << "rclone process for" << name << "finished with code" << exitCode << "status" << exitStatus
                           << "stderr:" << QString::fromUtf8(proc->readAllStandardError()).trimmed();
            });

            qInfo() << "Starting rclone for" << name << "(" << remote << ") on port" << port;
#ifdef Q_OS_LINUX
            //rclone inherits the fs credentials of the spawning thread, so temporarily
            //switch to the system user (same approach as OnMyDiskFileWorker)
            uid_t prevFsUid = (uid_t)-1; gid_t prevFsGid = (gid_t)-1;
            if (asSystemUser) {
                prevFsGid = setfsgid((gid_t)rcloneGid);
                prevFsUid = setfsuid((uid_t)rcloneUid);
            }
#endif
            proc->start();
#ifdef Q_OS_LINUX
            if (asSystemUser) {
                if (prevFsUid != (uid_t)-1) setfsuid(prevFsUid);
                if (prevFsGid != (gid_t)-1) setfsgid(prevFsGid);
            }
#endif
            m_rcloneProcesses[name] = proc;
        }
    }

    // Stop no longer active processes
    QStringList currentRunning = m_rcloneProcesses.keys();
    for (const QString &name : currentRunning) {
        if (!activeNames.contains(name)) {
            QProcess *proc = m_rcloneProcesses[name];
            proc->kill();
            proc->waitForFinished(2000);
            delete proc;
            m_rcloneProcesses.remove(name);
        }
    }
}


