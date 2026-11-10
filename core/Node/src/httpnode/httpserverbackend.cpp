#include "httpserverbackend.h"
#include "version.h"
#include "templateparser.h"
#include "deviceauth.h"
#include "simplecrypt.h"
#include <malloc.h>
#include <QFileInfo>

//#define LSAN_DEBUG

#ifdef LSAN_DEBUG
#include <sanitizer/lsan_interface.h>
#endif

extern int get_rss();


HttpServerBackend::HttpServerBackend(QSettings *settings, QObject *parent) : QObject(parent),
    m_indexFiles({QStringLiteral("server.html"), QStringLiteral("index.html")}),
#ifdef P2P_ENABLED
    m_sharedIndexFiles({QStringLiteral("server.html"), QStringLiteral("index.html")}),
#else
    m_sharedIndexFiles( QStringLiteral("shared.html")),
#endif
    m_autoIndex(false),
	m_maxDiskCacheSize(settings->value(QStringLiteral("maxDiskCacheSize"), DISK_CACHE_SIZE).toULongLong()),
	m_maxRamCacheSize(settings->value(QStringLiteral("maxRamCacheSize"), RAM_CACHE_SIZE).toULongLong()),
    m_fileServer(parent),
	m_authProviders(),
    m_Settings(settings),
    m_sessionProvider(QStringLiteral("OnMyDisk 2.0"), settings->value(QStringLiteral("SearchServerToken"), "").toString()),
    m_pkiHelper(m_Settings, this),
    m_LinkServer(OnMyDiskLinkServer::instance()),
    m_mailer(OnMyDiskMailer::instance()),
    m_userEvents(UserEvents::instance()),
    m_notificationManager(NotificationManager::instance()),
    m_maxFileSize(m_Settings->value(QStringLiteral("Network/maxFileSize"), PACKET_SIZE).toULongLong())
{
    qRegisterMetaType<CallInfo>("CallInfo");
    qRegisterMetaType<SiteInfo>("SiteInfo");

    //Link Server
    m_LinkServer.init(m_Settings, &m_pkiHelper);

    //Icon cache
#ifdef SVAR
#else
#endif

    //Key login mode
    m_keyLogin =  settings->value(QStringLiteral("keyLogin"), false).toBool();
    //Search server enabled
    m_searchEnabled = m_Settings->value(QStringLiteral("SearchServerEnabled"), false).toBool();
    //Public AI node
    m_publicAiNodeID =  m_Settings->value(QStringLiteral("publicAiNode"), "ffc3f840f25dae37d1456f1be0e7e8c6").toString();

    //Default link encryption key
    SimpleCrypt::defaultKey = m_Settings->value(QStringLiteral("linkKey"), 0).toULongLong();

    //Template parser
    TemplateParser::instance().setKeyLogin(m_keyLogin);
    TemplateParser::instance().setSessionAuth(&m_sessionProvider);
    TemplateParser::instance().setStaticVar("FILES_APPTITLE", m_Settings->value(QStringLiteral("AppTitle"), QStringLiteral("On My Disk")));
    TemplateParser::instance().setStaticVar("APPTITLE", m_Settings->value(QStringLiteral("AppTitle"), QStringLiteral("On My Disk")));
    TemplateParser::instance().setStaticVar("CHATS_APPTITLE", m_Settings->value(QStringLiteral("ChatsTitle"), QStringLiteral("On My Chat")));
    TemplateParser::instance().setStaticVar("COMPANYURL", m_Settings->value(QStringLiteral("AppUrl"), QStringLiteral("https://github.com/alexeyvolkoff/onmydisk")));
    TemplateParser::instance().setStaticVar("TERMSURL", m_Settings->value(QStringLiteral("TermsUrl"), QStringLiteral("https://onmydisk.com/terms.html")));
    TemplateParser::instance().setStaticVar("COMPANY", m_Settings->value(QStringLiteral("Company"), QStringLiteral("On My Chat community")));
    TemplateParser::instance().setStaticVar("FILES_SLOGAN", m_Settings->value(QStringLiteral("Slogan"), QStringLiteral("Sharing your files right from your disk")));
    TemplateParser::instance().setStaticVar("SLOGAN", m_Settings->value(QStringLiteral("Slogan"), QStringLiteral("Sharing your files right from your disk")));
    TemplateParser::instance().setStaticVar("MEET_APPTITLE", m_Settings->value(QStringLiteral("MeetTitle"), QStringLiteral("On My Meet")));
    TemplateParser::instance().setStaticVar("MEET_SLOGAN", m_Settings->value(QStringLiteral("MeetSlogan"), QStringLiteral("Secure Meet & Share")));
    TemplateParser::instance().setStaticVar("CHAT_SLOGAN", m_Settings->value(QStringLiteral("ChatSlogan"), QStringLiteral("AI that keeps your data yours")));
    TemplateParser::instance().setStaticVar("VERIFICATION", m_Settings->value(QStringLiteral("Verification"), ""));
    TemplateParser::instance().setStaticVar("WSPORT", m_Settings->value(QStringLiteral("Notifications/Port"), "9988"));
    TemplateParser::instance().setStaticVar("KEYLOGIN", m_keyLogin);
    TemplateParser::instance().setStaticVar("LOGIN_LOGO", "login-logo");
    TemplateParser::instance().setStaticVar("LOGO_OG", "logo-og.jpg");
    TemplateParser::instance().setStaticVar("MEET_LOGIN_LOGO", "meet-login-logo");
    TemplateParser::instance().setStaticVar("MEET_LOGO_OG", "meet-loogo-og.jpg");
#ifdef QT_DEBUG
    QString server =  QStringLiteral("localhost");
    QString serverUrl = QString("http://%1:8080").arg(server);
    QString pushServerUrl =  QStringLiteral("http://localhost:5000");
    TemplateParser::instance().setStaticVar("SERVER",  server);
    TemplateParser::instance().setStaticVar("SERVERURL", serverUrl);
    TemplateParser::instance().setStaticVar("CALLBACKURL", QStringLiteral("http://192.168.122.1:8080"));
    TemplateParser::instance().setStaticVar("SEARCHSERVERENABLED", true);
    TemplateParser::instance().setCondition("SEARCHSERVERENABLED", true);
    //TemplateParser::instance().setStaticVar("AI_SERVER", QString("http://localhost:8080%1").arg(CHAT_PREFIX));
    //TemplateParser::instance().setStaticVar("CHATSERVER", QString("%1:8080").arg(CHAT_DOMAIN));
    TemplateParser::instance().setStaticVar("AI_SERVER", QString("https://onmychat.com%1").arg(CHAT_PREFIX));
    TemplateParser::instance().setStaticVar("CHATSERVER", CHAT_DOMAIN);
    #ifdef P2P_ENABLED
    TemplateParser::instance().setStaticVar("GUN_HUB_URL", QString("https://direct.onmydisk.net:8765"));
    #else
    TemplateParser::instance().setStaticVar("GUN_HUB_URL", QString("https://staging.onmydisk.net:8765"));
    #endif
    TemplateParser::instance().setCondition("DEV", true);
    TemplateParser::instance().setCondition("PROD", false);
#else
    QString server =  m_Settings->value("Server", DEFAULT_CLOUD).toString();
    QString serverUrl = QString("https://%1").arg(server);
    QString pushServerUrl = m_Settings->value(QStringLiteral("PushServer"), "").toString();
    TemplateParser::instance().setStaticVar("SERVER", server);
    TemplateParser::instance().setStaticVar("SERVERURL", serverUrl);
    TemplateParser::instance().setStaticVar("CALLBACKURL", QString("https://%1").arg(server));
    TemplateParser::instance().setStaticVar("SEARCHSERVERENABLED", m_searchEnabled);
    TemplateParser::instance().setCondition("SEARCHSERVERENABLED", m_searchEnabled);
    TemplateParser::instance().setStaticVar("AI_SERVER", QString("https://%1%2").arg(CHAT_DOMAIN, CHAT_PREFIX));
    TemplateParser::instance().setStaticVar("CHATSERVER", m_Settings->value(QStringLiteral("ChatServer"), QString(CHAT_DOMAIN)));
    #ifdef P2P_ENABLED
    TemplateParser::instance().setStaticVar("GUN_HUB_URL", m_Settings->value(QStringLiteral("GunHubUrl"), QString("https://direct.onmydisk.net:8765")));
    #else
    TemplateParser::instance().setStaticVar("GUN_HUB_URL", m_Settings->value(QStringLiteral("GunHubUrl"), QString("https://staging.onmydisk.net:8765")));
    #endif
    TemplateParser::instance().setCondition("PROD", true);
    TemplateParser::instance().setCondition("DEV", false);
#endif
    TemplateParser::instance().setStaticVar("SYSTEMURL", serverUrl);
    TemplateParser::instance().setStaticVar("CONSOLE_URL", m_Settings->value(QStringLiteral("Console"), QString("https://console.onmydisk.net")));
    TemplateParser::instance().setStaticVar("MEETSERVER", m_Settings->value(QStringLiteral("MeetServer"), QString("jitsi.bineon.team")));

    CloudResource::m_maxFileSize = m_maxFileSize;
    m_serverUrl = serverUrl;
    m_clientWrapper = nullptr;

    if (!m_pkiHelper.initPKI())
    {
        qCritical() << "Unable to initialize PKI configuration";
    }
#ifndef QT_DEBUG
#ifdef SVAR
    QString defaultDocRoot = QStringLiteral("/srv/www");
#else
    QString defaultDocRoot = QStringLiteral(":/filemanager");
#endif
    QString docRootPath = m_Settings->value(QStringLiteral("docRoot"), defaultDocRoot).toString();
    QString themeRootPath = m_Settings->value(QStringLiteral("theme"), QString("%1/customtheme").arg(docRootPath)).toString();
#else
    qDebug() << "Using dev  version of frontend from" << QCoreApplication::applicationDirPath() + "/../../../web/dist";
    QString docRootPath =  QCoreApplication::applicationDirPath() + "/../../../web/dist";
    QString themeRootPath = QCoreApplication::applicationDirPath() + "/../../../web/dist/customtheme";
#endif

    TemplateParser::instance().setStaticVar("VERSION", APP_VERSION);

    m_docRoot = new LocalResource(docRootPath, QStringLiteral("/"), QStringLiteral("/"), QStringLiteral(""),  true, nullptr);
    m_themeRoot = new LocalResource(themeRootPath, QStringLiteral("/"), QStringLiteral("/"), QStringLiteral(""),  true, nullptr);

	m_publicRoot = new RootResource(m_docRoot, m_themeRoot);
    m_publicRoot->addCustomThemeItem("colors", themeRootPath + "/colors");
    m_publicRoot->addCustomThemeItem("mainlogo", themeRootPath + "/logo-dark.svg");
    m_publicRoot->addCustomThemeItem("mainlogo-chat", themeRootPath + "/logo-chat.svg");
    m_publicRoot->addCustomThemeItem("mainlogo-chat-dark", themeRootPath + "/logo-chat-dark.svg");
    m_publicRoot->addCustomThemeItem("mainlogo-meet", themeRootPath + "/mainlogo-meet.svg");
    m_publicRoot->addCustomThemeItem("logo-white",  themeRootPath + "/logo.svg");
    m_publicRoot->addCustomThemeItem("logo",  themeRootPath + "/onmydisk.svg");
    m_publicRoot->addCustomThemeItem("logo.css", themeRootPath + "/logo.css");
    m_publicRoot->addCustomThemeItem("meetlogo",  themeRootPath + "/meetlogo.svg");
    m_publicRoot->addCustomThemeItem("meetlogo.css", themeRootPath + "/meetlogo.css");
    m_publicRoot->addCustomThemeItem("loginbackground", themeRootPath + "/canvas1.png");
    m_publicRoot->addCustomThemeItem("favicon.ico", themeRootPath + "/favicon.ico");
    m_publicRoot->addCustomThemeItem("logo-og.jpg", themeRootPath + "/logo-og.jpg");

    m_publicRoot->addCustomRootItem(ADMIN_ROOT, ":/filemanager/admin");
    m_publicRoot->addCustomRootItem(CALL_ROOT, ":/filemanager/call.html");

    m_nonAuthDestinations << "src" << "dist" << "img"  << "qwebchannel" << "theme" << "apps" << "node_modules" << "templates" << "mimetypes"
                          << "home-back-b.jpg" << "logo-white.svg" << "customtheme" << "call/" << "manifest.json"
                          << "favicon.ico"  <<  "signin" <<  "userinfo" <<  "tos" << "tos.html" << "about" << "about.html" << "about-i18n.js" << "robots.txt" << "service-worker.js" << "assets";
#ifdef P2P_ENABLED
    m_nonAuthDestinations << "shared";
#endif

    m_adminDestinations << ADMIN_ROOT;
	m_ignoredFiles << "angular.min.js.map";

    QDir logDir(settings->value(QStringLiteral("LogDir"), QStringLiteral("logs")).toString());
    if (!logDir.exists())
        logDir.mkpath(logDir.absolutePath());

    m_serverString = QStringLiteral("OnMyDisk Server Node (2.0)");

    connect(&m_LinkServer, &OnMyDiskLinkServer::linkDisconnected, this, &HttpServerBackend::linkDisconnected, Qt::QueuedConnection);
    connect(&m_LinkServer, &OnMyDiskLinkServer::linkInactive, this, &HttpServerBackend::linkInactive);
    connect(&m_LinkServer, &OnMyDiskLinkServer::shareUpdated, this, [this](QString linkID, QString user, QString shareName, QHash<QString, QVariant> shareData, QHash<QString, QHash<QString, QVariant>> shareEntryData) {
         shareUpdated(linkID, user, shareName, shareData, shareEntryData);
    });
    connect(&NotificationManager::instance(), &NotificationManager::shareUpdated, this, &HttpServerBackend::shareUpdated);
    connect(&NotificationManager::instance(), &NotificationManager::linkDisconnected, this, &HttpServerBackend::linkDisconnected, Qt::QueuedConnection);


    if (settings->childGroups().contains(QStringLiteral("FileNode"))  && settings->value(QStringLiteral("FileNode/Enabled"), false).toBool())
    {
        if (settings->value(QStringLiteral("FileNode/Network"), false).toBool())
        {
            setupNetwork(&m_fileServer, settings, QString("FileNode"));
            qInfo() << "Starting Web UI on port" << settings->value(QStringLiteral("FileNode/Port"), 8080).toInt();
        }
        else
        {
            QString unixSocket = settings->value(QStringLiteral("FileNode/Socket"), QStringLiteral("/run/onmydisk/node.socket")).toString();
            m_fileServer.listen(unixSocket, m_maxFileSize);
            qInfo() << "Starting Web UI on unix socket:" << unixSocket;
        }

        m_fileServer.setTimeOut(800);
        if (m_fileServer.isListening())
        {
            m_authProviders.append(new BasicAuth(QStringLiteral(ONMYDISK_REALM)));
            m_authProviders.append(new DeviceAuth(QStringLiteral(ONMYDISK_REALM)));
            if (!m_keyLogin)
                m_authProviders.append(new DigestAuth QStringLiteral(ONMYDISK_REALM));
			connect(&m_fileServer, &QHttpServer::newRequest, this, &HttpServerBackend::handleRequest, Qt::DirectConnection);
        }
        else
            qCritical() << "Can't start Web UI";
    }

    m_mailer.init(m_Settings, m_keyLogin);

    connect(&m_LinkServer, &OnMyDiskLinkServer::publicLinkAdded, &m_mailer, &OnMyDiskMailer::publicLinkAdded);

    connect(&m_cleanupTimer, &QTimer::timeout, this, &HttpServerBackend::cleanUp);

    m_notificationManager.init(&m_sessionProvider, pushServerUrl);
    setupNotifications(settings, "Notifications");

    //user events & notifications
    connect(&UserEvents::instance(), &UserEvents::sharedLinkAdded, this, &HttpServerBackend::sharedLinkAdded);
    connect(&UserEvents::instance(), &UserEvents::sharedLinkRemoved, this, &HttpServerBackend::sharedLinkRemoved);
    connect(&UserEvents::instance(), &UserEvents::addShareRequested, this, &HttpServerBackend::shareAddRequested);
    connect(&UserEvents::instance(), &UserEvents::removeShareRequested, this, &HttpServerBackend::shareRemoveRequested);
    connect(&UserEvents::instance(), &UserEvents::publicLinkRemoved, this, &HttpServerBackend::removePublicLink);
    connect(&UserEvents::instance(), &UserEvents::publicLinkRequested, this, &HttpServerBackend::publicLinkRequested);
    connect(&UserEvents::instance(), &UserEvents::siteUpdated, this, &HttpServerBackend::siteUpdated);
    connect(&UserEvents::instance(), &UserEvents::accountCreated, this, &HttpServerBackend::accountCreated);
    connect(&UserEvents::instance(), &UserEvents::fileRemoved, this, &HttpServerBackend::fileRemoved);
    connect(&UserEvents::instance(), &UserEvents::fileUpdated, this, &HttpServerBackend::fileUpdated);
    connect(&UserEvents::instance(), &UserEvents::subscribe, this, &HttpServerBackend::subscribe);
    connect(&UserEvents::instance(), &UserEvents::unsubscribe, this, &HttpServerBackend::unsubscribe);
    connect(&UserEvents::instance(), &UserEvents::siteCreated, this, &HttpServerBackend::siteCreated);

    m_cleanupTimer.setInterval(5000);
    m_statsEnabled = settings->value(QStringLiteral("FileNode/stats"), false).toBool();
    //if (!m_statsEnabled)
    //    m_cleanupTimer.setSingleShot(true);

    m_cleanupTimer.start();

    //Watchdog

    #ifndef QT_DEBUG
    m_watchdog = new Watchdog(pthread_self());
    m_watchdogThread = new QThread(nullptr);
    m_watchdogThread->setObjectName("WatchdogThread");
    connect(m_watchdogThread, &QThread::started, m_watchdog, &Watchdog::start);
    connect(m_watchdogThread, &QThread::finished, m_watchdog, &Watchdog::deleteLater);
    connect(m_watchdogThread, &QThread::finished, m_watchdogThread, &QThread::deleteLater);
    connect(m_watchdog, &Watchdog::ping, this, &HttpServerBackend::watchDogPing, Qt::QueuedConnection);
    m_watchdog->moveToThread(m_watchdogThread);
    m_watchdogThread->start();
    #endif
}

HttpServerBackend::~HttpServerBackend()
{
    shutdown();

    //Protect against concurrent access from LinkThread or Signal handlers
    QMutexLocker lock(&m_mutex);

    Q_FOREACH(RootResource *r, m_rootResources) {
        if (r) {
            r->removeAllChildren();
            delete r;
        }
    }
    m_rootResources.clear();

    Q_FOREACH(RootResource *r, m_sharedRootResources) {
        if (r) {
            r->removeAllChildren();
            delete r;
        }
    }
    m_sharedRootResources.clear();

    Q_FOREACH(IAuthorization *p, m_authProviders) {
        delete p;
    }
    m_authProviders.clear();

    delete m_wsServer;
    m_wsServer = nullptr;

    if (m_clientWrapper) {
        delete m_clientWrapper;
        m_clientWrapper = nullptr;
    }

    if (m_publicRoot) {
        m_publicRoot->removeAllChildren();
	    delete m_publicRoot;
        m_publicRoot = nullptr;
    }

    if (m_docRoot) {
        m_docRoot->removeAllChildren();
        delete m_docRoot;
        m_docRoot = nullptr;
    }
    
    if (m_themeRoot) {
        m_themeRoot->removeAllChildren();
        delete m_themeRoot;
        m_themeRoot = nullptr;
    }


    #ifndef QT_DEBUG
    if (m_watchdogThread) {
        m_watchdogThread->quit();
        m_watchdogThread->wait();
        delete m_watchdogThread; 
    }
    #endif
}

bool HttpServerBackend::isListening()
{
    return m_fileServer.isListening() && m_LinkServer.isListening();
}

void HttpServerBackend::setupNetwork(QHttpServer* server, QSettings *settings, QString settingsSection)
{
    qint16 port = settings->value(QString("%1/Port").arg(settingsSection), 8080).toInt();
    bool useHTTPS = settings->value(QString("%1/Secure").arg(settingsSection), false).toBool();
    bool isLocalOnly = settings->value(QString("%1/Localhost").arg(settingsSection), false).toBool();
    QHostAddress listenOn = isLocalOnly ? QHostAddress::LocalHost : QHostAddress::AnyIPv4;
    if (useHTTPS)
    {        
        if (settings->contains(QString("%1/CA").arg(settingsSection))
                && settings->contains(QString("%1/Cert").arg(settingsSection))
                && settings->contains(QString("%1/pKey").arg(settingsSection)))
        {
            QString caPath = settings->value(QString("%1/CA").arg(settingsSection)).toString();
            QString certPath = settings->value(QString("%1/Cert").arg(settingsSection)).toString();
            QString keyPath = settings->value(QString("%1/pKey").arg(settingsSection)).toString();
            QString keyPass = settings->value(QString("%1/pKeyPass").arg(settingsSection)).toString();
            if (QFileInfo(caPath).isReadable() && QFileInfo(certPath).isReadable() && QFileInfo(keyPath).isReadable())
            {
                m_serverString += ", " + QSslSocket::sslLibraryVersionString();

                server->listen(port, m_maxFileSize, caPath, certPath, keyPath, keyPass);
            }
            else
                qWarning() << QStringLiteral("Non-readable ssl certificate and key..");
        }
        else
            qWarning() << QStringLiteral("SSL settings are broken.");
    }
    else
    {
        server->listen(listenOn, port, m_maxFileSize * (PACKETS_AHEAD + 1));
    }

}

void HttpServerBackend::setupNotifications(QSettings *settings, QString settingsSection)
{

    //WebSocket Server
    if (settings->value(QString("%1/Secure").arg(settingsSection), false).toBool())
    {
        m_wsServer = new QWebSocketServer("OnMyDisk 2.0 Notifications",  QWebSocketServer::SecureMode);
        QSslConfiguration sslConfig;
        if (settings->contains(QString("%1/Cert").arg(settingsSection))  && settings->contains(QString("%1/pKey").arg(settingsSection)))
        {
            QString caPath = settings->value(QString("%1/CA").arg(settingsSection)).toString();
            QString certPath = settings->value(QString("%1/Cert").arg(settingsSection)).toString();
            QString keyPath = settings->value(QString("%1/pKey").arg(settingsSection)).toString();
            QString keyPass = settings->value(QString("%1/pKeyPass").arg(settingsSection)).toString();
            if (QFileInfo(certPath).isReadable() && QFileInfo(keyPath).isReadable())
            {
                QFile keyFile(keyPath);
                if (keyFile.open(QIODevice::ReadOnly))
                {
                    QSslKey privateKey(&keyFile, QSsl::Rsa, QSsl::Pem, QSsl::PrivateKey, keyPass.toLatin1());
                    keyFile.close();
                    sslConfig.setPrivateKey(privateKey);
                }

                QFile caFile(caPath);

                if (caFile.open(QIODevice::ReadOnly))
                {
                    QList<QSslCertificate> CaCerts = QSslCertificate::fromDevice(&caFile);
                    caFile.close();
                    sslConfig.setCaCertificates(CaCerts);
                }

                QFile certFile(certPath);
                if (certFile.open(QIODevice::ReadOnly))
                {
                    QList<QSslCertificate> certs = QSslCertificate::fromDevice(&certFile);
                    certFile.close();
                    if (certs.count() > 0)
                    {
                        sslConfig.setLocalCertificateChain(certs);
                    }
                }

                sslConfig.setPeerVerifyMode(QSslSocket::VerifyNone);
                sslConfig.setProtocol(QSsl::TlsV1_2);

                m_wsServer->setSslConfiguration(sslConfig);
            }
            else
            {
                qWarning() << QStringLiteral("Non-readable SSL configuration for WS notifications.");
            }
        }
        else
        {
            qWarning() << QStringLiteral("Missing SSL configuration for WS notifications.");
        }

    }
    else
        m_wsServer = new QWebSocketServer("OnMyDisk 2.0 Notifications",  QWebSocketServer::NonSecureMode);


    if (! m_wsServer->listen(QHostAddress::AnyIPv4, settings->value(QString("%1/Port").arg(settingsSection), 9988).toInt())) {
        qWarning() << "Failed to start WebSocket server on port" << m_wsServer->serverPort();

    } else {
        // get real port
        qInfo() << "Starting WebSocket notifications on port" << m_wsServer->serverPort();
        m_clientWrapper = new WebSocketClientWrapper(m_wsServer);
        connect(m_clientWrapper, &WebSocketClientWrapper::clientConnected, &m_notificationManager, &NotificationManager::webSocketConnected);
        connect(&m_notificationManager, &NotificationManager::notifierConnected, this, &HttpServerBackend::userConnected);
    }

    // Bridge gateway → browser WebRTC signaling delivery: when a tunnel
    // message targets a web device with a live WebSocket channel (registered
    // via ClientNotifier::setLinkId), push it over QWebChannel instead of
    // waiting for the HTTP polling fallback.
    OnMyDiskLinkServer::instance().setWebPushHandler(
                [this](const QString &linkId, const QString &tunnelId, const QString &kind, const QByteArray &payload) -> bool {
            QSharedPointer<ClientNotifier> notifier = m_notificationManager.getLinkNotifier(linkId);
            if (notifier.isNull())
                return false;

            QJsonObject sig;
            sig["kind"] = kind;
            sig["tunnelID"] = tunnelId;
            sig["payload"] = QString::fromUtf8(payload.toBase64());

            QMetaObject::invokeMethod(notifier.data(), "pushWebSignal", Qt::QueuedConnection,
                                      Q_ARG(QJsonObject, sig));
            return true;
        });

}

//do not lock!
RootResource*  HttpServerBackend::addRootFor(QString login)
{
    RootResource * rootResource = new RootResource(m_publicRoot);
    m_rootResources.insert(login, rootResource);
    return rootResource;
}


//do not lock!
void HttpServerBackend::removeRootFor(QString login)
{
    if (m_rootResources.contains(login))
    {
        RootResource* root = m_rootResources.take(login);
        if (root)
        {
            delete root;
        }
    }
}

//do not lock!
void HttpServerBackend::removeSharedRoot(QString sharedRootId)
{
    if (m_sharedRootResources.contains(sharedRootId))
    {
        RootResource* root = m_sharedRootResources.take(sharedRootId);
        if (root)
        {
            delete root;
        }
    }
}

RootResource *HttpServerBackend::getRootFor(QString login, bool canCreate)
{
    QMutexLocker lock(&m_mutex);
    RootResource* root = nullptr;
    if (m_rootResources.contains(login))
    {
        root = m_rootResources[login];
    } else {
        if (canCreate) {
         if (login.isEmpty())
            qWarning() << "Empty root!!!";
          root = addRootFor(login);
        } else
          root = m_publicRoot;
    }
    return root;
}

RootResource *HttpServerBackend::getSharedRootFor(QString linkId, QString sharedItemPath, QString owner, bool directShare, bool writable, QString version)
{
    RootResource *rootResource;
    QMutexLocker lock(&m_mutex);
    QString sharedRootId = QString("%1:%2").arg(linkId, sharedItemPath);
    if (!m_sharedRootResources.contains(sharedRootId))
    {
        rootResource = new RootResource(m_publicRoot);
        m_sharedRootResources.insert(sharedRootId, rootResource);
        rootResource->setCustomAttribute("version", version);
    } else
        rootResource = m_sharedRootResources[sharedRootId];

    rootResource->addSharedRoot(linkId, sharedItemPath, owner, directShare, writable);
    return rootResource;
}

QString HttpServerBackend::rootPathForLink(QString login, QString linkId)
{
	if (m_rootResources.contains(login))
		return m_rootResources[login]->rootPathForLink(linkId);
	 else
		return "";
}

void HttpServerBackend::shareAddRequested(QString user, QString path)
{
    QMutexLocker lock(&m_mutex);
    RootResource* rootResource = this->getRootFor(user);
    QString linkID = rootResource->linkForPath(path.split("/", Qt::SplitBehaviorFlags::SkipEmptyParts));
    if (! linkID.isEmpty() )
    {
        QPointer<OnMyDiskLink> link = m_LinkServer.getLink(linkID);
        link->addShare(path);
    }
}


void HttpServerBackend::shareRemoveRequested(QString user, QString path)
{
    QMutexLocker lock(&m_mutex);
    RootResource* rootResource = this->getRootFor(user);
    QString linkID = rootResource->linkForPath(path.split("/", Qt::SplitBehaviorFlags::SkipEmptyParts));
    if (! linkID.isEmpty() )
    {
        QPointer<OnMyDiskLink> link = m_LinkServer.getLink(linkID);
        link->removeShare(path);
    }
}

void HttpServerBackend::removePublicLink(QString linkID, QString path, QString owner)
{
    QString sharedRootId = QString("%1:%2").arg(linkID, path);
    RootResource* sharedRoot = nullptr;
    QMutexLocker lock(&m_mutex);
    if (m_sharedRootResources.contains(sharedRootId)) {
       sharedRoot = m_sharedRootResources[sharedRootId];
    }
    if (sharedRoot) {
        sharedRoot->clearSharedRoot();
    }
    qInfo() << "Public link removed" << path;

    // Calculate refName similarly to shareUpdated (with path normalization)
    QString normalizedPath = path;
    foreach(QString shareName, m_sites.keys()) {
        if (m_sites[shareName].linkID == linkID) {
             QString hostPrefix = QString("/%1").arg(shareName);
             if (!normalizedPath.startsWith(hostPrefix)) {
                 if (!normalizedPath.startsWith("/"))
                     normalizedPath.prepend("/");
                 normalizedPath.prepend("/" + shareName);
             }
             break;
        }
    }
    QString refName = SimpleCrypt::stringToHashString(QString("%1:%2").arg(linkID, normalizedPath));
    
    // Remove from Public Root
    m_publicRoot->removeRemoteResourcesByName("public", refName);
    
    // Remove from User's "shared-by-me"
    QString user = owner;
    if (user == "anonymous" || user.isEmpty()) {
        QPointer<OnMyDiskLink> link = m_LinkServer.getLink(linkID);
        if (link) {
            user = link->user();
        } else {
            QSharedPointer<ClientNotifier> notifier = m_notificationManager.getLinkNotifier(linkID);
            if (notifier) {
                user = notifier->getToken();
            }
        }
    }
    RootResource* userRoot = m_rootResources.value(user, nullptr);
    if (userRoot) {
        userRoot->removeRemoteResourcesByName("shared-by-me", refName);
    }

}

bool HttpServerBackend::isAuthRequest(QHttpRequest *req)
{
    return (req->url().path() == "/basicauth");
}

void HttpServerBackend::sendAuthOkResponse(QHttpResponse *resp)
{
    QString answerString = QStringLiteral("200 Authentication success");
    resp->addHeader("WWW-Authenticate", "basic");
    QByteArray body = answerString.toUtf8();
    body.append("\n");
    resp->addHeader("Content-Type", "text/plain; charset=UTF-8");
    resp->addHeader("Content-Length", QString::number(body.size()).toUtf8());
    resp->addHeader("Server", m_serverString.toUtf8());
    resp->setStatusCode( qhttp::ESTATUS_OK);
    //qDebug() << resp->headers();
    resp->end(body);
}

void HttpServerBackend::performRequest(QHttpRequest* req, QHttpResponse* resp, bool isPublic, QString userID,  QString rootOwnerID, RootResource* rootResource)
{

	QStringList indexes = isPublic ?  m_sharedIndexFiles : m_indexFiles;
    QPointer<HttpHandler> handler = new HttpHandler(m_Settings, req, resp, indexes, m_serverString, m_maxFileSize,
									   m_autoIndex, userID, rootOwnerID,  &m_pkiHelper, &m_sessionProvider,
                                       rootResource, m_searchEnabled, m_keyLogin);

    connect(req, &QHttpRequest::end, handler, [ handler]() {
        handler->requestReady(false);
	});

    QPointer<QHttpConnection> conn = req->connection();

    QPointer<QHttpRequest> pReq = req;
    QPointer<QHttpResponse> pResp = resp;
    if (req)
        req->disown();
    if (resp)
        resp->disown();

    connect(conn, &QHttpConnection::disconnected,  handler, [this, handler, conn]() {
		//qDebug() << "Done"  << req->url().toString();
        if (!handler.isNull()) {
            handler->abort();
            handler->requestCompleted();
        }
    });

    connect(handler, &HttpHandler::completed, handler, [this, handler] {
        handler->deleteLater();
    });

    //decrease counters on handler release
    connect(handler, &HttpHandler::completed, this,  [this, handler, pReq, pResp] {
        m_handlers.removeAll(handler);
        if (m_requestCount)
            --m_requestCount;
        if (!pReq.isNull())
            pReq->deleteLater();
        if (!pResp.isNull())
            pResp->deleteLater();
    });

    connect(resp, &QHttpResponse::bytesWritten, handler, [handler]() {
         handler->readNextDataPart();
    }, Qt::QueuedConnection);

    connect(resp, &QHttpResponse::allBytesWritten, handler, [handler]() {
         handler->readNextDataPart();
    }, Qt::QueuedConnection);


#if 0 //QT_DEBUG
    connect(conn->thread(), &QThread::finished,  this, [this]() {
        if (m_exiting) {
            shutdown();
            QTimer::singleShot(3000, this,  [this]() {
                QTimer::singleShot(3000, this,  [this]() {
                        cleanUp();
                        qApp->quit();
                });
            });
        }
     });
#endif

	//qInfo() << "Running:" << req->url().toString();
    QPointer<HttpHandler> h = handler;
    QMetaObject::invokeMethod(this, [this, h] {
        if (!h.isNull()) {
            ++m_requestCount;
            m_handlers.append(h);
        }
    }, Qt::QueuedConnection);
    QMetaObject::invokeMethod(this, "startCleanUp", Qt::QueuedConnection);
    handler->doWork();
}

void HttpServerBackend::shutdown()
{
    qDebug() << "Shutdown";
    m_exiting = true;
    m_wsServer->close();
    m_LinkServer.close();
    m_LinkServer.disconnectLinks();
    m_sessionProvider.closeAll();
    m_fileServer.stopListening();
}



void HttpServerBackend::cleanUp()
{
    if (! m_mutex.tryLock(1000)) {
        qCritical() << "Deadlock suspected";
        return;
    }

    Q_FOREACH(QString rootName, m_rootResources.keys()) {
        if ((m_requestCount == 0) && m_rootResources[rootName] && m_rootResources[rootName]->remoteChildCount() == 0)
        {
            qInfo() << "Removing empty root for" << rootName;
            RootResource *r = m_rootResources.take(rootName);
            r->removeAllChildren();
            delete r;
        }
    }

    if (m_lastRequestCount != m_requestCount || m_requestCount) {
        //m_LinkServer.checkLinks();
        qInfo() <<  "Connected links:" << m_LinkServer.linkClount();
        qInfo() <<  "Root resources:" << m_rootResources.count();
        qInfo() <<  "Shared resources:" << m_sharedRootResources.count();
        qInfo() <<  "Requests:" << m_fileServer.requestCount();
        qInfo() <<  "Handlers:" << m_requestCount;

        Q_FOREACH(QPointer<HttpHandler>handler, m_handlers) {
            if (handler.isNull()) {
                qInfo() <<  "Handler released";
                m_handlers.removeAll(handler);
            } else
                qInfo() <<  "  "  << qPrintable(handler->reqId());
        }
        m_lastRequestCount = m_requestCount;
        malloc_trim(0);
        qInfo() << "RSS: " << get_rss();
        #ifdef LSAN_DEBUG
             __lsan_do_leak_check();
        #endif
    }


    m_mutex.unlock();

}


void HttpServerBackend::startCleanUp()
{
   if (!m_cleanupTimer.isActive())
        m_cleanupTimer.start();
}

void HttpServerBackend::watchDogPing()
{
    m_watchdog->pingResponse();
}


void HttpServerBackend::handleRequest(QPointer<QHttpRequest> req, QPointer<QHttpResponse> resp)
{
	QString userID = QStringLiteral("anonymous");
	QString rootOwnerID  = QStringLiteral("anonymous");
    QString itemPath = req->url().path();
	QString linkID = "";

	if (m_ignoredFiles.contains(QFileInfo(itemPath).fileName()))
	{
		notFound(resp);
		//qDebug() << "Ignored:"  << itemPath;
		return;
	}

	AuthResult r = checkAuth(req, resp,  userID, linkID, rootOwnerID, itemPath);
    qDebug() << "Request:" << req->methodString() << itemPath << "Auth result:" << r << "User:" << userID;


    switch (r) {
        case AUTH_OK_CRAWLING:
            //The gateway serves no robot content (search runs on the nodes)
            break;
        case AUTH_OK_PUBLIC:
        {
            //qInfo() << "Public resource shared by " << userID << "requested";
            performRequest(req, resp, true, userID, rootOwnerID, m_publicRoot);
            break;
        }
        case AUTH_OK_REF:
        {
            //Legacy personal link refs are gone - public root only
            performRequest(req, resp, true, userID, rootOwnerID, m_publicRoot);
            break;
        }
        case AUTH_OK_SHARED:
        {
            //Legacy share links are gone - public root only
            performRequest(req, resp, true, userID, rootOwnerID, m_publicRoot);
            break;
        }
        case AUTH_OK_SEARCHING:
        {
            //Legacy gateway-side search aggregation is gone (search runs on the nodes)
            QJsonObject empty;
            QJsonDocument responseData(empty);
            QByteArray body = responseData.toJson(QJsonDocument::Compact);
            body.append("\n");
            resp->addHeader("Content-Type", "text/json; charset=UTF-8");
            resp->addHeader("Content-Length", QString::number(body.size()).toUtf8());
            resp->addHeader("Server", m_serverString.toUtf8());
            resp->setStatusCode(qhttp::ESTATUS_OK);
            resp->end(body);
            break;
        }
		case AUTH_OK:
        {
            //just checking authentication?
            if (isAuthRequest(req)) {
                sendAuthOkResponse(resp);
                return;
            }

			//if (req->method() != qhttp::EHTTP_PROPFIND)
            
            RootResource* rootResource = this->getRootFor(userID);
            Q_UNUSED(linkID) //legacy link-based serving is gone
            performRequest(req, resp, false, userID, rootOwnerID, rootResource);
            break;
        }
        case AUTH_NOTALLOWED: //We use this response for redirecting to link password page
        {
            //requestAuthKey(req, resp);
            requestAuth(req, resp);
            break;
		}
        case AUTH_OFFLINE: //We use this response for redirecting to link password page
        {
            replyOffline(req, resp);
            break;
        }
        case AUTH_FAIL:
        {
			//qDebug() << "Auth failed:" << req->methodString() << itemPath;
			requestAuth(req, resp);
			break;
        }
    }
    //qInfo() << "End";
}



void HttpServerBackend::linkDisconnected(QString linkID, QString user)
{
    QMutexLocker lock(&m_mutex);
    //remove shared links
    if (m_rootResources.contains(user))
    {
            RootResource* root = m_rootResources[user];
            if (root) {
                QStringList shareNames = root->getSharesByLink(linkID);
                root->removeRemoteResources(linkID);
                //Recompute localAI/localAgent attributes in case the disconnected node provided AI/coding
                recomputeAgentAttributes(root);
                Q_FOREACH(QString shareName, shareNames)
                {
                    //notify web clients
                    m_notificationManager.shareRemoved(user, shareName);
                }
                if (!m_sessionProvider.checkUserIsGuest(user)) {
                    root->removeRemoteResources("shared-by-me", linkID);
                    //remove sites
                    root->removeRemoteResources("sites", linkID);
                }
            } else {
                qCritical() << "Root resource is null for user" << user;
                m_rootResources.remove(user);
            }
    }

    //Get links
    bool linksOk = false;
    //Thread safe db access here!!!
    QHash<QString, QHash<QString, QString>> links;



    if (linksOk && links.count() > 0)
    {
            Q_FOREACH(const QString &id, links.keys())
            {
                QString linkUser =  links[id]["user"];
                QString shareName = QFileInfo(links[id]["path"]).fileName();
                sharedLinkRemoved(linkUser, shareName);
            }
    }

    //Removing shared public roots
    Q_FOREACH(QString key, m_sharedRootResources.keys()) {

        if (key.startsWith(linkID)) {
           RootResource *r = m_sharedRootResources.take(key);
           delete r;
        }
    }

    //Remove search node
    Q_FOREACH(QString key, m_searchNodes.keys()) {
        if (m_searchNodes[key].linkID == linkID) {
           m_searchNodes.remove(key);
        }
    }

    //Remove ai node
    QMutableHashIterator<QString, SiteInfo> ai_it(m_aiNodes);
    while (ai_it.hasNext()) {
        ai_it.next();
        if (ai_it.value().linkID == linkID)
            ai_it.remove();
    }

    //Remove sites
    Q_FOREACH(QString key, m_sites.keys()) {
        if (m_sites[key].linkID == linkID) {
           m_sites.remove(key);
        }
    }

    //remove shared links
    if (!m_sessionProvider.checkUserIsGuest(user)) {
        m_publicRoot->removeRemoteResources("public", linkID);
        m_publicRoot->removeRemoteResources("sites", linkID);
    }

    startCleanUp();
    //qInfo() << "Removed root for " << linkID << userList;
}

void HttpServerBackend::linkInactive(QString linkID, QString user)
{
    Q_UNUSED(linkID)
    Q_UNUSED(user)
    startCleanUp();
}




void HttpServerBackend::userConnected(const QString &user, QSharedPointer<ClientNotifier> notifier)
{
    QMutexLocker lock(&m_mutex);
    if (!notifier->isGuest() && m_rootResources.contains(user))
    {
        notifier->setDeviceCount(m_rootResources[user]->remoteChildCount());
		qInfo() << "User devices:" << m_rootResources[user]->remoteChildCount();
    }
}




void HttpServerBackend::shareUpdated(QString linkID, QString user, QString shareName, QHash<QString, QVariant> shareData, QHash<QString, QHash<QString, QVariant>> shareEntryData, QHash<QString, QHash<QString, QVariant>> shareLinks)
{
    QMutexLocker lock(&m_mutex);
    //check if we need to emit event
    QString addFolder = shareData["addfolder"].toString();
    QString removeFolder = shareData["removefolder"].toString();
    QString sharedWith = shareData["sharedWith"].toString();
    QStringList sharedWithUsers = sharedWith.split(",",  Qt::SplitBehaviorFlags::SkipEmptyParts);
    bool removing = !removeFolder.isEmpty();
    bool adding = !addFolder.isEmpty();
    //check folder parameter. if missing, work with host root
    QString folderName = removing ? removeFolder : addFolder;
    QString sharePath = !folderName.isEmpty() ? shareName + "/" + folderName : shareName ;

    //guest users with PWA or refresh
    if (user == "anonymous" || user.isEmpty()) {
       {
       //Device keys are not tracked by the gateway anymore: resolve via live link/notifier
           QPointer<OnMyDiskLink> link = m_LinkServer.getLink(linkID);
           if (link) {
               user = link->user();
           } else {
               QSharedPointer<ClientNotifier> notifier = m_notificationManager.getLinkNotifier(linkID);
               if (notifier) {
                   user = notifier->getToken();
               } else {
                   user = linkID;
               }
           }
       }
    }

    QString deviceOwner = user;
    QStringList users; users << user; users << sharedWithUsers;

    foreach (const QString& user,  users) {
        //update root
        RootResource* userRoot = getRootFor(user, true);
        bool removing = !removeFolder.isEmpty();
        adding = adding || !userRoot->hasRemoteShare(shareName);

        if (!removing) {
            userRoot->updateRemoteResource(shareName, linkID, shareData, shareEntryData);
        }

        //set attrs for root
        if (!m_publicAiNodeID.isEmpty() || (shareData.contains("localAI") && shareData["localAI"].toBool())) {
            userRoot->setCustomAttribute("localAI", true);
        }

        if (shareData.contains("localSearch") && shareData["localSearch"].toBool()) {
            userRoot->setCustomAttribute("localSearch", true);
        }

        if (user == deviceOwner) {
            bool hasAgent = false;
            QString agentNodeName = "";
            QStringList shares = userRoot->remoteEntryList();
            foreach (const QString& share, shares) {
                CloudResource* r = userRoot->getChild(QStringList { share }, false);
                if (r) {
                    QHash<QString, QVariant> rAttrs = r->getCustomAttributes();
                    if (rAttrs.contains("localAI") && rAttrs["localAI"].toBool()) {
                        hasAgent = true;
                        agentNodeName = share;
                        break;
                    }
                }
            }
            if (hasAgent) {
                userRoot->setCustomAttribute("localAgent", true);
                userRoot->setCustomAttribute("agentNode", agentNodeName);
            } else {
                userRoot->setCustomAttribute("localAgent", false);
                userRoot->setCustomAttribute("agentNode", "");
            }
        }

        int newDeviceCount = userRoot->remoteChildCount();

        //safe not to lock, as we do not remove notifiers in other threads
        m_notificationManager.setDeviceCount(user, newDeviceCount);
        if (removing)
            m_notificationManager.shareRemoved(user, sharePath);
        else if (adding)
        {
            qInfo() << "User devices updated:" << newDeviceCount;
            m_notificationManager.shareAdded(user, sharePath);
        }
    }

    //Update mounted links for other users of this share
    bool linksOk = false;
    //Thread safe db access here!!!
    QHash<QString, QHash<QString, QString>> links;



    if (linksOk && links.count() > 0)
    {
        Q_FOREACH(const QString &id, links.keys())
        {
            QString linkUser =  links[id]["user"];
            if (linkUser.isEmpty()) continue;

            QString path = links[id]["path"];
            while (path.endsWith("/") && path.length() > 1) {
                path.chop(1);
            }
            QString shareName = QFileInfo(path).fileName();
            if (shareName.isEmpty()) continue;
            bool isDir = true;
            Q_FOREACH(const QString& linkKey, shareLinks.keys()) {
                if (shareLinks[linkKey]["itemPath"].toString() == path) {
                    isDir = shareLinks[linkKey].value("isDir", true).toBool();
                    break;
                }
            }

            if (!removing) // adding shared link moun point
                sharedLinkAdded(linkUser, linkID, shareName, user, links[id]["rights"] == "writable", isDir, path);
            else
                sharedLinkRemoved(linkUser, shareName);
        }
   }


    //update index after some time
    bool userSearchEnabled = shareData.contains("localSearch") && shareData["localSearch"].toBool();
    bool userAiEnabled = shareData.contains("localAI") && shareData["localAI"].toBool();
    qInfo() << "Share updated for" << user << "Search:" << userSearchEnabled << "AI:" << userAiEnabled << "Adding:" << adding;
    if (adding && userSearchEnabled)
    {
        //Store search node info
        SiteInfo searchNode;
        searchNode.linkID = linkID;
        searchNode.owner = user;
        searchNode.shareName = shareName;
        searchNode.users = sharedWithUsers;

        //Subscriptions live on the legacy gateway during the GunDB migration
        m_searchNodes[user] = searchNode;

        qInfo() << "Adding search node" << linkID << "for" << user;
    }

    if (adding && userAiEnabled)
    {
        //Store AI node info
        SiteInfo aiNode;
        aiNode.linkID = linkID;
        aiNode.owner = user;
        aiNode.shareName = shareName;
        QString shareTo = shareData["shareTo"].toString();
        aiNode.users = shareTo.split(",",  Qt::SplitBehaviorFlags::SkipEmptyParts);

        // Remove existing entry for this specific LinkID to avoid duplicates in MultiHash
        QMutableHashIterator<QString, SiteInfo> ai_it(m_aiNodes);
        while (ai_it.hasNext()) {
            ai_it.next();
            if (ai_it.value().linkID == aiNode.linkID)
                ai_it.remove();
        }

        //Subscriptions live on the legacy gateway during the GunDB migration
        m_aiNodes.insert(user, aiNode);

        qInfo() << "Adding AI node" << linkID << "for" << user;
    }
    //share owner's root
    RootResource* userRoot = getRootFor(user, true);
    //Add Sites to public and owner's roots
    if (shareData.contains("sites") && shareData.contains("sitesRoot"))
    {
        QString sitesRoot = shareData["sitesRoot"].toString();
        QVariantHash sites = shareData["sites"].toHash();
        //remove removed sites
        foreach(const QString& key, m_sites.keys()) {
            if (m_sites[key].linkID == linkID && !sites.contains(key) ) {
                m_sites.remove(key);
                m_publicRoot->removeRemoteResourcesByName("sites", key);
                userRoot->removeRemoteResourcesByName("sites", key);
            }
        }

        foreach(const QString& key, sites.keys()) {
            if (!m_sites.contains(key) && !CloudResource::isSkipped(key)) {
                SiteInfo site;
                site.linkID = linkID;
                site.shareName = shareName;
                site.name = key;
                site.owner = user;
                // Domain linking
                QHash<QString, QVariant> attrs = sites[key].toHash();
                site.domain = attrs.contains("domain") ? attrs["domain"].toString() : "";
                //Shared site
                QString shareTo = attrs.contains("shareTo") ?attrs["shareTo"].toString() : "";

                qInfo() << "Adding site:"  << key;
                m_sites[key] = site;

                attrs["isSite"] = true;
                attrs["siteName"] = key;
                attrs["siteUrl"] = site.domain.isEmpty() ? QString("/sites/%1").arg(key) :  QString("https://%1").arg(site.domain);
                QHash<QString, QVariant> siteData;
                siteData["isWritable"] = true;
                siteData["isReadable"] = true;
                siteData["isDir"] = false;
                siteData["isFile"] = true;
                siteData["exists"] = true;
                siteData["owner"] = user;
                siteData["contentType"] = "text/html";
                siteData["customAttributes"] = attrs;
                siteData["link"] = QString("/sites/%1").arg(key);
                userRoot->addRemoteReference("sites", key, linkID, QString("/%1/%2").arg(sitesRoot, key), siteData);
                //disable writable for public
                siteData["isWritable"] = false;
                m_publicRoot->addRemoteReference("sites", key, linkID, QString("/%1/%2").arg(sitesRoot, key), siteData);
                if (!shareTo.isEmpty()) {
                    sharedSiteAdded(shareTo, linkID, key, user);
                }
            }
        }
    }

    if (!shareLinks.isEmpty()) {
          Q_FOREACH(QString linkKey, shareLinks.keys()) {
              QHash<QString, QVariant> linkData = shareLinks[linkKey];
              QString itemPath;
              if (linkData.contains("itemPath")) {
                 itemPath = linkData["itemPath"].toString();
              }

              // Robust name determination: filename -> itemName -> basename(itemPath) -> linkKey
              QString name = linkData.value("filename", linkData.value("itemName", QFileInfo(itemPath).fileName())).toString();
              if (name.isEmpty()) name = linkKey;

              // Use linkKey as the node name to ensure unshare (by ID) works correctly.
              // Pass the resolved name in linkData for display purposes via customAttributes.
              QVariantHash customAttrs = linkData.value("customAttributes").toHash();
              customAttrs["filename"] = name;
              customAttrs["itemName"] = name;
              customAttrs["label"] = name;
              linkData["customAttributes"] = customAttrs; 
              linkData["fileName"] = name;

              linkData["isReadable"] = true;
              linkData["exists"] = true;

              // Assuming 'attributes' in the instruction refers to 'linkData'
              m_publicRoot->addRemoteReference("public", linkKey, linkID, itemPath, linkData);
              userRoot->addRemoteReference("shared-by-me", linkKey, linkID, itemPath, linkData);
          }
    }

    startCleanUp();
}


void HttpServerBackend::createShareForUser(RootResource* userRoot, QString linkID , QString shareName, QString remotePath, QString owner, QString version, bool writable, QVariantHash attrs) {

    QHash<QString, QVariant> linkData;
    linkData["overrideWritable"] = true;
    linkData["isWritable"] = writable;
    linkData["isReadable"] = true;
    linkData["sharedLinkId"] = linkID;
    linkData["isDir"] = attrs.contains("isDir") ? attrs["isDir"].toBool() : true;
    linkData["isFile"] = ! linkData["isDir"].toBool();
    linkData["exists"] = true;
    linkData["version"] = version;
    linkData["owner"] = owner;
    linkData["mounted"] = true;
    linkData["customAttributes"] = attrs;

    userRoot->addLinkedResource(shareName, linkID, remotePath, linkData );

}

void HttpServerBackend::sharedSiteAdded(QString user, QString linkID, QString siteName, QString owner)
{
    QPointer<OnMyDiskLink> link = m_LinkServer.getLink(linkID);
    if (!link.isNull() && m_sites.contains(siteName))
    {
        RootResource* userRoot = getRootFor(user, true);
        int deviceCount = userRoot->remoteChildCount();

        if (!userRoot->hasRemoteShare(siteName)) {
            SiteInfo site = m_sites[siteName];
            QString rootPath = link->shareName();
            QString sharedItemPath = QString("/%1/sites/%2").arg(rootPath,  siteName);
            QVariantHash attrs;
            attrs.insert("isSite", true);
            attrs["isSite"] = true;
            attrs["siteName"] = siteName;
            attrs["siteUrl"] = site.domain.isEmpty() ? QString("/sites/%1").arg(siteName) : QString("https://%1").arg(site.domain);

            qInfo() << "Mounting site" << siteName << "for" << user;
            createShareForUser(userRoot, linkID, siteName, sharedItemPath, owner, link->linkVersion(), true, attrs);


        }

        int newDeviceCount = userRoot->remoteChildCount();
        if (newDeviceCount != deviceCount)
        {
            //fire event in JavaScript
            m_notificationManager.setDeviceCount(user, newDeviceCount);
            m_notificationManager.shareAdded(user, siteName);
        }
    }

}

void HttpServerBackend::sharedLinkAdded(QString user,  QString linkID, QString shareName,  QString owner,  bool writable, bool isDir, QString itemPath)
{
    QPointer<OnMyDiskLink> link = m_LinkServer.getLink(linkID);
    if (!link.isNull())
    {
        RootResource* userRoot = getRootFor(user, true);
        int deviceCount = userRoot->remoteChildCount();

        if (userRoot->hasRemoteShare(shareName)) {
            userRoot->removeRemoteShare(shareName);
        }
        {
            QString rootPath = link->shareName();
            QString sharedItemPath;
            if (!itemPath.isEmpty() && itemPath != shareName) {
                if (itemPath.startsWith("/" + rootPath + "/")) {
                    sharedItemPath = itemPath;
                } else if (itemPath.startsWith(rootPath + "/")) {
                    sharedItemPath = "/" + itemPath;
                } else {
                    if (!itemPath.startsWith("/")) itemPath.prepend("/");
                    sharedItemPath = QString("/%1%2").arg(rootPath, itemPath);
                }
            } else {
                QString foundPath = "";
                if (!foundPath.isEmpty()) {
                    if (foundPath.startsWith("/" + rootPath + "/")) {
                        sharedItemPath = foundPath;
                    } else if (foundPath.startsWith(rootPath + "/")) {
                        sharedItemPath = "/" + foundPath;
                    } else {
                        if (!foundPath.startsWith("/")) foundPath.prepend("/");
                        sharedItemPath = QString("/%1%2").arg(rootPath, foundPath);
                    }
                } else {
                    sharedItemPath = QString("/%1/shared/%2").arg(rootPath,  shareName);
                }
            }

            qInfo() << "Mounting" << shareName << "for" << user << "writable:" << writable;
            QVariantHash attrs;
            attrs["isDir"] = isDir;
            createShareForUser(userRoot, linkID, shareName, sharedItemPath, owner, link->linkVersion(), writable, attrs);


        }

        int newDeviceCount = userRoot->remoteChildCount();
        if (newDeviceCount != deviceCount)
        {
            //fire event in JavaScript
            m_notificationManager.setDeviceCount(user, newDeviceCount);
            m_notificationManager.shareAdded(user, shareName);
        }
    }
}

void HttpServerBackend::recomputeAgentAttributes(RootResource *userRoot, bool updateAgent)
{
    if (!userRoot) return;
    bool hasAI = !m_publicAiNodeID.isEmpty();
    bool hasAgent = false;
    QString agentNodeName = "";
    QStringList shares = userRoot->remoteEntryList();
    foreach (const QString& share, shares)
    {
        CloudResource* r = userRoot->getChild(QStringList { share }, false);
        if (r)
        {
            QHash<QString, QVariant> rAttrs = r->getCustomAttributes();
            if (rAttrs.contains("localAI") && rAttrs["localAI"].toBool())
            {
                hasAI = true;
                hasAgent = true;
                agentNodeName = share;
                break;
            }
        }
    }
    userRoot->setCustomAttribute("localAI", hasAI);
    if (updateAgent)
    {
        if (hasAgent)
        {
            userRoot->setCustomAttribute("localAgent", true);
            userRoot->setCustomAttribute("agentNode", agentNodeName);
        }
        else
        {
            userRoot->setCustomAttribute("localAgent", false);
            userRoot->setCustomAttribute("agentNode", "");
        }
    }
}

void HttpServerBackend::sharedLinkRemoved(QString user, QString shareName)
{
    if (m_rootResources.contains(user))
    {
        RootResource* userRoot = m_rootResources[user];
        userRoot->removeRemoteShare(shareName);
        //Recompute localAI attribute for users who had the removed AI/coding share mounted
        recomputeAgentAttributes(userRoot, false);
        int newDeviceCount = userRoot->remoteChildCount();
        m_notificationManager.setDeviceCount(user, newDeviceCount);
        m_notificationManager.shareRemoved(user, shareName);
    }
}

void HttpServerBackend::fileMoved(const QString &owner, const QString &user, const QString &origPath, const QString &newPath)
{
    fileRemoved(owner, user, origPath);
    fileUpdated(owner, user, newPath);
}

void HttpServerBackend::fileUpdated(const QString &owner, const QString &user, const QString &filePath)
{
    Q_UNUSED(owner)
    QMutexLocker lock(&m_mutex);

    //update mounted shares
    Q_FOREACH(const QString &shareUser, m_rootResources.keys()) {
        RootResource *r =  m_rootResources[shareUser];
        QString mountedPath = r->findSharedPath(filePath);
        if (!mountedPath.isEmpty()) { //removing something mounted
            m_notificationManager.fileUpdated(shareUser, user, mountedPath);
        }
    }
    //Update for guests
    m_notificationManager.fileUpdatedForGuests(user, filePath);

}

void HttpServerBackend::fileRemoved(const QString &owner, const QString &user, const QString &filePath)
{
    Q_UNUSED(owner)
    QMutexLocker lock(&m_mutex);

    //check if we remove a shared root
    Q_FOREACH(const QString &key, m_sharedRootResources.keys()) {
        RootResource *r =  m_sharedRootResources[key];
        if (r->remotePath() == filePath) {
              m_sharedRootResources.remove(key);
              delete r;
        }
    }

    //update mounted shares
    Q_FOREACH(const QString &shareUser, m_rootResources.keys()) {
        RootResource *r =  m_rootResources[shareUser];
        QString mountedPath = r->findSharedPath(filePath);
        if (!mountedPath.isEmpty()) { //removing something mounted
            m_notificationManager.fileRemoved(shareUser, user, mountedPath);
        }
    }
    //Site removed?
    QStringList pathList = filePath.split("/", Qt::SplitBehaviorFlags::SkipEmptyParts);
    if (pathList.count() == 3 && pathList[1] == "sites")
    {
        QString siteName = pathList[2];
        if (m_sites.contains(siteName)) {
            SiteInfo site = m_sites[siteName];
            QPointer<OnMyDiskLink> link = m_LinkServer.getLink(site.linkID);
            if (!link.isNull()) {
                qInfo() << "Removing site" << site.name;
                QVariantHash params;
                params.insert("siteRemoved", site.name);
                params.insert("type", site.type);
                link->invoke("restartsite", params);
            }
        }
    }

    //Remove for guests
    m_notificationManager.fileRemovedForGuests(user, filePath);

    //Notify sync engine about file removals
    }

void HttpServerBackend::subscribe(const QString &userName, const QString &user, const QString &site)
{
    QMutexLocker lock(&m_mutex);
    qInfo() << "Subscribing" << userName << "for" << (!user.isEmpty() ? ("user " + user) : !site.isEmpty() ? ("site: " + site) : "") ;

    //Subscriptions live on the legacy gateway during the GunDB migration;
    //only the in-memory search node subscriptions are maintained here.
    if (! user.isEmpty() && m_searchNodes.contains(userName) && !m_searchNodes[userName].subscriptions.contains(user))
        m_searchNodes[userName].subscriptions.append(user);

    if (! site.isEmpty()) {
        if (!m_sites.contains(site)) {
            qCritical() << "Site not found" << site;
            return;
        };
        if (m_searchNodes.contains(userName) && !m_searchNodes[userName].subscriptions.contains(site))
            m_searchNodes[userName].subscriptions.append(site);
    }
}

void HttpServerBackend::unsubscribe(const QString &userName, const QString &user, const QString &site)
{
    QMutexLocker lock(&m_mutex);
    //Subscriptions live on the legacy gateway during the GunDB migration
    if (! user.isEmpty() && m_searchNodes.contains(userName))
        m_searchNodes[userName].subscriptions.removeAll(user);

    if (! site.isEmpty() && m_searchNodes.contains(userName))
        m_searchNodes[userName].subscriptions.removeAll(site);
}

void HttpServerBackend::siteCreated(QString owner, SiteInfo site)
{
    QMutexLocker lock(&m_mutex);
    qInfo() << "Site created:" << site.name << "owner:" << owner << "type:" << site.type << site.linkID;
    if (site.type != "plain")   {
        QPointer<OnMyDiskLink> link = m_LinkServer.getLink(site.linkID);
        if (!link.isNull()) {
            QVariantHash params;
            params.insert("siteAdded", site.name);
            params.insert("type", site.type);
            params.insert("template", site.siteTemplate);
            params.insert("rebuild", true);
            link->invoke("restartsite", params);
        }
    }
}


void HttpServerBackend::siteUpdated(QString owner, QString siteName, bool rebuild)
{
    Q_UNUSED(owner)
    QMutexLocker lock(&m_mutex);
    qInfo() << "Site updated:" << siteName;
    if ( m_sites.contains(siteName)) {
        SiteInfo site = m_sites[siteName];
        QPointer<OnMyDiskLink> link = m_LinkServer.getLink(site.linkID);
        if (!link.isNull()) {
            QVariantHash params;
            params.insert("siteUpdated", site.name);
            params.insert("type", site.type);
            params.insert("rebuild", rebuild);
            link->invoke("restartsite", params);
        }
    }

}

void HttpServerBackend::publicLinkRequested(QString linkID, QString path, QString owner, QVariantHash linkData)
{
    QMutexLocker lock(&m_mutex);
    qInfo() << "Public link requested" << linkID << path << owner;
    QString user = owner;
    if (owner == "anonymous") {
        QPointer<OnMyDiskLink> link = m_LinkServer.getLink(linkID);
        if (link) {
            user = link->user();
            qInfo() << "Resolved anonymous user to" << user << "from link" << linkID;
        } else {
            QSharedPointer<ClientNotifier> notifier = m_notificationManager.getLinkNotifier(linkID);
            if (notifier) {
                user = notifier->getToken();
                qInfo() << "Resolved anonymous user to session" << user << "from notifier" << linkID;
            } else {
                qWarning() << "Link not found for anonymous request:" << linkID;
            }
        }
    }


    RootResource* root = nullptr;
    if (m_rootResources.contains(user)) {
        root = m_rootResources[user];
    } else {
        qInfo() << "Creating root for" << user;
        root = addRootFor(user);
    }

    //add link to "shared-by-me" list
    QString name = QFileInfo(path).fileName();
    qInfo() << "Adding to shared-by-me" << name << "for" << user;
    //make sure it is readable at least
    linkData["isReadable"] = true;
    linkData["exists"] = true;
    root->addRemoteReference("shared-by-me", name, linkID, path, linkData);

    getSharedRootFor(linkID, path, user, true, linkData["writable"].toBool(), linkData["version"].toString());
    QString link =  linkData["link"].toString();
    qInfo() << "Public link added" << linkData["link"].toString();

}

void HttpServerBackend::accountCreated(QString user)
{
    //Account lifecycle is handled by the legacy gateway during the GunDB migration
    Q_UNUSED(user)
}

AuthResult HttpServerBackend::checkAuth(QHttpRequest *req, QHttpResponse *resp, QString &userID, QString &linkID, QString &rootOwnerID, QString &itemPath)
{

    //qDebug() << "Check auth:" << req->methodString() << req->url().path(QUrl::PrettyDecoded);
    if (req->method() == qhttp::EHTTP_NOTIFY || req->method() == qhttp::EHTTP_OPTIONS)
        return AUTH_OK;

    // WebRTC signaling transport stays open (guests and old cached frontends
    // use it), but every handler verifies session-based ownership itself.
    // Validate the session opportunistically to fill userID: real users
    // resolve to their user name, guests to their session ID - this is the
    // owner key the signaling handlers check against.
    const bool signalingPath = req->urlPath().size() >= 2 && req->urlPath().at(0) == QStringLiteral("api") && req->urlPath().at(1) == QStringLiteral("signaling");
    const bool devicesPath = req->urlPath().size() >= 2 && req->urlPath().at(0) == QStringLiteral("api") && (req->urlPath().at(1) == QStringLiteral("devices") || req->urlPath().at(1) == QStringLiteral("my-nearby") || req->urlPath().at(1) == QStringLiteral("ai-node"));
    const bool deviceRegisterPath = req->urlPath().size() >= 3 && req->urlPath().at(0) == QStringLiteral("api") && req->urlPath().at(1) == QStringLiteral("device") && req->urlPath().at(2) == QStringLiteral("register");
    if (signalingPath || devicesPath || deviceRegisterPath)
    {
        m_sessionProvider.validate(req, resp, userID, rootOwnerID, linkID, itemPath);
        return AUTH_OK_PUBLIC;
    }

    if (req->urlPath().size() > 0 && m_nonAuthDestinations.contains(req->urlPath().at(0)) )        
    {
        return AUTH_OK_PUBLIC;
    }

	//check cookies
    AuthResult sessionAuthResult = m_sessionProvider.validate(req, resp, userID, rootOwnerID, linkID, itemPath);

	if (sessionAuthResult != AUTH_FAIL)
		return sessionAuthResult;


    if (m_authProviders.count() == 0)
        return AUTH_OK;

    if (req->headers().contains("authorization"))
    {
        QString authValue(req->headers()["authorization"]);
        for (int i = 0; i < m_authProviders.length(); i++)
        {
			QString methodName = m_authProviders.at(i)->methodName().toLower();
            if (authValue.toLower().startsWith(methodName))
            {
				AuthResult result = m_authProviders.at(i)->validate(req, resp, userID, rootOwnerID, itemPath);
				//qDebug() << methodName << result;
				if (result != AUTH_OK)
					qWarning() << "Auth failed for" << req->methodString() << req->url().path(QUrl::PrettyDecoded);
				return result;
            }
        }
	}
    return AUTH_FAIL;
}



void HttpServerBackend::requestAuth(QHttpRequest *req, QHttpResponse *resp)
{
	if (req->method() == qhttp::EHTTP_PROPFIND || isAuthRequest(req))
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
        error["error"] = loginUrl.toString();

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
    else
    {
        QString answerString = QStringLiteral("401 Unauthorized");
        QByteArray body = answerString.toUtf8();
        body.append("\n");
        resp->addHeader("Content-Type", "text/plain; charset=UTF-8");
        resp->addHeader("Content-Length", QString::number(body.size()).toUtf8());
        resp->addHeader("Server", m_serverString.toUtf8());
        resp->setStatusCode(qhttp::ESTATUS_UNAUTHORIZED);
        resp->end(body);
    }
}

void HttpServerBackend::replyOffline(QHttpRequest *req, QHttpResponse *resp)
{
    Q_UNUSED(req)
    QFile loginPageFile(":/filemanager/offline.html");
    QByteArray loginPageBytes;
    if (loginPageFile.open(QIODevice::ReadOnly))
    {
        QString loginSource = QString::fromUtf8(loginPageFile.readAll());
        Template loginTemplate(loginSource, "login");
        loginPageBytes = TemplateParser::instance().parse("", loginTemplate);
        loginPageFile.close();
    }
#ifdef SERVER
    // SVAR/OMD 3.0 build has no packaged offline.html (legacy filemanager qrc removed);
    // fall back to the docRoot copy served with the template parser.
    if (loginPageBytes.isEmpty())
    {
       QFile pageFile(m_docRoot->path() + "/offline.html");
       if (pageFile.open(QIODevice::ReadOnly))
       {
            QString loginSource = QString::fromUtf8(pageFile.readAll());
            Template loginTemplate(loginSource, "login");
            loginPageBytes = TemplateParser::instance().parse("", loginTemplate);
            pageFile.close();
       }
    }
#endif
    loginPageBytes.append("\n");
    resp->addHeader("Content-Type", "text/html; charset=UTF-8");
    resp->addHeader("Content-Length", QString::number(loginPageBytes.size()).toUtf8());
    resp->addHeader("Server", m_serverString.toUtf8());
    resp->setStatusCode( qhttp::ESTATUS_NOT_FOUND);
    resp->end(loginPageBytes);
}


void HttpServerBackend::notFound( QHttpResponse *resp)
{
	QString answerString = QStringLiteral("404 Not found");
	QByteArray body = answerString.toUtf8();

	body.append("\n");
    resp->addHeader("Content-Type", "text/plain; charset=UTF-8");
	resp->addHeader("Content-Length", QString::number(body.size()).toUtf8());
	resp->addHeader("Server", m_serverString.toUtf8());
	resp->setStatusCode( qhttp::ESTATUS_NOT_FOUND);
	//qDebug() << resp->headers();
	resp->end(body);
}

void HttpServerBackend::notAllowed(QHttpResponse *resp)
{
    QString answerString = QStringLiteral("405 Method not allowed");
	QByteArray body = answerString.toUtf8();
	body.append("\n");
    resp->addHeader("Content-Type", "text/plain; charset=UTF-8");
	resp->addHeader("Content-Length", QString::number(body.size()).toUtf8());
	resp->addHeader("Server", m_serverString.toUtf8());
	resp->setStatusCode(qhttp::ESTATUS_METHOD_NOT_ALLOWED);
	resp->end(body);
}
