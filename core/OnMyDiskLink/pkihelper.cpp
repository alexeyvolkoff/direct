#include "pkihelper.h"
#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/opensslv.h>
#include <openssl/crypto.h>
#include <QFileInfo>
#include <QDir>
#include <QDebug>

static int addExt(X509 *caCrt, X509 *crt, int nid, const char *value)
{
    X509_EXTENSION *ex = nullptr;
    X509V3_CTX ctx;
    X509V3_set_ctx_nodb(&ctx);
    X509V3_set_ctx(&ctx, caCrt, crt, nullptr, nullptr, 0);
    ex = X509V3_EXT_conf_nid(nullptr, &ctx, nid, const_cast<char *>(value));
    if (!ex)
        return 0;

    int ret = X509_add_ext(crt, ex, -1);
    X509_EXTENSION_free(ex);
    return ret;
}

PKIHelper::PKIHelper(QSettings *settings, QObject *parent) : SslHelper(parent)
{
    m_caCertPath = settings->value("PKI/CACert", "rootCA.crt").toString();
    m_caKeyPath = settings->value("PKI/CAKey", "rootCA.key").toString();
    m_caKeyPass = settings->value("PKI/CAPass", "").toString();

    m_szCountry = settings->value("PKI/Country", m_szCountry).toString();
    m_szProvince = settings->value("PKI/Province", m_szProvince).toString();
    m_szCity = settings->value("PKI/City", m_szCity).toString();
    m_szOrganization = settings->value("PKI/Organization", m_szOrganization).toString();

    // Initialization moved to SslHelper constructor
}

bool PKIHelper::initPKI()
{
    if (loadPKI())
        return true;

    qInfo() << "Creating PKI...";
    QFileInfo certInfo(m_caCertPath);
    QDir certDir = certInfo.absoluteDir();

    if (!certDir.exists() && !certDir.mkpath(certDir.absolutePath())) {
        qCritical() << "Unable to create PKI directory:" << certDir.absolutePath();
        return false;
    }

    EVP_PKEY *pkey = nullptr;
    X509 *x509 = nullptr;
    BIO *bp_private = nullptr;
    bool success = false;

    do {
#if OPENSSL_VERSION_NUMBER >= 0x30000000L // OpenSSL 3.0+
        // Generate RSA key
        pkey = EVP_RSA_gen(4096);
        if (!pkey) {
            qCritical() << "Failed to generate RSA key with EVP_RSA_gen";
            break;
        }
#else // OpenSSL 1.1.1
        RSA *rsa = nullptr;
        BIGNUM *bne = nullptr;

        bne = BN_new();
        if (!bne || BN_set_word(bne, RSA_3) != 1) {
            qCritical() << "BN_set_word failed";
            break;
        }

        rsa = RSA_new();
        if (!rsa || RSA_generate_key_ex(rsa, 4096, bne, nullptr) != 1) {
            qCritical() << "RSA_generate_key_ex failed";
            BN_free(bne);
            RSA_free(rsa);
            break;
        }

        pkey = EVP_PKEY_new();
        if (!pkey || EVP_PKEY_assign_RSA(pkey, rsa) != 1) {
            qCritical() << "Failed to assign RSA key to EVP_PKEY";
            BN_free(bne);
            RSA_free(rsa);
            EVP_PKEY_free(pkey);
            break;
        }
        rsa = nullptr; // Owned by pkey
        BN_free(bne);
#endif

        // Save private key
        bp_private = BIO_new_file(m_caKeyPath.toUtf8().constData(), "w");
        if (!bp_private) {
            qCritical() << "Failed to create BIO for private key:" << m_caKeyPath;
            break;
        }

        if (!PEM_write_bio_PrivateKey(bp_private, pkey, EVP_aes_256_cbc(),
                                      m_caKeyPass.isEmpty() ? nullptr : (unsigned char *)m_caKeyPass.toLocal8Bit().data(),
                                      m_caKeyPass.size(), nullptr, nullptr)) {
            qCritical() << "Failed to write private key";
            break;
        }

        // Create CA certificate
        x509 = X509_new();
        if (!x509) {
            qCritical() << "Unable to create X509 structure";
            break;
        }

        if (!ASN1_INTEGER_set(X509_get_serialNumber(x509), 0)) {
            qCritical() << "Failed to set serial number";
            break;
        }

        X509_gmtime_adj(X509_get_notBefore(x509), 0);
        X509_gmtime_adj(X509_get_notAfter(x509), 315360000L); // 10 years

        if (!X509_set_pubkey(x509, pkey)) {
            qCritical() << "Failed to set public key";
            break;
        }

        X509_NAME *name = X509_get_subject_name(x509);
        if (!name ||
            !X509_NAME_add_entry_by_txt(name, "C", MBSTRING_ASC, (const unsigned char *)m_szCountry.toUtf8().constData(), -1, -1, 0) ||
            !X509_NAME_add_entry_by_txt(name, "ST", MBSTRING_ASC, (const unsigned char *)m_szProvince.toUtf8().constData(), -1, -1, 0) ||
            !X509_NAME_add_entry_by_txt(name, "L", MBSTRING_ASC, (const unsigned char *)m_szCity.toUtf8().constData(), -1, -1, 0) ||
            !X509_NAME_add_entry_by_txt(name, "O", MBSTRING_ASC, (const unsigned char *)m_szOrganization.toUtf8().constData(), -1, -1, 0) ||
            !X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, (const unsigned char *)"OnMyDisk CA", -1, -1, 0)) {
            qCritical() << "Failed to set subject name";
            break;
        }

        if (!X509_set_issuer_name(x509, name)) {
            qCritical() << "Failed to set issuer name";
            break;
        }

        // Add extensions
        if (!addExt(x509, x509, NID_basic_constraints, "critical,CA:TRUE") ||
            !addExt(x509, x509, NID_key_usage, "critical,keyCertSign,cRLSign")) {
            qCritical() << "Failed to add extensions";
            break;
        }

        // Sign certificate
        if (!X509_sign(x509, pkey, EVP_sha256())) {
            qCritical() << "Failed to sign certificate";
            break;
        }

        // Save certificate
        BIO *bp_cert = BIO_new_file(m_caCertPath.toUtf8().constData(), "w");
        if (!bp_cert || !PEM_write_bio_X509(bp_cert, x509)) {
            qCritical() << "Failed to write certificate:" << m_caCertPath;
            BIO_free(bp_cert);
            break;
        }
        BIO_free(bp_cert);

        success = true;
    } while (0);

    BIO_free(bp_private);
    EVP_PKEY_free(pkey);
    X509_free(x509);

    if (success) {
        qInfo() << "PKI created";
        return loadPKI();
    }
    return false;
}

bool PKIHelper::signCSR(const QByteArray &csrBody, int serialNumber, const QString &userName, QByteArray &signedCertBody)
{
    BIO *reqbio = nullptr;
    BIO *cacert_bio = nullptr;
    BIO *pkey_bio = nullptr;
    X509_REQ *certreq = nullptr;
    X509 *newcert = nullptr;
    X509 *cacert = nullptr;
    EVP_PKEY *ca_privkey = nullptr;
    EVP_PKEY *req_pubkey = nullptr;
    ASN1_INTEGER *aserial = nullptr;
    bool success = false;

    do {
        // Load CSR
        reqbio = BIO_new_mem_buf(csrBody.constData(), csrBody.size());
        if (!reqbio) {
            qCritical() << "Unable to create CSR BIO";
            break;
        }

        certreq = PEM_read_bio_X509_REQ(reqbio, nullptr, nullptr, nullptr);
        if (!certreq) {
            qCritical() << "Unable to read CSR";
            break;
        }

        // Load CA certificate
        QByteArray caPemBytes = m_caCert.toPem();
        cacert_bio = BIO_new_mem_buf(caPemBytes.constData(), caPemBytes.size());
        if (!cacert_bio) {
            qCritical() << "Unable to create CA BIO";
            break;
        }

        cacert = PEM_read_bio_X509(cacert_bio, nullptr, nullptr, nullptr);
        if (!cacert) {
            qCritical() << "Unable to read CA certificate";
            break;
        }

        // Load CA private key
        pkey_bio = BIO_new_file(m_caKeyPath.toUtf8().constData(), "r");
        if (!pkey_bio) {
            qCritical() << "Unable to open CA private key file:" << m_caKeyPath;
            break;
        }

        ca_privkey = PEM_read_bio_PrivateKey(pkey_bio, nullptr, nullptr,
                                             m_caKeyPass.isEmpty() ? nullptr : (void *)m_caKeyPass.toLocal8Bit().data());
        if (!ca_privkey) {
            qCritical() << "Unable to read CA private key";
            break;
        }

        // Create new certificate
        newcert = X509_new();
        if (!newcert || !X509_set_version(newcert, 2)) {
            qCritical() << "Unable to create/set version for new X509 cert";
            break;
        }

        // Set serial number
        aserial = ASN1_INTEGER_new();
        if (!aserial || !ASN1_INTEGER_set(aserial, serialNumber) || !X509_set_serialNumber(newcert, aserial)) {
            qCritical() << "Unable to set serial number";
            break;
        }

        // Get subject name from CSR
        X509_NAME *name = X509_REQ_get_subject_name(certreq);
        if (!name) {
            qCritical() << "Unable to get subject name from CSR";
            break;
        }

        char buf[128];
        if (!X509_NAME_get_text_by_NID(name, NID_commonName, buf, sizeof(buf))) {
            qCritical() << "Unable to get commonName from CSR";
            break;
        }

        QString altName = QString("DNS:%1").arg(buf);

        // Add username to subject name
        if (!X509_NAME_add_entry_by_NID(name, NID_givenName, MBSTRING_UTF8,
                                        (unsigned char *)userName.toUtf8().constData(), -1, -1, 0)) {
            qCritical() << "Unable to set givenName";
            break;
        }

        if (!X509_set_subject_name(newcert, name)) {
            qCritical() << "Unable to set subject name";
            break;
        }

        // Set issuer name from CA
        name = X509_get_subject_name(cacert);
        if (!name || !X509_set_issuer_name(newcert, name)) {
            qCritical() << "Unable to set issuer name";
            break;
        }

        // Get public key from CSR
        req_pubkey = X509_REQ_get_pubkey(certreq);
        if (!req_pubkey || X509_REQ_verify(certreq, req_pubkey) != 1) {
            qCritical() << "Unable to verify/get CSR public key";
            break;
        }

        if (!X509_set_pubkey(newcert, req_pubkey)) {
            qCritical() << "Unable to set public key";
            break;
        }

        // Set validity period
        if (!X509_gmtime_adj(X509_get_notBefore(newcert), 0) ||
            !X509_gmtime_adj(X509_get_notAfter(newcert), 31536000L)) { // 1 year
            qCritical() << "Unable to set validity dates";
            break;
        }

        // Add extensions
        if (!addExt(cacert, newcert, NID_key_usage, "critical,digitalSignature,keyEncipherment,dataEncipherment") ||
            !addExt(cacert, newcert, NID_ext_key_usage, "critical,clientAuth,serverAuth") ||
            !addExt(cacert, newcert, NID_netscape_comment, "Generated by OnMyDisk PKI Server") ||
            !addExt(cacert, newcert, NID_subject_alt_name, altName.toLatin1().constData())) {
            qCritical() << "Failed to add extensions";
            break;
        }

        // Sign certificate
        if (!X509_sign(newcert, ca_privkey, EVP_sha256())) {
            qCritical() << "Unable to sign certificate";
            break;
        }

        // Write certificate to BIO
        BIO *outbio = BIO_new(BIO_s_mem());
        if (!outbio || !PEM_write_bio_X509(outbio, newcert)) {
            qCritical() << "Unable to write signed certificate to BIO";
            BIO_free(outbio);
            break;
        }

        char *data = nullptr;
        long size = BIO_get_mem_data(outbio, &data);
        if (size <= 0 || !data) {
            qCritical() << "Failed to extract certificate bytes";
            BIO_free(outbio);
            break;
        }

        signedCertBody = QByteArray(data, size);
        BIO_free(outbio);
        success = true;
        qInfo() << "Signed certificate for" << userName << "- OK";
    } while (0);

    ASN1_INTEGER_free(aserial);
    EVP_PKEY_free(req_pubkey);
    EVP_PKEY_free(ca_privkey);
    X509_REQ_free(certreq);
    X509_free(newcert);
    X509_free(cacert);
    BIO_free_all(reqbio);
    BIO_free_all(cacert_bio);
    BIO_free_all(pkey_bio);
    return success;
}

QSslCertificate PKIHelper::getCaCert()
{
    return m_caCert;
}

bool PKIHelper::loadPKI()
{
    BIO *cert_bio = BIO_new_file(m_caCertPath.toUtf8().constData(), "r");
    if (!cert_bio) {
        qCritical() << "Cannot open CA certificate:" << m_caCertPath;
        return false;
    }

    X509 *x509 = PEM_read_bio_X509(cert_bio, nullptr, nullptr, nullptr);
    BIO_free(cert_bio);
    if (!x509) {
        qCritical() << "Cannot read CA certificate:" << m_caCertPath;
        return false;
    }

    BIO *outbio = BIO_new(BIO_s_mem());
    if (!outbio || !PEM_write_bio_X509(outbio, x509)) {
        qCritical() << "Failed to convert CA certificate to PEM";
        BIO_free(outbio);
        X509_free(x509);
        return false;
    }

    char *data = nullptr;
    long size = BIO_get_mem_data(outbio, &data);
    if (size <= 0 || !data) {
        qCritical() << "Failed to extract CA certificate bytes";
        BIO_free(outbio);
        X509_free(x509);
        return false;
    }

    QByteArray certData(data, size);
    BIO_free(outbio);
    X509_free(x509);

    m_caCert = QSslCertificate(certData, QSsl::Pem);
    if (m_caCert.isNull()) {
        qCritical() << "Invalid CA certificate:" << m_caCertPath;
        return false;
    }

    BIO *key_bio = BIO_new_file(m_caKeyPath.toUtf8().constData(), "r");
    if (!key_bio) {
        qCritical() << "Cannot open CA key:" << m_caKeyPath;
        return false;
    }

    EVP_PKEY *pkey = PEM_read_bio_PrivateKey(key_bio, nullptr, nullptr,
                                             m_caKeyPass.isEmpty() ? nullptr : (void *)m_caKeyPass.toLocal8Bit().data());
    BIO_free(key_bio);
    if (!pkey) {
        qCritical() << "Cannot read CA key:" << m_caKeyPath;
        return false;
    }

    BIO *key_outbio = BIO_new(BIO_s_mem());
    if (!key_outbio || !PEM_write_bio_PrivateKey(key_outbio, pkey, nullptr, nullptr, 0, nullptr, nullptr)) {
        qCritical() << "Failed to convert CA key to PEM";
        BIO_free(key_outbio);
        EVP_PKEY_free(pkey);
        return false;
    }

    data = nullptr;
    size = BIO_get_mem_data(key_outbio, &data);
    if (size <= 0 || !data) {
        qCritical() << "Failed to extract CA key bytes";
        BIO_free(key_outbio);
        EVP_PKEY_free(pkey);
        return false;
    }

    QByteArray keyData(data, size);
    BIO_free(key_outbio);
    EVP_PKEY_free(pkey);

    m_caPKey = QSslKey(keyData, QSsl::Rsa, QSsl::Pem, QSsl::PrivateKey);
    if (m_caPKey.isNull()) {
        qCritical() << "Invalid CA key:" << m_caKeyPath;
        return false;
    }

    return true;
}
