#include "httphandler.h"
#include <QCryptographicHash>
#include <QJsonObject>
#include <QJsonDocument>
#include <QJsonArray>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <QUrlQuery>
#include <QEventLoop>
#include <QTimer>
#include <QImage>
#include <QUuid>
#include <QXmlStreamReader>
#include "qglobal.h"
#include "simplecrypt.h"
#include "version.h"
#if (!defined(P2P_ENABLED) && !defined(NO_WEB_UI)) || defined(SERVER)
#include "httpserverbackend.h"
#endif
#include "userevents.h"
#if (!defined(P2P_ENABLED) && !defined(NO_WEB_UI)) || defined(SERVER)
#include "notificationmanager.h"
#endif
#if (!defined(P2P_ENABLED) && !defined(NO_WEB_UI)) || defined(SERVER)
#include "templateparser.h"
#endif
#include "onmydisklinkserver.h"

#if defined(P2P_ENABLED) || defined(NO_WEB_UI)
// P2P build has no template engine; stripHtml replicates TemplateParser::strip() for the robot index.
static QString stripHtml(const QString &strValue)
{
    QXmlStreamReader xml(strValue);
    QString plainValue = "";
    while (!xml.atEnd()) {
        if ( xml.readNext() == QXmlStreamReader::Characters ) {
            plainValue += xml.text();
        }
    }
    if (plainValue.isEmpty())
        plainValue = strValue;
    plainValue.replace("&", "&amp;");
    plainValue.replace("#", "");
    plainValue.replace("\"", "");
    plainValue.replace("'", "");
    plainValue.replace("{", "");
    plainValue.replace("}", "");
    plainValue.replace("[", "");
    plainValue.replace("]", "");
    return plainValue.trimmed();
}
#else
static QString stripHtml(const QString &strValue)
{
    return TemplateParser::strip(strValue);
}
#endif

int HttpHandler::m_reqNumber = 0;

//#define REQUEST_TIMEOUT 180000
#define REQUEST_TIMEOUT 10000
#define REQUEST_TRANSCODE_TIMEOUT 10000
#define TRANSCODE_FINISHED -2
#define TRANSCODE_NO_PROGRESS -1


HttpHandler::HttpHandler(QSettings *settings, QHttpRequest *req, QHttpResponse *resp,
                             QList<QString> indexes, QString serverString, quint64 maxFileSize, bool genIndex,
                             QString userName, QString rootOwner,
                             SessionAuth* sessionAuthProvider,
                             RootResource* rootResource,
                             bool searchEnabled,
                             bool keyLogin,
                             QObject *parent):
    QObject(parent), m_ServerString(serverString),
    m_indexFiles(indexes), m_req(req), m_resp(resp), m_rootResource(rootResource), m_maxBufferSize(maxFileSize), m_genIndex(genIndex),
    m_Settings(settings), m_userName(userName),
    m_rootOwner(rootOwner),
    m_searchEnabled(searchEnabled),
    m_keyLogin(keyLogin),
    m_pkiHelper(nullptr),
	m_sessionAuthProvider(sessionAuthProvider),
	m_resourceCollector(this)
{
    m_isServerNode = false;
    init();
}



HttpHandler::HttpHandler(QSettings *settings, QHttpRequest *req, QHttpResponse *resp,
                             QList<QString> indexes, QString serverString, quint64 maxFileSize, bool genIndex,
                             QString userName, QString rootOwner,
                             PKIHelper *pkiHelper,
                             SessionAuth* sessionAuthProvider,
                             RootResource* rootResource,
                             bool searchEnabled, bool keyLogin,
                             QObject *parent):
    QObject(parent), m_ServerString(serverString),
    m_indexFiles(indexes), m_req(req), m_resp(resp), m_rootResource(rootResource), m_maxBufferSize(maxFileSize), m_genIndex(genIndex),
    m_Settings(settings), m_userName(userName),
    m_rootOwner(rootOwner),
    m_searchEnabled(searchEnabled),
    m_keyLogin(keyLogin),
    m_pkiHelper(pkiHelper),
	m_sessionAuthProvider(sessionAuthProvider)
{
    m_isServerNode = true;
    init();
}

void HttpHandler::init()
{
    m_reqNumber++;
    m_reqId = QString("[%1]-%2").arg(QString::number(m_reqNumber), m_req->url().toString());
    m_iconDir = QStringLiteral("mimetypes");
    m_reader = nullptr;
    m_requestReady = false;
    m_working = true;
    m_reading = false;
    m_writing = false;
    m_bytesReceived = 0;
    m_totalBytes = 0;
    m_bytesToSend = 0;
    m_headerSent = false;

    m_Depth = Depth::zero;

    if (m_req->headers().contains("depth"))
    {
        QString depthVal = m_req->headers()["depth"];
        if (depthVal == "0")
            m_Depth = Depth::zero;
        else if (depthVal == "1")
            m_Depth = Depth::one;
        else
            m_Depth = Depth::infinity;
    }
}

bool HttpHandler::checkSitePath(const QString& urlPath, QString &redirectUrl)
{
    Q_UNUSED(redirectUrl)
    bool result = false;

if (urlPath.startsWith(CHAT_PREFIX)) // AI Assistant proxy
    {
#ifdef SERVER
        QString chatDevice = m_req->property("ChatDevice").toString();
        if (chatDevice.isEmpty())
            chatDevice = m_sessionAuthProvider->getSessionVariable(m_req->sessionId(), "ChatDevice",  "").toString();

        if (chatDevice.isEmpty()) {
             QString host = m_req->headers()["host"];
             SiteInfo site;
             if (OnMyDiskLinkServer::instance().getChatNode("", site)) {
                  chatDevice = site.shareName; // Fallback for public requests
                  qInfo() << "CHAT_PREFIX: Using fallback AI node" << chatDevice << "for session" << m_req->sessionId();
             } else {
                  qCritical() << "CHAT_PREFIX: No public AI node available for host" << host << "session" << m_req->sessionId();
             }
        }
        if (! chatDevice.isEmpty())  {
            result = true;
            bool addSlash = urlPath.endsWith("/");
            QStringList rpcList = m_requestedItemPathList;
            rpcList.removeFirst();
            rpcList.prepend("chat"); //RPC call type
            m_rpcCallPath = rpcList.join("/");
            m_rpcCall = true;
            m_requestedItemPathList.clear();
            m_requestedItemPathList.append(chatDevice);
            if (addSlash)
                m_rpcCallPath.append("/");
        } else {
            replyServiceUnavailable(tr("AI service is currently unavailable"));
            return true; // Handled
        }
#else
        // Embedded client: proxy /ai requests directly to local OnMyChat service
        m_localAiProxy = true;
        QString aiBase = m_Settings->value("AiServer", DEFAULT_AI_URL).toString();
        QString proxyPath = urlPath.mid(QString(CHAT_PREFIX).length());
        if (proxyPath.isEmpty()) proxyPath = "/";
        m_localAiProxyUrl = aiBase + proxyPath;
        qDebug() << "Proxying to local AI:" << m_localAiProxyUrl;
        result = true;
#endif
    }
    return result;
}

void HttpHandler::handleLocalAiProxy(const QByteArray& payload)
{
    m_reading = true;

    if (!m_externalAccessManager)
        m_externalAccessManager = new QNetworkAccessManager(this);

    QUrl proxyUrl(m_localAiProxyUrl);
    // Preserve query string from original request (e.g. ?size=360)
    QString queryString = m_req->url().query(QUrl::FullyEncoded);
    if (!queryString.isEmpty())
        proxyUrl.setQuery(queryString);
    QNetworkRequest request(proxyUrl);

    // Forward original request headers
    foreach(const QByteArray &key, m_req->headers().keys()) {
        QString lowerKey = QString::fromUtf8(key).toLower();
        if (lowerKey != "host" && lowerKey != "content-length" && lowerKey != "connection") {
            request.setRawHeader(key, m_req->headers()[key]);
        }
    }

    request.setRawHeader("X-OMD-Private-Mode", "1");

    // Set accept for streaming if path looks like streaming
    QString path = QUrl(m_localAiProxyUrl).path();
    if (isStreamingPath(path)) {
        request.setRawHeader("Accept", "text/event-stream");
    }

    QNetworkReply *reply = nullptr;
    if (m_req->method() == qhttp::EHTTP_POST) {
        reply = m_externalAccessManager->post(request, payload);
    } else if (m_req->method() == qhttp::EHTTP_PUT) {
        reply = m_externalAccessManager->put(request, payload);
    } else if (m_req->method() == qhttp::EHTTP_DELETE) {
        reply = m_externalAccessManager->deleteResource(request);
    } else if (m_req->method() == qhttp::EHTTP_PATCH) {
        reply = m_externalAccessManager->sendCustomRequest(request, "PATCH", payload);
    } else {
        reply = m_externalAccessManager->get(request);
    }

    if (!reply) {
        replyInternalServerError("Failed to create proxy request");
        return;
    }

    bool isStreaming = isStreamingPath(path);
    QPointer<HttpHandler> self(this);
    if (isStreaming) {
        // Stream SSE chunks back as they arrive
        connect(reply, &QNetworkReply::readyRead, this, [self, reply]() {
            if (!self || !self->m_working || self->m_resp.isNull()) {
                reply->deleteLater();
                return;
            }

            QByteArray chunk = reply->readAll();
            if (chunk.isEmpty()) return;

            if (!self->m_streaming) {
                // Forward response headers on first chunk
                QVariant statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
                const auto headers = reply->rawHeaderPairs();
                for (const auto &h : headers) {
                    QString lower = QString::fromUtf8(h.first).toLower();
                    if (lower != "content-length" && lower != "server" &&
                        lower != "access-control-allow-origin" && lower != "transfer-encoding") {
                        self->m_resp->addHeader(h.first, h.second);
                    }
                }
                if (!self->m_resp->headers().contains("Content-Type"))
                    self->m_resp->addHeader("Content-Type", "text/event-stream");
                self->m_resp->addHeader("X-Accel-Buffering", "no");
                self->m_resp->addHeader("Cache-Control", "no-cache");
                self->setServerString();
                self->m_streaming = true;
            }

            self->writeReply(chunk, qhttp::ESTATUS_OK);
        });

        connect(reply, &QNetworkReply::finished, this, [self, reply]() {
            if (self) {
                if (self->m_streaming) {
                    QByteArray lastChunk = reply->readAll();
                    if (!lastChunk.isEmpty())
                        self->writeReply(lastChunk, qhttp::ESTATUS_OK);
                }
                self->m_reading = false;
            }
            reply->deleteLater();
        });
    } else {
        // Non-streaming: wait for full response
        connect(reply, &QNetworkReply::finished, this, [self, reply]() {
            if (!self || !self->m_working || self->m_resp.isNull()) {
                reply->deleteLater();
                return;
            }

            QVariant statusCodeVariant = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
            int statusCode = statusCodeVariant.isValid() ? statusCodeVariant.toInt() : 200;
            QString contentType;
            QByteArray responseData = reply->readAll();

            const auto headers = reply->rawHeaderPairs();
            for (const auto &h : headers) {
                QString lower = QString::fromUtf8(h.first).toLower();
                if (lower == "content-type") contentType = QString::fromUtf8(h.second);
                if (lower != "content-length" && lower != "server" && lower != "access-control-allow-origin" && lower != "transfer-encoding") {
                    self->m_resp->addHeader(h.first, h.second);
                }
            }

            self->m_reading = false;
            if (statusCode >= 400) {
                qWarning() << "Local AI proxy error:" << statusCode << self->m_localAiProxyUrl;
            }
            self->endReply(responseData, contentType.toUtf8(), static_cast<qhttp::TStatusCode>(statusCode));
            reply->deleteLater();
        });
    }
}

void HttpHandler::doReinit()
{
    //reinit connection in link thread
    if (m_reinitConnection && !m_req.isNull() && !m_resp.isNull() && m_req->connection())
    {
        m_req->connection()->reinit();
        if (m_req->headers().contains("Expect")) /*&& m_req->collectedData().count() == 0)*/
        {
            m_resp->send100Continue();
            qDebug() << "REINIT: continue";
        }
    }

}

void HttpHandler::doWork()
{

    if (m_aborted.load(std::memory_order_acquire) || m_stopped.load(std::memory_order_acquire) || !m_working || m_released) {
        // disconnected before started
        releaseHandler();
        return;
    }

    if (!m_req || m_req.isNull())
    {
        m_working = false;
        qDebug() << "Done with it!!!" << m_req;
        releaseHandler();
        return;
    }
    if (! m_req->connection() || ! m_req->connection()->isAlive()) {
        m_working = false;
        //qDebug() << "Dead socket!!!";
        releaseHandler();
        return;
    }

    m_working = true;

    doReinit();


    if (!m_working || m_released) {
        // disconnected before started
        releaseHandler();
        return;
    }

    //qDebug() << this << "Request started:" << m_req->url().toString() << m_req->methodString();

    switch (m_req->method()) {
    case qhttp::EHTTP_DELETE:
		#ifdef SERVER
        handleDelete();
		#else
		replyNotImplemented();
		#endif
		break;
    case qhttp::EHTTP_GET:
        handleGet(false);
        break;
    case qhttp::EHTTP_HEAD:
        handleGet(true);
        break;
    case qhttp::EHTTP_POST:
        beginHandlePost();
        break;
    case qhttp::EHTTP_PUT:
        beginHandlePut();
        break;
    case qhttp::EHTTP_PATCH:
        replyNotImplemented();
        break;
    case qhttp::EHTTP_OPTIONS:
        handleOptions();
        break;
    case qhttp::EHTTP_COPY:
        //WebDAV on the gateway is retired.
        replyNotImplemented();
        break;
    case qhttp::EHTTP_MOVE:
        //WebDAV on the gateway is retired.
        replyNotImplemented();
        break;
    case qhttp::EHTTP_LOCK:
        replyNotImplemented();
        break;
    case qhttp::EHTTP_UNLOCK:
        replyNotImplemented();
        break;
    case qhttp::EHTTP_MKCOL:
    case qhttp::EHTTP_PROPFIND:
        //WebDAV on the gateway is retired: integrations go through the nodes
        //or omd-tunnel tunnels.
        replyNotImplemented();
        break;
    case qhttp::EHTTP_PROPPATCH:
        replyNotImplemented();
        break;
    case qhttp::EHTTP_CONNECT:
    case qhttp::EHTTP_TRACE:
    case qhttp::EHTTP_SEARCH:
    case qhttp::EHTTP_BIND:
    case qhttp::EHTTP_REBIND:
    case qhttp::EHTTP_UNBIND:
    case qhttp::EHTTP_ACL:
    case qhttp::EHTTP_REPORT:
    case qhttp::EHTTP_MKACTIVITY:
    case qhttp::EHTTP_CHECKOUT:
    case qhttp::EHTTP_MERGE:
    case qhttp::EHTTP_MSEARCH:
    case qhttp::EHTTP_NOTIFY:
    case qhttp::EHTTP_SUBSCRIBE:
    case qhttp::EHTTP_UNSUBSCRIBE:
    case qhttp::EHTTP_PURGE:
    case qhttp::EHTTP_MKCALENDAR:
    case qhttp::EHTTP_LINK:
    case qhttp::EHTTP_UNLINK:
        replyNotImplemented();
        break;
    }
}


HttpHandler::~HttpHandler()
{
	m_req = nullptr;
	m_resp = nullptr;
	m_buffer.close();
	m_bufferBytes.clear();
	m_putData.clear();
	m_postData.clear();
    m_nextDataPart.clear();
    m_bufferBytes.squeeze();
    m_postData.squeeze();
    m_bufferBytes.squeeze();
    m_nextDataPart.squeeze();
    m_putData.squeeze();

    if (m_streaming) {
        qWarning() << "Released while streaming";
    }

	if (m_reader)
	{
		delete m_reader;
		m_reader = nullptr;
    }

    if (m_rpcReader) {
        delete m_rpcReader;
        m_rpcReader = nullptr;
    }

    if (m_delayedOpTimer)
    {
          m_delayedOpTimer->stop();
          delete m_delayedOpTimer;
          m_delayedOpTimer = nullptr;
    }

    if ( m_externalAccessManager) {
        delete m_externalAccessManager;
        m_externalAccessManager = nullptr;
    }

    if (m_converting) {
        disconnect(m_converter, 0, 0, 0);
        m_converter->close();
        delete m_converter;
    }
    //if stopped due to link disconnect, resources are already released
    if (! m_stopped)
        m_resourceCollector.cleanup();

    m_rootResource->close();
}


void HttpHandler::receivePostData(const QByteArray &data)
{
	qint64 bytesWritten = 0;
	//qDebug() << "writing " << data.size() << " bytes of" << m_bytesToRecieve;


    if (! m_working || m_req.isNull() || m_resp.isNull() || !m_buffer.isOpen()) {
        releaseHandler();
        return;
    } //

	if (data.size() > 0) {
		bytesWritten = m_buffer.write(data);
        m_bytesToSend -= bytesWritten;
	} else {
		return;
	}
	//qDebug() << "Bytes to recieve: " << m_bytesToRecieve;
    if (m_bytesToSend == 0 || bytesWritten < data.size())
        endHandlePost();
}

void HttpHandler::replySendSignedCert(QByteArray signedCert)
{
    m_resp->addHeader("Content-Type", "application/pkix-cert");
    m_resp->addHeader("certfile", "client.pem");
    //m_resp->addHeader("Content-Disposition", "inline; name=\"client.crt\"; filename=\"client.crt\"");
    m_resp->addHeader("Content-Length", QString::number(signedCert.size()).toUtf8());

    setServerString();
    m_resp->setStatusCode(qhttp::ESTATUS_OK);
    m_resp->end(signedCert);
    releaseHandler();
}

void HttpHandler::replySendCa(QString caCert)
{
    QFile caFile(caCert);
    if (caFile.open(QIODevice::ReadOnly))
    {
        QByteArray caBody = caFile.readAll();
        m_resp->addHeader("Content-Type", "application/pkix-cert");
        m_resp->addHeader("cafile", "ca.pem");
        //m_resp->addHeader("Content-Disposition", "inline; name=\"ca.pem\"; filename=\"ca.pem\"");
        m_resp->addHeader("Content-Length", QString::number(caBody.size()).toUtf8());
        caFile.close();

        setServerString();
        m_resp->setStatusCode(qhttp::ESTATUS_OK);
        m_working = false;
        m_reading = false;
        m_resp->end(caBody);
        releaseHandler();
    }
    else
    {
        replyInternalServerError(QString("Can not open CA certificate file."));
	}
}

static QJsonObject fetchProfileFromGateway(const QString& omdToken, QSettings* settings)
{
    Q_UNUSED(settings)
    QJsonObject omdProfile;
    if (omdToken.isEmpty())
        return omdProfile;

    QEventLoop loop;
    QNetworkAccessManager* accessManager = new QNetworkAccessManager();

    QObject::connect(accessManager, &QNetworkAccessManager::sslErrors, [](QNetworkReply *reply, const QList<QSslError> &errors) {
        Q_UNUSED(errors);
        reply->ignoreSslErrors();
    });

    bool ok = false;
    QJsonObject response;
    QObject::connect(accessManager, &QNetworkAccessManager::finished, [&loop, &ok, &response](QNetworkReply *reply) {
        if (reply->error() == QNetworkReply::NoError) {
            response = QJsonDocument::fromJson(reply->readAll()).object();
            ok = true;
        } else {
            qCritical() << "OMD gateway profile fetch error:" <<  reply->errorString();
        }
        reply->deleteLater();
        loop.exit(0);
    });

#ifdef QT_DEBUG
    QString apiUrl = QString("http://localhost:8080/");
#else
#ifdef INNBOX
    QString apiUrl = QString("https://%1/").arg(settings->value(QStringLiteral("Network/server"), QString("storage.innbox.app")).toString());
#else
    QString apiUrl = QString("https://%1/").arg(settings->value(QStringLiteral("Network/server"), QString("disk.direct")).toString());
#endif
#endif

    QNetworkRequest apiRequest(apiUrl);
    apiRequest.setRawHeader("content-type", "application/json");
    apiRequest.setRawHeader("authorization", QString("token:%1").arg(omdToken).toUtf8());

    QJsonObject reqObj;
    reqObj["action"] = "getProfileData";
    reqObj["user"] = "";

    QJsonDocument requestData(reqObj);

    accessManager->post(apiRequest, requestData.toJson(QJsonDocument::Compact));

    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    timer.start(5000); // 5 seconds timeout

    loop.exec();

    if (timer.isActive()) {
        timer.stop();
    } else {
        qWarning() << "Timeout fetching profile from gateway";
    }

    delete accessManager;

    if (ok) {
        QJsonObject result = response["result"].toObject();
        if (result.value("success").toBool()) {
            omdProfile = result.value("profile").toObject();
            
            // Ensure rights field is also populated for compatibility with frontend expectations
            QJsonObject rightsObj;
            QJsonObject permissionsObj = omdProfile.value("permissions").toObject();
            rightsObj["ai_chat"] = permissionsObj.value("ai_chat").toBool();
            rightsObj["seats_allowed"] = permissionsObj.value("collab").toBool() ? 10 : 1;
            omdProfile["rights"] = rightsObj;
        }
    }
    return omdProfile;
}

void HttpHandler::endHandlePost()
{
    if (!m_working || m_req.isNull() || m_resp.isNull()) {
        releaseHandler();
        return;
    }

    QStringList reqPath = m_req->urlPath();

    disconnect(m_req.data(), &QHttpRequest::data, this, &HttpHandler::receivePostData);

    // Local AI proxy bypass (Embedded client - offline mode)
    if (m_localAiProxy) {
        m_buffer.seek(0);
        QByteArray payload = m_buffer.readAll();
        handleLocalAiProxy(payload);
        return;
    }

#ifdef SERVER
    // Public AI node (AI Assistant Public Node) discovery:
    // returns the connected system AI node so the frontend can attach
    // a guest P2P chat channel without pairing the device.
    if (reqPath.size() >= 2 && reqPath.at(0) == QStringLiteral("api") && reqPath.at(1) == QStringLiteral("ai-node"))
    {
        QJsonObject response;
        SiteInfo site;
        if (OnMyDiskLinkServer::instance().getPublicAiNode(site)) {
            QJsonObject aiNode;
            aiNode["linkID"] = site.linkID;
            aiNode["name"] = site.name;
            aiNode["shareName"] = site.shareName;
            aiNode["owner"] = site.owner;
            aiNode["type"] = QStringLiteral("ai-node");
            QJsonArray caps;
            caps.append(QStringLiteral("chat"));
            aiNode["capabilities"] = caps;
            aiNode["status"] = QStringLiteral("online");
            response["status"] = "ok";
            response["aiNode"] = aiNode;
        } else {
            response["status"] = "ok";
            response["aiNode"] = QJsonValue(QJsonValue::Null);
        }
        endReply(QJsonDocument(response).toJson(QJsonDocument::Compact), "application/json", qhttp::ESTATUS_OK);
        return;
    }

    if (reqPath.size() == 2 && reqPath.at(0) == QStringLiteral("api") && reqPath.at(1) == QStringLiteral("devices"))
    {
        const QString requester = m_userName;
        QJsonArray filterLinkIds;
        m_buffer.seek(0);
        QByteArray postData = m_buffer.readAll();
        if (!postData.isEmpty()) {
            QJsonDocument jsonDoc = QJsonDocument::fromJson(postData);
            if (jsonDoc.isArray()) {
                filterLinkIds = jsonDoc.array();
            } else if (jsonDoc.isObject()) {
                if (jsonDoc.object().contains("linkIds")) {
                    filterLinkIds = jsonDoc.object().value("linkIds").toArray();
                } else if (jsonDoc.object().contains("devices")) {
                    filterLinkIds = jsonDoc.object().value("devices").toArray();
                }
            }
        }

        QJsonArray devices = OnMyDiskLinkServer::instance().getConnectedDevices(requester, filterLinkIds);
        QJsonObject response;
        response["status"] = "ok";
        response["devices"] = devices;
        endReply(QJsonDocument(response).toJson(QJsonDocument::Compact), "application/json", qhttp::ESTATUS_OK);
        return;
    }

    if (reqPath.size() >= 2 && reqPath.at(0) == QStringLiteral("api") &&
        (reqPath.at(1) == QStringLiteral("my-nearby") ||
         (reqPath.at(1) == QStringLiteral("devices") && reqPath.size() >= 3 && reqPath.at(2) == QStringLiteral("nearby"))))
    {
        QString clientIp;
        if (m_req->headers().contains("x-forwarded-for")) {
            clientIp = QString::fromUtf8(m_req->headers()["x-forwarded-for"]).split(',').first().trimmed();
        } else if (m_req->headers().contains("X-Forwarded-For")) {
            clientIp = QString::fromUtf8(m_req->headers()["X-Forwarded-For"]).split(',').first().trimmed();
        } else {
            clientIp = m_req->remoteAddress();
        }

        QList<QJsonObject> nodes = OnMyDiskLinkServer::instance().getNearbyNodes(clientIp);
        QJsonArray nearbyArray;
        for (const auto &n : nodes) {
            nearbyArray.append(n);
        }

        QJsonObject response;
        response["status"] = "ok";
        response["clientIp"] = clientIp;
        response["devices"] = nearbyArray;
        endReply(QJsonDocument(response).toJson(QJsonDocument::Compact), "application/json", qhttp::ESTATUS_OK);
        return;
    }

    // WebRTC P2P DataChannel Signaling endpoint for SVAR browser frontend.
    // All parameters travel in the POST body - never in the URL query,
    // so linkIDs, SDP offers and ICE candidates don't leak into proxy logs.
    if (reqPath.size() >= 2 && reqPath.at(0) == QStringLiteral("api") && reqPath.at(1) == QStringLiteral("signaling"))
    {
        // Ownership is bound to the session: m_userName holds the resolved
        // owner key (user name, or session ID for guests) filled by checkAuth.
        // Guest sessions on public links legitimately have a session ID here;
        QString requester = m_userName.isEmpty() ? m_req->sessionId() : m_userName;
        if (requester.isEmpty())
        {
            requester = QStringLiteral("anonymous");
        }

        m_buffer.seek(0);
        QByteArray postData = m_buffer.readAll();
        QJsonDocument jsonDoc = QJsonDocument::fromJson(postData);
        if (!jsonDoc.isNull() && jsonDoc.isObject())
        {
            QJsonObject obj = jsonDoc.object();
            if (obj.contains("senderId") && !obj.value("senderId").toString().isEmpty()) {
                requester = obj.value("senderId").toString();
            } else if (obj.contains("token") && !obj.value("token").toString().isEmpty()) {
                requester = obj.value("token").toString();
            }
            QString linkID = obj.value("linkID").toString();
            QString ID = obj.value("ID").toString();
            QString kind = obj.value("kind").toString();
            QByteArray payload = QByteArray::fromBase64(obj.value("payload").toString().toUtf8());
            quint16 port = obj.contains("port") ? (quint16)obj.value("port").toInt() : 0;
            QString service = obj.value("service").toString();

            if (kind == "devices" || kind == "getDevices")
            {
                QJsonArray filterLinkIds;
                if (obj.contains("linkIds")) {
                    filterLinkIds = obj.value("linkIds").toArray();
                } else if (obj.contains("devices")) {
                    filterLinkIds = obj.value("devices").toArray();
                }
                QJsonArray devices = OnMyDiskLinkServer::instance().getConnectedDevices(requester, filterLinkIds);
                QJsonObject response;
                response["status"] = "ok";
                response["devices"] = devices;
                endReply(QJsonDocument(response).toJson(QJsonDocument::Compact), "application/json", qhttp::ESTATUS_OK);
                return;
            }

            // Web device polling: kind=poll with the sentinel ID drains
            // queued TunnelOpen signals targeted at this browser device
            if (kind == "poll" && !linkID.isEmpty() && ID == "client-device-poll")
            {
                QList<WebSignal> tunnelSignals = OnMyDiskLinkServer::instance().getPendingTunnelOpen(linkID, requester, 0);
                QJsonObject response;
                response["status"] = "ok";
                QJsonArray sigArray;
                for (const auto &sig : tunnelSignals)
                {
                    QJsonObject sigObj;
                    sigObj["kind"] = sig.kind;
                    sigObj["tunnelID"] = sig.tunnelID;
                    sigObj["payload"] = QString::fromUtf8(sig.payload.toBase64());
                    sigArray.append(sigObj);
                }
                response["signals"] = sigArray;
                endReply(QJsonDocument(response).toJson(QJsonDocument::Compact), "application/json", qhttp::ESTATUS_OK);
                return;
            }

            if (!linkID.isEmpty() && !ID.isEmpty())
            {
                bool ok = OnMyDiskLinkServer::instance().sendWebSignaling(linkID, ID, kind, payload, requester, port, service, m_userName);
                if (!ok)
                {
                    replyJsonError(tr("Target node not connected or access denied"));
                    return;
                }

                // Return signals only when polling; sending an offer/answer must not consume its own queued message
                QList<WebSignal> webSignalsList;
                if (kind == QStringLiteral("poll"))
                {
                    webSignalsList = OnMyDiskLinkServer::instance().getWebSignals(ID, requester, 0);
                }

                QJsonObject response;
                response["status"] = "ok";
                QJsonArray sigArray;
                for (const auto &sig : webSignalsList)
                {
                    QJsonObject sigObj;
                    sigObj["kind"] = sig.kind;
                    sigObj["tunnelID"] = sig.tunnelID;
                    sigObj["payload"] = QString::fromUtf8(sig.payload.toBase64());
                    sigArray.append(sigObj);
                }
                response["signals"] = sigArray;

                endReply(QJsonDocument(response).toJson(QJsonDocument::Compact), "application/json", qhttp::ESTATUS_OK);
                return;
            }
        }
        replyJsonBadRequest(tr("Invalid signaling parameters"));
        return;
    }

    // Web device registration — browser PWA registers its linkId so the gateway
    // can route incoming TunnelOpen messages to it.
    if (reqPath.size() >= 3 && reqPath.at(0) == QStringLiteral("api") && reqPath.at(1) == QStringLiteral("device") && reqPath.at(2) == QStringLiteral("register"))
    {
        m_buffer.seek(0);
        QByteArray postData = m_buffer.readAll();
        QJsonDocument jsonDoc = QJsonDocument::fromJson(postData);
        if (!jsonDoc.isNull() && jsonDoc.isObject())
        {
            QJsonObject obj = jsonDoc.object();
            QString linkId = obj.value("linkId").toString();
            if (linkId.isEmpty()) {
                replyJsonBadRequest(tr("Missing linkId"));
                return;
            }

            OnMyDiskLinkServer::instance().registerWebDevice(linkId, m_userName.isEmpty() ? linkId : m_userName);

            QJsonObject response;
            response["status"] = "ok";
            endReply(QJsonDocument(response).toJson(QJsonDocument::Compact), "application/json", qhttp::ESTATUS_OK);
            return;
        }
        replyJsonBadRequest(tr("Invalid registration parameters"));
        return;
    }
#endif

    //Custom forms from sites - send form to site's owner
     //create user from web client: pass postdata to server
     #ifdef SERVER
    if (reqPath.size() == 1 && reqPath[0] == QStringLiteral("submitform"))
    {
        QHash<QByteArray, QByteArray> postData;
        if (parsePostData(&postData) ) {
            StringsHash formData;
            foreach(QByteArray key, postData.keys())
                formData[QString::fromUtf8(key)] = QString::fromUtf8(postData[key]);
            OnMyDiskMailer::instance().sendCustomForm(m_rootOwner, formData);
            replyJsonSuccess();
        } else
            replyJsonError(tr("Ivalid form posted"));
        return;
    }

    // RPC POST invokation - calling local apps over OMD proxy
    if (m_rpcCall) {
        m_buffer.seek(0);
        QByteArray payload = m_buffer.readAll();;
        QHash<QString, QVariant> params;

        CloudResource* requestedResource = m_rootResource->getChildClone(m_requestedItemPathList);

        if (! requestedResource)
        {
            replyNotFound();
            return;
        }
        m_resourceCollector.watchFor(requestedResource);

        if (!m_working || m_req.isNull() || m_resp.isNull()) {
            releaseHandler();
            return;
        }

        //add headers
        foreach(const QByteArray &key, m_req->headers().keys()) {
            QString keyStr = QString::fromUtf8(key);
            QString lowerKey = keyStr.toLower();
            if (lowerKey == "content-type")
                 params["Content-Type"] = m_req->headers()[key];
            else if (lowerKey != "content-length")
                 params[keyStr] = m_req->headers()[key];
        }
        
        // Inject Token Balance for rights enforcement
        QPointer<ClientSession> session = m_sessionAuthProvider->getSession(m_req->sessionId());
        if (session) {
            params["X-OMD-Token-Balance"] = QString::number(session->getVar("token_balance", 0.0).toDouble());

#ifdef SERVER
            // Private mode for the chat node owner or invited users
            QString userName = session->userName();
            if (!userName.isEmpty()) {
                SiteInfo site;
                if (OnMyDiskLinkServer::instance().getChatNode(userName, site)) {
                    bool isOwner = (userName == site.owner);
                    bool isSharedTo = site.users.contains(userName);
                    if (isOwner || isSharedTo) {
                        params["X-OMD-Private-Mode"] = "1";
                    }
                }
            }
#endif
        }
        //add query
        params["query"] = m_req->url().query();
        params["action"] = "post";
        if (isStreamingPath(m_rpcCallPath)) {
             params["accept"] = "text/event-stream";
        }
        m_reading = true;


        m_rpcReader = new FileReader(requestedResource, this);
        connect(m_rpcReader, &FileReader::responseReady, this, [this] (const QHash<QString, QVariant> &params, const QByteArray& payload) {

            if (!m_working || m_resp.isNull()) {
                releaseHandler();
                return;
            }

            int statusCode =  params["statuscode"].toInt();
            QString contentType = "";
            foreach(const QString &key, params.keys()) {
                QString lower = key.toLower();
                if (lower == "content-type") contentType = params[key].toString();
            }

            //Populate headers
            foreach(const QString &key, params.keys()) {
                QString lower = key.toLower();
                if (lower != "statuscode" &&
                    lower != "content-length" &&
                    lower != "stream" &&
                    lower != "server" &&
                    lower != "access-control-allow-origin")
                    {
                        m_resp->addHeader(key.toUtf8(), params[key].toString().toUtf8());
                    }
            }
            bool streaming = params.contains("stream") && params["stream"].toBool();

            QString transferEncoding = params.contains("Transfer-Encoding") ? params["Transfer-Encoding"].toString() : params.contains("transfer-encoding") ?  params["transfer-encoding"].toString() : "";
            bool chunked = transferEncoding == "chunked";
            bool isDone = (statusCode == 204) || (params.contains("stream") && !params["stream"].toBool());

            //default 200
            //qInfo() << m_rpcCallPath << contentType << statusCode << streaming << chunked;

            if (statusCode == 0) {
                replyJsonError("RPC error");
                return;
            }

            if (! streaming || isDone) {

                    if (statusCode >= 400)
                        qWarning() << "RPC error:" << statusCode << m_rpcCallPath;

                    if (chunked) {
                        endChunkedReply(payload, qhttp::ESTATUS_OK);
                    } else if (statusCode == 304)
                        replyNotModified();
                    else if (statusCode == 204)
                        replyNoContent();
                    else
                        endReply(payload, contentType, static_cast<qhttp::TStatusCode>(statusCode));

                    m_reading = false;
            } else {
                if (!m_streaming) {
                    if (!contentType.isEmpty())
                        m_resp->addHeader("Content-Type", contentType.toUtf8()); // text/event-stream
                    m_resp->addHeader("X-Accel-Buffering", "no");
                    m_resp->addHeader("Cache-Control", "no-cache");
                    setServerString();
                    m_streaming = true;
                }

                if (chunked) {
                    writeChunk(payload,  static_cast<qhttp::TStatusCode>(statusCode));
                } else {
                    writeReply(payload,  static_cast<qhttp::TStatusCode>(statusCode));
                }
            }
        });

        if (!m_rpcReader->invoke(m_rpcCallPath, params, payload)) {
            replyNotFound();
        }
        return;
    }
    #endif





    if (reqPath.size() > 1 && reqPath.at(1) == QStringLiteral("omdrights"))
    {
        m_Settings->beginGroup("Settings");
        QString omdUser = m_Settings->value("user").toString();
        QString omdToken = m_Settings->value("omdToken").toString();
        if (omdToken.isEmpty()) {
            omdToken = m_Settings->value("storageId").toString();
        }
        m_Settings->endGroup();

        // Read token from POST data if provided
        m_buffer.seek(0);
        m_postData = m_buffer.readAll();
        QJsonDocument postJson = QJsonDocument::fromJson(m_postData);
        if (!postJson.isNull() && postJson.isObject()) {
            QJsonObject postObj = postJson.object();
            if (postObj.contains("token")) {
                QString customToken = postObj.value("token").toString();
                if (!customToken.isEmpty()) {
                    omdToken = customToken;
                    omdUser = "anonymous"; // force using token as identifier
                }
            }
        }

        qInfo() << "Local API /omdrights requested for user:" << omdUser << "token:" << omdToken;

        QJsonObject rightsProfile = fetchProfileFromGateway(omdToken, m_Settings);

        QJsonObject response;
        QJsonObject result;
        if (!rightsProfile.isEmpty()) {
            result["success"] = true;
            result["profile"] = rightsProfile;
        } else {
            result["success"] = false;
            result["message"] = "Invalid token or unable to fetch profile";
        }
        response["result"] = result;

        QJsonDocument responseDoc(response);
        endJsonReply(responseDoc.toJson(QJsonDocument::Compact), qhttp::ESTATUS_OK);
        return;
    }

    //create user from web client: pass postdata to server
    if (!m_isServerNode && reqPath.size() > 1 && reqPath.at(1) == QStringLiteral("createaccount"))
    {
        QUrl nextUrl = m_req->url();
        QHash<QByteArray, QByteArray> postData;

        if (m_req->headers()["content-length"].toULongLong() == 0)
        {
            nextUrl.setPath(m_sessionAuthProvider->loginPagePath());
            replySeeOther(nextUrl);
            return;
        }

        if (m_req->headers()["content-length"].toULongLong() > 0 && !parsePostData(&postData) )
        {
            nextUrl.setPath(m_sessionAuthProvider->loginPagePath());
            replySeeOther(nextUrl);
            return;
        }

        if (!postData.contains("server"))
        {
            nextUrl.setPath(m_sessionAuthProvider->loginPagePath());
            replySeeOther(nextUrl);
            return;
        }

        if (!postData.contains("login"))
        {
            nextUrl.setPath(m_sessionAuthProvider->loginPagePath());
            replySeeOther(nextUrl);
            return;
        }

        if (!postData.contains("password"))
        {
            nextUrl.setPath(m_sessionAuthProvider->loginPagePath());
            replySeeOther(nextUrl);
            return;
        }

       //send original data
       if ( m_sessionAuthProvider->createUserAccount(postData.value("server").trimmed(), postData.value("login").trimmed(), postData.value("password").trimmed(),  m_postData))
       {
           replyJsonSuccess();
       }
       else
           replyJsonError(tr("Error creating account. Username or email are already used."));

       return;
    }

    // Handle Subscription Creation
    if (!m_isServerNode && reqPath.size() > 2 && reqPath.at(1) == QStringLiteral("subscriptions") && reqPath.at(2) == QStringLiteral("create"))
    {
        //Collab subscriptions moved to the legacy gateway during the GunDB migration
        replyJsonNotImplemented();
        return;
    }

/*
    if (reqPath.size() > 0 && reqPath.at(0) == QStringLiteral("login"))
    {
        QHash<QByteArray, QByteArray> postData;
        //qInfo() << "lofin" ;
        QUrl nextUrl = m_req->url();
        //nextUrl.setHost(domain);
		nextUrl.setPath(m_sessionAuthProvider->loginPagePath());

        if (m_req->headers()["content-length"].toULongLong() == 0)
        {
            replySeeOther(nextUrl);
            return;
        }

        if (m_req->headers()["content-length"].toULongLong() > 0 && !parsePostData(&postData))
        {
            replySeeOther(nextUrl);
            return;
        }

        replySeeOther(nextUrl);
        return;
    }
    */

    #ifdef SERVER
    //shared link password
    if (reqPath.size() > 0 && reqPath.at(0) == QStringLiteral("key"))
    {
        QHash<QByteArray, QByteArray> postData;
        QString referer = m_req->headers()["Referer"];
        QUrl nextUrl = QUrl(referer);
        bool isAjax = (m_req->headers().contains("Accept") && m_req->headers()["Accept"].contains("application/json")) ||
                      (m_req->headers().contains("accept") && m_req->headers()["accept"].contains("application/json")) ||
                      (m_req->headers().contains("X-Requested-With") && m_req->headers()["X-Requested-With"].contains("XMLHttpRequest")) ||
                      (m_req->headers().contains("x-requested-with") && m_req->headers()["x-requested-with"].contains("XMLHttpRequest"));

        qInfo() << "[KEY-ROUTE] Incoming request. isAjax:" << isAjax << "Referer:" << referer << "Content-length:" << m_req->headers()["content-length"];

        if (m_req->headers()["content-length"].toULongLong() == 0 || !parsePostData(&postData))
        {
            qWarning() << "[KEY-ROUTE] Parse failed or empty body!";
            if (isAjax) {
                replyJsonError(tr("Invalid request"));
            } else {
                replySeeOther(nextUrl);
            }
            return;
        }

        qInfo() << "[KEY-ROUTE] Parsed keys:" << postData.keys();

        QString sharedPath = QUrl(referer).path();
        int sp = sharedPath.lastIndexOf(":");
        if (sp != -1) {
             sharedPath = sharedPath.left(sp);
        }

        if (postData.contains("key"))
        {
            QString sessionID = m_sessionAuthProvider->getSessionIdForSharedPath(sharedPath, m_req);
            QString user_key = postData.value("key");
            QString tos = postData["tos"].trimmed();
            quint64 key = SimpleCrypt::stringToHash(user_key);
            SimpleCrypt crypt(key);
            QString decodedLink = crypt.decryptToString(sharedPath.mid(8));

            qInfo() << "[KEY-ROUTE] Shared path:" << sharedPath << "Key hash:" << key << "Decoded:" << decodedLink << "TOS:" << tos;

            if (decodedLink == "" || tos != "yes") {
                //redirect to link password page
                qCritical() << "[KEY-ROUTE] Invalid key or missing TOS agreement. Path:" << sharedPath << "Key:" << user_key << "TOS:" << tos;
                if (isAjax) {
                    replyJsonError(tr("Invalid password"));
                } else {
                    replySeeOther(nextUrl);
                }
                return;
            }
            int p = decodedLink.indexOf(":");

            //invalid link
            if (p == -1) {
                qCritical() << "[KEY-ROUTE] Decrypted link invalid format:" << decodedLink;
                if (isAjax) {
                    replyJsonError(tr("Invalid link"));
                } else {
                    replySeeOther(nextUrl);
                }
                return;
            }

            // Successfully authorized guest
            QPointer<ClientSession> session = m_sessionAuthProvider->addSessionFor(sessionID, 7200000, sessionID); //2 hours
            session->setVar("key", key);
            qInfo() << "[KEY-ROUTE] Successfully authorized guest access. Session:" << sessionID;
            if (isAjax) {
                replyJsonSuccess();
            } else {
                replySeeOther(nextUrl);
            }
            return;
        } else {
            qWarning() << "[KEY-ROUTE] Request missing password key!";
            if (isAjax) {
                replyJsonError(tr("Password is required"));
            } else {
                replySeeOther(nextUrl);
            }
        }
        return;
    }

    if (reqPath.size() > 1 && reqPath.at(1) == QStringLiteral("checkauth"))
    {
        m_resp->addHeader("authentication-success", "success");
        replyJsonSuccess();
        return;
    }

    if (reqPath.size() > 1 && reqPath.at(1) == QStringLiteral("getca"))
    {
        QString caCert = m_Settings->value("PKI/CACert", "rootCA.crt").toString();
        replySendCa(caCert);
        return;
    }

	if (reqPath.size() > 1 && reqPath.at(1) == QStringLiteral("removekey"))
	{
		//Device keys are not tracked by the gateway anymore: identities live in GunDB
		replyJsonSuccess();
		return;
	}

    if (reqPath.size() > 1 && reqPath.at(1) == QStringLiteral("signcsr"))
    {
        QHash<QByteArray, QByteArray> postData;

        if (m_req->headers()["content-length"].toULongLong() > 0 && !parsePostData(&postData))
        {
            replyBadRequest("Wrong content type");
            return;
        }

        if (!postData.contains("csr"))
        {
            replyBadRequest(tr("Missing csr data."));
            return;
        }

        if (!postData.contains("user"))
        {
            replyBadRequest(tr("Missing user."));
            return;
        }

        QByteArray signedCert;
        if (!signCSR(postData["csr"], signedCert))
        {
            replyInternalServerError(tr("Can not add certificate to DB"));
            return;
        }

        //Device-to-user binding is not tracked by the gateway anymore:
        //identities live in GunDB, the node pairs via DID on its own.
        replySendSignedCert(signedCert);
        return;
    }

    //note: m_userName contains device ID for rest calls from clients
    if (reqPath.size() > 1 && reqPath.at(1) == QStringLiteral("revokecrt"))
    {
        //Device keys are not tracked by the gateway anymore: identities live in GunDB
        replyJsonSuccess();
        return;
    }


    //create user
    if (reqPath.size() > 1 && reqPath.at(1) == QStringLiteral("adduser"))
    {
        //Accounts are created on the legacy gateway during the GunDB migration
        replyJsonError(tr("Account registration is handled by the legacy gateway."));
        return;
    }

    //Reset user
    if (reqPath.size() > 1 && reqPath.at(1) == QStringLiteral("reset"))
    {
        //Password reset is handled by the legacy gateway during the GunDB migration
        replyJsonError(tr("Password reset is handled by the legacy gateway."));
        return;
    }

    #endif

    if (m_req->headers()["content-length"].toULongLong() == 0) {
        qWarning() << "Empty post from" << m_req->remoteAddress();
        replyBadRequest("Invalid post");
        return;
    }

    if (m_buffer.size() == 0) {
        qWarning() << "Empty post buffer from" << m_req->remoteAddress();
        replyBadRequest("Invalid post");
        return;
    }

    if (!m_working || m_req.isNull() || m_resp.isNull()) {
        releaseHandler();
        return;
    }


    m_buffer.seek(0);
    QByteArray data = m_buffer.readAll();
    data.replace("\n", ""); data.replace("\r", ""); // strip post data
    QJsonDocument postData = QJsonDocument::fromJson(data);
    if (! postData.isObject()) {
        qCritical() << "Invalid object posted:" << data;
        replyBadRequest("Not an object");
        return;
    }

    QJsonObject request = postData.object();

    QString path = m_req->url().path(QUrl::FullyDecoded);

    if (path.startsWith("/webhooks/") || path.startsWith(SIGNED_WEBHOOK_PREFIX)) {

        // Sync Rights Webhook
        if (path.endsWith("/sync-rights")) {
            QString signature = m_req->headers()["X-OmD-Signature"];
            QString secret = m_sessionAuthProvider->crawlerToken();
            if (!secret.isEmpty() && !signature.isEmpty()) {
                 QByteArray signedPayload = QMessageAuthenticationCode::hash(postData.toJson(), secret.toUtf8(), QCryptographicHash::Sha256).toHex();
                 if (signedPayload == signature) {
                     QString userOmdId = request["user_omd_id"].toString();
                     QJsonObject rights = request["rights"].toObject();
                     // Add extra fields to rights object
                     rights["status"] = request["status"].toString();
                     rights["group_name"] = request["group_name"].toString();
                     rights["is_owner"] = request["is_owner"].toBool();
                     rights["token_balance"] = request["token_balance"].toDouble();

                     if (!userOmdId.isEmpty()) {
                         qInfo() << "OMD: Received sync-rights for user:" << userOmdId << "Status:" << rights["status"].toString();
                         m_sessionAuthProvider->updateUserRights(userOmdId, rights);
                         // Notify client about rights update
                         UserEvents::instance().userEvent(userOmdId, "account", rights);
                         qInfo() << "OMD: Emitted userEvent account for" << userOmdId;
                         return replyJsonSuccess();
                     } else {
                         qWarning() << "OMD: Missing user_omd_id in sync-rights payload";
                         return replyJsonBadRequest("Missing user_omd_id");
                     }
                 } else {
                     return replyJsonForbidden();
                 }
            } else {
                 return replyJsonForbidden();
            }
        }
        
        // Collab Provisioning Webhook
        if (path.endsWith("/provision-group")) {
            QString signature = m_req->headers()["X-OmD-Signature"];
            QString secret = m_sessionAuthProvider->crawlerToken();
            if (!secret.isEmpty() && !signature.isEmpty()) {
                 QByteArray signedPayload = QMessageAuthenticationCode::hash(postData.toJson(), secret.toUtf8(), QCryptographicHash::Sha256).toHex();
                 if (signedPayload == signature) {
                     bridgeProvisionGroup(request);
                     return replyJsonSuccess();
                 } else {
                     return replyJsonForbidden();
                 }
            } else {
                 return replyJsonForbidden();
            }
        }


        return replyJsonSuccess();
    }


    if ( request["action"].toString() == "unlock" )
    {
        QString user_key = request["key"].toString();
        bool tos = request["tos"].toBool();
        QString sharedPath = request["sharedPath"].toString();

        if (sharedPath.isEmpty()) {
             sharedPath = m_req->url().path();
        }
        if (!sharedPath.startsWith("/shared/") && m_req->headers().contains("Referer")) {
             sharedPath = QUrl(m_req->headers()["Referer"]).path();
        }

        int sp = sharedPath.lastIndexOf(":");
        if (sp != -1) {
             sharedPath = sharedPath.left(sp);
        }

        qInfo() << "[UNLOCK-ACTION] Incoming unlock request. sharedPath:" << sharedPath << "key length:" << user_key.length() << "tos:" << tos;

        if (!sharedPath.startsWith("/shared/")) {
             replyJsonError(tr("Invalid shared path"));
             return;
        }

        if (user_key.isEmpty()) {
             replyJsonError(tr("Password is required"));
             return;
        }

        if (!tos) {
             replyJsonError(tr("You must agree to the Terms of Service"));
             return;
        }

        QString linkPath = sharedPath.mid(8); // remove "/shared/"
        QString sessionID = m_sessionAuthProvider->getSessionIdForSharedPath(linkPath, m_req);
        quint64 key = SimpleCrypt::stringToHash(user_key);
        SimpleCrypt crypt(key);
        QString decodedLink = crypt.decryptUrlEncodedToString(linkPath);

        qInfo() << "[UNLOCK-ACTION] Attempting decryption. Path:" << sharedPath << "Key hash:" << key << "Decoded:" << decodedLink;

        if (decodedLink == "" || decodedLink.indexOf(":") == -1) {
             qCritical() << "[UNLOCK-ACTION] Invalid key. Path:" << sharedPath << "Key:" << user_key;
             replyJsonError(tr("Invalid password"));
             return;
        }

        int p = decodedLink.indexOf(":");
        QString linkId = decodedLink.left(p);
        QString shareName = "";
        QString shareVersion = "";
        QString rootOwnerID = "anonymous";
        QString linkID = linkId;
        QString itemPath = decodedLink.mid(p + 1);

        QPointer<OnMyDiskLink> clientLink = nullptr;
        #ifdef SERVER
        clientLink = OnMyDiskLinkServer::instance().getLink(linkId);
        #endif
        if (clientLink) {
            rootOwnerID = clientLink->user();
            shareName = clientLink->shareName();
            shareVersion = clientLink->linkVersion();
        }
        #ifdef SERVER
        if (clientLink.isNull()) {
            QSharedPointer<ClientNotifier> notifier = NotificationManager::instance().getLinkNotifier(linkId);
            if (notifier) {
                rootOwnerID = notifier->getUser();
                shareName = notifier->shareName();
                shareVersion = notifier->version();
            }
        }
        #endif

        bool directShare = false;
        QString rights = "readonly";

        // Successfully authorized guest
        QPointer<ClientSession> session = m_sessionAuthProvider->addSessionFor(sessionID, 7200000, sessionID); //2 hours
        session->setVar("key", key);

        QHash<QString, QVariant> sessionVars = session->getSessionVariables();
        sessionVars["User"] = QStringLiteral("anonymous");
        sessionVars["Link"] = linkID;
        sessionVars["Item"] = itemPath;
        sessionVars["Owner"] = rootOwnerID;
        sessionVars["LinkUrl"] = sharedPath;
        sessionVars["CanMount"] = false;
        sessionVars["isGuest"] = true;
        sessionVars["directShare"] = directShare;
        sessionVars["shareName"] = shareName;
        sessionVars["version"] = shareVersion;
        sessionVars["rights"] = rights;
        sessionVars["locked"] = false;
        session->setSessionVariables(sessionVars);

        qInfo() << "[UNLOCK-ACTION] Successfully authorized guest access. SessionID:" << sessionID;

        replyJsonSuccess();
        return;
    }

    if ( request["action"].toString() == "list")
    {
        return bridgeList(request);
    }

    if ( request["action"].toString() == "getContent" )
    {
        return bridgeGetContent(request);
    }

    if ( request["action"].toString() == "getAttributes" )
    {
        return bridgeGetAttributes(request);
    }

    if ( request["action"].toString() == "invokeClient")
    {
        return bridgeinvokeClient(request);
    }
    // Support of 'robots.txt' in folders
    if ( request["action"].toString() == "excludeFromIndexing" )
    {
        return bridgeExcludeFromIndexing(request);
    }

    if ( request["action"].toString() == "createNew" )
    {
        return bridgeCreateNew(request);
    }

    if ( request["action"].toString() == "subscribe" )
    {
        return bridgeSubscribe(request);
    }

    if ( request["action"].toString() == "subscriptionSuccess" )
    {
        return bridgeSubscriptionSuccess(request);
    }

    if ( request["action"].toString() == "unsubscribe" )
    {
        return bridgeUnsubscribe(request);
    }

    if ( request["action"].toString() == "createSite" )
    {
        return bridgeCreateSite(request);
    }

    if ( request["action"].toString() == "getVideoAttributes" )
    {
        return bridgeGetVideoAttributes(request);
    }

    if ( request["action"].toString() == "transcodeVideo" )
    {
        return bridgeTranscodeVideo(request);
    }

    if ( request["action"].toString() == "downscaleVideo" )
    {
        return bridgeDownscaleVideo(request);
    }

    if ( request["action"].toString() == "transcodeAbort" )
    {
        return bridgeTranscodeAbort(request);
    }

    if ( request["action"].toString() == "getTranscodeProgress" )
    {
        return bridgeGetTranscodeProgress(request);
    }

    if ( request["action"].toString() == "transcodeAbort" )
    {
        return bridgeTranscodeAbort(request);
    }

    if ( request["action"].toString() == "rename" )
    {
        return bridgeRename(request);
    }

    if (request["action"].toString() == "move")
    {
        return bridgeMove(request);
    }

    if ( request["action"].toString() == "copy" )
    {
        return bridgeCopy(request);
    }

    if ( request["action"].toString() == "remove" )
    {
        return bridgeRemove(request);
    }

    if ( request["action"].toString() == "edit" )
    {
        return bridgeEdit(request);
    }

    if ( request["action"].toString() == "createFolder" )
    {
        return bridgeMkCol(request);
    }

    if ( request["action"].toString() == "shareByLink" )
    {
        return bridgeShareByLink(request);
    }

    if ( request["action"].toString() == "removeSharedLink" )
    {
        return bridgeRemoveSharedLink(request);
    }

    if ( request["action"].toString() == "unshareByLink" )
    {
        return bridgeUnshareByLink(request);
    }

    if ( request["action"].toString() == "sharePathToSession" )
    {
        return bridgeSharePathToSession(request);
    }

    if ( request["action"].toString() == "unsharePathFromSession" )
    {
        return bridgeUnsharePathFromSession(request);
    }

    if ( request["action"].toString() == "inviteUsersToCall" )
    {
        return bridgeInviteUsersToCall(request);
    }

    if ( request["action"].toString() == "connectDevice" )
    {
        return bridgeConnectDevice(request);
    }

#ifdef SERVER
    // Web device registration — browser PWA registers its linkId so the gateway
    // can route incoming TunnelOpen messages to it.
    if ( request["action"].toString() == "registerDevice" )
    {
        QString linkId = request["linkId"].toString();
        if (linkId.isEmpty())
        {
            QJsonObject response;
            QJsonObject result;
            result["success"] = false;
            QJsonObject error;
            error["message"] = "Missing linkId";
            response["result"] = result;
            response["error"] = error;
            endJsonReply(QJsonDocument(response).toJson(QJsonDocument::Compact), qhttp::ESTATUS_BAD_REQUEST);
            return;
        }

        OnMyDiskLinkServer::instance().registerWebDevice(linkId, m_userName.isEmpty() ? linkId : m_userName);

        QJsonObject response;
        QJsonObject result;
        result["success"] = true;
        result["linkId"] = linkId;
        response["result"] = result;
        endJsonReply(QJsonDocument(response).toJson(QJsonDocument::Compact), qhttp::ESTATUS_OK);
        return;
    }
#endif

    if ( request["action"].toString() == "getProfileData" )
    {
        return bridgeGetProfileData(request);
    }

    if ( request["action"].toString() == "getPublicProfile" )
    {
        return bridgeGetPublicProfile(request);
    }

    if ( request["action"].toString() == "updateProfileData" )
    {
        return bridgeUpdateProfileData(request);
    }

	if ( request["action"].toString() == "createProfile" )
	{
		return bridgeCreateProfile(request);
	}

    if ( request["action"].toString() == "getUserInfo" )
    {
        return bridgeGetUserInfo(request);
    }

    if ( request["action"].toString() == "signin" )
    {
        return bridgeSignIn(request);
    }

    if ( request["action"].toString() == "sessionlogin" )
    {
        return bridgeSessionLogin(request);
    }

    if ( request["action"].toString() == "invite" )
    {
        return bridgeInvite(request);
    }

    if ( request["action"].toString() == "signout" )
    {
        QString session_id = !request["session_id"].toString().isEmpty() ? request["session_id"].toString() : m_req->sessionId();
        endSession(session_id );

        QString domain = m_req->url().host();
        m_resp->addHeader("Set-Cookie", QString("%1=deleted; Domain=%2; path=/; expires=Thu, 01 Jan 1970 00:00:00 GMT").arg(m_sessionAuthProvider->globalCookieID(), domain).toUtf8());

        QJsonObject response;
        QJsonObject result;
        result["success"] = true;
        result["guest_token"] = m_sessionAuthProvider->getSessionIdForSharedPath("", m_req);
        response["result"] = result;
        QJsonDocument responseData(response);
        endJsonReply(responseData.toJson(QJsonDocument::Compact), qhttp::ESTATUS_OK);
        return;
        #ifdef QT_DEBUG
        //uncomment for profiling memory
        //m_exiting = true; uncomment for profiling memory
        #endif
        return;
    }

    if ( request["action"].toString() == "signinwithgoogle" )
    {
        return bridgeSignInWithGoogle(request);
    }

    if ( request["action"].toString() == "relogin" )
    {
        return bridgeRelogin(request);
    }

    qCritical() << "Bad POST request:" << request ;


    if (m_req->headers().contains("accept") && m_req->headers()["accept"].contains("json"))
    {
        replyJsonBadRequest("Wrong path");
        return;
    }

    replyBadRequest("Wrong request");
    return;
}

void HttpHandler::bridgeList(QString path)
{
    if (path.startsWith("..") || path.startsWith("/.")) {
        replyJsonBadRequest("Wrong path");
        return;
    }

    // result type depends on action. for list it is array
    //QJsonArray list;
    QJsonObject folder;

    m_reading = true;
	//Get child entries and make loop
    QStringList itemPath = path.split("/", Qt::SplitBehaviorFlags::SkipEmptyParts);

    int batchSize = 50;
    int offset = 0;
    bool partial = false;
    bool isChunked = false;
    QJsonArray list;
    int count = 0;

    //Initial request
    QString query = QString("fromPos=%1&count=%2").arg(offset).arg(batchSize);
    CloudResource* item = m_rootResource->getChildClone(itemPath, query, true);

    if (item == nullptr)
    {
        qCritical() << "Path not found" << path;
        replyNotFound();
        return;
    }

    m_resourceCollector.watchFor(item);

    if (! m_working) {
        releaseHandler();
        return;
    } //check if already dropped

    QStringList entryList = item->entryList();

    if (! m_working) {
        releaseHandler();
        return;
    } //check if already dropped


    //List device content for root request in case of single device
    #ifdef SERVER
    if (m_keyLogin && !m_rootResource->isSharedRoot() && entryList.size() == 1 && (!CloudResource::isUnlisted(entryList[0])) && path == "")
    #else
    if (m_keyLogin && entryList.size() == 1 &&  path == "")
    #endif
    {
        QString firstDevice = entryList[0];
        if ( ! firstDevice.isEmpty() ) {
            itemPath.append(firstDevice);
            item = m_rootResource->getChildClone(itemPath, query, true); //trick to refresh children
            if (item == nullptr)
            {
                qCritical() << "Child not found" << path;
                replyNotFound();
                return;
            }
            m_resourceCollector.watchFor(item);
            path = firstDevice;
            entryList = item->entryList();
            if (! m_working) {
                releaseHandler();
                return;
            } //check if already dropped
        }
    }

    /* folder attributes */
    QJsonObject folderAttr;
    CloudResource*  tagsResource = item;

    Q_FOREACH(const QString &key, tagsResource->getCustomAttributes().keys()) {
        if (key == "sharedLinkId")
            folderAttr.insert("mounted", true);
        else if (key != "itemPath")
            folderAttr.insert(key, QJsonValue::fromVariant(tagsResource->getCustomAttributes().value(key)));
    }

    bool sharedRootWritable = false;
    if (m_rootResource->isSharedRoot())
    {
        sharedRootWritable = m_rootResource->sharedRootItem()->getLinkData()["writable"].toBool();
    }

    //get link data
    QJsonObject folderLinkData;
    Q_FOREACH(const QString &key, tagsResource->getLinkData().keys()) {
        folderLinkData.insert(key, QJsonValue::fromVariant(tagsResource->getLinkData().value(key)));
    }

    if (m_rootResource->isSharedRoot()) {
        folderLinkData["writable"] =  sharedRootWritable;
    }

    QHash<QString, QVariant> folderAttrs = item->getCustomAttributes();
    QString folderName = m_rootResource->isSharedRoot() && path == "" ? m_rootResource->sharedRootName() : 
                         (folderAttrs.contains("filename") ? folderAttrs["filename"].toString() : (folderAttrs.contains("itemName") ? folderAttrs["itemName"].toString() : item->fileName()));
    folder["name"] = itemPath.isEmpty() ? folderName : itemPath.last();
    folder["filename"] = folderName;
    
    folder["type"] = m_rootResource->isSharedRoot() && path == "" ? tagsResource->getResourceTypeString() :  item->getResourceTypeString();
    folder["date"] = item->lastModified().toString("yyyy-MM-dd hh:mm:ss");
    folder["size"] = item->size();
    folder["exists"] =  item->exists();
    folder["link"] = item->getPublicLink();
    folder["mimetype"] = item->getContentType().replace("/", "-");
    folder["owner"] = folderAttr.contains("owner") ? folderAttr["owner"].toString() : m_rootOwner;
    folder["childCount"] = entryList.count();
    folder["customAttributes"] = folderAttr;
    folder["rights"] = QJsonObject {
        {"readable",  item->isReadable() },
        {"writable",  item->isWritable() }
    };

    //Link data with shared to

    if (m_isServerNode )
    {
        folder["isSharedRoot"] = m_rootResource->isSharedRoot();
    }

    folder["linkData"] = folderLinkData;

    //Create respose object
    QJsonObject response;
    response["folder"] = folder;
    response["path"] = m_rootResource->isSharedRoot() && path == "" ? m_rootResource->sharedRootName() : path;

    //Add item to list for singe view of shared file
    if (m_rootResource->isSharedRoot()  && item->isFile()) {
        //QJsonDocument fileDoc(folder);
        list << folder;
    }

    do {

        //write listing
        Q_FOREACH(QString entry, entryList)
        {
            if (CloudResource::isSkipped(entry))
                 continue;

            if (itemPath.count() < 2 &&  CloudResource::isUnlisted(entry)) continue; //skip shared/references folder

            CloudResource* childItem = item->getChild(entry, false);

            //repeat this in loop
            if (childItem != nullptr)
            {
                if (! m_working || m_resp.isNull() || m_req.isNull() ||  !m_req->connection()->isAlive()) {
                    qWarning() << "Request dropped" << path;
                    releaseHandler();
                    return; //check if already dropped
                }

                // skip non-readable system items
                if (! childItem->isReadable())
                    continue;

                QJsonObject file;
                QJsonObject customAttr;
                QJsonObject linkData;
                QHash<QString, QVariant> fileAttrs = childItem->getCustomAttributes();
                file["name"] =  entry;
                QString entryFileName = fileAttrs.contains("filename") ? fileAttrs["filename"].toString() : (fileAttrs.contains("itemName") ? fileAttrs["itemName"].toString() : childItem->fileName());
                file["filename"] = entryFileName;
                file["type"] = childItem->getResourceTypeString();
                file["date"] = childItem->lastModified().toString("yyyy-MM-dd hh:mm:ss");
                file["size"] = childItem->size();

                file["link"] = childItem->getPublicLink();

                file["exists"] = true;
                if (childItem->isDir())
                    file["childCount"] = childItem->nLink();
                file["mimetype"] = childItem->getContentType().replace("/", "-");
                file["owner"] = fileAttrs.contains("owner") ?  fileAttrs["owner"].toString() : m_rootOwner;
                file["rights"] = QJsonObject {
                            {"readable",  childItem->isReadable() },
                            {"writable",  childItem->isWritable() }
                };

                //get custom attributes
                Q_FOREACH(const QString &key, fileAttrs.keys()) {
                    if (key == "sharedLinkId")
                        customAttr.insert("mounted", true);
                    else if (key != "itemPath")
                        customAttr.insert(key, QJsonValue::fromVariant(fileAttrs.value(key)));
                }

                if (m_rootResource->isSharedRoot() || sharedRootWritable) {
                    linkData["writable"] =  sharedRootWritable;
                }

                //get link data
                Q_FOREACH(const QString &key, childItem->getLinkData().keys()) {
                    linkData.insert(key, QJsonValue::fromVariant(childItem->getLinkData().value(key)));
                }

                file["customAttributes"] = customAttr;
                file["linkData"] = linkData;

                //dataToSend.append(fileDoc.toJson(QJsonDocument::Compact));
                list << file;
                count++;

                if (count >= batchSize) { // Send chunk
                    if (!isChunked) {
                        isChunked = true; //make chunked response
                        m_resp->addHeader("Transfer-Encoding", "chunked");
                    }
                    response["list"] = list;
                    QJsonDocument chunkDoc(response);
                    writeChunk(chunkDoc.toJson(QJsonDocument::Compact));
                    list = {}; //clear list
                    response.remove("folder"); //remove folder for the next chunks
                    response.remove("list"); //remove list for next chunks
                    count = 0;
                }
            }
        }

        partial = item->getCustomAttributes().contains("partial");

        if (partial) {
             offset += batchSize;
             query = QString("fromPos=%1&count=%2").arg(offset).arg(batchSize);
             item = m_rootResource->getChildClone(itemPath, query, true);
             if (item) {
                m_resourceCollector.watchFor(item);
                entryList = item->entryList();
             } else
                partial = false;
        }

    } while (partial && m_working && !m_req.isNull() && m_req->connection()->isAlive());

    // End JSON
    response["list"] = list;
    QJsonDocument finalDoc(response);
//    dataToSend.append("]}"); // Close list and JSON
    if (isChunked )
        //Write  last chunk
        endChunkedReply(finalDoc.toJson(QJsonDocument::Compact), qhttp::ESTATUS_OK);
     else
        endJsonReply(finalDoc.toJson(QJsonDocument::Compact), qhttp::ESTATUS_OK);
}

void HttpHandler::bridgeList(QJsonObject request)
{
    QString path = request.contains("path") ? request["path"].toString() : m_req->url().path(QUrl::PrettyDecoded);
    if (path.startsWith("/")) path.remove(0, 1);
    bridgeList(path);
}


void HttpHandler::bridgeGetContent(QJsonObject request)
{
    QString path = request["item"].toString();

    //Get child entries and make loop
    m_reading = true;
    CloudResource* item = m_rootResource->getChildClone(path.split("/", Qt::SplitBehaviorFlags::SkipEmptyParts), m_req->url().query(QUrl::FullyDecoded));
	if (item == nullptr)
    {
        replyJsonNotFound();
        return;
    }

    m_resourceCollector.watchFor(item);

    if (! m_working) {
        releaseHandler();
        return;
    }

	if ( !item->exists())
	{
		replyJsonNotFound();
		return;
	}

	if (item->openResource(QIODevice::ReadOnly) == FILEHANDLE_ERROR)
    {
        replyJsonInternalServerError("Can not open file " + item->fileName());
        return;
    }

    if (! m_working) {
        releaseHandler();
        return;
    }

    //Create result object
    QJsonObject response;
    QByteArray data = item->read(item->size());
    QString content = QString::fromUtf8(data);
    QJsonObject result;
    result["content"] = content;
    result["success"] = true;
    response["result"] = result;

    item->close();

    //Write response
    QJsonDocument responseData(response);
    endJsonReply(responseData.toJson(QJsonDocument::Compact), qhttp::ESTATUS_OK);
}



void HttpHandler::bridgeGetAttributes(QJsonObject request)
{
    QString path = request["item"].toString();

    m_reading = true;
    CloudResource* item = m_rootResource->getChildClone(path.split("/", Qt::SplitBehaviorFlags::SkipEmptyParts), "attrs");
    if (item == nullptr)
    {
        replyJsonNotFound();
        return;
    }

    m_resourceCollector.watchFor(item);

    if (! m_working) {
        releaseHandler();
        return;
    }

    if ( !item->exists())
    {
        replyJsonNotFound();
        return;
    }

    //Create result object
    QJsonObject response;
    QJsonObject result;

    QHash<QString, QVariant> attr = item->getCustomAttributes();

    if (! m_working) {
        releaseHandler();
        return;
    }

    //get custom attributes
    Q_FOREACH(const QString &key, attr.keys()) {
        if (key == "sharedLinkId")
            result.insert("mounted", true);
        else if (key != "itemPath")
            result.insert(key, QJsonValue::fromVariant(attr.value(key)));
    }

    response["success"] = true;
    response["mimetype"] = item->getContentType().replace("/", "-");
    response["filename"] = item->fileName();
    response["owner"] = item->owner();
    response["type"] = item->isDir() ? "dir" : "file";
    response["size"] = item->size();
    response["date"] = item->lastModified().toString("yyyy-MM-dd hh:mm:ss");
    response["result"] = result;

    //Write response
    QJsonDocument responseData(response);
    endJsonReply(responseData.toJson(QJsonDocument::Compact), qhttp::ESTATUS_OK);
}

void HttpHandler::bridgeinvokeClient(QJsonObject request)
{
    QJsonObject response;
    QJsonObject result;
    QJsonObject cmd = request["data"].toObject();
    #ifndef ROUTER
    if (cmd["cmd"].toString() == "addShare") {
        QString path = cmd["path"].toString();
        bool handled = false;
#ifdef SERVER
        if (m_isServerNode) {
             QStringList plist = path.split("/", Qt::SplitBehaviorFlags::SkipEmptyParts);
             QString linkID = m_rootResource->linkForPath(plist);
             if (!linkID.isEmpty()) {
                  QPointer<OnMyDiskLink> link = OnMyDiskLinkServer::instance().getLink(linkID);
                  if (link) {
                       if (plist.size() > 1) plist.removeFirst();
                       QString targetPath = plist.join("/");
                       qInfo() << "Routing addShare to link:" << linkID << "path:" << targetPath;
                       QMetaObject::invokeMethod(link.data(), "addShare", Qt::QueuedConnection, Q_ARG(QString, targetPath));
                       handled = true;
                  }
             }
        }
#endif
        if (!handled) {
            emit UserEvents::instance().addShareRequested(m_userName, path);
        }
        result["success"] = true;
    }
    if (cmd["cmd"].toString() == "removeShare") {
        QString path = cmd["sharePath"].toString();
        bool handled = false;
#ifdef SERVER
        if (m_isServerNode) {
             QStringList plist = path.split("/", Qt::SplitBehaviorFlags::SkipEmptyParts);
             QString linkID = m_rootResource->linkForPath(plist);
             if (!linkID.isEmpty()) {
                  QPointer<OnMyDiskLink> link = OnMyDiskLinkServer::instance().getLink(linkID);
                  if (link) {
                       if (plist.size() > 1) plist.removeFirst();
                       QString targetPath = plist.join("/");
                       qInfo() << "Routing removeShare to link:" << linkID << "path:" << targetPath;
                       QMetaObject::invokeMethod(link.data(), "removeShare", Qt::QueuedConnection, Q_ARG(QString, targetPath));
                       handled = true;
                  }
             }
        }
#endif
        if (!handled) {
            emit UserEvents::instance().removeShareRequested(m_userName, path);
        }
        result["success"] = true;
    }
    #endif
    response["result"] = result;
    QJsonDocument responseData(response);
    endJsonReply(responseData.toJson(QJsonDocument::Compact), qhttp::ESTATUS_OK);
}




void HttpHandler::bridgeGetVideoAttributes(QJsonObject request)
{
    QString path = request["item"].toString();
    QString query = handleAddToQuery(m_req->url().query(QUrl::FullyDecoded), request["addToQuery"].toString());

    //Get child entries and make loop
    CloudResource* item = m_rootResource->getChildClone(path.split("/", Qt::SplitBehaviorFlags::SkipEmptyParts), query);
    if (item == nullptr)
    {
        replyJsonNotFound();
        return;
    }

    m_resourceCollector.watchFor(item);

    if ( !item->exists())
    {
        replyJsonNotFound();
        return;
    }

    //Create result object
    QJsonObject response;
    QJsonObject result;

    // Do not refresh item, it will getVideoAttributes two times
    // item->refresh();
    /*
     * Dependance on addToQuery getVideoDuration&getVideoDimensions ...
     * Attributes will have:
     *  videoDuration
     *  videoDimensions
    */
    QHash<QString, QVariant> attr = item->getCustomAttributes();

    //get custom attributes
    Q_FOREACH(const QString &key, attr.keys()) {
        if (key == "sharedLinkId")
            result.insert("mounted", true);
        else if (key != "itemPath")
            result.insert(key, QJsonValue::fromVariant(attr.value(key)));
    }

    result["success"] = true;
    response["mimetype"] = item->getContentType().replace("/", "-");
    response["result"] = result;

    //Write response
    QJsonDocument responseData(response);
    endJsonReply(responseData.toJson(QJsonDocument::Compact), qhttp::ESTATUS_OK);
}

void HttpHandler::bridgeTranscodeVideo(QJsonObject request)
{
    QString path = request["item"].toString();
    QString query = handleAddToQuery(m_req->url().query(QUrl::FullyDecoded), request["addToQuery"].toString());
    QString videoPath = QFileInfo(path).path();
    QString fileName = QFileInfo(path).baseName();

    // Transcode log file will be used to determine transcode progress
    QString transcodeLogFile = QString("%1/.tmp/%2.transcode.log").arg(videoPath, fileName);

    qDebug() << "bridgeTranscodeVideo  path" << path << ", query" << query << ", transcodeLogFile: " << transcodeLogFile;

    // emit transcodeProgress(0.01);
    //Get child entries and make loop
    CloudResource* item = m_rootResource->getChildClone(path.split("/", Qt::SplitBehaviorFlags::SkipEmptyParts), query);
    if (item == nullptr)
    {
        replyJsonNotFound();
        return;
    }

    m_resourceCollector.watchFor(item);

    if ( !item->exists())
    {
        replyJsonNotFound();
        return;
    }

    // transcodeVideo will start and ffmpeg will return QProcess PID as transcodePid
    QHash<QString, QVariant> videoAttrs = item->getCustomAttributes();

    //qDebug() << "transcodeVideo: transcodePid" << videoAttrs["transcodePid"].toString();

    // emit transcodeProgress(0.015);
    QThread::msleep(REQUEST_TRANSCODE_TIMEOUT);
    float transcodePercent = readTranscodeProgress(transcodeLogFile);


    //emit transcodeProgress(transcodeProgressValue);

    QJsonObject response;
    QJsonObject result;
    if (transcodePercent > 0)
        result["transcodePercent"] = transcodePercent;

    if (transcodePercent == TRANSCODE_FINISHED) {
        result["transcodeSucceed"] = true;
        result["transcodePercent"] = 1;
        removeTranscodeLog(transcodeLogFile, true);
    } else if (videoAttrs.contains("transcodePid")) {
        result["transcodePid"] = videoAttrs["transcodePid"].toString();
        if (transcodePercent > 0)
            result["transcodePercent"] = transcodePercent;
    }

    if (transcodePercent == TRANSCODE_NO_PROGRESS && m_transcodeError != "") {
        result["transcodeError"] = m_transcodeError;
        m_transcodeError = "";
    }

    response["result"] = result;

    //Write response
    QJsonDocument responseData(response);
    endJsonReply(responseData.toJson(QJsonDocument::Compact), qhttp::ESTATUS_OK);

    /*
    if (transcodeProgressValue > 0)
        startTranscodeTimer(transcodeLogFile);
    */
}

void HttpHandler::bridgeGetTranscodeProgress(QJsonObject request)
{
    QString path = request["item"].toString();
    QString mode = request["mode"].toString();             // transcode|downscale
    QString videoPath = QFileInfo(path).path();
    QString fileName = QFileInfo(path).baseName();

    // Transcode log file will be used to determine transcode progress
    QString transcodeLogFile = "";
    if (mode == "downscale")
        transcodeLogFile = QString("%1/.tmp/%2.downscale.log").arg(videoPath, fileName);
    else
        transcodeLogFile = QString("%1/.tmp/%2.transcode.log").arg(videoPath, fileName);

    qDebug() << "path: " << path << ", mode: " << mode << ", transcodeLogFile: " << transcodeLogFile;

    float transcodePercent = 0;

    if (transcodeLogFile != "")
        transcodePercent = readTranscodeProgress(transcodeLogFile);

    //if (transcodePercent > 0)
    //    emit transcodeProgress(transcodePercent);

    QJsonObject response;
    QJsonObject result;

    if (transcodePercent > 0)
        result["transcodePercent"] = transcodePercent;

    if (transcodePercent == TRANSCODE_FINISHED) {
        //emit transcodeProgress(1.0);
        result["transcodeSucceed"] = true;
        result["transcodePercent"] = 1;
        removeTranscodeLog(transcodeLogFile, true);
    }

    if (transcodePercent == TRANSCODE_NO_PROGRESS && m_transcodeError != "") {
        result["transcodeError"] = m_transcodeError;
        m_transcodeError = "";
    }

    response["result"] = result;

    //Write response
    QJsonDocument responseData(response);
    endJsonReply(responseData.toJson(QJsonDocument::Compact), qhttp::ESTATUS_OK);
}

void HttpHandler::bridgeDownscaleVideo(QJsonObject request)
{
    QString path = request["item"].toString();
    QString query = handleAddToQuery(m_req->url().query(QUrl::FullyDecoded), request["addToQuery"].toString());
    QString videoPath = QFileInfo(path).path();
    QString fileName = QFileInfo(path).baseName();

    // Transcode log file will be used to determine transcode progress
    QString transcodeLogFile = QString("%1/.tmp/%2.downscale.log").arg(videoPath, fileName);
    qDebug() << "path" << path << ", query" << query << ", transcodeLogFile: " << transcodeLogFile;

    //emit transcodeProgress(0.01);
    //Get child entries and make loop
    CloudResource* item = m_rootResource->getChildClone(path.split("/", Qt::SplitBehaviorFlags::SkipEmptyParts), query);
    if (item == nullptr)
    {
        replyJsonNotFound();
        return;
    }

    m_resourceCollector.watchFor(item);

    if ( !item->exists())
    {
        replyJsonNotFound();
        return;
    }

    // downscaleVideo will start and ffmpeg will return QProcess PID as transcodePid
    QHash<QString, QVariant> videoAttrs = item->getCustomAttributes();
    // qDebug() << "downscaleVideo: transcodePid" << videoAttrs["transcodePid"].toString();

    // emit transcodeProgress(0.015);
    QThread::msleep(REQUEST_TRANSCODE_TIMEOUT);
    float transcodePercent = readTranscodeProgress(transcodeLogFile);


    //emit transcodeProgress(transcodeProgressValue);

    QJsonObject response;
    QJsonObject result;
    if (transcodePercent > 0)
        result["transcodePercent"] = transcodePercent;

    if (transcodePercent == TRANSCODE_FINISHED) {
        result["transcodeSucceed"] = true;
        result["transcodePercent"] = 1;
        removeTranscodeLog(transcodeLogFile, true);
    } else if (videoAttrs.contains("transcodePid")) {
        result["transcodePid"] = videoAttrs["transcodePid"].toString();
        if (transcodePercent > 0)
            result["transcodePercent"] = transcodePercent;
    }

    if (transcodePercent == TRANSCODE_NO_PROGRESS && m_transcodeError != "") {
        result["transcodeError"] = m_transcodeError;
        m_transcodeError = "";
    }

    response["result"] = result;

    //Write response
    QJsonDocument responseData(response);
    endJsonReply(responseData.toJson(QJsonDocument::Compact), qhttp::ESTATUS_OK);
}

void HttpHandler::bridgeTranscodeAbort(QJsonObject request)
{
    QString path = request["item"].toString();
    QString query = handleAddToQuery(m_req->url().query(QUrl::FullyDecoded), request["addToQuery"].toString());

    //Get child entries and make loop
    CloudResource* item = m_rootResource->getChildClone(path.split("/", Qt::SplitBehaviorFlags::SkipEmptyParts), query);
    if (item == nullptr)
    {
        replyJsonNotFound();
        return;
    }

    m_resourceCollector.watchFor(item);

    if ( !item->exists())
    {
        replyJsonNotFound();
        return;
    }

    QHash<QString, QVariant> videoAttrs = item->getCustomAttributes();

    // in the local resource process will be killed and transcode/downscale file will be deleted with appropriate log file
    if (videoAttrs.contains("abort"))
    {
        // Here we will implement the trick, even if abort failed we'll send success=false
        // and not error in response, so that we can have custom error handling to clean transcode process on client gui side
        if (videoAttrs["abort"].toBool())
            replyJsonSuccess();
        else {
            QJsonObject response;
            QJsonObject result;

            result["success"] = false;
            response["result"] = result;

            //Write response
            QJsonDocument responseData(response);
            endJsonReply(responseData.toJson(QJsonDocument::Compact), qhttp::ESTATUS_OK);
        }
    }
}

void HttpHandler::updateProfileWithRights(QJsonObject &profile, const QString &username, const QString &pkey, ClientSession* session)
{
    // Fetch rights from Console or Gateway
    QJsonObject rightsProfile;
    QString omdToken;
    if (!m_isServerNode) {
        m_Settings->beginGroup("Settings");
        omdToken = m_Settings->value("omdToken").toString();
        if (omdToken.isEmpty()) {
            omdToken = m_Settings->value("storageId").toString();
        }
        m_Settings->endGroup();

        if (!omdToken.isEmpty()) {
            rightsProfile = fetchProfileFromGateway(omdToken, m_Settings);
        }
    } else {
#ifdef QT_DEBUG
        Q_UNUSED(username)
        Q_UNUSED(pkey)
        profile["status"] = "Unlimited";
        profile["token_balance"] = 1000.0;
        QJsonObject permissions;
        permissions["collab"] = true;
        permissions["ai_chat"] = true;
        permissions["search"] = true;
        profile["permissions"] = permissions;
        return;
#else
        rightsProfile = m_sessionAuthProvider->fetchRights(username, pkey);
#endif
    }

    if (!rightsProfile.isEmpty()) {
        if (!m_isServerNode) {
            profile["user"] = rightsProfile["user"].toString();
            profile["email"] = rightsProfile["email"].toString();
            profile["displayname"] = rightsProfile["displayname"].toString();
            profile["profileimage"] = rightsProfile["profileimage"].toString();
            profile["omd_key"] = omdToken;
            profile["valid"] = true;
        }

        if (rightsProfile.contains("rights")) {
            QJsonObject rights = rightsProfile["rights"].toObject();
            bool collab = (rightsProfile["token_balance"].toDouble() > 0.0) || (rights["seats_allowed"].toInt() > 1);

            profile["status"] = rightsProfile["status"].toString();
            profile["token_balance"] = rightsProfile["token_balance"].toDouble();
            QJsonObject permissions;
            permissions["collab"] = collab;
            permissions["ai_chat"] = rights["ai_chat"].toBool();
            profile["permissions"] = permissions;

            if (collab) {
                if (username != "anonymous")
                    profile["group"] = rightsProfile["group_name"].toString();
                if (rightsProfile["is_owner"].toBool()) {
                    profile["isGroupAdmin"] = true;
                }
            }
        }
    }

    if (session) {
        QHash<QString, QVariant> sessionVars = session->getSessionVariables();
        sessionVars["status"] = profile["status"].toString();
        sessionVars["token_balance"] = profile["token_balance"].toDouble();
        QJsonObject permissions = profile["permissions"].toObject();
        sessionVars["collab"] = permissions["collab"].toBool();
        sessionVars["ai_chat"] = permissions["ai_chat"].toBool();
        session->setSessionVariables(sessionVars);
    }

    // Check search permission: user owns or has access to a Search or AI node.
    // Also grant search if a public AI node exists (no personal node required).
    // Safe to call here: updateProfileWithRights is only called during profile
    // requests, never during RPC/site serving — no mutex conflicts.
    bool hasSearch = false;
#ifdef SERVER
    SitesHash searchNodes = OnMyDiskLinkServer::instance().getSearchNodes();
    foreach(const SiteInfo& site, searchNodes.values()) {
        if (site.owner == username || site.users.contains(username) || site.users.contains("*")) {
            hasSearch = true;
            break;
        }
    }
    if (!hasSearch) {
        SiteInfo chatNode;
        if (OnMyDiskLinkServer::instance().getChatNode(username, chatNode)) {
            hasSearch = true;
        }
    }
#else
    hasSearch = true;
#endif
    QJsonObject searchPerms = profile["permissions"].toObject();
    searchPerms["search"] = hasSearch;
    profile["permissions"] = searchPerms;

    if (session) {
        session->setVar("search", hasSearch);
    }

    bool hasCode = false;
#ifdef SERVER
    hasCode = OnMyDiskLinkServer::instance().hasCodePermission(username);
#else
    hasCode = true;
#endif
    QJsonObject codePerms = profile["permissions"].toObject();
    codePerms["code"] = hasCode;
    profile["permissions"] = codePerms;

    if (session) {
        session->setVar("code", hasCode);
    }
}

void HttpHandler::bridgeSignInWithGoogle(QJsonObject request)
{
    QString idToken = request["id_token"].toString();

    if (idToken.isEmpty())
    {
        replyJsonBadRequest("Missing id_token");
        return;
    }

    // Verify token with Google
    if (!m_externalAccessManager)
        m_externalAccessManager = new QNetworkAccessManager(this);

    QNetworkRequest verifyRequest(QUrl("https://oauth2.googleapis.com/tokeninfo?id_token=" + idToken));
    QNetworkReply *reply = m_externalAccessManager->get(verifyRequest);

    // Wait for response (synchronous for simplicity in this handler context, 
    // though async is preferred for high load, but this is a login action)
    // Using a local event loop to wait for the reply
    QEventLoop loop;
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    if (reply->error() != QNetworkReply::NoError)
    {
        qCritical() << "Google token verification failed:" << reply->errorString();
        replyJsonError("Token verification failed");
        reply->deleteLater();
        return;
    }

    QByteArray responseData = reply->readAll();
    reply->deleteLater();

    QJsonDocument jsonDoc = QJsonDocument::fromJson(responseData);
    QJsonObject tokenInfo = jsonDoc.object();

    // Verify Audience (Client ID)
    QString clientId = "959849209352-vbi0vfimrqj904598felj8ieeb6to8s2.apps.googleusercontent.com"; 
    if (tokenInfo["aud"].toString() != clientId) {
        replyJsonError("Invalid token audience");
        return;
    }

    QString email = tokenInfo["email"].toString();
    QString displayName = tokenInfo["name"].toString();
    QString profileImage = tokenInfo["picture"].toString();

    // Download profile image and convert to base64
    if (!profileImage.isEmpty()) {
        QNetworkRequest imageRequest;
        imageRequest.setUrl(QUrl(profileImage));
        QNetworkReply *imageReply = m_externalAccessManager->get(imageRequest);
        QEventLoop imageLoop;
        connect(imageReply, &QNetworkReply::finished, &imageLoop, &QEventLoop::quit);
        imageLoop.exec();

        if (imageReply->error() == QNetworkReply::NoError) {
            QByteArray imageData = imageReply->readAll();
            QString contentType = imageReply->header(QNetworkRequest::ContentTypeHeader).toString();
            if (contentType.isEmpty()) contentType = "image/jpeg"; // Fallback
            profileImage = "data:" + contentType + ";base64," + imageData.toBase64();
        }
        imageReply->deleteLater();
    }

    if (email.isEmpty())
    {
        replyJsonError("Invalid or unverified email");
        return;
    }

    //User accounts live on the legacy gateway during the GunDB migration:
    //the verified Google identity is authenticated there.
    QString username = email;
    request["action"] = "signinwithgoogle";
    request["verified_email"] = email;
    QJsonObject upstream;
    bool ok = callUpstream(request, upstream);

    if (ok) {
        QJsonObject response;
        QJsonObject result = upstream["result"].toObject();
        QJsonObject profile = result["profile"].toObject();
        QString pkey = result["token"].toString();
        QString deviceKey = result["device_key"].toString();

        result["success"] = true;
        result["device_key"] = deviceKey;
        QString deviceId = request["deviceId"].toString();
        QString sessionID = deviceKey;

        m_sessionAuthProvider->addSessionFor(username, 86400000, sessionID); //24hours
        QPointer<ClientSession> session = m_sessionAuthProvider->sessionForToken(sessionID);

        // Fetch rights from Console and update profile
        updateProfileWithRights(profile, username, pkey);

        QHash<QString, QVariant> sessionVars;
        sessionVars["User"] = username;
        sessionVars["isAdmin"] = profile["isAdmin"].toBool();
        sessionVars["isGroupAdmin"] = profile["isGroupAdmin"].toBool();
        sessionVars["isGuest"] = false;
        sessionVars["group"] = profile["group"].toString();

        // Populate permissions, status and token balance from profile
        if (profile.contains("permissions")) {
            QJsonObject rights = profile["permissions"].toObject();
            sessionVars["collab"] = rights["collab"].toBool();
            sessionVars["ai_chat"] = rights["ai_chat"].toBool();
            sessionVars["code"] = rights["code"].toBool();
        }
        sessionVars["status"] = profile["status"].toString();
        sessionVars["token_balance"] = profile["token_balance"].toDouble();
        session->setSessionVariables(sessionVars);

        // Add token and status to profile returned to UI
        profile["omd_key"] = pkey; // Ensure consistent API key across all devices

        result["profile"] =  profile;
        result["session_id"] = sessionID;
        result["device_key"] = deviceKey;
        result["token"] = deviceKey;
        result["valid"] = true;

        m_resp->addHeader("authentication-success", "success");
        saveSessionCookie(sessionID);
        qInfo() << "User signed in" << session->userName() << "session:" << session->sessionID();
        emit UserEvents::instance().signedIn(username);

        //Write response
        response["result"] = result;
        QJsonDocument responseData(response);
        endJsonReply(responseData.toJson(QJsonDocument::Compact), qhttp::ESTATUS_OK);
    } else {
        replyJsonError("Authentication failed");
    }
}

QString HttpHandler::handleAddToQuery(const QString &query, const QString &addToQuery)
{
    QString queryValue;

    if (query.trimmed() == "")
        queryValue = addToQuery;
    else
        queryValue = query + "&" + addToQuery;

    return queryValue;
}

float HttpHandler::readTranscodeProgress(QString transcodeLogFile)
{
    // If we don't find current progress return -1, if transcode finished we'll return -2 (TRANSCODE_FINISHED)
    // otherwise return current progress of transcoding/downscaling
    float progressValue = TRANSCODE_NO_PROGRESS;
    bool hasDuration = false, hasTime = false;
    m_transcodeError = "";

    CloudResource* item = m_rootResource->getChildClone(transcodeLogFile.split("/", Qt::SplitBehaviorFlags::SkipEmptyParts), "");
    if (item == nullptr)
    {
        //qDebug() << "transcode log file is nullptr";
        return progressValue;
    }

    m_resourceCollector.watchFor(item);

    if ( !item->exists())
    {
        //qDebug() << "transcode log file does not exists";
        return progressValue;
    }

    if (item->openResource(QIODevice::ReadOnly) == FILEHANDLE_ERROR)
    {
        //qDebug() << "Can not open transcode log file " << item->fileName();
        return progressValue;
    }
    QByteArray data = item->read(item->size());
    QString content = QString::fromUtf8(data);
    item->close();

    QStringList logFileLines = content.split(QRegExp("\n|\r\n|\r"));

    // First get video duration
    // Duration: 00:00:21.38, start: 0.000000, bitrate: 22163 kb/s
    // Get first line from where we have "Duration:"
    QString durationInLog = "";             // Duration: 00:00:21.38
    for(auto& line : logFileLines)
    {
        if (line.contains("Duration:", Qt::CaseSensitive) && line.contains("start:", Qt::CaseSensitive) && line.contains("bitrate:", Qt::CaseSensitive))
        {
            durationInLog = line.simplified();
            break;
        }
    }
    QString durationValueInLog = "";        // 00:00:21.38
    if (durationInLog != "")
    {
        if (durationInLog.size() >= 21)
        {
            durationValueInLog = durationInLog.mid(10, 11);
            hasDuration = true;
        }
    }

    /*
        Check if transcode finished the job, last line should be empty string and line before should look like: video:582kB audio:173kB subtitle:0kB other streams:0kB global headers:0kB muxing overhead: 0.799954%
        frame=  640 fps= 12 q=29.0 Lsize=    6564kB time=00:00:21.37 bitrate=2515.7kbits/s speed=0.399x
        video:6233kB audio:307kB subtitle:0kB other streams:0kB global headers:0kB muxing overhead: 0.368810%
        ... donwscale has different lines from end of the file ...
        [libx264 @ 0x55db2f148540] kb/s:2390.94
        [aac @ 0x55db2f22d680] Qavg: 6596.452
        Read from behind to see if we have
        End of the downscale/transcode process
        video:6233kB audio:307kB subtitle:0kB other streams:0kB global headers:0kB muxing overhead: 0.368810%
        dowscale/transcode in progress
        frame=   12 fps=0.0 q=0.0 size=       1kB time=00:00:00.97 bitrate=   5.9kbits/s speed=1.83x
    */

    QString progressTimeInLog = "";
    for (int i = logFileLines.size() - 1; i > -1; i--) {
        QString checkLine = logFileLines[i].simplified();
        if (checkLine != "") {
            if (checkLine.contains("video:", Qt::CaseSensitive) && checkLine.contains("audio:", Qt::CaseSensitive) && checkLine.contains("subtitle:", Qt::CaseSensitive)) {
                // This is the end of transcode/dowscale process
                progressValue = TRANSCODE_FINISHED;
                break;
            } else if (checkLine.contains("time=", Qt::CaseSensitive) && checkLine.contains("frame=", Qt::CaseSensitive) &&
                       checkLine.contains("fps=", Qt::CaseSensitive) && checkLine.contains("size=", Qt::CaseSensitive) &&
                       checkLine.contains("bitrate=", Qt::CaseSensitive) && checkLine.contains("speed=", Qt::CaseSensitive)) {
                int timeLocationInLine = checkLine.indexOf("time=", 0, Qt::CaseSensitive);
                if (timeLocationInLine > 0)
                {
                    progressTimeInLog = checkLine.mid(timeLocationInLine + 5, 11);
                    hasTime = true;
                    break;
                }
            // Check for errors
            } else if (hasDuration && !hasTime && (checkLine.contains("- exiting", Qt::CaseSensitive) || checkLine.contains("Error", Qt::CaseSensitive))) {
                if (checkLine.contains("- exiting", Qt::CaseSensitive)) {
                    // If we have duration in log file but for some reason third line from behind has "- exiting" string ffmpeg has error
                    // Second line from behind is error message
                    m_transcodeError = logFileLines[logFileLines.size()-2].simplified();
                } else if (checkLine.contains("Error", Qt::CaseSensitive)) {
                    // If we have Error in line, the line is error message
                    m_transcodeError = checkLine;
                }
                progressValue = TRANSCODE_NO_PROGRESS;
                break;
            }
        }
        // qDebug() << "Line from log: " << logFileLines[i];
    }

    if (m_transcodeError == "" && hasDuration && hasTime)
    {
        // Duration: 00:00:09.83 time: 00:00:01.15
        float videoDuration = 0;
        float progressTime = 0;

        if (durationValueInLog != "")
            videoDuration = getSecondsFromLog(durationValueInLog);
        if (progressTimeInLog != "")
            progressTime = getSecondsFromLog(progressTimeInLog);

        float calculateProgress = 0;

        if (videoDuration > 0 && progressTime > 0)
            calculateProgress = progressTime / videoDuration;

        // qDebug() << "calculateProgress: " << calculateProgress;

        if (calculateProgress > 0)
            progressValue = calculateProgress;
    }

    return progressValue;
}

/*
 * Used to get seconds for duration and time in transcode/downscale log
 * Input:   00:00:21.37
 * Output:  21              (number of seconds)
*/
float HttpHandler::getSecondsFromLog(const QString& valueInLog)
{
    // qDebug() << "getSecondsFromLog: " << valueInLog;
    float secondsValue = 0;
    float seconds = 0, minutes = 0, hours = 0;
    QStringList parseValueInLog = valueInLog.split(":");
    bool ok;

    if (parseValueInLog.size() == 3)
    {
        seconds = parseValueInLog[2].toFloat(&ok);
        if (ok && seconds > 0)
            secondsValue = seconds;
        minutes = parseValueInLog[1].toFloat(&ok);
        if (ok && minutes > 0)
            secondsValue += minutes * 60;
        hours = parseValueInLog[0].toFloat(&ok);
        if (ok && hours > 0)
            secondsValue += hours * 3600;
    }

    return secondsValue;
}

bool HttpHandler::removeTranscodeLog(QString transcodeLogFile, bool forced)
{
    bool removeResult = false;

    CloudResource* item = m_rootResource->getChildClone(transcodeLogFile.split("/", Qt::SplitBehaviorFlags::SkipEmptyParts), "");

    if (item == nullptr)
    {
        //qDebug() << "Transcode log file not found";
        return removeResult;
    }

    m_resourceCollector.watchFor(item);

    if ( !item->exists())
    {
        //qDebug() << "Transcode log file not found";
        return removeResult;
    }

    removeResult = item->remove(forced);

    return removeResult;
}

void HttpHandler::bridgeRename(QJsonObject request)
{
    QString path = request["item"].toString();
    QStringList itemPath = path.split("/", Qt::SplitBehaviorFlags::SkipEmptyParts);
    QString newName = request["newItemName"].toString();

    //Get child entries
    m_reading = true;
    CloudResource* item = m_rootResource->getChildClone(itemPath, m_req->url().query(QUrl::FullyDecoded), false);
    if (item == nullptr)
    {
        replyJsonNotFound();
        return;
    }

    m_resourceCollector.watchFor(item);

    if (! m_working) {
        releaseHandler();
        return;
    }

	if (!item->exists())
	{
        replyJsonNotFound();
        return;
	}

    QStringList newItemPath = item->remotePath().split("/", Qt::SplitBehaviorFlags::SkipEmptyParts);
    if (newItemPath.count() > 1)
        newItemPath.removeLast();
    newItemPath.append(newName);
    QString newPath = "/" + newItemPath.join("/");

    //check version
    if (m_rootResource->linkVersion(itemPath.first()).startsWith("2.1.5")) {

        //try new implementation
        #ifdef SERVER
        QString resourceName = newPath;
        #else
        QString resourceName = m_rootResource->getLocalPath(newPath.split("/", Qt::SplitBehaviorFlags::SkipEmptyParts));
        #endif
        if (QFileInfo(resourceName).suffix() == "" && QFileInfo(path).suffix() != "")
            resourceName = resourceName + "." + QFileInfo(path).suffix();

        if (item->rename(resourceName)) {
            //Create result object
            QJsonObject response;
            QJsonObject result;
            result["success"] = true;
            result["date"] = item->lastModified().toString("yyyy-MM-dd hh:mm:ss");
            response["result"] = result;

            if (item->isDir())
                newPath.append("/");
            emit UserEvents::instance().fileRemoved(item->owner(m_rootOwner), m_userName, item->remotePath());
            emit UserEvents::instance().fileUpdated(item->owner(m_rootOwner), m_userName, newPath);

            //Write response
            QJsonDocument responseData(response);
            endJsonReply(responseData.toJson(QJsonDocument::Compact), qhttp::ESTATUS_OK);
        } else
            replyJsonError(tr("Error while renaming '%1' to '%2'").arg(path, newPath));

        //qInfo() << "rename:" <<  resourceName << m_rootResource->linkVersion(itemPath.first());
        return;
    }

    //fallback to old
    if (item->isDir() ) { //do not allow moving non-empty folders
        if (item->entryList().count() > 0 ) {
            replyJsonError(tr("Can not rename/move non-empty folder"));
            return;
        }
    }

    itemPath = newPath.split("/", Qt::SplitBehaviorFlags::SkipEmptyParts);
    CloudResource* dstItem = m_rootResource->getChildClone(itemPath);

    if (!dstItem)
    {
        replyJsonNotFound();
        return;
    }

    m_resourceCollector.watchFor(dstItem);

    if (! m_working) {
        releaseHandler();
        return;
    }

    QHash<QString, QString> errList;
    item->moveTo(dstItem, &errList);

    //qInfo() << "rename moveTo:" << newPath << m_rootResource->linkVersion(itemPath.first());

    if (errList.size() == 0)
    {
        item->SetParentOutdated();
        dstItem->refresh();
        //Create result object
        QJsonObject response;
        QJsonObject result;
        result["success"] = true;
        result["date"] = dstItem->lastModified().toString("yyyy-MM-dd hh:mm:ss");
        response["result"] = result;
        if (item->isDir())
            newPath.append("/");

        emit UserEvents::instance().fileRemoved(item->owner(m_rootOwner), m_userName, item->remotePath());
        emit UserEvents::instance().fileUpdated(dstItem->owner(m_rootOwner), m_userName, newPath);
        //Write response
        QJsonDocument responseData(response);
        endJsonReply(responseData.toJson(QJsonDocument::Compact), qhttp::ESTATUS_OK);
        return;
    }

    replyJsonError(errList.values().first());
}

void HttpHandler::bridgeRemove(QJsonObject request)
{
    QJsonArray paths = request["items"].toArray();
    bool forced = request["forced"].toBool();

    m_reading = true;
    Q_FOREACH(QJsonValue s_path, paths)
    {
        QHash<QString, QString> currentErrList;
        QStringList path = s_path.toString().split("/", Qt::SplitBehaviorFlags::SkipEmptyParts);

        CloudResource* item = m_rootResource->getChildClone(path, m_req->url().query(QUrl::FullyDecoded));

		if (item == nullptr)
        {
            replyJsonNotFound();
            return;
        }

        m_resourceCollector.watchFor(item);

        if (! m_working) {
            releaseHandler();
            return;
        }

		if ( !item->exists())
		{
			replyJsonNotFound();
			return;
		}

        item->remove(forced);

        emit UserEvents::instance().fileRemoved(item->owner(m_rootOwner), m_userName, item->remotePath());
    }

    replyJsonSuccess();
}

void HttpHandler::bridgeEdit(QJsonObject request)
{
    QString path = request["item"].toString();
    QString content = request["content"].toString();

    QStringList pathList = path.split("/", Qt::SplitBehaviorFlags::SkipEmptyParts);
    //Get child entries
    m_reading = true;
    CloudResource* item = m_rootResource->getChildClone(pathList, m_req->url().query(QUrl::FullyDecoded),  true);
	if (item == nullptr)
    {
        replyJsonNotFound();
        return;
    }

    m_resourceCollector.watchFor(item);

    if (! m_working) {
        releaseHandler();
        return;
    }

	if (item->openResource(QIODevice::WriteOnly) == FILEHANDLE_ERROR)
    {
        replyJsonInternalServerError("Can not open file " + item->fileName());
        return;
    }

    int bytesWriten = item->write(content.toUtf8());
    item->close();

    QVariantHash attrs = item->getCustomAttributes();
    path = item->remotePath();

    if (!attrs.contains("isSite")) {
        if (path != "/") {
            emit UserEvents::instance().fileUpdated(item->owner(m_rootOwner), m_userName, path);
        }
    } else {
        bool rebuild = path.endsWith("_config.yml") || path.endsWith("index.js");
        QString siteName =  attrs["siteName"].toString();
        emit UserEvents::instance().siteUpdated(item->owner(m_rootOwner),  siteName, rebuild);
    }

    if (bytesWriten != 0)
        replyJsonSuccess();
    else
        replyJsonError("Unable to write content");
}

void HttpHandler::bridgeMkCol(QJsonObject request)
{
    QString path = request["newPath"].toString();

    m_reading = true;

    CloudResource* item = m_rootResource->touch(path.split("/", Qt::SplitBehaviorFlags::SkipEmptyParts));

    if (item == nullptr)
    {
        replyJsonInternalServerError("Can not create directory: " + path);
        return;
    }
    m_resourceCollector.watchFor(item);

    if (! m_working) {
        releaseHandler();
        return;
    }

    if (item->exists())
    {
        replyJsonInternalServerError("Directory already exists: " + item->fileName());
        return;
    }


    if (!item->mkcol())
    {
        replyJsonInternalServerError("Can not create directory " + item->fileName());
        return;
    }

    item->SetParentOutdated();

    replyJsonSuccess();
}



void HttpHandler::bridgeConnectDevice(QJsonObject request)
{
    //Devices are attached on the legacy gateway during the GunDB migration
    QJsonObject response, result;
    QJsonObject upstream;
    if (callUpstream(request, upstream))
    {
        response = upstream;
    } else
    {
        result["success"] = false;
        result["message"] = tr("Upstream gateway error.");
        response["result"] = result;
    }
    QJsonDocument responseData(response);
    endJsonReply(responseData.toJson(QJsonDocument::Compact), qhttp::ESTATUS_OK);

}

void HttpHandler::bridgeRemoveSharedLink(QJsonObject request)
{
    QString shareName = request["shareName"].toString();
    QJsonObject response, result;

    bool ok = false;

    if (m_isServerNode)
    {
        //User shares are removed on the legacy gateway during the GunDB migration
        QJsonObject upstream;
        ok = callUpstream(request, upstream);
        emit UserEvents::instance().sharedLinkRemoved(m_userName, shareName);
    }

    if (ok)
    {
        result["success"] = true;
    } else
    {
        result["message"] = tr("Link not found.");
        result["success"] = false;
    }

    response["result"] = result;
    QJsonDocument responseData(response);
    endJsonReply(responseData.toJson(QJsonDocument::Compact), qhttp::ESTATUS_OK);
}

void HttpHandler::bridgeShareByLink(QJsonObject request)
{
    QString path = request["item"].toString();
    QJsonObject jsLinkData = request["linkData"].toObject();

    //Convert js object to variant
    QHash<QString, QVariant> linkData;
    Q_FOREACH(QString key, jsLinkData.keys()) {
        linkData.insert(key, jsLinkData[key].toVariant());
    }

    //Set discuss according to setMeet
    linkData["discuss"] = linkData["setMeet"].toBool();
    QString shareTo = linkData["shareTo"].toString();
    bool setShareTo = linkData["setShareTo"].toBool();
    QString origShareTo = linkData["origShareTo"].toString();

    //if shareTo is not checked but setShareTo present, remove share for users
    bool removeShareTo = !setShareTo && ! shareTo.isEmpty();

    // Check Rights
    double tokenBalance = m_sessionAuthProvider->getSessionVariable(m_req->sessionId(), "token_balance", 0.0).toDouble();
    bool isGuest = m_sessionAuthProvider->getSessionVariable(m_req->sessionId(), "isGuest", false).toBool();
    
    // Allow removing shares, but restrict adding if no rights
    if (!isGuest && setShareTo && tokenBalance < 1.0) {
          replyJsonForbidden();
          return;
    }

    if ( linkData["setMeet"].toBool() && tokenBalance < 1.0 ) {
          replyJsonForbidden();
          return;
    }

    //Get child entries
    m_reading = true;
    QStringList itemPath = path.split("/", Qt::SplitBehaviorFlags::SkipEmptyParts);
    CloudResource* item = m_rootResource->getChildClone(itemPath);

    if (item == nullptr)
    {
        replyJsonNotFound();
        return;
    }

    m_resourceCollector.watchFor(item);

    if (! m_working) {
        releaseHandler();
        return;
    }

    if ( !item->exists())
    {
        replyJsonNotFound();
        return;
    }

    // Re-share and GW-only links
    QHash<QString, QVariant> attrs = item->getCustomAttributes();
    QString sharedLinkId = attrs["sharedLinkId"].toString();
    if (!sharedLinkId.isEmpty()) {
        //QString itemPath = attrs["itemPath"].toString();
        QString itemPath = item->remotePath();
        QString key = attrs["key"].toString();
        quint64 keyHash = key.isEmpty() ? SimpleCrypt::defaultKey : SimpleCrypt::stringToHash(key);
        SimpleCrypt crypt(keyHash);

        QString path = itemPath;
        if (path.endsWith("/"))
            path.chop(1);

        QString link = QString("/shared/") + crypt.encryptToString(QString("%1:%2").arg(sharedLinkId, path));
        QString linkKey = QString("%1:%2").arg(sharedLinkId, path);
        QString access = "readonly";

        if (item->isWritable() && linkData["writable"].toBool())
            access = "writable";
        else
            access = "readonly";

        //update guest sessions
        m_sessionAuthProvider->updateGuestAccess(path, access);

        // create shared root
        #ifndef ROUTER
        linkData["link"] = link;
        emit UserEvents::instance().publicLinkRequested(sharedLinkId, itemPath, m_rootOwner, linkData);
        #endif
        //Write response
        QJsonObject response, sharedUrl;
        sharedUrl["success"] = true;
        sharedUrl["link"] = link;
        response["result"] = sharedUrl;

        QJsonDocument responseData(response);
        endJsonReply(responseData.toJson(QJsonDocument::Compact), qhttp::ESTATUS_OK);
        return;
    }

    QString link = item->getPublicLink(linkData);

    if (! m_working) {
        releaseHandler();
        return;
    }

    QString parsedShareTo;

    if (m_isServerNode && item->isDir() && (setShareTo || removeShareTo))
    {
        //User-to-user sharing is handled by the legacy gateway / GunDB during the migration
    }

    #ifndef ROUTER
    linkData["link"] = link;
    emit UserEvents::instance().publicLinkRequested(item->storageID(), path, m_rootOwner, linkData);
    #endif

    //Write response
    QJsonObject response, sharedUrl;
    sharedUrl["success"] = true;
    sharedUrl["link"] = link;
    sharedUrl["shareTo"] = parsedShareTo;
    response["result"] = sharedUrl;

    QJsonDocument responseData(response);
    endJsonReply(responseData.toJson(QJsonDocument::Compact), qhttp::ESTATUS_OK);
}


void HttpHandler::bridgeUnshareByLink(QJsonObject request)
{
    QString path = request["item"].toString();
    if (path.endsWith("/"))
        path.chop(1);

    m_reading = true;
    QStringList itemPath = path.split("/", Qt::SplitBehaviorFlags::SkipEmptyParts);
    CloudResource* item = m_rootResource->getChildClone(itemPath);

    if (item == nullptr)
    {
        replyJsonNotFound();
        return;
    }

    m_resourceCollector.watchFor(item);

    if (! m_working) {
        releaseHandler();
        return;
    }

    if ( !item->exists())
    {
        replyJsonNotFound();
        return;
    }

    m_sessionAuthProvider->removeGuestAccess(path);
    
    QHash<QString, QVariant> attrs = item->getCustomAttributes();
    QString sharedLinkId = attrs["sharedLinkId"].toString();
    
    // Also remove by LinkID if available (robustness for PWA/Virtual Paths)
    if (sharedLinkId.isEmpty() && !attrs["isRef"].toBool()) {
         // Try to find link ID from item if not explicitly set
         sharedLinkId = item->GetLinkID();
    }
    
    if (!sharedLinkId.isEmpty()) {
        m_sessionAuthProvider->removeGuestAccessByLink(sharedLinkId);
    }

    if (!sharedLinkId.isEmpty() || attrs["isRef"].toBool())
    {
        if (m_isServerNode) {
             emit UserEvents::instance().sharedLinkRemoved(m_userName, item->fileName());
        }
    }

    qDebug() << "Removing link for" << path;
    item->removePublicLink();

    #ifndef ROUTER
    if (m_isServerNode) {
        emit UserEvents::instance().publicLinkRemoved(item->GetID(), item->remotePath(), m_userName);
    }
    #endif

    QJsonObject response;
    response["success"] = true;

    //Write response
    QJsonDocument responseData(response);
    endJsonReply(responseData.toJson(QJsonDocument::Compact), qhttp::ESTATUS_OK);
}

void HttpHandler::bridgeSharePathToSession(QJsonObject request)
{
    QString path = request["item"].toString();
    QString callId = request["sessionid"].toString();
    QString linkId= m_rootResource->linkForPath(path.split("/", Qt::SplitBehaviorFlags::SkipEmptyParts));

    m_sessionAuthProvider->sharePathInCall(callId, linkId, path);

    QJsonObject response;
    response["success"] = true;

    //Write response
    QJsonDocument responseData(response);
    endJsonReply(responseData.toJson(QJsonDocument::Compact), qhttp::ESTATUS_OK);
}

void HttpHandler::bridgeUnsharePathFromSession(QJsonObject request)
{
    QString callId = request["sessionid"].toString();

    m_sessionAuthProvider->unsharePathInCall(callId);
    QJsonObject response;
    response["success"] = true;

    //Write response
    QJsonDocument responseData(response);
    endJsonReply(responseData.toJson(QJsonDocument::Compact), qhttp::ESTATUS_OK);
}

void HttpHandler::bridgeInviteUsersToCall(QJsonObject request)
{

 #ifdef SERVER

    QJsonObject response;
    QString users = request["users"].toString();
    QString organizer = request["organizer"].toString();

    if (!users.isEmpty() || !organizer.isEmpty())
    {
         CallInfo callInfo;
         callInfo.callId = request["callId"].toString();
         callInfo.callTopic = request["callTopic"].toString();
         callInfo.meet = request["meet"].toVariant().toDateTime();
         callInfo.setMeet = request["setMeet"].toBool();
         callInfo.users = users;
         callInfo.organizer = organizer;
         callInfo.organizerName = request["organizerName"].toString();
         OnMyDiskMailer::instance().sendCallNotification(callInfo);
    }

    response["success"] = true;

    //Write response
    QJsonDocument responseData(response);
    endJsonReply(responseData.toJson(QJsonDocument::Compact), qhttp::ESTATUS_OK);
#else
   Q_UNUSED(request)
   replyJsonNotImplemented();
#endif
}


void HttpHandler::bridgeGetProfileData(QJsonObject request)
{
     if (!m_isServerNode)
     {
         QEventLoop loop;
         bool ok = false;
         QJsonObject response;

         m_externalAccessManager = new QNetworkAccessManager(this);

         connect(m_externalAccessManager, &QNetworkAccessManager::sslErrors, this, [this] (QNetworkReply *reply, const QList<QSslError> &errors) {
             Q_FOREACH(QSslError error, errors)
             {
                 qWarning() << "SSL notice:" << error.errorString();
             }
             reply->ignoreSslErrors();
         });

         connect(m_externalAccessManager, &QNetworkAccessManager::finished, this, [this, &loop, &ok, &response] (QNetworkReply *reply) {
             if (reply->error() == QNetworkReply::NoError) {
                 response = QJsonDocument::fromJson(reply->readAll()).object();
                 ok = true;
             } else {
                 qCritical() << "api call error:" <<  reply->errorString();
             }
             reply->deleteLater();
             loop.exit(0);
         });

         #ifdef QT_DEBUG
         QString apiUrl = QString("http://localhost:8080/");
         #else
         #ifdef INNBOX
         QString apiUrl = QString("https://%1/").arg(m_Settings->value(QStringLiteral("Network/server"), QString("storage.innbox.app")).toString());
         #else
         QString apiUrl = QString("https://%1/").arg(m_Settings->value(QStringLiteral("Network/server"), QString("disk.direct")).toString());
         #endif
         #endif
         QNetworkRequest apiRequest(apiUrl);
         apiRequest.setRawHeader("content-type", "application/json");
         apiRequest.setRawHeader("authorization", m_req->headers()["authorization"]);

         QJsonDocument requestData(request);

         m_externalAccessManager->post(apiRequest, requestData.toJson(QJsonDocument::Compact));
         loop.exec();

         QJsonDocument responseData(response);
         endJsonReply(responseData.toJson(QJsonDocument::Compact), qhttp::ESTATUS_OK);
         return;
     }

     QString user = request["user"].toString();
     if (user.isEmpty())
         user = m_userName;

     // Identification logic
     QString auth = m_req->headers()["authorization"];
     QString sessionID = auth.startsWith("token:") ? auth.mid(6) : auth;
     QPointer<ClientSession> session = m_sessionAuthProvider->getSession(sessionID);
     bool isAdmin = session && session->getVar("isAdmin", false).toBool();
     bool isOwner = (!m_userName.isEmpty() && user == m_userName) || isAdmin;

     if (!isOwner) {
          QJsonObject response, result;
          result["success"] = false;
          result["message"] = tr("Unauthorized.");
          response["result"] = result;
          QJsonDocument responseData(response);
          endJsonReply(responseData.toJson(QJsonDocument::Compact), qhttp::ESTATUS_UNAUTHORIZED);
          return;
     }

     QJsonObject response, result;

    if (m_isServerNode)
    {
        //User profiles live on the legacy gateway during the GunDB migration
        if (! callUpstream(request, response))
        {
            result["success"] = false;
            result["message"] = tr("Upstream gateway error.");
            response["result"] = result;
        }
    } else {
        //call on client
        QEventLoop loop;
        bool ok = false;

        m_externalAccessManager = new QNetworkAccessManager(this);

        connect(m_externalAccessManager, &QNetworkAccessManager::sslErrors, this, [this] (QNetworkReply *reply, const QList<QSslError> &errors) {
            Q_FOREACH(QSslError error, errors)
            {
                qWarning() << "SSL notice:" << error.errorString();
            }
            reply->ignoreSslErrors();
        });

        connect(m_externalAccessManager, &QNetworkAccessManager::finished, this, [this, &loop, &ok, &response] (QNetworkReply *reply) {
            //API response
            if (reply->error() == QNetworkReply::NoError) {
                response = QJsonDocument::fromJson(reply->readAll()).object();
                ok = true;
            } else {
                qCritical() << "api call error:" <<  reply->errorString();
            }
            reply->deleteLater();
            loop.exit(0);
        });

        //Sign in to OMD via API
        #ifdef QT_DEBUG
        QString apiUrl = QString("http://localhost:8080/");
        #else
        #ifdef INNBOX
        QString apiUrl = QString("https://%1/").arg(m_Settings->value(QStringLiteral("Network/server"), QString("storage.innbox.app")).toString();
        #else
        QString apiUrl = QString("https://%1/").arg(m_Settings->value(QStringLiteral("Network/server"), QString("disk.direct")).toString());
        #endif
        #endif
        QNetworkRequest apiRequest(apiUrl);
        apiRequest.setRawHeader("content-type", "application/json");
        apiRequest.setRawHeader("authorization", m_req->headers()["authorization"]);

        QJsonDocument requestData(request);

        m_externalAccessManager->post(apiRequest, requestData.toJson(QJsonDocument::Compact));
        loop.exec();
    }

     QJsonDocument responseData(response);
     endJsonReply(responseData.toJson(QJsonDocument::Compact), qhttp::ESTATUS_OK);
}

void HttpHandler::bridgeGetPublicProfile(QJsonObject request)
{
     QString user = request["user"].toString();
     if (user.isEmpty())
         user = m_userName;

     QJsonObject response, result;

    if (m_isServerNode)
    {
        //User profiles live on the legacy gateway during the GunDB migration
        if (! callUpstream(request, response))
        {
            result["success"] = false;
            result["message"] = tr("Upstream gateway error.");
            response["result"] = result;
        }
    } else {
        //call on client
        QEventLoop loop;
        bool ok = false;

        m_externalAccessManager = new QNetworkAccessManager(this);

        connect(m_externalAccessManager, &QNetworkAccessManager::sslErrors, this, [this] (QNetworkReply *reply, const QList<QSslError> &errors) {
            Q_FOREACH(QSslError error, errors)
            {
                qWarning() << "SSL notice:" << error.errorString();
            }
            reply->ignoreSslErrors();
        });

        connect(m_externalAccessManager, &QNetworkAccessManager::finished, this, [this, &loop, &ok, &response] (QNetworkReply *reply) {
            //API response
            if (reply->error() == QNetworkReply::NoError) {
                response = QJsonDocument::fromJson(reply->readAll()).object();
                ok = true;
            } else {
                qCritical() << "api call error:" <<  reply->errorString();
            }
            reply->deleteLater();
            loop.exit(0);
        });

        //Sign in to OMD via API
        #ifdef QT_DEBUG
        QString apiUrl = QString("http://localhost:8080/");
        #else
        #ifdef INNBOX
        QString apiUrl = QString("https://%1/").arg(m_Settings->value(QStringLiteral("Network/server"), QString("storage.innbox.app")).toString());
        #else
        QString apiUrl = QString("https://%1/").arg(m_Settings->value(QStringLiteral("Network/server"), QString("disk.direct")).toString());
        #endif
        #endif
        QNetworkRequest apiRequest(apiUrl);
        apiRequest.setRawHeader("content-type", "application/json");
        apiRequest.setRawHeader("authorization", m_req->headers()["authorization"]);

        QJsonDocument requestData(request);

        m_externalAccessManager->post(apiRequest, requestData.toJson(QJsonDocument::Compact));
        loop.exec();
    }

     QJsonDocument responseData(response);
     endJsonReply(responseData.toJson(QJsonDocument::Compact), qhttp::ESTATUS_OK);
}

void HttpHandler::bridgeGetUserInfo(QJsonObject request)
{
     QString sessionID = request["session_id"].toString();

     QJsonObject result;

     if (sessionID.isEmpty() || !m_sessionAuthProvider->sessionExists(sessionID)) {
         result["valid"] = false;
         QJsonDocument responseData(result);
         endJsonReply(responseData.toJson(QJsonDocument::Compact), qhttp::ESTATUS_UNAUTHORIZED);
         return;
     }

     QPointer<ClientSession> session = m_sessionAuthProvider->getSession(sessionID);

     bool ok = false;
     QJsonObject profile;
     if (m_isServerNode)
     {
        //User profiles live on the legacy gateway during the GunDB migration
        ok = callUpstream(request, result);
        profile = result;
        QJsonDocument responseData(result);
        endJsonReply(responseData.toJson(QJsonDocument::Compact), ok ? qhttp::ESTATUS_OK : qhttp::ESTATUS_UNAUTHORIZED);
        return;
     }

     if (ok)
     {
         result["user"] = session->userName();
         result["displayname"] = profile["displayname"].toString();
         result["profileimage"] = profile["profileimage"].toString();
         result["isAdmin"] = profile["isAdmin"].toBool();
         result["group"] = profile["group"].toString();
         result["permissions"] = profile["permissions"].toString();
         result["email"] = profile["email"].toString();
         result["session_id"] = sessionID;
         result["valid"] = true;
         QJsonDocument responseData(result);
         endJsonReply(responseData.toJson(QJsonDocument::Compact), qhttp::ESTATUS_OK);
     } else
     {
         result["valid"] = false;
         QJsonDocument responseData(result);
         endJsonReply(responseData.toJson(QJsonDocument::Compact), qhttp::ESTATUS_UNAUTHORIZED);
     }

}

// The gateway is a signaling relay and keeps no local user accounts: user,
// profile and group APIs are served by the legacy gateway
// (Network/legacyServer) until accounts fully migrate to GunDB identities.
// Nodes proxy the same APIs to their own server.
bool HttpHandler::callUpstream(const QJsonObject &request, QJsonObject &response)
{
    QEventLoop loop;
    bool ok = false;

    m_externalAccessManager = new QNetworkAccessManager(this);

    connect(m_externalAccessManager, &QNetworkAccessManager::sslErrors, this, [this] (QNetworkReply *reply, const QList<QSslError> &errors) {
        Q_FOREACH(QSslError error, errors)
        {
            qWarning() << "SSL notice:" << error.errorString();
        }
        reply->ignoreSslErrors();
    });

    connect(m_externalAccessManager, &QNetworkAccessManager::finished, this, [this, &loop, &ok, &response] (QNetworkReply *reply) {
        //API response
        if (reply->error() == QNetworkReply::NoError) {
            response = QJsonDocument::fromJson(reply->readAll()).object();
            ok = true;
        } else {
            qCritical() << "api call error:" <<  reply->errorString();
        }
        reply->deleteLater();
        loop.exit(0);
    });

#ifdef SERVER
    QString apiUrl = QString("https://%1/signin").arg(m_Settings->value(QStringLiteral("Network/legacyServer"), QString("disk.direct")).toString());
#else
    #ifdef QT_DEBUG
    QString apiUrl = QString("http://localhost:8080/signin");
    #else
    #ifdef INNBOX
    QString apiUrl = QString("https://%1/signin").arg(m_Settings->value(QStringLiteral("Network/server"), QString("storage.innbox.app")).toString());
    #else
    QString apiUrl = QString("https://%1/signin").arg(m_Settings->value(QStringLiteral("Network/server"), QString("disk.direct")).toString());
    #endif
    #endif
#endif
    QNetworkRequest apiRequest(apiUrl);
    apiRequest.setRawHeader("content-type", "application/json");
    apiRequest.setRawHeader("authorization", m_req->headers()["authorization"]);

    m_externalAccessManager->post(apiRequest, QJsonDocument(request).toJson(QJsonDocument::Compact));
    loop.exec();
    return ok;
}

void HttpHandler::bridgeSignIn(QJsonObject request)
{
     bool ok = false;
     QJsonObject result;
     QJsonObject profile;
     QString deviceKey = request["device_key"].toString();
     bool hasKeyInRequest = !request["device_key"].toString().isEmpty();
     if (deviceKey.isEmpty()) {
          // For anonymous users, generate a random unique token instead of IP-based one
          // to prevent collisions for users behind the same NAT
          deviceKey = QUuid::createUuid().toString(QUuid::WithoutBraces);
     }
     QString password = request["password"].toString();
     QString username = (m_keyLogin && password.isEmpty()) ? deviceKey : request["username"].toString();
     bool remember =  request["remember"].toBool();
     QString sessionID;
     QString pkey;

     if (m_isServerNode)
     {

        if (!hasKeyInRequest && password.isEmpty()) { 
            ok = true;// Fallback for anonymous
            pkey = deviceKey;
            username = "anonymous";
        }
        sessionID = m_req->sessionId();
        if (sessionID.isEmpty())
            sessionID = m_sessionAuthProvider->getSessionIdForSharedPath("/", m_req);
        if (ok)
        {
            QString deviceId = request["deviceId"].toString();
            sessionID = (remember ? deviceKey : sessionID);
            profile = m_sessionAuthProvider->getUserProfile(username);
            
            if (username == "anonymous" && (profile.isEmpty() || profile["user"].toString().isEmpty())) {
                 profile["user"] = "anonymous";
                 profile["displayname"] = "anonymous";
                 profile["email"] = "";
                 profile["lang"] = "en"; 
                 profile["valid"] = true;
                 profile["isGuest"] = true;
            }
            //Only overwrite username if it is not anonymous token
            if (username != "anonymous")
                username = profile["user"].toString();

            // Fetch rights from Console and update profile
            updateProfileWithRights(profile, username, pkey);
        }
    }

    // No local match: sign in via the upstream gateway API. The gateway
    // proxies to the legacy gateway, which keeps the user database during
    // the GunDB migration.
    if (!ok)
    {
        if (!m_isServerNode && !password.isEmpty()) {
            if (m_sessionAuthProvider->checkUserPass(username, password)) {
                ok = true;
                sessionID = m_req->sessionId();
                if (sessionID.isEmpty())
                    sessionID = m_sessionAuthProvider->getSessionIdForSharedPath("/", m_req);
                sessionID = (remember ? deviceKey : sessionID);
                
                // Generate a local token/key for the session
                pkey = QString(QCryptographicHash::hash(QString("%1:%2").arg(username).arg(SimpleCrypt::defaultKey).toUtf8(), QCryptographicHash::Md5).toHex());
                
                profile["user"] = username;
                profile["displayname"] = username;
                profile["email"] = "";
                profile["lang"] = "en"; 
                profile["valid"] = true;
                profile["isGuest"] = false;
                
                // Fetch rights from Console and update profile
                updateProfileWithRights(profile, username, pkey);
            }
        }

        if (!ok) {
            QEventLoop loop;

            m_externalAccessManager = new QNetworkAccessManager(this);

            connect(m_externalAccessManager, &QNetworkAccessManager::sslErrors, this, [this] (QNetworkReply *reply, const QList<QSslError> &errors) {
                Q_FOREACH(QSslError error, errors)
                {
                    qWarning() << "SSL notice:" << error.errorString();
                }
                reply->ignoreSslErrors();
            });

            connect(m_externalAccessManager, &QNetworkAccessManager::finished, this, [this, &loop, &ok, &profile, &username, &sessionID, &pkey] (QNetworkReply *reply) {
                //API response
                if (reply->error() == QNetworkReply::NoError) {
                    QJsonObject data = QJsonDocument::fromJson(reply->readAll()).object();
                    profile = data["profile"].toObject();
                    sessionID = data["session_id"].toString();
                    pkey = data["token"].toString();
                    username = profile["user"].toString();

                    // Fetch rights from Console and update profile
                    updateProfileWithRights(profile, username, pkey);
                    ok = true;
                } else {
                    qCritical() << "Sign in error:" <<  reply->errorString();
                }
                reply->deleteLater();
                loop.exit(0);
            });

            //Sign in to OMD via API
            #ifdef SERVER
            //legacy gateway keeps the user database during migration
            QString signinAPI = QString("https://%1/signin").arg(m_Settings->value(QStringLiteral("Network/legacyServer"), QString("disk.direct")).toString());
            #else
            #ifdef STAGING
            QString signinAPI = QString("https://%1/signin").arg(m_Settings->value(QStringLiteral("Network/server"), QString("disk.direct")).toString());
            #else
            #ifdef QT_DEBUG
            QString signinAPI = QString("http://localhost:8080/signin");
            #else
            #ifdef INNBOX
            QString signinAPI = QString("https://%1/signin").arg(m_Settings->value(QStringLiteral("Network/server"), QString("storage.innbox.app")).toString());
            #else
            QString signinAPI = QString("https://%1/signin").arg(m_Settings->value(QStringLiteral("Network/server"), QString("disk.direct")).toString());
            #endif
            #endif
            #endif
            #endif
            QNetworkRequest signinRequest(signinAPI);
            signinRequest.setRawHeader("content-type", "application/json");

            QJsonDocument requestData(request);

            m_externalAccessManager->post(signinRequest, requestData.toJson(QJsonDocument::Compact));
            loop.exec();
        }
    }

    if (ok)
    {
        QPointer<ClientSession> session = m_sessionAuthProvider->addSessionFor(username, 86400000, sessionID); //24hours
        QHash<QString, QVariant> sessionVars;
        sessionVars["User"] = username;
        sessionVars["isAdmin"] = profile["isAdmin"].toBool();
        sessionVars["isGroupAdmin"] = profile["isGroupAdmin"].toBool();
        sessionVars["isGuest"] = profile["isGuest"].toBool();
        sessionVars["group"] = profile["group"].toString();

        // Populate permissions, status and token balance from profile
        if (profile.contains("permissions")) {
            QJsonObject rights = profile["permissions"].toObject();
            sessionVars["collab"] = rights["collab"].toBool();
            sessionVars["ai_chat"] = rights["ai_chat"].toBool();
            sessionVars["code"] = rights["code"].toBool();
        }
        sessionVars["status"] = profile["status"].toString();
        sessionVars["token_balance"] = profile["token_balance"].toDouble();
        sessionVars["token"] = pkey;

        session->setSessionVariables(sessionVars);
        profile["omd_key"] = pkey; // Ensure consistent API key across all devices
        result["profile"] =  profile;
        result["session_id"] = sessionID;
        result["device_key"] = (remember && !deviceKey.isEmpty())  ? deviceKey : "";
        result["token"] = pkey;
        result["valid"] = true;

        if (m_resp.isNull()) {
             qCritical() << "Response is null";
             m_working = false;
             m_reading = false;
             releaseHandler();
             return;
        }

        m_resp->addHeader("authentication-success", "success");
        saveSessionCookie(sessionID);
        qInfo() << "User signed in" << session->userName() << "session:" << session->sessionID();
        emit UserEvents::instance().signedIn(username);
        QJsonDocument responseData(result);
        endJsonReply(responseData.toJson(QJsonDocument::Compact), qhttp::ESTATUS_OK);
    }  else {
        result["valid"] = false;
        QJsonDocument responseData(result);
        endJsonReply(responseData.toJson(QJsonDocument::Compact), qhttp::ESTATUS_UNAUTHORIZED);
    }
}

void HttpHandler::bridgeInvite(QJsonObject request)
{
#ifdef SERVER
     QString sessionID = request["session_id"].toString();
     QString groupId = request["group"].toString();
     QString groupname = request["groupname"].toString();
     QString user = request["user"].toString();

     qInfo() << "invite:" << request;

     QJsonObject result;

     if (sessionID.isEmpty() || !m_sessionAuthProvider->sessionExists(sessionID)) {
         result["valid"] = false;
         QJsonDocument responseData(result);
         qCritical() << "Admin session expired" << sessionID;
         endJsonReply(responseData.toJson(QJsonDocument::Compact), qhttp::ESTATUS_UNAUTHORIZED);
         return;
     }

     QPointer<ClientSession> session = m_sessionAuthProvider->getSession(sessionID);

     //Invites are handled by the legacy gateway during the GunDB migration
     QJsonObject upstream;
     if (callUpstream(request, upstream))
     {
         QJsonDocument responseData(upstream);
         endJsonReply(responseData.toJson(QJsonDocument::Compact), qhttp::ESTATUS_OK);
         return;
     }

     result["success"] = false;
     QJsonDocument responseData(result);
     endJsonReply(responseData.toJson(QJsonDocument::Compact), qhttp::ESTATUS_OK);
#else
    Q_UNUSED(request)
    replyJsonNotImplemented();
#endif
}

void HttpHandler::bridgeCreateProfile(QJsonObject request)
{
#ifdef SERVER
	//Accounts are created on the legacy gateway during the GunDB migration
	QJsonObject response, result;
	if (! callUpstream(request, response))
	{
		result["success"] = false;
		result["message"] = tr("Upstream gateway error.");
		response["result"] = result;
	}
	QJsonDocument responseData(response);
	endJsonReply(responseData.toJson(QJsonDocument::Compact), qhttp::ESTATUS_OK);
#else
    Q_UNUSED(request)
    replyJsonNotImplemented();
#endif
}


void HttpHandler::bridgeUpdateProfileData(QJsonObject request)
{
    QJsonObject response, result;
    QJsonObject profile = request["profile"].toObject();
    QString user = profile["user"].toString();

    if (m_isServerNode) {
        //User profiles live on the legacy gateway during the GunDB migration
        if (! callUpstream(request, response))
        {
            result["success"] = false;
            response["result"] = result;
        }
    } else {
        QEventLoop loop;
        m_externalAccessManager = new QNetworkAccessManager(this);

        connect(m_externalAccessManager, &QNetworkAccessManager::sslErrors, this, [this] (QNetworkReply *reply, const QList<QSslError> &errors) {
            Q_FOREACH(QSslError error, errors)
            {
                qWarning() << "SSL notice:" << error.errorString();
            }
            reply->ignoreSslErrors();
        });

        connect(m_externalAccessManager, &QNetworkAccessManager::finished, this, [this, &loop, &response, &result] (QNetworkReply *reply) {
            //API response
            if (reply->error() == QNetworkReply::NoError) {
                response = QJsonDocument::fromJson(reply->readAll()).object();
            } else {
                result["success"] = false;
                response["result"] = result;
                qCritical() << "api call error:" <<  reply->errorString();
            }
            reply->deleteLater();
            loop.exit(0);
        });

        //Sign in to OMD via API
        #ifdef QT_DEBUG
        QString apiUrl = QString("http://localhost:8080/");
        #else
        #ifdef INNBOX
        QString apiUrl = QString("https://%1/").arg(m_Settings->value(QStringLiteral("Network/server"), QString("storage.innbox.app")).toString();
        #else
        QString apiUrl = QString("https://%1/").arg(m_Settings->value(QStringLiteral("Network/server"), QString("disk.direct")).toString());
        #endif
        #endif
        QNetworkRequest apiRequest(apiUrl);
        apiRequest.setRawHeader("content-type", "application/json");
        apiRequest.setRawHeader("authorization", m_req->headers()["authorization"]);

        QJsonDocument requestData(request);

        m_externalAccessManager->post(apiRequest, requestData.toJson(QJsonDocument::Compact));
        loop.exec();

    }

    QJsonDocument responseData(response);
    endJsonReply(responseData.toJson(QJsonDocument::Compact), qhttp::ESTATUS_OK);
}

void HttpHandler::bridgeMove(QJsonObject request)
{
    QJsonArray paths = request["items"].toArray();
    QStringList baseDestPath = request["newPath"].toString().split("/", Qt::SplitBehaviorFlags::SkipEmptyParts);

    QHash<QString, QString> errList;
    m_reading = true;
    QString destID = m_rootResource->linkForPath(baseDestPath);

    Q_FOREACH(QJsonValue s_path, paths)
    {
        QHash<QString, QString> currentErrList;
        QStringList path = s_path.toString().split("/", Qt::SplitBehaviorFlags::SkipEmptyParts);

        CloudResource* item = m_rootResource->getChildClone(path, m_req->url().query(QUrl::FullyDecoded));

		if (item == nullptr)
        {
            replyJsonNotFound();
            return;
        }

        m_resourceCollector.watchFor(item);
        if (! m_working) {
            releaseHandler();
            return;
        }


		if (!item->exists())
		{
			replyJsonNotFound();
			return;
		}

        //check same device
        #ifdef SERVER
        QStringList destPath = baseDestPath;
        destPath.append(path.last());
        destPath = m_rootResource->getRemotePath(baseDestPath).split("/", Qt::SplitBehaviorFlags::SkipEmptyParts);
        path = m_rootResource->getRemotePath(path).split("/", Qt::SplitBehaviorFlags::SkipEmptyParts);
        destPath.append(path.last());
        bool sameDevice = m_rootResource->linkVersion(path.first()).startsWith("2.1.5") && destPath.first() == path.first();
        QString destResourcePath = "/" + destPath.join("/");
        #else
        QStringList destPath = baseDestPath;
        destPath.append(path.last());
        bool sameDevice = true;
        QString destResourcePath = m_rootResource->getLocalPath(destPath);
        #endif

        //check version
        if (sameDevice )
        {
            //try new implementation
            if (! item->move(destResourcePath)) {
                replyJsonError(tr("Error while moving '%1' to '%2'. Upgrade your client").arg(s_path.toString(), destResourcePath));
                return;
            }
            emit UserEvents::instance().bytesUploaded(m_rootOwner, item->size());

            if (item->isDir())
                destResourcePath.append("/");
            // move in index
            emit UserEvents::instance().fileRemoved(item->owner(m_rootOwner), m_userName, item->remotePath());
            emit UserEvents::instance().fileUpdated(item->owner(m_rootOwner), m_userName,  destResourcePath);

            continue;
        }


        //legacy way
        destPath = baseDestPath;
        destPath.append(path.last());
        CloudResource* dstItem = m_rootResource->touch(destPath);

        if (!dstItem)
        {
            replyJsonNotFound();
            return;
        }

        m_resourceCollector.watchFor(dstItem);

        if (! m_working) {
            releaseHandler();
            return;
        }

        if (! dstItem->isWritable()) {
                replyJsonError(tr("Can not move '%1' to '%2'").arg(s_path.toString(), destResourcePath));
                return;
        }

        item->moveTo(dstItem, &currentErrList);
        qInfo() << "moveTo:" << destResourcePath;

        if (currentErrList.size() == 0)
        {
            item->SetParentOutdated();
            dstItem->SetParentOutdated();

            emit UserEvents::instance().fileRemoved(item->owner(m_rootOwner), m_userName, item->remotePath());
            emit UserEvents::instance().fileUpdated(dstItem->owner(m_rootOwner), m_userName, dstItem->remotePath());
        }
        else
        {
            Q_FOREACH(QString path, currentErrList.keys())
            {
                errList.insert(path, currentErrList.value(path));
            }
        }
	}


    if (errList.size() > 0)
    {
        replyJsonError(errList.values().first());
        return;
    }

    if (!m_delayedRelease)
        replyJsonSuccess();
}

void HttpHandler::bridgeCopy(QJsonObject request)
{
    QJsonArray paths = request["items"].toArray();
    QStringList baseDestPath = request["newPath"].toString().split("/", Qt::SplitBehaviorFlags::SkipEmptyParts);

    m_reading = true;
    QString destID = m_rootResource->linkForPath(baseDestPath);

    QHash<QString, QString> errList;
    Q_FOREACH(QJsonValue s_path, paths)
    {
        QHash<QString, QString> currentErrList;
        QStringList path = s_path.toString().split("/", Qt::SplitBehaviorFlags::SkipEmptyParts);

        CloudResource* item = m_rootResource->getChildClone(path, m_req->url().query(QUrl::FullyDecoded));

		if (item == nullptr)
        {
            replyJsonNotFound();
            return;
        }

        m_resourceCollector.watchFor(item);

        if (! m_working) {
            releaseHandler();
            return;
        }


		if (!item->exists())
		{
			replyJsonNotFound();
			return;
		}

        //check same device
        #ifdef SERVER
        QStringList destPath = baseDestPath;
        destPath.append(paths.count() == 1 ? request["singleFilename"].toString() : item->fileName());
        destPath = m_rootResource->getRemotePath(destPath).split("/", Qt::SplitBehaviorFlags::SkipEmptyParts);
        path = m_rootResource->getRemotePath(path).split("/", Qt::SplitBehaviorFlags::SkipEmptyParts);
        bool sameDevice = m_rootResource->linkVersion(path.first()).startsWith("2.1.5") && destPath.first() == path.first() && !destPath.contains("shared");
        QString destResourcePath = "/" + destPath.join("/");
        #else
        QStringList destPath = baseDestPath;
        destPath.append(paths.count() == 1 ? request["singleFilename"].toString() : item->fileName());
        bool sameDevice = true;
        QString destResourcePath = m_rootResource->getLocalPath(destPath);
        #endif

        //check version
        if (sameDevice)
        {
            //try new implementation
            if (! item->copy(destResourcePath)) {
                replyJsonError(tr("Error while copying '%1' to '%2'. Upgrade your client").arg(s_path.toString(), destResourcePath));
                return;
            }

            emit UserEvents::instance().bytesUploaded(m_userName, item->size());
            //copy in index
            emit UserEvents::instance().fileUpdated(item->owner(m_rootOwner), m_userName,  destResourcePath);
            //qInfo() << "same device copy:" << destResourcePath << m_rootResource->linkVersion(path.first());
            continue;
        }


        //legacy fallback
        destPath = baseDestPath;
        destPath.append(paths.count() == 1 ? request["singleFilename"].toString() : item->fileName());
        CloudResource* dstItem = m_rootResource->touch(destPath);

        if (!dstItem)
        {
            replyJsonNotFound();
            return;
        }

        m_resourceCollector.watchFor(dstItem);

        if (! m_working) {
            releaseHandler();
            return;
        }

        if (! dstItem->isWritable()) {
            replyJsonError(tr("Can not copy '%1' to '%2'").arg(s_path.toString(), destResourcePath));
            return;
        }

        item->copyTo(dstItem, &currentErrList);

        //qInfo() << "copyTo:" << destResourcePath << m_rootResource->linkVersion(path.first());

        // copy in index
        emit UserEvents::instance().fileUpdated(dstItem->owner(m_rootOwner), m_userName, destResourcePath);

        if (currentErrList.size() == 0)
            dstItem->SetParentOutdated();
        else
        {
            Q_FOREACH(QString path, currentErrList.keys())
            {
                errList.insert(path, currentErrList.value(path));
            }
        }
    }


    if (errList.size() > 0)
    {
        replyJsonError(errList.values().first());
        return;
    }

    if (!m_delayedRelease)
        replyJsonSuccess();
}


void HttpHandler::replyJsonMethodNotAllowed()
{
    QJsonObject result;
    result["success"] = false;

    QJsonObject error;
    error["message"] = "Method no allowed";

    QJsonObject response;
    response["result"] = result;
    response["error"] = error;

    //Write response
    QJsonDocument responseData(response);
    endJsonReply(responseData.toJson(QJsonDocument::Compact), qhttp::ESTATUS_OK);
}

void HttpHandler::replyJsonBadRequest(QString s_details)
{
    QJsonObject result;
    result["success"] = false;

    QJsonObject error;
    error["message"] = "Bad request [" + s_details + "]";

    QJsonObject response;
    response["result"] = result;
    response["error"] = error;

    //Write response
    QJsonDocument responseData(response);
    endJsonReply(responseData.toJson(QJsonDocument::Compact), qhttp::ESTATUS_OK);
}

void HttpHandler::replyJsonRequestEntityTooLarge(QString s_details)
{
    QJsonObject result;
    result["success"] = false;

    QJsonObject error;
    error["message"] = "Request Entity Too Large [" + s_details + "]";

    QJsonObject response;
    response["result"] = result;
    response["error"] = error;

    //Write response
    QJsonDocument responseData(response);
    endJsonReply(responseData.toJson(QJsonDocument::Compact), qhttp::ESTATUS_OK);
}

void HttpHandler::replyJsonInternalServerError(QString s_details)
{
    QJsonObject result;
    result["success"] = false;

    QJsonObject error;
    error["message"] = "Gateway error :( [" + s_details + "]";

    QJsonObject response;
    response["result"] = result;
    response["error"] = error;

    //Write response
    QJsonDocument responseData(response);
    endJsonReply(responseData.toJson(QJsonDocument::Compact), qhttp::ESTATUS_OK);
}

void HttpHandler::replyJsonNotFound()
{
    QJsonObject result;
    result["success"] = false;

    QJsonObject error;
    error["message"] = "Not found";

    QJsonObject response;
    response["result"] = result;
    response["error"] = error;

    //Write response
    QJsonDocument responseData(response);
    endJsonReply(responseData.toJson(QJsonDocument::Compact), qhttp::ESTATUS_OK);
}

void HttpHandler::replyJsonForbidden()
{
    QJsonObject result;
    result["success"] = false;

    QJsonObject error;
    error["message"] = "Access denied";

    QJsonObject response;
    response["result"] = result;
    response["error"] = error;

    //Write response
    QJsonDocument responseData(response);
    endJsonReply(responseData.toJson(QJsonDocument::Compact), qhttp::ESTATUS_OK);
}

void HttpHandler::endJsonReply(QByteArray body, qhttp::TStatusCode code)
{
    m_working = false;
    m_reading = false;
    m_writing = false;
    if (!m_resp.isNull()) {
        body.append("\n");
        m_resp->addHeader("Content-Type", "text/json; charset=UTF-8");
        m_resp->addHeader("Content-Length", QString::number(body.size()).toUtf8());
        setServerString();
        m_resp->setStatusCode(code);
        m_resp->end(body);
        releaseHandler();
    } else
        releaseHandler();
}

void HttpHandler::endChunkedReply(QByteArray chunk, qhttp::TStatusCode code)
{
    m_working = false;
    m_reading = false;
    m_writing = false;
    if (!m_resp.isNull()) {
        setServerString(); // Ensure CORS headers are added before ending chunked response
        QString chunkHeader = QString("%1\r\n").arg(chunk.size(), 0, 16);
        chunk.prepend(chunkHeader.toLatin1());
        chunk.append("\r\n");
        chunk.append("0\r\n\r\n");
        m_resp->setStatusCode(code);
        m_resp->end(chunk);
    } else
        releaseHandler();
}

void HttpHandler::writeChunk(QByteArray chunk, qhttp::TStatusCode code)
{
    QString chunkHeader = QString("%1\r\n").arg(chunk.size(), 0, 16);
    chunk.prepend(chunkHeader.toLatin1());
    chunk.append("\r\n");
    setServerString();
    m_resp->setStatusCode(code);
    m_resp->write(chunk);
    m_resp->flush();
}

void HttpHandler::replyJsonNotImplemented()
{
    QJsonObject result;
    result["success"] = false;

    QJsonObject error;
    error["message"] = "Not Implemented";

    QJsonObject response;
    response["result"] = result;
    response["error"] = error;

    //Write response
    QJsonDocument responseData(response);
    endJsonReply(responseData.toJson(QJsonDocument::Compact), qhttp::ESTATUS_OK);
}

void HttpHandler::replyJsonNotModified(QString ETag, QString LastModified)
{
    if (!m_resp.isNull()) {
        m_resp->addHeader("ETag", ETag.toUtf8());
        m_resp->addHeader("Last-Modified", LastModified.toUtf8());
        setServerString();
    } else {
        releaseHandler();
        return;
    }

    QJsonObject result;
    result["success"] = false;

    QJsonObject error;
    error["message"] = "Not modified";

    QJsonObject response;
    response["result"] = result;
    response["error"] = error;

    //Write response
    QJsonDocument responseData(response);
    endJsonReply(responseData.toJson(QJsonDocument::Compact), qhttp::ESTATUS_OK);
}


void HttpHandler::replyJsonMoved(QUrl s_newUrl)
{
    if (!m_resp.isNull()) {
        m_resp->addHeader("Location", s_newUrl.toString(QUrl::FullyEncoded).toUtf8());
    } else {
        releaseHandler();
        return;
    }
    QJsonObject result;
    result["success"] = false;

    QJsonObject error;
    error["message"] = s_newUrl.toString();

    QJsonObject response;
    response["result"] = result;
    response["error"] = error;

    //Write response
    QJsonDocument responseData(response);
    endJsonReply(responseData.toJson(QJsonDocument::Compact), qhttp::ESTATUS_MOVED_PERMANENTLY);
}

void HttpHandler::replyJsonSeeOther(QUrl s_newUrl)
{
    if (!m_resp.isNull()) {
        m_resp->addHeader("Location", s_newUrl.toString(QUrl::FullyEncoded).toUtf8());

    } else {
        releaseHandler();
        return;
    }
    QJsonObject result;
    result["success"] = false;

    QJsonObject error;
    error["message"] = s_newUrl.toString();

    QJsonObject response;
    response["result"] = result;
    response["error"] = error;

    //Write response
    QJsonDocument responseData(response);
    endJsonReply(responseData.toJson(QJsonDocument::Compact), qhttp::ESTATUS_FOUND);
}

void HttpHandler::replyJsonSuccess()
{
    QJsonObject result;
    result["success"] = true;

    QJsonObject response;
    response["result"] = result;

    //Write response
    QJsonDocument responseData(response);
    endJsonReply(responseData.toJson(QJsonDocument::Compact), qhttp::ESTATUS_OK);
}

void HttpHandler::replyJsonBasic(int error)
{
    QJsonObject result;
    result["error"] = error;
    //Write response
    QJsonDocument responseData(result);
    endJsonReply(responseData.toJson(QJsonDocument::Compact), qhttp::ESTATUS_OK);
}

void HttpHandler::replyJsonError(QString errorString)
{
    QJsonObject result;
    result["success"] = false;

    QJsonObject error;
    error["message"] = errorString;

    QJsonObject response;
    response["result"] = result;
    response["error"] = error;

    //Write response
    QJsonDocument responseData(response);
    endJsonReply(responseData.toJson(QJsonDocument::Compact), qhttp::ESTATUS_OK);
}

bool HttpHandler::parsePostData(QHash<QByteArray, QByteArray> *parsedPostData)
{
    m_buffer.seek(0);
    if (m_req->headers()["content-type"].startsWith("application/x-www-form-urlencoded"))
    {
        m_postData = m_buffer.readAll();
        QList<QByteArray> postDataPairs = m_postData.split('&');
        Q_FOREACH(QByteArray postDataPair, postDataPairs)
        {
            if (postDataPair.contains("="))
            {
                postDataPair = postDataPair.replace("+", " ");
                QByteArray key = QByteArray::fromPercentEncoding(postDataPair.mid(0, postDataPair.indexOf('=')));
                QByteArray value = QByteArray::fromPercentEncoding(postDataPair.mid(postDataPair.indexOf('=') + 1));
                parsedPostData->insert(key, value);
            }
            else
                parsedPostData->insert(QByteArray::fromPercentEncoding(postDataPair.replace("+", " ")), "");
        }

        return true;
    }
    else if (m_req->headers()["content-type"].startsWith("multipart/form-data"))
    {
        QByteArray boundary = m_req->headers()["content-type"].mid(m_req->headers()["content-type"].indexOf("=") + 1);
        boundary = boundary.prepend("--");
        QByteArray lastBoundary = boundary;
        lastBoundary = lastBoundary.append("--");
        //qDebug() << boundary;
        //qDebug() << lastBoundary;

        //skip empty lines prior first boundary:
        QByteArray postString;// = m_buffer.readLine();
        while (!postString.trimmed().startsWith(boundary))
        {
            postString = m_buffer.readLine();
        }
        bool isReadingHeader = true;
        bool isReadingBody = false;
        QByteArray partName;

        //read parts:
        while (!postString.startsWith(lastBoundary))
        {
            postString = m_buffer.readLine();
            if (isReadingHeader)
            {
                if (postString.startsWith("Content-Disposition"))
                {
                    QList<QByteArray> params = postString.split(';');
                    Q_FOREACH(QByteArray param, params)
                    {
                        param = param.trimmed();
                        if (param.startsWith("name="))
                        {
                            partName = param.mid(5);
                            if (partName.startsWith("\""))
                                partName = partName.mid(1);
                            if (partName.endsWith("\""))
                                partName = partName.left(partName.length() -1);
                            parsedPostData->insert(partName, "");
                            break;
                        }
                    }
                }

                if (postString == "\r\n")
                {
                    isReadingBody = true;
                    isReadingHeader = false;
                }

                continue;
            }

            if (isReadingBody)
            {
                if (postString.startsWith(boundary))
                {
                    isReadingBody = false;
                    isReadingHeader = true;
                    parsedPostData->operator [](partName) = parsedPostData->operator [](partName).left(parsedPostData->operator [](partName).length() - 2);
                    continue;
                }

                parsedPostData->operator [](partName).append(postString);
            }
        }

        return true;
    }
    else
    {
        return false;
    }
}

void HttpHandler::endHandlePut()
{
    if (m_Reciever && m_Reciever->isOpen())
        m_Reciever->close();

    if (!m_resp.isNull()) {
        setServerString();
        m_resp->setStatusCode(qhttp::ESTATUS_CREATED);
        if (m_putJsonResponse)
            replyJsonSuccess();
        else
            replyCreated();

        //reindex affected path
        QString fileName = m_Reciever->fileName();
        if (CloudResource::isSkipped(fileName)) return;
        QString path = m_Reciever->remotePath();
        if (path != "/") { //crawl location
            emit UserEvents::instance().fileUpdated(m_Reciever->owner(m_rootOwner), m_userName,  path);
        }
    } else
        releaseHandler();
}


void HttpHandler::readNextDataPart()
{    
    //qDebug() << "Sent" << m_resp->bytesToSend() <<  m_bytesToSend << m_totalBytes << m_requestPackets;
    if (! m_resp.isNull() && m_working)
    {
        if (m_bytesToSend > 0 && m_resp->bytesToSend() <= (qint64) m_maxBufferSize * PACKETS_AHEAD ) {
             //qDebug() << "Requesting..." << m_reading << m_resp->bytesToSend() <<  m_bytesToSend << m_totalBytes << m_requestPackets;
             requestData();
        }
    }
}


void HttpHandler::requestData()
{
    if (!m_working ) {
        m_reading = false;
        releaseHandler();
        return;
    }

    if (m_requestPackets < PACKETS_AHEAD)
        ++m_requestPackets;

    if (m_reading  ) {
        //qDebug()  << "Already reading" << m_resp->bytesToSend() <<  m_bytesToSend << m_totalBytes << m_requestPackets;
        return;
    }

    if (m_reader && m_bytesToSend > 0 && !m_resp.isNull() )
    {
        m_reading = true;

        while (m_working && !m_resp.isNull() && m_bytesToSend > 0 && m_resp->bytesToSend() < (qint64)m_maxBufferSize * PACKETS_AHEAD  &&  m_requestPackets > 0 && !m_reader->atEnd()) {
             //qDebug() << "Reading" << m_resp->bytesToSend() <<  m_bytesToSend << m_totalBytes << m_requestPackets;
             --m_requestPackets;
             m_reader->requestData(m_maxBufferSize);
        }

        m_reading = false;
    }
}

void HttpHandler::converterDataReady()
{
    QByteArray textData = m_converter->readAllStandardOutput();
    if (!m_resp.isNull() && !textData.isEmpty())
    {
        writeReply(textData, qhttp::ESTATUS_OK);
    }
}

void HttpHandler::converterCompleted(int exitCode, QProcess::ExitStatus exitStatus)
{
    Q_UNUSED(exitStatus)
    if (!m_resp.isNull())
    {
        if (exitCode == 0) {
            QByteArray textData = m_converter->readAllStandardOutput();
            endReply(textData, qhttp::ESTATUS_OK );
        } else
            replyNoContent();
    } else {
        m_working = false;
        m_reading = false;
        releaseHandler();
    }
}

void HttpHandler::requestedDataReady(const QByteArray &data, int size)
{
    if (!m_working ) {
        m_reading = false;
        releaseHandler();
        return;
    }

    //Increase/decrease counters
    ++m_requestPackets;
    m_bytesToSend -= size;
    if (m_bytesToSend < 0) m_bytesToSend = 0;
    m_bytesReceived += size;

    // put to converter if converting
    if (m_converting) {
        if (m_converter->isWritable()) {
            m_converter->write(data);
            if ( m_reader->atEnd() || m_bytesToSend == 0 ) { // introduce some limit
                m_converter->closeWriteChannel();
                m_reading = false;
            }
        } else {
            m_reading = false;
            releaseHandler();
        }
        return;
    }

    //return responce
    QByteArray dataToSend = data;
    if (m_reader->isChanked()) {
        QString chunkHeader = QString("%1\r\n").arg(size, 0,16);
        dataToSend.prepend(chunkHeader.toLatin1());
        dataToSend.append("\r\n");
    }

    if (!m_resp.isNull())
    {
         if ( m_reader->atEnd() || m_bytesToSend == 0  || size == 0) {
                //qInfo() << "Completed" << m_req->url() << m_reader->atEnd() << m_totalBytes << m_reader->size();
                if (m_reader->isChanked())
                    dataToSend.append(m_reader->getLastBytes());
                m_working = false;
                m_reading = false;
                m_resp->end(dataToSend);
                releaseHandler();
         } else {
             m_resp->write(dataToSend);
         }
    } else {
        m_working = false;
        m_reading = false;
        releaseHandler();
    }
}


void HttpHandler::handleGetFile(CloudResource* cloudResource, bool asHead)
{
    if (!asHead && isNotExpired(cloudResource))
    {
        replyNotModified(cloudResource->ETag(), cloudResource->lastModifiedString());
    }
    else
    {
        sendFile(cloudResource, asHead);
    }
}


void HttpHandler::handleGet(bool asHead)
{
 //   QString pathPrefix = m_req->urlPath().size() > 0 ? m_req->urlPath().at(0) : "";
    if (m_aborted.load(std::memory_order_acquire) || m_stopped.load(std::memory_order_acquire) || !m_working || m_released || !m_req || m_req.isNull()) {
        releaseHandler();
        return;
    }


	CloudResource* requestedResource = nullptr;
#if defined(SERVER) || defined(P2P_ENABLED)
    bool isPublicLink = m_req->url().path(QUrl::PrettyDecoded).startsWith(SHARED_LINK_PREFIX);
#else
    bool isPublicLink = false;
    #endif
    m_reading = true;
    m_working = true;
    bool indexing =  false;
    bool attrQuery =  false;
    bool listing = false;

    QString  urlPath = m_req->url().path(QUrl::FullyDecoded);

#ifdef SERVER
    //Public site metadata for the static site bootstrap (in-memory lookup,
    //no LinkThread, no RPC): the visitor's ServiceWorker tunnel serves the content.
    if (urlPath == "/api/site-meta")
    {
        QUrlQuery q(m_req->url().query(QUrl::FullyDecoded));
        QString siteName = q.queryItemValue("name");
        QString host = m_req->headers()["host"];
        if (host.contains(":")) host = host.split(":")[0];

        SiteInfo site;
        bool found = (!siteName.isEmpty() && OnMyDiskLinkServer::instance().getSite(siteName, site))
                  || (!host.isEmpty() && OnMyDiskLinkServer::instance().getSiteByDomain(host, site));
        QJsonObject response;
        if (found) {
            QJsonObject result;
            result["name"] = site.name;
            result["owner"] = site.owner;
            result["linkId"] = site.linkID;
            result["type"] = site.type;
            result["domain"] = site.domain;
            response["status"] = "ok";
            response["result"] = result;
        } else {
            response["status"] = "error";
            response["error"] = "Site not found";
        }
        endReply(QJsonDocument(response).toJson(QJsonDocument::Compact), "application/json", qhttp::ESTATUS_OK);
        return;
    }

    //Static site bootstrap: Jekyll sites are served P2P by the visitor's
    //ServiceWorker + page tunnel; the gateway only hands out the loader page.
    if (urlPath.startsWith(SITES_PREFIX))
    {
        QString siteFile = m_Settings->value(QStringLiteral("docRoot"), QStringLiteral("/srv/www")).toString() + "/site-view.html";
        QFile sitePage(siteFile);
        QByteArray siteHtml;
        if (sitePage.open(QIODevice::ReadOnly))
        {
            siteHtml = sitePage.readAll();
            sitePage.close();
        }
        if (!siteHtml.isEmpty()) {
            m_resp->addHeader("Cache-Control", "no-cache");
            endReply(siteHtml, "text/html; charset=UTF-8", qhttp::ESTATUS_OK);
        } else
            replyNotFound();
        return;
    }

    //Zero-config P2P App Proxy bootstrap: if a tab hard-reloads /__app/... bypassing
    //the ServiceWorker, re-claim SW control and reload into the P2P tunnel.
    if (urlPath.startsWith(QStringLiteral("/__app/")))
    {
        static const QByteArray appLoaderHtml(
            "<!doctype html><html><head><meta charset=\"utf-8\">"
            "<title>Connecting P2P App…</title>"
            "<style>body{margin:0;height:100vh;display:flex;align-items:center;justify-content:center;"
            "background:#0f141b;color:#e6edf3;font:14px system-ui,sans-serif}</style></head>"
            "<body>Connecting P2P App Proxy…<script>"
            "(async()=>{"
            "if(!('serviceWorker' in navigator))return;"
            "const reg=await navigator.serviceWorker.register('/service-worker.js');"
            "await navigator.serviceWorker.ready;"
            "if(!navigator.serviceWorker.controller&&reg.active){"
            "await new Promise(r=>{navigator.serviceWorker.addEventListener('controllerchange',r,{once:true});"
            "reg.active.postMessage({type:'claim'});setTimeout(r,1500);});"
            "}"
            "if(navigator.serviceWorker.controller)location.reload();"
            "})();"
            "</script></body></html>"
        );
        m_resp->addHeader("Cache-Control", "no-store");
        endReply(appLoaderHtml, "text/html; charset=UTF-8", qhttp::ESTATUS_OK);
        return;
    }
#endif //SERVER

    QString subUrl = "";
#ifdef SERVER
    if (urlPath == "/api/ai-node") {
        QJsonObject response;
        SiteInfo site;
        if (OnMyDiskLinkServer::instance().getPublicAiNode(site)) {
            QJsonObject aiNode;
            aiNode["linkID"] = site.linkID;
            aiNode["name"] = site.name;
            aiNode["shareName"] = site.shareName;
            aiNode["owner"] = site.owner;
            aiNode["type"] = QStringLiteral("ai-node");
            QJsonArray caps;
            caps.append(QStringLiteral("chat"));
            aiNode["capabilities"] = caps;
            aiNode["status"] = QStringLiteral("online");
            response["status"] = "ok";
            response["aiNode"] = aiNode;
        } else {
            response["status"] = "ok";
            response["aiNode"] = QJsonValue(QJsonValue::Null);
        }
        endReply(QJsonDocument(response).toJson(QJsonDocument::Compact), "application/json", qhttp::ESTATUS_OK);
        return;
    }

    if (urlPath == "/api/devices/nearby" || urlPath == "/api/my-nearby") {
        QString clientIp;
        if (m_req->headers().contains("x-forwarded-for")) {
            clientIp = QString::fromUtf8(m_req->headers()["x-forwarded-for"]).split(',').first().trimmed();
        } else if (m_req->headers().contains("X-Forwarded-For")) {
            clientIp = QString::fromUtf8(m_req->headers()["X-Forwarded-For"]).split(',').first().trimmed();
        } else {
            clientIp = m_req->remoteAddress();
        }

        QList<QJsonObject> nodes = OnMyDiskLinkServer::instance().getNearbyNodes(clientIp);
        QJsonArray nearbyArray;
        for (const auto &n : nodes) {
            nearbyArray.append(n);
        }

        QJsonObject response;
        response["status"] = "ok";
        response["clientIp"] = clientIp;
        response["devices"] = nearbyArray;
        endReply(QJsonDocument(response).toJson(QJsonDocument::Compact), "application/json", qhttp::ESTATUS_OK);
        return;
    }

    if (urlPath == "/api/signaling") {
        // Ownership is bound to the session (see the POST handler).
        const QString requester = m_userName;
        if (requester.isEmpty())
        {
            QJsonObject response;
            response["status"] = "error";
            response["error"] = "Authentication required";
            endJsonReply(QJsonDocument(response).toJson(QJsonDocument::Compact), qhttp::ESTATUS_UNAUTHORIZED);
            return;
        }

        QUrlQuery query(m_req->url());
        QString ID = query.queryItemValue("ID");
        QString linkID = query.queryItemValue("linkID");
        QString kind = query.queryItemValue("kind");
        QString payloadB64 = query.queryItemValue("payload");
        quint16 port = query.hasQueryItem("port") ? query.queryItemValue("port").toUInt() : 0;
        QString service = query.queryItemValue("service");

        // Web device polling: when kind=poll and ID is the sentinel, use
        // getPendingTunnelOpen to deliver TunnelOpen signals to the browser
        if (kind == "poll" && !linkID.isEmpty() && ID == "client-device-poll")
        {
            QList<WebSignal> tunnelSignals = OnMyDiskLinkServer::instance().getPendingTunnelOpen(linkID, requester, 0);
            QJsonObject response;
            response["status"] = "ok";
            QJsonArray sigArray;
            for (const auto &sig : tunnelSignals)
            {
                QJsonObject sigObj;
                sigObj["kind"] = sig.kind;
                sigObj["tunnelID"] = sig.tunnelID;
                sigObj["payload"] = QString::fromUtf8(sig.payload.toBase64());
                sigArray.append(sigObj);
            }
            response["signals"] = sigArray;
            endReply(QJsonDocument(response).toJson(QJsonDocument::Compact), "application/json", qhttp::ESTATUS_OK);
            return;
        }

        if (!linkID.isEmpty() && !ID.isEmpty() && !kind.isEmpty())
        {
            QByteArray payload = QByteArray::fromBase64(payloadB64.toUtf8());
            bool ok = OnMyDiskLinkServer::instance().sendWebSignaling(linkID, ID, kind, payload, requester, port, service, m_userName);
            if (!ok)
            {
                replyJsonError(tr("Target node not connected or access denied"));
                return;
            }
        }

        if (!ID.isEmpty())
        {
            QList<WebSignal> webSignalsList = OnMyDiskLinkServer::instance().getWebSignals(ID, requester, 0);
            QJsonObject response;
            response["status"] = "ok";
            QJsonArray sigArray;
            for (const auto &sig : webSignalsList)
            {
                QJsonObject sigObj;
                sigObj["kind"] = sig.kind;
                sigObj["payload"] = QString::fromUtf8(sig.payload.toBase64());
                sigArray.append(sigObj);
            }
            response["signals"] = sigArray;

            endReply(QJsonDocument(response).toJson(QJsonDocument::Compact), "application/json", qhttp::ESTATUS_OK);
            return;
        }
        replyJsonBadRequest(tr("Missing ID parameter"));
        return;
    }
#endif

    if (urlPath == "/robots.txt") {
        QByteArray robotsTxt = "User-agent: *\nAllow: /\n";
        endReply(robotsTxt, "text/plain; charset=UTF-8", qhttp::ESTATUS_OK);
        return;
    } else
    if (urlPath == "/profile") {
        QUrl url = m_req->url();
        url.setPath("/");
        url.setFragment("settings/profile");
        replySeeOther(url);
        return;
    } else
    if (urlPath == "/tos" || urlPath == "/tos.html") {
        if (serveHtmlTemplatePage(QStringLiteral("tos.html"), QStringLiteral("tos"), asHead))
            return;
        QStringList path;
        path << "tos.html";
        requestedResource = m_rootResource->getChildClone(path);
    } else
    if (urlPath == "/about" || urlPath == "/about.html") {
        if (serveHtmlTemplatePage(QStringLiteral("about.html"), QStringLiteral("about"), asHead))
            return;
        QStringList path;
        path << "about.html";
        requestedResource = m_rootResource->getChildClone(path);
    } else
    if (urlPath == "/about-i18n.js") {
#if (!defined(P2P_ENABLED) && !defined(NO_WEB_UI)) || defined(SERVER)
        QByteArray jsData;
        if (m_rootResource) {
            QStringList path;
            path << QStringLiteral("about-i18n.js");
            CloudResource* res = m_rootResource->getChildClone(path);
            if (res) {
                m_resourceCollector.watchFor(res);
                if (res->exists() && res->isReadable() && res->openResource(QIODevice::ReadOnly) > 0) {
                    jsData = res->read(0, res->size());
                    res->close();
                }
            }
        }
        if (jsData.isEmpty() && m_Settings) {
            QFile file(m_Settings->value(QStringLiteral("docRoot"), QStringLiteral("/srv/www")).toString() + "/about-i18n.js");
            if (file.open(QIODevice::ReadOnly)) {
                jsData = file.readAll();
                file.close();
            }
        }
        if (!jsData.isEmpty()) {
            m_resp->addHeader("Cache-Control", "no-cache, must-revalidate");
            if (asHead) {
                m_resp->addHeader("Content-Length", QString::number(jsData.size()).toUtf8());
                endReply(QByteArray(), "application/javascript; charset=UTF-8", qhttp::ESTATUS_OK);
            } else {
                endReply(jsData, "application/javascript; charset=UTF-8", qhttp::ESTATUS_OK);
            }
            return;
        }
#endif
        QStringList path;
        path << "about-i18n.js";
        requestedResource = m_rootResource->getChildClone(path);
    } else
    if (urlPath.startsWith(CALL_LINK_PREFIX)) {
        QStringList path;
        path << "call.html";
        requestedResource = m_rootResource->getChildClone(path);
    } else
    if (urlPath == "/activate") {
        //Account activation is handled by the legacy gateway during the GunDB migration
        replyJsonError(tr("Account activation is handled by the legacy gateway."));
        return;
	}
	else {
        QString  userAgent = m_req->headers()["user-agent"];
		QString  query = m_req->url().query(QUrl::FullyDecoded);

        if (false) { //legacy Nextcloud integration removed
			//request from externall service
			query = "resize=true&width=960&height=960";
			//qInfo() << "Resizing Nextcloud request" << query;
		}


        subUrl =  m_req->subUrl();
        QUrlQuery q(query);

        if ((q.hasQueryItem("key")))
        {
            QString referer = m_req->headers()["Referer"];
            //  safer url with no key in address bar
            if (referer.isEmpty())
            {
                QUrl nextUrl;
                nextUrl.setHost(m_req->url().host());
                nextUrl.setScheme(m_req->url().scheme());
                nextUrl.setPort(m_req->url().port());
                nextUrl.setPath(urlPath);
                // Redirect back
                replySeeOther(nextUrl);
                return;
            }
        }

        if (!m_working || m_req.isNull() || m_resp.isNull()) {
            releaseHandler();
            return;
        }

        //metadata for shared links
        if  (isPublicLink  && (q.hasQueryItem("direct") || q.hasQueryItem("description") || q.hasQueryItem("cover") || q.hasQueryItem("totext") ||
             q.hasQueryItem("preview") || q.hasQueryItem("title") || q.hasQueryItem("poster") || q.hasQueryItem("readme")))
        {
            if (subUrl.isEmpty())
                subUrl = m_rootResource->sharedRootName();
        }

        //Search robot support
        indexing = m_rootOwner == "robot" || m_userName == "robot" || m_req->property("indexing").toBool();

        //fix metatags content type for old clients
        if ( q.hasQueryItem("description") || q.hasQueryItem("title") || q.hasQueryItem("readme") || q.hasQueryItem("totext"))
        {
            m_resp->addHeader("Content-Type", "text/plain; charset=UTF-8");
            attrQuery = true;
        }

        if ( q.hasQueryItem("totext") || q.hasQueryItem("direct"))
        {
            attrQuery = true;
        }

        if ( q.hasQueryItem("list"))
        {
            m_resp->addHeader("Content-Type", "application/json; charset=UTF-8");
            m_resp->addHeader("Cache-Control", "no-cache");
            listing = true;
        }

        if ( q.hasQueryItem("cover") || q.hasQueryItem("preview")  || q.hasQueryItem("poster") )
        {
            m_resp->addHeader("Content-Type", "image/jpeg");
            attrQuery = true;
        }

        QString redirectUrl;
        m_requestedItemPathList = !subUrl.isEmpty() ? subUrl.split("/", Qt::SplitBehaviorFlags::SkipEmptyParts) : m_req->urlPath();
        m_rpcCall = checkSitePath(urlPath,  redirectUrl);

        if (!m_working || m_req.isNull())
            return;

        if (!redirectUrl.isEmpty()) {
            QUrl nextUrl = m_req->url();
            nextUrl.setPath(redirectUrl);
            replySeeOther(nextUrl);
            return;
        }

        // Local AI proxy bypass (Embedded client - offline mode)
        if (m_localAiProxy) {
            handleLocalAiProxy(QByteArray());
            return;
        }

        if (indexing && m_requestedItemPathList.count() > 0 &&  m_requestedItemPathList[0] != "sites" && !m_rpcCallPath.startsWith("chat/") && m_rootOwner != "robot") {
            //remove user name prefix
            m_requestedItemPathList.removeFirst();
        }

        if (indexing && isPublicLink && m_requestedItemPathList.count() > 0 ) {
            //remove link itself
            m_requestedItemPathList.removeFirst();
        }
        // qWarning() << "Path after removal:" << m_requestedItemPathList;

        if (m_rpcCall)
            requestedResource = m_rootResource->getChildClone(m_requestedItemPathList, "", (indexing && !m_rpcCallPath.startsWith("chat/")));
        else
            requestedResource = m_rootResource->getChildClone(m_requestedItemPathList, query, indexing);

        if (! requestedResource)
        {
            replyNotFound();
            return;
        }

        m_resourceCollector.watchFor(requestedResource);

        //converting to text
        if (q.hasQueryItem("totext")) {
            query = "";
            QString fileName = m_rootResource->isSharedRoot() ? m_rootResource->sharedRootName() : requestedResource->fileName();
            QString suffix = QFileInfo(fileName).suffix().toLower();
            m_converting = true;
            QStringList arguments;
            m_converter = new QProcess(this);
            if (suffix == "pdf") {
                arguments << "-" << "-"; // Pass "-" to read from stdin and stdout
                m_converter->setProgram("pdftotext");
                arguments << "-layout"; // <-- чтобы сохранить структуру
                m_converter->setArguments(arguments);
            } else if (CloudResource::isConvertible(fileName)) {
                arguments << "--from=" + suffix << "--to=gfm";
                m_converter->setProgram("pandoc");
                m_converter->setArguments(arguments);
            } else {
                qCritical() << "Converter not supported for file:" << fileName;
                replyNoContent();
                return;
            }
            connect(m_converter, &QProcess::readyReadStandardOutput, this, &HttpHandler::converterDataReady);
            connect(m_converter, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this, &HttpHandler::converterCompleted);
            connect(m_converter, &QProcess::readyReadStandardError, this,  [this, fileName] () {
                qCritical() << "Converter error:" << fileName;
                qCritical() << m_converter->readAllStandardError().left(120);
            });
            m_converter->start();
            m_converter->waitForStarted();
        }


        if (!m_working || m_req.isNull()) {
            releaseHandler();
            return;
        }

        // RPC GET invokation - calling local apps over OMD proxy
        if (m_rpcCall) {
            QByteArray payload; //empty
            QHash<QString, QVariant> params;

            //add headers
            foreach(const QByteArray& key, m_req->headers().keys()) {
                params[QString::fromUtf8(key)] = QString::fromUtf8(m_req->headers()[key]);
            }
            
            // Inject Token Balance for rights enforcement
            QPointer<ClientSession> session = m_sessionAuthProvider->getSession(m_req->sessionId());
            if (session) {
                params["X-OMD-Token-Balance"] = QString::number(session->getVar("token_balance", 0.0).toDouble());

#ifdef SERVER
                // Private mode for the chat node owner or invited users
                QString userName = session->userName();
                if (!userName.isEmpty()) {
                    SiteInfo site;
                    if (OnMyDiskLinkServer::instance().getChatNode(userName, site)) {
                        bool isOwner = (userName == site.owner);
                        bool isSharedTo = site.users.contains(userName);
                        if (isOwner || isSharedTo) {
                            params["X-OMD-Private-Mode"] = "1";
                        }
                    }
                }
#endif
            }
            params["query"] = query;
            params["action"] = "get";
            if (isStreamingPath(m_rpcCallPath)) {
                params["accept"] = "text/event-stream";
            }

            m_reading = true;

            m_rpcReader = new  FileReader(requestedResource, this);
            connect(m_rpcReader, &FileReader::responseReady, this, [this] (const QHash<QString, QVariant> &params, const QByteArray& payload) {

                if (!m_working || m_resp.isNull()) {
                    releaseHandler();
                    return;
                }

                int statusCode = params.contains("statuscode") ? params["statuscode"].toInt() : 0;
                QString contentType = params.contains("Content-Type") ?  params["Content-Type"].toString() : params.contains("content-type") ?  params["content-type"].toString() : "";

                if (payload.size() == 0 && statusCode != 204 && statusCode != 304 &&  statusCode != 200 && statusCode != 101 && statusCode != 100 && statusCode != 102) {
                    qWarning() << "Empty response:" << statusCode << m_rpcCallPath;
                }

                if (statusCode == 0) {
                    replyJsonError("RPC error");
                    return;
                }

                if (params.contains("Location"))
                    m_resp->addHeader("Location", params["Location"].toString().toUtf8());

                //Populate headers
                foreach(const QString &key, params.keys()) {
                    QString lower = key.toLower();
                    if (lower != "statuscode" &&
                        lower != "content-length" &&
                        lower != "stream" &&
                        lower != "server" && lower != "access-control-allow-origin")
                            m_resp->addHeader(key.toUtf8(), params[key].toString().toUtf8());
                }

                bool streaming = params.contains("stream") && params["stream"].toBool();
                QString transferEncoding = params.contains("Transfer-Encoding") ?  params["Transfer-Encoding"].toString() : params.contains("transfer-encoding") ?  params["transfer-encoding"].toString() : "";
                bool chunked =  transferEncoding == "chunked";
                bool isDone = (statusCode == 204) || (params.contains("stream") && !params["stream"].toBool()) || payload.contains("\"done\": true") || payload.contains("\"done\":true");

                if (! streaming || isDone) {
                    m_reading = false;

                    if (chunked)
                        endChunkedReply(payload, qhttp::ESTATUS_OK);
                    else if (statusCode == 304)
                        replyNotModified();
                    else if (statusCode == 204)
                        replyNoContent();
                    else
                        endReply(payload, contentType, static_cast<qhttp::TStatusCode>(statusCode));

                } else {
                    if (!m_streaming) {
                        if (!contentType.isEmpty())
                            m_resp->addHeader("Content-Type", contentType.toUtf8()); // text/event-stream
                        m_resp->addHeader("Connection", "keep-alive");
                        m_resp->addHeader("X-Accel-Buffering", "no");
                        m_resp->addHeader("Cache-Control", "no-cache");
                        setServerString();
                        m_streaming = true;
                    }

                    if (chunked) {
                        writeChunk(payload,  static_cast<qhttp::TStatusCode>(statusCode));
                    } else {
                        writeReply(payload,  static_cast<qhttp::TStatusCode>(statusCode));
                    }
                }
            });

            if (!m_rpcReader->invoke(m_rpcCallPath, params, payload)) {
                replyNotFound();
            }
            return;
        }

        if (q.hasQueryItem("reload") )
        {
            m_resp->addHeader("Cache-Control", "max-age=0,no-cache,no-store,must-revalidate");
            m_resp->addHeader("Pragma", "no-cache");
            m_resp->addHeader("Expires", "Tue, 01 Jan 1970 00:00:00 GMT");
        }

        //Find the default item in the shared root for metadata requests
        if (m_rootResource->isSharedRoot() && !indexing)
        {
            //Add attributes to session variables
            RemoteResource *shareRootItem = m_rootResource->sharedRootItem();
            if (shareRootItem) {
                if (m_req.isNull() ) {
                    qCritical() << "Dead req";
                    replyNoContent();
                };

                QString sessionId = m_req->sessionId();
                if (sessionId.isNull() || sessionId.isEmpty()) {
                    qCritical() << "Invalid session:" << m_req->url().path(QUrl::FullyDecoded);
                    replyNoContent();
                };

                bool sessionPopulated = m_sessionAuthProvider->getSessionVariable(sessionId, "populated", false).toBool();
                if (! sessionPopulated) {
                    m_rootResource->updateSharedRoot();
                    m_sessionAuthProvider->setSessionVariables(sessionId, shareRootItem->getCustomAttributes());
                    m_sessionAuthProvider->setSessionVariable(sessionId, "writable", shareRootItem->getLinkData()["writable"].toBool());
                    m_sessionAuthProvider->setSessionVariable(sessionId, "populated", true);
                }
            }
        }        
    }

    //Generate search index for PeARS

    //update shared  root
    if (m_working && indexing && m_rootResource->isSharedRoot())
    {
        m_rootResource->updateSharedRoot();
    }

    //indexing
    if (m_working && indexing && !attrQuery)
    {
        if (requestedResource->isDir() || !CloudResource::isPlain(requestedResource->fileName())) {
            replyRobotIndex(requestedResource);
            return;
        }
    }

    //file JSON listing
    if (m_working && listing)
    {
        QString path = m_requestedItemPathList.join("/");
        bridgeList(path);
        return;
    }


    // Serve manifest.json with template substitution (gateway needs it:
    // injects APPTITLE/SLOGAN). Guard excludes only the embedded P2P connector.
// Serve manifest.json with template substitution. The gateway injects
    // APPTITLE/SLOGAN; the embedded P2P connector does not serve pages.
#if (!defined(P2P_ENABLED) && !defined(NO_WEB_UI)) || defined(SERVER)
    if (m_req->url().path(QUrl::FullyDecoded) == "/manifest.json")
    {
         QString host = QString::fromUtf8(m_req->headers()["host"]);
         QString forwardedHost = QString::fromUtf8(m_req->headers()["x-forwarded-host"]);
         if (!forwardedHost.isEmpty()) host = forwardedHost.section(',', 0, 0).trimmed();
         bool isChat = isChatHost(host);

         // Read and parse: packaged qrc manifest first, then docRoot (SVAR dist)
         QFile manifestFile(":/filemanager/manifest.json");
         if (!manifestFile.open(QIODevice::ReadOnly))
         {
             manifestFile.setFileName(m_Settings->value(QStringLiteral("docRoot"), QStringLiteral("/srv/www")).toString() + "/manifest.json");
             manifestFile.open(QIODevice::ReadOnly);
         }
         if (manifestFile.open(QIODevice::ReadOnly))
         {
             QString manifestSource = QString::fromUtf8(manifestFile.readAll());
             Template manifestTemplate(manifestSource, "manifest");

             QString appTitle = isChat ? TemplateParser::instance().staticVar("CHATS_APPTITLE").toString() : TemplateParser::instance().staticVar("FILES_APPTITLE").toString();
             QString slogan = isChat ? TemplateParser::instance().staticVar("CHAT_SLOGAN").toString() : TemplateParser::instance().staticVar("FILES_SLOGAN").toString();

             manifestTemplate.setVariable("APPTITLE", appTitle);
             manifestTemplate.setVariable("SLOGAN", slogan);

             QByteArray manifestBytes = TemplateParser::instance().parse(manifestTemplate);
             manifestFile.close();

             m_resp->addHeader("Content-Type", "application/json; charset=UTF-8");
             m_resp->addHeader("Cache-Control", "no-cache, no-store, must-revalidate");
             endReply(manifestBytes, qhttp::ESTATUS_OK);
             return;
         }
    }
#endif

    //Show index page for root item (gateway serves the landing page and
    //injects computed variables into shared links; P2P_ENABLED auto-set for
    //server builds via libdatachannel.pri, so exclude only embedded connector)
    #if (!defined(P2P_ENABLED) && !defined(NO_WEB_UI)) || defined(SERVER)
    bool showIndex = isPublicLink ? subUrl.isEmpty() : requestedResource->isDir();
    if ( showIndex )
    {
        //Check for Chat App
        QString host = QString::fromUtf8(m_req->headers()["host"]);
        QString forwardedHost = QString::fromUtf8(m_req->headers()["x-forwarded-host"]);
        if (!forwardedHost.isEmpty()) host = forwardedHost.section(',', 0, 0).trimmed();

        bool isChat = isChatHost(host);

        if (isChat)
        {
             QString sessionId = m_req->sessionId();
             if (!sessionId.isEmpty())
             {
                 m_sessionAuthProvider->setSessionVariable(sessionId, "AppTitle", TemplateParser::instance().staticVar("CHATS_APPTITLE").toString());
                 m_sessionAuthProvider->setSessionVariable(sessionId, "Slogan", TemplateParser::instance().staticVar("CHAT_SLOGAN").toString());
                 m_sessionAuthProvider->setSessionVariable(sessionId, "ChatApp", true);
             }
        } else {
             QString sessionId = m_req->sessionId();
             if (!sessionId.isEmpty())
             {
                 //Reset to default
                 m_sessionAuthProvider->setSessionVariable(sessionId, "AppTitle", TemplateParser::instance().staticVar("FILES_APPTITLE").toString());
                 m_sessionAuthProvider->setSessionVariable(sessionId, "Slogan", TemplateParser::instance().staticVar("FILES_SLOGAN").toString());
                 m_sessionAuthProvider->setSessionVariable(sessionId, "ChatApp", false);
             }
        }

        // Smart Root `/` on Chat hosts (e.g. aiassistant.direct):
        // - `?launch=1` records `omd_entered=1` cookie and serves the web app (`index.html`).
        // - First-time visitors and bots on bare `/` (no `omd_entered=1` cookie, no query params)
        //   receive `about.html` directly for instant landing/SEO without loading the SPA bundle.
        QUrlQuery rootQuery(m_req->url());
        bool isLandingQuery = m_req->url().query().isEmpty()
            || (rootQuery.queryItems().size() == 1 && rootQuery.hasQueryItem(QStringLiteral("lang")));
        if (rootQuery.hasQueryItem(QStringLiteral("launch")))
        {
            m_resp->addHeader("Set-Cookie", "omd_entered=1; Path=/; Max-Age=31536000; SameSite=Lax");
        }
        else if (isChat && !isPublicLink && (urlPath == "/" || urlPath.isEmpty()) && isLandingQuery)
        {
            QHash<QString, QString> cookies = m_req->parseCookies();
            QByteArray rawCookieHeader = m_req->headers().value("cookie");
            bool hasEntered = (cookies.value(QStringLiteral("omd_entered")) == QStringLiteral("1"))
                              || rawCookieHeader.contains("omd_entered=1");
            if (!hasEntered)
            {
                if (serveHtmlTemplatePage(QStringLiteral("about.html"), QStringLiteral("about"), asHead))
                    return;
            }
        }

        //Creating index page

		//m_timeLogger.ReportTime("Index file search start");
		Q_FOREACH (QString indexFile, m_indexFiles)
		{
            QStringList indexPath;
            if (! isPublicLink)
                indexPath.append(m_req->urlPath());
            indexPath.append(indexFile);
            //QStringList indexPath(indexFile);
            CloudResource* indexResource = m_rootResource->getChildClone(indexPath, m_req->url().query(QUrl::FullyDecoded), false);
			if (indexResource == nullptr)
				continue;

            m_resourceCollector.watchFor(indexResource);            

            if (!m_working) {
                releaseHandler();
                return;
            }

			if (indexResource->exists() && indexResource->isFile() && indexResource->isReadable())
			{
#if defined(P2P_ENABLED) || defined(NO_WEB_UI)
                if (indexResource->openResource(QIODevice::ReadOnly) > 0)
                {
                    QByteArray indexData = indexResource->read(0, indexResource->size());
                    indexResource->close();
                    QString indexSource = QString::fromUtf8(indexData);
                    Template indexTemplate(indexSource, "index");

                    indexTemplate.setCondition("isShared", isPublicLink);

                    if (isPublicLink)
                    {
                        QString urlPathStr = m_req->url().path(QUrl::FullyDecoded);
                        QString address;
                        static const QString sharedPrefix = QStringLiteral(SHARED_LINK_PREFIX);
                        if (urlPathStr.startsWith(sharedPrefix))
                            address = urlPathStr.mid(sharedPrefix.length());

                        if (address.contains('/'))
                            address = address.split('/')[0];

                        QString scheme = m_req->headers().contains("x-forwarded-proto")
                                         ? QString::fromUtf8(m_req->headers()["x-forwarded-proto"])
                                         : QStringLiteral("https");
                        QString host = QString::fromUtf8(m_req->headers()["host"]);
                        if (host.isEmpty()) host = QStringLiteral("disk.direct");
                        QString serverUrl = scheme + "://" + host;

                        QString title;
                        QString description;
                        QString ownerName;
                        bool hasCover = false;
                        bool isProtected = false;

                        if (!address.isEmpty())
                        {
                            QString gunHubUrl = TemplateParser::instance().staticVar("GUN_HUB_URL").toString();
                            if (gunHubUrl.isEmpty())
                                gunHubUrl = QStringLiteral("https://disk.direct:8765");

                            QNetworkRequest metaReq(QUrl(gunHubUrl + "/api/share-meta?address=" + QUrl::toPercentEncoding(address)));
                            metaReq.setRawHeader("User-Agent", "OMD-Gateway/2.0");

                            if (!m_externalAccessManager)
                                m_externalAccessManager = new QNetworkAccessManager(this);

                            QEventLoop loop;
                            QNetworkReply *reply = m_externalAccessManager->get(metaReq);
                            QTimer::singleShot(3000, &loop, &QEventLoop::quit);
                            connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
                            loop.exec();

                            if (reply && reply->error() == QNetworkReply::NoError)
                            {
                                QByteArray body = reply->readAll();
                                QJsonDocument doc = QJsonDocument::fromJson(body);
                                if (!doc.isNull() && doc.isObject())
                                {
                                    QJsonObject meta = doc.object();
                                    title = meta["title"].toString();
                                    description = meta["description"].toString();
                                    ownerName = meta["ownerName"].toString();
                                    hasCover = meta["hasCover"].toBool();
                                    isProtected = meta["protected"].toBool();
                                }
                            }
                            if (reply) reply->deleteLater();
                        }

                        QString appTitle = isChat
                            ? TemplateParser::instance().staticVar("CHATS_APPTITLE").toString()
                            : TemplateParser::instance().staticVar("FILES_APPTITLE").toString();
                        if (appTitle.isEmpty()) appTitle = isChat ? QStringLiteral("AI Assistant") : QStringLiteral("Disk Direct");

                        if (title.isEmpty()) title = appTitle;
                        else title = QStringLiteral("%1 - %2").arg(title, appTitle);
                        if (ownerName.isEmpty()) ownerName = QStringLiteral("OMD User");

                        indexTemplate.setVariable("TITLE", title);
                        indexTemplate.setVariable("DESCRIPTION", description);
                        indexTemplate.setVariable("OWNER", ownerName);
                        indexTemplate.setVariable("LINKURL", QString(SHARED_LINK_PREFIX) + address);
                        indexTemplate.setVariable("SERVERURL", serverUrl);
                        indexTemplate.setVariable("COVERVERSION", QString::number(QDateTime::currentSecsSinceEpoch()));
                        indexTemplate.setVariable("APPTITLE", appTitle);

                        if (hasCover && !isProtected) {
                            indexTemplate.setVariable("CARD", "summary_large_image");
                            indexTemplate.setVariable("IMG_WIDTH", "1200");
                            indexTemplate.setVariable("IMG_HEIGHT", "630");
                        } else {
                            indexTemplate.setVariable("CARD", "summary");
                            indexTemplate.setVariable("IMG_WIDTH", "400");
                            indexTemplate.setVariable("IMG_HEIGHT", "400");
                        }
                    } else {
                        QString appTitle = isChat
                            ? TemplateParser::instance().staticVar("CHATS_APPTITLE").toString()
                            : TemplateParser::instance().staticVar("FILES_APPTITLE").toString();
                        if (appTitle.isEmpty()) appTitle = TemplateParser::instance().staticVar("APPTITLE").toString();
                        if (appTitle.isEmpty()) appTitle = QStringLiteral("Disk Direct");
                        indexTemplate.setVariable("TITLE", appTitle);
                        indexTemplate.setVariable("APPTITLE", appTitle);
                    }

                    // The same gateway serves both frontend domains; pick the
                    // main app per request. Shared links always open in files mode.
                    indexTemplate.setVariable("APP_MODE", (isChat && !isPublicLink) ? "chat" : "files");

                    QByteArray parsedBytes = TemplateParser::instance().parse(indexTemplate);
                    m_resp->addHeader("Content-Type", "text/html; charset=UTF-8");
                    m_resp->addHeader("Cache-Control", "max-age=0,no-cache,no-store,must-revalidate");
                    m_resp->addHeader("Pragma", "no-cache");
                    m_resp->addHeader("Expires", "Tue, 01 Jan 1970 00:00:00 GMT");
                    endReply(parsedBytes, qhttp::ESTATUS_OK);
                    return;
                }
#endif
                //disable cache for index
                m_resp->addHeader("Cache-Control", "max-age=0,no-cache,no-store,must-revalidate");
                m_resp->addHeader("Pragma", "no-cache");
                m_resp->addHeader("Expires", "Tue, 01 Jan 1970 00:00:00 GMT");
                handleGetFile(indexResource, asHead );
				return;
			}
		}

		//m_timeLogger.ReportTime("Index file search end");

        //qWarning() << "Missing index page";
		replyForbidden();
		return;
	}
#endif

#if defined(P2P_ENABLED)
    if (isPublicLink && attrQuery)
    {
        QString urlPathStr = m_req->url().path(QUrl::FullyDecoded);
        QString address;
        static const QString sharedPrefix = QStringLiteral(SHARED_LINK_PREFIX);
        if (urlPathStr.startsWith(sharedPrefix))
            address = urlPathStr.mid(sharedPrefix.length());

        if (address.contains('/'))
            address = address.split('/')[0];

        if (!address.isEmpty())
        {
            QString gunHubUrl = m_Settings->value(QStringLiteral("GunHubUrl")).toString();
            if (gunHubUrl.isEmpty())
                gunHubUrl = QStringLiteral("https://disk.direct:8765");

            QNetworkRequest coverReq(QUrl(gunHubUrl + "/api/share-cover?address=" + QUrl::toPercentEncoding(address)));
            coverReq.setRawHeader("User-Agent", "OMD-Gateway/2.0");

            if (!m_externalAccessManager)
                m_externalAccessManager = new QNetworkAccessManager(this);

            QEventLoop loop;
            QNetworkReply *reply = m_externalAccessManager->get(coverReq);
            QTimer::singleShot(3000, &loop, &QEventLoop::quit);
            connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
            loop.exec();

            if (reply && reply->error() == QNetworkReply::NoError)
            {
                QByteArray coverBytes = reply->readAll();
                QByteArray contentType = reply->rawHeader("Content-Type");
                if (contentType.isEmpty()) contentType = "image/jpeg";
                reply->deleteLater();

                if (!coverBytes.isEmpty())
                {
                    m_resp->addHeader("Content-Type", contentType);
                    m_resp->addHeader("Cache-Control", "max-age=86400");
                    m_resp->addHeader("Access-Control-Allow-Origin", "*");
                    endReply(coverBytes, qhttp::ESTATUS_OK);
                    return;
                }
            } else if (reply) {
                reply->deleteLater();
            }
        }

        // Fallback: serve default share icon from docRoot
        CloudResource* imgRes = m_rootResource->getChildClone(QStringList("icon-512.png"), "", false);
        if (imgRes && imgRes->exists() && imgRes->isFile() && imgRes->isReadable())
        {
            m_resp->addHeader("Content-Type", "image/png");
            m_resp->addHeader("Cache-Control", "max-age=86400");
            handleGetFile(imgRes, asHead);
            return;
        }
    }
#endif


	//missing mime icon?
	if (!requestedResource->isDir()  && !requestedResource->exists())
    {
            // looking for missing mimetype icon? get default one
            QStringList path = m_req->urlPath();
            if (path.count() > 0 && path.first() == m_iconDir) {
                path.removeLast();
                path.append("application-blank.png");
				requestedResource = m_rootResource->getChildClone(path, m_req->url().query(QUrl::FullyDecoded));
				if (requestedResource)
                    m_resourceCollector.watchFor(requestedResource);
            }
    }


    if (!m_working) {
        releaseHandler();
        return;
    }

    if (!indexing && !requestedResource->exists() && !isPublicLink && !attrQuery && !requestedResource->fileName().endsWith(".map") && m_requestedItemPathList.count())
    {
        // Not readable — redirect to root instead of logging noise
        QUrl url = m_req->url();
        url.setPath("/");
        url.setFragment("/" + m_requestedItemPathList.join("/"));
        replySeeOther(url);
        return;
    }

    if (requestedResource->isFile())
    {
        if (requestedResource->isReadable())
        {
            handleGetFile(requestedResource, asHead);
        }
        else
        {
            qWarning() << "Ureadable" << m_req->url().toString() << m_req->headers()["authorization"];
            replyForbidden();
        }
        return;
    }
    else
    {
        //creating auto index
        if (m_genIndex)
        {
            if (isNotExpired(requestedResource))
            {
                replyNotModified(requestedResource->ETag(), requestedResource->lastModifiedString());
            }
            else
            {
                replyFakeIndex(requestedResource);
            }
            return;
        }
    }
    replyForbidden();
}

void HttpHandler::handleOptions()
{
    setServerString();
    //Limiting list of supported operation by RO
    //QString allowedMethods = "OPTIONS,GET,HEAD,PUT,POST,MKCOL,PROPFIND,DELETE,PROPPATCH,COPY,MOVE,LOCK,UNLOCK";

    QString allowedMethods = "OPTIONS,GET,HEAD,POST,PUT,DELETE,MKCOL,PROPFIND,PROPPATCH,COPY,MOVE,LOCK,UNLOCK";
    QString allowedHeaders = "Access-Control-Allow-Headers,Origin,Accept,X-Requested-With,X-OMD-Key,Content-Type,Access-Control-Request-Method,Access-Control-Request-Headers,Authorization,If-Modified-Since,Range,Expect,X-OMD-Private-Mode";
    m_resp->addHeader("DAV", "1,2");
    m_resp->addHeader("DAV", "<http://apache.org/dav/propset/fs/1>");
    m_resp->addHeader("MS-Author-Via", "DAV");
    m_resp->addHeader("Content-Length", "0");
    m_resp->addHeader("Access-Control-Allow-Credentials", "true");
    m_resp->addHeader("Access-Control-Expose-Headers", "Content-Length,Content-Range");
    m_resp->addHeader("Access-Control-Allow-Methods", allowedMethods.toUtf8());
    m_resp->addHeader("Access-Control-Allow-Headers", allowedHeaders.toUtf8());
    m_resp->addHeader("Allow", allowedMethods.toUtf8());
    m_resp->setStatusCode(qhttp::ESTATUS_OK);
    m_working = false;
    m_reading = false;
    m_resp->end();
    releaseHandler();
}


void HttpHandler::beginHandlePost()
{

    if (!m_req->headers().contains("content-length"))
    {
        replyBadRequest("No content-length header");
        return;
    }

	if (!m_req->headers().contains("content-length"))
	{
		replyBadRequest("No content-length header");
		return;
	}

    m_bytesToSend = m_req->headers()["content-length"].toULongLong();

    if (m_bytesToSend > PACKET_SIZE)
    {
        replyRequestEntityTooLarge(QString("Request size is too large: %1. Limit is 2097152 bytes (2M)").arg(m_bytesToSend));
        return;
    }

    QString redirectUrl; //not used for POST
    m_requestedItemPathList = m_req->urlPath();
    QString  urlPath = m_req->url().path(QUrl::FullyDecoded);
    m_rpcCall = checkSitePath(urlPath,  redirectUrl); //check if we need to forward post data to site

    if (!m_working || m_req.isNull())
        return;

    if (m_bytesToSend == 0)
	{
		//just auth request
		endHandlePost();
		return;
	}

    if (m_buffer.isOpen()) {
        m_buffer.close();
    }

    m_buffer.setBuffer(&m_bufferBytes);
    if (!m_buffer.open(QFile::ReadWrite))
    {
        replyInternalServerError(QString("Can't open %1 for write").arg(m_req->url().path(QUrl::PrettyDecoded)));
        return;
    }

    connect(m_req.data(), &QHttpRequest::data, this, &HttpHandler::receivePostData);
    QByteArray data = m_req->getCollectedData();


    if ( data.count() > 0)
        receivePostData(data);
}

void HttpHandler::beginHandlePut()
{

    QString error = "";
    m_bytesToSend = m_req->headers()["content-length"].toULongLong();
    m_bytesReceived = 0;
    m_bytesWritten = 0;
    m_totalBytes = m_bytesToSend;

    //qDebug() << "Uploading" <<  m_bytesToSend;

    m_writing = true;
    m_reading = false;

    m_putJsonResponse = m_req->url().query(QUrl::FullyDecoded).contains("jsonResponse") || m_req->headers()["Response"]=="json";


    QStringList childPath;
    if (m_rootResource->isSharedRoot()) {
        QString filePath =QUrl::fromPercentEncoding(m_req->headers()["Dest"]);
        QString destFile = "/" + filePath;
        childPath =  destFile.split("/", Qt::SplitBehaviorFlags::SkipEmptyParts);
        m_userName = m_sessionAuthProvider->getCurrentUser(m_req->sessionId());
    } else {
        childPath = m_req->urlPath();
    }

	//hack for Nextcloud upload
    /*if (m_req->url().path().indexOf("ocTransfer")) {
        qDebug() << m_req->url().path();
		QString partName = childPath.takeLast();
		int ocPartPos = partName.indexOf("ocTransfer");
		QString fixedName = partName.left(ocPartPos - 1);
		childPath.append(fixedName);
		//qDebug() << "Fixed upload name:"  << fixedName;
    }*/

    if (!m_req->headers().contains("content-length"))
    {
        error = tr("No content-length header");
        m_req->stopCollectData();
		replyInternalServerError(error);
		return;
    }

    //qDebug() << "Collected:" << m_putData.size();

    m_reading = false;
    m_writing = true;
    m_Reciever = m_rootResource->touch(childPath);

    if (m_Reciever == nullptr)
    {
        qCritical() << "Can not create" << childPath;
        error = tr("Can't create %1").arg(m_req->url().path(QUrl::PrettyDecoded));
        if (m_putJsonResponse)
            replyJsonError(error);
        else
            replyInternalServerError(error);
		return;
	}

    m_resourceCollector.watchFor(m_Reciever);

    if (!m_working) {
        releaseHandler();
        return;
    }

    if (! m_Reciever->isWritable())
    {
        QString error = tr("Can not write to '%1'").arg(m_Reciever->fileName());
        if (m_putJsonResponse)
            replyJsonError(error);
        else
            replyInternalServerError(error);
        qCritical() << "Can not upload to:" << m_Reciever->path();
        return;
    }

	if (m_Reciever->openResource(QIODevice::WriteOnly) == FILEHANDLE_ERROR)
    {
        qCritical() << "Can not open" << m_Reciever->fileName() << "for write";
        error = tr("Can't open %1 for write").arg(m_Reciever->path());
        if (m_putJsonResponse)
            replyJsonError(error);
        else
            replyInternalServerError(error);
		return;
	}

    // empty file? - just create it and leave
    if (m_bytesToSend == 0) {
        endHandlePut();
        return;
    }

    connect(m_req.data(), &QHttpRequest::data, this, &HttpHandler::receivePutData);
	//2. then get collected data. it will switch receive to recievePutData slot.
	QByteArray data = m_req->getCollectedData();

	if (data.size())
	{
        receivePutData(data);
    }

    if (m_bytesToSend > 0 &&  m_working && m_req->headers().contains("Expect"))
    {
        m_resp->send100Continue();
    }

    //startOpTimer();
}

void HttpHandler::startOpTimer()
{
    m_delayedOpTimer = new QTimer();
    m_delayedOpTimer->setSingleShot(true);
    connect(m_delayedOpTimer, &QTimer::timeout, [this]() {
        qDebug() << "Request timeout. Bytes to receive:" << m_bytesToSend << "of" << m_totalBytes << "received:" << m_bytesReceived;
        if (!m_req.isNull()) {
            m_req->stopCollectData();
            qDebug() << "Request:"  << m_req->methodString() << m_req->url().toString();
        }
        if (!m_resp.isNull()) {
            if (m_putJsonResponse)
                replyJsonError(tr("Request timeout"));
            else
                replyInternalServerError(tr("Request timeout"));
        }
    });
    m_delayedOpTimer->start(REQUEST_TIMEOUT);
}


void HttpHandler::receivePutData(const QByteArray &data)
{

    if (!m_working) {
        releaseHandler();
        return;
    }

	m_putData.append(data);
	// get available block data size
    m_bytesReceived += data.size();

    //qDebug() << "Received " << m_bytesReceived << " bytes of" << m_totalBytes;

    //check if we have data in buffer
    if (m_putData.size() >= (qint64)m_maxBufferSize || m_bytesReceived >= m_totalBytes)  //wait for more data to arrive
	{
        writePutData();
	}

}

void HttpHandler::writePutData()
{
    m_writing = true;

    qint64 blockSize = m_putData.size() >= (qint64) m_maxBufferSize  ?  m_maxBufferSize : m_putData.size();
    QByteArray writePacket = m_putData.left(blockSize);
    m_putData.remove(0, blockSize);
    m_putData.squeeze();

    m_bytesToSend -= writePacket.size();

    // write to target file
    qint64 written = m_Reciever->write(writePacket);

    m_writing = false;

    if (!m_working) {
        releaseHandler();
        return;
    }

    if (written < 1)
    {
        qCritical() << "Error writing remote file" << m_Reciever->fileName();
        if (!m_resp.isNull()) {
            setServerString();
            if (m_putJsonResponse)
                replyJsonInternalServerError("Error writing remote file");
             else
                replyInternalServerError("Error writing remote file");
        } else
            releaseHandler();
        return;
    }

    if (m_putJsonResponse)
        emit UserEvents::instance().bytesUploaded(m_userName, writePacket.size());

    m_bytesWritten += writePacket.size();
    //qDebug() << "Written:" << m_bytesWritten << "of" << m_totalBytes << ", left" << m_bytesToSend;

    //Check if compleated received?
    if (m_bytesWritten >= m_totalBytes)
    {
        endHandlePut();
    } else if (m_bytesReceived >= m_totalBytes && m_bytesToSend >= 0) { // all received
        // qDebug() << "Sending  blocks";
         QMetaObject::invokeMethod(this, "writePutData", Qt::QueuedConnection);
    }

}

void HttpHandler::requestReady(bool reinit)
{
   //qDebug() << "Request ready";
   m_requestReady = true;
   m_reinitConnection = reinit;
}


void HttpHandler::stop()
{
    m_aborted.store(true, std::memory_order_release);
    m_stopped.store(true, std::memory_order_release);
    m_working = false;
    releaseHandler();
}

void HttpHandler::abort()
{
    m_aborted.store(true, std::memory_order_release);
    m_stopped.store(true, std::memory_order_release);
    m_working = false;
}

QString HttpHandler::reqId() const {
    return   QString("%1 | %2 | %3 | %4").arg(m_reqId, QString::number(m_streaming), QString::number(m_rpcCall), QString::number(m_working));
}

void HttpHandler::requestCompleted()
{
    m_working = false;
    if (QThread::currentThread() != this->thread()) {
        // Called from another thread
        QMetaObject::invokeMethod(this, "requestCompleted",  Qt::QueuedConnection);
    } else {
        if (!m_delayedRelease && !m_released)
        {
            releaseHandler();
        }
    }
}

void HttpHandler::releaseHandler()
{
    if (m_released)
        return;

    m_released = true;
    m_streaming = false;

    if (m_rpcReader) {
        m_rpcReader->close();
    }

    if (!m_stopped && m_reader && m_reader->isOpen())
    {
        m_reader->close();
    }

    if (!m_stopped && m_Reciever && m_Reciever->isOpen())
    {
        m_Reciever->close();
    }

    m_req = nullptr;
    m_resp = nullptr;
    m_working = false;

    emit completed();
}



#ifdef SERVER
bool HttpHandler::signCSR(QByteArray csrBody, QByteArray &signedCertBody)
{
    return m_pkiHelper->signCSR(csrBody, m_reqNumber, m_userName, signedCertBody);
}
#endif


int HttpHandler::returnCode() const
{
    return 0;
}

void HttpHandler::handleDelete()
{
    m_reading = true;
    QString  urlPath = m_req->url().path(QUrl::FullyDecoded);
    QString redirectUrl;
    m_requestedItemPathList = m_req->urlPath();
    m_rpcCall = checkSitePath(urlPath,  redirectUrl);

    if (!m_working || m_req.isNull())
        return;

    // Local AI proxy bypass (Embedded client - offline mode)
    if (m_localAiProxy) {
        handleLocalAiProxy(QByteArray());
        return;
    }

    CloudResource* resourceToDelete = m_rootResource->getChildClone(m_requestedItemPathList, m_req->url().query(QUrl::FullyDecoded));

    if (resourceToDelete == nullptr)
    {
        replyNotFound();
        return;
    }

    if (! m_working) {
        releaseHandler();
        return;
    }

    // RPC DELETE invokation - calling local apps over OMD proxy
    if (m_rpcCall) {
            QByteArray payload; //empty
            QHash<QString, QVariant> params;

            //add headers
            foreach(const QByteArray& key, m_req->headers().keys()) {
                params[QString::fromUtf8(key)] = QString::fromUtf8(m_req->headers()[key]);
            }
            params["action"] = "delete";
            m_reading = true;

            m_rpcReader = new  FileReader(resourceToDelete, this);
            connect(m_rpcReader, &FileReader::responseReady, this, [this ] (const QHash<QString, QVariant> &params, const QByteArray& payload) {
                setServerString();
                m_reading = false;

                if (!m_working || m_resp.isNull()) {
                    releaseHandler();
                    return;
                }

                int statusCode = params.contains("statuscode") ? params["statuscode"].toInt() : 0;

                if (payload.size() == 0 && statusCode != 204 && statusCode != 304 &&  statusCode != 200) {
                    qWarning() << "Empty response:" << statusCode << m_rpcCallPath;
                }

                if (statusCode == 0) {
                    replyJsonError("RPC error");
                    return;
                }

                QString contentType = params.contains("Content-Type") ?  params["Content-Type"].toString() : params.contains("content-type") ?  params["content-type"].toString() : "";

                if (params.contains("Location"))
                    m_resp->addHeader("Location", params["Location"].toString().toUtf8());

                //Populate headers
                foreach(const QString &key, params.keys()) {
                    QString lower = key.toLower();
                    if (lower != "statuscode" &&
                        lower != "content-length" &&
                        lower != "stream" &&
                        lower != "server" && lower != "access-control-allow-origin")
                            m_resp->addHeader(key.toUtf8(), params[key].toString().toUtf8());
                }

                qInfo() << m_rpcCallPath << contentType << statusCode ;

                if (statusCode == 304)
                    replyNotModified();
                else if (statusCode == 204)
                    replyNoContent();
                else
                    endReply(payload, contentType, static_cast<qhttp::TStatusCode>(statusCode));

            });

            if (!m_rpcReader->invoke(m_rpcCallPath, params, payload)) {
                replyNotFound();
            }
            return;
    }

    if (resourceToDelete->isFile() && resourceToDelete->url(m_req->url()).path() != m_req->url().path())
    {
		qCritical() << "DELETE: moved" <<  m_req->url().query(QUrl::FullyDecoded);
		replyMoved(resourceToDelete->url(m_req->url()));
        return;
    }

    if (resourceToDelete->getResourceType() == collection && m_Depth != infinity)
    {
		qCritical() << "DELETE: Removing collections should be done with Depth=infinity" <<  m_req->url().query(QUrl::FullyDecoded);
		replyBadRequest(QString("Removing collections should be done with Depth=infinity"));
        return;
    }

    if (! resourceToDelete->remove(true))
    {
        replyBadRequest(tr("Error while removing '%1'").arg(resourceToDelete->path()));
        qCritical() << "Remove failed:" << resourceToDelete->path();
        return ;
    }


    replyNoContent();
}

void HttpHandler::replyNotImplemented()
{
    QString method = m_req->methodString();
    method = method.replace("HTTP_","");
    //Q_ASSERT_X(false, "handleNotImplemented", QString("method not implemented %1").arg(method).toStdString().c_str());
    QString answerString = "501 Not Implemented";
    QByteArray body = answerString.toUtf8();
    endReply(body, qhttp::ESTATUS_NOT_IMPLEMENTED);
}

void HttpHandler::replyNotFound()
{
    QString answerString = "404 Not found";
    QByteArray body = answerString.toUtf8();
    endReply(body, qhttp::ESTATUS_NOT_FOUND);
}

void HttpHandler::replyNotFoundPage()
{
    QByteArray loginPageBytes;
#if (!defined(P2P_ENABLED) && !defined(NO_WEB_UI)) || defined(SERVER)
    QFile pageFile(":/filemanager/offline.html");
    if (pageFile.open(QIODevice::ReadOnly))
    {
        QString loginSource = QString::fromUtf8(pageFile.readAll());
        Template loginTemplate(loginSource, "login");
        loginPageBytes = TemplateParser::instance().parse("", loginTemplate);
        pageFile.close();
    } else
#endif
#if (defined(P2P_ENABLED) || defined(NO_WEB_UI)) && !defined(SERVER)
    loginPageBytes = "404 Not found";
    (void)loginPageBytes;
#endif
#ifdef SERVER
    // SVAR build has no packaged offline.html (legacy filemanager qrc);
    // fall back to the docRoot copy served with the template parser.
    if (loginPageBytes.isEmpty())
    {
       QFile pageFile(m_Settings->value(QStringLiteral("docRoot"), QStringLiteral("/srv/www")).toString() + "/offline.html");
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
    setServerString();
    m_resp->addHeader("Content-Type", "text/html; charset=UTF-8");
    m_resp->addHeader("Content-Length", QString::number(loginPageBytes.size()).toUtf8());
    m_resp->setStatusCode( qhttp::ESTATUS_NOT_FOUND);
    m_working = false;
    m_reading = false;
    m_writing = false;
    m_resp->end(loginPageBytes);
    releaseHandler();
}


void HttpHandler::replyNoContent()
{
    if (m_working && !m_resp.isNull()) {
        setServerString();
        m_resp->addHeader("Content-Length", "0");
        m_resp->setStatusCode(qhttp::ESTATUS_NO_CONTENT);
        m_working = false;
        m_reading = false;
        m_writing = false;
        m_resp->end();
    } else
        releaseHandler();
}

void HttpHandler::replyMethodNotAllowed()
{
    QString answerString = "405 Method no allowed";
    QByteArray body = answerString.toUtf8();

    endReply(body, qhttp::ESTATUS_METHOD_NOT_ALLOWED);
}

void HttpHandler::replyConflict(QString s_details)
{
    QString answerString = "409 Conflict";
    if (s_details.length() > 0)
    {
        answerString += "\r\n";
        answerString += s_details;
    }
    QByteArray body = answerString.toUtf8();

    endReply(body, qhttp::ESTATUS_CONFLICT);
}

void HttpHandler::replyForbidden()
{
    QString answerString = "403 Forbidden";
    QByteArray body = answerString.toUtf8();

    endReply(body, qhttp::ESTATUS_FORBIDDEN);
}

void HttpHandler::replyCreated()
{
    QString answerString = "201 Created";
    QByteArray body = answerString.toUtf8();

    //qDebug() << "Created" << m_req->url().toString();

    endReply(body, qhttp::ESTATUS_CREATED);
}

void HttpHandler::replyFakeIndex(CloudResource* requestedResource)
{    
    //requestedResource->RefreshInfo();
    m_resp->addHeader("ETag", requestedResource->ETag().toUtf8());
    m_resp->addHeader("Last-Modified", requestedResource->lastModifiedString().toUtf8());

    QString answerString = "INDEX of " + m_req->url().path(QUrl::PrettyDecoded) + "<pre>";

    m_reading = true;

    QStringList localListing = requestedResource->entryList();

    if (! m_working) {
        releaseHandler();
        return;
    } //check if already dropped

    Q_FOREACH(QString childName,  localListing)
    {
            CloudResource *childResource = requestedResource->getChild(childName);
            if (childResource == nullptr)
                continue;

            if (! m_working) {
                releaseHandler();
                return;
            }

            // skip non-readable system items
            if (! childResource->isReadable())
                continue;

            QString url = childResource->url(m_req->url()).toString(QUrl::PrettyDecoded);
            if (childResource->isDir())
                answerString += QString("<a href=\"%1\">%2/</a>").arg(url).arg(childName) + "\r\n";
            else
                answerString += QString("<a href=\"%1\">%2</a>").arg(url).arg(childName) + "\r\n";
    }

    answerString += "</pre>";
    QByteArray body = answerString.toUtf8();

    endHtmlReply(body, qhttp::ESTATUS_OK);
}

// Support of 'robots.txt' in folders
void HttpHandler::bridgeExcludeFromIndexing(QJsonObject request)
{
    QString path = request["item"].toString();
    bool excludeFromIndexing = request["excludeFromIndexing"].toBool();
    QString query = handleAddToQuery(m_req->url().query(QUrl::FullyDecoded), request["addToQuery"].toString());

    //qDebug() << "The item:" << path << "The item:" << excludeFromIndexing;
    if (excludeFromIndexing) {
        qDebug() << "We will perform same as we have for readme file.";
    }

    m_reading = true;

    CloudResource* item = m_rootResource->getChildClone(path.split("/", Qt::SplitBehaviorFlags::SkipEmptyParts), query);
    if (item == nullptr)
    {
        replyJsonNotFound();
        return;
    }

    if ( !item->exists())
    {
        replyJsonNotFound();
        return;
    }

    //Create result object
    QJsonObject response;
    QJsonObject result;

    item->refresh();
    QHash<QString, QVariant> attr = item->getCustomAttributes();

    result["excludeFromIndexing"] = QVariant(attr.value("excludeFromIndexing")).toString();
    result["success"] = true;
    response["result"] = result;

    //Write response
    QJsonDocument responseData(response);
    endJsonReply(responseData.toJson(QJsonDocument::Compact), qhttp::ESTATUS_OK);
}

void HttpHandler::bridgeCreateNew(QJsonObject request)
{
    QString docType = request["docType"].toString();
    QString path = request["itemPath"].toString();
    QString templatePath = "templates/template." + docType;
    QString query = "";

    m_reading = true;

    CloudResource* templateDoc = m_rootResource->getChildClone(templatePath.split("/", Qt::SplitBehaviorFlags::SkipEmptyParts), query);
    CloudResource* item = m_rootResource->getChildClone(path.split("/", Qt::SplitBehaviorFlags::SkipEmptyParts), query);

    if (templateDoc == nullptr)
    {
        replyJsonNotFound();
        return;
    }

    if ( !templateDoc->exists())
    {
        replyJsonNotFound();
        return;
    }
    QHash<QString, QString> failedList;

    templateDoc->copyTo(item, &failedList);

    //Create result object
    QJsonObject response;
    QJsonObject result;

    result["createNewDocument"] = true;
    result["success"] = true;
    response["result"] = result;

    //Write response
    QJsonDocument responseData(response);
    endJsonReply(responseData.toJson(QJsonDocument::Compact), qhttp::ESTATUS_OK);
}

void HttpHandler::bridgeSubscribe(QJsonObject request)
{

    QString user = request["user"].toString();
    QString site = request["site"].toString();
    //Create result object
    QJsonObject response;
    QJsonObject result;

    result["success"] = true;
    response["result"] = result;

    //Event
    emit UserEvents::instance().subscribe(m_userName, user, site);

    //Write response
    QJsonDocument responseData(response);
    endJsonReply(responseData.toJson(QJsonDocument::Compact), qhttp::ESTATUS_OK);
}


void HttpHandler::bridgeSubscriptionSuccess(QJsonObject request)
{
    QString user = request["user"].toString();
    QJsonObject data = request["data"].toObject();

    //Create result object
    QJsonObject response;
    QJsonObject result;

    result["success"] = true;
    response["result"] = result;

    //Event
    emit UserEvents::instance().userEvent(user, "subscription", data);

    //Write response
    QJsonDocument responseData(response);
    endJsonReply(responseData.toJson(QJsonDocument::Compact), qhttp::ESTATUS_OK);
}


void HttpHandler::bridgeUnsubscribe(QJsonObject request)
{
    QString user = request["user"].toString();
    QString site = request["site"].toString();
    //Create result object
    QJsonObject response;
    QJsonObject result;

    result["success"] = true;
    response["result"] = result;

    //Event
    emit UserEvents::instance().unsubscribe(m_userName, user, site);

    //Write response
    QJsonDocument responseData(response);
    endJsonReply(responseData.toJson(QJsonDocument::Compact), qhttp::ESTATUS_OK);
}


bool HttpHandler::saveFile(const QStringList& path, const QByteArray& data)
{
    CloudResource *item = m_rootResource->touch(path);
    if (item == nullptr)
    {
        return false;
    }

    m_resourceCollector.watchFor(item);

    if (! m_working) {
        return false;
    }

    if (item->openResource(QIODevice::WriteOnly) == FILEHANDLE_ERROR)
    {
        return false;
    }

    item->write(data);
    item->close();
    return true;
}

void HttpHandler::bridgeCreateSite(QJsonObject request)
{
    m_reading = true;

    // Check Rights
        double tokenBalance = m_sessionAuthProvider->getSessionVariable(m_req->sessionId(), "token_balance", 0.0).toDouble();
        if (tokenBalance < 1.0)
        {
             replyJsonForbidden();
             return;
        }

    QJsonObject siteInfo = request["siteInfo"].toObject();
    QString siteName = siteInfo["name"].toString();
    QString sitesPath = request["path"].toString();
    QString path = sitesPath + "/" + siteName;
    //Count sites
    CloudResource* sitesItem = m_rootResource->getChildClone(sitesPath.split("/", Qt::SplitBehaviorFlags::SkipEmptyParts), "", true);
    if (sitesItem == nullptr ) {
        replyJsonInternalServerError("No sites directory: " + sitesPath);
        return;
    }
    m_resourceCollector.watchFor(sitesItem);
    if (!sitesItem->exists())
    {
        replyJsonInternalServerError("No sites directory: " + sitesItem->fileName());
        return;
    }

    //Count site folders
    int c = 0;
    QStringList entries = sitesItem->entryList();
    foreach(const QString& entry, entries)
        if (sitesItem->getChild(entry)->isDir())
            c++;

    //Create site folder first
    CloudResource* item = m_rootResource->touch(path.split("/", Qt::SplitBehaviorFlags::SkipEmptyParts));
    if (item == nullptr)
    {
        replyJsonInternalServerError("Can not create directory: " + path);
        return;
    }
    m_resourceCollector.watchFor(item);

    if (! m_working) {
        releaseHandler();
        return;
    }

    if (item->exists())
    {
        replyJsonInternalServerError("Directory already exists: " + item->fileName());
        return;
    }

    if (!item->mkcol())
    {
        replyJsonInternalServerError("Can not create directory " + item->fileName());
        return;
    }

    //Create readme
    QString title = siteInfo["title"].toString();
    QString descr = siteInfo["description"].toString();

    QString readMe = QString("# %1\n%2").arg(title, descr);
    QString readmePath = path + "/Readme.md";

    //Save readme
    if (! saveFile(readmePath.split("/", Qt::SplitBehaviorFlags::SkipEmptyParts), readMe.toUtf8()))
    {
        replyJsonInternalServerError("Could not save Readme.md");
        return;
    }

    if (siteInfo["type"].toString() == "jekyll") {

        //copy site template
        CloudResource* siteTemplate = m_rootResource->getChildClone(QString("templates/jekyll").split("/"), "", true);
        QHash<QString, QString> errs;
        m_resourceCollector.watchFor(siteTemplate);
        siteTemplate->copyTo(item, &errs);

        //Create post
        QString dateStr = QDateTime::currentDateTime().toString("yyyy-MM-dd");
        QString postsFolderPath = QString(path + "/_posts");
        CloudResource* postsFolder = m_rootResource->touch(postsFolderPath.split("/", Qt::SplitBehaviorFlags::SkipEmptyParts));
        m_resourceCollector.watchFor(postsFolder);
        postsFolder->mkcol();
        QString postPath = QString(postsFolderPath + "/%1-%2").arg(dateStr, "welcome-to-jekyll.markdown");
        CloudResource* postTemplate = m_rootResource->getChildClone(QString("templates/post.markdown").split("/"));
        m_resourceCollector.watchFor(postTemplate);
        CloudResource* postDest = m_rootResource->touch(postPath.split("/", Qt::SplitBehaviorFlags::SkipEmptyParts));
        m_resourceCollector.watchFor(postDest);
        postTemplate->copyTo(postDest, &errs);

        //create config
        int port = 4000 + c;
        QString theme = siteInfo["template"].toString();
        QString configFile = path + "/_config.yml";
        QString formattedDescr = "";
        QStringList descrList = descr.split("\n", Qt::SplitBehaviorFlags::SkipEmptyParts);
        foreach(const QString& descrLine,  descrList)
            formattedDescr += "\n   " + descrLine.trimmed();
        QString config = QString("# Generated by Disk Direct Site Wizard\n"
                "title: \"%1\"\n"
                "email: your-email@example.com\n"
                "description: >- # multi-line value"
                "%2\n"
                "url: \"%3\"\n"
                "baseurl: \"/sites/%4\" # the subpath of your site\n"
                "twitter_username: jekyllrb\n"
                "github_username:  jekyll\n"
                "\n"
                "# Build settings\n"
                "port: %5\n"
                "theme: \"%6\"\n"
                "plugins:\n"
                "  - jekyll-feed\n"
                "  - jekyll-seo-tag\n").arg(title, formattedDescr, m_sessionAuthProvider->getStaticVariable("SERVERURL").toString(), siteName, QString::number(port), theme);
        saveFile(configFile.split("/", Qt::SplitBehaviorFlags::SkipEmptyParts), config.toUtf8());

        QString siteSettings = QString("[settings]\n"
                "baseurl=http://localhost:%1/sites/%2\n"
                "type=jekyll\n").arg(QString::number(port), siteName);
        QString settingsFile = path + "/.conf";
        saveFile(settingsFile.split("/", Qt::SplitBehaviorFlags::SkipEmptyParts), siteSettings.toUtf8());
    }

    //TODO: for create index.js for NodeJS sites
    if (siteInfo["type"].toString() == "nodejs") {
        int port = 8000 + c;
        QString siteSettings = QString("[settings]\n"
                "baseurl=http://localhost:%1/sites/%2\n"
                "type=nodejs").arg(QString::number(port), siteName);
        QString settingsFile = path + "/.conf";
        saveFile(settingsFile.split("/", Qt::SplitBehaviorFlags::SkipEmptyParts), siteSettings.toUtf8());
    }

    SiteInfo site;
    site.linkID = item->storageID();
    site.owner = item->owner(m_rootOwner);
    site.shareName = path.split("/", Qt::SplitBehaviorFlags::SkipEmptyParts).first();
    site.name = path.split("/", Qt::SplitBehaviorFlags::SkipEmptyParts).last();
    site.type = siteInfo["type"].toString();
    site.siteTemplate = siteInfo["template"].toString();
    emit UserEvents::instance().siteCreated(site.owner,  site);

    //Create result object
    QJsonObject response;
    QJsonObject result;

    result["success"] = true;
    response["result"] = result;

    //Write response
    QJsonDocument responseData(response);
    endJsonReply(responseData.toJson(QJsonDocument::Compact), qhttp::ESTATUS_OK);
    reportTokenUsage("workspace", 1.0);
}


void HttpHandler::replyRobotIndex(CloudResource* requestedResource)
{

    if (! requestedResource->exists()) {
        replyNotFound();
        return;
    }

    m_resp->addHeader("ETag", requestedResource->ETag().toUtf8());
    m_resp->addHeader("Last-Modified", requestedResource->lastModifiedString().toUtf8());
    QStringList pathList= m_req->urlPath();

    QString responseXml = "<omd_index>";

    QStringList localListing;
    if (m_req->urlPath().size() == 0) { // root path '/' - return list of users (users having access to manual indexing from admin concole only -)
        // Get list of devices, shares and sites
        localListing = m_rootResource->entryList();
    } else { // all nested paths should be already covered by requestedResource tree structure
        // Get listing first
        localListing = requestedResource->entryList();
    }

    // add folder itself
    QHash<QString, QVariant> resourceAttrs;
    QString lastChanged;
    if (m_rootResource->isSharedRoot()) {
        resourceAttrs = m_rootResource->sharedRootItem()->getCustomAttributes();
        lastChanged = m_rootResource->sharedRootItem()->lastModified().toString("yyyy-MM-dd hh:mm:ss");
    } else {
        resourceAttrs = requestedResource->getCustomAttributes();
        lastChanged = requestedResource->lastModified().toString("yyyy-MM-dd hh:mm:ss");
    }
    if (lastChanged.isEmpty())
        lastChanged = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss");

    QString realFileName = m_rootResource->isSharedRoot() ? m_rootResource->sharedRootName() :  requestedResource->fileName();
    QString defaultTitle = realFileName;

    bool hasReadme = resourceAttrs["longdescr"].toBool() || resourceAttrs["customdescr"].toBool();
    QString title = resourceAttrs["customtitle"].toBool() ? resourceAttrs["title"].toString() : defaultTitle;
    QString descr = hasReadme ?  resourceAttrs["description"].toString() : title;
    QString plainDescr = descr.isEmpty() ? "" : stripHtml(descr);
    QString plainTitle = stripHtml(title).left(150);
    QString owner = resourceAttrs.contains("owner") ? resourceAttrs["owner"].toString() : m_userName;
    QString sharedTo = "";
    bool isSite = resourceAttrs.contains("isSite") && resourceAttrs["isSite"].toBool();
    bool folderIsRef = resourceAttrs.contains("isRef") ? resourceAttrs["isRef"].toBool() : false;
    if (isSite) {
        QString descr = hasReadme ?  resourceAttrs["description"].toString() : "";
        QString title = resourceAttrs["customtitle"].toBool() ? resourceAttrs["title"].toString() : realFileName;
        QString plainDescr = descr.isEmpty() ? "" : stripHtml(descr);
        QString plainTitle = stripHtml(title).left(150);
        QString siteUrl = resourceAttrs.contains("domain") ? QString("https://%1").arg(resourceAttrs["domain"].toString()) : QString("/sites/%1/").arg(realFileName);
        responseXml +=  QString("<doc url='%1' contentType='text/html' owner='%2' last_modified='%3'><title>%4</title>").arg(siteUrl, owner, lastChanged, plainTitle);
        if (!plainDescr.isEmpty())
            responseXml += QString("<description>%1</description>").arg(plainDescr);
        responseXml += "</doc>";
    } else  if (requestedResource->isDir() ) {
        if (! folderIsRef) {
            QString url = resourceAttrs["longdescr"].toBool() ? "?readme" :  "?description";
            if (!sharedTo.isEmpty())
                responseXml +=  QString("<doc url='%1' contentType='text/plain' owner='%2' shared_with='%3' last_modified='%4'><title>%5</title>").arg(url, owner, sharedTo, lastChanged, plainTitle);
            else
                responseXml +=  QString("<doc url='%1' contentType='text/plain' owner='%2' last_modified='%3'><title>%4</title>").arg(url, owner, lastChanged, plainTitle);
            if (!plainDescr.isEmpty())
                responseXml += QString("<description>%1</description>").arg(plainDescr);
            responseXml += "</doc>";
        }
    } else if (CloudResource::isConvertible(realFileName)) {
        QString url = "?totext";
        responseXml +=  QString("<doc url='%1' contentType='text/plain' owner='%2' convertible='True' last_modified='%3'><title>%4</title>").arg(url, owner, lastChanged, plainTitle);
        if (!plainDescr.isEmpty())
            responseXml += QString("<description>%1</description>").arg(plainDescr);
        responseXml += "</doc>";
    } else {
        QString url = "?direct";
        responseXml +=  QString("<doc url='%1' contentType='%2' owner='%3'  last_modified='%4'><title>%5</title>").arg(url, requestedResource->getContentType(), owner, lastChanged, plainTitle);
        if (!plainDescr.isEmpty())
            responseXml += QString("<description>%1</description>").arg(plainDescr);
        responseXml += "</doc>";
    }


    if (! m_working) {
        releaseHandler();
        return;
    } //check if already dropped

    if ( !m_rootResource->isSharedRoot()) {
        Q_FOREACH(QString entryMame, localListing)
        {
            CloudResource *childResource = requestedResource->getChild(entryMame);
            QString childName = childResource->fileName();
            if (CloudResource::isSkipped(childName)) //skip md and cover files from indexing, they are used for parent's descriptions
                continue;
            if (CloudResource::isUnlisted(childName)) //skip md and cover files from indexing, they are used for parent's descriptions
                continue;

            //qDebug() << "Check localListing: " << localListing.at(i);

            if (childResource == nullptr)
                continue;

            if (! m_working) {
                releaseHandler();
                return;
            }
            // skip non-readable system items
            if (! childResource->isReadable())
                continue;

            // Get resource properties
            QHash<QString, QVariant> resourceAttrs = childResource->getCustomAttributes();
            QString lastChanged = childResource->lastModified().toString("yyyy-MM-dd hh:mm:ss");
            if (lastChanged.isEmpty())
                lastChanged = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss");
            bool isRef = resourceAttrs.contains("isRef") && resourceAttrs["isRef"].toBool();
            bool isSite = resourceAttrs.contains("isSite") && resourceAttrs["isSite"].toBool();
            bool hasReadme = resourceAttrs["longdescr"].toBool() || resourceAttrs["customdescr"].toBool();

            //Skip manually excluded
            if (resourceAttrs.contains("excludeFromIndexing") && resourceAttrs["excludeFromIndexing"].toBool()) continue;

            QString owner = resourceAttrs.contains("owner") ? resourceAttrs["owner"].toString() : m_userName;

            //qInfo() << "indexing: " << itemUrl;

            if (isSite) {
                QString descr = hasReadme ?  resourceAttrs["description"].toString() : "";
                QString title = resourceAttrs["customtitle"].toBool() ? resourceAttrs["title"].toString() : childResource->fileName();
                QString plainDescr = descr.isEmpty() ? "" : stripHtml(descr);
                QString plainTitle = stripHtml(title).left(150);
                QString siteUrl = resourceAttrs.contains("domain") ? QString("https://%1").arg(resourceAttrs["domain"].toString()) : QString("/sites/%1/").arg(childName);
                responseXml +=  QString("<doc url='%1' contentType='text/html' owner='%2' last_modified='%3'><title>%4</title>").arg(siteUrl, owner, lastChanged, plainTitle);
                if (!plainDescr.isEmpty())
                    responseXml += QString("<description>%1</description>").arg(plainDescr);
                responseXml += "</doc>";

            } else if (childResource->isDir())
            {

                QString descr = hasReadme ?  resourceAttrs["description"].toString() : "";
                QString title = resourceAttrs["customtitle"].toBool() ? resourceAttrs["title"].toString() : childResource->fileName();
                QString plainDescr = descr.isEmpty() ? "" : stripHtml(descr);
                QString plainTitle = stripHtml(title).left(150);

                if (!isRef)
                {
                    if (!sharedTo.isEmpty())
                        responseXml +=  QString("<doc url='%1/' contentType='folder' owner='%2' shared_with='%3' last_modified='%4'><title>%5</title>").arg(childName, owner, sharedTo, lastChanged, plainTitle);
                    else
                        responseXml +=  QString("<doc url='%1/' contentType='folder' owner='%2' last_modified='%3'><title>%4</title>").arg(childName, owner, lastChanged, plainTitle);
                    if (!plainDescr.isEmpty())
                        responseXml += QString("<description>%1</description>").arg(plainDescr);
                    responseXml += "</doc>";
                }

                if (!childResource->getPublicLink().isEmpty()) {
                    if (!sharedTo.isEmpty())
                        responseXml +=  QString("<doc url='%1' contentType='folder' owner='%2' shared_with='%3' last_modified='%4'><title>%5</title>").arg(childResource->getPublicLink(), owner, sharedTo, lastChanged, plainTitle);
                    else
                        responseXml +=  QString("<doc url='%1' contentType='folder' owner='%2' last_modified='%3'><title>%4</title>").arg(childResource->getPublicLink(), owner, lastChanged, plainTitle);
                    if (!plainDescr.isEmpty())
                        responseXml += QString("<description>%1</description>").arg(plainDescr);
                    responseXml += "</doc>";
                }
            }  else {
                    //responseString += QString("<a href=\"%1\">%1</a>").arg(localListing[i]) + "\r\n";
                QString convertible = CloudResource::isConvertible(childResource->fileName()) ? "True" : "False";
                QString title = resourceAttrs["customtitle"].toBool() ? resourceAttrs["title"].toString() : childResource->fileName();
                QString plainTitle = stripHtml(title).left(150);
                QString descr = hasReadme ?  resourceAttrs["description"].toString() : "";
                QString plainDescr = descr.isEmpty() ? "" : stripHtml(descr);
                if (! isRef )
                {
                    //Skip un-annotaited photos and videos
                    if (!hasReadme && ( childResource->fileName().startsWith("VID_") || childResource->fileName().startsWith("IMG_"))) continue;

                    if (!sharedTo.isEmpty())
                        responseXml +=  QString("<doc url='%1' contentType='%2' convertible='%3' owner='%4' shared_with='%5' last_modified='%6'><title>%7</title>").arg(childName,  childResource->getContentType(),  convertible, owner, sharedTo, lastChanged, plainTitle);
                    else
                        responseXml +=  QString("<doc url='%1' contentType='%2' convertible='%3' owner='%4' last_modified='%5'><title>%6</title>").arg(childName,  childResource->getContentType(),  convertible, owner, lastChanged, plainTitle);

                    if (resourceAttrs["longdescr"].toBool() || resourceAttrs["customdescr"].toBool())
                        responseXml += QString("<description>%1</description>").arg(plainDescr);

                    if (resourceAttrs.contains("album"))
                        responseXml += QString("<artist>%1</artist><album>%2</album>").arg(resourceAttrs["artist"].toString()).arg(resourceAttrs["album"].toString());
                    responseXml += "</doc>";
                }

                if (!childResource->getPublicLink().isEmpty())
                {
                    responseXml += QString("<doc url='%1' contentType='%2' convertible='%3' owner='%4' last_modified='%5'><title>%6</title>").arg(childResource->getPublicLink(), childResource->getContentType(), convertible, owner, lastChanged, plainTitle);
                    if (resourceAttrs["longdescr"].toBool() || resourceAttrs["customdescr"].toBool())
                        responseXml += QString("<description>%1</description>").arg(plainDescr);

                    if (resourceAttrs.keys().contains("album"))
                        responseXml += QString("<artist>%1</artist><album>%2</album>").arg(resourceAttrs["artist"].toString()).arg(resourceAttrs["album"].toString());
                    responseXml += "</doc>";
                }
            }
        }
    }
    //responseString += "</pre>";
    responseXml +="</omd_index>";
    // qDebug() << "Checking responseString: " << responseString;
    //qDebug() << "Checking responseXml: " << responseXml;
    
    // QByteArray body = responseString.toUtf8();
    QByteArray body = responseXml.toUtf8();

    endXmlReply(body, qhttp::ESTATUS_OK);
}


void HttpHandler::replyNotModified(QString ETag, QString LastModified)
{
    if (m_working && !m_resp.isNull()) {
        if (!ETag.isEmpty())
            m_resp->addHeader("ETag", ETag.toUtf8());
        if (!LastModified.isEmpty())
            m_resp->addHeader("Last-Modified", LastModified.toUtf8());
        setServerString();
        m_resp->addHeader("Content-Length", "0");
        m_resp->setStatusCode(qhttp::ESTATUS_NOT_MODIFIED);
        m_working = false;
        m_reading = false;
        m_writing = false;
        m_resp->end();
        releaseHandler();
    } else
        releaseHandler();
}

void HttpHandler::replyInternalServerError(QString s_details)
{
    QString answerString = "Gateway error :(";
    QByteArray body = answerString.toUtf8();
    if (s_details.length() > 0)
    {
        answerString += "\r\n";
        answerString += s_details;
    }

    endReply(body, qhttp::ESTATUS_INTERNAL_SERVER_ERROR);
}

void HttpHandler::replyBadRequest(QString s_details)
{
    QString answerString = "400 Bad Request";
    if (s_details.length() > 0)
    {
        answerString += "\r\n";
        answerString += s_details;
    }
    QByteArray body = answerString.toUtf8();

    endReply(body, qhttp::ESTATUS_BAD_REQUEST);
}

void HttpHandler::replyRequestEntityTooLarge(QString s_details)
{
    QString answerString = "413 Request Entity Too Large";
    if (s_details.length() > 0)
    {
        answerString += "\r\n";
        answerString += s_details;
    }
    QByteArray body = answerString.toUtf8();

    endReply(body, qhttp::ESTATUS_REQUEST_ENTITY_TOO_LARGE);
}

void HttpHandler::replyUnsupportedMediaType(QString s_details)
{
    QString answerString = "415 Unsupported Media Type";
    if (s_details.length() > 0)
    {
        answerString += "\r\n";
        answerString += s_details;
    }
    QByteArray body = answerString.toUtf8();

    endReply(body, qhttp::ESTATUS_REQUEST_UNSUPPORTED_MEDIA_TYPE);
}

void HttpHandler::endReply(QByteArray body, const QString &contentType, qhttp::TStatusCode code)
{
    if (m_working && !m_resp.isNull()) {
        QString path = m_req->urlPath().isEmpty() ? "" : m_req->urlPath().at(0);
        bool skipCleanup = path == "ai" || path == "code";

        if (!skipCleanup)
            body.append("\n");

        if (!contentType.isEmpty())
            m_resp->addHeader("Content-Type", contentType.toUtf8());

        if (!body.isEmpty() && (!m_resp->headers().has("Content-Length") || !skipCleanup))
            m_resp->addHeader("Content-Length", QString::number(body.size()).toUtf8());

        setServerString();
        m_working = false;
        m_reading = false;
        m_writing = false;
        m_resp->setStatusCode(code);
        m_resp->end(body);
        releaseHandler();
    } else
        releaseHandler();
}

void HttpHandler::writeReply(QByteArray body,  qhttp::TStatusCode code)
{
    if (m_working && !m_resp.isNull()) {
        setServerString();
        m_resp->setStatusCode(code);
        m_resp->write(body);
        m_resp->flush();
    } else
        releaseHandler();
}


void HttpHandler::endReply(QByteArray body, qhttp::TStatusCode code)
{
    endReply(body, "text/plain; charset=UTF-8", code);
}


void HttpHandler::endHtmlReply(QByteArray body, qhttp::TStatusCode code)
{
    endReply(body, "text/html; charset=UTF-8", code);
}

void HttpHandler::endXmlReply(QByteArray body, qhttp::TStatusCode code)
{
    endReply(body, "text/xml; charset=UTF-8", code);
}


void HttpHandler::replyMoved(QUrl s_newUrl)
{
    QString answerString = "301 Moved Permanently";
    answerString += "\r\n";
    answerString += QString("The document has been moved <a href=\"%1\">here</a>.").arg(s_newUrl.toString(QUrl::PrettyDecoded));
    QByteArray body = answerString.toUtf8();
    m_resp->addHeader("Location", s_newUrl.toString(QUrl::FullyEncoded).toUtf8());
    endReply(body, qhttp::ESTATUS_MOVED_PERMANENTLY);
}

void HttpHandler::replyFound(QUrl s_newUrl)
{
    QString answerString = "302 Found";
    answerString += "\r\n";
    answerString += QString("The document can be found <a href=\"%1\">here</a>.").arg(s_newUrl.toString(QUrl::PrettyDecoded));
    QByteArray body = answerString.toUtf8();
    m_resp->addHeader("Location", s_newUrl.toString(QUrl::FullyEncoded).toUtf8());

    endReply(body, qhttp::ESTATUS_FOUND);
}

void HttpHandler::replySeeOther(QUrl s_newUrl)
{
    QString answerString = "303 See Other";
    answerString += "\r\n";
    answerString += QString("The document has been moved <a href=\"%1\">here</a>.").arg(s_newUrl.toString(QUrl::PrettyDecoded));
    QByteArray body = answerString.toUtf8();
    
    // Check if we are behind an SSL proxy
    if (m_req->headers().contains("X-Forwarded-Proto") && m_req->headers()["X-Forwarded-Proto"] == "https") {
         s_newUrl.setScheme("https");
    }

    m_resp->addHeader("Location", s_newUrl.toString(QUrl::FullyEncoded).toUtf8());
    endReply(body, qhttp::ESTATUS_SEE_OTHER);
}

void HttpHandler::replyBadGateway(QString s_details)
{
    QString answerString = "502 Bad Gateway";
    if (s_details.length() > 0)
    {
        answerString += "\r\n";
        answerString += s_details;
    }
    QByteArray body = answerString.toUtf8();

    endReply(body, qhttp::ESTATUS_BAD_GATEWAY);
}

void HttpHandler::replyServiceUnavailable(QString s_details)
{
    QString answerString = "503 Service Unavailable";
    if (s_details.length() > 0)
    {
        answerString += "\r\n";
        answerString += s_details;
    }
    QByteArray body = answerString.toUtf8();

    endReply(body, qhttp::ESTATUS_SERVICE_UNAVAILABLE);
}

void HttpHandler::replyPreconditionFailed(QString s_details)
{
    QString answerString = "412 Precondition Failed";
    if (s_details.length() > 0)
    {
        answerString += "\r\n";
        answerString += s_details;
    }
    QByteArray body = answerString.toUtf8();

    endReply(body, qhttp::ESTATUS_PRECONDITION_FAILED);
}

void HttpHandler::setServerString()
{
    if (m_headerSent || m_resp.isNull())
        return;

    m_headerSent = true;
    m_resp->addHeader("Server", m_ServerString.toUtf8());
    // Robust CORS addition to ensure exactly one '*' header
    if (!m_resp->headers().has("Access-Control-Allow-Origin") && !m_resp->headers().has("access-control-allow-origin")) {
         m_resp->addHeader("Access-Control-Allow-Origin", "*");
    }
}

void HttpHandler::saveSessionCookie(const QString& sessionID)
{
    QByteArray cookieBody;
    QString domain = m_req->url().host();
    cookieBody.append(QString("%1=%2").arg(m_sessionAuthProvider->globalCookieID(), sessionID).toUtf8());
    cookieBody.append("; Path=/");
    cookieBody.append("; Domain=");
    cookieBody.append(domain.toUtf8());
    cookieBody.append("; SameSite=Lax");
    m_resp->addHeader("Set-Cookie", cookieBody);
}


void HttpHandler::endSession(const QString &session_id)
{
    QString userId = m_req.isNull() ? m_userName : m_sessionAuthProvider->getCurrentUser(session_id);
    m_sessionAuthProvider->removeSession(session_id);

    QString sessionID = !session_id.isEmpty() ?  session_id : m_req->sessionId();
    if (! sessionID.isEmpty() && !m_resp.isNull()) {
        m_resp->addHeader("Set-Cookie", QString("%1=deleted; path=/; expires=Thu, 01 Jan 1970 00:00:00 GMT").arg(m_sessionAuthProvider->globalCookieID()).toUtf8());
        emit UserEvents::instance().sessionEnded(userId, sessionID);
    }
}


void HttpHandler::sendFile(CloudResource* cloudResource, bool asHead)
{    
    m_resp->addHeader("Accept-Ranges", "bytes");
    setServerString();
    m_resp->addHeader("ETag", cloudResource->ETag().toUtf8());
    m_resp->addHeader("Last-Modified", cloudResource->lastModifiedString().toUtf8());

    if (m_req->url().query(QUrl::FullyDecoded).contains("direct"))
    {
        QString fileName = cloudResource->fileName();
        m_resp->addHeader("Content-Disposition", "attachment; filename=\"" + fileName.toUtf8() + "\"");
    }

    cloudResource->setSessionId(m_req->sessionId());

    m_reader = new FileReader(cloudResource,  m_req->headers()["range"], m_req->url().query(QUrl::FullyDecoded).contains("saveAs"), false, this);
	//qDebug() << "Range:" << m_req->headers()["range"];
    //qDebug() << "SaveAs:" << m_req->url().query(QUrl::FullyDecoded).contains("saveAs");

    if (m_reader && m_reader->open())
    {
        if (!m_working) {
            m_reading = false;
            releaseHandler();
            return;
        }

        // http connection closed during opening file
        if (m_resp.isNull()) {
            m_reading = false;
            m_working = false;
            releaseHandler();
            return;
        }

        // set correct headers
        m_resp->setStatusCode(m_reader->getStatusCode());
        if (!m_converting)
            m_resp->addHeaders(m_reader->getHeaders());

        m_bytesToSend = m_reader->size();

        if (asHead || m_reader->getRequestGetType() == NotSatisfiableRangeGet)
        {
            m_working = false;
            m_reading = false;
            m_resp->end();
            releaseHandler();
            return;
        }

        if (m_bytesToSend <= 0) {
            m_working = false;
            m_resp->end();
            releaseHandler();
            return;
        }

        m_totalBytes = m_bytesToSend;
        //qInfo() << "Reading " << cloudResource->fileName() << cloudResource->size() << m_reader->size() <<  m_req->headers()["range"];

        connect(m_reader, &FileReader::dataReady, this, &HttpHandler::requestedDataReady);

        m_requestPackets = PACKETS_AHEAD;
        m_reading = false;
        requestData();
    }
    else
    {
        qDebug() << "Can not open" << m_req->url().toString();
        replyForbidden();
    }

}

bool HttpHandler::isNotExpired(CloudResource* cloudResource)
{
    if (!m_working)
        return false;

    if (cloudResource)
    {
        if (m_req->headers().contains("if-none-match"))
        {
            return (m_req->headers()["if-none-match"] == cloudResource->ETag());
        }
        if (m_req->headers().contains("if-modified-since"))
        {
            QString rawDateTime = m_req->headers()["if-modified-since"];
            QDateTime LastModifiedHeader = QLocale::c().toDateTime(rawDateTime.replace(" GMT", ""), "ddd, dd MMM yyyy hh:mm:ss");
			if (LastModifiedHeader.isValid()) {
				return true;
			}
            LastModifiedHeader.setTimeSpec(Qt::UTC);
            QDateTime LastModifiedFile = cloudResource->lastModified();
			if (LastModifiedFile.secsTo(LastModifiedHeader) >= 0) {
				return true;
			}
        }
    }
    return false;
}

void HttpHandler::bridgeProvisionGroup(QJsonObject request)
{
    //Groups are provisioned on the legacy gateway during the GunDB migration
    QJsonObject upstream;
    if (callUpstream(request, upstream))
    {
        QJsonDocument responseData(upstream);
        endJsonReply(responseData.toJson(QJsonDocument::Compact), qhttp::ESTATUS_OK);
        return;
    }
    replyJsonInternalServerError(tr("Upstream gateway error."));
}

void HttpHandler::reportTokenUsage(QString action, double units)
{
    if (m_userName.isEmpty())
        return;

    QString consoleUrl = m_Settings->value("ConsoleUrl", "https://console.onmydisk.net").toString();
    QString url = consoleUrl + "/api/v1/tokens/report-usage";
    QString secret = m_sessionAuthProvider->crawlerToken();

    QNetworkAccessManager *mgr = new QNetworkAccessManager(this);
    connect(mgr, &QNetworkAccessManager::finished, mgr, &QNetworkAccessManager::deleteLater);

    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    req.setRawHeader("X-OMD-Secret", secret.toUtf8());

    QJsonObject json;
    json["omd_core_id"] = m_userName;
    json["action_type"] = action;
    json["units"] = (double) units;

    mgr->post(req, QJsonDocument(json).toJson());
    qInfo() << "Reported usage for" << m_userName << ":" << action << "x" << units;
}

void HttpHandler::bridgeRelogin(QJsonObject request)
{
    QString token = request["token"].toString();
    if (token.isEmpty()) {
        replyJsonBadRequest("Missing token");
        return;
    }

    // Reuse bridgeSignIn by faking a device login request
    QJsonObject signInRequest;
    signInRequest["action"] = "signin";
    signInRequest["device_key"] = token;
    signInRequest["remember"] = true;

    bridgeSignIn(signInRequest);
}

void HttpHandler::bridgeSessionLogin(QJsonObject request)
{
    Q_UNUSED(request);
    QJsonObject result;

    // Read session ID from cookie
    QHash<QString, QString> cookies = m_req->parseCookies();
    QString cookieName = m_sessionAuthProvider->globalCookieID();
    QString sessionID = cookies.value(cookieName);

    if (sessionID.isEmpty()) {
        result["valid"] = false;
        QJsonDocument responseData(result);
        endJsonReply(responseData.toJson(QJsonDocument::Compact), qhttp::ESTATUS_UNAUTHORIZED);
        return;
    }

    // Look up existing session
    QPointer<ClientSession> session = m_sessionAuthProvider->getSession(sessionID);
    if (session.isNull()) {
        result["valid"] = false;
        QJsonDocument responseData(result);
        endJsonReply(responseData.toJson(QJsonDocument::Compact), qhttp::ESTATUS_UNAUTHORIZED);
        return;
    }

    QHash<QString, QVariant> sessionVars = session->getSessionVariables();
    QString username = sessionVars["User"].toString();
    QString pkey = sessionVars["token"].toString();

    if (username.isEmpty() || pkey.isEmpty()) {
        result["valid"] = false;
        QJsonDocument responseData(result);
        endJsonReply(responseData.toJson(QJsonDocument::Compact), qhttp::ESTATUS_UNAUTHORIZED);
        return;
    }

    // Renew session lifetime
    m_sessionAuthProvider->addSessionFor(username, 86400000, sessionID);
    saveSessionCookie(sessionID);

    // Build profile
    QJsonObject profile;
    if (m_isServerNode) {
        //User profiles live on the legacy gateway during the GunDB migration
        profile = m_sessionAuthProvider->getUserProfile(username);
    } else {
        profile["user"] = username;
        profile["displayname"] = sessionVars["displayname"].toString().isEmpty() ? username : sessionVars["displayname"].toString();
        profile["email"] = sessionVars["email"].toString();
        profile["lang"] = "en";
        profile["valid"] = true;
        profile["isGuest"] = sessionVars["isGuest"].toBool();
    }

    updateProfileWithRights(profile, username, pkey, session.data());
    profile["omd_key"] = pkey;

    result["profile"] = profile;
    result["session_id"] = sessionID;
    result["token"] = pkey;
    result["valid"] = true;

    if (m_resp.isNull()) {
        qCritical() << "Response is null";
        m_working = false;
        m_reading = false;
        releaseHandler();
        return;
    }

    qInfo() << "Session login:" << username << "session:" << sessionID;
    QJsonDocument responseData(result);
    endJsonReply(responseData.toJson(QJsonDocument::Compact), qhttp::ESTATUS_OK);
}

bool HttpHandler::isStreamingPath(const QString& path)
{
    return path.endsWith("/stream") || path.contains("/v1/") || path.contains("/api/") || path.contains("/event") || path.contains("/code/");
}

bool HttpHandler::serveHtmlTemplatePage(const QString& fileName, const QString& templateName, bool asHead)
{
#if (!defined(P2P_ENABLED) && !defined(NO_WEB_UI)) || defined(SERVER)
    QByteArray pageData;
    if (m_rootResource) {
        QStringList path;
        path << fileName;
        CloudResource* res = m_rootResource->getChildClone(path);
        if (res) {
            m_resourceCollector.watchFor(res);
            if (res->exists() && res->isReadable() && res->openResource(QIODevice::ReadOnly) > 0) {
                pageData = res->read(0, res->size());
                res->close();
            }
        }
    }
    if (pageData.isEmpty() && m_Settings) {
        QFile file(m_Settings->value(QStringLiteral("docRoot"), QStringLiteral("/srv/www")).toString() + "/" + fileName);
        if (file.open(QIODevice::ReadOnly)) {
            pageData = file.readAll();
            file.close();
        }
    }
    if (pageData.isEmpty())
        return false;

    QString pageSource = QString::fromUtf8(pageData);
    Template pageTemplate(pageSource, templateName);

    QString host = QString::fromUtf8(m_req->headers()["host"]);
    QString forwardedHost = QString::fromUtf8(m_req->headers()["x-forwarded-host"]);
    if (!forwardedHost.isEmpty()) host = forwardedHost.section(',', 0, 0).trimmed();
    bool isChat = isChatHost(host);

    QString appTitle = isChat ? TemplateParser::instance().staticVar("CHATS_APPTITLE").toString()
                              : TemplateParser::instance().staticVar("FILES_APPTITLE").toString();
    if (appTitle.isEmpty()) appTitle = TemplateParser::instance().staticVar("APPTITLE").toString();
    if (appTitle.isEmpty()) appTitle = QStringLiteral("Disk Direct");

    QString slogan = isChat ? TemplateParser::instance().staticVar("CHAT_SLOGAN").toString()
                            : TemplateParser::instance().staticVar("FILES_SLOGAN").toString();
    if (slogan.isEmpty()) slogan = TemplateParser::instance().staticVar("SLOGAN").toString();
    if (slogan.isEmpty()) slogan = QStringLiteral("Delivering content right from your disk");

    QString company = TemplateParser::instance().staticVar("COMPANY").toString();
    if (company.isEmpty()) company = QStringLiteral("Direct community");

    QString companyUrl = TemplateParser::instance().staticVar("COMPANYURL").toString();
    if (companyUrl.isEmpty()) companyUrl = QStringLiteral("https://github.com/alexeyvolkoff/onmydisk");

    pageTemplate.setVariable("TITLE", appTitle);
    pageTemplate.setVariable("APPTITLE", appTitle);
    pageTemplate.setVariable("SLOGAN", slogan);
    pageTemplate.setVariable("COMPANY", company);
    pageTemplate.setVariable("COMPANYURL", companyUrl);

    QByteArray parsedBytes = TemplateParser::instance().parse(pageTemplate);
    m_resp->addHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    m_resp->addHeader("Pragma", "no-cache");
    m_resp->addHeader("Expires", "Tue, 01 Jan 1970 00:00:00 GMT");
    if (asHead) {
        m_resp->addHeader("Content-Length", QString::number(parsedBytes.size()).toUtf8());
        endHtmlReply(QByteArray(), qhttp::ESTATUS_OK);
    } else {
        endHtmlReply(parsedBytes, qhttp::ESTATUS_OK);
    }
    return true;
#else
    Q_UNUSED(fileName);
    Q_UNUSED(templateName);
    Q_UNUSED(asHead);
    return false;
#endif
}
