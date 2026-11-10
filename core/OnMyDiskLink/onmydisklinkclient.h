#ifndef ONMYDISKLINKCLIENT_H
#define ONMYDISKLINKCLIENT_H

#include <QObject>
#include <QSettings>
#include <QFile>
#include <QDir>
#include <QSsl>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSignalBlocker>
#include <QTimer>
#include <QThread>
#include <QNetworkAccessManager>
#include <QNetworkInterface>
#include <QHostInfo>
#include <QSysInfo>
#include <QStandardPaths>
#include <QNetworkReply>
#include <QMimeType>
#include <QMimeDatabase>
#include <QUuid>
#include <QMutex>
#include <QRecursiveMutex>
#include <QMutexLocker>
#include <QPointer>
#include <QSet>
#include <QTcpSocket>
#if !defined(Q_OS_MAC) || defined(P2P_ENABLED)
#include <openssl/pem.h>
#include <openssl/conf.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#ifndef OPENSSL_NO_ENGINE
#include <openssl/engine.h>
#endif
#endif
#include <QSsl>
#include <QSslCertificate>
#include <QSslKey>
#include <QWaitCondition>
#include "onmydisklink_global.h"
#include "omdsettings.h"
#include "linksocket.h"
#include "linkmessage.h"
#include "linkcommunicator.h"
#include "hostresource.h"
#include "sslhelper.h"
#include "timelogger.h"
#include <QEventLoop>

class P2PTunnel;



class  OnMyDiskLinkClient : public QNetworkAccessManager
{
    Q_OBJECT
public:
    explicit OnMyDiskLinkClient(QSettings* settings, HostResource *root,  QObject *parent = nullptr);
    ~OnMyDiskLinkClient();

	enum LinkState {Connected, Authorizing, Connecting, Disconnected};

    // Static variables for connection settings
    static QHash<QString, QString> shares;
    static QSslConfiguration sslConfiguration;
    static quint16 cloudPort;
    static quint16 plainPort;
    static QString aiBaseURL;
    static QString searchBaseURL;
    QString linkId() const { return m_linkId; }
    QString deviceId() {return  m_deviceId; }
    QString user() {return  m_user; }
    bool keyLogin() {return m_keyLogin; }
    LinkSocket *serviceLink() const { return serviceSocket; }
    void readSettings();

signals:
    void linkConnected();
    void linkDisconnected();
    void sslError(QList<QSslError> errorList, bool &ignore);
    void sslWarning(QList<QSslError> errorList);
    void connectionError(QAbstractSocket::SocketError socketError);

    void privateKeyStatus(QString statusString);
    void privateKeyError(QString errorString);
    void sslOK(QString commonName, QString givenName, QString certhash, QString validFrom, QString validTo);

    void noSslSupport();
    void cloudPKIConnectionError(QNetworkReply::NetworkError error);

    void shareExists(QString shareName);
    //alexey: provide custom authentication headers
    void beforePost(QNetworkRequest *request);
    void accountCreated(QString commonName, QString givenName);
    void deviceIdUpdated(QString cdeviceId);
    void restartSites(const QString& action, const QString& site = "");
    void settingsChanged(const OnMyDiskSettings& settings);

public slots:
    void connectToCloud();
    void disconnectFromCloud();
    void clearDeviceId();
    void resetDeviceId();
    void addLocalShare(QString shareName, QString sharePath, bool persistent);
    void removeLocalShare(QString shareName);
    void addShare(QString sharePath);
    void removeUserKey();
    void resetTimer();
    //public link added
    void publicLinkAdded(const QString& itemPath, const QString& link, const QHash<QString, QVariant> &linkData);
    //user settings changed/loaded
    void updateSettings(const OnMyDiskSettings& settings);
private:
    QSettings* m_settings;
    OnMyDiskSettings m_omdSettings;
    QRecursiveMutex m_sharesMutex; //now we have to protect shares

    HostResource *m_hostRoot;
    LinkSocket* serviceSocket = nullptr;
    QString m_deviceId;
    QString m_origDeviceId;
    QString m_profilePath;
    QString m_sitesPath;
    QStringList m_syncPaths;
    QString m_sharedWith;

    bool m_keyLogin;
    bool m_searchSupported = false;
    bool m_aiSupported = false;

public:
    enum class P2PAuthLevel {
        Unauthenticated = 0,
        ScopedGuest = 1,
        Admin = 2
    };

    struct P2PSession {
        P2PAuthLevel authLevel = P2PAuthLevel::Unauthenticated;
        QString user = "";
        QString didPub = "";
        QString scopePath = "";
        bool writable = false;
        QString shareId = "";
        QByteArray rxBuffer = "";
    };

private:
    void handleConnectDataSockets(const LinkMessage &message);
    void handleRPCRequest(const LinkMessage &message);
    void handleTunnelOpenRequest(const LinkMessage &message);
    void handleTunnelDataRequest(const LinkMessage &message);
    void handleTunnelCloseRequest(const LinkMessage &message);
    void handleTunnelSignalingRequest(const LinkMessage &message);
    void handleP2PFileRequest(P2PTunnel *p2p, const QByteArray &data);
    void processP2PFileCommand(P2PTunnel *p2p, const QJsonObject &req);
    void cleanupP2PTunnel(const QString &ID);
    // Tunnel caller identity: the gateway validated the initiator (owner /
    // shared user / guest) before routing TunnelOpen to this node. For port
    // tunnels we re-write the request stream towards the local AI service,
    // stripping client-supplied identity headers and injecting these trusted
    // ones - the Python service must trust neither the raw request headers
    // nor a fake "X-OMD-Private-Mode".
    void writeInjectedRequestData(const QString &ID, const QByteArray &data, QTcpSocket *tcp);
    static QByteArray injectCallerHeaders(const QByteArray &head, const QString &user, const QString &role, const QString &targetOwner, const QString &did = QString());
    // P2P FS requests are handled by a global QThreadPool, so a tunnel must
    // never be closed while a chunk is still being processed/sent: destroying
    // the libdatachannel state concurrently with an in-flight send() can hang
    // the main thread in a futex wait forever. These helpers refcount in-flight
    // tasks and defer close()/deleteLater() until the last task returned.
    bool p2pTaskStarted(P2PTunnel *p2p);
    void p2pTaskFinished(P2PTunnel *p2p);
    void finalizeP2PTunnel(P2PTunnel *p2p);
    // Notify the gateway that a P2P tunnel is dead (ICE failed / closed) so
    // it can drop the routing state and tell the web end immediately.
    void sendTunnelCloseFor(const QString &ID);
    void sendTunnelDataFor(const QString &ID, const QByteArray &data);
    void sendTunnelSignalingFor(const QString &ID, const QString &kind, const QByteArray &payload);
    bool setupSSL();
    bool verifyCert(QSslCertificate cert);
    QString m_CAPath, m_CertPath, m_KeyPath;
    QString m_user, m_newUser, m_hash, m_linkId;
    QUrl m_restApiUrl;     
    QHash<QString, QTcpSocket*> m_tunnels;
    QHash<QString, P2PTunnel*> m_p2pTunnels;
    QHash<P2PTunnel*, P2PSession> m_p2pSessions;
    QMutex m_p2pMutex;
    // Refcount of pool threads currently using a tunnel + tunnels being torn down.
    QHash<P2PTunnel*, int> m_p2pBusy;
    QSet<P2PTunnel*> m_p2pDying;
    QHash<QString, QByteArray> m_tunnelE2EEPub;
    QHash<QString, QByteArray> m_tunnelKeys;
    // trusted caller identity relayed by the gateway with TunnelOpen
    QHash<QString, QString> m_tunnelCallerUser;
    QHash<QString, QString> m_tunnelCallerRole;
    QHash<QString, QString> m_tunnelTargetOwner;
    // per-tunnel request-stream reassembly for header injection
    QHash<QString, QPair<QByteArray, qint64> > m_tunnelHttpStream; // ID -> [pending bytes, bodyLeft<0=head phase]
    bool fetchCA();
    bool fetchCert(QByteArray CSR);
    bool genPrivateKey();
    QByteArray genCertificateSignRequest(QString ownerId);
    void removeClientKeys();
    SslHelper m_SslHelper;
    int m_maxDataSockets;
    int m_connectedDataSockets;
    LinkState m_linkStatus;
    QMutex m_connectionStateMutex;
    bool m_treatSslErrorsAsWarnings;
    QSet<QString> m_mountedWebdavShares;
    QEventLoop m_loop;
    QTimer m_pingResponseTimer;
    int m_pingMisses = 0;
    qint64 m_lastPingTickMs = 0;
    QTimer m_ragReindexTimer;
    QTimer m_ragFlushTimer;
    QTimer m_ragStartupTimer;
    bool m_ragStartupGrace = false;
    QSet<QString> m_ragPending;
    QNetworkAccessManager *m_ragManager = nullptr;
    bool m_responseResult;
    void removeShare(QString shareName);
    void addShare(QString shareName, QString sharePath, bool persistent);
    void sendUpdateHostRoot();
    void sendUpdateSite(QString site);
    void sendUpdateHostAddShare(QString shareName, bool persistent);
    void sendUpdateHostRemoveShare(QString shareName);
    QString randomKey(int length);
    void loadDeviceId();
	bool checkDeviceId();
    void createDeviceId();
    bool requestNewCert(QString ownerId = "");
	void removeDeviceKey();
    // RAG 3.0 owner-only indexing (AI node REST, see docs/RAG_3_0.md)
    void ragIndexPath(const QString &path, bool force = false);
    void ragSendIndex(const QString &path, bool force);
    void ragDeleteSource(const QString &source);
    void ragReindexTick();
    void ragFlushPending();
    void ragStartupGraceElapsed();
    QString ragLocalPathFor(const QString &virtualPath);
    QString ragVirtualPathFor(const QString &realPath);
    QNetworkAccessManager *ragManager();
private slots:
    void timeToPing();
    void pingTimeout();
    void messageRecieved(LinkSocket* serverSocket, const LinkMessage& message);
    void requestFinished(QNetworkReply*reply);
    void workerConnected();
    void socketDisconnected();
    void socketConnected();

public slots:
    void onSslError(const QList<QSslError> &errors);
    void onSslErrors(QNetworkReply *reply, const QList<QSslError> &errors);
    void onConnectionError(QAbstractSocket::SocketError socketError);
    void signedIn(QString userName);
    void authRequested(QString userName, QString password);
    void siteCreated(QString owner, SiteInfo site);
    void fileRemoved(QString owner, QString user, QString path);
    void siteUpdated(QString owner, QString site, bool rebuild);
};

#endif // ONMYDISKLINKCLIENT_H
