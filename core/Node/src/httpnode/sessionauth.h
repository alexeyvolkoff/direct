#ifndef SESSIONAUTH_H
#define SESSIONAUTH_H

#include <QObject>
#include <QtSql>
#include <QTimer>
#include <QUuid>
#include <QMutex>
#include <QString>
#include <QStringList>
#include <QPointer>
#include "qhttpserver.hpp"
#include "qhttpserverconnection.hpp"
#include "qhttpserverrequest.hpp"
#include "qhttpserverresponse.hpp"
#include "iauthorization.h"
#include "clientsession.h"


class SessionAuth : public IAuthorization
{
    Q_OBJECT
public:
    explicit SessionAuth(QString realm, QString crawlerToken = "");
    ~SessionAuth();

	QString methodName();
	QString headerString(QHttpRequest *httpRequest);
    virtual AuthResult validate(QHttpRequest *httpRequest, QHttpResponse *httpResponse, QString &userID, QString &rootOwnerID, QString &linkID, QString &itemPath);
    QPointer<ClientSession> addSessionFor(QString userName, quint64 sessionLifeTime = 60000, QString sessionID = "");
    QPointer<ClientSession> getSession(QString sessionID, quint64 sessionLifeTime = 60000);
    QList<ClientSession*> getSessionsForPath(const QString &itemPath);
    bool sessionExists(QString sessionID);
    void removeSession(QHttpRequest *httpRequest);
    bool checkUserPass(QString userName, QString password);
    bool checkUserIsAdmin(QString userName);
    bool checkUserIsGuest(QString userID);
    QJsonObject getUserProfile(QString userName, QString group = "");
    bool checkKey(QString key);
    void restoreSessionForToken(const QString& token);
    ClientSession *sessionForToken(const QString& token, quint64 sessionLifetime = 86400000);
    bool linkIsMounted(QString user, QString linkID, QString itemPath);
    bool createUserAccount(QString server, QString userName, QString password, QByteArray postData);
    QString loginPagePath();
    void setSessionVariable(QString sessionID, QString varName, QVariant value);
    void setSessionVariables(QString sessionID, QHash<QString, QVariant> vars);
    QVariant getSessionVariable(QString sessionID, QString valueName, QVariant defaultValue);
    QVariant getStaticVariable(QString valueName);
    QString getCurrentUser(QString sessionID);
    void setCrawlerToken(QString crawlerToken) {m_crawlerToken = crawlerToken;}
    QString crawlerToken() { return m_crawlerToken; }
    const QString globalCookieID();
    const QString getSessionIdForSharedPath(const QString &sharedPath, QHttpRequest *httpRequest);
    const QString getSharedPath(QHttpRequest *httpRequest);
    void updateGuestAccess(QString itemPath, QString accessRights);
    void removeGuestAccess(QString itemPath);
    void removeGuestAccessByLink(QString linkId);
    void sharePathInCall(QString callID, QString LinkID,  QString itemPath);
    void unsharePathInCall(QString callID);
    void closeAll();
    void setServer(const QString& server){ m_server = server; }
    void updateUserRights(QString userName, QJsonObject rights);
    QJsonObject fetchRights(QString userName, QString token = "");
private:
    QString m_Realm;
    QString m_crawlerToken;
    QString m_server;
    QString m_consoleUrl;
    QMutex m_mutex;
    QHash<QString, ClientSession*> m_Sessions;
    ClientSession* userSession(QString userName, quint64 sessionLifeTime = 60000, QString sessionID = "");
    ClientSession* session(QString sessionID, quint64 sessionLifeTime = 60000);
    QString readSessionFromCookie(QHttpRequest *httpRequest);
    void applyRights(ClientSession* session, QJsonObject rightsProfile);
    void setSafeSessionId(qhttp::server::QHttpRequest *httpRequest, const QString &sessionID);
public slots:
    void removeSession(QString sessionID);
private slots:
    void sessionTimeout();
signals:
    bool authRequest(QString userName, QString password);
	bool createAccount(QString server, QString userName, QString password, QByteArray postData);
	bool keyAuthRequest(QString key);
    bool sessionExpired(QString userName, QString session);
};

#endif // SESSIONAUTH_H
