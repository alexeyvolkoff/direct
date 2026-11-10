#include "sslhelper.h"
#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/opensslv.h>
#include <openssl/crypto.h>

#pragma message("OPENSSL_VERSION: " OPENSSL_VERSION_TEXT)

SslHelper::SslHelper(QObject *parent) : QObject(parent)
{
#if OPENSSL_VERSION_NUMBER < 0x10100000L // OpenSSL < 1.1.0
    // OpenSSL 1.0.x: Manual initialization
    OpenSSL_add_all_algorithms();
    ERR_load_BIO_strings();
    ERR_load_crypto_strings();
#elif OPENSSL_VERSION_NUMBER < 0x30000000L // OpenSSL 1.1.0 - 1.1.1
    // OpenSSL 1.1.x: Optional error string loading (usually automatic)
    ERR_load_BIO_strings();
    ERR_load_crypto_strings();
#else // OpenSSL 3.0+
    // OpenSSL 3.0: Initialize with required components, error strings loaded automatically
    OPENSSL_init_crypto(OPENSSL_INIT_LOAD_CRYPTO_STRINGS |
                        OPENSSL_INIT_ADD_ALL_CIPHERS |
                        OPENSSL_INIT_ADD_ALL_DIGESTS, nullptr);
#endif
}

bool SslHelper::genPrivateKeyToFile(int length, const QString &keyFilePath, const char *keyPass)
{
    QByteArray keyBytes;
    if (!genPrivateKeyToBytes(length, keyPass, keyBytes) )
    {
        qCritical() << "Unable to generate private key!";
        return false;
    }

    if (keyBytes.isEmpty())
    {
        qCritical() << "Could not generate key" << keyFilePath;
        return false;
    }


    QFile privateKeyFile(keyFilePath);
    if (!privateKeyFile.open(QIODevice::WriteOnly))
    {
        qCritical() << "Unable to save private key to file!" << keyFilePath;
        return false;
    }


    privateKeyFile.write(keyBytes);
    privateKeyFile.close();

    return true;
}

bool SslHelper::genPrivateKeyToBytes(int length, const char *keyPass, QByteArray &privateKey) {
    EVP_PKEY *pkey = nullptr;
    BIO *bp_private = nullptr;
#if OPENSSL_VERSION_NUMBER < 0x30000000L
    RSA *rsa = nullptr;
    BIGNUM *bne = nullptr;
#endif
    bool success = false;

    qDebug() << "Generating private key with length:" << length << "Passphrase provided:" << (keyPass ? "yes" : "no");
    if (keyPass) {
        qDebug() << "Passphrase length:" << strlen(keyPass);
    }

    do {
        // Generate RSA key
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
        pkey = EVP_RSA_gen(length);
        if (!pkey) {
            qCritical() << "Failed to generate RSA key with EVP_RSA_gen:" << ERR_error_string(ERR_get_error(), nullptr);
            break;
        }
#else
        bne = BN_new();
        if (!bne || BN_set_word(bne, RSA_F4) != 1) {
            qCritical() << "Failed to initialize BIGNUM:" << ERR_error_string(ERR_get_error(), nullptr);
            break;
        }

        rsa = RSA_new();
        if (!rsa || RSA_generate_key_ex(rsa, length, bne, nullptr) != 1) {
            qCritical() << "Failed to generate RSA key:" << ERR_error_string(ERR_get_error(), nullptr);
            break;
        }

        pkey = EVP_PKEY_new();
        if (!pkey || EVP_PKEY_assign_RSA(pkey, rsa) != 1) {
            qCritical() << "Failed to assign RSA key to EVP_PKEY:" << ERR_error_string(ERR_get_error(), nullptr);
            break;
        }
        rsa = nullptr; // Owned by pkey
#endif

        // Write private key to BIO
        bp_private = BIO_new(BIO_s_mem());
        if (!bp_private) {
            qCritical() << "Failed to create BIO:" << ERR_error_string(ERR_get_error(), nullptr);
            break;
        }

        if (keyPass) {
            if (!PEM_write_bio_PrivateKey(bp_private, pkey, EVP_aes_256_cbc(),
                                          (unsigned char *)keyPass, strlen(keyPass), nullptr, nullptr)) {
                qCritical() << "Failed to write private key with passphrase:" << ERR_error_string(ERR_get_error(), nullptr);
                break;
            }
        } else {
            if (!PEM_write_bio_PrivateKey(bp_private, pkey, nullptr, nullptr, 0, nullptr, nullptr)) {
                qCritical() << "Failed to write private key:" << ERR_error_string(ERR_get_error(), nullptr);
                break;
            }
        }

        // Extract bytes
        char *data = nullptr;
        long size = BIO_get_mem_data(bp_private, &data);
        if (size <= 0 || !data) {
            qCritical() << "Failed to extract private key bytes:" << ERR_error_string(ERR_get_error(), nullptr);
            break;
        }

        privateKey.clear();
        privateKey.append(data, size);
        success = true;
    } while (0);

    // Cleanup
    BIO_free(bp_private);
    EVP_PKEY_free(pkey);
#if OPENSSL_VERSION_NUMBER < 0x30000000L
    RSA_free(rsa);
    BN_free(bne);
#endif
    qDebug() << "Key generation" << (success ? "succeeded" : "failed");
    return success;
}

bool SslHelper::genCSRToFile(const QString &csrFilePath, const QString &keyFilePath, const char *keyPass, const QString &commonName) {
    bool isOk = false;
    QByteArray csrBytes = genCSRToBytes(isOk, keyFilePath, keyPass, commonName);
    if (!isOk) {
        qCritical() << "Unable to generate CSR!";
        return false;
    }

    QFile csrFile(csrFilePath);
    if (!csrFile.open(QIODevice::WriteOnly)) {
        qCritical() << "Unable to save CSR to file!" << csrFilePath;
        return false;
    }

    csrFile.write(csrBytes);
    csrFile.close();
    return true;
}


QByteArray SslHelper::genCSRToBytes(bool &isOk, const QString &keyFilePath, const char *keyPass, const QString &commonName) {
    QByteArray csrBytes;
    EVP_PKEY *pkey = nullptr;
    X509_REQ *x509_req = nullptr;
    X509_NAME *x509_name = nullptr;
    BIO *key_bio = nullptr;
    BIO *bio = nullptr;
    isOk = false;

    do {
        // Load private key
        key_bio = BIO_new_file(keyFilePath.toUtf8().constData(), "r");
        if (!key_bio) {
            qCritical() << "Failed to open private key file:" << keyFilePath;
            break;
        }

        pkey = PEM_read_bio_PrivateKey(key_bio, nullptr, nullptr, keyPass ? (void *)keyPass : nullptr);
        if (!pkey) {
            qCritical() << "Failed to read private key";
            break;
        }

        // Create X509 request
        x509_req = X509_REQ_new();
        if (!x509_req) {
            qCritical() << "Failed to create X509_REQ";
            break;
        }

        if (X509_REQ_set_version(x509_req, 0) != 1) {
            qCritical() << "Failed to set X509_REQ version";
            break;
        }

        // Set subject name
        x509_name = const_cast<X509_NAME *>(X509_REQ_get_subject_name(x509_req));
        if (!x509_name) {
            qCritical() << "Failed to get subject name";
            break;
        }

        if (X509_NAME_add_entry_by_txt(x509_name, "CN", MBSTRING_ASC,
                                       (const unsigned char *)commonName.toUtf8().constData(), -1, -1, 0) != 1) {
            qCritical() << "Failed to set common name";
            break;
        }

        // Set public key
        if (X509_REQ_set_pubkey(x509_req, pkey) != 1) {
            qCritical() << "Failed to set public key";
            break;
        }

        // Sign the CSR
        if (X509_REQ_sign(x509_req, pkey, EVP_sha256()) <= 0) {
            qCritical() << "Failed to sign CSR";
            break;
        }

        // Write CSR to BIO
        bio = BIO_new(BIO_s_mem());
        if (!bio) {
            qCritical() << "Failed to create BIO";
            break;
        }

        if (PEM_write_bio_X509_REQ(bio, x509_req) != 1) {
            qCritical() << "Failed to write CSR";
            break;
        }

        // Extract bytes
        char *data = nullptr;
        long size = BIO_get_mem_data(bio, &data);
        if (size <= 0 || !data) {
            qCritical() << "Failed to extract CSR bytes";
            break;
        }

        csrBytes.append(data, size);
        isOk = true;
    } while (0);

    // Cleanup
    BIO_free(bio);
    BIO_free(key_bio);
    X509_REQ_free(x509_req);
    EVP_PKEY_free(pkey);
    return csrBytes;
}

bool SslHelper::genSelfSignedCert(int length, const QString &keyFilePath, const char *keyPass, const QString &commonName, const QString &certFilePath, const QString &organization) {
    EVP_PKEY *pkey = nullptr;
    X509 *x509 = nullptr;
    X509_NAME *name = nullptr;
    BIO *key_bio = nullptr;
    BIO *bio = nullptr;
    bool success = false;

    key_bio = BIO_new_file(keyFilePath.toUtf8().constData(), "r");
    if (key_bio) {
        pkey = PEM_read_bio_PrivateKey(key_bio, nullptr, nullptr, keyPass ? (void *)keyPass : nullptr);
        BIO_free(key_bio);
        key_bio = nullptr;
    }

    if (!pkey) {
        if (!genPrivateKeyToFile(length, keyFilePath, keyPass)) {
            qCritical() << "Failed to generate private key for self-signed cert";
            return false;
        }
        key_bio = BIO_new_file(keyFilePath.toUtf8().constData(), "r");
        if (!key_bio) {
            qCritical() << "Failed to open private key file:" << keyFilePath;
            return false;
        }
        pkey = PEM_read_bio_PrivateKey(key_bio, nullptr, nullptr, keyPass ? (void *)keyPass : nullptr);
        BIO_free(key_bio);
        key_bio = nullptr;
        if (!pkey) {
            qCritical() << "Failed to read generated private key";
            return false;
        }
    }

    do {
        x509 = X509_new();
        if (!x509) {
            qCritical() << "Failed to create X509";
            break;
        }

        if (X509_set_version(x509, 2) != 1) { // X509v3
            qCritical() << "Failed to set X509 version";
            break;
        }

        ASN1_INTEGER_set(X509_get_serialNumber(x509), 1);

        X509_gmtime_adj(X509_get_notBefore(x509), -60);
        X509_gmtime_adj(X509_get_notAfter(x509), 60L * 60 * 24 * 365 * 10);

        name = const_cast<X509_NAME *>(X509_get_subject_name(x509));
        if (!name) {
            qCritical() << "Failed to get subject name";
            break;
        }

        if (X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                       (const unsigned char *)commonName.toUtf8().constData(), -1, -1, 0) != 1) {
            qCritical() << "Failed to set common name";
            break;
        }

        if (!organization.isEmpty()) {
            if (X509_NAME_add_entry_by_txt(name, "O", MBSTRING_ASC,
                                           (const unsigned char *)organization.toUtf8().constData(), -1, -1, 0) != 1) {
                qCritical() << "Failed to set organization";
                break;
            }
        }

        //self-signed: issuer == subject
        if (X509_set_issuer_name(x509, name) != 1) {
            qCritical() << "Failed to set issuer name";
            break;
        }

        if (X509_set_pubkey(x509, pkey) != 1) {
            qCritical() << "Failed to set public key";
            break;
        }

        if (X509_sign(x509, pkey, EVP_sha256()) <= 0) {
            qCritical() << "Failed to sign certificate";
            break;
        }

        bio = BIO_new(BIO_s_mem());
        if (!bio) {
            qCritical() << "Failed to create BIO";
            break;
        }

        if (PEM_write_bio_X509(bio, x509) != 1) {
            qCritical() << "Failed to write certificate";
            break;
        }

        char *data = nullptr;
        long size = BIO_get_mem_data(bio, &data);
        if (size <= 0 || !data) {
            qCritical() << "Failed to extract certificate bytes";
            break;
        }

        QFile certFile(certFilePath);
        if (!certFile.open(QIODevice::WriteOnly)) {
            qCritical() << "Unable to save certificate to file:" << certFilePath;
            break;
        }

        certFile.write(data, size);
        certFile.close();
        success = true;
    } while (0);

    BIO_free(bio);
    X509_free(x509);
    EVP_PKEY_free(pkey);
    return success;
}

SslHelper::~SslHelper() {
#ifdef OPENSSL_NO_INIT
    // OpenSSL 1.0.x: Manual cleanup
    CRYPTO_cleanup_all_ex_data();
    ERR_free_strings();
    EVP_cleanup();
#else
    // OpenSSL 1.1.1 and 3.0: No explicit cleanup needed
#endif
}

