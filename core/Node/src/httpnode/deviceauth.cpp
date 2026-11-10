#include "deviceauth.h"

DeviceAuth::DeviceAuth(QString realm)
{
    m_Realm = realm;
}

QString DeviceAuth::methodName()
{
    return QStringLiteral("device");
}

QString DeviceAuth::headerString(QHttpRequest *httpRequest)
{
    Q_UNUSED(httpRequest);
    return QString("Device realm=\"%1\"").arg(m_Realm);
}

AuthResult DeviceAuth::validate(QHttpRequest *httpRequest, QHttpResponse *httpResponse, QString &userID, QString &rootOwnerID, QString &itemPath)
{
    Q_UNUSED(itemPath);
    Q_UNUSED(httpResponse);

    if (httpRequest->method() != qhttp::EHTTP_POST)
        return AUTH_FAIL;

    QString authValue = "";
    if (httpRequest->headers().contains("authorization"))
    {
        authValue = httpRequest->headers()["authorization"];
    } else
        return AUTH_FAIL;

    if (! authValue.startsWith("device"))
        return AUTH_FAIL;

   QString deviceId = authValue.mid(7).trimmed();

   userID = deviceId;
   rootOwnerID = userID;


   return AUTH_OK;
}

