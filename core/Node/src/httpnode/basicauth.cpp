#include "basicauth.h"

BasicAuth::BasicAuth(QString realm)
{
    m_Realm = realm;
}

QString BasicAuth::methodName()
{
    return QStringLiteral("Basic");
}

QString BasicAuth::headerString(QHttpRequest *httpRequest)
{
    Q_UNUSED(httpRequest);
    return QString("Basic realm=\"%1\"").arg(m_Realm);
}

AuthResult BasicAuth::validate(QHttpRequest *httpRequest, QHttpResponse *httpResponse, QString &userID, QString &rootOwnerID, QString &itemPath)
{
    Q_UNUSED(itemPath);
    Q_UNUSED(httpRequest);
    Q_UNUSED(httpResponse);
    Q_UNUSED(userID);
    Q_UNUSED(rootOwnerID);
    // Legacy HTTP Basic auth removed: accounts live in GunDB (DID identities),
    // the gateway is a signaling relay and knows no users.
    return AUTH_FAIL;
}

