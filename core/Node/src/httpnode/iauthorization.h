#ifndef IAUTHORIZATION_H
#define IAUTHORIZATION_H

#include <QObject>
#include <QCryptographicHash>
#include "qhttpserver.hpp"
#include "qhttpserverconnection.hpp"
#include "qhttpserverrequest.hpp"
#include "qhttpserverresponse.hpp"



using namespace qhttp::server;

enum AuthResult {AUTH_OK, AUTH_OK_PUBLIC, AUTH_OK_SHARED, AUTH_OK_CRAWLING, AUTH_OK_SEARCHING, AUTH_OK_REF,  AUTH_OFFLINE, AUTH_NOTALLOWED, AUTH_FAIL};

class IAuthorization: public QObject
{
public:
    virtual QString methodName() = 0;
    virtual QString headerString(QHttpRequest *httpRequest) = 0;
    virtual AuthResult validate(QHttpRequest *httpRequest, QHttpResponse *httpResponse, QString &userID, QString &rootOwnerID, QString &itemPath) {
        Q_UNUSED(httpRequest)
        Q_UNUSED(httpResponse)
        Q_UNUSED(userID)
        Q_UNUSED(rootOwnerID)
        Q_UNUSED(itemPath)
        return  AUTH_FAIL;
    };

    static QString base64_encode(QString string)
    {
        QByteArray ba;
        ba.append(string.toUtf8());
        return ba.toBase64();
    }

    static QString base64_decode(QString string)
    {
        QByteArray ba;
        ba.append(string.toUtf8());
        return QByteArray::fromBase64(ba);
    }

    static QString md5(QString value)
    {
        return QString(QCryptographicHash::hash(value.toUtf8(),QCryptographicHash::Md5).toHex());
    }

protected:
	AuthResult checkMethod(QHttpRequest *httpRequest, QString allowedMethods, QString denyedMethods)
    {
        if (allowedMethods.toLower() == "all")
        {
            QStringList methods = denyedMethods.toLower().split(';');
            if (methods.contains(httpRequest->methodString().toLower()))
                return AUTH_NOTALLOWED;
        }
        else
        {
            QStringList methods = allowedMethods.toLower().split(';');
            if (!methods.contains(httpRequest->methodString().toLower()))
                return AUTH_NOTALLOWED;
        }

        return AUTH_OK;
    }

signals:

public slots:
};

#endif // IAUTHORIZATION_H
