#ifndef BASICAUTH_H
#define BASICAUTH_H

#include <QObject>
#include <QtSql>
#include "qhttpserver.hpp"
#include "qhttpserverconnection.hpp"
#include "qhttpserverrequest.hpp"
#include "qhttpserverresponse.hpp"
#include "iauthorization.h"

using namespace qhttp::server;

class BasicAuth : public IAuthorization
{
    Q_OBJECT
public:
    BasicAuth( QString realm);

    // BaseAccountProvider interface
    QString methodName();
    QString headerString(QHttpRequest *httpRequest);
    AuthResult validate(QHttpRequest *httpRequest, QHttpResponse *httpResponse, QString &userID, QString &rootOwnerID, QString &itemPath);

private:
    QString m_Realm;    
};

#endif // BASICAUTH_H
