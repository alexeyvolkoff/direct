#ifndef PKIHELPER_H
#define PKIHELPER_H

#include <QObject>
#include <QSettings>
#include "sslhelper.h"

class PKIHelper : public SslHelper
{
    Q_OBJECT
public:
    explicit PKIHelper(QSettings *settings, QObject *parent = nullptr);
    bool initPKI();
    QSslCertificate getCaCert();
    bool signCSR(const QByteArray &csrBody, int serialNumber, const QString &userName, QByteArray &signedCertBody);

private:
    const char      *szCN = "onmydisk.pki.ca";
    bool loadPKI();
    QString m_caCertPath;
    QString m_legacyCaCertPath;
    QSslCertificate m_caCert;

    QString m_caKeyPath;
    QSslKey m_caPKey;
    QString m_caKeyPass;

    QString m_szCountry = "SI";
    QString m_szProvince = "LJ";
    QString m_szCity = "Ljubljana";
    QString m_szOrganization = "B1NEON d.o.o.";
};

#endif // PKIHELPER_H
