#ifndef TUNNELCRYPTO_H
#define TUNNELCRYPTO_H

#include <QByteArray>

// E2EE for omd-tunnel: X25519 ephemeral handshake + ChaCha20-Poly1305.
// The tunnel payload format is: nonce(12) || ciphertext || tag(16).
namespace TunnelCrypto
{
    bool generateKeyPair(QByteArray &priv, QByteArray &pub);
    QByteArray computeSharedSecret(const QByteArray &priv, const QByteArray &peerPub);
    QByteArray deriveKey(const QByteArray &shared);
    QByteArray encryptPacket(const QByteArray &key, const QByteArray &plain);
    bool decryptPacket(const QByteArray &key, const QByteArray &packet, QByteArray &plain);
}

#endif // TUNNELCRYPTO_H
