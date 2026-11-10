#include <QNetworkReply>
#include "notificationmanager.h"
#include "userevents.h"
#include "onmydisklinkserver.h"

NotificationManager::NotificationManager()
    : QObject(nullptr)
{

}

NotificationManager &NotificationManager::instance()
{
    static NotificationManager manager;
    return manager;
}

void NotificationManager::init(SessionAuth *sessionAuthProvider, const QString &pushServer)
{
    m_pushServerEnabled = !pushServer.isEmpty();
    m_pushServer = pushServer;
    m_sessionProvider = sessionAuthProvider;
    qRegisterMetaType<QHash<QString, QVariant>>("QVariantHash");
    qRegisterMetaType<QHash<QString, QHash<QString, QVariant>>>("QHash<QString, QHash<QString, QVariant>>");

    connect(m_sessionProvider, &SessionAuth::sessionExpired, this, &NotificationManager::sessionEnded);
    connect(&UserEvents::instance(), &UserEvents::bytesUploaded, this, &NotificationManager::bytesUploaded);
    connect(&UserEvents::instance(), &UserEvents::fileUpdated, this, &NotificationManager::fileUpdated);
    connect(&UserEvents::instance(), &UserEvents::fileRemoved, this, &NotificationManager::fileRemoved);
    connect(&UserEvents::instance(), &UserEvents::userEvent, this, &NotificationManager::userEvent);

    // Broadcast link connect / disconnect events to all active WebSocket clients so
    // browser PWAs immediately see remote nodes come online/offline without refresh.
    connect(&OnMyDiskLinkServer::instance(), &OnMyDiskLinkServer::linkConnected, this, [this](const QString &linkID, const QString &user) {
        QMutexLocker lock(&m_mutex);
        if (linkID.isEmpty() && user.isEmpty()) return;
        qInfo() << "[NotificationManager] linkConnected:" << linkID << "user:" << user << "broadcasting to" << m_connectedClients.count() << "clients";
        for (const auto &n : m_connectedClients) {
            if (n && !n->isDisconnected()) {
                QMetaObject::invokeMethod(n.data(), [n, linkID, user]() {
                    if (!linkID.isEmpty()) emit n->linkConnected(linkID);
                    if (!user.isEmpty() && user != linkID) emit n->linkConnected(user);
                }, Qt::QueuedConnection);
            }
        }
    });

    connect(&OnMyDiskLinkServer::instance(), &OnMyDiskLinkServer::linkDisconnected, this, [this](const QString &linkID, const QString &user) {
        QMutexLocker lock(&m_mutex);
        if (linkID.isEmpty() && user.isEmpty()) return;
        if (!linkID.isEmpty()) m_linkNotifiers.remove(linkID);
        if (!user.isEmpty()) m_linkNotifiers.remove(user);
        qInfo() << "[NotificationManager] linkDisconnected:" << linkID << "user:" << user << "broadcasting to" << m_connectedClients.count() << "clients";
        for (const auto &n : m_connectedClients) {
            if (n && !n->isDisconnected()) {
                QMetaObject::invokeMethod(n.data(), [n, linkID, user]() {
                    if (!linkID.isEmpty()) emit n->linkDisconnected(linkID);
                    if (!user.isEmpty() && user != linkID) emit n->linkDisconnected(user);
                }, Qt::QueuedConnection);
            }
        }
    });

    // Install direct WebPushHandler so incoming WebRTC signaling (answer, candidates) from C++ nodes
    // is immediately delivered to the browser's live WebSocket connection with zero HTTP latency.
    OnMyDiskLinkServer::instance().setWebPushHandler([](const QString &targetID, const QString &tunnelID, const QString &kind, const QByteArray &payload) -> bool {
        QList<QSharedPointer<ClientNotifier>> targets;
        {
            QMutexLocker lock(&NotificationManager::instance().m_mutex);
            for (const auto &n : NotificationManager::instance().m_connectedClients) {
                if (n && !n->isDisconnected() && !targets.contains(n)) {
                    if (targetID.isEmpty() || n->getLinkId() == targetID || n->getUser() == targetID || n->getToken() == targetID) {
                        targets.append(n);
                    }
                }
            }
        }

        if (targets.isEmpty()) {
            qDebug() << "[NotificationManager] webPushHandler: no active clients matching target:" << targetID << "for tunnel:" << tunnelID;
            return false;
        }

        QJsonObject sigData;
        sigData["ID"] = tunnelID;
        sigData["tunnelID"] = tunnelID;
        sigData["kind"] = kind;
        sigData["payload"] = QString::fromLatin1(payload.toBase64());

        qDebug() << "[NotificationManager] webPushHandler: broadcasting" << kind << "for tunnel:" << tunnelID << "to" << targets.count() << "targets (total clients:" << NotificationManager::instance().m_connectedClients.count() << ")";

        for (const auto &notifier : targets) {
            QMetaObject::invokeMethod(notifier.data(), [notifier, sigData]() {
                notifier->pushWebSignal(sigData);
            }, Qt::QueuedConnection);
        }
        return true;
    });
}



void NotificationManager::webSocketConnected(WebSocketTransport *client)
{
    //qDebug() << "WebSocket connected";
    QSharedPointer<ClientNotifier> notifier(new ClientNotifier(client, m_sessionProvider), &QObject::deleteLater);
    {
        QMutexLocker lock(&m_mutex);
        m_connectedClients.append(notifier);
    }

    connect(notifier.data(), &ClientNotifier::userConnected, this, &NotificationManager::userConnected);
    connect(notifier.data(), &ClientNotifier::userDisconnected, this, &NotificationManager::userDisconnected);
    connect(notifier.data(), &ClientNotifier::pushNotificationsSubscribed, this, &NotificationManager::userSubscribedForPush);

    //Listen to LinkConnected event from PWA
    connect(notifier.data(), &ClientNotifier::linkConnected, this, [this, notifier] (const QString& linkID){
        QMutexLocker lock(&m_mutex);
        m_linkNotifiers.insert(linkID, notifier);
    });

}

void NotificationManager::userConnected(const QString &userId)
{
    ClientNotifier *notifierPtr = static_cast<ClientNotifier *>(sender());
    QSharedPointer<ClientNotifier> notifier;
    {
        QMutexLocker lock(&m_mutex);
        for(auto n : m_connectedClients) {
            if (n.data() == notifierPtr) {
                notifier = n;
                break;
            }
        }
    }

    if (!notifier)
        return;

    if (notifier->isGuest())
        qInfo() << "Guest session started:" << userId;
    else
        qInfo() << "User signed in:" << userId;

    {
        QMutexLocker lock(&m_mutex);
        if (! m_clientNotifiers[userId].contains(notifier))
            m_clientNotifiers[userId].append(notifier);
    }
    
    emit notifierConnected(userId, notifier);

    //check cached event
    {
        QMutexLocker lock(&m_mutex);
        Q_FOREACH(UserEvent userEvent, m_cachedEvents[userId]) {
            emit notifier->userEvent(userEvent.event, userEvent.data);
        }
        m_cachedEvents.remove(userId);
    }
}

void NotificationManager::userDisconnected(const QString &userId)
{
    ClientNotifier *notifierPtr = static_cast<ClientNotifier *>(sender());
    QSharedPointer<ClientNotifier> notifier;
    {
        QMutexLocker lock(&m_mutex);
        for(auto n : m_connectedClients) {
            if (n.data() == notifierPtr) {
                notifier = n;
                break;
            }
        }
    }

    if (!notifier)
        return;

    if (notifier->isGuest())
        qInfo() << "Guest session ended:" << userId;
    else
        qInfo() << "User signed out" << userId;
    
    {
        QMutexLocker lock(&m_mutex);
        m_clientNotifiers[userId].removeAll(notifier);
    }

    QString linkID = notifier->getLinkId();
    if (!linkID.isEmpty()) {
        QMutexLocker lock(&m_mutex);
        // Count connections with the same linkID
        int count = 0;
         Q_FOREACH(auto n, m_clientNotifiers[userId]) {
             if (n->getLinkId() == linkID)
                 count++;
         }
         
        // Only remove if this was the last connection for this linkID
        if (count == 0) {
            m_linkNotifiers.remove(linkID);
            emit linkDisconnected(linkID, userId);
        }
    }

    {
        QMutexLocker lock(&m_mutex);
        // Delay removal from connected clients to allow any pending stack frames (nested loops) to finish
        QTimer::singleShot(100, [this, notifier]() {
            QMutexLocker lock(&m_mutex);
            m_connectedClients.removeAll(notifier);
        });
    }
}


void NotificationManager::bytesUploaded(const QString &userId, int bytes)
{
    QMutexLocker lock(&m_mutex);
    Q_FOREACH(auto notifier, m_clientNotifiers[userId])
        emit notifier->bytesUploaded(bytes);
}

void NotificationManager::sessionEnded(const QString &userId, const QString &sessionId)
{
    QMutexLocker lock(&m_mutex);
    Q_FOREACH(auto notifier, m_clientNotifiers[userId])
        if (notifier->getToken() == sessionId)
            notifier->expire();
}

void NotificationManager::userSubscribedForPush(const QString &user, const QString &token, QJsonObject subscription)
{
    QMutexLocker lock(&m_mutex);
    m_pushSubscriptions[user][token] = subscription;
}

void NotificationManager::sendPushNotification(const QJsonObject& subscription, const QJsonObject& payload)
{
    QNetworkRequest request(QUrl(QString("%1/push").arg(m_pushServer)));
    QJsonObject data;
    data["subscription"]  = subscription;
    data["payload"] = payload;
    QObject* singleTimeContext = new QObject();
    connect(&m_networkManager, &QNetworkAccessManager::finished, singleTimeContext, [this, singleTimeContext] (QNetworkReply *reply) {
        delete singleTimeContext;
        if (reply->error() !=  QNetworkReply::NoError)
            qWarning() << "Push error:" << reply->errorString();

        reply->deleteLater();
    });
    request.setRawHeader("Content-Type", "application/json; charset=UTF-8");
    QJsonDocument requestData(data);
    m_networkManager.post(request, requestData.toJson(QJsonDocument::Compact));
}


void NotificationManager::userEvent(const QString &userId, const QString& event, QJsonObject data)
{
    QMutexLocker lock(&m_mutex);
    if (m_clientNotifiers[userId].count()) {
        data["timestamp"] = QDateTime::currentDateTime().toLocalTime().toString("yyyy-MM-dd hh:mm:ss");
        Q_FOREACH(auto notifier, m_clientNotifiers[userId])
            emit notifier->userEvent(event, data);
    } else {
        data["timestamp"] = QDateTime::currentDateTime().toLocalTime().toString("yyyy-MM-dd hh:mm:ss");
        UserEvent userEvent{event, data};
        m_cachedEvents[userId].append(userEvent);
        if (m_cachedEvents[userId].count() > 20)
            m_cachedEvents[userId].removeFirst();
    }

    //send push notifications for subscribers
    if (m_pushServerEnabled && m_pushSubscriptions[userId].count()) {
        Q_FOREACH(const QJsonObject& subscription, m_pushSubscriptions[userId])  {
            QJsonObject payload = data;
            payload["event"] = event;
            sendPushNotification(subscription, payload);
        }
    }
}


void NotificationManager::shareAdded(const QString &user, const QString &shareName)
{
    QMutexLocker lock(&m_mutex);
    qInfo() << "Share added for user:" << user << "share:" << shareName;
    Q_FOREACH(auto notifier, m_clientNotifiers[user])
        emit notifier->shareAdded(shareName);
}

void NotificationManager::shareRemoved(const QString &user, const QString &shareName)
{
    QMutexLocker lock(&m_mutex);
    qInfo() << "Share removed for user:" << user << "share:" << shareName;
    Q_FOREACH(auto notifier, m_clientNotifiers[user])
        emit notifier->shareRemoved(shareName);
}

void NotificationManager::setDeviceCount(const QString &user, int deviceCount)
{
    QMutexLocker lock(&m_mutex);
    Q_FOREACH(auto notifier, m_clientNotifiers[user])
        emit notifier->deviceCountChanged(deviceCount);
}




void NotificationManager::fileUpdated(const QString &owner, const QString &user, const QString &filePath)
{
    {
        QMutexLocker lock(&m_mutex);
        Q_FOREACH(auto notifier, m_clientNotifiers[owner])
            emit notifier->fileUpdated(filePath);
    }
    //notify owner
    if (owner != user) {
        QJsonObject event;
        event["user"] = m_sessionProvider->checkUserIsGuest(user) ? "external_user"  : user;
        event["path"] = filePath;
        userEvent(owner, "file_updated", event);
    }
}

void NotificationManager::fileRemoved(const QString &owner, const QString &user, const QString &filePath)
{
    {
        QMutexLocker lock(&m_mutex);
        Q_FOREACH(auto notifier, m_clientNotifiers[owner])
            emit notifier->fileRemoved(filePath);
    }
    //notify owner
    if (owner != user) {
        QJsonObject event;
        event["user"] = m_sessionProvider->checkUserIsGuest(user) ? "external_user" : user;
        event["path"] = filePath;
        userEvent(owner, "file_removed", event);
    }
}

void NotificationManager::statsUpdated(const QString &user, QJsonObject stats)
{
    QMutexLocker lock(&m_mutex);
    Q_FOREACH(auto notifier, m_clientNotifiers[user])
        if (notifier->isAdmin())
            emit notifier->statsUpdated(stats);
}

void NotificationManager::fileUpdatedForGuests(const QString &user, const QString &filePath)
{
    QList<ClientSession *> list = m_sessionProvider->getSessionsForPath(filePath);
    Q_FOREACH(ClientSession* s,  list) {
        QString sharedPath = s->getVar("itemPath", filePath).toString();
        QString sharedRootName = s->getVar("Item", "").toString();
        int p = sharedPath.length();
        QString updatedSharedPath = filePath.mid(p);
        if (!updatedSharedPath.isEmpty())
            updatedSharedPath = QString("/%1/%2").arg(sharedRootName, updatedSharedPath);
        else
            updatedSharedPath = QString("/%1").arg(sharedRootName);
        fileUpdated(s->sessionID(), user,  updatedSharedPath);
    }
}

void NotificationManager::fileRemovedForGuests(const QString &user, const QString &filePath)
{
    QList<ClientSession *> list = m_sessionProvider->getSessionsForPath(filePath);
    Q_FOREACH(ClientSession* s,  list) {
        QString sharedPath = s->getVar("itemPath", filePath).toString();
        QString sharedRootName = s->getVar("Item", "").toString();
        int p = sharedPath.length();
        QString updatedSharedPath = filePath.mid(p);
        if (!updatedSharedPath.isEmpty())
            updatedSharedPath = QString("/%1/%2").arg(sharedRootName, updatedSharedPath);
        else
            updatedSharedPath = QString("/%1").arg(sharedRootName);
        fileRemoved(s->sessionID(), user, updatedSharedPath);
    }
}

QSharedPointer<ClientNotifier> NotificationManager::getLinkNotifier(const QString &linkID)
{
    QMutexLocker lock(&m_mutex);
    if (m_linkNotifiers.contains(linkID)) {
        auto cached = m_linkNotifiers[linkID];
        if (cached && !cached->isDisconnected())
            return cached;
        m_linkNotifiers.remove(linkID);
    }

    // Fallback: Lookup in m_connectedClients strictly by active linkID
    for (const auto &notifier : m_connectedClients) {
        if (notifier && !notifier->isDisconnected() && notifier->getLinkId() == linkID) {
            m_linkNotifiers.insert(linkID, notifier);
            return notifier;
        }
    }

    return QSharedPointer<ClientNotifier>(nullptr);
}

bool NotificationManager::isWebSocketLink(const QString &linkID)
{
    return hasActiveNotifierForLink(linkID, nullptr);
}

bool NotificationManager::hasActiveNotifierForLink(const QString &linkID, const ClientNotifier *exclude)
{
    if (linkID.isEmpty())
        return false;
    QMutexLocker lock(&m_mutex);
    for (const auto &notifier : m_connectedClients) {
        if (notifier && notifier.data() != exclude && !notifier->isDisconnected() && notifier->getLinkId() == linkID)
            return true;
    }
    return false;
}

bool NotificationManager::hasConnections(const QString &userId, const QString &sessionId)
{
    QMutexLocker lock(&m_mutex);
    bool result = false;
    Q_FOREACH(auto notifier, m_clientNotifiers[userId])
        if (notifier->getToken() == sessionId) {
            result = true;
            break;
        }
    return result;
}
