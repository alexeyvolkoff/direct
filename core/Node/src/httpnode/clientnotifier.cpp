#include "clientnotifier.h"
#include "version.h"
#include <QTimer>
#include "notificationmanager.h"
#include "wsbackend.h"
#include "userevents.h"
#include "httpserverbackend.h"
#include "onmydisklinkserver.h"

#define CLEINT_CLEANUP_INTERVAL 6000


ClientNotifier::ClientNotifier(WebSocketTransport *client, SessionAuth *sessionAuth, QObject *parent) : QObject(parent),
    m_sessionAuth(sessionAuth), m_isAdmin(false), m_isGroupAdmin(false),
    m_clientTransport(client)
{
    m_user = "";
    m_token = "";
    m_group = "";
    m_deviceCount = 0;
    m_disconnected = false;
    m_isGuest = false;
    m_version = APP_VERSION;
    qRegisterMetaType<QJsonObject>("QJsonObject");

    //register self in javascript as "server"
    m_webChannel.registerObject(QStringLiteral("backend"), this);
    m_webChannel.connectTo(client);
    connect(m_clientTransport, &WebSocketTransport::disconnected, this,  &ClientNotifier::socketDisconnected);
    connect(&UserEvents::instance(), &UserEvents::userEvent, this, &ClientNotifier::onUserEvent);
}

void ClientNotifier::pushWebSignal(const QJsonObject &signalData)
{
    if (m_disconnected)
        return;
    qDebug() << "[ClientNotifier" << this << "] pushWebSignal emit webTunnelOpen user:" << m_user << "linkID:" << m_linkID << "tunnel:" << signalData.value("tunnelID").toString() << "kind:" << signalData.value("kind").toString();
    emit webTunnelOpen(signalData);
}

bool ClientNotifier::sendSignaling(const QString &linkID, const QString &tunnelID, const QString &kind, const QString &payloadB64, quint16 port, const QString &service)
{
#ifdef SERVER
    QByteArray payload = QByteArray::fromBase64(payloadB64.toUtf8());
    QString requester = m_user.isEmpty() ? m_linkID : m_user;
    if (requester.isEmpty()) requester = m_token;
    return OnMyDiskLinkServer::instance().sendWebSignaling(linkID, tunnelID, kind, payload, requester, port, service, m_user);
#else
    Q_UNUSED(linkID)
    Q_UNUSED(tunnelID)
    Q_UNUSED(kind)
    Q_UNUSED(payloadB64)
    Q_UNUSED(port)
    Q_UNUSED(service)
    return false;
#endif
}

void ClientNotifier::setLinkId(const QString &linkID)
{
    m_linkID = linkID;
#ifdef SERVER
    if (!m_linkID.isEmpty())
    {
        OnMyDiskLinkServer::instance().registerWebDevice(m_linkID, m_user.isEmpty() ? m_linkID : m_user);
    }
#endif
    if (!m_linkID.isEmpty()) {
        qInfo() << "WebSocket link connected:" << m_linkID << "user:" << m_user;
        emit linkConnected(linkID);
    }
}


void ClientNotifier::setToken(const QString &token)
{
    if (doSubscribe(token)) {
        if (! m_disconnected && ! m_user.isEmpty())
            emit userConnected(m_user);
    } else
        emit sessionExpired(); //reload fronend
}

void ClientNotifier::subscribe(QJsonObject subscription)
{
    QString subscriptionToken = QString(QCryptographicHash::hash(QJsonDocument(subscription).toJson(), QCryptographicHash::Md5).toHex());
    emit pushNotificationsSubscribed(m_user, subscriptionToken, subscription);
}

bool ClientNotifier::subscribeChannel(const QString &token, const QString &linkID)
{
    if (!linkID.isEmpty())
        setLinkId(linkID);
    if (!token.isEmpty())
        setToken(token);
    qInfo() << "[ClientNotifier] subscribeChannel completed for user:" << m_user << "linkID:" << m_linkID;
    return true;
}

int ClientNotifier::getDeviceCount()
{
   return m_deviceCount;
}

void ClientNotifier::setDeviceCount(int count)
{
   m_deviceCount = count;
   if (! m_disconnected && !m_isGuest)
        emit deviceCountChanged(m_deviceCount);
}


QString ClientNotifier::getBackendType()
{
    if (m_isGuest)
     return "public";
    else
     return "server";
}


bool ClientNotifier::doSubscribe(const QString &token)
{
    //qDebug() << "ClientNotifier::doSubscribe token:" << token << "version:" << APP_VERSION;

    if (!m_sessionAuth) {
        qCritical() << "ClientNotifier::doSubscribe: m_sessionAuth is null";
        return false;
    }

    QPointer<ClientSession> session = m_sessionAuth->getSession(token);
    
    if (session.isNull()) {
         qWarning() << "ClientNotifier::doSubscribe: Session not found for token:" << token;
         return false; //restore session
    }

    // Double check session validity
    if (!session) {
          qCritical() << "ClientNotifier::doSubscribe: Session pointer is invalid";
          return false;
    }

    // Safe assignment with checks
    m_token = token;
    
    // Copy values to locals first to debug potential string issues
    m_isGuest = session->isGuest();
    m_user = session->userID();
    QHash<QString, QVariant> sessionVars = session->getSessionVariables();
    
    m_isAdmin = sessionVars.value("isAdmin", false).toBool();
    m_isGroupAdmin = sessionVars.value("isGroupAdmin", false).toBool();
    m_group = sessionVars.value("group", "").toString();
    m_owner = sessionVars.value("Owner", "").toString();
    
    // Safety check for m_user string operation
    bool isUserGuest = (m_user == "guest");
    m_isGuest = m_isGuest || isUserGuest;

    //qInfo() << "ClientNotifier::doSubscribe: Subscribed user:" << m_user << "Guest:" << m_isGuest;
    return true;
}

void ClientNotifier::socketDisconnected()
{
   if (m_disconnected) return;
   m_disconnected = true;

#ifdef SERVER
   // Channel gone - unregister the web device ONLY if no other open browser tab
   // still holds a live WebSocket channel for the same linkID.
   if (!m_linkID.isEmpty() && !NotificationManager::instance().hasActiveNotifierForLink(m_linkID, this))
       OnMyDiskLinkServer::instance().unregisterWebDevice(m_linkID);
#endif

   m_webChannel.deregisterObject(this);
   m_webChannel.disconnectFrom(m_clientTransport);

   if (!m_user.isEmpty())
        emit userDisconnected(m_user);

   // if (!m_linkID.isEmpty())
   //    emit NotificationManager::instance().linkDisconnected(m_linkID, user);

   this->disconnect();
 }



ClientNotifier::~ClientNotifier()
{
   delete m_clientTransport;
}

QJsonObject ClientNotifier::getUserProfile(const QString &user)
{
    QJsonObject profile;
    if (m_isAdmin )
        profile = m_sessionAuth->getUserProfile(user);
    else if ( m_isGroupAdmin && ! m_group.isEmpty())
        profile = m_sessionAuth->getUserProfile(user, m_group);
    return profile;
}

bool ClientNotifier::updateUserProfile(const QString &user, QJsonObject profile)
{
    //User profiles live on the legacy gateway during the GunDB migration
    Q_UNUSED(user)
    Q_UNUSED(profile)
    return false;
}

bool ClientNotifier::addUserProfile(const QString &user, QJsonObject profile)
{
    //User profiles live on the legacy gateway during the GunDB migration
    Q_UNUSED(user)
    Q_UNUSED(profile)
    return false;
}

bool ClientNotifier::addGroup(const QJsonObject group)
{
    //Groups live on the legacy gateway during the GunDB migration
    Q_UNUSED(group)
    return false;
}

bool ClientNotifier::updateGroup(const QString& group, QJsonObject groupData)
{
    //Groups live on the legacy gateway during the GunDB migration
    Q_UNUSED(group)
    Q_UNUSED(groupData)
    return false;
}

bool ClientNotifier::removeGroup(const QString& group)
{
    //Groups live on the legacy gateway during the GunDB migration
    Q_UNUSED(group)
    return false;
}

bool ClientNotifier::removeUserProfile(const QString &user)
{
    //User profiles live on the legacy gateway during the GunDB migration
    Q_UNUSED(user)
    return false;
}

bool ClientNotifier::removeDevice(const QString &deviceId)
{
    //Device keys are not tracked by the gateway anymore: identities live in GunDB
    Q_UNUSED(deviceId)
    return false;
}

void ClientNotifier::expire()
{
    emit sessionExpired();
}


QJsonArray ClientNotifier::getUsers()
{
   //User lists live on the legacy gateway during the GunDB migration
   return QJsonArray();
}

QJsonArray ClientNotifier::getDevices(const QJsonArray& filterLinkIds)
{
    //Device registry lives in GunDB; the gateway only reports live connections
    QJsonArray devices;
    QJsonArray liveConnected = OnMyDiskLinkServer::instance().getConnectedDevices(m_user, filterLinkIds);
    for (const QJsonValue &val : liveConnected) {
        devices.append(val);
    }
    // qDebug() << "[ClientNotifier] getDevices returning total:" << devices.count() << "devices:" << devices;
    return devices;
}

QJsonArray ClientNotifier::getGroups()
{
   //Groups live on the legacy gateway during the GunDB migration
   return QJsonArray();
}



void ClientNotifier::updateShareInfo(const QString &token, QJsonObject shareData, QJsonArray entryData, QJsonArray links)
{
    if (m_disconnected) {
        return;
    }

    if (m_token != token) {
        //qDebug() << "ClientNotifier::updateShareInfo: switching token from" << m_token << "to" << token;
        if (doSubscribe(token)) {
            if (! m_disconnected ) {
                QString userId = m_isGuest ? token : m_user;
                emit userConnected(userId);
            }
        } else {
            qWarning() << "ClientNotifier::updateShareInfo: failed to subscribe with token" << token;
            emit sessionExpired(); //reload fronend
            return;
        }
    }

    if (shareData.isEmpty()) {
         return;
    }

    QHash<QString, QVariant> shareInfo = WebSocketBackend::jsonToData(shareData);
    QHash<QString, QHash<QString, QVariant>> shareEntryInfo = WebSocketBackend::jsonToEntryData("name", entryData);
    QHash<QString, QHash<QString, QVariant>> shareLinks = WebSocketBackend::jsonToEntryData("name", links);

    QString shareName = shareInfo.value("name").toString();

    if (shareName.isEmpty()) {
         return;
    }

    QString linkID = shareInfo.value("storageID").toString();

    if (!linkID.isEmpty() && m_linkID != linkID) {
        m_linkID = linkID;
        qInfo() << "WebSocket link connected:" << m_linkID << "user:" << m_user;
        emit linkConnected(m_linkID);
        emit NotificationManager::instance().linkConnected(m_linkID,  m_user);
    }

    m_shareName = shareName;
    emit NotificationManager::instance().shareUpdated(m_linkID, m_user, m_shareName, shareInfo, shareEntryInfo, shareLinks);
}

void ClientNotifier::notifyPublicLinkAdded(const QString &itemPath, const QString &link, const QHash<QString, QVariant> &linkData)
{
    if (m_disconnected) return;
    qInfo() << "Public Link created for" << itemPath << link;
    qInfo() << "  notifier m_user:" << m_user << "m_token:" << m_token << "m_isGuest:" << m_isGuest;
    emit UserEvents::instance().publicLinkRequested(m_linkID, itemPath, m_user, linkData);
}


//Javascript response
void ClientNotifier::linkResponse(const QJsonObject responseData)
{
    if (m_disconnected) return;
    emit responseReady(responseData);
}

QJsonObject ClientNotifier::fileApi(const QJsonObject cmd)
{
    if (m_disconnected) return QJsonObject();
    QJsonObject rsp;
    QEventLoop loop;
    QString cmdId = cmd.contains("ID") ? cmd["ID"].toString() : "";
    QPointer<ClientNotifier> guard(this);
    QObject context;

    connect(this, &ClientNotifier::userDisconnected, &loop, &QEventLoop::quit);
    connect(this, &ClientNotifier::destroyed, &loop, &QEventLoop::quit);
    connect(this, &ClientNotifier::responseReady, &context, [&loop, &rsp, cmdId, &guard] (const QJsonObject& response) {
        if (guard.isNull()) return;
        QString rspId = response.contains("ID") ? response["ID"].toString() : "";
        if (cmdId == rspId) { //stop waiting when got response for our command
            rsp = response;
            loop.quit();
        }
    });

    emit linkCommand(cmd); //call JavaScript
    loop.exec(); //wait for response

    if (guard.isNull()) return QJsonObject();
    return rsp;
}

void ClientNotifier::onUserEvent(QString user, QString event, QJsonObject data)
{
    if (m_disconnected) return;
    if (m_user == user) {
        emit userEvent(event, data);
    }
}
