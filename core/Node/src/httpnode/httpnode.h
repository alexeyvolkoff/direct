#ifndef HTTPNODE_H
#define HTTPNODE_H

#include <QObject>
#include <QFileSystemWatcher>
#include <QSettings>
#include <QHostInfo>
#include <QMimeDatabase>
#include <QTextStream>
#include <QThread>
#include <QThreadPool>
#include <QRunnable>


#include "onmydisklinkclient.h"
#include "omdnode.h"
#include "omdclient.h"
#include "hostresource.h"
#include "localresource.h"
#include "rootresource.h"
#include "httphandler.h"

#if !defined(P2P_ENABLED) && !defined(NO_WEB_UI)
#include <QWebChannel>
#include <QWebSocketServer>
#include "qhttpserver.hpp"
#include "qhttpserverconnection.hpp"
#include "qhttpserverrequest.hpp"
#include "qhttpserverresponse.hpp"
#else
#include "qhttpserver.hpp"
#include "qhttpserverconnection.hpp"
#include "qhttpserverrequest.hpp"
#include "qhttpserverresponse.hpp"
#endif

#include "onmydisklink_global.h"
#include "remoteresource.h"
#include <QThread>
#include "iauthorization.h"
#include "sessionauth.h"
#if !defined(P2P_ENABLED) && !defined(NO_WEB_UI)
#include "httpdevicebackend.h"
#endif
#include "crawler.h"
#include "ssdpannouncer.h"


using namespace qhttp;
using namespace server;


class OnMyDiskHttpNode : public OnMyDiskNode
{
    Q_OBJECT
public:
    explicit OnMyDiskHttpNode(QString configFile, QObject *parent = nullptr);
    ~OnMyDiskHttpNode();
    bool isListening();
    quint16 serverPort();

    void close();

private slots:
    void handleRequest(QPointer<QHttpRequest> req, QPointer<QHttpResponse> resp);
	void cleanUp();
    void shareAdded(QString shareName);
    void shareRemoved(QString shareName);
    void settingsLoaded(const OnMyDiskSettings &settings);
private:

    QStringList m_indexFiles;
    QStringList m_sharedIndexFiles;
    bool m_autoIndex;
    quint64 m_maxDiskCacheSize;
    quint64 m_maxRamCacheSize;
    quint64 m_maxFileSize;
    QHttpServer m_fileServer;
    QString m_serverString;
    OnMyDiskSettings m_omdSettings;

    QList<IAuthorization*> m_authProviders;

    bool m_keyLogin;
    bool m_searchEnabled;

    QStringList m_nonAuthDestinations;

    //websocket server
#if !defined(P2P_ENABLED) && !defined(NO_WEB_UI)
    QWebSocketServer m_wsServer;
    HttpDeviceBackend m_clientBackend;
#endif
    SessionAuth m_sessionProvider;
    OnMyDiskLinkClient m_linkClient;
    OnMyDiskClient m_client;
    SsdpAnnouncer* m_ssdpAnnouncer = nullptr;

    Crawler& m_crawler;

    AuthResult CheckAuth(QHttpRequest *req, QHttpResponse *resp, QString &userid, QString &rootOwnerID, QString &itemPath);
    void RequestAuth(QHttpRequest *req, QHttpResponse *resp);
    void setupNetwork(QHttpServer* server, QSettings *settings, QString settingsSection);
#if !defined(P2P_ENABLED) && !defined(NO_WEB_UI)
    void setupNotifications(QSettings *settings, QString settingsSection);
#endif
	void performRequest(QHttpRequest *req, QHttpResponse *resp, HttpHandler *handler);
#if !defined(P2P_ENABLED) && !defined(NO_WEB_UI)
	void enablePort(int port);
#endif
    void sendClaimPage(QHttpRequest *req, QHttpResponse *resp);
    void sendPairInfo(QHttpRequest *req, QHttpResponse *resp);

signals:
    void workRequested();

};

#endif // HTTPBACKEND_H
