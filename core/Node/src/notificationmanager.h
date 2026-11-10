#ifndef NOTIFICATIONMANAGER_H
#define NOTIFICATIONMANAGER_H

#include <QObject>
#include <QNetworkAccessManager>
#include <QRecursiveMutex>

#include "clientnotifier.h"
#include "sessionauth.h"

typedef struct {
    QString event;
    QJsonObject data;
} UserEvent;
Q_DECLARE_METATYPE(UserEvent)

class NotificationManager : public QObject
{
    Q_OBJECT
private:
    QString m_pushServer;
    bool m_pushServerEnabled = false;
    QRecursiveMutex m_mutex;
    SessionAuth *m_sessionProvider;
    QNetworkAccessManager m_networkManager;
    QMap<QString, QList<QSharedPointer<ClientNotifier>>> m_clientNotifiers;
    QHash<QString, QSharedPointer<ClientNotifier>> m_linkNotifiers;
    QList<QSharedPointer<ClientNotifier>> m_connectedClients;
    QMap<QString, QList<UserEvent>> m_cachedEvents;
    QMap<QString, QMap<QString, QJsonObject>> m_pushSubscriptions;
    void sendPushNotification(const QJsonObject &subscription, const QJsonObject &payload);

    explicit NotificationManager();
public:
    static NotificationManager& instance();
    void init(SessionAuth * sessionAuthProvider, const QString &pushServer = "");
    void shareAdded(const QString& user,  const QString& shareName);
    void shareRemoved(const QString& user, const QString& shareName);
    void setDeviceCount(const QString& user, int deviceCount);
    void statsUpdated(const QString& user,  QJsonObject stats);
    void fileUpdatedForGuests(const QString& user, const QString& filePath);
    void fileRemovedForGuests(const QString& user, const QString& filePath);
    QSharedPointer<ClientNotifier> getLinkNotifier(const QString& linkID);
    bool isWebSocketLink(const QString& linkID);
    bool hasActiveNotifierForLink(const QString& linkID, const ClientNotifier *exclude = nullptr);
    bool hasConnections(const QString &userId, const QString &sessionId);
public slots:
    void webSocketConnected(WebSocketTransport *client);
    void userConnected(const QString &userId);
    void userDisconnected(const QString &userId);
    void userEvent(const QString &user, const QString &event, QJsonObject data);
    void fileUpdated(const QString &owner, const QString& user, const QString& filePath);
    void fileRemoved(const QString &owner, const QString& user, const QString& filePath);
private slots:
    void bytesUploaded(const QString &userId, int bytes);
    void sessionEnded(const QString &userId, const QString &sessionId);
    void userSubscribedForPush(const QString &user, const QString &token,  QJsonObject subscription);

signals:
    void notifierConnected(const QString &user, QSharedPointer<ClientNotifier> notifier);
    //PWA client share updated
    void linkConnected(QString linkID, QString user);
    void shareUpdated(QString ID, QString user, QString shareName, QHash<QString, QVariant> shareData, QHash<QString, QHash<QString, QVariant>> shareEntryData, QHash<QString, QHash<QString, QVariant>> shareLinks = {});
    void linkDisconnected(QString linkID, QString user);

};

#endif // NOTIFICATIONMANAGER_H
