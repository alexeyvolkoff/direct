#include "tunnelcrypto.h"

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/err.h>

namespace TunnelCrypto
{
    static const int kKeyLen = 32;
    static const int kNonceLen = 12;
    static const int kTagLen = 16;

    bool generateKeyPair(QByteArray &priv, QByteArray &pub)
    {
        EVP_PKEY_CTX *pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_X25519, nullptr);
        if (!pctx)
            return false;
        if (EVP_PKEY_keygen_init(pctx) <= 0)
        {
            EVP_PKEY_CTX_free(pctx);
            return false;
        }
        EVP_PKEY *pkey = nullptr;
        if (EVP_PKEY_keygen(pctx, &pkey) <= 0)
        {
            EVP_PKEY_CTX_free(pctx);
            return false;
        }
        EVP_PKEY_CTX_free(pctx);

        pub.resize(kKeyLen);
        priv.resize(kKeyLen);
        size_t pubLen = kKeyLen, privLen = kKeyLen;
        bool ok = EVP_PKEY_get_raw_public_key(pkey, reinterpret_cast<unsigned char *>(pub.data()), &pubLen) == 1
                  && EVP_PKEY_get_raw_private_key(pkey, reinterpret_cast<unsigned char *>(priv.data()), &privLen) == 1;
        EVP_PKEY_free(pkey);
        if (!ok)
            return false;
        pub.resize(static_cast<int>(pubLen));
        priv.resize(static_cast<int>(privLen));
        return true;
    }

    QByteArray computeSharedSecret(const QByteArray &priv, const QByteArray &peerPub)
    {
        if (priv.size() != kKeyLen || peerPub.size() != kKeyLen)
            return QByteArray();

        EVP_PKEY *pkey = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr,
                                                      reinterpret_cast<const unsigned char *>(priv.constData()), priv.size());
        if (!pkey)
            return QByteArray();

        EVP_PKEY *peerPkey = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, nullptr,
                                                         reinterpret_cast<const unsigned char *>(peerPub.constData()), peerPub.size());
        if (!peerPkey)
        {
            EVP_PKEY_free(pkey);
            return QByteArray();
        }

        EVP_PKEY_CTX *pctx = EVP_PKEY_CTX_new(pkey, nullptr);
        if (!pctx)
        {
            EVP_PKEY_free(peerPkey);
            EVP_PKEY_free(pkey);
            return QByteArray();
        }
        QByteArray secret;
        if (EVP_PKEY_derive_init(pctx) <= 0
            || EVP_PKEY_derive_set_peer(pctx, peerPkey) <= 0)
        {
            EVP_PKEY_CTX_free(pctx);
            EVP_PKEY_free(peerPkey);
            EVP_PKEY_free(pkey);
            return QByteArray();
        }
        EVP_PKEY_free(peerPkey);
        size_t secretLen = 0;
        if (EVP_PKEY_derive(pctx, nullptr, &secretLen) <= 0 || secretLen == 0)
        {
            EVP_PKEY_CTX_free(pctx);
            EVP_PKEY_free(pkey);
            return QByteArray();
        }
        secret.resize(static_cast<int>(secretLen));
        if (EVP_PKEY_derive(pctx, reinterpret_cast<unsigned char *>(secret.data()), &secretLen) <= 0)
        {
            EVP_PKEY_CTX_free(pctx);
            EVP_PKEY_free(pkey);
            return QByteArray();
        }
        EVP_PKEY_CTX_free(pctx);
        EVP_PKEY_free(pkey);
        secret.resize(static_cast<int>(secretLen));
        return secret;
    }

    QByteArray deriveKey(const QByteArray &shared)
    {
        EVP_MD_CTX *mdctx = EVP_MD_CTX_new();
        if (!mdctx)
            return QByteArray();
        QByteArray key(kKeyLen, Qt::Uninitialized);
        unsigned int keyLen = 0;
        bool ok = EVP_DigestInit_ex(mdctx, EVP_sha256(), nullptr) == 1
                  && EVP_DigestUpdate(mdctx, shared.constData(), static_cast<size_t>(shared.size())) == 1
                  && EVP_DigestFinal_ex(mdctx, reinterpret_cast<unsigned char *>(key.data()), &keyLen) == 1;
        EVP_MD_CTX_free(mdctx);
        if (!ok || keyLen != static_cast<unsigned int>(kKeyLen))
            return QByteArray();
        key.resize(kKeyLen);
        return key;
    }

    QByteArray encryptPacket(const QByteArray &key, const QByteArray &plain)
    {
        if (key.size() != kKeyLen || plain.isEmpty())
            return QByteArray();

        QByteArray nonce(kNonceLen, Qt::Uninitialized);
        if (RAND_bytes(reinterpret_cast<unsigned char *>(nonce.data()), kNonceLen) != 1)
            return QByteArray();

        EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
        if (!ctx)
            return QByteArray();

        QByteArray out(plain.size() + kTagLen, Qt::Uninitialized);
        int outLen = 0, tmpLen = 0;
        bool ok = EVP_EncryptInit_ex(ctx, EVP_chacha20_poly1305(), nullptr, nullptr, nullptr) == 1
                  && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, kNonceLen, nullptr) == 1
                  && EVP_EncryptInit_ex(ctx, nullptr, nullptr,
                                        reinterpret_cast<const unsigned char *>(key.constData()),
                                        reinterpret_cast<const unsigned char *>(nonce.constData())) == 1
                  && EVP_EncryptUpdate(ctx, reinterpret_cast<unsigned char *>(out.data()),
                                       &outLen,
                                       reinterpret_cast<const unsigned char *>(plain.constData()), plain.size()) == 1
                  && EVP_EncryptFinal_ex(ctx,
                                         reinterpret_cast<unsigned char *>(out.data()) + outLen, &tmpLen) == 1;
        if (!ok)
        {
            EVP_CIPHER_CTX_free(ctx);
            return QByteArray();
        }
        outLen += tmpLen;
        unsigned char tag[kTagLen];
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG, kTagLen, tag) != 1)
        {
            EVP_CIPHER_CTX_free(ctx);
            return QByteArray();
        }
        EVP_CIPHER_CTX_free(ctx);
        out.resize(outLen + kTagLen);
        memcpy(out.data() + outLen, tag, kTagLen);
        out.prepend(nonce);
        return out;
    }

    bool decryptPacket(const QByteArray &key, const QByteArray &packet, QByteArray &plain)
    {
        if (key.size() != kKeyLen || packet.size() < kNonceLen + kTagLen)
            return false;

        const QByteArray nonce = packet.left(kNonceLen);
        const QByteArray cipher = packet.mid(kNonceLen, packet.size() - kNonceLen - kTagLen);
        const unsigned char *tag = reinterpret_cast<const unsigned char *>(packet.constData()) + packet.size() - kTagLen;

        EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
        if (!ctx)
            return false;

        plain.resize(cipher.size());
        int outLen = 0, tmpLen = 0;
        bool ok = EVP_DecryptInit_ex(ctx, EVP_chacha20_poly1305(), nullptr, nullptr, nullptr) == 1
                  && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, kNonceLen, nullptr) == 1
                  && EVP_DecryptInit_ex(ctx, nullptr, nullptr,
                                        reinterpret_cast<const unsigned char *>(key.constData()),
                                        reinterpret_cast<const unsigned char *>(nonce.constData())) == 1
                  && EVP_DecryptUpdate(ctx, reinterpret_cast<unsigned char *>(plain.data()),
                                       &outLen,
                                       reinterpret_cast<const unsigned char *>(cipher.constData()), cipher.size()) == 1
                  && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_TAG, kTagLen, const_cast<unsigned char *>(tag)) == 1
                  && EVP_DecryptFinal_ex(ctx,
                                         reinterpret_cast<unsigned char *>(plain.data()) + outLen, &tmpLen) == 1;
        EVP_CIPHER_CTX_free(ctx);
        if (!ok)
        {
            plain.clear();
            return false;
        }
        plain.resize(outLen + tmpLen);
        return true;
    }
}
