#include "sessionauth.h"
#include "httphandler.h"
#include "simplecrypt.h"
#if !defined(P2P_ENABLED) && !defined(NO_WEB_UI)
#include "templateparser.h"
#endif
#ifdef SERVER
#include "onmydisklinkserver.h"
#endif
#ifdef SERVER
#include "notificationmanager.h"
#endif
#include "omdsettings.h"
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QEventLoop>
#include <QSettings>
#include <QFileInfo>
#include <QCoreApplication>

#ifdef Q_OS_LINUX
#include <shadow.h>
#include <unistd.h>
#include <crypt.h>
#include <string.h>

#include <QProcess>

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
#include <QProcess>
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

SessionAuth::SessionAuth(QString realm, QString crawlerToken)
{
    m_Realm = realm;
    m_crawlerToken = crawlerToken;
    QSettings settings("/etc/onmydisk/server.conf", QSettings::IniFormat);
    m_consoleUrl = settings.value("ConsoleUrl", "https://console.onmydisk.net").toString();
}

SessionAuth::~SessionAuth()
{
    closeAll();
}

QString SessionAuth::methodName()
{
    return QStringLiteral("Session");
}

QString SessionAuth::headerString(QHttpRequest *httpRequest)
{
    Q_UNUSED(httpRequest)
    return QString("Session realm=\"%1\"").arg(m_Realm);
}



void SessionAuth::updateGuestAccess(QString itemPath, QString accessRights)
{
    QMutexLocker lock(&m_mutex);
    Q_FOREACH(ClientSession* s, m_Sessions) {
        if (s->getVar("Item", "").toString() == itemPath)
            s->setVar("rights", accessRights);
    }
}

void SessionAuth::removeGuestAccess(QString itemPath)
{
    QMutexLocker lock(&m_mutex);
    Q_FOREACH(ClientSession* s, m_Sessions) {
        if (s->getVar("Item", "").toString() == itemPath) {
            s->setVar("directShare", false);
        }
    }
}

void SessionAuth::removeGuestAccessByLink(QString linkId)
{
    QMutexLocker lock(&m_mutex);
    Q_FOREACH(ClientSession* s, m_Sessions) {
        if (s->getVar("Link", "").toString() == linkId) {
            s->setVar("directShare", false);
        }
    }
}

void SessionAuth::sharePathInCall(QString callID, QString LinkID, QString itemPath)
{
    QMutexLocker lock(&m_mutex);
    Q_FOREACH(ClientSession* s, m_Sessions) {
        if (s->getVar("CallId", "").toString() == callID) {
            QHash<QString, QVariant> values;
            values["Item"] = itemPath;
            values["Link"] = LinkID;
            values["directShare"] = true;
            s->setSessionVariables(values);
        }
    }
}

void SessionAuth::unsharePathInCall(QString callID)
{
    QMutexLocker lock(&m_mutex);
    Q_FOREACH(ClientSession* s, m_Sessions) {
        if (s->getVar("CallId", "").toString() == callID) {
            QHash<QString, QVariant> values;
            values["Item"] = "";
            values["Link"] = "";
            values["directShare"] = false;
            s->setSessionVariables(values);
        }
    }
}


void SessionAuth::updateUserRights(QString userName, QJsonObject rights)
{
#ifdef QT_DEBUG
    rights["status"] = "Unlimited";
    rights["token_balance"] = 1000.0;
    rights["ai_chat"] = true;
    rights["seats_allowed"] = 10;
#endif
    QMutexLocker lock(&m_mutex);
    Q_FOREACH(ClientSession* s, m_Sessions) {
        if (s->userName() == userName) {
            QHash<QString, QVariant> sessionVars = s->getSessionVariables();
            
            // Update rights
            if (!rights.isEmpty()) {
                bool collab = (rights["token_balance"].toDouble() > 0.0) || (rights["seats_allowed"].toInt() > 1);

                sessionVars["collab"] = collab;
                sessionVars["ai_chat"] = rights["ai_chat"].toBool();
                sessionVars["token_balance"] = rights["token_balance"].toDouble();
                sessionVars["status"] = rights["status"].toString();

                if (collab) {
                    sessionVars["group"] = rights["group_name"].toString();
                    if (rights["is_owner"].toBool()) {
                        sessionVars["isGroupAdmin"] = true;
                    }
                }
            
             s->setSessionVariables(sessionVars);
        }
    }
}
}

void SessionAuth::closeAll()
{
    Q_FOREACH(ClientSession* s, m_Sessions)
    {
           delete  s;
    }
    m_Sessions.clear();
}

AuthResult SessionAuth::validate(QHttpRequest *httpRequest, QHttpResponse *httpResponse, QString &userID, QString &rootOwnerID, QString &linkID, QString &itemPath)
{
    Q_UNUSED(httpResponse)
    
    // Allow OPTIONS pre-flight requests
    if (httpRequest->method() == qhttp::EHTTP_OPTIONS) {
         return AUTH_OK;
    }

    // Webhooks bypass normal session auth
    if (itemPath.startsWith("/webhooks/")) {
         return AUTH_OK;
    }

    // Key submission bypasses normal session auth
    if (httpRequest->method() == qhttp::EHTTP_POST && httpRequest->urlPath().size() > 0 && httpRequest->urlPath().at(0) == "key") {
         return AUTH_OK_PUBLIC;
    }

    QPointer<QHttpRequest> req = httpRequest;
    // Use QMutexLocker to wait indefinitely for the lock, preventing "Can not lock" errors
    QMutexLocker lock(&m_mutex); 
    bool indexing = false;
    Q_UNUSED(indexing) //node-only: the robot branch and shared-link resolution below
    // Search robot (indexer)
    QStringList reqPath = httpRequest->urlPath();
    QString authToken = req->headers()["authorization"];
    //Check token in query
    QUrlQuery q(httpRequest->url().query(QUrl::FullyDecoded));

    QString token = "";
    bool chatApp = false;

#ifndef SERVER
    //Search robot (indexer) - local search nodes crawl the node's own shares.
    //The gateway serves no robot content: search runs on the nodes.
    if (!m_crawlerToken.isEmpty() && authToken == m_crawlerToken ) {
        userID = "robot";
        indexing = true;
        if (reqPath.size() > 0 && !itemPath.startsWith(SHARED_LINK_PREFIX)) {
            userID = reqPath[0];
            //qInfo() << "Auth:" << req->headers()["Authorization"] <<  req->headers()["authorization"];
            rootOwnerID = "robot";
            return AUTH_OK_CRAWLING;
        }
    }
#endif

    #ifdef SERVER
    // One gateway serves both frontend domains. Prefer the public host header
    // (with reverse-proxy fallback) so ChatApp sessions and the injected
    // APP_MODE use exactly the same host check.
    QString domain = QString::fromUtf8(req->headers()["host"]);
    QString forwardedHost = QString::fromUtf8(req->headers()["x-forwarded-host"]);
    if (!forwardedHost.isEmpty()) domain = forwardedHost.section(',', 0, 0).trimmed();
    if (domain.isEmpty()) domain = httpRequest->url().host();
    chatApp = isChatHost(domain);

    if (authToken.contains("token")) {
        token = authToken.mid(authToken.indexOf(":") + 1).trimmed();
    }

    if (token.isEmpty() && authToken.startsWith("Bearer ")) {
         token = authToken.mid(7).trimmed();
    }

    if (token.isEmpty()) {
        token = readSessionFromCookie(httpRequest);
    }


    //Search query
    if (itemPath.startsWith(SEARCH_PREFIX) && reqPath.size() == 1 ) //user's sites
    {
        if (authToken.startsWith("token"))
        {
            token = authToken.mid(6).trimmed();
            if (m_Sessions.contains(token))
            {
                ClientSession* session = m_Sessions[token];
                userID = session->userName();
                rootOwnerID =userID;
                setSafeSessionId(httpRequest, token);
            }
        }

        QUrlQuery q =  QUrlQuery(httpRequest->url().query(QUrl::FullyDecoded));
        if (q.hasQueryItem("q"))
            return  AUTH_OK_SEARCHING;
        else
            return  AUTH_OK; //just show search page
    }


    if (itemPath == SITES_PREFIX) //global sites endpoint
    {
        return AUTH_OK_PUBLIC;
    }

    if (itemPath.startsWith(SITES_LINK_PREFIX) && reqPath.size() > 1 ) //user's sites
    {
        SiteInfo site;
        QString userSiteName = reqPath[1];
        if ( OnMyDiskLinkServer::instance().getSite(userSiteName, site))
        {
            userID = site.owner;
            rootOwnerID = site.owner;
            linkID = site.linkID;
            itemPath = QString("%1/%2").arg(site.shareName, userSiteName);

            QHash<QString, QVariant> sessionVars;
            QString siteSessionToken = getSessionIdForSharedPath(itemPath, httpRequest);
            ClientSession* session = userSession(userID, 7200000, siteSessionToken); //2 hours 7200000
            sessionVars["User"] = QStringLiteral("anonymous");
            sessionVars["Owner"] = rootOwnerID;
            sessionVars["isGuest"] = true;

            //Save item path
            sessionVars["SiteDevice"] = site.shareName;
            sessionVars["SiteName"] = userSiteName;
            if (site.domain == domain) {
                sessionVars["SiteDomain"] = domain;
            }
            session->setSessionVariables(sessionVars);
            setSafeSessionId(httpRequest, session->sessionID());
            return AUTH_OK;
        } else
            return AUTH_OFFLINE; //invalid/offline link
    }


    if (itemPath.startsWith(CHAT_PREFIX) ) //user's AI chats
    {
        SiteInfo site;
        bool found = false;

        ClientSession* session =  sessionForToken(token);
        QString user = session ? session->userName() : "";
        
        QHash<QString, QVariant> sessionVars;
        AuthResult result = AUTH_OK;

        if (! found) {
            found = OnMyDiskLinkServer::instance().getChatNode(user, site);
        }

        if (found) {
            user = site.owner;
        }

        if (user.isEmpty())
            return AUTH_FAIL;

        if (! session) {
             session = userSession(user, 3600000); // 1 hour session for token-auth
        }
        if (! found && user.contains("@")) {
            QString shortUser = user.left(user.indexOf("@"));
            found = OnMyDiskLinkServer::instance().getChatNode(shortUser, site);
        }

        if (chatApp)
            sessionVars["ChatApp"] = true;

        if (found)
        {
            // Access control: everyone can use basic assistant/loras, but only owner/shared can search/index or use code mode
            QString path = httpRequest->url().path();
            bool isSensitive = path.contains("/search") || path.contains("/index") || path.contains("/crawl") || path.contains("/code");
            QString reqUser = session ? session->userName() : "";
            bool isAuthorized = (reqUser == site.owner || site.users.contains(reqUser));

            if (isSensitive && isAuthorized) {
                // Access control on path prefix: make sure users only index their own files or their group's files
                QUrlQuery q(httpRequest->url().query(QUrl::FullyDecoded));
                QString pathParam = q.queryItemValue("path");
                QString urlParam = q.queryItemValue("url");

                if (pathParam.isEmpty() && !urlParam.isEmpty()) {
                    QUrl targetUrl(urlParam);
                    pathParam = targetUrl.path();
                }

                if (!pathParam.isEmpty()) {
                    QString cleanPath = pathParam.trimmed();
                    if (cleanPath.startsWith("/")) {
                        cleanPath = cleanPath.mid(1);
                    }
                    QStringList plist = cleanPath.split("/", Qt::SplitBehaviorFlags::SkipEmptyParts);
                    if (plist.size() > 0) {
                        QString targetUser = plist[0];
                        bool isPathAuthorized = (targetUser == site.owner || site.users.contains(targetUser));
                        if (!isPathAuthorized) {
                            qWarning() << "AI sensitive request: path prefix DENIED for user:" << reqUser
                                       << "Resource owner:" << targetUser << "Path:" << pathParam;
                            isAuthorized = false;
                        }
                    }
                }
            }

            if (isSensitive && !isAuthorized) {
                 qWarning() << "AI sensitive request DENIED for user:" << reqUser << "Path:" << path;
                 result = AUTH_FAIL;
            } else {
                rootOwnerID = site.owner;
                linkID = site.linkID;
                itemPath = site.shareName;
                userID = site.owner;
                //Save item path
                sessionVars["ChatDevice"] = site.shareName;
                result = AUTH_OK;
            }
        } else {
            qWarning() << "AI node NOT found for user:" << user << "Path:" << httpRequest->url().path();
            // Let HttpHandler return 404/503 instead of redirect for identified users
            result = AUTH_OK;
        }

        session->setSessionVariables(sessionVars);
        setSafeSessionId(httpRequest, session->sessionID());
        return result;
    }

    if ( itemPath == SUBMIT_FORM_PREFIX && httpRequest->method() == qhttp::EHTTP_POST) { //user's form submitted - check referred site
        QString referer = httpRequest->headers()["Referer"];
        QStringList refererList = referer.split("/", Qt::SplitBehaviorFlags::SkipEmptyParts);
        if (refererList.size() > 3) {
            QString siteName = refererList[3];
            SiteInfo site;
            if ( OnMyDiskLinkServer::instance().getSite(siteName, site))
            {
                rootOwnerID = site.owner;
                userID = site.owner;
                return AUTH_OK_PUBLIC;
            }  else
                return AUTH_OFFLINE;
         } else
            return AUTH_FAIL;
    }

    if (httpRequest->method() == qhttp::EHTTP_PUT) {
        itemPath = QUrl::fromPercentEncoding(httpRequest->headers()["Dest"]);
    }

     //signed webhook prefix
    if (itemPath.startsWith(SIGNED_WEBHOOK_PREFIX) && httpRequest->method() == qhttp::EHTTP_POST ) {
        rootOwnerID = "webhook";
        userID = "webhook";
        return AUTH_OK_PUBLIC;
    }

    if (itemPath == CALL_LINK_PREFIX || itemPath == CALL_PREFIX) {
        return AUTH_OK;
    }

    //check call link prefix
    if (itemPath.startsWith(CALL_LINK_PREFIX)) {

        QString callId = itemPath.mid(6);
        QString linkToken = getSessionIdForSharedPath(callId, httpRequest);
        ClientSession* session = userSession(userID, 7200000, linkToken); //2 hours 7200000
        QHash<QString, QVariant> sessionVars;
        sessionVars["User"] = QStringLiteral("anonymous");
        sessionVars["Owner"] = QStringLiteral("anonymous");
        sessionVars["isGuest"] = true;
        //Create session for guest call
        sessionVars["LinkUrl"] =  itemPath;
        if (!callId.isEmpty())
            sessionVars["CallId"] = callId;

        session->setSessionVariables(sessionVars);
        setSafeSessionId(httpRequest, session->sessionID());
        return AUTH_OK_PUBLIC;
    }
    else if (itemPath.startsWith(PUBLIC_LINKS_PREFIX) ||
             itemPath.startsWith(PERSONAL_LINKS_PREFIX) || itemPath.startsWith(SITES_PREFIX))
    {
        token = authToken.contains("token") ? authToken.mid(authToken.indexOf(":") + 1).trimmed() :
                        q.hasQueryItem("token") ? q.queryItemValue("token", QUrl::FullyDecoded) :  readSessionFromCookie(httpRequest);
        if (!token.isEmpty() && m_Sessions.contains(token))
        {
            ClientSession* session = m_Sessions[token];
            QHash<QString, QVariant> sessionVars = session->getSessionVariables();
            userID = session->userName() == "anonymous" ? token : session->userName();
            session->restartTimer();
            rootOwnerID =sessionVars.value("Owner", rootOwnerID).toString();
            setSafeSessionId(httpRequest, token);
        }
        return AUTH_OK_REF;
    }
    else if (itemPath.startsWith(SHARED_LINK_PREFIX))
	{
#ifdef P2P_ENABLED
        // In P2P mode, /shared/<address> is resolved on client side via GunDB & WebRTC
        return AUTH_OK_PUBLIC;
#else
        QStringList reqPath = httpRequest->urlPath();
        /*if (httpRequest->method() == qhttp::EHTTP_POST && reqPath.size() > 0 && (reqPath.at(0) == "login" || reqPath.at(0) == "key" ))
        {
            //allow to process key submission
            return AUTH_OK_PUBLIC;
        }*/
        itemPath = QUrl(itemPath).path();

        QString subItemPath = "";
        //check "<encodedlink>:/path/to/subresource" in tail
        int sp = itemPath.lastIndexOf(":");

        if (sp != -1) {
            subItemPath = itemPath.mid(sp + 1);
            itemPath = itemPath.left(sp);
            httpRequest->setSubUrl(subItemPath);
        }
        QString linkPath = itemPath.mid(8);
        QString linkUrl = getSharedPath(httpRequest);
        QString linkToken = getSessionIdForSharedPath(linkPath, httpRequest);
        //get session
        ClientSession* session = userSession(userID, 7200000, linkToken); //2 hours 7200000
        quint64 key = session->getVar("key", 0).toULongLong();

        if (key == 0) {
            key = SimpleCrypt::defaultKey;
        }

        SimpleCrypt crypt(key);

        //Try new way
        QString decodedLink = crypt.decryptUrlEncodedToString(linkPath);

        if (decodedLink == "" || decodedLink.indexOf(":") == -1) {
            // Decryption failed or invalid decoded link (password wrong or missing).
            // Check if they are trying to fetch actual files or list folder content
            QString query = httpRequest->url().query(QUrl::FullyDecoded);
            bool isSubResource = !subItemPath.isEmpty() || query.contains("list") || query.contains("preview") || query.contains("cover") || query.contains("direct") || query.contains("totext");
            if (isSubResource) {
                return AUTH_NOTALLOWED;
            }

            userID = "anonymous";
            rootOwnerID = "anonymous";
            linkID = "";

            QHash<QString, QVariant> sessionVars = session->getSessionVariables();
            sessionVars["User"] = QStringLiteral("anonymous");
            sessionVars["LinkUrl"] = linkUrl;
            sessionVars["isGuest"] = true;
            sessionVars["directShare"] = false;
            sessionVars["rights"] = "readonly";
            sessionVars["locked"] = true;
            session->setSessionVariables(sessionVars);
            setSafeSessionId(httpRequest, session->sessionID());
            return AUTH_OK_SHARED;
        }

		int p = decodedLink.indexOf(":");

        if (req.isNull())  { // it may die on getting the link
            return  AUTH_NOTALLOWED;
        }

        QString linkId = decodedLink.left(p);
        QString shareName = "";
        QString shareVersion = "";
        QPointer<OnMyDiskLink> clientLink = OnMyDiskLinkServer::instance().getLink(linkId);
        if (clientLink) {
            userID = indexing ? "robot" : clientLink->user();
            rootOwnerID = clientLink->user();
            shareName = clientLink->shareName();
            shareVersion = clientLink->linkVersion();
            linkID = linkId;
        }
        #ifdef SERVER
        if (clientLink.isNull())  {
            QSharedPointer<ClientNotifier> notifier = NotificationManager::instance().getLinkNotifier(linkId);
            if (notifier) {
                userID = indexing ? "robot" : notifier->getUser();
                rootOwnerID = notifier->getUser();
                shareName = notifier->shareName();
                shareVersion = notifier->version();
                linkID = linkId;
            }
        }
        #endif
        // re-sharing and gw-only links
        bool directShare = false;
        QString rights = "readonly";
        itemPath = decodedLink.mid(p + 1);

        //Default "link expired or offline "
        QHash<QString, QVariant> sessionVars;
        sessionVars["User"] = QStringLiteral("anonymous");
        sessionVars["Link"] = linkID;
        sessionVars["Item"] = itemPath;
        sessionVars["Owner"] = rootOwnerID;
        sessionVars["LinkUrl"] = linkUrl;
        sessionVars["CanMount"] = false;
        sessionVars["isGuest"] = true;
        sessionVars["directShare"] = directShare;
        sessionVars["shareName"] = shareName;
        sessionVars["version"] = shareVersion;
        sessionVars["rights"] = rights;
        sessionVars["locked"] = false;
        session->setSessionVariables(sessionVars);
        setSafeSessionId(httpRequest, session->sessionID());

        //qDebug() << itemPath << rootOwnerID;
        //save to session
        sessionVars["directShare"] = directShare;
        session->setSessionVariables(sessionVars);
        setSafeSessionId(httpRequest, session->sessionID());
        return AUTH_OK_SHARED;
#endif
	}
    #else
    Q_UNUSED(indexing)
    #endif

    //temporary session for signup and reset
    if (itemPath.startsWith("/signup") || itemPath == "/reset") {
        ClientSession* session;
        token = getSessionIdForSharedPath(itemPath, httpRequest);
        if (m_Sessions.contains(token))
        {
            session = m_Sessions[token];
            session->restartTimer();
        } else {
            session = userSession(userID, 7200000, token); //2 hours 7200000
        }
        userID = session->userName();
        rootOwnerID = session->getVar("Owner", rootOwnerID).toString();
        setSafeSessionId(httpRequest, token);
        return AUTH_OK;
    }
	//Check token in auth  header
	if (httpRequest->headers().contains("authorization"))
	{
        //move along, nothing to see there
        if (httpRequest->urlPath().size() > 0 && httpRequest->urlPath().first() == ADMIN_ROOT)
            return AUTH_FAIL;

        if (authToken.startsWith("token"))
        {
            token = authToken.mid(6).trimmed();
            ClientSession* session = sessionForToken(token);
            if (session)
			{
                QHash<QString, QVariant> sessionVars = session->getSessionVariables();
                userID = sessionVars.value("isGuest", false).toBool() ? token : session->userName();
                session->restartTimer();
                itemPath = sessionVars.value("Item", "").toString();
                linkID = sessionVars.value("Link", "").toString();
                rootOwnerID = sessionVars.value("Owner", rootOwnerID).toString();
                setSafeSessionId(httpRequest, token);

                if (itemPath != "" &&  linkID != "")
					return AUTH_OK_SHARED;
                else
                    return AUTH_OK;
            }  else {
                return AUTH_FAIL;
            }
		}
	}
    // check token in quety
    if (q.hasQueryItem("token"))
    {
        //move along, nothing to see there
        if (httpRequest->urlPath().size() > 0 && httpRequest->urlPath().first() == ADMIN_ROOT)
            return AUTH_FAIL;

        token =  q.queryItemValue("token", QUrl::FullyDecoded);

#ifdef SERVER
        //Check if token is link id
        QPointer<OnMyDiskLink> clientLink = OnMyDiskLinkServer::instance().getLink(token);
        if (!clientLink.isNull()) {
            userID = clientLink->user();
            linkID = token;
            rootOwnerID = userID;
            setSafeSessionId(httpRequest, token);
            //httpResponse->addHeader("Access-Control-Allow-Origin", "*");
            return AUTH_OK;
        //then check if it is session id
        }
#endif
        ClientSession* session = sessionForToken(token);
        if (session)
        {
            //ClientSession* session = m_Sessions[token];
            QHash<QString, QVariant> sessionVars = session->getSessionVariables();
            userID = sessionVars.value("isGuest", false).toBool() ? token : session->userName();
            session->restartTimer();
            rootOwnerID = sessionVars.value("Owner", userID).toString();
            linkID = sessionVars.value("Link", "").toString();
            itemPath = sessionVars.value("Item", "").toString();
            setSafeSessionId(httpRequest, token);

            if (itemPath != "" &&  linkID != "") // link and itemPath are saved for shared links
                return AUTH_OK_SHARED;
            else
                return AUTH_OK;
        }
    }
    // authorization by "?key=" requests
    if (q.hasQueryItem("key")) {
        QString key = q.queryItemValue("key");
        if (httpRequest->method() == qhttp::EHTTP_GET && checkKey(key)) {
            ClientSession* session = userSession(key, 86400000, key); //24hours
            userID = key;
            rootOwnerID = key;
            setSafeSessionId(httpRequest, session->sessionID());
            return AUTH_OK;
        }
    }

    QString referer = httpRequest->headers()["Referer"];

    if (!referer.isEmpty())
    {
        QUrlQuery q(QUrl(referer).query(QUrl::FullyDecoded));

        if (q.hasQueryItem("key")) {
            QString key = q.queryItemValue("key");

            if (m_Sessions.contains(key))
            {
                ClientSession* session = m_Sessions[key];
                userID = session->userName();
                rootOwnerID = session->userName();
                session->restartTimer();
                return AUTH_OK;
            }
        }
    }

	//check referer, but do not allow to access root
    if (!referer.isEmpty() && itemPath != "/")
	{
		QString encodedPath = QUrl(referer).path();
        token = "";
        if (encodedPath.startsWith(CALL_LINK_PREFIX)) {
            token = encodedPath.mid(6);
        } else
        if (encodedPath.startsWith(SHARED_LINK_PREFIX))
		{
#ifdef P2P_ENABLED
            return AUTH_OK_PUBLIC;
#else
            token = getSessionIdForSharedPath(encodedPath.mid(8), httpRequest);
#endif
        }
        if (!token.isEmpty() && m_Sessions.contains(token))
        {
            ClientSession* session = m_Sessions[token];
            QHash<QString, QVariant> sessionVars = session->getSessionVariables();
            userID = session->userName();
            session->restartTimer();
            itemPath = sessionVars.value("Item", "").toString();
            linkID = sessionVars.value("Link", "").toString();
            rootOwnerID =sessionVars.value("Owner", rootOwnerID).toString();
            setSafeSessionId(httpRequest, token);

            if (itemPath != "" &&  linkID != "")
					return AUTH_OK_SHARED;
            if (itemPath.startsWith(CHAT_PREFIX) ) //user's AI chats
            {
                SiteInfo site;

                ClientSession* session =  sessionForToken(token);

                if (chatApp)
                    session->setVar("ChatApp", true);

                // For guest sessions the link is registered under the session token (not "anonymous"),
                // so use the token as userID so getRootFor() finds the correct RootResource.
                userID = session->isGuest() ? session->sessionID() : session->userName();
                rootOwnerID = session->getVar("Owner", userID).toString();

                QHash<QString, QVariant> sessionVars = session->getSessionVariables();
                if (sessionVars.contains("Link") && sessionVars.contains("Item")) {
                    linkID = sessionVars["Link"].toString();
                    itemPath = sessionVars["Item"].toString();
                    if (sessionVars.contains("Owner"))
                         rootOwnerID = sessionVars["Owner"].toString();

                    setSafeSessionId(httpRequest, token);
                    return AUTH_OK_SHARED;
                }
            }
            else
                    return AUTH_FAIL;
        }
	}

    //fallback to cookie
    token = authToken.contains("token", Qt::CaseInsensitive) ? (authToken.contains(":") ? authToken.mid(authToken.indexOf(":") + 1).trimmed() : (authToken.contains(" ") ? authToken.mid(authToken.indexOf(" ") + 1).trimmed() : authToken.trimmed())) :
            authToken.contains("bearer", Qt::CaseInsensitive) ? authToken.mid(authToken.indexOf(" ") + 1).trimmed() :
            q.hasQueryItem("token") ? q.queryItemValue("token", QUrl::FullyDecoded) :  readSessionFromCookie(httpRequest);

    ClientSession* session =  sessionForToken(token);

    if (chatApp)
            session->setVar("ChatApp", true);

    // For guest sessions the link is registered under the session token (not "anonymous"),
    // so use the token as userID so getRootFor() finds the correct RootResource.
    userID = session->isGuest() ? session->sessionID() : session->userName();
    rootOwnerID = session->getVar("Owner", userID).toString();

    QHash<QString, QVariant> sessionVars = session->getSessionVariables();
    if (sessionVars.contains("Link") && sessionVars.contains("Item")) {
        linkID = sessionVars["Link"].toString();
        itemPath = sessionVars["Item"].toString();
        if (sessionVars.contains("Owner"))
             rootOwnerID = sessionVars["Owner"].toString();

        setSafeSessionId(httpRequest, token);
        return AUTH_OK_SHARED;
    }

    AuthResult result = AUTH_OK;
    if (httpRequest->method() == qhttp::EHTTP_GET)
        result = AUTH_OK;
     else {
        bool isAuthPath = itemPath.startsWith("/rest/signin") || itemPath.startsWith("/rest/relogin") || itemPath.startsWith("/rest/signinwithgoogle");
        // WebRTC signaling and device registration are always allowed for
        // guest sessions so that public (shared) links work without login.
        bool isSignalingPath = itemPath.startsWith("/api/signaling") || itemPath.startsWith("/api/device/register");
        result = (session->isGuest() && !isAuthPath && !isSignalingPath) ? AUTH_FAIL : AUTH_OK;
    }
    setSafeSessionId(httpRequest, session->sessionID());
    return result;
}

QPointer<ClientSession> SessionAuth::addSessionFor(QString userName, quint64 sessionLifeTime, QString sessionID)
{
    QMutexLocker lock(&m_mutex);
    ClientSession* session = this->userSession(userName, sessionLifeTime, sessionID);
    return session;
}

QPointer<ClientSession> SessionAuth::getSession(QString sessionID, quint64 sessionLifeTime)
{
    QMutexLocker lock(&m_mutex);
    ClientSession* session = this->sessionForToken(sessionID, sessionLifeTime);
    return session;
}

QList<ClientSession *> SessionAuth::getSessionsForPath(const QString& itemPath)
{
    QList<ClientSession *> list;
    QMutexLocker lock(&m_mutex);
    Q_FOREACH(ClientSession* s, m_Sessions) {
        QString sharedPath = s->getVar("itemPath", "").toString();
        if (!sharedPath.isEmpty() && itemPath.startsWith(sharedPath))
            list << s;
    }
    return list;
}

bool SessionAuth::sessionExists(QString sessionID)
{
    QMutexLocker lock(&m_mutex);
    return m_Sessions.contains(sessionID);
}


ClientSession *SessionAuth::userSession(QString userName, quint64 sessionLifeTime, QString sessionID)
{
    ClientSession* session = this->session(sessionID, sessionLifeTime);
    session->setUserName(userName);
    session->setVar("Owner", userName);
    return session;
}

ClientSession *SessionAuth::session(QString sessionID, quint64 sessionLifeTime)
{
    ClientSession* session;
    if (sessionID == "") {
        sessionID = QUuid::createUuid().toString();
        while (m_Sessions.contains(sessionID))
        {
            sessionID = QUuid::createUuid().toString();
        }
    }
    if (!(m_Sessions.contains(sessionID))) {
        session = new ClientSession(sessionLifeTime);
        session->setSessionID(sessionID);
        session->setVar("Owner", QStringLiteral("anonymous"));
        session->moveToThread(qApp->thread());
        connect(session, &ClientSession::timeout, this, &SessionAuth::sessionTimeout);
        m_Sessions.insert(sessionID, session);
    } else {
        session = m_Sessions[sessionID];
    }
    session->restartTimer();
    return session;
}

void SessionAuth::removeSession(QHttpRequest *httpRequest )
{
    QString sessionID = httpRequest->sessionId();
    removeSession(sessionID);
}

void SessionAuth::removeSession(QString sessionID)
{

    if (QThread::currentThread() != this->thread()) {
           QMetaObject::invokeMethod(this, "removeSession", Qt::QueuedConnection, Q_ARG(QString, sessionID));
    } else {
        QMutexLocker lock(&m_mutex);
        if (m_Sessions.contains(sessionID))
        {
            ClientSession* session = m_Sessions[sessionID];
            
            // Notify OMC about signout
            QNetworkAccessManager *mgr = new QNetworkAccessManager(this);
            connect(mgr, &QNetworkAccessManager::finished, mgr, &QNetworkAccessManager::deleteLater);
            
            // Assuming OMC is running on localhost:8002. Ideally this should be configurable.
            QNetworkRequest req(QUrl("http://localhost:8002/notify/signout")); 
            req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
            QJsonObject json;
            json["omd_key"] = sessionID;
            mgr->post(req, QJsonDocument(json).toJson());

            qDebug() << "End session" << sessionID << "for user" << session->userName();
            m_Sessions.remove(sessionID);
            delete session;
        }
    }
}


bool SessionAuth::checkUserPass(QString userName, QString password)
{
    //No local user accounts on the gateway (accounts live in GunDB and, during
    //the migration period, on the legacy gateway). Nodes authenticate against
    //local OS accounts.

    // Resolve configuration file path (client mode)
    QString configFile;
#ifdef Q_OS_LINUX
#ifdef QT_DEBUG
    configFile = QFileInfo(QCoreApplication::applicationDirPath() + "/../../etc/onmydisk/onmydisk.conf").absoluteFilePath();
#else
#ifdef DEPLOY
    configFile = QString("/etc/onmydisk/onmydisk.conf");
#else
    configFile = QFileInfo(QCoreApplication::applicationDirPath() + "/../../etc/onmydisk/onmydisk.conf").absoluteFilePath();
#endif
#endif
#else // Windows / macOS / etc.
    configFile = QCoreApplication::applicationDirPath() + "/../etc/onmydisk.conf";
#endif
    configFile = QFileInfo(configFile).absoluteFilePath();

    QSettings settings(configFile, QSettings::IniFormat);
    QString allowedUser = settings.value("FileNode/User", "").toString();

    if (!allowedUser.isEmpty() && userName != allowedUser)
    {
        qWarning() << "Login rejected for user:" << userName << "(FileNode config only permits" << allowedUser << ")";
        return false;
    }

#ifdef Q_OS_LINUX
    return authenticateLinuxUser(userName, password);
#elif defined(Q_OS_WIN)
    return authenticateWindowsUser(userName, password);
#elif defined(Q_OS_MAC)
    return authenticateMacUser(userName, password);
#else
    return emit authRequest(userName, password);
#endif
}

bool SessionAuth::checkUserIsAdmin(QString userName)
{
    QJsonObject profile = getUserProfile(userName);
    return profile["isAdmin"].toBool();
}

bool SessionAuth::checkUserIsGuest(QString userID)
{
    QMutexLocker lock(&m_mutex);
    if (m_Sessions.contains(userID)) {
        lock.unlock();
        return m_Sessions[userID]->getVar("isGuest", false).toBool();
    } else
        return false;
}

QJsonObject SessionAuth::getUserProfile(QString userName, QString group)
{
    //User profiles live on the legacy gateway during the GunDB migration
    Q_UNUSED(userName)
    Q_UNUSED(group)
    return QJsonObject();
}

bool SessionAuth::checkKey(QString key)
{
    #ifdef SERVER
    //Device keys are not tracked by the gateway anymore: identities live in GunDB
    Q_UNUSED(key)
    return false;
    #else
    return emit keyAuthRequest(key);
    #endif
}


ClientSession *SessionAuth::sessionForToken(const QString &token, quint64 sessionLifeTime)
{
    ClientSession *session = nullptr;
    if (m_Sessions.contains(token)) {
        session = m_Sessions[token];
        return session;
    }
    QJsonObject profile;
    bool ok = false;
    //The gateway keeps sessions in memory only: identities live in GunDB
    #ifndef SERVER
        QEventLoop loop;

        QNetworkAccessManager* externalAccessManager = new QNetworkAccessManager();

        connect(externalAccessManager, &QNetworkAccessManager::sslErrors, this, [this] (QNetworkReply *reply, const QList<QSslError> &errors) {
            Q_FOREACH(QSslError error, errors)
            {
                qWarning() << "SSL notice:" << error.errorString();
            }
            reply->ignoreSslErrors();
        });

        connect(externalAccessManager, &QNetworkAccessManager::finished, this, [this, &loop, &ok, &profile] (QNetworkReply *reply) {
            //API response
            if (reply->error() == QNetworkReply::NoError) {
                profile = QJsonDocument::fromJson(reply->readAll()).object();
                ok = profile["valid"].toBool();
            } else {
                qCritical() << "Sign in error:" <<  reply->errorString();
            }
            reply->deleteLater();
            loop.exit(0);
        });

        //Sign in to OMD via API
        #ifdef STAGING
        QString signinAPI = QString("https://%1/signin").arg(m_server);
        #else
        #ifdef QT_DEBUG
        QString signinAPI = QString("http://localhost:8080/signin");
        #else
        QString signinAPI = QString("https://%1/signin").arg(m_server);
        #endif
        #endif
        QNetworkRequest signinRequest(signinAPI);
        signinRequest.setRawHeader("content-type", "application/json");
        QJsonObject request;
        request["action"] = "signin";
        request["device_key"] = token;
        request["remember"] = true;

        QJsonDocument requestData(request);

        externalAccessManager->post(signinRequest, requestData.toJson(QJsonDocument::Compact));
        loop.exec();

        externalAccessManager->deleteLater();

    #endif
    if (ok) {
        // Logged in to Core. Now Fetch Rights from Console asynchronously.
        QString userName = profile["user"].toString();
        
        session = userSession(userName, sessionLifeTime, token);
        QHash<QString, QVariant> sessionVars;
        sessionVars["isAdmin"] = profile["isAdmin"].toBool();
        sessionVars["isGroupAdmin"] = profile["isGroupAdmin"].toBool();
        sessionVars["isGuest"] = false;
        sessionVars["group"] = profile["group"].toString();
        
        // Fallback default rights (Free)
#ifdef QT_DEBUG
        sessionVars["status"] = "Unlimited";
        sessionVars["collab"] = true;
        sessionVars["ai_chat"] = true;
        sessionVars["token_balance"] = 1000.0;
#else
        sessionVars["status"] = "Free";
        sessionVars["collab"] = false;
        sessionVars["ai_chat"] = true;
        sessionVars["token_balance"] = 0.0;
#endif
        
        sessionVars["console_url"] = m_consoleUrl;
        sessionVars["token"] = token;
        
        session->setSessionVariables(sessionVars);

    } else {
        //guest session or anonymous AI chat user
        session = userSession(QStringLiteral("anonymous"), 7200000, token); //2 hours 7200000
        
        QHash<QString, QVariant> sessionVars;
        sessionVars["isGuest"] = true;
        sessionVars["token"] = token;
        session->setSessionVariables(sessionVars);

    }

    return session;

}

bool SessionAuth::linkIsMounted(QString user, QString linkID, QString itemPath)
{
    //GW-hosted links are gone: shares are resolved by the nodes
    Q_UNUSED(user)
    Q_UNUSED(linkID)
    Q_UNUSED(itemPath)
    return false;
}

bool SessionAuth::createUserAccount(QString server, QString userName, QString password, QByteArray postData)
{
	return emit createAccount(server, userName, password,  postData);
}

QString SessionAuth::readSessionFromCookie(QHttpRequest *httpRequest)
{
    QHash<QString, QString> parsedCookies = httpRequest->parseCookies();
    QString path = httpRequest->url().path();
    QString referer = httpRequest->headers()["Referer"];
    bool isShared = path.startsWith("/shared/") || referer.contains("/shared/");

    if (isShared && parsedCookies.contains("OMD_SHARED_SESSION_ID"))
    {
       return parsedCookies.value("OMD_SHARED_SESSION_ID");
    }
    if (parsedCookies.contains(globalCookieID()))
    {
       QString sessionID = parsedCookies.value(globalCookieID());
       return sessionID;
    } else
       return getSessionIdForSharedPath(httpRequest->url().host(), httpRequest);;
}

const QString SessionAuth::globalCookieID()
{
    #ifdef SERVER
    return "OMD_SESSION_ID";
    #else
    return "OMD_CLIENT_ID";
    #endif
}

const QString SessionAuth::getSessionIdForSharedPath(const QString& sharedPath, QHttpRequest *httpRequest)
{
    QString userAgent = httpRequest->headers()["User-Agent"];
    //qDebug() << "User agent:" << userAgent;
    QString sessionPeerId = QString("%1:%2:%3:%4").arg(sharedPath, httpRequest->remoteAddress(), userAgent, QString::number(SimpleCrypt::defaultKey, 16));
    //qDebug() << "Guest id:" << sessionPeerId;
    QString sessionId = QString(QCryptographicHash::hash(sessionPeerId.toUtf8(), QCryptographicHash::Md5).toHex());
    //qDebug() << "Guest session:" << sessionId;
    return sessionId;
}

const QString SessionAuth::getSharedPath(QHttpRequest *httpRequest)
{
    QString path = httpRequest->url().path();
    //check "<encodedlink>:/path/to/subresource" in tail
    int sp = path.lastIndexOf(":");
    if (sp != -1) {
        path = path.left(sp);
    }
    return path;
}


QString SessionAuth::loginPagePath()
{
    return QStringLiteral("");
}

void SessionAuth::setSessionVariable(QString sessionID, QString varName, QVariant value)
{
    QMutexLocker lock(&m_mutex);
    ClientSession *session = this->session(sessionID);
    lock.unlock();
    session->setVar(varName, value);
    if (varName == "User")
        session->setUserName(value.toString());
}

void SessionAuth::setSessionVariables(QString sessionID, QHash<QString, QVariant> vars)
{
    QMutexLocker lock(&m_mutex);
    ClientSession *session = this->session(sessionID);
    lock.unlock();
    session->setSessionVariables(vars);
}

QVariant SessionAuth::getSessionVariable(QString sessionID, QString valueName, QVariant defaultValue)
{
    QMutexLocker lock(&m_mutex);
    if (m_Sessions.contains(sessionID)) {
        ClientSession *session = this->session(sessionID);
        lock.unlock();
        return session->getVar(valueName, defaultValue);
    } else
        return defaultValue;
}

QVariant SessionAuth::getStaticVariable(QString valueName)
{
#if !defined(P2P_ENABLED) && !defined(NO_WEB_UI)
    return TemplateParser::instance().staticVar(valueName);
#else
    Q_UNUSED(valueName)
    return QVariant();
#endif
}

QString SessionAuth::getCurrentUser(QString sessionID)
{
    QMutexLocker lock(&m_mutex);
    if (m_Sessions.contains(sessionID)) {
        lock.unlock();
        return m_Sessions[sessionID]->getVar("isGuest", false).toBool() ? sessionID : m_Sessions[sessionID]->userName();
    } else
        return sessionID;
}

void SessionAuth::sessionTimeout()
{
    ClientSession* session = static_cast<ClientSession*> (sender());

    #ifdef SERVER
    if (OnMyDiskLinkServer::instance().hasActiveLink(session->userName())) {
        session->restartTimer();
        return;
    }

    if (NotificationManager::instance().hasConnections(session->userID(), session->sessionID())) {
         session->restartTimer();
         return;
    }

    emit sessionExpired(session->userID(), session->sessionID());
    m_Sessions.remove(session->sessionID());
    delete session;
    #else
    emit sessionExpired(session->userName(), session->sessionID());
    m_Sessions.remove(session->sessionID());
    delete session;
    #endif
}

QJsonObject SessionAuth::fetchRights(QString userName, QString token)
{
    QJsonObject rightsProfile;
#ifdef QT_DEBUG
    Q_UNUSED(userName)
    Q_UNUSED(token)
    rightsProfile["status"] = "Unlimited";
    rightsProfile["token_balance"] = 1000.0;
    QJsonObject rights;
    rights["ai_chat"] = true;
    rights["seats_allowed"] = 10;
    rightsProfile["rights"] = rights;
    return rightsProfile;
#endif
    QPointer<QNetworkAccessManager> consoleAccessManager = new QNetworkAccessManager();
    QPointer<QEventLoop> consoleLoop = new QEventLoop();

    // Prepare Request
    QString consoleUrl = m_consoleUrl;
    if (consoleUrl.isEmpty()) {
        delete consoleLoop;
        consoleAccessManager->deleteLater();
        return rightsProfile;
    }
    
    QString identifier = userName;
    if (userName == "anonymous" && !token.isEmpty())
        identifier = token;

    QString consoleApi = QString("%1/api/v1/internal/rights/%2").arg(consoleUrl, identifier);
    QNetworkRequest req(consoleApi);
    req.setRawHeader("token", m_crawlerToken.toUtf8());

    QPointer<QNetworkReply> reply = consoleAccessManager->get(req);

    // Connect signals
    connect(reply.data(), &QNetworkReply::sslErrors, reply.data(), [reply] (const QList<QSslError> &errors) {
        Q_UNUSED(errors);
        if (reply) reply->ignoreSslErrors();
    });

    connect(consoleAccessManager.data(), &QNetworkAccessManager::finished, this, [consoleLoop, reply, &rightsProfile] (QNetworkReply *r) {
        if (r && r->error() == QNetworkReply::NoError) {
            rightsProfile = QJsonDocument::fromJson(r->readAll()).object();
        } else if (r) {
            qWarning() << "Failed to fetch rights from Console:" << r->errorString();
        }
        if (r) r->deleteLater();
        if (consoleLoop) consoleLoop->quit();
    });

    QTimer::singleShot(1000, consoleLoop.data(), [consoleLoop, reply, userName] () {
        qWarning() << "Timeout fetching rights from Console:" << userName;
        if (reply) reply->abort();
        if (consoleLoop) consoleLoop->quit();
    });

    consoleLoop->exec();
    delete consoleLoop;
    consoleAccessManager->deleteLater();
    return rightsProfile;
}

void SessionAuth::applyRights(ClientSession *session, QJsonObject rightsProfile)
{
    QHash<QString, QVariant> sessionVars = session->getSessionVariables();

    // Parse Rights
    if (!rightsProfile.isEmpty() && rightsProfile.contains("rights")) {
        QJsonObject rights = rightsProfile["rights"].toObject();
        bool collab = (rightsProfile["token_balance"].toDouble() > 0.0) || (rights["seats_allowed"].toInt() > 1);

        sessionVars["collab"] = collab;
        sessionVars["ai_chat"] = rights["ai_chat"].toBool();
        sessionVars["status"] = rightsProfile["status"].toString();
        sessionVars["token_balance"] = rightsProfile["token_balance"].toDouble();

        if (collab) {
            sessionVars["group"] = rightsProfile["group_name"].toString();
            if (rightsProfile["is_owner"].toBool()) {
                sessionVars["isGroupAdmin"] = true;
            }
        }
    } else {
            // Fallback default rights (Free)
#ifdef QT_DEBUG
            sessionVars["status"] = "Unlimited";
            sessionVars["collab"] = true;
            sessionVars["ai_chat"] = true;
            sessionVars["token_balance"] = 1000.0;
#else
            sessionVars["status"] = "Free";
            sessionVars["collab"] = false;
            sessionVars["ai_chat"] = true;
            sessionVars["token_balance"] = 0.0;
#endif
    }

    sessionVars["console_url"] = m_consoleUrl;
    
    session->setSessionVariables(sessionVars);
}


void SessionAuth::setSafeSessionId(qhttp::server::QHttpRequest *httpRequest, const QString &sessionID)
{
    if (!sessionID.isEmpty())
        httpRequest->setSessionId(sessionID);
}
