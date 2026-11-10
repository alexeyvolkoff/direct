#include "onmydisklinkclient.h"
#include <openssl/opensslv.h>
#include <openssl/evp.h>
#include <openssl/ec.h>
#include <openssl/ecdsa.h>
#include <openssl/bn.h>
#include <openssl/obj_mac.h>
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
#include <openssl/core_names.h>
#include <openssl/params.h>
#endif
#include "version.h"
#include <QNetworkProxy>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QThreadPool>
#include <QProcess>
#include <QHostInfo>
#include <QRandomGenerator>
#include <QDateTime>
#include <QStandardPaths>
#include <memory>
#include "simplecrypt.h"
#include "localfilebackend.h"
#include "webdavbackend.h"
#include "remoteresource.h"
#include "ipcclient.h"
#include "userevents.h"
#include "tunnelcrypto.h"
#ifdef P2P_ENABLED
#include "p2ptunnel.h"
#endif
#ifdef EMBEDDED
#include "omdupdater.h"
#endif
#ifdef Q_OS_UNIX
#include <sys/time.h>
#endif
#ifdef Q_OS_LINUX
#include <sys/types.h>
#include <pwd.h>
#include <grp.h>
#include <shadow.h>
#include <unistd.h>
#include <crypt.h>
#include <string.h>

#include <QProcess>

#ifdef P2P_ENABLED
static bool authenticateLinuxUser(const QString &username, const QString &password) {
    if (username.isEmpty() || password.isEmpty()) {
        return false;
    }

    // 1. If running as root or with shadow read permissions, try getspnam directly
    struct spwd *shadow_entry = getspnam(username.toUtf8().constData());
    if (shadow_entry && shadow_entry->sp_pwdp) {
        char *encrypted = crypt(password.toUtf8().constData(), shadow_entry->sp_pwdp);
        if (encrypted && strcmp(encrypted, shadow_entry->sp_pwdp) == 0) {
            return true;
        }
    }

    // 2. Use unix_chkpwd (setgid shadow helper used by PAM for non-root processes)
    QProcess proc;
    proc.start("/sbin/unix_chkpwd", QStringList() << username << "nullok");
    if (!proc.waitForStarted(1000)) {
        proc.start("/usr/sbin/unix_chkpwd", QStringList() << username << "nullok");
        if (!proc.waitForStarted(1000)) {
            qWarning() << "Failed to start unix_chkpwd for user:" << username;
            return false;
        }
    }

    QByteArray passData = password.toUtf8();
    passData.append('\0'); // unix_chkpwd expects null-terminated password on stdin
    proc.write(passData);
    proc.closeWriteChannel();

    if (!proc.waitForFinished(3000)) {
        proc.kill();
        return false;
    }

    return (proc.exitCode() == 0);
}
#endif //P2P_ENABLED (onmydisklinkclient)
#endif


#ifdef Q_OS_WIN
#include <windows.h>
static bool authenticateWindowsUser(const QString &username, const QString &password) {
    HANDLE token = nullptr;
    bool success = LogonUserW(
        (wchar_t*)username.utf16(),
        L".",
        (wchar_t*)password.utf16(),
        LOGON32_LOGON_INTERACTIVE,
        LOGON32_PROVIDER_DEFAULT,
        &token
    );
    if (success) {
        CloseHandle(token);
    }
    return success;
}
#endif

#ifdef Q_OS_MAC
static bool authenticateMacUser(const QString &username, const QString &password) {
    if (username.isEmpty() || password.isEmpty())
        return false;
    QProcess proc;
    proc.start("/usr/bin/dscl", QStringList() << "." << "-authonly" << username << password);
    if (!proc.waitForFinished(3000)) {
        proc.kill();
        return false;
    }
    return (proc.exitCode() == 0);
}
#endif

#ifdef P2P_ENABLED
static bool authenticateNodeCredentials(QSettings *settings, const QString &username, const QString &password) {
    if (username.isEmpty() || password.isEmpty())
        return false;

    QString allowedUser = settings ? settings->value("FileNode/User", "").toString() : "";
    if (!allowedUser.isEmpty() && username != allowedUser) {
        qWarning() << "P2P Auth: rejected for user:" << username << "(configured allowed user is:" << allowedUser << ")";
        return false;
    }

#ifdef Q_OS_LINUX
    return authenticateLinuxUser(username, password);
#elif defined(Q_OS_WIN)
    return authenticateWindowsUser(username, password);
#elif defined(Q_OS_MAC)
    return authenticateMacUser(username, password);
#else
    return false;
#endif
}
#endif /* P2P_ENABLED */



quint16 OnMyDiskLinkClient::cloudPort = DEFAULT_PORT;
quint16 OnMyDiskLinkClient::plainPort = DEFAULT_PORT + 1;
QString OnMyDiskLinkClient::aiBaseURL = DEFAULT_AI_URL;
QString OnMyDiskLinkClient::searchBaseURL = DEFAULT_SEARCH_URL;
QSslConfiguration OnMyDiskLinkClient::sslConfiguration =  QSslConfiguration();
QHash<QString, QString> OnMyDiskLinkClient::shares = {};


OnMyDiskLinkClient::OnMyDiskLinkClient(QSettings *settings, HostResource *root, QObject *parent) :
    QNetworkAccessManager(parent), m_settings(settings),  m_hostRoot(root), m_SslHelper(this)
{
    qRegisterMetaType<QAbstractSocket::SocketError>("QAbstractSocket::SocketError");
    qRegisterMetaType<QSslCertificate>("QSslCertificate");
    qRegisterMetaType<LinkMessage>("LinkMessage");
    qRegisterMetaType<LinkMessage::LinkMessageType>("LinkMessage::LinkMessageType");
    qRegisterMetaType<LinkMessage::LinkMessageResult>("LinkMessage::LinkMessageResult");
    qRegisterMetaType<QNetworkReply::NetworkError>("QNetworkReply::NetworkError");
    qRegisterMetaType<bool*>("bool&");

    m_connectedDataSockets = 0;
    m_deviceId = "";
    m_user = "";
    m_hash = "";
	m_linkId = "";

    #ifdef ROUTER
    #ifdef INNBOX
    m_keyLogin = m_settings->value("Network/keyLogin", true).toBool();
    #else
    m_keyLogin = m_settings->value("Network/keyLogin", false).toBool();
    #endif
    #else
    m_keyLogin = m_settings->value("Network/keyLogin", true).toBool();
    #endif
    m_searchSupported =  false;
    cloudPort = m_omdSettings.port();
    plainPort = m_omdSettings.port() + 1;

#ifdef P2P_ENABLED
    m_restApiUrl = QUrl("https://disk.direct/pki");
    m_restApiUrl.setHost(m_omdSettings.server());
    m_restApiUrl.setPort(443);
#elif defined(STAGING)
    m_restApiUrl = QUrl("https://disk.direct/pki");
    m_restApiUrl.setHost(m_omdSettings.server());
    m_restApiUrl.setPort(443);
#else
#ifndef QT_DEBUG
    m_restApiUrl = QUrl("https://disk.direct/pki");
    m_restApiUrl.setHost(m_omdSettings.server());
    m_restApiUrl.setPort(443);
#else
    m_restApiUrl = QUrl("http://localhost/rest");
    m_restApiUrl.setPort(8080);
    m_restApiUrl.setScheme("http");
    m_restApiUrl.setHost("localhost");
#endif
#endif

	CloudResource::m_maxFileSize = m_settings->value("Network/maxFileSize", PACKET_SIZE).toULongLong();
    m_maxDataSockets = 5;
    m_linkStatus = Disconnected;
    m_treatSslErrorsAsWarnings = true;

    connect(this, &OnMyDiskLinkClient::finished, this, &OnMyDiskLinkClient::requestFinished);
    connect(&m_pingResponseTimer, &QTimer::timeout, this, &OnMyDiskLinkClient::pingTimeout);
    connect(&m_ragReindexTimer, &QTimer::timeout, this, &OnMyDiskLinkClient::ragReindexTick);
    connect(&m_ragFlushTimer, &QTimer::timeout, this, &OnMyDiskLinkClient::ragFlushPending);
    connect(&m_ragStartupTimer, &QTimer::timeout, this, &OnMyDiskLinkClient::ragStartupGraceElapsed);
    m_ragFlushTimer.setSingleShot(true);
    m_ragStartupTimer.setSingleShot(true);

#if (EMBEDDED || SERVICE)
    // Profile path is system settings file location
    m_profilePath = m_settings->value("FileNode/ProfilePath",  QFileInfo(m_settings->fileName()).absolutePath()).toString();
#else
	m_profilePath = m_settings->value("FileNode/ProfilePath", QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).toString();
	if (m_profilePath == "")
		m_profilePath = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
#endif

#ifdef QT_DEBUG
    m_profilePath = m_profilePath + "/debug";
#endif

    qInfo() << "Profile:" << m_profilePath;

    QDir appProfileDir(m_profilePath);
    if (!appProfileDir.exists())
        appProfileDir.mkpath(m_profilePath);

    m_CAPath = appProfileDir.filePath(m_settings->value("Network/CA", "ssl/rootCA.crt").toString());

    QFileInfo caInfo(m_CAPath);
    if (!caInfo.absoluteDir().exists())
        caInfo.absoluteDir().mkpath(caInfo.absoluteDir().path());

    m_CertPath = appProfileDir.filePath(m_settings->value("Network/Cert", "ssl/user.crt").toString());

    QFileInfo certInfo(m_CertPath);
    if (!certInfo.absoluteDir().exists())
        certInfo.absoluteDir().mkpath(certInfo.absoluteDir().path());

    m_KeyPath = appProfileDir.filePath(m_settings->value("Network/pKey", "ssl/user.key").toString());

    QFileInfo keyInfo(m_KeyPath);
    if (!keyInfo.absoluteDir().exists())
        keyInfo.absoluteDir().mkpath(keyInfo.absoluteDir().path());

    connect(this, &OnMyDiskLinkClient::sslErrors, this, &OnMyDiskLinkClient::onSslErrors);

    //init randomizer (for Qt < 5.15)
    SimpleCrypt::init();

    if (m_keyLogin) {
       if (!checkDeviceId())
            createDeviceId();
    }

        //addShare("root", QStandardPaths::writableLocation(QStandardPaths::HomeLocation));
}

OnMyDiskLinkClient::~OnMyDiskLinkClient()
{

}

void OnMyDiskLinkClient::readSettings()
{
    loadDeviceId();
    if (m_settings) {
        m_settings->beginGroup("Settings");
        m_omdSettings.readSettings(m_settings);
        m_settings->endGroup();
        // Apply without triggering cert renewal side-effects on startup;
        // tokens/hostname are needed for GetSettings.
        // Use updateSettings to sync derived state (search/ai flags etc)
        // but avoid password clear.
        QString savedPass = m_omdSettings.password();
        updateSettings(m_omdSettings);
        m_omdSettings.setPassword(savedPass);
    }
}


QString OnMyDiskLinkClient::randomKey(int length)
{
   //qsrand(QDateTime::currentDateTime().toTime_t ());
   const QString alphanumericCharacters("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789");

   QString key;
   int i = 0;
   while( i < length)
   {
       int index = SimpleCrypt::rand()  % alphanumericCharacters.length();
       QChar nextChar = alphanumericCharacters.at(index);
	   if (key.indexOf(nextChar) == -1) {
		 key.append(nextChar);
		 i++;
	   }
   }
   return key;
}

void OnMyDiskLinkClient::workerConnected()
{
    //add to list
    QMutexLocker locker(&m_connectionStateMutex);
    ++m_connectedDataSockets;

    //Legacy file worker is gone in 3.0 - never spawn data-socket workers
}


void OnMyDiskLinkClient::loadDeviceId()
{
    QDir appProfileDir(m_profilePath);
    QString deviceIdFileName = appProfileDir.filePath("device.id");

    QFile deviceFile(deviceIdFileName);
    if (deviceFile.open(QIODevice::ReadOnly | QIODevice::Text))
	{
			QTextStream stream(&deviceFile);
			m_deviceId =  stream.readLine().trimmed();
            m_origDeviceId = m_deviceId;
            deviceFile.close();
    } else
            createDeviceId();

    qInfo() << "Device Id:" << m_deviceId;


    emit deviceIdUpdated(m_deviceId);
}

bool OnMyDiskLinkClient::checkDeviceId()
{
	QDir appProfileDir(m_profilePath);
	QString deviceIdFileName = appProfileDir.filePath("device.id");
	return QFileInfo(deviceIdFileName).exists();
}

//creates id/key pair for "userless" devices
void OnMyDiskLinkClient::createDeviceId()
{
    qDebug() << "Generating device id";
    m_deviceId = randomKey(10);
	QDir appProfileDir(m_profilePath);
	QString deviceIdFileName = appProfileDir.filePath("device.id");
	QFile deviceIdFile(deviceIdFileName);
	if (deviceIdFile.open(QIODevice::WriteOnly | QIODevice::Text))
	{
        qInfo() << "Saving device id" << m_deviceId << "to" << deviceIdFileName;
		QByteArray data = QString("%1\n").arg(m_deviceId).toUtf8();
		deviceIdFile.write(data);
		deviceIdFile.close();
	}
}


void OnMyDiskLinkClient::connectToCloud()
{
    if (m_linkStatus != Connected)
    {

        if (!QSslSocket::supportsSsl())
        {
			qCritical() << "OpenSSL not supported";
            emit noSslSupport();
            return;
        }


        if (setupSSL())
        {
			m_linkStatus = Connecting;

			if (serviceSocket)
			{
                disconnect(serviceSocket, nullptr, nullptr, nullptr);
                serviceSocket->stopPingTimer();
				serviceSocket->abort();
                serviceSocket->deleteLater();
                serviceSocket = nullptr;
			}

            serviceSocket = new LinkSocket(true);
            serviceSocket->setSslConfiguration(sslConfiguration);

            connect(serviceSocket, &LinkSocket::connected, serviceSocket, [this]() {
                serviceSocket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
                serviceSocket->setKeepAlive();
            });

            void(LinkSocket::*sslErrorSignal)(const QList<QSslError> &errors) = &LinkSocket::sslErrors;

            connect(serviceSocket, sslErrorSignal, this, &OnMyDiskLinkClient::onSslError);

            #if QT_VERSION < 0x050F00
            void(LinkSocket::*errorSignal)(QAbstractSocket::SocketError) = &LinkSocket::error;
            #else
            void(LinkSocket::*errorSignal)(QAbstractSocket::SocketError) = &LinkSocket::errorOccurred;
            #endif
            connect(serviceSocket, errorSignal, this, &OnMyDiskLinkClient::onConnectionError);
            connect(serviceSocket, &LinkSocket::newMessage, this, &OnMyDiskLinkClient::messageRecieved);
            connect(serviceSocket, &LinkSocket::disconnected, this, &OnMyDiskLinkClient::socketDisconnected);
            connect(serviceSocket, &LinkSocket::encrypted, this, &OnMyDiskLinkClient::socketConnected);
            connect(serviceSocket, &LinkSocket::timeToPing, this, &OnMyDiskLinkClient::timeToPing);

            serviceSocket->setProxy(QNetworkProxy::DefaultProxy);

			//Clear workers
            //Legacy file workers are gone in 3.0
            m_connectedDataSockets = 0;
            #ifdef STAGING
            QString linkServer = m_omdSettings.server();
            #else
            #ifdef QT_DEBUG
            QString linkServer = "localhost";
            #else
            QString linkServer = m_omdSettings.server();
            #endif
            #endif

            qDebug() << qPrintable(QString("Connecting to %1:%2, secure:").arg(linkServer, QString::number(cloudPort))) << m_omdSettings.secure();

            serviceSocket->connectToHostEncrypted(linkServer, static_cast<quint16>(cloudPort));


        } else
        {
            if (m_linkStatus != Authorizing && m_keyLogin)
			{
                if (requestNewCert(m_deviceId))
                    QMetaObject::invokeMethod(this, "connectToCloud", Qt::QueuedConnection); //connect device
				else {
                    emit privateKeyStatus(tr("Certificate not ready yet..."));
                    qWarning() << "Certificate not ready yet";
				}
            } else {
                emit privateKeyStatus(tr("Certificate not ready yet, login is required"));
                qWarning() << "SSL configuration not ready yet";
            }
        }
    }
}


void OnMyDiskLinkClient::disconnectFromCloud()
{
    LinkSocket *s = nullptr;
    {
        QMutexLocker locker(&m_connectionStateMutex);
        if (!serviceSocket)
            return;
        qWarning() << "[LinkClient] disconnectFromCloud called, aborting serviceSocket";
        s = serviceSocket;
        disconnect(s, nullptr, nullptr, nullptr);
        s->stopPingTimer();
        s->abort();
        s->deleteLater();
        serviceSocket = nullptr;
        m_pingResponseTimer.stop();
        m_pingMisses = 0;
        m_lastPingTickMs = 0;
        m_linkStatus = Disconnected;
        m_connectedDataSockets = 0;

        Q_FOREACH(QTcpSocket* tcpSocket, m_tunnels)
        {
            tcpSocket->disconnectFromHost();
            tcpSocket->deleteLater();
        }
        m_tunnels.clear();

        const QStringList p2pIds = m_p2pTunnels.keys();
        Q_FOREACH(const QString &p2pID, p2pIds)
            cleanupP2PTunnel(p2pID);
    }
    emit linkDisconnected();
}



void OnMyDiskLinkClient::removeDeviceKey()
{
	QUrl requestUrl(m_restApiUrl);
	requestUrl.setPath(requestUrl.path() + "/removekey");
    requestUrl.setHost(m_omdSettings.server());

	QNetworkRequest request(requestUrl);
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    request.setTransferTimeout(15000);
#endif

	request.setRawHeader("authorization", QString("device:" + m_deviceId).toUtf8());
	this->post(request, QByteArray(""));
    QTimer::singleShot(20000, &m_loop, &QEventLoop::quit);
    m_loop.exec();
}

void OnMyDiskLinkClient::clearDeviceId()
{
    qDebug() << "Resetting device ID";
    m_origDeviceId = m_deviceId;
    m_deviceId = "";
}

void OnMyDiskLinkClient::resetDeviceId()
{
    disconnectFromCloud();
    if ( m_deviceId != "") {
        removeDeviceKey();
    }
    m_deviceId = "";
    QDir appProfileDir(m_profilePath);
    QString deviceIdFileName = appProfileDir.filePath("device.id");
    QFile deviceFile(deviceIdFileName);
    deviceFile.remove();
    QString deviceKeyFileName = appProfileDir.filePath("device.key");
    QFile deviceKeyFile(deviceKeyFileName);
    deviceKeyFile.remove();
    removeUserKey();
    emit privateKeyError(tr("Login is required"));
    qDebug() << "Device reset";

    loadDeviceId();

    if (m_keyLogin)
    {
        if (m_settings->value(("Network/connect"), true).toBool()) {
            m_linkStatus = Disconnected;
            connectToCloud();
        }
    }
}


void OnMyDiskLinkClient::addLocalShare(QString shareName, QString sharePath, bool persistent)
{
    if (QThread::currentThread() != this->thread()) {
       // Called from different thread
        QMetaObject::invokeMethod(this, "addLocalShare", Qt::QueuedConnection, Q_ARG(QString, shareName), Q_ARG(QString, sharePath), Q_ARG(bool, persistent));
    }
    else
        addShare(shareName, sharePath, persistent);
}

void OnMyDiskLinkClient::removeLocalShare(QString shareName)
{
    if (QThread::currentThread() != this->thread()) {
       // Called from different thread
        QMetaObject::invokeMethod(this, "removeLocalShare", Qt::QueuedConnection, Q_ARG(QString, shareName));
    }
    else
		removeShare(shareName);
}

void OnMyDiskLinkClient::addShare(QString sharePath)
{
	if (QThread::currentThread() != this->thread()) {
	   // Called from different thread
        QMetaObject::invokeMethod(this, "addShare", Qt::QueuedConnection, Q_ARG(QString, sharePath));
	}
	else {
		int p = sharePath.indexOf(":");
		if (p > 0) {
			QString shareName = sharePath.left(p);
			QString path = sharePath.mid(p + 1);
            addShare(shareName, path, true);
		} else {
			QString shareName = QFileInfo(sharePath).fileName();
            addShare(shareName, sharePath, true);
		}
	}
}

void OnMyDiskLinkClient::removeUserKey()
{
    removeClientKeys();
}

void OnMyDiskLinkClient::resetTimer()
{
    if (serviceSocket)
    {
        m_pingResponseTimer.stop();
        serviceSocket->startPingTimer(PING_INTERVAL);
    }
}

void OnMyDiskLinkClient::publicLinkAdded(const QString &itemPath, const QString &link, const QHash<QString, QVariant> &linkData)
{
    QHash<QString, QVariant> linkResponseData;
    linkResponseData["link"] = link;
    linkResponseData["linkData"] = linkData;
    linkResponseData["itemPath"] = itemPath;

    LinkMessage message;
    message.setMessageType(LinkMessage::GetPublicLink);
    message.setResultCode(LinkMessage::OK);
    message.setData(linkResponseData);

    serviceSocket->writeMessage(message);
}

void OnMyDiskLinkClient::updateSettings(const OnMyDiskSettings &settings)
{
    m_omdSettings = settings;

    bool userChanged = (!m_user.isEmpty() && m_omdSettings.user() != m_user);
    bool renewRequested = m_omdSettings.resetId();

    //setup proxy
    QNetworkProxy proxy;
    if (m_omdSettings.useProxy())
    {
         proxy.setType(QNetworkProxy::Socks5Proxy);
         proxy.setHostName(m_omdSettings.proxyHost());
         proxy.setPort(static_cast<quint16>( m_omdSettings.proxyPort()));
         proxy.setUser(m_omdSettings.proxyUser());
         proxy.setPassword(m_omdSettings.proxyPassword());
    } else
    {
         proxy.setType(QNetworkProxy::NoProxy);
    }
    m_omdSettings.setPassword("");

    QNetworkProxy::setApplicationProxy(proxy);

    #if (EMBEDDED || SERVICE || defined(DESKTOP) || defined(SERVER)) && !defined(ROUTER)
    m_searchSupported =  m_omdSettings.localSearch();
    m_aiSupported =  m_omdSettings.localAI();
    #endif
    // RAG 3.0: periodic owner re-index of shares (autoReindex setting)
    m_ragReindexTimer.stop();
    if (m_aiSupported && m_omdSettings.autoReindex()) {
        int intervalMs = m_settings->value("RAG/IndexIntervalSecs", RAG_REINDEX_INTERVAL_SECS).toInt() * 1000;
        if (intervalMs <= 0) intervalMs = RAG_REINDEX_INTERVAL_SECS * 1000;
        m_ragReindexTimer.start(intervalMs);
    }
    // RAG 3.0: startup grace — defer the initial kick of share indexing until
    // the local AI node (uvicorn :4000) is up, so the very first POST is not
    // fired into a "Connection refused"
    m_ragStartupTimer.stop();
    if (m_aiSupported) {
        m_ragStartupGrace = true;
        int graceSecs = m_settings->value("RAG/StartupDelaySecs", RAG_STARTUP_DELAY_SECS).toInt();
        if (graceSecs <= 0) graceSecs = RAG_STARTUP_DELAY_SECS;
        m_ragStartupTimer.start(graceSecs * 1000);
        qInfo() << "RAG: startup grace armed for" << graceSecs << "s — index requests will be deferred";
    }
    //update host name from settings
    m_hostRoot->setHostName(m_omdSettings.hostName());
    m_hostRoot->setCustomAttribute("localSearch", m_searchSupported);

#if ((EMBEDDED || SERVICE || defined(DESKTOP) || defined(SERVER)) && !defined(ROUTER))
    // Add or update WebDAV resources from rcloneShares
    QSet<QString> activeWebDAVShares;
    QVariantList rcloneShares = m_omdSettings.rcloneShares();
    for (const QVariant &shareVar : rcloneShares) {
        QVariantMap share = shareVar.toMap();
        QString name = share["name"].toString();
        int port = share["port"].toInt();

        if (name.isEmpty() || port <= 0) continue;
        activeWebDAVShares.insert(name);

        QVariantHash shareData;
        QVariantHash webdavData;
        webdavData["baseUrl"] = QString("http://127.0.0.1:%1/").arg(port);
        webdavData["backendType"] = "webdav";
        shareData["webdav"] = webdavData;
        shareData["exists"] = true;
        shareData["isDir"] = true;
        shareData["isReadable"] = true;
        shareData["isWritable"] = true;
        shareData["overrideWritable"] = true;

        m_hostRoot->addWebDAVResource(name, shareData);
    }

    // Remove any WebDAV shares that are no longer active
    for (const QString &oldShare : m_mountedWebdavShares) {
        if (!activeWebDAVShares.contains(oldShare)) {
            m_hostRoot->removeRemoteShare(oldShare);
        }
    }
    m_mountedWebdavShares = activeWebDAVShares;
#endif

    #if (EMBEDDED || SERVICE || defined(DESKTOP) || defined(SERVER))
    #ifdef ROUTER
    m_hostRoot->setCustomAttribute("managed", false);
    #else
    m_sitesPath = m_omdSettings.sitesRoot();
    m_sharedWith = m_omdSettings.sharedWith();
    m_syncPaths = m_omdSettings.syncPaths().split(",", Qt::SplitBehaviorFlags::SkipEmptyParts);

    m_hostRoot->setCustomAttribute("managed", true);
    m_hostRoot->setCustomAttribute("sharedWith", m_sharedWith);
    m_hostRoot->setCustomAttribute("localAI", m_aiSupported);

    if (m_omdSettings.sitesEnabled() && !m_sitesPath.isEmpty() ) {
        QVariantHash attrs;
        attrs.insert("sitesRoot", true);
        m_hostRoot->setCustomAttribute("hasSites", true);
        m_hostRoot->addLocalResource("sites",  m_sitesPath, attrs);
    } else
        m_hostRoot->setCustomAttribute("hasSites", false);

    #endif
    #endif

    cloudPort = m_omdSettings.port();
    plainPort = m_omdSettings.port() + 1;
#if ((EMBEDDED || SERVICE || defined(DESKTOP) || defined(SERVER)) && !defined(ROUTER))
    searchBaseURL = m_omdSettings.localSearchBase();
    aiBaseURL = m_omdSettings.aiBase();
#endif

    if (m_omdSettings.linkId() != "" && m_omdSettings.linkId() != m_linkId) {
        m_linkId = m_omdSettings.linkId();
        //make linkID persistent, as hash of the first cert received
        qInfo() << "Persistent node ID:" << m_linkId;
        m_hostRoot->setStorageId(m_linkId);
    }

    if (userChanged || renewRequested) {
        qInfo() << "Requesting new certificate for user:" << m_omdSettings.user() << "Renew:" << renewRequested;
        disconnectFromCloud();
        removeClientKeys();
        QString targetUser = m_omdSettings.user();
        if (targetUser.isEmpty() || targetUser == m_deviceId) {
            targetUser = m_deviceId;
        }
        requestNewCert(targetUser);
    }
}



void OnMyDiskLinkClient::sendUpdateHostRoot()
{
    qInfo() << "Refreshing share root:" << m_hostRoot->hostName() << "with ID:" << m_linkId;
    if (serviceSocket)
    {
        m_hostRoot->setCustomAttribute("localSearch", m_searchSupported);
        m_hostRoot->refresh();
        LinkMessage message;
        message.setMessageType(LinkMessage::UpdateShare);
        message.setData(m_hostRoot->getData());
        message.setEntryData(m_hostRoot->getEntryData());
        message.insertDataValue("sharename", m_hostRoot->hostName());
        message.insertDataValue("localSearch", m_searchSupported);
        message.insertDataValue("localAI", m_aiSupported);
        message.insertDataValue("linkID", m_linkId);
        #if (EMBEDDED || SERVICE || defined(DESKTOP) || defined(SERVER))
        #ifndef ROUTER
        if (!m_sitesPath.isEmpty()) {
            message.insertDataValue("sitesRoot", m_sitesPath);
            message.insertDataValue("sites", m_hostRoot->getEntryAttributes("sites"));
        }
        message.insertDataValue("sharedWith", m_sharedWith);
        if (!m_syncPaths.isEmpty())
            message.insertDataValue("syncPaths", QVariant::fromValue(m_syncPaths));

        message.insertDataValue("managed", true);
        #else
        message.insertDataValue("managed", false);
        #endif
        #endif
        message.insertDataValue("version", APP_VERSION);

        //qDebug() << "Root info" << m_hostRoot.getData();
        serviceSocket->writeMessage(message);
    }
}

void OnMyDiskLinkClient::sendUpdateSite(QString site)
{
    LinkMessage message;
    message.setMessageType(LinkMessage::UpdateShare);
    message.insertDataValue("siteUpdated", site);
    message.insertDataValue("linkID", m_linkId);
    serviceSocket->writeMessage(message);
}

void OnMyDiskLinkClient::sendUpdateHostAddShare(QString shareName, bool persistent)
{
    if (serviceSocket)
    {
        m_hostRoot->refresh();
        LinkMessage message;
        message.setMessageType(LinkMessage::UpdateShare);
        message.setData(m_hostRoot->getData());
        message.setEntryData(m_hostRoot->getEntryData());
        message.insertDataValue("sharename", m_hostRoot->hostName());
        message.insertDataValue("addfolder", shareName);
        message.insertDataValue("reindex", m_omdSettings.autoReindex() || persistent);
        message.insertDataValue("version", APP_VERSION);
        //qDebug() << "Root info" << m_hostRoot.getData();
        serviceSocket->writeMessage(message);
    }
}

void OnMyDiskLinkClient::sendUpdateHostRemoveShare(QString shareName)
{
    if (serviceSocket)
    {
        m_hostRoot->refresh();
        LinkMessage message;
        message.setMessageType(LinkMessage::UpdateShare);
        message.setData(m_hostRoot->getData());
        message.setEntryData(m_hostRoot->getEntryData());
        message.insertDataValue("sharename", m_hostRoot->hostName());
        message.insertDataValue("removefolder", shareName);
        message.insertDataValue("version", APP_VERSION);
        //qDebug() << "Root info" << m_hostRoot.getData();
        serviceSocket->writeMessage(message);
    }
}

void OnMyDiskLinkClient::timeToPing()
{
   LinkSocket *socket = serviceSocket;
   if (!socket)
   {
       qCritical() << "Ping: socket closed";
       return;
   }
   if (socket->state() != QAbstractSocket::ConnectedState)
   {
       qWarning() << "Ping: control socket is not connected, forcing reconnect";
       disconnectFromCloud();
       return;
   }

   const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
   if (m_lastPingTickMs > 0 && (nowMs - m_lastPingTickMs) > 35000)
   {
       qWarning() << "Ping: system sleep/wake jump detected (" << (nowMs - m_lastPingTickMs)
                  << "ms), cycling control socket and stale P2P tunnels";
       disconnectFromCloud();
       return;
   }
   m_lastPingTickMs = nowMs;

   m_pingResponseTimer.start(PING_TIMEOUT);
   LinkMessage message;
   message.setMessageType(LinkMessage::Ping);
   if (! socket->writeMessage(message))
   {
       qWarning() << "Ping: write failed, forcing reconnect";
       disconnectFromCloud();
   }
}

//Missed ping: give the link one immediate retry before giving up, so a single
//dropped packet or busy loop on the server does not kill the connection.
void OnMyDiskLinkClient::pingTimeout()
{
    ++m_pingMisses;
    if (m_pingMisses >= 2)
    {
        qWarning() << "Ping: no reply after" << m_pingMisses << "consecutive pings, reconnecting";
        disconnectFromCloud();
        return;
    }
    qWarning() << "Ping: no reply, retrying immediately";
    timeToPing();
}

void OnMyDiskLinkClient::addShare(QString shareName, QString sharePath, bool persistent)
{
    QMutexLocker lock(&m_sharesMutex);
    QFileInfo shareInfo(sharePath);
    if (!shares.contains(shareName) && shareInfo.exists() && shareInfo.isReadable())
    {
        shares[shareName] = shareInfo.absoluteFilePath();

        m_hostRoot->addLocalResource(shareName, shareInfo.absoluteFilePath());

        if (m_linkStatus == OnMyDiskLinkClient::Connected )
        {
            sendUpdateHostAddShare(shareName, persistent);
        }
        // RAG 3.0: index the new share root (also covers external disk mount)
        if (m_aiSupported)
            ragIndexPath(shareInfo.absoluteFilePath());
    }
}

void OnMyDiskLinkClient::removeShare(QString shareName)
{
    QMutexLocker lock(&m_sharesMutex);
    if (shares.contains(shareName))
    {
        QString removedPath = shares[shareName];
        shares.remove(shareName);
        m_hostRoot->removeShare(shareName);

        if (m_linkStatus == OnMyDiskLinkClient::Connected )
        {
            sendUpdateHostRemoveShare(shareName);
        }
        // RAG 3.0: drop removed share from the index
        if (m_aiSupported)
            ragDeleteSource(removedPath);
    }
}


void OnMyDiskLinkClient::handleConnectDataSockets(const LinkMessage &message)
{
    if (message.getData().contains("maxConnections"))
    {
        bool isOk;
        int recievedCount = message.getData()["maxConnections"].toInt(&isOk);
        if (isOk)
        {
            QMutexLocker locker(&m_connectionStateMutex);
            m_maxDataSockets = recievedCount;
            m_connectedDataSockets = 0;
            qDebug() << "Data sockets announced by gateway:" << recievedCount << "(ignored in 3.0 - single command socket)";

            //Let's pretend we are connected  to avoid unwanted disconnect on timer on slow device
            m_linkStatus = Connected;
            emit linkConnected();

            //Start ping timer
            m_lastPingTickMs = QDateTime::currentMSecsSinceEpoch();
            serviceSocket->startPingTimer(PING_INTERVAL);

            //Register share
            sendUpdateHostRoot();

            //Legacy file worker (LinkBackend data plane) is gone in 3.0:
            //file serving is local/P2P. Do not spin up extra sockets/threads.
            Q_UNUSED(m_maxDataSockets)
        }
    }

	//qDebug() << "Connecting sockets:" << m_maxDataSockets;

}

void OnMyDiskLinkClient::handleRPCRequest(const LinkMessage &message)
{
    QString rpcRequest = message.getData()["cmd"].toString();
    QHash<QString, QVariant> requestParams = message.getData()["params"].toHash();
    qInfo() << "RPC call:" << rpcRequest << requestParams;
    if (rpcRequest == "restartsite") {
        QString restartAction = requestParams.contains("rebuild") && requestParams["rebuild"].toBool() ? "restart" : "";
        QString action = requestParams.contains("siteRemoved") ? "remove" : restartAction;
        QString site = requestParams.contains("siteRemoved") ? requestParams["siteRemoved"].toString() : "";
        if (!action.isEmpty())
            emit restartSites(action, site);
    }
    if (requestParams.contains("siteAdded")) {
        qInfo() << "Creating site" << requestParams["siteAdded"].toString();
        sendUpdateHostRoot();
    }
}

#ifdef P2P_ENABLED
// TURN server co-located with the gateway: same host the client connects to,
// standard port 3478. Accepts a bare host or a URL.
static QString turnUrlFor(const QString &server)
{
    QString host = server;
    if (host.startsWith(QStringLiteral("https://")))
        host.remove(0, 8);
    else if (host.startsWith(QStringLiteral("http://")))
        host.remove(0, 7);
    int slash = host.indexOf(QLatin1Char('/'));
    if (slash > 0)
        host.truncate(slash);
    int colon = host.lastIndexOf(QLatin1Char(':'));
    // strip port unless it's IPv6 literal without port info we care about here
    if (colon > 0 && !host.mid(colon + 1).contains(QLatin1Char(':')))
        host.truncate(colon);
    return QStringLiteral("turn:%1:3478").arg(host);
}
#endif

void OnMyDiskLinkClient::handleTunnelOpenRequest(const LinkMessage &message)
{
    if (message.getResultCode() != LinkMessage::Request)
    {
        //this is a reply to a tunnel request sent by this device (e.g. omd-tunnel CLI)
        return;
    }

    if (!message.getData().contains("ID") || !message.getData().contains("port"))
    {
        qWarning() << "TunnelOpen request missing 'ID' or 'port' attribute";
        return;
    }

QString ID = message.getData()["ID"].toString();
    QString host = message.getData().contains("host") ? message.getData()["host"].toString() : "127.0.0.1";
    quint16 port = static_cast<quint16>(message.getData()["port"].toUInt());
    QString service = message.getData().value("service").toString();
    bool isFileTunnel = (port == 0 || service == "p2p-fs");

    // Gateway-validated caller identity: only the gateway (which authenticated
    // the initiator) is allowed to set these fields on TunnelOpen; the node
    // re-writes every request sent into the local port with them.
    const QString callerUser = message.getData().value("X-OMD-Caller-User").toString();
    const QString callerRole = message.getData().value("X-OMD-Caller-Role").toString();
    const QString targetOwner = message.getData().value("X-OMD-Target-Owner").toString();

    if (m_tunnels.contains(ID) || m_p2pTunnels.contains(ID))
    {
        qWarning() << "Tunnel already open:" << ID;
        return;
    }

    if (isFileTunnel)
    {
#ifdef P2P_ENABLED
        qInfo() << "Opening P2P FS DataChannel tunnel" << ID;
        P2PTunnel *p2p = new P2PTunnel(P2PTunnel::Role::Answerer, this);
        p2p->setTurnServer(turnUrlFor(m_omdSettings.server()), "turnuser", "turnpass");
        m_p2pTunnels.insert(ID, p2p);
        connect(p2p, &P2PTunnel::signalReady, this, [this, ID](const QString &kind, const QByteArray &payload) {
            sendTunnelSignalingFor(ID, kind, payload);
        });
        connect(p2p, &P2PTunnel::dataReceived, this, [this, p2p](const QByteArray &data) {
            if (!p2pTaskStarted(p2p))
                return; // tunnel teardown already in progress, drop the chunk
            QThreadPool::globalInstance()->start([this, p2p, data]() {
                handleP2PFileRequest(p2p, data);
                QMetaObject::invokeMethod(this, [this, p2p]() {
                    p2pTaskFinished(p2p);
                }, Qt::QueuedConnection);
            });
        });
        connect(p2p, &P2PTunnel::opened, this, [this, ID]() {
            qInfo() << "P2P FS Tunnel" << ID << "established";
            LinkMessage reply;
            reply.setMessageType(LinkMessage::TunnelOpen);
            reply.setResultCode(LinkMessage::OK);
            reply.insertDataValue("ID", ID);
            if (serviceSocket)
                serviceSocket->writeMessage(reply);
        });
        connect(p2p, &P2PTunnel::closed, this, [this, ID]() {
            qInfo() << "P2P FS Tunnel" << ID << "closed";
            sendTunnelCloseFor(ID);
            cleanupP2PTunnel(ID);
        });
        connect(p2p, &P2PTunnel::failed, this, [this, ID](const QString &error) {
            qWarning() << "P2P FS Tunnel" << ID << "failed:" << error;
            sendTunnelCloseFor(ID);
            cleanupP2PTunnel(ID);
        });
        p2p->start();
#endif
        return;
    }

    const int terminalPort = m_settings ? m_settings->value("FileNode/terminalPort", 22).toInt() : 22;
    const bool isTerminal = (service == "terminal" || port == 22 || (terminalPort > 0 && port == terminalPort));

    QTcpSocket* tcpSocket = new QTcpSocket(this);
    m_tunnels.insert(ID, tcpSocket);
    if (!isTerminal) {
        if (!callerUser.isEmpty())
            m_tunnelCallerUser.insert(ID, callerUser);
        if (!callerRole.isEmpty())
            m_tunnelCallerRole.insert(ID, callerRole);
        if (!targetOwner.isEmpty())
            m_tunnelTargetOwner.insert(ID, targetOwner);
    }

    auto p2pBuffer = std::make_shared<QByteArray>();

#ifdef P2P_ENABLED
    P2PTunnel *p2p = new P2PTunnel(P2PTunnel::Role::Answerer, this);
    p2p->setTurnServer(turnUrlFor(m_omdSettings.server()), "turnuser", "turnpass");
    m_p2pTunnels.insert(ID, p2p);

    connect(p2p, &P2PTunnel::signalReady, this, [this, ID](const QString &kind, const QByteArray &payload) {
        sendTunnelSignalingFor(ID, kind, payload);
    });
    connect(p2p, &P2PTunnel::dataReceived, this, [this, ID, tcpSocket, isTerminal](const QByteArray &data) {
        //qInfo() << "P2P TCP Tunnel" << ID << "recv from DC -> write to TCP:" << data.size() << "bytes";
        if (!tcpSocket || !tcpSocket->isOpen())
            return;
        if (isTerminal) {
            tcpSocket->write(data);
            return;
        }
        // Re-write the request stream: strip client-supplied identity headers
        // and inject the gateway-validated caller identity before each request.
        writeInjectedRequestData(ID, data, tcpSocket);
    });
    connect(p2p, &P2PTunnel::opened, this, [this, ID, p2pBuffer](const QString &pairInfo) {
        qInfo() << "P2P TCP Tunnel" << ID << "established via" << pairInfo;
        P2PTunnel *p2p = m_p2pTunnels.value(ID, nullptr);
        if (p2p && !p2pBuffer->isEmpty()) {
            qInfo() << "P2P TCP Tunnel" << ID << "flushing buffered TCP data to DC:" << p2pBuffer->size() << "bytes";
            p2p->sendData(*p2pBuffer);
            p2pBuffer->clear();
        }
    });
    connect(p2p, &P2PTunnel::closed, this, [this, ID]() {
        qInfo() << "P2P TCP Tunnel" << ID << "closed";
        sendTunnelCloseFor(ID);
    });
    connect(p2p, &P2PTunnel::failed, this, [this, ID](const QString &error) {
        qWarning() << "P2P TCP Tunnel" << ID << "failed:" << error;
        sendTunnelCloseFor(ID);
    });
    p2p->start();
#endif

    auto connectedFlag = std::make_shared<bool>(false);

    connect(tcpSocket, &QTcpSocket::connected, tcpSocket, [this, ID, connectedFlag]() {
        *connectedFlag = true;
        LinkMessage reply;
        reply.setMessageType(LinkMessage::TunnelOpen);
        reply.setResultCode(LinkMessage::OK);
        reply.insertDataValue("ID", ID);
        if (m_tunnelE2EEPub.contains(ID))
            reply.insertDataValue("e2eePub", QString::fromLatin1(m_tunnelE2EEPub[ID].toBase64()));
        if (serviceSocket)
            serviceSocket->writeMessage(reply);
    });

    connect(tcpSocket, &QTcpSocket::readyRead, tcpSocket, [this, ID, tcpSocket, p2pBuffer]() {
        QByteArray data = tcpSocket->readAll();
        if (data.isEmpty())
            return;
#ifdef P2P_ENABLED
        P2PTunnel *p2p = m_p2pTunnels.value(ID, nullptr);
        if (p2p) {
            if (p2p->isOpen()) {
                if (!p2pBuffer->isEmpty()) {
                    p2pBuffer->append(data);
                    //qInfo() << "P2P TCP Tunnel" << ID << "TCP readyRead -> send buffered to DC:" << p2pBuffer->size() << "bytes";
                    p2p->sendData(*p2pBuffer);
                    p2pBuffer->clear();
                } else {
                    //qInfo() << "P2P TCP Tunnel" << ID << "TCP readyRead -> send to DC:" << data.size() << "bytes";
                    p2p->sendData(data);
                }
            } else {
                //qInfo() << "P2P TCP Tunnel" << ID << "TCP readyRead -> DC not open yet, buffering:" << data.size() << "bytes";
                p2pBuffer->append(data);
            }
            return;
        }
#endif
        sendTunnelDataFor(ID, data);
    });

    connect(tcpSocket, QOverload<QAbstractSocket::SocketError>::of(&QAbstractSocket::errorOccurred), tcpSocket, [this, ID, tcpSocket, connectedFlag]() {
        if (m_tunnels.take(ID) != tcpSocket)
            return;
        m_tunnelE2EEPub.remove(ID);
        m_tunnelKeys.remove(ID);
        cleanupP2PTunnel(ID);
        LinkMessage reply;
        if (*connectedFlag) {
            // connection was already established: a later error is a teardown,
            // report it as a close rather than a failed open
            reply.setMessageType(LinkMessage::TunnelClose);
            reply.setResultCode(LinkMessage::OK);
        } else {
            reply.setMessageType(LinkMessage::TunnelOpen);
            reply.setResultCode(LinkMessage::LinkError);
            reply.setErrorMsg(tcpSocket->errorString());
        }
        reply.insertDataValue("ID", ID);
        if (serviceSocket)
            serviceSocket->writeMessage(reply);
        tcpSocket->disconnectFromHost();
        tcpSocket->deleteLater();
    });

    connect(tcpSocket, &QTcpSocket::disconnected, tcpSocket, [this, ID, tcpSocket]() {
        if (m_tunnels.take(ID) != tcpSocket)
            return;
        m_tunnelE2EEPub.remove(ID);
        m_tunnelKeys.remove(ID);
        cleanupP2PTunnel(ID);
        tcpSocket->deleteLater();
        LinkMessage reply;
        reply.setMessageType(LinkMessage::TunnelClose);
        reply.setResultCode(LinkMessage::OK);
        reply.insertDataValue("ID", ID);
        if (serviceSocket)
            serviceSocket->writeMessage(reply);
    });

    qInfo() << "Opening tunnel" << ID << "to" << host << port;
    tcpSocket->connectToHost(host, port);
}

// Helper to get user profile config path (e.g. /etc/onmydisk/onmydisk.conf.user)
// and paired-DID lookup. Both are used by writeInjectedRequestData (DID
// authentication of injected requests), which is compiled in non-P2P builds
// too - keep them outside the P2P_ENABLED guard.
static QString userConfigFilePath(QSettings *settings)
{
    if (!settings) return QString();
    QString base = settings->fileName();
    if (base.endsWith(".user"))
        return base;
    return base + ".user";
}

#ifdef P2P_ENABLED
static bool didRegistered(QSettings *settings, const QString &didPub)
{
    //The DID is paired when any entry exists under PairedDIDs/<didPub>
    //(legacy timestamps/secrets count too - the marker is the group itself).
    if (didPub.isEmpty()) return false;
    QString userFile = userConfigFilePath(settings);
    if (!userFile.isEmpty()) {
        QSettings userSettings(userFile, QSettings::IniFormat);
        userSettings.setIniCodec("UTF-8");
        if (userSettings.contains("PairedDIDs/" + didPub)) return true;
    }
    if (settings && settings->contains("PairedDIDs/" + didPub))
        return true;
    return false;
}

//Device binding: a device pubkey, certified by the DID at first login.
//Value = the bound DID pubkey.
static void bindDeviceDID(QSettings *settings, const QString &devicePub, const QString &didPub)
{
    if (devicePub.isEmpty() || didPub.isEmpty()) return;
    QString userFile = userConfigFilePath(settings);
    if (!userFile.isEmpty()) {
        QSettings userSettings(userFile, QSettings::IniFormat);
        userSettings.setIniCodec("UTF-8");
        userSettings.setValue("PairedDevice/" + devicePub, didPub);
        userSettings.sync();
        if (userSettings.status() != QSettings::NoError)
            qWarning() << "P2P bindDeviceDID: user settings write failed:" << userFile
                       << "status" << userSettings.status();
    } else if (settings) {
        qWarning() << "P2P bindDeviceDID: no user settings file resolved - main settings fallback";
        settings->setValue("PairedDevice/" + devicePub, didPub);
        settings->sync();
    }
}

static QString boundDeviceDID(QSettings *settings, const QString &devicePub)
{
    if (devicePub.isEmpty()) return QString();
    QString userFile = userConfigFilePath(settings);
    if (!userFile.isEmpty()) {
        QSettings userSettings(userFile, QSettings::IniFormat);
        userSettings.setIniCodec("UTF-8");
        QString did = userSettings.value("PairedDevice/" + devicePub).toString();
        if (!did.isEmpty()) return did;
    }
    if (settings && settings->contains("PairedDevice/" + devicePub))
        return settings->value("PairedDevice/" + devicePub).toString();
    qWarning() << "P2P boundDeviceDID: no binding found for devicePub" << devicePub
               << "(userFile:" << userFile << "status:" << (userFile.isEmpty() ? QString("none") : QString("resolved")) << ")";
    return QString();
}

//Registers the DID (marker) and binds the device pubkey to it.
static void registerPairing(QSettings *settings, const QString &didPub, const QString &devicePub)
{
    QString userFile = userConfigFilePath(settings);
    if (!userFile.isEmpty()) {
        QSettings userSettings(userFile, QSettings::IniFormat);
        userSettings.setIniCodec("UTF-8");
        bindDeviceDID(&userSettings, devicePub, didPub);
        userSettings.setValue("PairedDIDs/" + didPub, QString::number(QDateTime::currentSecsSinceEpoch()));
        userSettings.sync();
    } else if (settings) {
        settings->setValue("PairedDIDs/" + didPub, QString::number(QDateTime::currentSecsSinceEpoch()));
    }
}

// The DID pairing proof message: binds the DID, the client device pubkey and
// the session device id. Signed by the DID master key (at first login) or by
// the certified device key (on every subsequent session start).
static QString seaMessage(const QString &didPub, const QString &devicePub, const QString &deviceId)
{
    return QStringLiteral("did:%1,device:%2,session:%3").arg(didPub, devicePub, deviceId);
}

// Verifies a GunDB SEA signature ("SEA"+JSON {"m": msg, "s": base64(r||s)})
// against a Gun SEA public key ("x"+"y" base64url coordinates, P-256 curve).
// The signature is a WebCrypto raw r||s over SHA-256(msg).
static bool seaVerifySignature(const QString &sigStr, const QString &msg, const QString &pub)
{
    if (!sigStr.startsWith("SEA") || pub.isEmpty() || msg.isEmpty()) return false;

    QJsonDocument doc = QJsonDocument::fromJson(sigStr.mid(3).toUtf8());
    if (!doc.isObject()) return false;
    const QJsonObject obj = doc.object();
    if (obj.value("m").toString() != msg) return false;
    QByteArray rawSig = QByteArray::fromBase64(obj.value("s").toString().toUtf8());
    if (rawSig.size() != 64) return false;

    QStringList xy = pub.split('.');
    if (xy.size() != 2) return false;
    QByteArray x = QByteArray::fromBase64(xy[0].toUtf8(),
                        QByteArray::Base64UrlEncoding | QByteArray::AbortOnBase64DecodingErrors);
    QByteArray y = QByteArray::fromBase64(xy[1].toUtf8(),
                        QByteArray::Base64UrlEncoding | QByteArray::AbortOnBase64DecodingErrors);
    if (x.size() != 32 || y.size() != 32) return false;

    QByteArray digest = QCryptographicHash::hash(msg.toUtf8(), QCryptographicHash::Sha256);
    QByteArray point = QByteArray(1, char(4)) + x + y; //uncompressed X9.63 point

    bool ok = false;
    EVP_PKEY *pkey = NULL;
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
    //OpenSSL 3: raw EC point import via OSSL_PARAM (X9.63 uncompressed point
    //plus the explicit curve name; EVP_PKEY_new_raw_public_key does not take
    //a bare EC point).
    char groupName[] = "prime256v1";
    OSSL_PARAM ecParams[3];
    ecParams[0] = OSSL_PARAM_construct_utf8_string(OSSL_PKEY_PARAM_GROUP_NAME, groupName, 0);
    ecParams[1] = OSSL_PARAM_construct_octet_string(OSSL_PKEY_PARAM_PUB_KEY,
                        const_cast<void*>((const void*)point.constData()), point.size());
    ecParams[2] = OSSL_PARAM_construct_end();
    EVP_PKEY_CTX *fromCtx = EVP_PKEY_CTX_new_from_name(NULL, "EC", NULL);
    if (fromCtx) {
        EVP_PKEY_fromdata_init(fromCtx);
        EVP_PKEY_fromdata(fromCtx, &pkey, EVP_PKEY_PUBLIC_KEY, ecParams);
        EVP_PKEY_CTX_free(fromCtx);
    }
#else
    EC_KEY *ecKey = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
    if (ecKey) {
        if (EC_KEY_oct2key(ecKey, (const unsigned char*)point.constData(), point.size(), NULL) == 1) {
            pkey = EVP_PKEY_new();
            if (pkey && EVP_PKEY_assign_EC_KEY(pkey, ecKey) == 1) {
                ecKey = NULL; // ownership transferred to pkey
            } else {
                EVP_PKEY_free(pkey);
                pkey = NULL;
            }
        }
        if (ecKey) EC_KEY_free(ecKey);
    }
#endif

    ECDSA_SIG *sigStruct = ECDSA_SIG_new();
    if (sigStruct && pkey) {
        BIGNUM *r = BN_bin2bn((const unsigned char*)rawSig.constData(), 32, NULL);
        BIGNUM *s = BN_bin2bn((const unsigned char*)rawSig.constData() + 32, 32, NULL);
        if (r && s && ECDSA_SIG_set0(sigStruct, r, s) == 1) {
            unsigned char *der = NULL;
            int derLen = i2d_ECDSA_SIG(sigStruct, &der);
            if (derLen > 0) {
                EVP_MD_CTX *ctx = EVP_MD_CTX_new();
                if (ctx && EVP_DigestVerifyInit(ctx, NULL, EVP_sha256(), NULL, pkey) == 1)
                    //EVP_DigestVerify hashes the input itself: gun/WebCrypto sign
                    //subtle with the message SHA-256 digest as the input, so the
                    //signature is effectively over the double hash (SHA-256(SHA-256(msg))).
                    ok = (EVP_DigestVerify(ctx, der, derLen,
                          (const unsigned char*)digest.constData(), digest.size()) == 1);
                if (ctx) EVP_MD_CTX_free(ctx);
                OPENSSL_free(der);
            }
        } else {
            BN_free(r); BN_free(s);
        }
    }
    if (sigStruct) ECDSA_SIG_free(sigStruct);
    if (pkey) EVP_PKEY_free(pkey);
    return ok;
}
#endif //P2P_ENABLED (SEA pairing helpers)

//Attribution-only check for local service tunnel headers (NOT authorization:
//the DataChannel session authLevel is the access boundary). Old-format
//entries (timestamps) still count as registered for attribution.
static bool isDIDRegistered(QSettings *settings, const QString &didPub)
{
    if (didPub.isEmpty()) return false;
    QString userFile = userConfigFilePath(settings);
    if (!userFile.isEmpty()) {
        QSettings userSettings(userFile, QSettings::IniFormat);
        userSettings.setIniCodec("UTF-8");
        if (userSettings.contains("PairedDIDs/" + didPub)) return true;
    }
    if (settings && settings->contains("PairedDIDs/" + didPub))
        return true;
    return false;
}

// --- Trusted caller identity for local AI/service tunnels ------------------

// Removes spoofable identity headers from the incoming request head and
// appends the node-validated ones.
QByteArray OnMyDiskLinkClient::injectCallerHeaders(const QByteArray &head, const QString &user, const QString &role, const QString &targetOwner, const QString &did)
{
    static const QStringList kBlocked = {
        QStringLiteral("x-omd-caller-user"),
        QStringLiteral("x-omd-caller-role"),
        QStringLiteral("x-omd-target-owner"),
        QStringLiteral("x-omd-node-local"),
    };

    QByteArray result;
    const QList<QByteArray> lines = head.split('\n');
    for (int i = 0; i < lines.size(); ++i)
    {
        QByteArray line = lines[i];
        if (line.endsWith('\r'))
            line.chop(1);
        if (line.isEmpty())
            continue; // drop all empty lines; headers are terminated with \r\n below
        const QByteArray lower = line.toLower();
        bool blocked = false;
        for (const QString &suffix : kBlocked) {
            if (lower.startsWith((suffix + QStringLiteral(":")).toUtf8()))
                { blocked = true; break; }
        }
        if (blocked)
            continue;
        result += line + "\r\n";
    }
    if (!user.isEmpty())
        result += QStringLiteral("X-OMD-Caller-User: %1\r\n").arg(user).toUtf8();
    if (!role.isEmpty())
        result += QStringLiteral("X-OMD-Caller-Role: %1\r\n").arg(role.toLower()).toUtf8();
    if (!targetOwner.isEmpty())
        result += QStringLiteral("X-OMD-Target-Owner: %1\r\n").arg(targetOwner).toUtf8();
    if (!did.isEmpty())
        result += QStringLiteral("X-OMD-Caller-DID: %1\r\n").arg(did).toUtf8();
    result += "\r\n";
    return result;
}

// Per-tunnel request-stream reassembly. Requests from the web peer arrive as
// complete HTTP/1.1 requests (the drive serializes them one at a time); we
// wait for the head + Content-Length body, then write to the local service
// with trusted headers. Failures fall back to pass-through so we never wedge
// non-HTTP tunnel payloads.
void OnMyDiskLinkClient::writeInjectedRequestData(const QString &ID, const QByteArray &data, QTcpSocket *tcp)
{
    if (!tcp || !tcp->isOpen())
        return;

    const QByteArray trimmed = data.trimmed();
    if (trimmed == "__ready__" || trimmed == "__ping__" || trimmed == "__pong__")
        return;

    const QString user = m_tunnelCallerUser.value(ID);
    const QString role = m_tunnelCallerRole.value(ID);
    const QString targetOwner = m_tunnelTargetOwner.value(ID);
    if (user.isEmpty() && role.isEmpty() && targetOwner.isEmpty()) {
        tcp->write(data); // not an HTTP-to-service tunnel (legacy/CLI)
        return;
    }

    if (!m_tunnelHttpStream.contains(ID))
        m_tunnelHttpStream.insert(ID, qMakePair(QByteArray(), (qint64)-1)); // bodyLeft<0 = head phase
    auto &state = m_tunnelHttpStream[ID]; // [pending bytes, body-left; <0 = head phase]
    QByteArray &buf = state.first;
    qint64 &bodyLeft = state.second;

    buf.append(data);
    while (true)
    {
        if (bodyLeft < 0) {
            // Strip any control prefixes or stray leading CRLFs
            while (buf.startsWith("__ready__")) buf.remove(0, 9);
            while (buf.startsWith("__ping__")) buf.remove(0, 8);
            while (buf.startsWith("__pong__")) buf.remove(0, 8);
            while (buf.startsWith("\r\n")) buf.remove(0, 2);
            while (buf.startsWith("\n")) buf.remove(0, 1);

            // Fast pass-through for SSH protocol handshake/banner
            if (buf.startsWith("SSH-")) {
                tcp->write(buf);
                buf.clear();
                return;
            }

            const int idx = buf.indexOf("\r\n\r\n");
            if (idx < 0) {
                if (buf.size() > 128 * 1024) {
                    tcp->write(buf); buf.clear(); return; // not HTTP; pass through
                }
                return; // wait for the rest of the head
            }
            const QByteArray head = buf.left(idx + 4);
            qint64 contentLen = 0;
            QString clientDid;
            const QList<QByteArray> hl = head.split('\n');
            for (const QByteArray &line : hl)
            {
                const QByteArray t = line.trimmed();
                const QByteArray lower = t.toLower();
                if (lower.startsWith("content-length:")) {
                    contentLen = t.mid(15).trimmed().toLongLong();
                } else if (lower.startsWith("x-omd-caller-did:") || lower.startsWith("x-omd-did:")) {
                    int colon = t.indexOf(':');
                    if (colon > 0)
                        clientDid = QString::fromUtf8(t.mid(colon + 1).trimmed());
                } else if (clientDid.isEmpty() && (lower.startsWith("token:") || lower.startsWith("x-omd-key:"))) {
                    int colon = t.indexOf(':');
                    if (colon > 0) {
                        QString cand = QString::fromUtf8(t.mid(colon + 1).trimmed());
                        if (isDIDRegistered(m_settings, cand))
                            clientDid = cand;
                    }
                }
            }

            // DID authentication on the node itself:
            // The gateway is merely signaling; authority for node access belongs to
            // the node itself via its paired DIDs (PairedDIDs in user config).
            bool paired = !clientDid.isEmpty() && isDIDRegistered(m_settings, clientDid);
            QString effectiveRole = role.isEmpty() ? QStringLiteral("guest") : role;
            QString effectiveUser = user;
            if (paired) {
                effectiveRole = QStringLiteral("owner");
                if (effectiveUser.isEmpty() || effectiveUser.startsWith("guest")) {
                    effectiveUser = m_omdSettings.user().isEmpty()
                        ? (m_hostRoot ? m_hostRoot->hostName() : QStringLiteral("alexey"))
                        : m_omdSettings.user();
                }
                qInfo() << "P2P HTTP Tunnel" << ID << ": client DID" << clientDid << "is paired -> elevated to OWNER (user:" << effectiveUser << ")";
            }

            tcp->write(injectCallerHeaders(head, effectiveUser, effectiveRole, targetOwner, clientDid));
            buf = buf.mid(idx + 4);
            if (contentLen > 0) {
                bodyLeft = contentLen;
            } else {
                bodyLeft = -1; // no body: ready for next request
                if (!buf.isEmpty())
                    continue;
                return;
            }
        }
        const qint64 take = qMin(bodyLeft, (qint64)buf.size());
        if (take == 0)
            return; // wait for more body bytes
        tcp->write(buf.left(take)); buf = buf.mid(take); bodyLeft -= take;
        if (bodyLeft == 0) {
            bodyLeft = -1; // next request
            if (!buf.isEmpty())
                continue;
        }
        return;
    }

}

void OnMyDiskLinkClient::handleTunnelSignalingRequest(const LinkMessage &message)
{
#ifdef P2P_ENABLED
    const QString ID = message.getData().value("ID").toString();
    if (ID.isEmpty())
        return;
    P2PTunnel *p2p = m_p2pTunnels.value(ID, nullptr);
    if (!p2p)
        return;
    const QString kind = message.getData().value("kind").toString();
    p2p->handleSignaling(kind, message.getPayLoad());
#else
    Q_UNUSED(message)
#endif
}

void OnMyDiskLinkClient::sendTunnelDataFor(const QString &ID, const QByteArray &data)
{
    if (data.isEmpty() || !serviceSocket)
        return;
#ifdef P2P_ENABLED
    P2PTunnel *p2p = m_p2pTunnels.value(ID, nullptr);
    if (p2p && p2p->isOpen())
    {
        p2p->sendData(data);
        return;
    }
#endif
    QByteArray payload = data;
    if (m_tunnelKeys.contains(ID))
    {
        QByteArray encrypted = TunnelCrypto::encryptPacket(m_tunnelKeys[ID], data);
        if (encrypted.isEmpty())
            return;
        payload = encrypted;
    }
    LinkMessage chunk;
    chunk.setMessageType(LinkMessage::TunnelData);
    chunk.setResultCode(LinkMessage::OK);
    chunk.insertDataValue("ID", ID);
    chunk.setPayLoad(payload);
    serviceSocket->writeMessage(chunk);
}

void OnMyDiskLinkClient::handleTunnelDataRequest(const LinkMessage &message)
{
    QString ID = message.getData().value("ID").toString();
    if (ID.isEmpty() || !m_tunnels.contains(ID))
        return;

    QByteArray data = message.getPayLoad();
    if (m_tunnelKeys.contains(ID))
    {
        QByteArray plain;
        if (!TunnelCrypto::decryptPacket(m_tunnelKeys[ID], data, plain))
        {
            qWarning() << "Tunnel" << ID << "decrypt failed, dropping packet";
            return;
        }
        data = plain;
    }
    if (!data.isEmpty())
        writeInjectedRequestData(ID, data, m_tunnels.value(ID));
}

void OnMyDiskLinkClient::sendTunnelSignalingFor(const QString &ID, const QString &kind, const QByteArray &payload)
{
#ifdef P2P_ENABLED
    if (!serviceSocket)
        return;
    LinkMessage sig;
    sig.setMessageType(LinkMessage::TunnelSignaling);
    sig.setResultCode(LinkMessage::Request);
    sig.insertDataValue("ID", ID);
    sig.insertDataValue("kind", kind);
    sig.setPayLoad(payload);
    serviceSocket->writeMessage(sig);
#else
    Q_UNUSED(ID)
    Q_UNUSED(kind)
    Q_UNUSED(payload)
#endif
}

#if defined(Q_OS_LINUX) && defined(P2P_ENABLED)
// Resolve the configured FileNode system user: fills homeDir (falling back to
// the process home), uid and gid. Returns true when the user was found.
static bool resolveFileNodeUser(QSettings *settings, QString &homeDir, uid_t &uid, gid_t &gid)
{
    uid = 0;
    gid = 0;
    QString user = settings ? settings->value("FileNode/User", "").toString() : "";
    QString group = settings ? settings->value("FileNode/Group", "").toString() : "";
    struct passwd *pwd = nullptr;
    if (!user.isEmpty())
        pwd = getpwnam(user.toUtf8().constData());
    if (pwd) {
        homeDir = QString::fromUtf8(pwd->pw_dir);
        uid = pwd->pw_uid;
        gid = pwd->pw_gid;
        if (!group.isEmpty()) {
            struct group *grp = getgrnam(group.toUtf8().constData());
            if (grp) gid = grp->gr_gid;
        }
        return true;
    }
    homeDir = QDir::homePath();
    return false;
}
#endif

// Recursive copy helper for HomeCopy command.
static bool copyRecursively(const QString &srcFilePath, const QString &dstFilePath)
{
    QFileInfo srcFileInfo(srcFilePath);
    if (srcFileInfo.isDir()) {
        QDir targetDir(dstFilePath);
        if (!targetDir.exists() && !targetDir.mkpath(dstFilePath))
            return false;
        QDir sourceDir(srcFilePath);
        QStringList fileNames = sourceDir.entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
        for (const QString &fileName : fileNames) {
            if (!copyRecursively(srcFilePath + "/" + fileName, dstFilePath + "/" + fileName))
                return false;
        }
    } else {
        QFile::remove(dstFilePath);
        if (!QFile::copy(srcFilePath, dstFilePath))
            return false;
    }
    return true;
}

// Resolve a virtual path (as used by the home:true FileInfo browsing) relative
// to the FileNode user home into a real on-disk path. Returns false and sets
// error when the path is invalid or escapes the home directory.
#ifdef P2P_ENABLED
static bool resolveHomeRelativePath(QSettings *settings, const QString &virtualPath, QString &realPath, QString &error)
{
    QString homeDir;
#ifdef Q_OS_LINUX
    uid_t ruid = 0;
    gid_t rgid = 0;
    resolveFileNodeUser(settings, homeDir, ruid, rgid);
#else
    homeDir = QDir::homePath();
#endif

    if (virtualPath.contains("..")) {
        error = "Access Denied: Invalid path";
        return false;
    }
    QString cleaned = virtualPath.startsWith("/") ? virtualPath.mid(1) : virtualPath;
    realPath = QDir::cleanPath(homeDir + "/" + cleaned);
    if (!realPath.startsWith(homeDir)) {
        error = "Access Denied: path escapes home";
        return false;
    }
    return true;
}
#endif //resolveHomeRelativePath

#ifdef P2P_ENABLED
//Stores the DID together with a random pairing secret (per client device,
//so pairing one browser does not invalidate another) and returns the secret:
//the client must present it in PairAuth/AuthInfo (the pubkey alone is public).
//Old-format entries (timestamps) stop working - the node asks to re-pair once.
static bool hasAnyPairedDIDs(QSettings *settings)
{
    QString userFile = userConfigFilePath(settings);
    if (!userFile.isEmpty()) {
        QSettings userSettings(userFile, QSettings::IniFormat);
        userSettings.setIniCodec("UTF-8");
        userSettings.beginGroup("PairedDIDs");
        bool has = !userSettings.childGroups().isEmpty() || !userSettings.childKeys().isEmpty();
        userSettings.endGroup();
        if (has) return true;
    }
    if (settings) {
        settings->beginGroup("PairedDIDs");
        bool has = !settings->childGroups().isEmpty() || !settings->childKeys().isEmpty();
        settings->endGroup();
        if (has) return true;
    }
    return false;
}

// Guest DID registry (public AI node only): separate group so guest keys are
// never treated as paired (PairAuth can not promote a guest DID to admin).
static void saveGuestDID(QSettings *settings, const QString &didPub)
{
    QString userFile = userConfigFilePath(settings);
    if (!userFile.isEmpty()) {
        QSettings userSettings(userFile, QSettings::IniFormat);
        userSettings.setIniCodec("UTF-8");
        userSettings.setValue("GuestDIDs/" + didPub, QDateTime::currentSecsSinceEpoch());
        userSettings.sync();
    }
}

static bool isGuestDID(QSettings *settings, const QString &didPub)
{
    if (didPub.isEmpty()) return false;
    QString userFile = userConfigFilePath(settings);
    if (!userFile.isEmpty()) {
        QSettings userSettings(userFile, QSettings::IniFormat);
        userSettings.setIniCodec("UTF-8");
        if (userSettings.contains("GuestDIDs/" + didPub)) return true;
    }
    if (settings && settings->contains("GuestDIDs/" + didPub))
        return true;
    return false;
}

static bool guestPairingAllowed(QSettings *settings)
{
    return settings ? settings->value(QStringLiteral("Settings/GuestPairing"), false).toBool() : false;
}

static void saveP2PShare(QSettings *settings, const QString &shareId, const QString &cleanPath, bool writable, const QString &name)
{
    QString userFile = userConfigFilePath(settings);
    if (!userFile.isEmpty()) {
        QSettings userSettings(userFile, QSettings::IniFormat);
        userSettings.setIniCodec("UTF-8");
        userSettings.beginGroup("P2PShares/" + shareId);
        userSettings.setValue("path", cleanPath);
        userSettings.setValue("writable", writable);
        userSettings.setValue("name", name);
        userSettings.setValue("createdAt", QDateTime::currentSecsSinceEpoch());
        userSettings.endGroup();
        userSettings.sync();
    }
}

static void removeP2PShare(QSettings *settings, const QString &shareId)
{
    QString userFile = userConfigFilePath(settings);
    if (!userFile.isEmpty()) {
        QSettings userSettings(userFile, QSettings::IniFormat);
        userSettings.setIniCodec("UTF-8");
        userSettings.remove("P2PShares/" + shareId);
        userSettings.sync();
    }
    if (settings) {
        settings->remove("P2PShares/" + shareId);
        settings->sync();
    }
}

static bool getP2PShare(QSettings *settings, const QString &shareId, QString &scopePath, bool &writable)
{
    QString userFile = userConfigFilePath(settings);
    if (!userFile.isEmpty()) {
        QSettings userSettings(userFile, QSettings::IniFormat);
        userSettings.setIniCodec("UTF-8");
        userSettings.beginGroup("P2PShares/" + shareId);
        if (userSettings.contains("path")) {
            scopePath = userSettings.value("path").toString();
            writable = userSettings.value("writable", false).toBool();
            userSettings.endGroup();
            return true;
        }
        userSettings.endGroup();
    }
    if (settings) {
        settings->beginGroup("P2PShares/" + shareId);
        if (settings->contains("path")) {
            scopePath = settings->value("path").toString();
            writable = settings->value("writable", false).toBool();
            settings->endGroup();
            return true;
        }
        settings->endGroup();
    }
    return false;
}
#endif /* P2P_ENABLED */

void OnMyDiskLinkClient::handleP2PFileRequest(P2PTunnel *p2p, const QByteArray &data)
{
#ifdef P2P_ENABLED
    if (data.isEmpty() || !p2p)
        return;

    QList<QJsonObject> commandsToProcess;
    {
        QMutexLocker locker(&m_p2pMutex);
        P2PSession &session = m_p2pSessions[p2p];
        session.rxBuffer.append(data);

        while (!session.rxBuffer.isEmpty()) {
            // Strip leading whitespace or newlines
            while (!session.rxBuffer.isEmpty() && (session.rxBuffer.at(0) == '\n' || session.rxBuffer.at(0) == '\r' || session.rxBuffer.at(0) == ' ' || session.rxBuffer.at(0) == '\t')) {
                session.rxBuffer.remove(0, 1);
            }
            if (session.rxBuffer.isEmpty()) break;

            QJsonParseError err;
            QJsonDocument doc = QJsonDocument::fromJson(session.rxBuffer, &err);

            if (err.error == QJsonParseError::NoError && doc.isObject()) {
                session.rxBuffer.clear();
                commandsToProcess.append(doc.object());
                break;
            }

            if (err.error == QJsonParseError::GarbageAtEnd && err.offset > 0) {
                QByteArray firstMsg = session.rxBuffer.left(err.offset);
                session.rxBuffer.remove(0, err.offset);
                QJsonDocument subDoc = QJsonDocument::fromJson(firstMsg);
                if (subDoc.isObject()) {
                    commandsToProcess.append(subDoc.object());
                }
                continue;
            }

            // Incomplete JSON: wait for more packets
            if (session.rxBuffer.size() > 16 * 1024 * 1024) {
                qWarning() << "P2P FS: Buffer exceeded limit, clearing:" << session.rxBuffer.size();
                session.rxBuffer.clear();
            }
            break;
        }
    }

    for (const QJsonObject &req : commandsToProcess) {
        processP2PFileCommand(p2p, req);
    }
#else
    Q_UNUSED(p2p)
    Q_UNUSED(data)
#endif
}

void OnMyDiskLinkClient::processP2PFileCommand(P2PTunnel *p2p, const QJsonObject &req)
{
#ifdef P2P_ENABLED
    if (!p2p) return;

    P2PSession session;
    {
        QMutexLocker locker(&m_p2pMutex);
        session = m_p2pSessions.value(p2p);
    }

    int reqId = req.value("id").toInt();
    QString cmd = req.value("cmd").toString();
    QString pathStr = req.value("path").toString();
    QString queryStr = req.value("query").toString();
    
    qInfo() << "P2P FS: Received request" << reqId << "cmd:" << cmd << "path:" << pathStr << "query:" << queryStr;

    QJsonObject resp;
    resp["id"] = reqId;
    resp["cmd"] = cmd;

    if (cmd == "AuthInfo") {
        QString checkDid = req.value("didPub").toString();
        bool isPaired = didRegistered(m_settings, checkDid);
        bool hasPaired = hasAnyPairedDIDs(m_settings);
        if (session.authLevel == P2PAuthLevel::Admin) {
            isPaired = true;
        }

        resp["status"] = "ok";
        QJsonObject result;
        result["linkId"] = m_linkId;
        result["name"] = m_hostRoot ? m_hostRoot->hostName() : QHostInfo::localHostName();
        result["hasPairedDids"] = hasPaired;
        result["paired"] = isPaired;
        result["guestPairing"] = guestPairingAllowed(m_settings);
        result["authLevel"] = (session.authLevel == P2PAuthLevel::Admin) ? "admin" :
                              (session.authLevel == P2PAuthLevel::ScopedGuest ? "guest" : "unauthenticated");
        resp["result"] = result;
        p2p->sendText(QJsonDocument(resp).toJson(QJsonDocument::Compact));
        return;
    }

    if (cmd == "Login") {
        QString user = req.value("user").toString();
        QString pass = req.value("pass").toString();
        QString didPub = req.value("didPub").toString();
        QString deviceId = req.value("deviceId").toString();
        QString devicePub = req.value("devicePub").toString();
        QString didProof = req.value("didProof").toString();

        if (!authenticateNodeCredentials(m_settings, user, pass)) {
            resp["status"] = "error";
            resp["error"] = "Invalid username or password";
            p2p->sendText(QJsonDocument(resp).toJson(QJsonDocument::Compact));
            return;
        }

        session.authLevel = P2PAuthLevel::Admin;
        session.user = user;
        session.didPub = didPub;
        {
            QMutexLocker locker(&m_p2pMutex);
            m_p2pSessions[p2p].authLevel = P2PAuthLevel::Admin;
            m_p2pSessions[p2p].user = user;
            m_p2pSessions[p2p].didPub = didPub;
        }

        bool didRegisteredHere = false;
        qInfo() << "P2P Login: user len" << user.length() << "didPub len" << didPub.length()
                << "devicePub len" << devicePub.length() << "didProof len" << didProof.length();
        if (!didPub.isEmpty()) {
            //A fresh DID joins only with its master key (in memory right after a
            //DID password login). For a DID already registered on this node, the
            //node admin (the OS-credential holder) may attach this device at login:
            //the master key is not kept during long-lived device sessions.
            const bool masterSigValid = !devicePub.isEmpty() && !didProof.isEmpty()
                && seaVerifySignature(didProof, seaMessage(didPub, devicePub, deviceId), didPub);
            const bool marker = didRegistered(m_settings, didPub);
            if (marker || masterSigValid) {
                registerPairing(m_settings, didPub, devicePub);
                didRegisteredHere = true;
                qInfo() << "P2P Auth: DID" << didPub
                        << (masterSigValid ? "registered via master proof"
                                           : "device attached via OS login (registered marker)")
                        << "for user" << user << "devicePub" << devicePub;
            } else {
                qWarning() << "P2P Login: DID is not registered on this node and no valid master proof - device attach refused";
            }
        }

        resp["status"] = "ok";
        QJsonObject result;
        result["user"] = user;
        result["name"] = m_hostRoot ? m_hostRoot->hostName() : QHostInfo::localHostName();
        result["linkId"] = m_linkId;
        result["paired"] = true;
        result["authLevel"] = "admin";
        if (!didPub.isEmpty() && !didRegisteredHere)
            qWarning() << "P2P Login: DID registration did not happen (didProof missing or invalid)";
        result["didRegistered"] = didRegisteredHere;
        resp["result"] = result;
        p2p->sendText(QJsonDocument(resp).toJson(QJsonDocument::Compact));
        return;
    }

    if (cmd == "PairAuth") {
        QString didPub = req.value("didPub").toString();
        QString devicePub = req.value("devicePub").toString();
        QString deviceId = req.value("deviceId").toString();
        QString proof = req.value("proof").toString();
        QString msg = seaMessage(didPub, devicePub, deviceId);

        const bool deviceSigValid = !devicePub.isEmpty() && seaVerifySignature(proof, msg, devicePub);
        const bool deviceProven = deviceSigValid
            && boundDeviceDID(m_settings, devicePub) == didPub
            && didRegistered(m_settings, didPub);
        const bool masterProven = seaVerifySignature(proof, msg, didPub);
        qInfo() << "P2P PairAuth: didPub len" << didPub.length() << "devicePub len" << devicePub.length()
                << "deviceId len" << deviceId.length() << "proof len" << proof.length()
                << "deviceProof" << deviceProven << "masterProof" << masterProven;
        if (!deviceProven && !masterProven) {
            qWarning() << "P2P PairAuth rejected: devicePub" << devicePub
                       << "expected DID" << boundDeviceDID(m_settings, devicePub)
                       << "vs didPub" << didPub
                       << "didRegistered" << didRegistered(m_settings, didPub)
                       << "masterSigValid" << (!proof.isEmpty() && seaVerifySignature(proof, msg, didPub));
            resp["status"] = "error";
            resp["error"] = "DID pairing proof failed - sign in with the DID once";
            p2p->sendText(QJsonDocument(resp).toJson(QJsonDocument::Compact));
            return;
        }
        if (masterProven) {
            //The DID master key proves ownership: register the DID and bind the device.
            registerPairing(m_settings, didPub, devicePub);
        }

        session.authLevel = P2PAuthLevel::Admin;
        session.didPub = didPub;
        {
            QMutexLocker locker(&m_p2pMutex);
            m_p2pSessions[p2p].authLevel = P2PAuthLevel::Admin;
            m_p2pSessions[p2p].didPub = didPub;
        }

        resp["status"] = "ok";
        QJsonObject result;
        result["name"] = m_hostRoot ? m_hostRoot->hostName() : QHostInfo::localHostName();
        result["linkId"] = m_linkId;
        result["paired"] = true;
        result["authLevel"] = "admin";
        resp["result"] = result;
        p2p->sendText(QJsonDocument(resp).toJson(QJsonDocument::Compact));
        return;
    }

    // Guest pairing (public AI node): register the client DID with guest
    // rights, no password requested. Keys are stored in GuestDIDs so a guest
    // can never pass PairAuth / Login-based promotions, and the session only
    // reaches ScopedGuest level (no kernel/share/file admin operations).
    if (cmd == "GuestPair") {
        const bool guestPairing = guestPairingAllowed(m_settings);
        if (!guestPairing) {
            resp["status"] = "error";
            resp["error"] = "Guest pairing is disabled on this node";
            p2p->sendText(QJsonDocument(resp).toJson(QJsonDocument::Compact));
            return;
        }
        QString didPub = req.value("didPub").toString();
        if (didPub.isEmpty()) {
            resp["status"] = "error";
            resp["error"] = "DID pubkey is required";
            p2p->sendText(QJsonDocument(resp).toJson(QJsonDocument::Compact));
            return;
        }

        session.authLevel = P2PAuthLevel::ScopedGuest;
        session.didPub = didPub;
        {
            QMutexLocker locker(&m_p2pMutex);
            m_p2pSessions[p2p].authLevel = P2PAuthLevel::ScopedGuest;
            m_p2pSessions[p2p].didPub = didPub;
        }

        if (!isGuestDID(m_settings, didPub)) {
            saveGuestDID(m_settings, didPub);
        }
        qInfo() << "P2P Auth: guest DID paired" << didPub;

        resp["status"] = "ok";
        QJsonObject result;
        result["name"] = m_hostRoot ? m_hostRoot->hostName() : QHostInfo::localHostName();
        result["linkId"] = m_linkId;
        result["paired"] = true;
        result["guest"] = true;
        result["authLevel"] = "guest";
        resp["result"] = result;
        p2p->sendText(QJsonDocument(resp).toJson(QJsonDocument::Compact));
        return;
    }

    // ConsentAuth: granted to AI generation nodes or authorized peers
    if (cmd == "ConsentAuth") {
        QString token = req.value("token").toString();
        QString didPub = req.value("didPub").toString();
        QString scopePath = req.value("path").toString();
        bool writable = req.value("writable").toBool(true);

        QString cleanPath = scopePath;
        if (cleanPath.endsWith('/') && cleanPath.length() > 1)
            cleanPath.chop(1);
        if (cleanPath.isEmpty())
            cleanPath = "/";

        session.authLevel = P2PAuthLevel::ScopedGuest;
        session.didPub = didPub;
        session.scopePath = cleanPath;
        session.writable = writable;
        {
            QMutexLocker locker(&m_p2pMutex);
            m_p2pSessions[p2p].authLevel = P2PAuthLevel::ScopedGuest;
            m_p2pSessions[p2p].didPub = didPub;
            m_p2pSessions[p2p].scopePath = cleanPath;
            m_p2pSessions[p2p].writable = writable;
        }

        qInfo() << "P2P Auth: ConsentAuth accepted for scope" << cleanPath << "writable:" << writable;

        resp["status"] = "ok";
        QJsonObject result;
        result["authLevel"] = "granted";
        result["scope"] = cleanPath;
        result["writable"] = writable;
        result["linkId"] = m_linkId;
        resp["result"] = result;
        p2p->sendText(QJsonDocument(resp).toJson(QJsonDocument::Compact));
        return;
    }

    if (cmd == "CreateShare") {
        if (session.authLevel != P2PAuthLevel::Admin) {
            resp["status"] = "error";
            resp["error"] = "Access Denied: Administrator access required to create shares";
            p2p->sendText(QJsonDocument(resp).toJson(QJsonDocument::Compact));
            return;
        }

        QString sharePath = req.value("path").toString();
        bool writable = req.value("writable").toBool(false);
        QString name = req.value("name").toString();

        QString cleanPath = sharePath;
        if (cleanPath.endsWith('/') && cleanPath.length() > 1)
            cleanPath.chop(1);

        const QString chars = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
        QString shareId = "sh_";
        for (int i = 0; i < 16; ++i) {
            shareId.append(chars.at(QRandomGenerator::global()->bounded(chars.length())));
        }

        if (m_settings) {
            saveP2PShare(m_settings, shareId, cleanPath, writable, name);
        }

        resp["status"] = "ok";
        QJsonObject result;
        result["shareId"] = shareId;
        result["name"] = name.isEmpty() ? QFileInfo(cleanPath).fileName() : name;
        result["writable"] = writable;
        resp["result"] = result;
        p2p->sendText(QJsonDocument(resp).toJson(QJsonDocument::Compact));
        return;
    }

    if (cmd == "RevokeShare") {
        if (session.authLevel != P2PAuthLevel::Admin) {
            resp["status"] = "error";
            resp["error"] = "Access Denied: Administrator access required to revoke shares";
            p2p->sendText(QJsonDocument(resp).toJson(QJsonDocument::Compact));
            return;
        }

        QString shareId = req.value("shareId").toString();
        if (!shareId.isEmpty() && m_settings) {
            removeP2PShare(m_settings, shareId);
        }

        resp["status"] = "ok";
        p2p->sendText(QJsonDocument(resp).toJson(QJsonDocument::Compact));
        return;
    }

    if (cmd == "SetScope") {
        QString shareId = req.value("shareId").toString();
        QString scopePath = req.value("path").toString();
        bool writable = req.value("writable").toBool(false);

        if (!shareId.isEmpty() && m_settings) {
            if (!getP2PShare(m_settings, shareId, scopePath, writable)) {
                resp["status"] = "error";
                resp["error"] = "Share not found or revoked";
                p2p->sendText(QJsonDocument(resp).toJson(QJsonDocument::Compact));
                return;
            }
        } else if (session.authLevel != P2PAuthLevel::Admin && !scopePath.isEmpty()) {
            //Guest (ToS-accepted via GuestPair) path scoping: allowed only under
            //the node's public "sites" root - that's how site visitors get
            //scoped access to Jekyll sites. Any other path needs a shareId.
            const bool isSiteScope = (scopePath == QStringLiteral("sites") || scopePath.startsWith(QStringLiteral("sites/")));
            if (session.authLevel != P2PAuthLevel::ScopedGuest || !isSiteScope) {
                resp["status"] = "error";
                resp["error"] = "Access Denied: valid shareId required";
                p2p->sendText(QJsonDocument(resp).toJson(QJsonDocument::Compact));
                return;
            }
        }


        QString cleanPath = scopePath;
        if (cleanPath.endsWith('/') && cleanPath.length() > 1)
            cleanPath.chop(1);

        QStringList scopeList = cleanPath.split('/', Qt::SkipEmptyParts);
        CloudResource *scopeRes = m_hostRoot ? m_hostRoot->getChildClone(scopeList, "", false) : nullptr;
        if (!scopeRes || !scopeRes->exists()) {
            resp["status"] = "error";
            resp["error"] = "Scope path not found: " + cleanPath;
            if (scopeRes && scopeRes->isCopy()) delete scopeRes;
            p2p->sendText(QJsonDocument(resp).toJson(QJsonDocument::Compact));
            return;
        }

        session.authLevel = P2PAuthLevel::ScopedGuest;
        session.scopePath = cleanPath;
        session.writable = writable;
        session.shareId = shareId;
        {
            QMutexLocker locker(&m_p2pMutex);
            m_p2pSessions[p2p].authLevel = P2PAuthLevel::ScopedGuest;
            m_p2pSessions[p2p].scopePath = cleanPath;
            m_p2pSessions[p2p].writable = writable;
            m_p2pSessions[p2p].shareId = shareId;
        }

        resp["status"] = "ok";
        QJsonObject result;
        result["name"] = scopeRes->fileName();
        result["path"] = cleanPath;
        result["isDir"] = scopeRes->isDir();
        result["size"] = scopeRes->size();
        result["writable"] = writable;
        resp["result"] = result;
        if (scopeRes && scopeRes->isCopy()) delete scopeRes;
        p2p->sendText(QJsonDocument(resp).toJson(QJsonDocument::Compact));
        return;
    }

    bool isAdmin = (session.authLevel == P2PAuthLevel::Admin);
    bool isScoped = (session.authLevel == P2PAuthLevel::ScopedGuest);

    // Exec: run a shell command as the FileNode user and return its output.
    if (cmd == "Exec") {
        if (!isAdmin) {
            resp["status"] = "error";
            resp["error"] = "Access Denied: Exec requires administrator privileges";
            p2p->sendText(QJsonDocument(resp).toJson(QJsonDocument::Compact));
            return;
        }

        QString execDir = req.value("dir").toString();
        QString execInput = req.value("input").toString();
        if (execInput.size() > (64 * 1024)) {
            resp["status"] = "error";
            resp["error"] = "Exec input too large";
            p2p->sendText(QJsonDocument(resp).toJson(QJsonDocument::Compact));
            return;
        }

        QString execUser = m_settings ? m_settings->value("FileNode/User", "").toString() : "";

        QString homeDir;
        bool useSetpriv = false;
#ifdef Q_OS_LINUX
        uid_t execUid = 0;
        gid_t execGid = 0;
        useSetpriv = resolveFileNodeUser(m_settings, homeDir, execUid, execGid) && execUid != 0;
#else
        homeDir = QDir::homePath();
#endif

        // Resolve "~" / "~/" against the user home so the browser can send the
        // cwd exactly as the terminal reports it (OSC 7777).
        if (execDir.isEmpty())
            execDir = homeDir;
        else if (execDir == "~")
            execDir = homeDir;
        else if (execDir.startsWith("~/"))
            execDir = homeDir + execDir.mid(1);
        if (execDir.isEmpty())
            execDir = "/";

        QStringList execArgs;
#ifdef Q_OS_LINUX
        if (useSetpriv) {
            execArgs << "--reuid" << QString::number(execUid)
                     << "--regid" << QString::number(execGid)
                     << "--init-groups"
                     << "bash" << "-c" << execInput;
        } else
#endif
        {
            execArgs << "-c" << execInput;
        }

        QProcess proc;
        proc.setProgram(useSetpriv ? "setpriv" : "bash");
        proc.setArguments(execArgs);
        proc.setWorkingDirectory(execDir);
        QStringList env = QProcess::systemEnvironment();
        QStringList filteredEnv;
        for (const QString &kv : env) {
            if (!kv.startsWith("HOME="))
                filteredEnv << kv;
        }
        filteredEnv << ("HOME=" + homeDir);
        proc.setEnvironment(filteredEnv);

        qInfo() << "P2P FS: Exec user:" << execUser << "dir:" << execDir;
        proc.start();
        bool finished = proc.waitForFinished(15000);
        if (!finished && proc.state() == QProcess::NotRunning) {
            resp["status"] = "error";
            resp["error"] = "Exec failed to start: " + proc.errorString();
        } else if (!finished) {
            proc.kill();
            proc.waitForFinished(1000);
            resp["status"] = "error";
            resp["error"] = "Exec timed out";
        } else {
            resp["status"] = "ok";
            QJsonObject result;
            result["exit"] = proc.exitCode();
            result["stdout"] = QString::fromLatin1(proc.readAllStandardOutput().toBase64());
            result["stderr"] = QString::fromLatin1(proc.readAllStandardError().toBase64());
            resp["result"] = result;
        }
        p2p->sendText(QJsonDocument(resp).toJson(QJsonDocument::Compact));
        return;
    }

    // ResolvePath: map a logical (share-relative) path like "/Knowledge/data"
    if (cmd == "ResolvePath") {
        if (!isAdmin) {
            resp["status"] = "error";
            resp["error"] = "Access Denied: ResolvePath requires administrator privileges";
            p2p->sendText(QJsonDocument(resp).toJson(QJsonDocument::Compact));
            return;
        }

        QString logical = req.value("path").toString();
        QString homeDir;
#ifdef Q_OS_LINUX
        uid_t ruid = 0;
        gid_t rgid = 0;
        resolveFileNodeUser(m_settings, homeDir, ruid, rgid);
#else
        homeDir = QDir::homePath();
#endif
        QStringList parts = logical.split('/', Qt::SkipEmptyParts);
        QString realPath;
        if (parts.isEmpty()) {
            realPath = homeDir;
        } else {
            QString shareName = parts.takeFirst();
            QString base = shares.value(shareName);
            if (base.isEmpty()) {
                resp["status"] = "error";
                resp["error"] = "Unknown share: " + shareName;
                p2p->sendText(QJsonDocument(resp).toJson(QJsonDocument::Compact));
                return;
            }
            realPath = base;
            if (!parts.isEmpty())
                realPath += "/" + parts.join("/");
        }
        resp["status"] = "ok";
        QJsonObject result;
        result["realPath"] = realPath;
        resp["result"] = result;
        p2p->sendText(QJsonDocument(resp).toJson(QJsonDocument::Compact));
        return;
    }

    // AddShare: register a new logical share for a real on-disk folder.
    if (cmd == "AddShare") {
        if (!isAdmin) {
            resp["status"] = "error";
            resp["error"] = "Access Denied: AddShare requires administrator privileges";
            p2p->sendText(QJsonDocument(resp).toJson(QJsonDocument::Compact));
            return;
        }

        QString sharePath = req.value("path").toString();
        if (sharePath.isEmpty()) {
            resp["status"] = "error";
            resp["error"] = "Missing path";
            p2p->sendText(QJsonDocument(resp).toJson(QJsonDocument::Compact));
            return;
        }

        // If sharePath is virtual/relative, resolve it using FileNode home
        if (!QFileInfo(sharePath).exists()) {
            QString realSharePath;
            QString resolveErr;
            if (resolveHomeRelativePath(m_settings, sharePath, realSharePath, resolveErr)) {
                if (QFileInfo(realSharePath).exists()) {
                    sharePath = realSharePath;
                }
            }
        }

        QString shareName = QFileInfo(sharePath).fileName();
        if (shareName.isEmpty())
            shareName = "Share";

        addLocalShare(shareName, sharePath, true);
        emit UserEvents::instance().addShareRequested(m_user, sharePath);

        resp["status"] = "ok";
        QJsonObject result;
        result["name"] = shareName;
        result["path"] = sharePath;
        resp["result"] = result;
        p2p->sendText(QJsonDocument(resp).toJson(QJsonDocument::Compact));
        return;
    }

    // FileInfo with the "home" flag browses the real filesystem on the node
    bool browseHome = req.value("home").toBool(false);
    if (cmd == "FileInfo" && browseHome) {
        if (!isAdmin) {
            resp["status"] = "error";
            resp["error"] = "Access Denied: home browsing requires administrator privileges";
            p2p->sendText(QJsonDocument(resp).toJson(QJsonDocument::Compact));
            return;
        }

        QString homeDir;
#ifdef Q_OS_LINUX
        uid_t ruid = 0;
        gid_t rgid = 0;
        resolveFileNodeUser(m_settings, homeDir, ruid, rgid);
#else
        homeDir = QDir::homePath();
#endif

        QString realDir = homeDir;
        QString rel = pathStr;
        if (!rel.isEmpty() && rel != "/") {
            if (rel.contains("..")) {
                resp["status"] = "error";
                resp["error"] = "Access Denied: Invalid path";
                p2p->sendText(QJsonDocument(resp).toJson(QJsonDocument::Compact));
                return;
            }
            QString cleaned = rel.startsWith("/") ? rel.mid(1) : rel;
            realDir = QDir::cleanPath(homeDir + "/" + cleaned);
            if (!realDir.startsWith(homeDir)) {
                resp["status"] = "error";
                resp["error"] = "Access Denied: path escapes home";
                p2p->sendText(QJsonDocument(resp).toJson(QJsonDocument::Compact));
                return;
            }
        }

        QDir dir(realDir);
        QJsonObject result;
        result["name"] = (rel.isEmpty() || rel == "/") ? homeDir : QFileInfo(realDir).fileName();
        result["path"] = pathStr;
        result["isDir"] = true;
        result["exists"] = dir.exists();
        result["realPath"] = realDir;

        QJsonArray files;
        if (dir.exists()) {
            QFileInfoList entries = dir.entryInfoList(
                QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot | QDir::Readable, QDir::Name);
            for (const QFileInfo &fi : entries) {
                if (fi.fileName().startsWith("."))
                    continue;
                QJsonObject item;
                item["name"] = fi.fileName();
                item["isDir"] = fi.isDir();
                item["size"] = fi.isDir() ? 0 : fi.size();
                QDateTime mediaDate = LocalFileBackend::getMediaCreationDateTime(fi);
                item["lastModified"] = mediaDate.isValid() ? mediaDate.toString(Qt::ISODate) : fi.lastModified().toString(Qt::ISODate);
                if (mediaDate.isValid()) {
                    item["mediaDate"] = mediaDate.toString(Qt::ISODate);
                }
                item["mimeType"] = fi.isDir() ? QStringLiteral("inode/directory") : QString();
                files.append(item);
            }
        }
        result["files"] = files;

        resp["status"] = "ok";
        resp["result"] = result;
        p2p->sendText(QJsonDocument(resp).toJson(QJsonDocument::Compact));
        return;
    }

    // HomeMkDir: create a directory on the real filesystem (home browsing).
    if (cmd == "HomeMkDir") {
        if (!isAdmin) {
            resp["status"] = "error";
            resp["error"] = "Access Denied: HomeMkDir requires administrator privileges";
            p2p->sendText(QJsonDocument(resp).toJson(QJsonDocument::Compact));
            return;
        }

        QString rel = req.value("path").toString();
        if (rel.isEmpty() || rel == "/") {
            resp["status"] = "error";
            resp["error"] = "Invalid path";
            p2p->sendText(QJsonDocument(resp).toJson(QJsonDocument::Compact));
            return;
        }

        QString realDir;
        QString pathError;
        if (!resolveHomeRelativePath(m_settings, rel, realDir, pathError)) {
            resp["status"] = "error";
            resp["error"] = pathError;
            p2p->sendText(QJsonDocument(resp).toJson(QJsonDocument::Compact));
            return;
        }

        QDir dir;
        if (!dir.mkpath(realDir)) {
            resp["status"] = "error";
            resp["error"] = "Failed to create folder: " + rel;
            p2p->sendText(QJsonDocument(resp).toJson(QJsonDocument::Compact));
            return;
        }

        resp["status"] = "ok";
        QJsonObject result;
        result["path"] = rel;
        result["realPath"] = realDir;
        result["name"] = QFileInfo(realDir).fileName();
        resp["result"] = result;
        p2p->sendText(QJsonDocument(resp).toJson(QJsonDocument::Compact));
        return;
    }

    // HomeWrite: upload a file onto the real filesystem (home browsing).
    if (cmd == "HomeWrite") {
        if (!isAdmin) {
            resp["status"] = "error";
            resp["error"] = "Access Denied: HomeWrite requires administrator privileges";
            p2p->sendText(QJsonDocument(resp).toJson(QJsonDocument::Compact));
            return;
        }

        QString rel = req.value("path").toString();
        if (rel.isEmpty() || rel.endsWith("/")) {
            resp["status"] = "error";
            resp["error"] = "Invalid file path";
            p2p->sendText(QJsonDocument(resp).toJson(QJsonDocument::Compact));
            return;
        }

        QString realPath;
        QString pathError;
        if (!resolveHomeRelativePath(m_settings, rel, realPath, pathError)) {
            resp["status"] = "error";
            resp["error"] = pathError;
            p2p->sendText(QJsonDocument(resp).toJson(QJsonDocument::Compact));
            return;
        }

        QFileInfo fi(realPath);
        QDir parentDir(fi.absolutePath());
        if (!parentDir.exists()) {
            if (!parentDir.mkpath(fi.absolutePath())) {
                resp["status"] = "error";
                resp["error"] = "Failed to create parent directories";
                p2p->sendText(QJsonDocument(resp).toJson(QJsonDocument::Compact));
                return;
            }
        }

        QByteArray data = QByteArray::fromBase64(req.value("data").toString().toLatin1());
        qint64 offset = req.value("offset").toVariant().toLongLong();
        bool truncate = req.value("truncate").toBool(offset == 0);
        bool eof = req.value("eof").toBool(true);

        QFile file(realPath);
        QIODevice::OpenMode mode = QIODevice::ReadWrite;
        if (truncate && offset == 0) {
            mode = QIODevice::WriteOnly | QIODevice::Truncate;
        }

        if (!file.open(mode)) {
            resp["status"] = "error";
            resp["error"] = "Failed to open file for writing";
            p2p->sendText(QJsonDocument(resp).toJson(QJsonDocument::Compact));
            return;
        }

        if (offset > 0) {
            file.seek(offset);
        }

        qint64 written = file.write(data);
        file.close();
        if (written != data.size()) {
            resp["status"] = "error";
            resp["error"] = "Failed to write file: " + rel;
            p2p->sendText(QJsonDocument(resp).toJson(QJsonDocument::Compact));
            return;
        }

        if (!eof) {
            LocalFileBackend::touchFileWriting(realPath, 30000);
        } else {
            LocalFileBackend::markFileWriting(realPath, false);
            if (req.contains("mtime")) {
                qint64 mtimeMs = req.value("mtime").toVariant().toLongLong();
                if (mtimeMs > 0) {
#ifdef Q_OS_UNIX
                    struct timeval tv[2];
                    tv[0].tv_sec = mtimeMs / 1000;
                    tv[0].tv_usec = (mtimeMs % 1000) * 1000;
                    tv[1].tv_sec = tv[0].tv_sec;
                    tv[1].tv_usec = tv[0].tv_usec;
                    ::utimes(realPath.toUtf8().constData(), tv);
#endif
                }
            }
        }

        resp["status"] = "ok";
        QJsonObject result;
        result["path"] = rel;
        result["realPath"] = realPath;
        result["name"] = fi.fileName();
        result["size"] = written;
        resp["result"] = result;
        p2p->sendText(QJsonDocument(resp).toJson(QJsonDocument::Compact));
        // RAG 3.0: file save / upload → index after final chunk is written
        if (m_aiSupported && eof)
            ragIndexPath(realPath);
        return;
    }

    // HomeCopy: copy a file/folder on the real filesystem (home browsing).
    if (cmd == "HomeCopy") {
        if (!isAdmin) {
            resp["status"] = "error";
            resp["error"] = "Access Denied: HomeCopy requires administrator privileges";
            p2p->sendText(QJsonDocument(resp).toJson(QJsonDocument::Compact));
            return;
        }

        QString srcRel = req.value("path").toString();
        QString tgtRel = req.value("target").toString();
        QString srcReal, tgtReal;
        QString err;

        if (!resolveHomeRelativePath(m_settings, srcRel, srcReal, err)) {
            resp["status"] = "error"; resp["error"] = err;
            p2p->sendText(QJsonDocument(resp).toJson(QJsonDocument::Compact));
            return;
        }
        QString tgtParent = tgtRel;
        QString srcName = QFileInfo(srcReal).fileName();
        if (tgtParent.isEmpty() || tgtParent == "/") {
            tgtReal = QDir::cleanPath(QFileInfo(srcReal).absolutePath() + "/../" + srcName);
        } else {
            QString tgtParentReal;
            if (!resolveHomeRelativePath(m_settings, tgtParent, tgtParentReal, err)) {
                resp["status"] = "error"; resp["error"] = err;
                p2p->sendText(QJsonDocument(resp).toJson(QJsonDocument::Compact));
                return;
            }
            tgtReal = tgtParentReal + "/" + srcName;
        }

        bool ok = false;
        QFileInfo srcInfo(srcReal);
        if (srcInfo.isDir()) {
            ok = copyRecursively(srcReal, tgtReal);
        } else {
            QFile::remove(tgtReal);
            ok = QFile::copy(srcReal, tgtReal);
        }

        if (ok) {
            resp["status"] = "ok";
        } else {
            resp["status"] = "error";
            resp["error"] = "HomeCopy failed: " + srcRel + " -> " + tgtRel;
        }
        p2p->sendText(QJsonDocument(resp).toJson(QJsonDocument::Compact));
        return;
    }

    // HomeMove: move a file/folder on the real filesystem (home browsing).
    if (cmd == "HomeMove") {
        if (!isAdmin) {
            resp["status"] = "error";
            resp["error"] = "Access Denied: HomeMove requires administrator privileges";
            p2p->sendText(QJsonDocument(resp).toJson(QJsonDocument::Compact));
            return;
        }

        QString srcRel = req.value("path").toString();
        QString tgtRel = req.value("target").toString();
        QString srcReal, tgtReal;
        QString err;

        if (!resolveHomeRelativePath(m_settings, srcRel, srcReal, err)) {
            resp["status"] = "error"; resp["error"] = err;
            p2p->sendText(QJsonDocument(resp).toJson(QJsonDocument::Compact));
            return;
        }
        QString tgtParent = tgtRel;
        QString srcName = QFileInfo(srcReal).fileName();
        if (tgtParent.isEmpty() || tgtParent == "/") {
            tgtReal = QDir::cleanPath(QFileInfo(srcReal).absolutePath() + "/../" + srcName);
        } else {
            QString tgtParentReal;
            if (!resolveHomeRelativePath(m_settings, tgtParent, tgtParentReal, err)) {
                resp["status"] = "error"; resp["error"] = err;
                p2p->sendText(QJsonDocument(resp).toJson(QJsonDocument::Compact));
                return;
            }
            tgtReal = tgtParentReal + "/" + srcName;
        }

        if (QFile::rename(srcReal, tgtReal)) {
            resp["status"] = "ok";
        } else {
            resp["status"] = "error";
            resp["error"] = "HomeMove failed: " + srcRel + " -> " + tgtRel;
        }
        p2p->sendText(QJsonDocument(resp).toJson(QJsonDocument::Compact));
        // RAG 3.0: moved → drop old path, index new one
        if (m_aiSupported && resp["status"] == "ok") {
            ragDeleteSource(srcReal);
            ragIndexPath(tgtReal);
        }
        return;
    }

    // HomeRename: rename a file/folder on the real filesystem (home browsing).
    if (cmd == "HomeRename") {
        if (!isAdmin) {
            resp["status"] = "error";
            resp["error"] = "Access Denied: HomeRename requires administrator privileges";
            p2p->sendText(QJsonDocument(resp).toJson(QJsonDocument::Compact));
            return;
        }

        QString rel = req.value("path").toString();
        QString newName = req.value("newName").toString();
        QString realPath;
        QString err;

        if (newName.isEmpty()) {
            resp["status"] = "error"; resp["error"] = "New name is empty";
            p2p->sendText(QJsonDocument(resp).toJson(QJsonDocument::Compact));
            return;
        }
        if (!resolveHomeRelativePath(m_settings, rel, realPath, err)) {
            resp["status"] = "error"; resp["error"] = err;
            p2p->sendText(QJsonDocument(resp).toJson(QJsonDocument::Compact));
            return;
        }

        QString newPath = QFileInfo(realPath).absolutePath() + "/" + newName;
        if (QFile::rename(realPath, newPath)) {
            resp["status"] = "ok";
        } else {
            resp["status"] = "error";
            resp["error"] = "HomeRename failed: " + rel + " -> " + newName;
        }
        p2p->sendText(QJsonDocument(resp).toJson(QJsonDocument::Compact));
        // RAG 3.0: renamed → drop old path, index new one
        if (m_aiSupported && resp["status"] == "ok") {
            ragDeleteSource(realPath);
            ragIndexPath(newPath);
        }
        return;
    }

    // HomeRemove: remove a file/folder on the real filesystem (home browsing).
    if (cmd == "HomeRemove") {
        if (!isAdmin) {
            resp["status"] = "error";
            resp["error"] = "Access Denied: HomeRemove requires administrator privileges";
            p2p->sendText(QJsonDocument(resp).toJson(QJsonDocument::Compact));
            return;
        }

        QString rel = req.value("path").toString();
        QString realPath;
        QString err;

        if (!resolveHomeRelativePath(m_settings, rel, realPath, err)) {
            resp["status"] = "error"; resp["error"] = err;
            p2p->sendText(QJsonDocument(resp).toJson(QJsonDocument::Compact));
            return;
        }

        bool ok = false;
        QFileInfo fi(realPath);
        if (fi.isDir()) {
            ok = QDir(realPath).removeRecursively();
        } else {
            ok = QFile::remove(realPath);
        }

        if (ok) {
            resp["status"] = "ok";
        } else {
            resp["status"] = "error";
            resp["error"] = "HomeRemove failed: " + rel;
        }
        p2p->sendText(QJsonDocument(resp).toJson(QJsonDocument::Compact));
        // RAG 3.0: deleted → remove from index
        if (m_aiSupported && ok)
            ragDeleteSource(realPath);
        return;
    }

    // GetSettings: return the full OnMyDiskSettings as JSON.
    if (cmd == "GetSettings") {
        if (!isAdmin) {
            resp["status"] = "error";
            resp["error"] = "Access Denied: GetSettings requires administrator privileges";
            p2p->sendText(QJsonDocument(resp).toJson(QJsonDocument::Compact));
            return;
        }
        QJsonObject settingsJson = static_cast<QJsonObject>(m_omdSettings);
        settingsJson["rcloneInstalled"] = !QStandardPaths::findExecutable("rclone").isEmpty();
        // Mask any stored rclone share passwords before sending over P2P
        if (settingsJson.contains("rcloneShares") && settingsJson.value("rcloneShares").isArray()) {
            QJsonArray sharesArr = settingsJson.value("rcloneShares").toArray();
            QJsonArray maskedArr;
            for (const QJsonValue &v : sharesArr) {
                QJsonObject obj = v.toObject();
                if (!obj.value("password").toString().isEmpty())
                    obj["password"] = QStringLiteral("******");
                maskedArr.append(obj);
            }
            settingsJson["rcloneShares"] = maskedArr;
        }
        // Discover configured remote names from rclone.conf (names only, no secrets)
        {
            QString rcloneConf;
#ifdef Q_OS_LINUX
            if (::geteuid() == 0 && m_settings) {
                QString sysUser = m_settings->value("FileNode/User", "").toString();
                if (!sysUser.isEmpty()) {
                    struct passwd *pw = getpwnam(sysUser.toUtf8().constData());
                    if (pw && pw->pw_dir)
                        rcloneConf = QString::fromUtf8(pw->pw_dir) + "/.config/rclone/rclone.conf";
                }
            }
#endif
#ifdef Q_OS_WIN
            if (rcloneConf.isEmpty()) {
                QString appData = qEnvironmentVariable("APPDATA");
                if (!appData.isEmpty())
                    rcloneConf = QDir::fromNativeSeparators(appData) + "/rclone/rclone.conf";
            }
#endif
            if (rcloneConf.isEmpty())
                rcloneConf = QDir::homePath() + "/.config/rclone/rclone.conf";
            QJsonArray remotesArr;
            if (QFileInfo::exists(rcloneConf)) {
                QSettings rc(rcloneConf, QSettings::IniFormat);
                for (const QString &grp : rc.childGroups())
                    remotesArr.append(grp);
            }
            settingsJson["rcloneRemotes"] = remotesArr;
        }
#ifdef EMBEDDED
        settingsJson["appVersion"] = OMDUpdater::instance().currentVersion();
        settingsJson["newVersion"] = OMDUpdater::instance().newVersion();
        settingsJson["updateAvailable"] = OMDUpdater::instance().isUpdateAvailable();
        settingsJson["updateStatus"] = static_cast<int>(OMDUpdater::instance().status());
        settingsJson["updateMessage"] = OMDUpdater::instance().statusMessage();
#else
#ifdef APP_VERSION
        settingsJson["appVersion"] = QStringLiteral(APP_VERSION);
#endif
#endif
        resp["status"] = "ok";
        resp["result"] = QJsonObject{{"settings", settingsJson}};
        p2p->sendText(QJsonDocument(resp).toJson(QJsonDocument::Compact));
        return;
    }

    // CheckUpdate: trigger manual OTA update check
    if (cmd == "CheckUpdate") {
        if (!isAdmin) {
            resp["status"] = "error";
            resp["error"] = "Access Denied: CheckUpdate requires administrator privileges";
            p2p->sendText(QJsonDocument(resp).toJson(QJsonDocument::Compact));
            return;
        }
#ifdef EMBEDDED
        OMDUpdater::instance().checkForUpdates();
        resp["status"] = "ok";
        resp["result"] = QJsonObject{
            {"currentVersion", OMDUpdater::instance().currentVersion()},
            {"updateAvailable", OMDUpdater::instance().isUpdateAvailable()},
            {"newVersion", OMDUpdater::instance().newVersion()},
            {"updateStatus", static_cast<int>(OMDUpdater::instance().status())},
            {"updateMessage", OMDUpdater::instance().statusMessage()}
        };
#else
        resp["status"] = "error";
        resp["error"] = "OTA updates not supported on this platform";
#endif
        p2p->sendText(QJsonDocument(resp).toJson(QJsonDocument::Compact));
        return;
    }

    // ApplyUpdate: download and install OTA update
    if (cmd == "ApplyUpdate") {
        if (!isAdmin) {
            resp["status"] = "error";
            resp["error"] = "Access Denied: ApplyUpdate requires administrator privileges";
            p2p->sendText(QJsonDocument(resp).toJson(QJsonDocument::Compact));
            return;
        }
#ifdef EMBEDDED
        OMDUpdater::instance().downloadAndApplyUpdate();
        resp["status"] = "ok";
        resp["result"] = QJsonObject{
            {"message", "Update initiated. Device will restart once installation completes."}
        };
#else
        resp["status"] = "error";
        resp["error"] = "OTA updates not supported on this platform";
#endif
        p2p->sendText(QJsonDocument(resp).toJson(QJsonDocument::Compact));
        return;
    }

    // SetSettings: apply a settings JSON object to this agent.
    if (cmd == "SetSettings") {
        if (!isAdmin) {
            resp["status"] = "error";
            resp["error"] = "Access Denied: SetSettings requires administrator privileges";
            p2p->sendText(QJsonDocument(resp).toJson(QJsonDocument::Compact));
            return;
        }
        QJsonObject settingsJson = req.value("settings").toObject();
        OnMyDiskSettings newSettings(settingsJson);
        resp["status"] = "ok";
        p2p->sendText(QJsonDocument(resp).toJson(QJsonDocument::Compact));
        emit settingsChanged(newSettings);
        return;
    }

    // Block unauthenticated peers from accessing files
    if (session.authLevel == P2PAuthLevel::Unauthenticated) {
        resp["status"] = "error";
        resp["error"] = "Access Denied: Authentication required";
        p2p->sendText(QJsonDocument(resp).toJson(QJsonDocument::Compact));
        return;
    }

    QString effectivePath = pathStr;
    if (isScoped) {
        if (effectivePath.contains("..")) {
            resp["status"] = "error";
            resp["error"] = "Access Denied: Invalid path";
            p2p->sendText(QJsonDocument(resp).toJson(QJsonDocument::Compact));
            return;
        }

        if (effectivePath.isEmpty() || effectivePath == "/") {
            effectivePath = session.scopePath;
        } else if (effectivePath.startsWith("/")) {
            if (!effectivePath.startsWith(session.scopePath)) {
                effectivePath = session.scopePath + effectivePath;
            }
        } else {
            effectivePath = session.scopePath + "/" + effectivePath;
        }

        if ((cmd == "Write" || cmd == "HomeWrite" || cmd == "MkDir" || cmd == "HomeMkDir" || cmd == "Copy" || cmd == "Move" || cmd == "Rename" || cmd == "Remove") && !session.writable) {
            resp["status"] = "error";
            resp["error"] = "Access Denied: Share is read-only";
            p2p->sendText(QJsonDocument(resp).toJson(QJsonDocument::Compact));
            return;
        }
    }

    QStringList pathList = effectivePath.split('/', Qt::SkipEmptyParts);

    CloudResource *res = m_hostRoot ? m_hostRoot->getChildClone(pathList, queryStr, true) : nullptr;
    bool allowNonExistent = (cmd == "Write" || cmd == "HomeWrite" || cmd == "MkDir" || cmd == "HomeMkDir");
    if (!res || (!res->exists() && !allowNonExistent)) {
        resp["status"] = "error";
        resp["error"] = "Path not found: " + pathStr;
        if (res && res->isCopy()) delete res;
        p2p->sendText(QJsonDocument(resp).toJson(QJsonDocument::Compact));
        return;
    }

    if (cmd == "FileInfo") {
        QJsonObject result;
        result["name"] = (pathList.isEmpty() && m_hostRoot) ? m_hostRoot->hostName() : res->fileName();
        result["path"] = pathStr;
        result["isDir"] = res->isDir();
        result["exists"] = res->exists();
        result["size"] = res->size();
        result["lastModified"] = res->lastModified().toString(Qt::ISODate);
        result["mimeType"] = res->getContentType();

        QHash<QString, QVariant> resAttrs = res->getCustomAttributes();
        QJsonObject resAttrsObj;
        for(auto it = resAttrs.cbegin(); it != resAttrs.cend(); ++it) {
            resAttrsObj.insert(it.key(), QJsonValue::fromVariant(it.value()));
        }
        result["customAttributes"] = resAttrsObj;
        result["hasCover"] = resAttrsObj.contains("cover") && resAttrsObj["cover"].toBool();

        if (res->isDir()) {
            QJsonArray files;
            QStringList entries = res->entryList();
            for (const QString &entryName : entries) {
                CloudResource *child = res->getChild(QStringList() << entryName, false);
                bool isCopy = false;
                if (!child) {
                    QStringList childPath = pathList;
                    childPath.append(entryName);
                    child = m_hostRoot->getChildClone(childPath, "", false);
                    isCopy = child && child->isCopy();
                }
                if (child) {
                    QJsonObject item;
                    item["name"] = child->fileName();
                    item["isDir"] = child->isDir();
                    item["size"] = child->size();
                    item["lastModified"] = child->lastModified().toString(Qt::ISODate);
                    item["mimeType"] = child->getContentType();
                    
                    QHash<QString, QVariant> childAttrs = child->getCustomAttributes();
                    QJsonObject childAttrsObj;
                    for(auto it = childAttrs.cbegin(); it != childAttrs.cend(); ++it) {
                        const QString &key = it.key();
                        if (key == "coverData" || key == "thumbnail" || key == "preview" || key == "exif")
                            continue;
                        childAttrsObj.insert(key, QJsonValue::fromVariant(it.value()));
                    }
                    item["customAttributes"] = childAttrsObj;
                    item["hasCover"] = childAttrs.contains("cover") && childAttrs["cover"].toBool();
                    
                    files.append(item);
                    if (isCopy) delete child;
                }
            }

            if (pathList.isEmpty() && !isScoped) {
                bool terminalSupported = m_settings ? m_settings->value("FileNode/terminal", true).toBool() : true;
                if (terminalSupported) {
                    QJsonObject terminalItem;
                    terminalItem["name"] = "Terminal";
                    terminalItem["isDir"] = false;
                    terminalItem["size"] = 0;
                    terminalItem["lastModified"] = QDateTime::currentDateTime().toString(Qt::ISODate);
                    terminalItem["mimeType"] = "application/x-terminal";
                    QJsonObject terminalAttrs;
                    terminalAttrs["uiAction"] = "terminal";
                    terminalAttrs["port"] = m_settings ? m_settings->value("FileNode/terminalPort", 22).toInt() : 22; // SSH default port
                    terminalItem["customAttributes"] = terminalAttrs;
                    files.append(terminalItem);
                }

                if (m_aiSupported) {
                    QJsonObject aiItem;
                    aiItem["name"] = "Chats";
                    aiItem["isDir"] = false;
                    aiItem["size"] = 0;
                    aiItem["lastModified"] = QDateTime::currentDateTime().toString(Qt::ISODate);
                    aiItem["mimeType"] = "application/x-chat";
                    QJsonObject aiAttrs;
                    aiAttrs["uiAction"] = "chat";
                    aiAttrs["port"] = 4000;
                    aiItem["customAttributes"] = aiAttrs;
                    files.append(aiItem);
                }

                // Zero-config / user-configured local web apps ([Apps] in onmydisk.conf)
                if (m_settings) {
                    m_settings->beginGroup(QStringLiteral("Apps"));
                    const QStringList appSlugs = m_settings->childKeys();
                    for (const QString &rawSlug : appSlugs) {
                        const QString slug = rawSlug.trimmed().toLower();
                        const QString rawVal = m_settings->value(rawSlug).toString().trimmed();
                        if (slug.isEmpty() || rawVal.isEmpty()) continue;
                        const QStringList parts = rawVal.split(QLatin1Char('|'));
                        QString appName = slug.left(1).toUpper() + slug.mid(1);
                        int appPort = 0;
                        QString entryPath = QStringLiteral("/");
                        QString appIcon = slug;
                        if (parts.size() == 1) {
                            appPort = parts[0].trimmed().toInt();
                        } else if (parts.size() >= 2) {
                            if (!parts[0].trimmed().isEmpty()) appName = parts[0].trimmed();
                            appPort = parts[1].trimmed().toInt();
                            if (parts.size() >= 3 && !parts[2].trimmed().isEmpty()) entryPath = parts[2].trimmed();
                            if (parts.size() >= 4 && !parts[3].trimmed().isEmpty()) appIcon = parts[3].trimmed();
                        }
                        if (appPort <= 0) continue;
                        if (!entryPath.startsWith(QLatin1Char('/'))) entryPath.prepend(QLatin1Char('/'));

                        QJsonObject appItem;
                        appItem["name"] = appName;
                        appItem["isDir"] = false;
                        appItem["size"] = 0;
                        appItem["lastModified"] = QDateTime::currentDateTime().toString(Qt::ISODate);
                        appItem["mimeType"] = "application/x-web-app";
                        QJsonObject appAttrs;
                        appAttrs["uiAction"] = "web-app";
                        appAttrs["appSlug"] = slug;
                        appAttrs["port"] = appPort;
                        appAttrs["entryPath"] = entryPath;
                        appAttrs["icon"] = appIcon;
                        appItem["customAttributes"] = appAttrs;
                        files.append(appItem);
                    }
                    m_settings->endGroup();
                }
            }

            result["files"] = files;
        }

        resp["status"] = "ok";
        resp["result"] = result;
    }
    else if (cmd == "Open") {
        if (!queryStr.isEmpty() && (queryStr.contains("resize") || queryStr.contains("cover") || queryStr.contains("thumb") || queryStr.contains("getIcon"))) {
            QString localPath = m_hostRoot ? m_hostRoot->getLocalPath(pathList) : QString();
            if (localPath.isEmpty()) {
                LocalResource* localRes = dynamic_cast<LocalResource*>(res);
                if (localRes) localPath = localRes->getLocalPath();
            }
            if (!localPath.isEmpty() && LocalFileBackend::isFileWriting(localPath)) {
                resp["status"] = "error";
                resp["error"] = "File is currently being written";
                if (res && res->isCopy()) delete res;
                p2p->sendText(QJsonDocument(resp).toJson(QJsonDocument::Compact));
                return;
            }
        }
        resp["status"] = "ok";
        QJsonObject result;
        result["name"] = res->fileName();
        result["path"] = pathStr;
        result["isDir"] = res->isDir();
        result["size"] = res->size();
        result["mimeType"] = res->getContentType();
        resp["result"] = result;
    }
    else if (cmd == "Read") {
        if (!queryStr.isEmpty() && (queryStr.contains("resize") || queryStr.contains("cover") || queryStr.contains("thumb") || queryStr.contains("getIcon"))) {
            QString localPath = m_hostRoot ? m_hostRoot->getLocalPath(pathList) : QString();
            if (localPath.isEmpty()) {
                LocalResource* localRes = dynamic_cast<LocalResource*>(res);
                if (localRes) localPath = localRes->getLocalPath();
            }
            if (!localPath.isEmpty() && LocalFileBackend::isFileWriting(localPath)) {
                resp["status"] = "error";
                resp["error"] = "File is currently being written";
                if (res && res->isCopy()) delete res;
                p2p->sendText(QJsonDocument(resp).toJson(QJsonDocument::Compact));
                return;
            }
        }
        qint64 offset = req.value("offset").toVariant().toLongLong();
        qint64 size = req.value("size").toVariant().toLongLong();
        if (size <= 0) size = 262144;

        if (res->openResource(QIODevice::ReadOnly) != FILEHANDLE_ERROR) {
            QByteArray chunk = res->read(offset, size);
            res->close();
            resp["status"] = "ok";
            resp["offset"] = offset;
            resp["size"] = chunk.size();
            resp["data"] = QString::fromLatin1(chunk.toBase64());
        } else {
            resp["status"] = "error";
            resp["error"] = "Failed to open file for reading";
        }
    }
    else if (cmd == "MkDir" || cmd == "HomeMkDir") {
        if (res->mkcol()) {
            resp["status"] = "ok";
        } else {
            resp["status"] = "error";
            resp["error"] = "Mkdir failed: " + pathStr;
        }
    }
    else if (cmd == "Write" || cmd == "HomeWrite") {
        qint64 offset = req.value("offset").toVariant().toLongLong();
        QByteArray chunk = QByteArray::fromBase64(req.value("data").toString().toLatin1());
        bool truncate = req.value("truncate").toBool(false);
        bool eof = req.value("eof").toBool(true);

        QIODevice::OpenMode mode = QIODevice::ReadWrite;
        if (truncate && offset == 0) mode |= QIODevice::Truncate;

        RemoteResource *remRes = dynamic_cast<RemoteResource*>(res);
        WebDAVBackend *webdavBackend = remRes ? dynamic_cast<WebDAVBackend*>(remRes->backend()) : nullptr;
        if (webdavBackend) {
            webdavBackend->setWriteEof(eof);
        }

        if (res->openResource(mode) != FILEHANDLE_ERROR) {
            res->seek(offset);
            qint64 written = res->write(chunk);
            res->close();
            if (webdavBackend && eof && !webdavBackend->lastWriteOk()) {
                resp["status"] = "error";
                resp["error"] = "Failed to upload file to WebDAV share";
            } else {
                resp["status"] = "ok";
                resp["written"] = written;
            }

            QString localPath = m_hostRoot ? m_hostRoot->getLocalPath(pathList) : QString();
            if (localPath.isEmpty()) {
                LocalResource* localRes = dynamic_cast<LocalResource*>(res);
                if (localRes) localPath = localRes->getLocalPath();
            }
            if (!localPath.isEmpty()) {
                if (!eof) {
                    LocalFileBackend::touchFileWriting(localPath, 30000);
                } else {
                    LocalFileBackend::markFileWriting(localPath, false);
                    if (req.contains("mtime")) {
                        qint64 mtimeMs = req.value("mtime").toVariant().toLongLong();
                        if (mtimeMs > 0) {
#ifdef Q_OS_UNIX
                            struct timeval tv[2];
                            tv[0].tv_sec = mtimeMs / 1000;
                            tv[0].tv_usec = (mtimeMs % 1000) * 1000;
                            tv[1].tv_sec = tv[0].tv_sec;
                            tv[1].tv_usec = tv[0].tv_usec;
                            ::utimes(localPath.toUtf8().constData(), tv);
#endif
                        }
                    }
                }
            }
            // RAG 3.0: file upload to a share → index (owner device) after final chunk is written
            if (m_aiSupported && eof) {
                QString ragPath = ragLocalPathFor(effectivePath);
                if (!ragPath.isEmpty())
                    ragIndexPath(ragPath);
            }
        } else {
            resp["status"] = "error";
            resp["error"] = "Failed to open file for writing";
        }
    }
    else if (cmd == "Close") {
        res->close();
        resp["status"] = "ok";
    }
    else if (cmd == "Copy") {
        QString targetPath = req.value("target").toString();
        QStringList targetList = targetPath.split('/', Qt::SkipEmptyParts);
        if (targetList.isEmpty() || targetList.last() != res->fileName())
            targetList.append(res->fileName());
        QString destResourcePath = m_hostRoot->getLocalPath(targetList);
        if (destResourcePath.isEmpty())
            destResourcePath = QStringLiteral("/") + targetList.join(QStringLiteral("/"));

        if (res->copy(destResourcePath)) {
            resp["status"] = "ok";
        } else {
            resp["status"] = "error";
            resp["error"] = "Copy failed: " + pathStr + " -> " + targetPath;
        }
    }
    else if (cmd == "Move") {
        QString targetPath = req.value("target").toString();
        QStringList targetList = targetPath.split('/', Qt::SkipEmptyParts);
        if (targetList.isEmpty() || targetList.last() != res->fileName())
            targetList.append(res->fileName());
        QString destResourcePath = m_hostRoot->getLocalPath(targetList);
        if (destResourcePath.isEmpty())
            destResourcePath = QStringLiteral("/") + targetList.join(QStringLiteral("/"));

        if (res->move(destResourcePath)) {
            resp["status"] = "ok";
        } else {
            resp["status"] = "error";
            resp["error"] = "Move failed: " + pathStr + " -> " + targetPath;
        }
    }
    else if (cmd == "Rename") {
        QString newName = req.value("newName").toString();
        if (newName.isEmpty()) {
            resp["status"] = "error";
            resp["error"] = "New name is empty";
        } else if (res->rename(newName)) {
            resp["status"] = "ok";
        } else {
            resp["status"] = "error";
            resp["error"] = "Rename failed: " + pathStr + " -> " + newName;
        }
    }
    else if (cmd == "Remove") {
        bool forced = req.value("forced").toBool(false);
        if (res->remove(forced)) {
            resp["status"] = "ok";
            // RAG 3.0: file removed from a share → drop from index
            if (m_aiSupported) {
                QString localPath = ragLocalPathFor(effectivePath);
                if (!localPath.isEmpty())
                    ragDeleteSource(localPath);
            }
        } else {
            resp["status"] = "error";
            resp["error"] = "Remove failed: " + pathStr;
        }
    }
    else {
        resp["status"] = "error";
        resp["error"] = "Unknown command: " + cmd;
    }

    if (res && res->isCopy()) {
        delete res;
    }

    p2p->sendText(QJsonDocument(resp).toJson(QJsonDocument::Compact));
#else
    Q_UNUSED(p2p)
    Q_UNUSED(req)
#endif
}

void OnMyDiskLinkClient::sendTunnelCloseFor(const QString &ID)
{
    if (!serviceSocket)
        return;
    LinkMessage close;
    close.setMessageType(LinkMessage::TunnelClose);
    close.setResultCode(LinkMessage::OK);
    close.insertDataValue("ID", ID);
    close.insertDataValue("kind", "tunnel-closed");
    serviceSocket->writeMessage(close);
}

void OnMyDiskLinkClient::cleanupP2PTunnel(const QString &ID)
{
    m_tunnelCallerUser.remove(ID);
    m_tunnelCallerRole.remove(ID);
    m_tunnelTargetOwner.remove(ID);
    m_tunnelHttpStream.remove(ID);
#ifdef P2P_ENABLED
    P2PTunnel *p2p = nullptr;
    bool finalizeNow = false;
    {
        QMutexLocker locker(&m_p2pMutex);
        p2p = m_p2pTunnels.take(ID);
        if (p2p)
        {
            m_p2pSessions.remove(p2p);
            m_p2pDying.insert(p2p);
            finalizeNow = (m_p2pBusy.value(p2p, 0) == 0);
        }
    }
    if (finalizeNow)
        finalizeP2PTunnel(p2p);
#else
    Q_UNUSED(ID)
#endif
}

bool OnMyDiskLinkClient::p2pTaskStarted(P2PTunnel *p2p)
{
#ifdef P2P_ENABLED
    QMutexLocker locker(&m_p2pMutex);
    if (!p2p || m_p2pDying.contains(p2p))
    {
        qWarning() << "P2P FS: dropped incoming chunk (tunnel teardown in progress), tunnel:" << p2p;
        return false;
    }
    m_p2pBusy[p2p] = m_p2pBusy.value(p2p, 0) + 1;
    return true;
#else
    Q_UNUSED(p2p)
    return false;
#endif
}

void OnMyDiskLinkClient::p2pTaskFinished(P2PTunnel *p2p)
{
#ifdef P2P_ENABLED
    bool finalize = false;
    {
        QMutexLocker locker(&m_p2pMutex);
        const int n = m_p2pBusy.value(p2p, 0) - 1;
        if (n > 0) {
            m_p2pBusy[p2p] = n;
        } else {
            m_p2pBusy.remove(p2p);
            finalize = m_p2pDying.remove(p2p);
        }
    }
    if (finalize)
        finalizeP2PTunnel(p2p);
#else
    Q_UNUSED(p2p)
#endif
}

void OnMyDiskLinkClient::finalizeP2PTunnel(P2PTunnel *p2p)
{
#ifdef P2P_ENABLED
    if (!p2p)
        return;
    {
        // Remove the pointer from m_p2pDying BEFORE the object is deleted:
        // otherwise a new P2PTunnel allocated at the same address is
        // permanently treated as dying and all its chunks get silently
        // dropped ("silent" tunnels where no RPC ever arrives).
        QMutexLocker locker(&m_p2pMutex);
        m_p2pDying.remove(p2p);
    }
    p2p->close();
    p2p->deleteLater();
#else
    Q_UNUSED(p2p)
#endif
}


void OnMyDiskLinkClient::handleTunnelCloseRequest(const LinkMessage &message)
{
    QString ID = message.getData().value("ID").toString();
    if (ID.isEmpty() || !m_tunnels.contains(ID))
        return;

    m_tunnelE2EEPub.remove(ID);
    m_tunnelKeys.remove(ID);
    cleanupP2PTunnel(ID);
    QTcpSocket* tcpSocket = m_tunnels.take(ID);
    tcpSocket->disconnectFromHost();
    tcpSocket->deleteLater();
}


bool OnMyDiskLinkClient::requestNewCert(QString ownerId)
{
    m_linkId = "";

    if (m_linkStatus == Authorizing) {
        qWarning() << "Already authorizing...";
        return false;
    }
	m_linkStatus = Authorizing;
    if (ownerId == "") {
        loadDeviceId();
		m_user = m_deviceId;
	} else
        m_user = ownerId;


    QFile caFile(m_CAPath);
    if (! caFile.open(QIODevice::ReadOnly))
    {
        qDebug() << "Requesting server CA...";
        emit privateKeyStatus(tr("Requesting server CA..."));
        bool caOk = fetchCA();
        if (! caOk)
        {
            emit privateKeyError(tr("Can't get server CA"));
            return false;
        }
    } else
        caFile.close();

    qDebug() << "Requesting new certificate for" << ownerId << m_deviceId;

    qDebug() << "Generating private key...";
    emit privateKeyStatus(tr("Generating private key..."));
    genPrivateKey();


    qDebug() << "Requesting certificate...";
    QByteArray csrBody = genCertificateSignRequest(m_user);
    if (csrBody.count() == 0)
    {
        qDebug() << "Can't generate CSR";
        emit privateKeyError(tr("Can't generate certificate sign request"));
        return false;
    }

    emit privateKeyStatus(tr("Requesting certificate..."));

    bool certOk = fetchCert(csrBody);
    if (! certOk)
    {
        emit privateKeyError(tr("Certificate request error"));
        return false;
    }

    m_linkStatus = Connecting;
    emit accountCreated(m_omdSettings.server(), m_user);
    return true;
}

bool OnMyDiskLinkClient::setupSSL()
{
   QFile keyFile(m_KeyPath);
   if (keyFile.open(QIODevice::ReadOnly))
   {
            qDebug() << "Loading private key:" << m_KeyPath;
            emit privateKeyStatus(tr("Loading private key..."));

            QSslKey privateKey(&keyFile, QSsl::Rsa, QSsl::Pem, QSsl::PrivateKey, m_deviceId.toLatin1());

            keyFile.close();

            if (privateKey.isNull())
            {
                emit privateKeyError(tr("Private key access error"));
                qDebug() << "Error loading private key. Device key:" << m_deviceId;
                return false;
            }
            sslConfiguration.setPrivateKey(privateKey);
	} else {
	   qWarning() << "Private key not found" << m_KeyPath;
       if (m_keyLogin)
           requestNewCert(m_deviceId);
       return false;
   }

    QFile caFile(m_CAPath);
    if (caFile.open(QIODevice::ReadOnly))
    {
            QList<QSslCertificate> CaCerts = QSslCertificate::fromDevice(&caFile);
            caFile.close();
            if (verifyCert(CaCerts.at(0)))
            {
                sslConfiguration.setCaCertificates(CaCerts);
            } else
            {
                emit privateKeyError(tr("Error loading server CA"));
                return false;
            }
	 } else {
		 qWarning() << "CA not found" << m_CAPath;
         if (m_keyLogin)
             requestNewCert(m_deviceId);
         return false;
	}


     QFile certFile(m_CertPath);
     if (certFile.open(QIODevice::ReadOnly))
     {
            QList<QSslCertificate> certs = QSslCertificate::fromDevice(&certFile);
            certFile.close();

            if (certs.count() != 0 && verifyCert(certs.at(0)))
            {
                sslConfiguration.setLocalCertificate(certs.at(0));
            } else
            {
                emit privateKeyError(tr("Error loading device certificate"));
                return false;
            }

	 } else
	 {
		  qWarning() << "Certificate not found" << m_CertPath;
          if (m_keyLogin)
              requestNewCert(m_deviceId);
          return false;
	 }

     sslConfiguration.setPeerVerifyMode(QSslSocket::VerifyNone);
     sslConfiguration.setProtocol(QSsl::AnyProtocol);
     //qDebug() << sslConfiguration.localCertificate().subjectInfoAttributes()[1];
     //qDebug() << sslConfiguration.localCertificate().subjectInfo(sslConfiguration.localCertificate().subjectInfoAttributes()[1]);

     if (sslConfiguration.localCertificate().subjectInfo(QSslCertificate::CommonName).count() > 0)
        m_user = sslConfiguration.localCertificate().subjectInfo(QSslCertificate::CommonName).at(0);
     else
        m_user = m_deviceId;

    //hidden root
    if (isService())
        m_hostRoot->addLocalResource("root", QString("%1/%2").arg(HOST_USERS_ROOT , m_user)); //all users home + username
    else
        m_hostRoot->addLocalResource("root", QStandardPaths::writableLocation(QStandardPaths::HomeLocation));


     QString effectiveDate = sslConfiguration.localCertificate().effectiveDate().date().toString();
     QString expiryDate = sslConfiguration.localCertificate().expiryDate().date().toString();
     if (m_linkId.isEmpty())
        m_linkId = QString(QCryptographicHash::hash(sslConfiguration.localCertificate().toPem(),QCryptographicHash::Md5).toHex());

     emit sslOK(m_user, m_deviceId, m_linkId, effectiveDate, expiryDate);

     emit privateKeyStatus(tr("Connecting..."));

     return true;
}

bool OnMyDiskLinkClient::verifyCert(QSslCertificate cert)
{
    //TODO: write signature check, dates check and CRL check
    return !cert.isNull();
}


bool OnMyDiskLinkClient::fetchCA()
{
    QUrl requestUrl(m_restApiUrl);
    requestUrl.setPath(requestUrl.path() + "/getca");
	//qWarning() << "URL" << requestUrl;

    QNetworkRequest request(requestUrl);
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    request.setTransferTimeout(15000);
#endif

    request.setRawHeader("authorization", QString("device:" + m_deviceId).toUtf8());

    this->post(request, QByteArray(""));
    QTimer::singleShot(20000, &m_loop, &QEventLoop::quit);
    m_loop.exec();
    return m_responseResult;
}



bool OnMyDiskLinkClient::fetchCert(QByteArray CSR)
{
    m_responseResult = false;

    QUrl requestUrl(m_restApiUrl);
    requestUrl.setPath(requestUrl.path() + "/signcsr");
	//qWarning() << "URL" << requestUrl;


    QByteArray postData,boundary="1BEF0A57BE110FD467A";

    postData.append("--"+boundary+"\r\n");
    postData.append("Content-Disposition: form-data; name=\"csr\"");
    postData.append("\r\n\r\n");
    postData.append(CSR);
    postData.append("\r\n");

    postData.append("--"+boundary+"\r\n");
    postData.append("Content-Disposition: form-data; name=\"user\"");
    postData.append("\r\n\r\n");
    postData.append(m_user.toUtf8());
    postData.append("\r\n");
    postData.append("--"+boundary+"--\r\n");

    QNetworkRequest request(requestUrl);
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    request.setTransferTimeout(15000);
#endif

    request.setRawHeader("authorization", QString("device:" + m_deviceId).toUtf8());
    request.setRawHeader("content-type", "application/x-www-form-urlencoded");

    request.setHeader(QNetworkRequest::ContentTypeHeader,
            "multipart/form-data; boundary="+boundary);
    request.setHeader(QNetworkRequest::ContentLengthHeader,
            QByteArray::number(postData.length()));

    this->post(request, postData);
    QTimer::singleShot(20000, &m_loop, &QEventLoop::quit);
    m_loop.exec();

    return m_responseResult;
}

bool OnMyDiskLinkClient::genPrivateKey()
{
    return m_SslHelper.genPrivateKeyToFile(2048, m_KeyPath, m_deviceId.toLocal8Bit().constData());
}

QByteArray OnMyDiskLinkClient::genCertificateSignRequest(QString ownerId)
{   
   bool isOk;
   QByteArray csr = m_SslHelper.genCSRToBytes(isOk, m_KeyPath, m_deviceId.toLocal8Bit().constData(), ownerId);
   if (isOk)
       return csr;
   else
       return  QByteArray("");
}

void OnMyDiskLinkClient::removeClientKeys()
{

    QFile caFile(m_CAPath);
    if (caFile.exists())
        caFile.remove();

    QFile certFile(m_CertPath);
    if (certFile.exists())
        certFile.remove();

    QFile keyFile(m_KeyPath);
    if (keyFile.exists())
        keyFile.remove();
}


void OnMyDiskLinkClient::messageRecieved(LinkSocket* serverSocket, const LinkMessage &message)
{
    //TimeLogger logger("Handling request " + message->messageTypeString());
    //qInfo() << "LinkClient got message:" << message->toString();
    try
    {
        switch  (message.getMessageType()) {
        case LinkMessage::UpdateShare:
            break;
        case LinkMessage::GetPublicLink:
            break;
        case LinkMessage::RemovePublicLink:
            break;
        case LinkMessage::Ping:
        {
            LinkMessage reply;
            reply.setMessageType(LinkMessage::PingReply);
            reply.setResultCode(LinkMessage::OK);
            serverSocket->writeMessage(reply);
            break;
        }
        case LinkMessage::RPC:
            handleRPCRequest(message);
            break;
        case LinkMessage::TunnelOpen:
            handleTunnelOpenRequest(message);
            break;
        case LinkMessage::TunnelData:
            handleTunnelDataRequest(message);
            break;
        case LinkMessage::TunnelClose:
            handleTunnelCloseRequest(message);
            break;
        case LinkMessage::TunnelSignaling:
            handleTunnelSignalingRequest(message);
            break;
        case LinkMessage::MkDir:
        case LinkMessage::Remove:
        case LinkMessage::FileInfo:
        case LinkMessage::FileOpen:
        case LinkMessage::FileClose:
        case LinkMessage::FilePos:
        case LinkMessage::FileSeek:
        case LinkMessage::FileRead:
        case LinkMessage::FileWrite:
        case LinkMessage::ManageStorage:
        case LinkMessage::FuseMounted:
        case LinkMessage::Rename:
        case LinkMessage::Copy:
        case LinkMessage::Move:
        case LinkMessage::CollectionChanged:
        case LinkMessage::InitLink:
        case LinkMessage::SwitchPort:
        #if !defined(EMBEDDED) || defined(ROUTER)
        case LinkMessage::AddShare:
        case LinkMessage::RemoveShare:
        #endif
            qWarning() << "OnMyDiskLinkClient: you sent it to wrong guy: " << message.getMessageType() << " from " << message.getSenderID();
            break;
        #if defined(EMBEDDED) && !defined(ROUTER)
        case LinkMessage::AddShare: {
                QString sharePath = message.getData()["sharePath"].toString();
                emit UserEvents::instance().addShareRequested(m_user, sharePath);
                break;
            }
        case LinkMessage::RemoveShare: {
                QString sharePath = message.getData()["sharePath"].toString();
                emit UserEvents::instance().removeShareRequested(m_user, sharePath);
                break;
            }
        #endif
        case LinkMessage::PingReply:
        {
            //qInfo() << "Ping reply recieved.";
            m_pingResponseTimer.stop();
            m_pingMisses = 0;
        }
            break;
        case LinkMessage::Null:
            break;
        case LinkMessage::ConnectDataSockets:
            handleConnectDataSockets(message);
            break;
        case LinkMessage::BadCertificate:
            qDebug() << "Certificate declined by server";
            emit privateKeyError(tr("Certificate declined by server"));
            return;
        }
    }
    catch(...)
    {
        qCritical() << "Critical error during handling message with type " << message.getMessageType() << " from " << message.getSenderID();
    }

}

void OnMyDiskLinkClient::onSslError(const QList<QSslError> &errors)
{
    Q_FOREACH(QSslError error, errors)
	{
		qWarning() << "SSL notice:" << error.errorString();
	}
    LinkSocket* socket = static_cast<LinkSocket*>( sender());
    if (m_treatSslErrorsAsWarnings)
    {
        socket->ignoreSslErrors();
        emit sslWarning(errors);
    }
    else
    {
        bool ignore = false;
        emit sslError(errors, ignore);
        if (ignore)
            socket->ignoreSslErrors();
    }
}

void OnMyDiskLinkClient::onSslErrors(QNetworkReply *reply, const QList<QSslError> &errors)
{
	Q_FOREACH(QSslError error, errors)
	{
		qWarning() << "SSL notice:" << error.errorString();
	}

    if (m_treatSslErrorsAsWarnings)
    {
		qDebug() << "Ingoring SSL error";
        reply->ignoreSslErrors();
        emit sslWarning(errors);
    }
    else
    {
        bool ignore = false;
        emit sslError(errors, ignore);
        if (ignore)
            reply->ignoreSslErrors();
    }
}

void OnMyDiskLinkClient::onConnectionError(QAbstractSocket::SocketError socketError)
{
    QSslSocket * socket = static_cast<QSslSocket *>(sender());
    const QSslCertificate localCert = sslConfiguration.localCertificate();
    const bool hasValidCert = !localCert.isNull() &&
        (!localCert.expiryDate().isValid() || QDateTime::currentDateTime() < localCert.expiryDate());

    switch (socketError) {
    case QAbstractSocket::SslHandshakeFailedError:
        if (!hasValidCert)
            emit privateKeyError(tr("Login is required"));
        qWarning() << "SSL handshake failed (valid local cert:" << hasValidCert << "):" << (socket ? socket->errorString() : QString());
        break;
    case QAbstractSocket::ProxyConnectionRefusedError:
        qWarning() << "Proxy connection refused:" << (socket ? socket->errorString() : QString());
        break;
    case QAbstractSocket::SslInternalError:
        if (!hasValidCert)
            emit privateKeyError(tr("Login is required"));
        qCritical() << "SslInternalError (valid local cert:" << hasValidCert << ")";
        break;
    case QAbstractSocket::SslInvalidUserDataError:
         qCritical() << "SslInvalidUserDataError";
        emit privateKeyError(tr("Login is required"));
        break;
    default:
        if (socket)
            qCritical() << qPrintable(socket->errorString());
        break;
    }

   emit connectionError(socketError);
}

void OnMyDiskLinkClient::requestFinished(QNetworkReply * reply)
{

    m_responseResult = false;

    QNetworkReply::NetworkError error =  reply->error();
    switch (error) {
    case QNetworkReply::NoError:
    {

        if (reply->hasRawHeader("cafile"))
        {
            qDebug() << "CA received";
            QByteArray caBody = reply->readAll();
            QFile caFile(m_CAPath);
            if (caFile.open(QIODevice::WriteOnly))
            {
                caFile.write(caBody);
                caFile.close();
            }
            m_responseResult = true;

        }
        else if (reply->hasRawHeader("certfile"))
        {
            qDebug() << "Certificate received";

            QByteArray certBody = reply->readAll();
            QFile certFile(m_CertPath);
            if (certFile.open(QIODevice::WriteOnly))
            {
                certFile.write(certBody);
                certFile.close();
            }
            m_responseResult = true;
        }
        else if (reply->hasRawHeader("authentication-success"))
        {
            qDebug() << "Authorization succeeded";
            m_responseResult = true;
            signedIn(m_newUser);
        } else if (reply->hasRawHeader("accountcreated-success"))
        {
            qDebug() << "Account created";
            m_responseResult = true;
        }

        break;
    }
    case QNetworkReply::ConnectionRefusedError:
    case QNetworkReply::RemoteHostClosedError:
    case QNetworkReply::HostNotFoundError:
    case QNetworkReply::TimeoutError:
    case QNetworkReply::OperationCanceledError:
    case QNetworkReply::SslHandshakeFailedError:
    case QNetworkReply::TemporaryNetworkFailureError:
    case QNetworkReply::NetworkSessionFailedError:
    case QNetworkReply::BackgroundRequestNotAllowedError:
    #if QT_VERSION >= 0x050600
    case QNetworkReply::TooManyRedirectsError:
    case QNetworkReply::InsecureRedirectError:
    #endif
    case QNetworkReply::UnknownNetworkError:
    case QNetworkReply::ProxyConnectionRefusedError:
    case QNetworkReply::ProxyConnectionClosedError:
    case QNetworkReply::ProxyNotFoundError:
    case QNetworkReply::ProxyTimeoutError:
    case QNetworkReply::ProxyAuthenticationRequiredError:
    case QNetworkReply::UnknownProxyError:
    case QNetworkReply::ContentAccessDenied:
    case QNetworkReply::ContentOperationNotPermittedError:
    case QNetworkReply::ContentNotFoundError:
    case QNetworkReply::AuthenticationRequiredError:
    case QNetworkReply::ContentReSendError:
    case QNetworkReply::ContentConflictError:
    case QNetworkReply::ContentGoneError:
    case QNetworkReply::UnknownContentError:
    case QNetworkReply::ProtocolUnknownError:
    case QNetworkReply::ProtocolInvalidOperationError:
    case QNetworkReply::ProtocolFailure:
    case QNetworkReply::InternalServerError:
    case QNetworkReply::OperationNotImplementedError:
    case QNetworkReply::ServiceUnavailableError:
    case QNetworkReply::UnknownServerError:
    {
        m_user = "";
        qDebug() << "REST error:"  << error << reply->errorString();
        qDebug() << "Response code:" << reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        break;
    }
    default:
        break;
    }

    m_loop.exit();
    reply->deleteLater();
}

void OnMyDiskLinkClient::socketDisconnected()
{
   QMutexLocker locker(&m_connectionStateMutex);
   qDebug() << "Command socket disconnected";
   LinkSocket* socket = static_cast<LinkSocket*>( sender());
   qDebug() << "Reason:" << socket->errorString();
   if (serviceSocket == socket)
   {
            //serviceSocket->abort();

            disconnect(socket, 0, 0, 0);
            serviceSocket->stopPingTimer();
            serviceSocket->deleteLater();
            serviceSocket = nullptr;
            m_pingResponseTimer.stop();
            m_pingMisses = 0;
            m_lastPingTickMs = 0;

            m_linkStatus = Disconnected;

            //Clear workers
            //Legacy file workers are gone in 3.0
            m_connectedDataSockets = 0;
            //Close tunnel sockets
            Q_FOREACH(QTcpSocket* tcpSocket, m_tunnels)
            {
                tcpSocket->disconnectFromHost();
                tcpSocket->deleteLater();
            }
            m_tunnels.clear();

            //Close P2P (libdatachannel) tunnels through the ref-counted teardown
            const QStringList p2pIds = m_p2pTunnels.keys();
            Q_FOREACH(const QString &p2pID, p2pIds)
                cleanupP2PTunnel(p2pID);

            emit linkDisconnected();
    }
}


void OnMyDiskLinkClient::socketConnected()
{
    qDebug() << "Command socket connected";
    LinkSocket* socket = static_cast<LinkSocket*>( sender());
    m_lastPingTickMs = QDateTime::currentMSecsSinceEpoch();
    socket->startPingTimer(PING_INTERVAL);
    m_pingResponseTimer.stop();
    m_pingMisses = 0;
}

//Signed In with new API
void OnMyDiskLinkClient::signedIn(QString userName)
{
    qInfo() << "User signed in:"   << userName;

    QString certCommonName = "";
    if (sslConfiguration.localCertificate().subjectInfo(QSslCertificate::CommonName).count() > 0) {
        certCommonName = sslConfiguration.localCertificate().subjectInfo(QSslCertificate::CommonName).at(0);
    }

    if (!userName.isEmpty() && userName != "anonymous" && !certCommonName.isEmpty() && certCommonName != userName) {
        qInfo() << "Certificate common name" << certCommonName << "does not match signed-in user:" << userName << ". Requesting new certificate...";
        requestNewCert(userName);
        return;
    }

    if (m_user != userName) {
        if (!m_keyLogin) {
            if (m_user.isEmpty())
                requestNewCert(userName); //assign device to the first signed in user.
        }
        m_user = userName;
    }
    if (m_linkStatus != Connected || m_linkStatus != Connecting)
            connectToCloud();
}

void OnMyDiskLinkClient::authRequested(QString userName, QString password)
{
    //Sign in to OMD via API
    #ifdef STAGING
    QString signinAPI = QString("https://%1/signin").arg(m_omdSettings.server());
    #else
    #ifdef QT_DEBUG
    QString signinAPI = QString("http://localhost:8080/signin");
    #else
    QString signinAPI = QString("https://%1/signin").arg(m_omdSettings.server());
    #endif
    #endif
    QNetworkRequest signinRequest(signinAPI);
    signinRequest.setRawHeader("content-type", "application/json");

    QJsonObject request;
    request["action"] = "signin";
    request["username"] = userName;
    request["password"] = password;
    QJsonDocument requestData(request);
    m_newUser = userName;
    post(signinRequest, requestData.toJson(QJsonDocument::Compact));
}

void OnMyDiskLinkClient::siteCreated(QString owner, SiteInfo site)
{
    qInfo() << "Site created:" << site.name << "owner:" << owner << "type:" << site.type;
    if (site.type != "plain")   {
        emit restartSites("restart");
        sendUpdateSite(site.name);
    }
}

void OnMyDiskLinkClient::siteUpdated(QString owner, QString site, bool rebuild)
{
    Q_UNUSED(owner)
    qInfo() << "Site updated:" << site << "Rebuild:" << rebuild;
    if (rebuild)
        emit restartSites("restart");
    sendUpdateSite(site);
}


void OnMyDiskLinkClient::fileRemoved(QString owner, QString user, QString path)
{
    Q_UNUSED(owner)
    Q_UNUSED(user)
    //Site removed?
    QStringList pathList = path.split("/", Qt::SplitBehaviorFlags::SkipEmptyParts);
    if (pathList.count() == 2 && pathList[0] == "sites")
    {
        QString siteName = pathList[1];
        emit restartSites("remove", siteName);
        sendUpdateHostRoot();
    }
}

//==============================================================
// RAG 3.0 owner-only indexing (AI node REST)
// See docs/RAG_3_0.md §5 for trigger semantics.
//==============================================================

QNetworkAccessManager *OnMyDiskLinkClient::ragManager()
{
    if (!m_ragManager)
        m_ragManager = new QNetworkAccessManager(this);
    return m_ragManager;
}

QString OnMyDiskLinkClient::ragLocalPathFor(const QString &virtualPath)
{
    // "shares" maps shareName -> absolute path on this device.
    // virtualPath may be "/<devName>/<share>/..." or "/<share>/...".
    QStringList parts = virtualPath.split("/", Qt::SplitBehaviorFlags::SkipEmptyParts);
    if (parts.isEmpty()) return QString();
    const QString devName = m_hostRoot ? m_hostRoot->hostName() : QString();
    if (!devName.isEmpty() && parts.first() == devName)
        parts.removeFirst();
    if (parts.isEmpty()) return QString();
    QMutexLocker lock(&m_sharesMutex);
    QString shareName = parts.first();
    QString base = shares.value(shareName, "");
    if (base.isEmpty()) return QString();
    parts.removeFirst();
    QString rel = parts.join("/");
    return rel.isEmpty() ? QDir::cleanPath(base) : QDir::cleanPath(base + "/" + rel);
}

QString OnMyDiskLinkClient::ragVirtualPathFor(const QString &realPath)
{
    // Maps a real path on this device to the virtual "/<devName>/<share>/..." path,
    // as displayed in the app (shareName -> absolute path map).
    if (realPath.isEmpty()) return QString();
    const QString devName = m_hostRoot ? m_hostRoot->hostName() : QString();
    const QString prefix = devName.isEmpty() ? QString() : "/" + devName;
    QMutexLocker lock(&m_sharesMutex);
    QHashIterator<QString, QString> it(shares);
    while (it.hasNext()) {
        it.next();
        const QString base = QDir::cleanPath(it.value());
        if (realPath == base)
            return prefix + "/" + it.key();
        if (realPath.startsWith(base + "/"))
            return prefix + "/" + it.key() + realPath.mid(base.length());
    }
    return QString();
}

void OnMyDiskLinkClient::ragIndexPath(const QString &path, bool force)
{
    if (!m_aiSupported || path.isEmpty())
        return;

    if (QThread::currentThread() != this->thread()) {
        QMetaObject::invokeMethod(this, [this, path, force]() {
            ragIndexPath(path, force);
        }, Qt::QueuedConnection);
        return;
    }

    // Debounce: HomeWrite/Write arrive per chunk during an upload; index once
    // the path settles (force bypasses debounce for periodic/share-root events).
    if (!force) {
        m_ragPending.insert(path);
        if (!m_ragFlushTimer.isActive())
            m_ragFlushTimer.start(RAG_FLUSH_DEBOUNCE_MS);
        return;
    }
    ragSendIndex(path, force);
}

void OnMyDiskLinkClient::ragSendIndex(const QString &path, bool force)
{
    if (!m_aiSupported || path.isEmpty())
        return;

    if (QThread::currentThread() != this->thread()) {
        QMetaObject::invokeMethod(this, [this, path, force]() {
            ragSendIndex(path, force);
        }, Qt::QueuedConnection);
        return;
    }

    QString base = aiBaseURL;
    if (base.isEmpty() || base == "http://localhost:8000")
        base = DEFAULT_AI_URL;
    QUrl url(base);
    url.setPath("/rag/index");
    qInfo() << "RAG: Sending index request to" << url.toString() << "for" << path << "force:" << force;
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    req.setRawHeader("X-OMD-RAG-Scope", "all");
    req.setRawHeader("X-OMD-Private-Mode", "1");
    req.setRawHeader("X-OMD-Node-Local", "1");
    QJsonObject body;
    body["path"] = path;
    const QString virtualPath = ragVirtualPathFor(path);
    if (!virtualPath.isEmpty())
        body["logicalPath"] = virtualPath;
    body["recursive"] = true;
    body["force"] = force;
    QNetworkReply *reply = ragManager()->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply, path, force, virtualPath]() {
        const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError || (httpStatus && httpStatus >= 400)) {
            qWarning() << "RAG: index request failed for" << path
                       << "logical:" << virtualPath
                       << "force:" << force
                       << "http:" << httpStatus
                       << ":" << reply->errorString()
                       << ":" << reply->readAll();
        } else {
            qInfo() << "RAG: index request sent for" << path << "http:" << httpStatus;
        }
        reply->deleteLater();
    });
}

void OnMyDiskLinkClient::ragFlushPending()
{
    if (!m_aiSupported)
        return;
    // During the startup grace window the AI node may still be booting
    // (uvicorn :4000). Leave the pending set untouched — it will be flushed
    // once the grace timer fires.
    if (m_ragStartupGrace)
        return;
    QSet<QString> paths = m_ragPending;
    m_ragPending.clear();
    // force=false только в body: AI-нода скипает unchanged файлы по source_stamp,
    // через дебаунс не возвращаемся (см. ragIndexPath)
    Q_FOREACH(const QString &path, paths)
        ragSendIndex(path, false);
}

void OnMyDiskLinkClient::ragStartupGraceElapsed()
{
    m_ragStartupGrace = false;
    qInfo() << "RAG: startup grace elapsed — flushing deferred index requests";
    ragFlushPending();
}

void OnMyDiskLinkClient::ragDeleteSource(const QString &source)
{
    if (!m_aiSupported || source.isEmpty())
        return;

    if (QThread::currentThread() != this->thread()) {
        QMetaObject::invokeMethod(this, [this, source]() {
            ragDeleteSource(source);
        }, Qt::QueuedConnection);
        return;
    }

    QString base = aiBaseURL;
    if (base.isEmpty() || base == "http://localhost:8000")
        base = DEFAULT_AI_URL;
    QUrl url(base);
    url.setPath("/rag/delete");
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    req.setRawHeader("X-OMD-RAG-Scope", "all");
    req.setRawHeader("X-OMD-Private-Mode", "1");
    QJsonObject body;
    const QString virtualPath = ragVirtualPathFor(source);
    body["source"] = virtualPath.isEmpty() ? source : virtualPath;
    QNetworkReply *reply = ragManager()->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [reply, source, virtualPath]() {
        const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError || (httpStatus && httpStatus >= 400)) {
            qWarning() << "RAG: delete source failed for" << source
                       << "logical:" << virtualPath
                       << "http:" << httpStatus
                       << ":" << reply->errorString()
                       << ":" << reply->readAll();
        }
        reply->deleteLater();
    });
}

void OnMyDiskLinkClient::ragReindexTick()
{
    if (!m_aiSupported || !m_omdSettings.autoReindex())
        return;
    QStringList sharePaths;
    {
        QMutexLocker lock(&m_sharesMutex);
        sharePaths = shares.values();
    }
    qInfo() << "RAG: periodic reindex tick, shares:" << sharePaths.join(", ");
    Q_FOREACH(const QString &sharePath, sharePaths) {
        // force=false: AI-нода скипает файлы с неизменным source_stamp
        ragSendIndex(sharePath, false);
    }
}
