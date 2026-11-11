#include "httpnode.h"
#include "version.h"
#include <QJsonObject>
#include <QJsonDocument>
#include <QJsonArray>
#include <QTimer>
#if !defined(P2P_ENABLED) && !defined(NO_WEB_UI)
#include "templateparser.h"
#endif
#include "localdb.h"
#include "ipcclient.h"
#include "userevents.h"
#include "crawler.h"

#ifdef DESKTOP
#include <QDesktopServices>
#endif



OnMyDiskHttpNode::OnMyDiskHttpNode(QString configFile, QObject *parent) : OnMyDiskNode(configFile, parent),
	#ifdef ROUTER
	m_indexFiles(QStringLiteral("router.html")),
	#else
	m_indexFiles(QStringLiteral("client.html")),
	#endif
#ifdef P2P_ENABLED
    m_sharedIndexFiles({QStringLiteral("client.html"), QStringLiteral("index.html")}),
#else
    m_sharedIndexFiles(QStringLiteral("shared.html")),
#endif
    m_autoIndex(false),
	m_maxDiskCacheSize(m_systemSettings.value(QStringLiteral("FileNode/maxDiskCacheSize"), DISK_CACHE_SIZE).toULongLong()),
	m_maxRamCacheSize(m_systemSettings.value(QStringLiteral("FileNode/maxRamCacheSize"), RAM_CACHE_SIZE).toULongLong()),
    m_maxFileSize(m_systemSettings.value(QStringLiteral("Network/maxFileSize"),  PACKET_SIZE ).toULongLong()),
    m_authProviders(),
#if !defined(P2P_ENABLED) && !defined(NO_WEB_UI)
    m_wsServer(QStringLiteral("OnMyDisk 2.0 Node"),  QWebSocketServer::NonSecureMode),
    m_clientBackend(&m_wsServer),
#endif
    m_sessionProvider(QStringLiteral("OnMyDisk 2.0")),
    m_linkClient(&m_systemSettings, m_rootResource),
    #ifdef DESKTOP
    m_client(&m_userSettings, m_rootResource),
    #else
    #ifdef INNBOX
    m_client(&m_systemSettings, m_rootResource),
    #else
    m_client(&m_userSettings, m_rootResource),
    #endif
    #endif
    m_crawler(Crawler::instance())

{
    qRegisterMetaType<CallInfo>("CallInfo");
    qRegisterMetaType<SiteInfo>("SiteInfo");

    m_rootResource->setCustomAttribute("version", APP_VERSION);
    #if PERF_TEST
    HttpHandler::prepareTest();
    #endif
    CloudResource::m_maxFileSize = m_maxFileSize;


    //Key login mode
    m_keyLogin = m_linkClient.keyLogin();
    m_searchEnabled = false;

    m_nonAuthDestinations << "src" << "img"  << "link"  << "apps"   << "fonts" << "theme" << "qwebchannel" << "mimetypes" <<  "jssocial"
                          << "lightgallery" << "node_modules" << "dist" << "rest" << "customtheme" << "reset" << "key" << "signin" << "manifest.json"
                          << "home-back-b.jpg"  << "logo-white.svg" << "favicon.ico" << "login" <<  "signup" << "shared.html" << "assets"
                          << "tos" << "tos.html" << "about" << "about.html" << "about-i18n.js" << "robots.txt" << "claim" << "description.xml" << "api";
#ifdef P2P_ENABLED
    m_nonAuthDestinations << "shared";
#endif



    QString fqdn = QHostInfo::localHostName();
    if (!QHostInfo::localDomainName().isEmpty())
    {
        fqdn.append(".");
        fqdn.append(QHostInfo::localDomainName());
    }
    m_serverString = QStringLiteral("OnMyDisk Client Node (2.0)");

#if !defined(P2P_ENABLED) && !defined(NO_WEB_UI)
    //Template parser
    TemplateParser::instance().setKeyLogin(m_keyLogin);
    TemplateParser::instance().setSessionAuth(&m_sessionProvider);
    #ifdef INNBOX
    TemplateParser::instance().setStaticVar("APPTITLE", m_systemSettings.value(QStringLiteral("AppTitle"), QStringLiteral("InnboxShare")));
    TemplateParser::instance().setStaticVar("COMPANY", m_systemSettings.value(QStringLiteral("Company"), QStringLiteral("Innbox")));
    TemplateParser::instance().setStaticVar("SLOGAN", m_systemSettings.value(QStringLiteral("Slogan"), QStringLiteral("Innbox &mdash; World in a box")));
    #else
    TemplateParser::instance().setStaticVar("APPTITLE", m_systemSettings.value(QStringLiteral("AppTitle"), QStringLiteral("Disk Direct")));
    TemplateParser::instance().setStaticVar("COMPANY", m_systemSettings.value(QStringLiteral("Company"), QStringLiteral("Bineon")));
    TemplateParser::instance().setStaticVar("SLOGAN", m_systemSettings.value(QStringLiteral("Slogan"), QStringLiteral("Delivering content right from your disk")));
    #endif
    TemplateParser::instance().setStaticVar("KEYLOGIN", m_keyLogin);
    TemplateParser::instance().setStaticVar("VERSION", APP_VERSION);
#ifdef Q_OS_LINUX
    bool sambaSupported = isService();
#else
    bool sambaSupported = false;
#endif
    TemplateParser::instance().setStaticVar("SAMBASUPPORTED", sambaSupported);

#ifdef STAGING
    QString server = DEFAULT_CLOUD;
    TemplateParser::instance().setStaticVar("SERVER", server);
    TemplateParser::instance().setStaticVar("SERVERURL", QString("https://%1").arg(server));
    TemplateParser::instance().setStaticVar("CALLBACKURL", QString("https://%1").arg(server));
    TemplateParser::instance().setCondition("DEV", false);
    TemplateParser::instance().setCondition("PROD", true);
#else
#ifdef QT_DEBUG
    QString server = QStringLiteral("localhost");
    TemplateParser::instance().setStaticVar("SERVER", server);
    TemplateParser::instance().setStaticVar("SERVERURL", QString("http://localhost:8080"));
    TemplateParser::instance().setStaticVar("CALLBACKURL", QString("http://192.168.122.1:8080"));
    TemplateParser::instance().setCondition("DEV", true);
    TemplateParser::instance().setCondition("PROD", false);
#else
    //QString server = DEFAULT_CLOUD;
    QString server = m_systemSettings.value(QStringLiteral("Settings/server"), DEFAULT_CLOUD).toString();
    TemplateParser::instance().setStaticVar("SERVER", server);
    TemplateParser::instance().setStaticVar("SERVERURL", QString("https://%1").arg(server));
    TemplateParser::instance().setStaticVar("CALLBACKURL", QString("https://%1").arg(server));
    TemplateParser::instance().setCondition("DEV", false);
    TemplateParser::instance().setCondition("PROD", true);
#endif
#endif
    TemplateParser::instance().setStaticVar("LOGIN_LOGO", "login-logo");
    TemplateParser::instance().setStaticVar("LOGO_OG", "loogo-og.jpg");
    TemplateParser::instance().setStaticVar("MEET_LOGIN_LOGO", "meet-login-logo");
    TemplateParser::instance().setStaticVar("MEET_LOGO_OG", "meet-loogo-og.jpg");
    TemplateParser::instance().setStaticVar("MEET_APPTITLE", m_systemSettings.value(QStringLiteral("MeetTitle"), QStringLiteral("On My Meet")));
    TemplateParser::instance().setStaticVar("MEET_SLOGAN", m_systemSettings.value(QStringLiteral("MeetSlogan"), QStringLiteral("Secure Meet & Share service")));
    TemplateParser::instance().setStaticVar("CHATS_APPTITLE", m_systemSettings.value(QStringLiteral("ChatsTitle"), QStringLiteral("AI Assistant")));
    QString aiServer = m_systemSettings.value(QStringLiteral("AiServer"), QStringLiteral("http://localhost:4000")).toString();
    TemplateParser::instance().setStaticVar("AI_SERVER", aiServer);
    TemplateParser::instance().setStaticVar("CHAT_SERVER", aiServer.replace("http://", "").replace("https://", ""));
    #ifdef INNBOX
    TemplateParser::instance().setStaticVar("SYSTEMURL", QString("https://storage.innbox.app"));
    #else
    #ifdef P2P_ENABLED
    TemplateParser::instance().setStaticVar("SYSTEMURL", QString("https://disk.direct"));
    #elif defined(STAGING)
    TemplateParser::instance().setStaticVar("SYSTEMURL", QString("https://disk.direct"));
    #else
    #ifdef QT_DEBUG
    TemplateParser::instance().setStaticVar("SYSTEMURL", QString("http://localhost:8080"));
    #else
    TemplateParser::instance().setStaticVar("SYSTEMURL", m_systemSettings.value(QStringLiteral("Settings/SystemUrl"), "").toString());
    #endif
    #endif
    #endif
    TemplateParser::instance().setStaticVar("MEETSERVER", QString("jitsi.bineon.team"));
    #ifdef P2P_ENABLED
    TemplateParser::instance().setStaticVar("GUN_HUB_URL", m_systemSettings.value(QStringLiteral("GunHubUrl"), QString("https://disk.direct:8765")));
    #else
    TemplateParser::instance().setStaticVar("GUN_HUB_URL", m_systemSettings.value(QStringLiteral("GunHubUrl"), QString("https://disk.direct:8765")));
    #endif

    TemplateParser::instance().setStaticVar("SEARCHSERVERENABLED", m_searchEnabled);
    TemplateParser::instance().setStaticVar("SIGN_SLOGAN", m_systemSettings.value(QStringLiteral("SignSlogan"), QStringLiteral("Signing your documents right on your disk")));
#endif

    // Client events
    connect(&m_client, &OnMyDiskClient::shareAdded, &m_linkClient, &OnMyDiskLinkClient::addLocalShare, Qt::QueuedConnection);
    connect(&m_client, &OnMyDiskClient::shareRemoved, &m_linkClient, &OnMyDiskLinkClient::removeLocalShare, Qt::QueuedConnection);
#if !defined(P2P_ENABLED) && !defined(NO_WEB_UI)
    connect(&m_client, &OnMyDiskClient::shareAdded, &m_clientBackend, &HttpDeviceBackend::shareAdded);
    connect(&m_client, &OnMyDiskClient::shareRemoved, &m_clientBackend, &HttpDeviceBackend::shareRemoved);
	connect(&m_client, &OnMyDiskClient::storageIdUpdated, &m_clientBackend, &HttpDeviceBackend::updateStorageId);
    connect(&m_client, &OnMyDiskClient::logOutput, &m_clientBackend, &HttpDeviceBackend::logMessage);
#endif
    connect(&m_client, &OnMyDiskClient::settingsLoaded, &m_linkClient, &OnMyDiskLinkClient::updateSettings);
    connect(&m_linkClient, &OnMyDiskLinkClient::settingsChanged, &m_client, &OnMyDiskClient::applySettings, Qt::QueuedConnection);

    // Backend events
#if !defined(P2P_ENABLED) && !defined(NO_WEB_UI)
    connect(&m_clientBackend, &HttpDeviceBackend::settingsChanged, &m_client, &OnMyDiskClient::applySettings);
    connect(&m_clientBackend, &HttpDeviceBackend::settingsChanged, this, &OnMyDiskHttpNode::settingsLoaded);
    connect(&m_client, &OnMyDiskClient::stateChanged, &m_clientBackend, &HttpDeviceBackend::updateState);
    connect(&m_client, &OnMyDiskClient::settingsLoaded, &m_clientBackend, &HttpDeviceBackend::settingsLoaded);
#endif
    connect(&m_client, &OnMyDiskClient::settingsLoaded, this, &OnMyDiskHttpNode::settingsLoaded);

    //Link client events
    connect(&m_linkClient, &OnMyDiskLinkClient::linkConnected, &m_client, &OnMyDiskClient::linkConnected);
    connect(&m_linkClient, &OnMyDiskLinkClient::linkDisconnected, &m_client, &OnMyDiskClient::linkDisconnected);
    connect(&m_linkClient, &OnMyDiskLinkClient::connectionError, &m_client, &OnMyDiskClient::connectionError);
    connect(&m_linkClient, &OnMyDiskLinkClient::sslError, &m_client, &OnMyDiskClient::sslError, Qt::QueuedConnection);
    connect(&m_linkClient, &OnMyDiskLinkClient::privateKeyError, &m_client, &OnMyDiskClient::pkiError);
    connect(&m_linkClient, &OnMyDiskLinkClient::privateKeyStatus, &m_client, &OnMyDiskClient::pkiStatusChange);
    connect(&m_linkClient, &OnMyDiskLinkClient::sslOK, &m_client, &OnMyDiskClient::certLoaded);
    connect(&m_linkClient, &OnMyDiskLinkClient::accountCreated, &m_client, &OnMyDiskClient::accountCreated);
    connect(&m_linkClient, &OnMyDiskLinkClient::deviceIdUpdated, &m_client, &OnMyDiskClient::deviceIdUpdated);
    #ifndef ROUTER
    connect(&m_linkClient, &OnMyDiskLinkClient::restartSites, &m_client, &OnMyDiskClient::restartSites);
    connect(&UserEvents::instance(), &UserEvents::siteCreated, &m_linkClient, &OnMyDiskLinkClient::siteCreated);
    connect(&UserEvents::instance(), &UserEvents::siteUpdated, &m_linkClient, &OnMyDiskLinkClient::siteUpdated);
    #endif
    //link backend
    connect(&LocalDB::instance(), &LocalDB::publicLinkAdded, &m_linkClient,  &OnMyDiskLinkClient::publicLinkAdded);

    //UI client
    connect(&m_client, &OnMyDiskClient::connectCloud, &m_linkClient, &OnMyDiskLinkClient::connectToCloud, Qt::QueuedConnection);
    connect(&m_client, &OnMyDiskClient::disconnectCloud, &m_linkClient, &OnMyDiskLinkClient::disconnectFromCloud, Qt::QueuedConnection);
    connect(&m_client, &OnMyDiskClient::resetLink, &m_linkClient, &OnMyDiskLinkClient::resetDeviceId, Qt::QueuedConnection);
    connect(&m_client, &OnMyDiskClient::shareAdded, &m_linkClient, &OnMyDiskLinkClient::addLocalShare, Qt::QueuedConnection);
    connect(&m_client, &OnMyDiskClient::shareRemoved, &m_linkClient, &OnMyDiskLinkClient::removeLocalShare, Qt::QueuedConnection);

    //new signin and users evenrs API
    connect(&UserEvents::instance(), &UserEvents::signedIn, &m_linkClient, &OnMyDiskLinkClient::signedIn);
    #ifndef ROUTER
    connect(&UserEvents::instance(), &UserEvents::addShareRequested, &m_client, &OnMyDiskClient::shareAddRequested);
    connect(&UserEvents::instance(), &UserEvents::removeShareRequested, &m_client, &OnMyDiskClient::shareRemoveRequested);
    #endif
    connect(&m_client, &OnMyDiskClient::shareAdded, this, &OnMyDiskHttpNode::shareAdded);
    connect(&m_client, &OnMyDiskClient::shareRemoved, this, &OnMyDiskHttpNode::shareRemoved);


    //State change and cert load event
    connect(&m_client, &OnMyDiskClient::stateChanged, this, &OnMyDiskNode::stateChanged);
    connect(&m_linkClient, &OnMyDiskLinkClient::sslOK, this, &OnMyDiskNode::certLoaded);

	//IPC events
	connect(this, &OnMyDiskHttpNode::disconnectCloud, &m_client, &OnMyDiskClient::signOut, Qt::QueuedConnection);
	connect(this, &OnMyDiskHttpNode::connectCloud, &m_client, &OnMyDiskClient::signIn, Qt::QueuedConnection);
	connect(this, &OnMyDiskHttpNode::addShare, &m_client, &OnMyDiskClient::addShare, Qt::QueuedConnection);
	connect(this, &OnMyDiskHttpNode::removeShare, &m_client, &OnMyDiskClient::removeShare, Qt::QueuedConnection);
    connect(this, &OnMyDiskNode::resetLink, &m_linkClient, &OnMyDiskLinkClient::clearDeviceId);
    connect(this, &OnMyDiskHttpNode::quit, qApp, &QCoreApplication::quit, Qt::QueuedConnection);

	//http threads

#if !defined(P2P_ENABLED) && !defined(NO_WEB_UI)
    int wsPort = m_systemSettings.value("Webchannel/Port", 8897).toInt();
    if (! m_wsServer.listen(QHostAddress::AnyIPv4, wsPort)) {
        qWarning() << "Failed to start WebSocket server on port" << wsPort << ", trying random port...";
        if (! m_wsServer.listen(QHostAddress::AnyIPv4, 0)) {
             qCritical() << "Failed to start WebSocket server!";
        }
    }

    if (m_wsServer.isListening()) {
        qInfo() << "Starting WebSocket notifications on port" << m_wsServer.serverPort();
        TemplateParser::instance().setStaticVar("WSPORT", m_wsServer.serverPort());
    }
#endif

    //setup networking
    setupNetwork(&m_fileServer, &m_systemSettings, QString("FileNode"));
	if (m_fileServer.isListening())
	{
         qInfo() << "Starting Web UI on port" << m_fileServer.serverPort();
		 connect(&m_fileServer, &QHttpServer::newRequest, this, &OnMyDiskHttpNode::handleRequest, Qt::DirectConnection);
         //enablePort(m_fileServer.serverPort());
	} else
		 qCritical() << "Can't start Web UI";

    //load/create device id
    m_linkClient.readSettings();
	//read shares
	m_client.readSettings();

    //Announce the device
#ifdef Q_OS_LINUX
    if (isService() && m_fileServer.isListening()) {
#ifndef ROUTER
        qInfo() << "Announcing device:" << m_omdSettings.hostName();
        m_ssdpAnnouncer = new SsdpAnnouncer(this);
        m_ssdpAnnouncer->setHttpServerPort(m_fileServer.serverPort());
        m_ssdpAnnouncer->setHostName(m_omdSettings.hostName());
        m_ssdpAnnouncer->setModelName("OMD2.0");
        m_ssdpAnnouncer->setServerName("Linux/5.15 UPnP/1.0 OMD/2.0");
        m_ssdpAnnouncer->start();
#endif

        // Update Avahi mDNS service if avahi daemon directory exists
        QDir avahiDir("/etc/avahi/services");
        if (avahiDir.exists()) {
            QString avahiXml = QString(
                "<?xml version=\"1.0\" standalone='no'?>\n"
                "<!DOCTYPE service-group SYSTEM \"avahi-service.dtd\">\n"
                "<service-group>\n"
                "  <name replace-wildcards=\"yes\">OnMyDisk (%h)</name>\n"
                "  <service>\n"
                "    <type>_http._tcp</type>\n"
                "    <port>%1</port>\n"
                "    <txt-record>path=/</txt-record>\n"
                "  </service>\n"
                "</service-group>\n"
            ).arg(m_fileServer.serverPort());

            QFile f("/etc/avahi/services/onmydisk.service");
            if (f.open(QFile::WriteOnly | QFile::Text)) {
                f.write(avahiXml.toUtf8());
                f.close();
                qInfo() << "Configured Avahi mDNS on port" << m_fileServer.serverPort();
            }
        }
    }
#endif

}

void OnMyDiskHttpNode::settingsLoaded(const OnMyDiskSettings& settings)
{
    m_omdSettings = settings;
#if ((EMBEDDED || SERVICE || defined(DESKTOP) || defined(SERVER)) && !defined(ROUTER))
    m_searchEnabled = m_omdSettings.localSearch();
#else
    m_searchEnabled = false;
#endif
    QString server = m_omdSettings.server();
#if !defined(P2P_ENABLED) && !defined(NO_WEB_UI)
    TemplateParser::instance().setStaticVar("SEARCHSERVERENABLED", m_searchEnabled);
    #ifndef QT_DEBUG
    TemplateParser::instance().setStaticVar("SERVER", server);
    TemplateParser::instance().setStaticVar("SERVERURL", QString("https://%1").arg(server));
    #endif
#ifdef EMBEDDED
    // Embedded client: frontend sends AI requests to local httphandler which proxies to OnMyChat
    TemplateParser::instance().setStaticVar("AI_SERVER", QStringLiteral("/ai"));
#else
    TemplateParser::instance().setStaticVar("AI_SERVER", m_systemSettings.value(QStringLiteral("AiServer"), m_omdSettings.aiBase()));
#endif
#endif

#if ((EMBEDDED || SERVICE || defined(DESKTOP) || defined(SERVER)) && !defined(ROUTER))
    // Sync user AI settings to system settings so httphandler (which reads system config) can access them
    m_systemSettings.beginGroup("Settings");
    m_systemSettings.endGroup();
    if (!m_omdSettings.aiBase().isEmpty() && !m_systemSettings.contains("AiServer"))
        m_systemSettings.setValue("AiServer", m_omdSettings.aiBase());
    m_systemSettings.sync();
#endif

    m_sessionProvider.setServer(server);
    //Crawler setup
    m_crawler.init(QStringLiteral("http://localhost:9090"), QString("https://%1").arg(server));
#ifndef ROUTER
    if (m_ssdpAnnouncer)
        m_ssdpAnnouncer->setHostName(m_omdSettings.hostName());
#endif



}

OnMyDiskHttpNode::~OnMyDiskHttpNode()
{
#ifndef ROUTER
    if (m_ssdpAnnouncer) {
        m_ssdpAnnouncer->stop();
        delete m_ssdpAnnouncer;
    }
#endif

    m_rootResource->removeAllChildren();
}

#if !defined(P2P_ENABLED) && !defined(NO_WEB_UI)
void OnMyDiskHttpNode::enablePort(int port)
{
#ifdef OPENWRT
	QProcess iptables;
	iptables.start("/sbin/iptables",
		   QStringList() << "-I" << "INPUT" << "-p" << "tcp" << "--dport" << QString::number(port) << "-j" << "ACCEPT");
	iptables.waitForFinished(1000);
#else
 Q_UNUSED(port)
#endif
}
#endif

bool OnMyDiskHttpNode::isListening()
{
   return m_fileServer.isListening();
}

quint16 OnMyDiskHttpNode::serverPort()
{
  return  m_fileServer.serverPort();
}

void OnMyDiskHttpNode::setupNetwork(QHttpServer* server, QSettings *settings, QString settingsSection)
{
    qint16 port = settings->value(QString("%1/Port").arg(settingsSection), 8081).toInt();
    #if defined(STAGING) || defined(QT_DEBUG)
    if (port < 1024 && !isService())
        port = 8081;
    #endif
    server->listen(QHostAddress::AnyIPv4, port, m_maxFileSize);
}

#if !defined(P2P_ENABLED) && !defined(NO_WEB_UI)
void OnMyDiskHttpNode::setupNotifications(QSettings *settings, QString settingsSection)
{
    //WebSocket Server
    qint16 port = settings->value(QString("%1/Notifications").arg(settingsSection), 9988).toInt();
    if (! m_wsServer.listen(QHostAddress::AnyIPv4, port)) {
        qWarning() << "Failed to start WebSocket server on port" << m_wsServer.serverPort();
    } else
        // get real port
        qInfo() << "Starting WebSocket notifications on port" << m_wsServer.serverPort();
}
#endif


void OnMyDiskHttpNode::cleanUp()
{
	//IconCache::Instance()->clear();
	QTimer::singleShot(PING_INTERVAL, [this]() {
         qDebug() <<  "Child count:" << m_rootResource->childCount();
       // __lsan_do_recoverable_leak_check();
       //m_rootResource.removeAllChildren();
    });
}


void OnMyDiskHttpNode::shareAdded(QString shareName)
{
    if (m_searchEnabled) {
        m_rootResource->hostName();
        QTimer::singleShot(3000, this,  [this,  shareName]() {
            QString user = m_linkClient.user();
            QString collection = user.isEmpty() ? QStringLiteral("omd") : QString();
            QString path = QString ("%1/%2/%3/").arg(user, m_rootResource->hostName(), shareName);
            m_crawler.crawlPath(path, true, 86400, collection);
        });
    }
}

void OnMyDiskHttpNode::shareRemoved(QString shareName)
{
    if (m_searchEnabled) {
        QString path = QString ("%1/%2/%3").arg(m_linkClient.user(), m_rootResource->hostName(), shareName);
    }
}


void OnMyDiskHttpNode::close()
{
#if !defined(P2P_ENABLED) && !defined(NO_WEB_UI)
    m_wsServer.close();
#endif
}


void OnMyDiskHttpNode::performRequest(QHttpRequest* req, QHttpResponse* resp, HttpHandler *handler)
{
    Q_UNUSED(resp);

	connect(req, &QHttpRequest::end, [handler]() {
		//qDebug() << "Ready" << req->url().toString();
		handler->requestReady(false);
	});

    connect(resp, &QHttpResponse::bytesWritten, handler, [ handler]() {
        //qDebug() << "Bytes to send"  << resp->bytesToSend();
        handler->readNextDataPart();
    });

    connect(resp, &QHttpResponse::allBytesWritten, handler, [ handler]() {
        //qDebug() << "Bytes to send"  << resp->bytesToSend();
        handler->readNextDataPart();
    });

    connect(req->connection(), &QHttpConnection::disconnected,  handler, [handler]() {
                //qDebug() << "Done"  << req->url().toString();
		handler->requestCompleted();
        handler->releaseHandler();
        //delete handler;
	});


	//qInfo() << "Running:" << req->url().toString();
    handler->doWork();
}

void OnMyDiskHttpNode::handleRequest(QPointer<QHttpRequest> req, QPointer<QHttpResponse> resp)
{
    QString userID;
    QString rootOwnerID;
    QString itemPath;

    if (req->isDropped())
    {
        qWarning() << "Request dropped";
        return;
    }

    AuthResult r = CheckAuth(req, resp, userID, rootOwnerID, itemPath);

    switch (r) {
        case AUTH_OK_PUBLIC:
        case AUTH_OK_REF:
        case AUTH_OK_SHARED:
        case AUTH_OK_CRAWLING:
        case AUTH_OK_SEARCHING:
        case AUTH_OK:
        {
              QStringList indexFiles = r== AUTH_OK_PUBLIC ? m_sharedIndexFiles : m_indexFiles;

#ifndef ROUTER
              if (req->url().path() == "/description.xml" && m_ssdpAnnouncer) {
                    m_ssdpAnnouncer->sendDeviceDescription(resp);
              } else
#endif
              if (req->url().path() == "/claim" || req->url().path() == "/claim.html"
#ifdef EMBEDDED
                         || req->url().path() == "/" || req->url().path().isEmpty()
#endif
              ) {
                    sendClaimPage(req, resp);
              } else if (req->url().path() == "/api/pair/info") {
                    sendPairInfo(req, resp);
              } else if (req->url().path() == "/icons/onmydisk.png" || req->url().path() == "/onmydisk.png" || req->url().path() == "/favicon.ico") {
                    QFile iconFile(":/icons/onmydisk.png");
                    if (iconFile.open(QFile::ReadOnly)) {
                        QByteArray data = iconFile.readAll();
                        resp->addHeader("Content-Type", "image/png");
                        resp->addHeader("Content-Length", QString::number(data.size()).toUtf8());
                        resp->addHeader("Cache-Control", "max-age=86400");
                        resp->setStatusCode(qhttp::ESTATUS_OK);
                        resp->end(data);
                    } else {
                        resp->setStatusCode(qhttp::ESTATUS_NOT_FOUND);
                        resp->end();
                    }
              } else {

                HttpHandler *handler = new HttpHandler(&m_systemSettings, req, resp, indexFiles, m_serverString, m_maxFileSize,
									   m_autoIndex, userID, rootOwnerID,
									   &m_sessionProvider,
                                       m_rootResource, m_searchEnabled, m_keyLogin);

                performRequest(req, resp, handler);
              }
              break;

        }
        case AUTH_NOTALLOWED:
        {
            QString answerString = QStringLiteral("405 Method no allowed");
            QByteArray body = answerString.toUtf8();
            body.append("\n");
            resp->addHeader("Content-Type", "text/plain; charset=UTF-8");
            resp->addHeader("Content-Length", QString::number(body.size()).toUtf8());
            resp->addHeader("Server", m_serverString.toUtf8());
            resp->setStatusCode(qhttp::ESTATUS_METHOD_NOT_ALLOWED);
            resp->end(body);
            break;
        }
        case AUTH_OFFLINE:
        case AUTH_FAIL:
        {
            //qWarning() << "Auth failed for" << req->methodString() << req->url().toString();
            RequestAuth(req, resp);
            break;
        }
    }
}


AuthResult OnMyDiskHttpNode::CheckAuth(QHttpRequest *req, QHttpResponse *resp, QString &userid, QString &rootOwnerID, QString &itemPath)
{
    userid = QStringLiteral("anonymous");
    rootOwnerID = QStringLiteral("anonymous");
    itemPath = req->url().toString();
    QString linkID;

    if (req->method() == qhttp::EHTTP_NOTIFY || req->method() == qhttp::EHTTP_OPTIONS)
        return AUTH_OK;


    if (req->urlPath().size() > 0 && m_nonAuthDestinations.contains(req->urlPath().at(0)))
        return AUTH_OK;

    AuthResult sessionAuthResult = m_sessionProvider.validate(req, resp, userid, rootOwnerID, linkID, itemPath);

    if (sessionAuthResult != AUTH_FAIL && sessionAuthResult != AUTH_NOTALLOWED && sessionAuthResult != AUTH_OFFLINE) {
        return sessionAuthResult;
    }

    return sessionAuthResult;
}



void OnMyDiskHttpNode::RequestAuth(QHttpRequest *req, QHttpResponse *resp)
{
    if (req->method() == qhttp::EHTTP_PROPFIND || req->method() == qhttp::EHTTP_POST)
    {
        QString answerString = QStringLiteral("401 Unauthorized");
        for (int i = 0; i < m_authProviders.length(); i++)
        {
            resp->addHeader("WWW-Authenticate", m_authProviders.at(i)->headerString(req).toUtf8());
        }
        QByteArray body = answerString.toUtf8();

        body.append("\n");
        resp->addHeader("Content-Type", "text/plain; charset=UTF-8");
        resp->addHeader("Content-Length", QString::number(body.size()).toUtf8());
        resp->addHeader("Server", m_serverString.toUtf8());
        resp->setStatusCode( qhttp::ESTATUS_UNAUTHORIZED);
        //qDebug() << resp->headers();
        resp->end(body);
        return;
    }

    if (req->headers().contains("accept") && req->headers()["accept"].contains("json"))
    {
        QJsonObject result;
        result["success"] = false;

        QUrl loginUrl(req->url());
        loginUrl.setPath("/");
        QJsonObject error;
        error["message"] = loginUrl.toString();

        QJsonObject response;
        response["result"] = result;
        response["error"] = error;

        //Write response
        QJsonDocument responseData(response);
        QByteArray body = responseData.toJson(QJsonDocument::Compact);
        body.append("\n");
        resp->addHeader("Content-Type", "text/json; charset=UTF-8");
        resp->addHeader("Content-Length", QString::number(body.size()).toUtf8());
        resp->addHeader("Location", loginUrl.toString(QUrl::FullyEncoded).toUtf8());
        resp->addHeader("Server", m_serverString.toUtf8());
        resp->setStatusCode(qhttp::ESTATUS_UNAUTHORIZED);
        resp->end(body);
    }
}

void OnMyDiskHttpNode::sendClaimPage(QHttpRequest *req, QHttpResponse *resp)
{
    Q_UNUSED(req)
    QString linkId = m_linkClient.linkId();
    QString deviceId = m_linkClient.deviceId();
    QString claimId = !deviceId.isEmpty() ? deviceId : linkId;
    QString shortId = !deviceId.isEmpty() ? deviceId : linkId.left(10);
    QString deviceName = m_rootResource ? m_rootResource->hostName() : QHostInfo::localHostName();

    // Use configured server, fallback to onmydisk.net
    QString server = m_omdSettings.server();
    if (server.isEmpty())
        server = m_systemSettings.value(QStringLiteral("Settings/server"), QStringLiteral("disk.direct")).toString();

    QString claimUrl = QString("https://%1/?claimLink=%2&name=%3")
                           .arg(server,
                                QString::fromLatin1(QUrl::toPercentEncoding(claimId)),
                                QString::fromLatin1(QUrl::toPercentEncoding(deviceName)));

    // Escape for HTML attribute and for JS string
    QString claimUrlHtml = claimUrl.toHtmlEscaped();
    QString claimUrlJs = claimUrl;
    claimUrlJs.replace(QStringLiteral("\\"), QStringLiteral("\\\\"));
    claimUrlJs.replace(QStringLiteral("'"), QStringLiteral("\\'"));

    QString html = QStringLiteral(
        "<!DOCTYPE html>\n"
        "<html lang=\"en\">\n"
        "<head>\n"
        "  <meta charset=\"UTF-8\">\n"
        "  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n"
        "  <title>Disk Direct &mdash; ") + deviceName.toHtmlEscaped() + QStringLiteral("</title>\n"
        "  <style>\n"
        "    :root{--bg:#0f172a;--card:#1e293b;--border:#334155;--text:#f8fafc;--muted:#94a3b8;\n"
        "      --primary:#38bdf8;--primary-h:#0ea5e9;--green:#22c55e;--warn:#f59e0b;}\n"
        "    *{box-sizing:border-box;margin:0;padding:0}\n"
        "    body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;\n"
        "      background:var(--bg);color:var(--text);display:flex;align-items:center;\n"
        "      justify-content:center;min-height:100vh;padding:1.5rem}\n"
        "    .card{background:var(--card);border:1px solid var(--border);border-radius:16px;\n"
        "      padding:2.5rem 2rem;max-width:460px;width:100%;text-align:center;\n"
        "      box-shadow:0 15px 35px -5px rgba(0,0,0,.6)}\n"
        "    .badge{display:inline-flex;align-items:center;gap:.35rem;padding:.3rem .8rem;\n"
        "      border-radius:9999px;font-size:.75rem;font-weight:600;\n"
        "      background:rgba(34,197,94,.15);color:var(--green);margin-bottom:1.25rem;\n"
        "      letter-spacing:.05em;text-transform:uppercase}\n"
        "    h1{font-size:1.5rem;font-weight:700;margin-bottom:.5rem;letter-spacing:-.02em}\n"
        "    .sub{color:var(--muted);font-size:.875rem;line-height:1.6;margin-bottom:1.5rem}\n"
        "    .qr-wrap{background:#fff;padding:1.25rem;border-radius:14px;\n"
        "      display:inline-flex;align-items:center;justify-content:center;\n"
        "      margin-bottom:1.5rem;box-shadow:0 4px 12px rgba(0,0,0,.15);min-width:212px;min-height:212px}\n"
        "    .qr-wrap img,.qr-wrap canvas{display:block}\n"
        "    .info{background:rgba(15,23,42,.6);border:1px solid var(--border);border-radius:10px;\n"
        "      padding:.85rem 1.15rem;margin-bottom:1.25rem;text-align:left;font-size:.8125rem}\n"
        "    .row{display:flex;justify-content:space-between;align-items:center;margin-bottom:.35rem}\n"
        "    .row:last-child{margin-bottom:0}\n"
        "    .lbl{color:var(--muted)}\n"
        "    .val{font-family:ui-monospace,SFMono-Regular,Menlo,monospace;font-weight:600;\n"
        "      color:var(--text);letter-spacing:.04em}\n"
        "    .notice{background:rgba(245,158,11,.08);border:1px solid rgba(245,158,11,.25);\n"
        "      border-radius:10px;padding:.9rem 1rem;margin-bottom:1.25rem;\n"
        "      text-align:left;font-size:.8rem;line-height:1.55;color:#fde68a}\n"
        "    .notice-title{font-weight:700;color:var(--warn);margin-bottom:.25rem}\n"
        "    .btn{display:inline-flex;align-items:center;justify-content:center;gap:.5rem;\n"
        "      width:100%;background:var(--primary);color:#0f172a;padding:.85rem 1.25rem;\n"
        "      border-radius:10px;font-size:.9375rem;font-weight:600;text-decoration:none;\n"
        "      transition:all .15s ease;cursor:pointer}\n"
        "    .btn:hover{background:var(--primary-h);transform:translateY(-1px)}\n"
        "  </style>\n"
        "</head>\n"
        "<body>\n"
        "  <div class=\"card\">\n"
        "    <div class=\"badge\">&#9679; Online</div>\n"
        "    <h1>") + deviceName.toHtmlEscaped() + QStringLiteral("</h1>\n"
        "    <p class=\"sub\">Scan the QR code or click the button below to connect this device to your Disk Direct account.</p>\n"
        "    <div class=\"qr-wrap\" id=\"qr\"><img id=\"qr-img\"\n"
        "      src=\"https://api.qrserver.com/v1/create-qr-code/?size=180x180&amp;data=") + claimUrlHtml + QStringLiteral("\"\n"
        "      width=\"180\" height=\"180\" alt=\"QR Code\"/></div>\n"
        "    <div class=\"info\">\n"
        "      <div class=\"row\"><span class=\"lbl\">Link ID:</span><span class=\"val\">") + shortId.toHtmlEscaped() + QStringLiteral("</span></div>\n"
        "      <div class=\"row\"><span class=\"lbl\">Device:</span><span class=\"val\">") + deviceName.toHtmlEscaped() + QStringLiteral("</span></div>\n"
        "    </div>\n"
        "    <div class=\"notice\">\n"
        "      <div class=\"notice-title\">&#128274; Recommendation &mdash; Create a DID</div>\n"
        "      During pairing, we recommend creating a <strong>Decentralized Identity (DID)</strong> with your username and password.\n"
        "      This links all your devices into a private, encrypted P2P storage mesh without relying on centralized servers.\n"
        "    </div>\n"
        "    <a href=\"") + claimUrlHtml + QStringLiteral("\" class=\"btn\" target=\"_blank\" rel=\"noopener\">\n"
        "      Connect in Disk Direct &rarr;\n"
        "    </a>\n"
        "  </div>\n"
        "  <script>\n"
        "  (function(){\n"
        "    var url='") + claimUrlJs + QStringLiteral("';\n"
        "    var s=document.createElement('script');\n"
        "    s.src='https://cdnjs.cloudflare.com/ajax/libs/qrcodejs/1.0.0/qrcode.min.js';\n"
        "    s.onload=function(){\n"
        "      try{\n"
        "        var w=document.getElementById('qr');\n"
        "        w.innerHTML='';\n"
        "        new QRCode(w,{text:url,width:180,height:180,\n"
        "          colorDark:'#000000',colorLight:'#ffffff',\n"
        "          correctLevel:QRCode.CorrectLevel.M});\n"
        "      }catch(e){}\n"
        "    };\n"
        "    document.head.appendChild(s);\n"
        "  })();\n"
        "  </script>\n"
        "</body>\n"
        "</html>");

    QByteArray body = html.toUtf8();
    resp->addHeader("Content-Type", "text/html; charset=UTF-8");
    resp->addHeader("Content-Length", QString::number(body.size()).toUtf8());
    resp->addHeader("Access-Control-Allow-Origin", "*");
    resp->addHeader("Access-Control-Allow-Private-Network", "true");
    resp->setStatusCode(qhttp::ESTATUS_OK);
    resp->end(body);
}


void OnMyDiskHttpNode::sendPairInfo(QHttpRequest *req, QHttpResponse *resp)
{
    Q_UNUSED(req)
    QJsonObject info;
    const QString deviceId = m_linkClient.deviceId();
    const QString peerId = m_linkClient.linkId();
    info["linkId"] = !deviceId.isEmpty() ? deviceId : peerId;
    info["peerId"] = peerId;
    info["deviceId"] = deviceId;
    info["name"] = m_rootResource ? m_rootResource->hostName() : QHostInfo::localHostName();
    info["version"] = APP_VERSION;
    
    QByteArray body = QJsonDocument(info).toJson(QJsonDocument::Compact);
    resp->addHeader("Content-Type", "application/json; charset=UTF-8");
    resp->addHeader("Content-Length", QString::number(body.size()).toUtf8());
    resp->addHeader("Access-Control-Allow-Origin", "*");
    resp->addHeader("Access-Control-Allow-Private-Network", "true");
    resp->setStatusCode(qhttp::ESTATUS_OK);
    resp->end(body);
}




