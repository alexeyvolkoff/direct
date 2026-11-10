#ifndef ONMYDISKLINKSERVER_H
#define ONMYDISKLINKSERVER_H

#include <QTcpServer>
#include <QSslSocket>
#include <QSslConfiguration>
#include <QPointer>
#include <QSettings>
#include <QFile>
#include <QTime>
#include <QTimer>
#include <QThread>
#include <QProcess>
#include <QPair>
#include <functional>
#include "onmydisklink_global.h"
#include <QWaitCondition>
#include <QJsonArray>
#include "linksocket.h"
#include "linkmessage.h"
#include "sslhelper.h"
#include "timelogger.h"
#include <QThread>
#include "pkihelper.h"
#include "plainserver.h"
#include "onmydisklink.h"



struct LINK_API WebSignal {
    QString tunnelID;
    QString kind;
    QByteArray payload;
    quint64 timestamp = 0; // ms since epoch, set when queued (TTL sweep)
};

class LINK_API OnMyDiskLinkServer: public QTcpServer
{
    Q_OBJECT
private:

    explicit OnMyDiskLinkServer();
public:
    void init(QSettings* settings, PKIHelper* pkiHelper);

    ~OnMyDiskLinkServer();

    QPointer<OnMyDiskLink> getLink(const QString& linkID);
    bool getSite(const QString& siteName, SiteInfo& site);
    bool getSiteByDomain(const QString& domain, SiteInfo& site);
    SitesHash getSearchNodes();
    bool getChatNode(const QString& user, SiteInfo& site);
    bool getPublicAiNode(SiteInfo& site);
    bool hasCodePermission(const QString& user);
    QSslConfiguration sslConfiguration() {return m_sslConfig; }
	int linkClount() {return m_links.count(); }
    void checkLinks();
    void disconnectLinks();
    static OnMyDiskLinkServer &instance();
    bool hasActiveLink(const QString &user);
    QJsonArray getConnectedDevices(const QString &user, const QJsonArray &filterLinkIds = QJsonArray());

    bool sendWebSignaling(const QString &targetID, const QString &tunnelID, const QString &kind, const QByteArray &payload, const QString &senderId, quint16 port = 0, const QString &service = "", const QString &sessionUser = "");
public slots:
    void doSendToNodeSocket(const QString &linkID, const QString &tunnelID, bool sendOpen, const QString &kind, const QByteArray &payload, quint16 port, const QString &service, const QString &sessionUser = "");
public:
    QList<WebSignal> getWebSignals(const QString &tunnelID, const QString &requesterId, int timeoutMs = 3000);

    bool registerWebDevice(const QString &linkId, const QString &ownerId);
    void unregisterWebDevice(const QString &linkId);
    bool isWebDevice(const QString &linkId) const;
    bool isWebDeviceOwner(const QString &linkId, const QString &ownerId);
    QList<WebSignal> getPendingTunnelOpen(const QString &linkId, const QString &requesterId, int timeoutMs = 0);
    QList<QJsonObject> getNearbyNodes(const QString &clientIp);

    // Push delivery of web signaling (tunnel-open / offer / candidate) to a
    // web device via its persistent WebSocket channel. The upper layer
    // (HttpServerBackend) installs the handler bridging to ClientNotifier;
    // returning false means no live channel - caller falls back to the poll queue.
    typedef std::function<bool(const QString &linkId, const QString &tunnelId, const QString &kind, const QByteArray &payload)> WebPushHandler;
    void setWebPushHandler(WebPushHandler handler) { QMutexLocker locker(&m_webMutex); m_webPushHandler = handler; }

protected:
    void incomingConnection(qintptr handle);
    void createLinkCert();
    void initLinkSocket(LinkSocket* socket);
private slots:
    void clientDisconnected();
    void newServiceMessage(LinkSocket* serverSocket, const LinkMessage &message);
    void linkSocketReady(LinkSocket* socket);
    void sweepWebState();
private:
    bool pushWebSignal(const QString &targetID, const QString &tunnelID, const QString &kind, const QByteArray &payload);
    // Owner key of a node: user name for TLS links, bound owner for web
    // devices, empty for the "web" pseudo-node / unknown ids. Call under m_mutex.
    QString nodeOwnerId(const QString &linkId);
    // True if senderId owns one of the tunnel ends or created it via web signaling.
    bool isTunnelParticipant(const QString &tunnelID, const QString &senderId);
signals:
    void linkDisconnected(QString ID, QString user);
    void linkInactive(QString ID, QString user);
    void linkConnected(QString ID, QString user);
    void shareAdded(QString ID, QString shareName, QHash<QString, QVariant> shareData, QHash<QString, QHash<QString, QVariant>> shareEntryData);
    void shareUpdated(QString ID, QString user, QString shareName, QHash<QString, QVariant> shareData, QHash<QString, QHash<QString, QVariant>> shareEntryData);
    void shareRemoved(QString ID, QString user, QString shareName);
    void collectionChanged(QString ID, QString collectionPath, QHash<QString, QVariant> shareData, QHash<QString, QHash<QString, QVariant>> shareEntryData);
    void timeToPingSignal(QString LinkID);
    void publicLinkAdded(QString ID, QHash<QString, QVariant> linkData);
private:
    QRecursiveMutex m_mutex;
    QString m_CAPath, m_legacyCAPath, m_CertPath, m_KeyPath, m_KeyPass;
    bool m_caLoaded = false;
    bool m_legacyCaLoaded = false;
    QSettings* m_settings;
    QPointer<PKIHelper> m_pkiHelper;
    QSslConfiguration m_sslConfig;
    quint16 m_port;
    bool m_keyLogin;
    QString m_publicAiNodeID;
    bool setupSSL();
    PlainSocketServer m_plainServer;
    SslHelper m_sslHelper;
    QHash<QString, OnMyDiskLink*> m_links;
    QHash<QString, QString> m_persistentIDs;
    QHash<QString, QPair<QString, QString>> m_tunnels;
    QHash<QString, quint64> m_tunnelActivity; // tunnelID -> last relay activity, ms
    QMutex m_webMutex;
    QWaitCondition m_webWaitCondition;
    QHash<QString, QList<WebSignal>> m_webSignals;
    QSet<QString> m_webDevices;  // live web device linkIds (WS channel open or polling)
    QHash<QString, QString> m_webDeviceOwner; // linkId -> owner key (user name or guest session ID)
    QHash<QString, quint64> m_webDeviceSeen;  // linkId -> last owner activity, ms
    QHash<QString, QString> m_tunnelCreator;  // tunnelID -> owner key of the web side that created it
    WebPushHandler m_webPushHandler;
    QHash<QString, QList<LinkSocket*>> m_pendingLinks;
    SitesHash m_sites;
    SitesHash m_searchNodes;
    QMultiHash<QString, SiteInfo> m_aiNodes;
};

#endif // ONMYDISKLINKSERVER_H
