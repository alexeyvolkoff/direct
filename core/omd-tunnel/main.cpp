#include <QCoreApplication>
#include <QCommandLineParser>
#include <QSettings>
#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QRandomGenerator>
#include <QHostAddress>
#include <QSslSocket>
#include <QSslConfiguration>
#include <QSslCertificate>
#include <QSslKey>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QThread>
#include <QMetaObject>
#include <QUuid>
#include <QMessageLogContext>
#include <QHash>
#include <QList>

#include <cstdio>
#include <cstring>
#include <cerrno>
#include <functional>
#include <unistd.h>

#include "linkmessage.h"
#include "sslhelper.h"
#include "tunnelcrypto.h"
#ifdef P2P_ENABLED
#include "p2ptunnel.h"
#endif

static void messageHandler(QtMsgType, const QMessageLogContext &, const QString &msg)
{
    fprintf(stderr, "%s\n", msg.toUtf8().constData());
}

// TURN server co-located with the gateway: same host the client connects to,
// standard port 3478. Accepts a bare host or a URL.
static QString turnUrlFor(const QString &server)
{
    QString host = server;
    if (host.startsWith(QStringLiteral("https://")))
        host.remove(0, 8);
    else if (host.startsWith(QStringLiteral("http://")))
        host.remove(0, 7);
    int slash = host.indexOf(QLatin1Char('/'));
    if (slash > 0)
        host.truncate(slash);
    int colon = host.lastIndexOf(QLatin1Char(':'));
    if (colon > 0 && !host.mid(colon + 1).contains(QLatin1Char(':')))
        host.truncate(colon);
    return QStringLiteral("turn:%1:3478").arg(host);
}

static QString profileDirForArgs(const QCommandLineParser &parser)
{
    if (parser.isSet("dir"))
        return parser.value("dir");

    return QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) + QStringLiteral("/omd-tunnel");
}

static QString randomKey(int length)
{
    const QString chars = QStringLiteral("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789");
    QString key;
    for (int i = 0; i < length; ++i)
        key.append(chars.at(QRandomGenerator::global()->bounded(chars.length())));
    return key;
}

static QString readCertCommonName(const QString &certPath)
{
    QFile certFile(certPath);
    if (!certFile.open(QIODevice::ReadOnly))
        return QString();
    QList<QSslCertificate> certs = QSslCertificate::fromDevice(&certFile);
    certFile.close();
    if (certs.isEmpty() || certs.at(0).isNull())
        return QString();
    const QStringList names = certs.at(0).subjectInfo(QSslCertificate::CommonName);
    return names.isEmpty() ? QString() : names.at(0);
}

static QString formatCandidateInfo(const QByteArray &payload)
{
    const QString candStr = QString::fromUtf8(payload).trimmed();
    const QStringList parts = candStr.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (parts.size() >= 8 && parts.at(0).startsWith(QLatin1String("candidate:")))
    {
        const QString ip = parts.at(4);
        const QString port = parts.at(5);
        const QString proto = parts.at(2);
        const QString type = parts.at(7);
        return QStringLiteral("%1:%2 (%3 %4)").arg(ip, port, proto, type);
    }
    return candStr;
}

static bool loadOrCreateDeviceProfile(const QString &dir, const QString &user, QString &deviceId, QString &keyPath, QString &certPath)
{
    QDir d(dir);
    if (!d.exists() && !d.mkpath(dir))
    {
        fprintf(stderr, "omd-tunnel: can not create profile dir %s\n", qPrintable(dir));
        return false;
    }

    deviceId.clear();
    const QString idFile = dir + QStringLiteral("/device.id");
    QFile idF(idFile);
    if (idF.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        deviceId = QString::fromUtf8(idF.readAll()).trimmed();
        idF.close();
    }
    if (deviceId.isEmpty())
    {
        deviceId = randomKey(10);
        idF.open(QIODevice::WriteOnly | QIODevice::Text);
        idF.write((deviceId + "\n").toUtf8());
        idF.close();
    }

    keyPath = dir + QStringLiteral("/client.key");
    certPath = dir + QStringLiteral("/client.crt");

    if (QFile::exists(certPath) && QFile::exists(keyPath))
        return true;

    fprintf(stderr, "omd-tunnel: generating self-signed device certificate for \"%s\" in %s\n", qPrintable(user), qPrintable(dir));
    SslHelper ssl;
    return ssl.genSelfSignedCert(2048, keyPath, deviceId.toLatin1().constData(), user, certPath, QStringLiteral("omd-tunnel"));
}

static bool writeLinkMessage(QSslSocket *socket, const LinkMessage &message)
{
    QByteArray buffer;
    QDataStream out(&buffer, QIODevice::WriteOnly);
    out << quint64(0);
    out << message;
    out.device()->seek(0);
    out << quint64(buffer.size());
    return socket->write(buffer) != -1;
}

static bool readLinkMessage(QByteArray &buffer, quint64 &blockSize, LinkMessage &message)
{
    if (blockSize == 0 && (quint64)buffer.size() >= sizeof(quint64))
    {
        QDataStream s(&buffer, QIODevice::ReadOnly);
        s >> blockSize;
    }

    if (blockSize == 0 || (quint64)buffer.size() < blockSize)
        return false;

    QDataStream s(&buffer, QIODevice::ReadOnly);
    s.device()->seek(sizeof(quint64));
    s >> message;

    buffer.remove(0, blockSize);
    blockSize = 0;
    return true;
}

struct Tunnel
{
    QString id;
    QTcpSocket *tcp = nullptr;
    QByteArray pending;
    bool ready = false;
    bool openSent = false;
    QByteArray e2eePriv;
    QByteArray e2eePub;
    QByteArray key;
#ifdef P2P_ENABLED
    P2PTunnel *p2p = nullptr;
#endif
};

int main(int argc, char *argv[])
{
    setbuf(stderr, NULL);
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName("Bineon");
    QCoreApplication::setApplicationName("omd-tunnel");
    QCoreApplication::setApplicationVersion("1.0");
    qInstallMessageHandler(messageHandler);

    QCommandLineParser parser;
    parser.setApplicationDescription("omd-tunnel - TCP tunnel over OMD gateway, usable as ssh ProxyCommand.\nNo OMD profile required: a self-signed device identity is created in the config dir on first run.");
    parser.addHelpOption();
    parser.addOption(QCommandLineOption("server", "OMD gateway host (default from config, then OMD_TUNNEL_SERVER)", "host"));
    parser.addOption(QCommandLineOption("port", "OMD gateway TLS port (default from config, then OMD_TUNNEL_PORT, then 9000)", "port"));
    parser.addOption(QCommandLineOption("user", "certificate common name / user (default omd-tunnel)", "user"));
    parser.addOption(QCommandLineOption("dir", "directory for device identity (default ~/.config/omd-tunnel)", "dir"));
    parser.addOption(QCommandLineOption("host", "remote host on the target device (default 127.0.0.1)", "host"));
    parser.addOption(QCommandLineOption("timeout", "connection timeout in seconds (default 30)", "sec"));
    parser.addOption(QCommandLineOption("listen", "listen for local TCP connections on this port instead of using stdin/stdout (recommended on Windows)", "port"));
    parser.addOption(QCommandLineOption("debug", "print per-packet tunnel traffic diagnostics"));
    parser.addOption(QCommandLineOption("no-e2ee", "disable end-to-end encryption of tunnel payloads"));
    parser.addOption(QCommandLineOption("no-p2p", "disable peer-to-peer (WebRTC) tunnel establishment"));
    parser.addPositionalArgument("linkID", "LinkID of the target device");
    parser.addPositionalArgument("port", "TCP port on the target device (default 22)");
    parser.process(app);

    const QStringList positional = parser.positionalArguments();
    if (positional.size() < 1)
        parser.showHelp(1);

    const QString targetLinkID = positional.at(0);
    const quint16 targetPort = positional.size() > 1 ? static_cast<quint16>(positional.at(1).toUShort()) : 22;
    const QString remoteHost = parser.isSet("host") ? parser.value("host") : QStringLiteral("127.0.0.1");
    const bool listenMode = parser.isSet("listen");
    const quint16 listenPort = listenMode ? static_cast<quint16>(parser.value("listen").toUShort()) : 0;
    const bool debug = parser.isSet("debug");
    const bool e2ee = !parser.isSet("no-e2ee");
#ifdef P2P_ENABLED
    const bool useP2P = !parser.isSet("no-p2p");
#else
    const bool useP2P = false;
#endif

    const QString configPath = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) + QStringLiteral("/omd-tunnel/omd-tunnel.conf");
    QSettings settings(configPath, QSettings::IniFormat);

    QString server = settings.value("Network/host", QString()).toString();
    if (server.isEmpty())
        server = qgetenv("OMD_TUNNEL_SERVER");
    if (server.isEmpty()) {
#ifdef P2P_ENABLED
        server = QStringLiteral("disk.direct");
#else
        server = QStringLiteral("disk.direct");
#endif
    }
    if (parser.isSet("server"))
        server = parser.value("server");

    int serverPort = settings.value("Network/port", 0).toInt();
    if (serverPort == 0)
        serverPort = qgetenv("OMD_TUNNEL_PORT").toInt();
    if (serverPort == 0) {
#ifdef P2P_ENABLED
        serverPort = (server == QLatin1String("onmydisk.net")) ? 9000 : 9010;
#else
        serverPort = 9000;
#endif
    }
    if (parser.isSet("port"))
        serverPort = parser.value("port").toInt();

    const QString user = parser.isSet("user") ? parser.value("user") : settings.value("Network/user", QStringLiteral("omd-tunnel")).toString();
    const int timeoutSec = parser.isSet("timeout") ? parser.value("timeout").toInt() : settings.value("Network/timeout", 30).toInt();

    QString deviceId, keyPath, certPath;
    if (!loadOrCreateDeviceProfile(profileDirForArgs(parser), user, deviceId, keyPath, certPath))
        return 1;

    QString effectiveUser = user;
    if (QFile::exists(certPath))
    {
        const QString certUser = readCertCommonName(certPath);
        if (!certUser.isEmpty())
            effectiveUser = certUser;
    }

    QSslSocket *socket = nullptr;
    QByteArray buffer;
    quint64 blockSize = 0;
    QByteArray pendingStdin;
    QHash<QString, Tunnel *> tunnels;

    QTcpServer tcpServer;

    const auto sendTunnelOpen = [&](Tunnel *t) {
        if (!socket || !t)
            return;
        if (t->openSent)
            return;
        t->openSent = true;
        LinkMessage open;
        open.setMessageType(LinkMessage::TunnelOpen);
        open.setResultCode(LinkMessage::Request);
        open.insertDataValue("ID", t->id);
        open.insertDataValue("linkID", targetLinkID);
        open.insertDataValue("host", remoteHost);
        open.insertDataValue("port", targetPort);
        if (e2ee && !t->e2eePub.isEmpty())
            open.insertDataValue("e2eePub", t->e2eePub);
        if (debug)
            fprintf(stderr, "omd-tunnel: -> TunnelOpen %s for %s:%d\n", qPrintable(t->id), qPrintable(targetLinkID), targetPort);
        bool ok = writeLinkMessage(socket, open);
        if (!ok && debug)
            fprintf(stderr, "omd-tunnel: writeLinkMessage failed!\n");
    };

    const auto sendTunnelData = [&](Tunnel *t, const QByteArray &data) {
        if (!socket || !t)
            return;
#ifdef P2P_ENABLED
        if (t->p2p && t->p2p->isOpen())
        {
            t->p2p->sendData(data);
            return;
        }
#endif
        QByteArray payload = data;
        if (!t->key.isEmpty())
        {
            payload = TunnelCrypto::encryptPacket(t->key, data);
            if (payload.isEmpty())
            {
                fprintf(stderr, "omd-tunnel: encryption failed for %s, dropping %d bytes\n",
                        qPrintable(t->id), data.size());
                return;
            }
        }
        LinkMessage chunk;
        chunk.setMessageType(LinkMessage::TunnelData);
        chunk.setResultCode(LinkMessage::OK);
        chunk.insertDataValue("ID", t->id);
        chunk.setPayLoad(payload);
        writeLinkMessage(socket, chunk);
    };

    // Start a WebRTC DataChannel for a tunnel; signaling goes through the
    // gateway as TunnelSignaling messages. Once the DataChannel is open the
    // tunnel traffic is switched to the direct P2P path (see sendTunnelData).
#ifdef P2P_ENABLED
    std::function<void(Tunnel*)> setupP2P;
    setupP2P = [&](Tunnel *t) {
        if (!useP2P || !t || t->p2p)
            return;
        P2PTunnel *p2p = new P2PTunnel(P2PTunnel::Role::Offerer, &app);
        t->p2p = p2p;
        QObject::connect(p2p, &P2PTunnel::signalReady, &app, [&, p2p, id = t->id](const QString &kind, const QByteArray &payload) {
            Tunnel *tt = tunnels.value(id, nullptr);
            if (!tt || !socket)
                return;
            if (debug)
            {
                if (kind == QLatin1String("candidate"))
                    fprintf(stderr, "omd-tunnel: -> p2p candidate for %s: %s\n", qPrintable(id), qPrintable(formatCandidateInfo(payload)));
                else
                    fprintf(stderr, "omd-tunnel: -> p2p signaling %s for %s\n", qPrintable(kind), qPrintable(id));
            }
            LinkMessage sig;
            sig.setMessageType(LinkMessage::TunnelSignaling);
            sig.setResultCode(LinkMessage::Request);
            sig.insertDataValue("ID", id);
            sig.insertDataValue("kind", kind);
            sig.setPayLoad(payload);
            writeLinkMessage(socket, sig);
        });
        QObject::connect(p2p, &P2PTunnel::dataReceived, &app, [&, id = t->id](const QByteArray &data) {
            if (data.isEmpty())
                return;
            Tunnel *tt = tunnels.value(id, nullptr);
            if (!tt)
                return;
            if (tt->tcp)
                tt->tcp->write(data);
            else
            {
                ssize_t written = ::write(STDOUT_FILENO, data.constData(), static_cast<size_t>(data.size()));
                Q_UNUSED(written)
            }
        });
        QObject::connect(p2p, &P2PTunnel::opened, &app, [&, id = t->id](const QString &pairInfo) {
            if (!pairInfo.isEmpty())
                fprintf(stderr, "omd-tunnel: P2P established for %s via %s\n", qPrintable(id), qPrintable(pairInfo));
            else
                fprintf(stderr, "omd-tunnel: P2P established for %s\n", qPrintable(id));
        });
        QObject::connect(p2p, &P2PTunnel::closed, &app, [&, id = t->id]() {
            fprintf(stderr, "omd-tunnel: P2P closed for %s, falling back to relay\n", qPrintable(id));
            QTimer::singleShot(5000, &app, [&, id]() {
                Tunnel *tt = tunnels.value(id, nullptr);
                if (tt && (tt->tcp || !listenMode) && (!tt->p2p || !tt->p2p->isOpen()))
                {
                    if (tt->p2p) {
                        tt->p2p->close();
                        tt->p2p->deleteLater();
                        tt->p2p = nullptr;
                    }
                    if (debug)
                        fprintf(stderr, "omd-tunnel: retrying P2P connection for %s...\n", qPrintable(id));
                    setupP2P(tt);
                }
            });
        });
        QObject::connect(p2p, &P2PTunnel::failed, &app, [&, id = t->id](const QString &error) {
            fprintf(stderr, "omd-tunnel: P2P failed for %s: %s (using relay)\n", qPrintable(id), qPrintable(error));
        });
        p2p->start();
    };
#else
    std::function<void(Tunnel*)> setupP2P = [&](Tunnel *) { Q_UNUSED(useP2P); };
#endif

    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &app, [&]() {
        fprintf(stderr, "omd-tunnel: connection to %s:%d timed out\n", qPrintable(server), serverPort);
        app.exit(1);
    });
    timeout.start(timeoutSec * 1000);

    std::function<void()> connectGateway;
    connectGateway = [&]() {
        QSslSocket *sock = new QSslSocket(&app);
        socket = sock;
        sock->ignoreSslErrors();
        sock->setPeerVerifyMode(QSslSocket::VerifyNone);

        QFile keyFile(keyPath);
        if (!keyFile.open(QIODevice::ReadOnly))
        {
            fprintf(stderr, "omd-tunnel: can not open private key %s\n", qPrintable(keyPath));
            app.exit(1);
            return;
        }
        QSslKey key(&keyFile, QSsl::Rsa, QSsl::Pem, QSsl::PrivateKey, deviceId.toLatin1());
        keyFile.close();
        if (key.isNull())
        {
            fprintf(stderr, "omd-tunnel: can not load private key %s\n", qPrintable(keyPath));
            app.exit(1);
            return;
        }
        QSslConfiguration sslConfig = sock->sslConfiguration();
        sslConfig.setPrivateKey(key);

        QFile certFile(certPath);
        if (!certFile.open(QIODevice::ReadOnly))
        {
            fprintf(stderr, "omd-tunnel: can not open certificate %s\n", qPrintable(certPath));
            app.exit(1);
            return;
        }
        QList<QSslCertificate> certs = QSslCertificate::fromDevice(&certFile);
        certFile.close();
        if (certs.isEmpty() || certs.at(0).isNull())
        {
            fprintf(stderr, "omd-tunnel: can not load certificate %s\n", qPrintable(certPath));
            app.exit(1);
            return;
        }
        sslConfig.setLocalCertificate(certs.at(0));
        sock->setSslConfiguration(sslConfig);

        QObject::connect(sock, static_cast<void (QSslSocket::*)(const QList<QSslError> &)>(&QSslSocket::sslErrors), sock, [sock](const QList<QSslError> &) {
            sock->ignoreSslErrors();
        });

        QObject::connect(sock, &QSslSocket::errorOccurred, &app, [&, sock](QAbstractSocket::SocketError) {
            if (sock != socket)
                return;
            fprintf(stderr, "omd-tunnel: gateway socket error: %s\n", qPrintable(sock->errorString()));
            if (!listenMode)
                app.exit(1);
        });

        QObject::connect(sock, &QSslSocket::disconnected, &app, [&, sock]() {
            if (sock != socket)
                return;
            fprintf(stderr, "omd-tunnel: disconnected from gateway\n");
            if (listenMode)
            {
                fprintf(stderr, "omd-tunnel: reconnecting to gateway in 3 seconds...\n");
                sock->deleteLater();
                socket = nullptr;
                QTimer::singleShot(3000, &app, connectGateway);
            }
            else
            {
                app.exit(1);
            }
        });

        QObject::connect(sock, &QSslSocket::readyRead, &app, [&, sock]() {
            buffer.append(sock->readAll());

            LinkMessage msg;
            while (readLinkMessage(buffer, blockSize, msg))
            {
                const QString id = msg.getDataValue("ID").toString();
                Tunnel *t = tunnels.value(id, nullptr);
                if (msg.getMessageType() != LinkMessage::TunnelOpen && !t)
                    continue;

                switch (msg.getMessageType())
                {
                case LinkMessage::TunnelOpen:
                    if (msg.getResultCode() == LinkMessage::OK)
                    {
                        if (t)
                        {
                            if (t->key.isEmpty() && t->e2eePriv.size() == 32)
                            {
                                const QByteArray peerPub = QByteArray::fromBase64(msg.getDataValue("e2eePub").toByteArray());
                                if (peerPub.size() == 32)
                                {
                                    QByteArray shared = TunnelCrypto::computeSharedSecret(t->e2eePriv, peerPub);
                                    QByteArray key = TunnelCrypto::deriveKey(shared);
                                    if (!key.isEmpty())
                                    {
                                        t->key = key;
                                        if (debug)
                                            fprintf(stderr, "omd-tunnel: E2EE enabled for %s\n", qPrintable(id));
                                    }
                                }
                            }
                            t->ready = true;
                            if (debug)
                                fprintf(stderr, "omd-tunnel: tunnel open %s\n", qPrintable(id));
                            if (t->tcp)
                            {
                                if (!t->pending.isEmpty())
                                {
                                    sendTunnelData(t, t->pending);
                                    t->pending.clear();
                                }
                            }
                            else
                            {
                                if (!pendingStdin.isEmpty())
                                {
                                    sendTunnelData(t, pendingStdin);
                                    pendingStdin.clear();
                                }
                            }
                        }
                    }
                    else if (t)
                    {
                        fprintf(stderr, "omd-tunnel: open failed for %s: %s\n", qPrintable(id), qPrintable(msg.getErrorMsg()));
                        const bool exitAfter = !listenMode;
                        tunnels.remove(id);
#ifdef P2P_ENABLED
                        if (t->p2p)
                        {
                            t->p2p->close();
                            t->p2p->deleteLater();
                            t->p2p = nullptr;
                        }
#endif
                        if (t->tcp)
                        {
                            QObject::disconnect(t->tcp, nullptr, &app, nullptr);
                            t->tcp->disconnectFromHost();
                            t->tcp->deleteLater();
                        }
                        delete t;
                        if (exitAfter)
                            app.exit(1);
                    }
                    break;
                case LinkMessage::TunnelData:
                {
                    QByteArray data = msg.getPayLoad();
                    if (!t->key.isEmpty())
                    {
                        QByteArray plain;
                        if (!TunnelCrypto::decryptPacket(t->key, data, plain))
                        {
                            if (debug)
                                fprintf(stderr, "omd-tunnel: decrypt failed for %s, dropping packet\n", qPrintable(id));
                            break;
                        }
                        data = plain;
                    }
                    if (data.isEmpty())
                        break;
                    if (t->tcp)
                    {
                        t->tcp->write(data);
                    }
                    else
                    {
                        ssize_t written = ::write(STDOUT_FILENO, data.constData(), static_cast<size_t>(data.size()));
                        Q_UNUSED(written)
                    }
                    break;
                }
                case LinkMessage::TunnelClose:
                {
                    if (!t)
                        break;
                    if (debug)
                        fprintf(stderr, "omd-tunnel: tunnel closed %s\n", qPrintable(id));
#ifdef P2P_ENABLED
                    if (t->p2p)
                    {
                        t->p2p->close();
                        t->p2p->deleteLater();
                        t->p2p = nullptr;
                    }
#endif
                    if (listenMode)
                    {
                        if (t->tcp)
                        {
                            QObject::disconnect(t->tcp, nullptr, &app, nullptr);
                            t->tcp->disconnectFromHost();
                            t->tcp->deleteLater();
                        }
                        tunnels.remove(id);
                        delete t;
                    }
                    else
                    {
                        app.exit(0);
                    }
                    break;
                }
#ifdef P2P_ENABLED
                case LinkMessage::TunnelSignaling:
                {
                    if (!t || !t->p2p)
                        break;
                    const QString kind = msg.getDataValue("kind").toString();
                    if (debug)
                    {
                        if (kind == QLatin1String("candidate"))
                            fprintf(stderr, "omd-tunnel: <- p2p candidate for %s: %s\n", qPrintable(id), qPrintable(formatCandidateInfo(msg.getPayLoad())));
                        else
                            fprintf(stderr, "omd-tunnel: <- p2p signaling %s for %s\n", qPrintable(kind), qPrintable(id));
                    }
                    t->p2p->handleSignaling(kind, msg.getPayLoad());
                    break;
                }
#endif
                default:
                    break;
                }
            }
        });

        QObject::connect(sock, &QSslSocket::encrypted, &app, [&, sock]() {
            timeout.stop();
            fprintf(stderr, "omd-tunnel: connected to %s:%d as \"%s\" (v1.5-p2p-fmt)\n", qPrintable(server), serverPort, qPrintable(effectiveUser));

            if (listenMode)
            {
                if (!tcpServer.isListening())
                {
                    if (!tcpServer.listen(QHostAddress::LocalHost, listenPort))
                    {
                        fprintf(stderr, "omd-tunnel: can not listen on 127.0.0.1:%d: %s\n", listenPort, qPrintable(tcpServer.errorString()));
                        app.exit(1);
                        return;
                    }
                    fprintf(stderr, "omd-tunnel: listening on 127.0.0.1:%d, tunnel to %s:%d\n", listenPort, qPrintable(targetLinkID), targetPort);
                }

                for (Tunnel *t : tunnels)
                {
                    if (!t->ready && !t->openSent)
                        sendTunnelOpen(t);
                }
            }
            else
            {
                Tunnel *stdinTunnel = new Tunnel();
                stdinTunnel->id = QUuid::createUuid().toString(QUuid::WithoutBraces);
                if (e2ee)
                    TunnelCrypto::generateKeyPair(stdinTunnel->e2eePriv, stdinTunnel->e2eePub);
                tunnels.insert(stdinTunnel->id, stdinTunnel);
                setupP2P(stdinTunnel);
                sendTunnelOpen(stdinTunnel);
            }
        });

        sock->connectToHostEncrypted(server, static_cast<quint16>(serverPort));
    };

    connectGateway();

    const auto closeTunnel = [&](Tunnel *t) {
        if (!t)
            return;
        if (!tunnels.contains(t->id))
            return;
        if (socket)
        {
            LinkMessage close;
            close.setMessageType(LinkMessage::TunnelClose);
            close.setResultCode(LinkMessage::OK);
            close.insertDataValue("ID", t->id);
            writeLinkMessage(socket, close);
        }
        if (debug)
            fprintf(stderr, "omd-tunnel: client closed tunnel %s\n", qPrintable(t->id));
        tunnels.remove(t->id);
#ifdef P2P_ENABLED
        if (t->p2p)
        {
            t->p2p->close();
            t->p2p->deleteLater();
            t->p2p = nullptr;
        }
#endif
        if (t->tcp)
        {
            QObject::disconnect(t->tcp, nullptr, &app, nullptr);
            t->tcp->disconnectFromHost();
            t->tcp->deleteLater();
        }
        delete t;
    };

    if (listenMode)
    {
        QObject::connect(&tcpServer, &QTcpServer::newConnection, &app, [&]() {
            QTcpSocket *client = tcpServer.nextPendingConnection();

            Tunnel *t = new Tunnel();
            t->id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            t->tcp = client;
            if (e2ee)
                TunnelCrypto::generateKeyPair(t->e2eePriv, t->e2eePub);
            tunnels.insert(t->id, t);
            client->setSocketOption(QAbstractSocket::KeepAliveOption, 1);
            fprintf(stderr, "omd-tunnel: client connected on 127.0.0.1:%d (%s)\n", listenPort, qPrintable(t->id));

            QObject::connect(client, &QTcpSocket::readyRead, &app, [&, client, t]() {
                const QByteArray data = client->readAll();
                if (data.isEmpty())
                    return;
                if (t->ready)
                    sendTunnelData(t, data);
                else
                    t->pending.append(data);
            });
            QObject::connect(client, &QTcpSocket::errorOccurred, &app, [&, client, t](QAbstractSocket::SocketError err) {
                if (err == QAbstractSocket::RemoteHostClosedError)
                {
                    closeTunnel(t);
                    return;
                }
                fprintf(stderr, "omd-tunnel: client error: %s\n", qPrintable(client->errorString()));
                closeTunnel(t);
            });
            QObject::connect(client, &QTcpSocket::disconnected, &app, [&, t]() {
                closeTunnel(t);
            });

            // If the gateway socket is connected, send TunnelOpen.
            if (debug)
                fprintf(stderr, "omd-tunnel: new client connection, socket ptr=%p, state=%d, isEncrypted=%d\n",
                        (void*)socket, socket ? socket->state() : -1, (socket && socket->isEncrypted()) ? 1 : 0);
            if (socket && (socket->state() == QAbstractSocket::ConnectedState || socket->isEncrypted()))
            {
                if (debug)
                    fprintf(stderr, "omd-tunnel: new client, useP2P=%d\n", useP2P);
                if (useP2P && !t->p2p)
                {
                    P2PTunnel *p2p = new P2PTunnel(P2PTunnel::Role::Offerer, &app);
                    p2p->setTurnServer(turnUrlFor(server), "turnuser", "turnpass");
                    t->p2p = p2p;
                    QObject::connect(p2p, &P2PTunnel::signalReady, &app, [&, p2p, id = t->id](const QString &kind, const QByteArray &payload) {
                        Tunnel *tt = tunnels.value(id, nullptr);
                        if (!tt || !socket)
                            return;
                        if (debug)
                        {
                            if (kind == QLatin1String("candidate"))
                                fprintf(stderr, "omd-tunnel: -> p2p candidate for %s: %s\n", qPrintable(id), qPrintable(formatCandidateInfo(payload)));
                            else
                                fprintf(stderr, "omd-tunnel: -> p2p signaling %s for %s\n", qPrintable(kind), qPrintable(id));
                        }
                        LinkMessage sig;
                        sig.setMessageType(LinkMessage::TunnelSignaling);
                        sig.setResultCode(LinkMessage::Request);
                        sig.insertDataValue("ID", id);
                        sig.insertDataValue("kind", kind);
                        sig.setPayLoad(payload);
                        writeLinkMessage(socket, sig);
                    });
                    QObject::connect(p2p, &P2PTunnel::dataReceived, &app, [&, id = t->id](const QByteArray &data) {
                        if (data.isEmpty())
                            return;
                        Tunnel *tt = tunnels.value(id, nullptr);
                        if (!tt)
                            return;
                        if (tt->tcp)
                            tt->tcp->write(data);
                    });
                    QObject::connect(p2p, &P2PTunnel::opened, &app, [&, id = t->id](const QString &pairInfo) {
                        if (!pairInfo.isEmpty())
                            fprintf(stderr, "omd-tunnel: P2P established for %s via %s\n", qPrintable(id), qPrintable(pairInfo));
                        else
                            fprintf(stderr, "omd-tunnel: P2P established for %s\n", qPrintable(id));
                    });
                    QObject::connect(p2p, &P2PTunnel::closed, &app, [&, id = t->id]() {
                        fprintf(stderr, "omd-tunnel: P2P closed for %s, falling back to relay\n", qPrintable(id));
                    });
                    QObject::connect(p2p, &P2PTunnel::failed, &app, [&, id = t->id](const QString &error) {
                        if (debug)
                            fprintf(stderr, "omd-tunnel: P2P failed for %s: %s\n", qPrintable(id), qPrintable(error));
                    });
                    p2p->start();
                }

                if (!t->openSent)
                {
                    t->openSent = true;
                    LinkMessage open;
                    open.setMessageType(LinkMessage::TunnelOpen);
                    open.setResultCode(LinkMessage::Request);
                    open.insertDataValue("ID", t->id);
                    open.insertDataValue("linkID", targetLinkID);
                    open.insertDataValue("host", remoteHost);
                    open.insertDataValue("port", targetPort);
                    if (e2ee && !t->e2eePub.isEmpty())
                        open.insertDataValue("e2eePub", t->e2eePub);
                    if (debug)
                        fprintf(stderr, "omd-tunnel: -> TunnelOpen %s for %s:%d\n", qPrintable(t->id), qPrintable(targetLinkID), targetPort);
                    writeLinkMessage(socket, open);
                }
            }
        });
    }

    // Read stdin from a dedicated thread. QSocketNotifier can not be used on
    // Windows for pipes (only for sockets), so this also works as ProxyCommand.
    // Data received before the tunnel is open is buffered and flushed on open.
    if (!listenMode)
    {
        QThread *stdinThread = QThread::create([&]() {
            char buf[65536];
            for (;;)
            {
                ssize_t n = ::read(STDIN_FILENO, buf, sizeof(buf));
                if (n > 0)
                {
                    QByteArray data(buf, static_cast<int>(n));
                    QMetaObject::invokeMethod(&app, [&, data]() {
                        Tunnel *t = tunnels.isEmpty() ? nullptr : *tunnels.begin();
                        if (!t)
                        {
                            pendingStdin.append(data);
                            return;
                        }
                        if (!t->ready)
                        {
                            pendingStdin.append(data);
                            return;
                        }
                        sendTunnelData(t, data);
                    }, Qt::QueuedConnection);
                }
                else if (n == 0)
                {
                    // stdin EOF: stop feeding the tunnel, but keep reading until the
                    // remote side closes the TCP connection (the peer's tunnel agent
                    // sends TunnelClose on TCP disconnect).
                    QMetaObject::invokeMethod(&app, [&]() {
                        fprintf(stderr, "omd-tunnel: stdin closed\n");
                    }, Qt::QueuedConnection);
                    break;
                }
                else
                {
                    const QString msg = QString::fromLocal8Bit(strerror(errno));
                    QMetaObject::invokeMethod(&app, [&, msg]() {
                        fprintf(stderr, "omd-tunnel: read error: %s\n", qPrintable(msg));
                        app.exit(1);
                    }, Qt::QueuedConnection);
                    break;
                }
            }
        });
        stdinThread->start();
    }

    return app.exec();
}
