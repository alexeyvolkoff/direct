#ifndef TICKETAUTH_H
#define TICKETAUTH_H

#include <QObject>
#include <QtSql>
#include <QTimer>
#include <QUuid>
#include "qhttpserver.hpp"
#include "qhttpserverconnection.hpp"
#include "qhttpserverrequest.hpp"
#include "qhttpserverresponse.hpp"
#include "iauthorization.h"

class DeviceAuth : public IAuthorization
{
    Q_OBJECT
public:
    DeviceAuth(QString realm);

    // BaseAccountProvider interface
    QString methodName();
    QString headerString(QHttpRequest *httpRequest);
    AuthResult validate(QHttpRequest *httpRequest, QHttpResponse *httpResponse, QString &userID, QString &rootOwnerID, QString &itemPath);

private:
    QString m_Realm;
};

#endif // TICKETAUTH_H
