#ifndef HTTPBACKEND_H
#define HTTPBACKEND_H

#include <QObject>
#include <QFileSystemWatcher>
#include <QSettings>
#include <QHostInfo>
#include <QMimeDatabase>
#include <QTextStream>
#include <QThread>
#include <QThreadPool>
#include <QRunnable>

#include <QWebChannel>
#include <QWebSocketServer>
#include "clientnotifier.h"

#include "qhttpserver.hpp"
#include "qhttpserverrequest.hpp"
#include "qhttpserverresponse.hpp"
#include "rootresource.h"
#include "httphandler.h"
#include "sessionauth.h"
#include "onmydisklinkserver.h"
#include "remoteresource.h"
#include "pkihelper.h"
#include "userevents.h"
#include "notificationmanager.h"

#include "crawler.h"
#include "watchdog.h"


using namespace qhttp::server;


class OnMyDiskMailer;

class HttpServerBackend : public QObject
{
    Q_OBJECT
public:
    explicit HttpServerBackend(QSettings *settings, QObject *parent = nullptr);
    ~HttpServerBackend();
    bool isListening();

private slots:
    void handleRequest(QPointer<QHttpRequest> req, QPointer<QHttpResponse> resp);
    void linkDisconnected(QString linkID, QString user);
    void linkInactive(QString linkID, QString user);
    void shareUpdated(QString linkID, QString user, QString shareName, QHash<QString, QVariant> shareData, QHash<QString, QHash<QString, QVariant>> shareEntryData, QHash<QString, QHash<QString, QVariant>> shareLinks = {});
    void sharedLinkAdded(QString user, QString linkID, QString shareName, QString owner, bool writable, bool isDir = true, QString itemPath = QString());
    void sharedLinkRemoved(QString user,  QString shareName);
    void fileMoved(const QString &owner, const QString &user, const QString& origPath, const QString& newPath);
    void fileUpdated(const QString &owner, const QString &user, const QString& filePath);
    void fileRemoved(const QString &owner, const QString &user, const QString& filePath);
    void subscribe(const QString &userName, const QString &user, const QString& site);
    void unsubscribe(const QString &userName, const QString &user, const QString& site);
    void siteCreated(QString owner, SiteInfo site);

    //websockets
    void userConnected(const QString& user, QSharedPointer<ClientNotifier> notifier);

	void cleanUp();
	void startCleanUp();
    void watchDogPing();

    void shareAddRequested(QString user, QString path);
    void shareRemoveRequested(QString user, QString path);
    void removePublicLink(QString linkID, QString path, QString owner);

    void publicLinkRequested(QString linkID, QString path, QString owner, QHash<QString, QVariant> linkData);
    void siteUpdated(QString owner, QString siteName, bool rebuild);
    void accountCreated(QString user);
private:
    QStringList m_indexFiles;
    QStringList m_sharedIndexFiles;
    QList<QPointer<HttpHandler>> m_handlers;
    bool m_autoIndex;
    quint64 m_maxDiskCacheSize;
    quint64 m_maxRamCacheSize;
    QHttpServer m_fileServer;
    QString m_serverString;
    QList<IAuthorization*> m_authProviders;
    QSettings* m_Settings;
    SessionAuth m_sessionProvider;
    PKIHelper m_pkiHelper;
    OnMyDiskLinkServer& m_LinkServer;
    OnMyDiskMailer& m_mailer;
    //Decentralized search
    QMap<QString, QJsonObject> m_searcheNodeResponses;
    QMap<QString, int> m_searcheNodeRequests;
    QMap<QString, int> m_searcheNodeRequestsDone;
    UserEvents& m_userEvents;
    NotificationManager& m_notificationManager;
    QHash<QString, RootResource*> m_rootResources;
    QHash<QString, RootResource*> m_sharedRootResources;
    SitesHash m_searchNodes;
    QMultiHash<QString, SiteInfo> m_aiNodes;
    SitesHash m_sites;
    //QString m_docRoot, m_themeRoot;
    quint64 m_maxFileSize;
    //QString m_loginPageFile;
    //QString m_loginKeyFile;
    //QString m_loginLinkFile;
    bool m_keyLogin;
    bool m_searchEnabled;
    QString m_publicAiNodeID;
    QStringList m_nonAuthDestinations;
    QStringList m_adminDestinations;
    QStringList m_ignoredFiles;
    LocalResource* m_docRoot;
    LocalResource* m_themeRoot;
    QHash<qint64, QString> m_publicLinks;
    RootResource* m_publicRoot;
	QRecursiveMutex m_mutex;
	QTimer m_cleanupTimer;
    int m_requestCount = 0;
    int m_lastRequestCount = 0;
    bool m_exiting = false;
    bool m_statsEnabled = false;
    QString m_serverUrl;

    //Watchdog
    Watchdog* m_watchdog;
    QThread* m_watchdogThread;

    //Sync engine

    //websocket notifications
    QWebSocketServer *m_wsServer;
    WebSocketClientWrapper *m_clientWrapper;

    RootResource* getRootFor(QString login, bool canCreate = false);
    RootResource* getSharedRootFor(QString linkId, QString sharedItemPath, QString owner, bool directShare, bool writable, QString version);
    QString rootPathForLink(QString login,  QString linkId);
	QString getOwnerOfLink(QString linkID, bool &isOk);

	AuthResult checkAuth(QHttpRequest *req, QHttpResponse *resp, QString &userID, QString &linkID, QString &rootOwnerID, QString &itemPath);
    void requestAuth(QHttpRequest *req, QHttpResponse *resp);
    void requestAuthKey(QHttpRequest *req, QHttpResponse *resp);
    void replyOffline(QHttpRequest *req, QHttpResponse *resp);
    void notFound(QHttpResponse *resp);
	void notAllowed(QHttpResponse *resp);
	void setupNetwork(QHttpServer* server, QSettings *settings, QString settingsSection);
    void setupNotifications(QSettings *settings, QString settingsSection);
    RootResource* addRootFor(QString login);
    void removeRootFor(QString login);
    void removeSharedRoot(QString sharedRootId);
    void createShareForUser(RootResource *userRoot, QString linkID, QString shareName, QString remotePath, QString owner, QString version, bool writable, QVariantHash attrs = QVariantHash());
    void sharedSiteAdded(QString user, QString linkID, QString siteName, QString owner);
    void recomputeAgentAttributes(RootResource *userRoot, bool updateAgent = true);

    //BasicAuth for integration
    bool isAuthRequest(QHttpRequest *req);
	void sendAuthOkResponse(QHttpResponse *resp);
	void performRequest(QHttpRequest *req, QHttpResponse *resp, bool isPublic, QString userID,  QString rootOwnerID, RootResource *rootResource);
    void shutdown();
    void disconnectNotifiers(QString user, QPointer<HttpHandler> handler);
    void addSharedLink(RootResource *root, QString shareName, QString linkId, QString itemPath, bool writable);
};

#endif // HTTPBACKEND_H
