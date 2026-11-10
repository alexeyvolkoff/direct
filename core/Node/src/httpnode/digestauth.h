#ifndef DIGESTAUTH_H
#define DIGESTAUTH_H

#include <QObject>
#include <QDateTime>
#include <QString>
#include <QHash>
#include <QUuid>
#include <QTimer>
#include <QtSql>
#include "qhttpserver.hpp"
#include "qhttpserverconnection.hpp"
#include "qhttpserverrequest.hpp"
#include "qhttpserverresponse.hpp"
#include "iauthorization.h"
#include "clientsession.h"

using namespace qhttp::server;

class DigestAuth : public IAuthorization
{
    Q_OBJECT
public:
    DigestAuth( QString realm);

    // IAccountProvider interface
    QString methodName();
    QString headerString(QHttpRequest *httpRequest);
    AuthResult validate(QHttpRequest *httpRequest, QHttpResponse *httpResponse, QString &userID, QString &rootOwnerID, QString &itemPath);

private:
    QString GenerateNonce();
    bool IsValidNonce(QString nonce);
    QHash<QString, QString> getHeaderParts(QString authString);
    QString m_Realm;
    const QString m_Algorithm = "MD5";
    const QString m_Qop = "auth";
    QHash <QString, ClientSession*> m_DigestSessions;
    ClientSession* peekDigestSession(QString Opaque);
    QString getOpaque(QHttpRequest* httpRequest);
};

#endif // DIGESTAUTH_H
