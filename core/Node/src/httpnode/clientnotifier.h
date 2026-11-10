#ifndef CLIENTNOTIFIER_H
#define CLIENTNOTIFIER_H

//Used only to fire events in JavaScript.
#include <QObject>
#include <QPointer>
#include <QWebChannel>
#include <QWebSocketServer>

#include "websocketclientwrapper.h"
#include "websockettransport.h"
#include "sessionauth.h"


class ClientNotifier: public QObject
{
    Q_OBJECT

private:
    QWebChannel m_webChannel;
    SessionAuth *m_sessionAuth;
    QString getBackendType();
    QString m_user;
    QString m_owner;
    QString m_token;
    QString m_linkID;
    QString m_group;
    QString m_shareName;
    QString m_version;
    int m_deviceCount;
    bool m_isAdmin;
    bool m_isGroupAdmin;
    bool m_isGuest;
    bool m_disconnected;
    WebSocketTransport *m_clientTransport;
    bool doSubscribe(const QString& token);
private slots:
    void socketDisconnected();
public:
    ClientNotifier(WebSocketTransport *client, SessionAuth *sessionAuth, QObject *parent = Q_NULLPTR);

    QString getUser() { return m_user; }
    QString group() { return m_group; }
    QString shareName() { return m_shareName; }
    QString version() { return m_version; }
    bool isAdmin() { return m_isAdmin; }
    bool isGuest() { return m_isGuest; }
    bool isGroupAdmin() { return m_isGroupAdmin; }
    bool isDisconnected() const { return m_disconnected; }
    QString getToken() { return m_token; }
    QString getLinkId() { return m_linkID; }
    QString getVersion() { return m_version; }
    void setLinkId(const QString& linkID);
    void setToken(const QString& token);
    int getDeviceCount();
    void setDeviceCount(int count);

    //PWA fileApi support
    QJsonObject fileApi(const QJsonObject cmd);

    Q_PROPERTY(QString backendType READ  getBackendType CONSTANT)
    Q_PROPERTY(QString user READ  getUser CONSTANT)
    Q_PROPERTY(QString token READ  getToken WRITE setToken NOTIFY userConnected)
    Q_PROPERTY(QString linkID READ  getLinkId WRITE setLinkId NOTIFY linkConnected)
    Q_PROPERTY(QString version READ  getVersion CONSTANT)
    Q_PROPERTY(int deviceCount READ  getDeviceCount WRITE setDeviceCount  NOTIFY deviceCountChanged)

    ~ClientNotifier();
public slots:
    QJsonObject getUserProfile(const QString& user);
    bool updateUserProfile(const QString& user, QJsonObject profile);
    bool addUserProfile(const QString& user, QJsonObject profile);
    bool addGroup(const QJsonObject group);
    bool updateGroup(const QString& group, QJsonObject groupData);
    bool removeGroup(const QString& group);
    bool removeUserProfile(const QString& user);
    bool removeDevice(const QString& deviceId);
    void expire();
    void subscribe(QJsonObject subscription);
    bool subscribeChannel(const QString& token, const QString& linkID);
    QJsonArray getUsers();
    QJsonArray getDevices(const QJsonArray& filterLinkIds = QJsonArray());
    QJsonArray getGroups();

    //Update Share info
    void updateShareInfo(const QString &token, QJsonObject shareData, QJsonArray entryData, QJsonArray links = QJsonArray());
    void linkResponse(const QJsonObject responseData);
    // Gateway → browser delivery of WebRTC signaling for this web device
    void pushWebSignal(const QJsonObject& signalData);
    bool sendSignaling(const QString &linkID, const QString &tunnelID, const QString &kind, const QString &payloadB64, quint16 port = 0, const QString &service = "");
    void notifyPublicLinkAdded(const QString& itemPath, const QString& link, const QHash<QString, QVariant>& linkData);
    void onUserEvent(QString user, QString event, QJsonObject data);

signals:
    void shareAdded(const QString& shareName);
    void shareRemoved(const QString& shareName);
    void collectionChanged(const QString& collectionPath, QHash<QString, QVariant> shareData, QHash<QString, QHash<QString, QVariant>> shareEntryData);
    void progress(float progress);
    void bytesUploaded(int bytes);
    void userConnected(const QString& user);
    void userDisconnected(const QString& user);
    void deviceCountChanged(int devices);
    void statsUpdated(QJsonObject stats);
    void sessionExpired();
    void fileUpdated(const QString& filePath);
    void fileRemoved(const QString& filePath);
    void userEvent(const QString& event, QJsonObject data);
    void pushNotificationsSubscribed(const QString& user, const QString& token, QJsonObject subscription);
    void linkConnected(const QString& linkID);
    void linkDisconnected(const QString& linkID);
    void linkCommand(const QJsonObject& cmd);
    void responseReady(const  QJsonObject response);
    // Emitted to the browser when the gateway routes WebRTC signaling
    // (tunnel-open / offer / candidate) to this device's WebSocket channel.
    // payload field carries base64-encoded signaling data, kind identifies it.
    void webTunnelOpen(const QJsonObject& signalData);

};

#endif // CLIENTNOTIFIER_H
