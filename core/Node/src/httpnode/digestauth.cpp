#include "digestauth.h"

DigestAuth::DigestAuth(QString realm)
{
    m_Realm = realm;
}

QString DigestAuth::methodName()
{
    return QStringLiteral("Digest");
}

QString DigestAuth::headerString(QHttpRequest *httpRequest)
{
    QString s_Nonce = GenerateNonce();
    QString s_Opaque = getOpaque(httpRequest);
    ClientSession* session = this->peekDigestSession(s_Opaque);

    bool isNonceStale = session->getVar(QStringLiteral("staleNonce"), false).toBool();

    QString authHeader = QString("Digest");
    authHeader.append(QStringLiteral(" realm=\""));
    authHeader.append(m_Realm);
    authHeader.append(QStringLiteral("\""));
    authHeader.append(QStringLiteral(", nonce=\""));
    authHeader.append(s_Nonce);
    authHeader.append(QStringLiteral("\""));
    authHeader.append(QStringLiteral(", opaque=\""));
    authHeader.append(s_Opaque);
    authHeader.append(QStringLiteral("\""));
    authHeader.append(QStringLiteral(", stale="));
    authHeader.append(isNonceStale ? QStringLiteral("true") : QStringLiteral("false"));
    authHeader.append(QStringLiteral(", algorithm=\""));
    authHeader.append(m_Algorithm);
    authHeader.append(QStringLiteral("\""));
    authHeader.append(QStringLiteral(", qop=\""));
    authHeader.append(m_Qop);
    authHeader.append(QStringLiteral("\""));

    //qDebug() << authHeader;
    return authHeader;
}

AuthResult DigestAuth::validate(QHttpRequest *httpRequest, QHttpResponse *httpResponse, QString &userID, QString &rootOwnerID, QString &itemPath)
{
    Q_UNUSED(itemPath);
    //qDebug() << httpRequest->headers();
    QHash<QString, QString> authHeaders = getHeaderParts(httpRequest->headers()["authorization"]);

    //qDebug() << "parsed: " << authHeaders;

    if (!authHeaders.contains(QStringLiteral("username")))
        return AUTH_FAIL;

    // a, b)
    // A1 = unq(username-value) ":" unq(realm-value) ":" passwd
    Q_UNUSED(userID);
    Q_UNUSED(rootOwnerID);
    // Legacy HTTP Digest auth removed: accounts live in GunDB (DID identities),
    // the gateway is a signaling relay and knows no users.
    QString HA1 = "";
    Q_UNUSED(HA1)
    return AUTH_FAIL;

    //HA1 = md5(HA1);

    // c)
    // A2 = HTTP Method ":" digest-uri-value
    QString A2 = QString("%1:%2").arg(httpRequest->methodString()).arg(authHeaders[QStringLiteral("uri")]);
    QString RSPA2 = QString(":%1").arg(authHeaders[QStringLiteral("uri")]);

    // d)
    // HA2 = MD5(A2)
    QString HA2 = md5(A2);
    QString RSPHA2 = md5(RSPA2);

    // e)
    // GENRESPONSE = HA1 ":" nonce ":" nc ":" cnonce ":" qop ":" HA2
    QString GENRESPONSE;
    QString GENRSPAUTH;
    if ( authHeaders.contains(QStringLiteral("qop")))
    {
        GENRESPONSE = QString("%1:%2:%3:%4:%5:%6")
                                .arg(HA1)
                                .arg(authHeaders[QStringLiteral("nonce")])
                                .arg(authHeaders[QStringLiteral("nc")])
                                .arg(authHeaders[QStringLiteral("cnonce")])
                                .arg(authHeaders[QStringLiteral("qop")])
                                .arg(HA2);    

        GENRSPAUTH = QString("%1:%2:%3:%4:%5:%6")
                                .arg(HA1)
                                .arg(authHeaders[QStringLiteral("nonce")])
                                .arg(authHeaders[QStringLiteral("nc")])
                                .arg(authHeaders[QStringLiteral("cnonce")])
                                .arg(authHeaders[QStringLiteral("qop")])
                                .arg(RSPHA2);
    }
    else
    {
        GENRESPONSE = QString("%1:%2:%3")
                                .arg(HA1)
                                .arg(authHeaders[QStringLiteral("nonce")])
                                .arg(HA2);
        GENRSPAUTH = QString("%1:%2:%3")
                                .arg(HA1)
                                .arg(authHeaders[QStringLiteral("nonce")])
                                .arg(RSPHA2);
    }

    //qDebug() << GENRESPONSE;
    QString HGENRESPONSE = md5(GENRESPONSE);
    QString RSPAUTH = md5(GENRSPAUTH);

    // Проверяем nonce

    bool isNonceStale = !IsValidNonce(authHeaders[QStringLiteral("nonce")]);
    QString s_Opaque = getOpaque(httpRequest);
    ClientSession* session = this->peekDigestSession(s_Opaque);
    session->setVar(QStringLiteral("staleNonce"), isNonceStale);

    if (isNonceStale)
    {
        //qDebug() << "stalen nonce!";
        return AUTH_FAIL;
    }


    // Сверяем HGENRESPONSE с response заголовка
    // Проверяем nonce
    // если всё ok - создаём GenericPrincipal с группами юзера

    if ((authHeaders[QStringLiteral("response")] == HGENRESPONSE))
    {
        QString AuthenticationInfo = QString("rspauth=\"%1\", cnonce=\"%2\", nc=%3, qop=auth")
                                            .arg(RSPAUTH)
                                            .arg(authHeaders[QStringLiteral("cnonce")])
                                            .arg(authHeaders[QStringLiteral("nc")]);
        userID = authHeaders[QStringLiteral("username")];
        rootOwnerID = userID;
        QByteArray utf8Value = AuthenticationInfo.toUtf8();
        httpResponse->addHeader("Authentication-Info", utf8Value);

        return AUTH_OK;
    }

    return AUTH_FAIL;
}


QString DigestAuth::GenerateNonce()
{
    // Создаём unique nonce - самый облегчённый вариант
    // настоящее время + 3 минуты, закодированное base64
    // проверка на качество nonce тоже будет происходить по времени
    // Пример усиленного варианта - использование ETag и ещё одного ключа, который
    // знает server
    QDateTime now = QDateTime::currentDateTime().addSecs(180);
    QString expireStr = now.toString(Qt::ISODate);
    expireStr = base64_encode(expireStr);
    while (expireStr.endsWith('='))
        expireStr.chop(1);

    return expireStr;
}

bool DigestAuth::IsValidNonce(QString nonce)
{
    // Проверяем nonce
    // раскодируем из base64 и сверяем
    // в этом примере nonce простой - поэтому и проверка простая -
    // сверяем с временем

    QDateTime expireTime;
    int numPadChars = nonce.length() % 4;
    if (numPadChars > 0)
       numPadChars = 4 - numPadChars;
    QString newNonce = nonce.leftJustified(nonce.length() + numPadChars, '=');

    try
    {
       QString preNonce = base64_decode(newNonce);
       expireTime = QDateTime::fromString(preNonce, Qt::ISODate);
    }
    catch (...)
    {
       return false;
    }
    return (expireTime >= QDateTime::currentDateTime());
}

QHash<QString, QString> DigestAuth::getHeaderParts(QString authString)
{
    //qDebug() << authString;
    QHash<QString, QString> pairs;
    QString pureAuthString = authString.mid(7);
    QString authKey;
    QString authValue;
    bool readingKey = true;
    bool readingValue = false;
    bool quotedValue = false;
    Q_FOREACH(QChar c, pureAuthString)
    {
        if (readingKey)
        {
            if (c == '=')
            {
                readingKey = false;
                readingValue = true;
                continue;
            }

            if (c == ' ' || c == ',')
                continue;

            authKey.append(c);
            continue;
        }

        if (readingValue)
        {
            if (c == '"')
            {
                if (quotedValue)
                {
                    readingValue = false;
                    quotedValue = false;
                    readingKey = true;
                    pairs.insert(authKey, authValue);
                    authKey = QString("");
                    authValue = QString("");
                    continue;
                }
                else
                {
                    quotedValue = true;
                    continue;
                }
            }

            if (!quotedValue && c == ' ')
                continue;

            if (!quotedValue && c == ',')
            {
                readingKey = true;
                readingValue = false;
                pairs.insert(authKey, authValue);
                authKey = QString("");
                authValue = QString("");
                continue;
            }

            authValue.append(c);
        }
    }
    pairs.insert(authKey, authValue);

    return pairs;
}

ClientSession *DigestAuth::peekDigestSession(QString Opaque)
{
    if (!m_DigestSessions.contains(Opaque))
    {
        ClientSession* session = new ClientSession(600000);
        session->setUserName(Opaque);
        session->moveToThread(qApp->thread());

        connect(session, &ClientSession::timeout, session, [this, session, Opaque]() {
            m_DigestSessions.remove(Opaque);
            delete session;
        });

        m_DigestSessions.insert(Opaque, session);
        session->restartTimer();
    }
    return m_DigestSessions[Opaque];
}


QString DigestAuth::getOpaque(QHttpRequest *httpRequest)
{
    QHash<QString, QString> authHeaders;
    if (httpRequest->headers().contains("authorization"))
    {
        QString authValue(httpRequest->headers()["authorization"]);
        authHeaders = getHeaderParts(authValue);
    }
    return authHeaders.contains(QStringLiteral("opaque")) ? authHeaders[QStringLiteral("opaque")] : md5(QUuid::createUuid().toString());
}
