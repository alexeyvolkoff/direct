#ifndef SSLHELPER_H
#define SSLHELPER_H

#include <QObject>
#include <QtDebug>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QByteArray>
#include <QSsl>
#include <QSslCertificate>
#include <QSslCipher>
#include <QSslConfiguration>
#include <QSslKey>
#include "onmydisklink_global.h"


class  SslHelper : public QObject
{
    Q_OBJECT
public:
    explicit SslHelper(QObject *parent = nullptr);
    bool genPrivateKeyToFile(int length, const QString &keyFilePath, const char *keyPass);
    bool genPrivateKeyToBytes(int length, const char *keyPass, QByteArray &privateKey);
    bool genCSRToFile(const QString &csrFilePath, const QString &keyFilePath, const char *keyPass, const QString &commonName);
    QByteArray genCSRToBytes(bool &isOk, const QString &keyFilePath, const char *keyPass, const QString &commonName);
    bool genSelfSignedCert(int length, const QString &keyFilePath, const char *keyPass, const QString &commonName, const QString &certFilePath, const QString &organization = QString());
    ~SslHelper();
};

#endif // SSLHELPER_H
