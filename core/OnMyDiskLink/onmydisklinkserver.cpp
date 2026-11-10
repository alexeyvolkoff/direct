#include "onmydisklinkserver.h"
#include "onmydisklink.h"
#include "userevents.h"
#include "cloudresource.h"
#include <QSslCertificate>
#include <QSslCipher>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDateTime>

// Web signaling queue TTL: messages not picked up by the peer are dropped.
#define WEB_SIGNAL_TTL_MS (5 * 60 * 1000)
// Web device owner binding idle TTL: another session may take over the
// linkId only after the previous owner has been inactive this long.
#define WEB_DEVICE_IDLE_TTL_MS (10 * 60 * 1000)
// Tunnel idle TTL: tunnels without relayed traffic are dropped so a dead
// P2P session cannot block reconnects with fresh tunnel ids.
#define TUNNEL_IDLE_TTL_MS (10 * 60 * 1000)
// Sweep interval for expired web state.
#define WEB_SWEEP_INTERVAL_MS (60 * 1000)

// Raw AI-service ports reachable through web-initiated tunnels for sessions
// that are not the node owner / not in sharedWith (see sendWebSignaling).
// Inference ports additionally require the target to be the PUBLIC AI node.
constexpr quint16 CHAT_TUNNEL_PORT = 4000;
constexpr quint16 OLLAMA_TUNNEL_PORT = 11434;
constexpr quint16 COMFY_TUNNEL_PORT = 8188;


OnMyDiskLinkServer::OnMyDiskLinkServer(): QTcpServer(nullptr)
{
    qRegisterMetaType<QAbstractSocket::SocketError>("QAbstractSocket::SocketError");
    qRegisterMetaType<LinkMessage::LinkMessageType>("LinkMessage::LinkMessageType");
    qRegisterMetaType<LinkMessage::LinkMessageResult>("LinkMessage::LinkMessageResult");
    qRegisterMetaType<LinkMessage>("LinkMessage");
    qRegisterMetaType<SiteInfo>("SiteInfo");
    qRegisterMetaType<SitesHash>("SitesHash");
}

void OnMyDiskLinkServer::init(QSettings *settings, PKIHelper *pkiHelper)
{
    m_settings = settings;
    m_pkiHelper = pkiHelper;
    m_port = m_settings->value("Network/port", 9000).toUInt();
    //Key login mode
    m_keyLogin =  m_settings->value(QStringLiteral("keyLogin"), false).toBool();
    m_publicAiNodeID =  m_settings->value(QStringLiteral("publicAiNode"), "ffc3f840f25dae37d1456f1be0e7e8c6").toString();


    if (QSslSocket::supportsSsl()) {
        setupSSL();
    } else
        qCritical() << "SSL not supported.";

    if (listen(QHostAddress::AnyIPv4, m_port)) {
        qInfo() << "Accepting links on port" <<  serverPort();
    } else
        qCritical() << "Can't start link server on port" << m_port;

    m_plainServer.init(settings);
    connect(&m_plainServer, &PlainSocketServer::plainSocketReady, this, &OnMyDiskLinkServer::linkSocketReady);

    // Periodic cleanup of expired web signaling state.
    QTimer *sweepTimer = new QTimer(this);
    connect(sweepTimer, &QTimer::timeout, this, &OnMyDiskLinkServer::sweepWebState);
    sweepTimer->start(WEB_SWEEP_INTERVAL_MS);
}


OnMyDiskLinkServer::~OnMyDiskLinkServer()
{
    //let it burn.
   disconnectLinks();
}

QPointer<OnMyDiskLink> OnMyDiskLinkServer::getLink(const QString &linkID)
{
    OnMyDiskLocker lock(&m_mutex);
    if (! lock.isLocked())
        qWarning() << "Can not lock in OnMyDiskLinkServer::getLink()" ;

    OnMyDiskLink * link = nullptr;

    if (m_links.contains(linkID) )
    {
        link = m_links[linkID];

        if (link->state() == OnMyDiskLink::Disconnected)
        {
            qCritical() << "Requested link disconnected:" << linkID;
            return nullptr;
        }
    }

    return  link;
}

bool OnMyDiskLinkServer::getSite(const QString &siteName, SiteInfo &site)
{
    QMutexLocker locker(&m_mutex);
    if (m_sites.contains(siteName)) {
       site = m_sites[siteName];
       return true;
    } else
       return false;
}

bool OnMyDiskLinkServer::getSiteByDomain(const QString &domain, SiteInfo &site)
{
    QMutexLocker locker(&m_mutex);
    foreach(const QString& siteName, m_sites.keys()) {
        if (m_sites[siteName].domain == domain) {
            site = m_sites[siteName];
            return true;
        }
    }
    return false;
}

SitesHash OnMyDiskLinkServer::getSearchNodes()
{
    QMutexLocker locker(&m_mutex);
    return m_searchNodes;
}

bool OnMyDiskLinkServer::getChatNode(const QString &user, SiteInfo &site)
{
    QMutexLocker locker(&m_mutex);
    
    // Try to find by user first (direct match)
    if (m_aiNodes.contains(user)) {
       site = m_aiNodes.value(user); // returns the most recently inserted one
       return true;
    } 

    // Try to find a node shared with this user
    if (!user.isEmpty()) {
        Q_FOREACH(const SiteInfo& siteInfo, m_aiNodes) {
            if (siteInfo.users.contains(user)) {
                site = siteInfo;
                return true;
            }
        }
    }

    // Fallback: check all AI nodes for the public node ID or nodes shared with everyone ("*")
    Q_FOREACH(const SiteInfo& siteInfo, m_aiNodes) {
        if (siteInfo.linkID == m_publicAiNodeID || siteInfo.users.contains("*")) {
            site = siteInfo;
            return true;
        }
    }

    return false;
}

// Resolve the system public AI node (On My Chat Public Node):
// either the node whose linkID matches the gateway's publicAiNode setting
// (empty setting disables the feature), or any AI node announced with
// users == "*" (shared with everyone). Present in m_aiNodes only while
// the node is connected, so a positive answer means the node is online.
bool OnMyDiskLinkServer::getPublicAiNode(SiteInfo &site)
{
    QMutexLocker locker(&m_mutex);

    if (m_publicAiNodeID.isEmpty())
        return false;

    Q_FOREACH(const SiteInfo& siteInfo, m_aiNodes) {
        if (siteInfo.linkID == m_publicAiNodeID || siteInfo.users.contains("*")) {
            site = siteInfo;
            return true;
        }
    }

    return false;
}

void OnMyDiskLinkServer::checkLinks()
{
    //no-op: the legacy worker-thread cleanup is gone
}

void OnMyDiskLinkServer::disconnectLinks()
{
    //let it burn.
    Q_FOREACH(OnMyDiskLink *link, m_links)
    {
        m_links.remove(link->linkID());
        link->disconnectLink();
        delete link;
    }
    m_links.clear();
}

OnMyDiskLinkServer &OnMyDiskLinkServer::instance()
{
  static OnMyDiskLinkServer linkServer;
  return linkServer;
}

bool OnMyDiskLinkServer::setupSSL()
{
    //CA
    m_CAPath = m_settings->value("PKI/CACert", "rootCA.crt").toString();
    m_legacyCAPath = m_settings->value("PKI/LegacyCA", "").toString();

    //Link certificate
    m_CertPath = m_settings->value("Network/Cert", "link.crt").toString();
    m_KeyPath =  m_settings->value("Network/pKey", "link.key").toString();
    m_KeyPass =  m_settings->value("Network/pKeyPass", "onmydisk").toString();

    //Load legacy CA
    if (! m_legacyCaLoaded && !m_legacyCAPath.isEmpty() ) {
        QList<QSslCertificate> legacyCaCerts = QSslCertificate::fromPath(m_legacyCAPath, QSsl::Pem, QSslCertificate::PatternSyntax::Wildcard);
        if (legacyCaCerts.count() > 0) {
            m_sslConfig.addCaCertificates(legacyCaCerts);
            m_legacyCaLoaded = true;
        }
    }

    // Load CA
    if (!m_caLoaded)
    {
        QFile caFile(m_CAPath);
        if (caFile.open(QIODevice::ReadOnly))
        {
            QList<QSslCertificate> caCerts = QSslCertificate::fromDevice(&caFile);
            caFile.close();

            if (caCerts.count()) {
                Q_FOREACH(QSslCertificate ca, caCerts)
                {
                    if (ca.isNull()) {
                        break;
                    }
                }
                m_sslConfig.addCaCertificates(caCerts);
                m_caLoaded = true;
            }

            if (! m_caLoaded) {
                qWarning() << "Missing or invalid root CA";
                QTimer::singleShot(LINK_CONFIG_RELOAD_TIMEOUT, this,  [this]() {
                    m_pkiHelper->initPKI();
                    setupSSL();
                });
                return false;
            }
        }
        else
        {
            qWarning() << "Creating root CA";
            m_pkiHelper->initPKI();
            QTimer::singleShot(LINK_CONFIG_RELOAD_TIMEOUT, this, [this]() {
                setupSSL();
            });
            return false;
        }
    }

    QFile keyFile(m_KeyPath);
    if (keyFile.open(QIODevice::ReadOnly))
    {
        QSslKey privateKey(&keyFile, QSsl::Rsa, QSsl::Pem, QSsl::PrivateKey, m_KeyPass.toLatin1());
        keyFile.close();
        if (privateKey.isNull())
        {
            qWarning() << "Can not load link private key";
            QTimer::singleShot(LINK_CONFIG_RELOAD_TIMEOUT, this, [this]() {
                createLinkCert();
                setupSSL();
            });
            return false;
        }
        m_sslConfig.setPrivateKey(privateKey);
    }
    else
    {
        qWarning() << "Creating link private key";
        createLinkCert();
        QTimer::singleShot(LINK_CONFIG_RELOAD_TIMEOUT, this, [this]() {
            setupSSL();
        });
        return false;
    }

    QFile certFile(m_CertPath);
    if (certFile.open(QIODevice::ReadOnly))
    {
        QList<QSslCertificate> certs = QSslCertificate::fromDevice(&certFile);
        certFile.close();

        if (certs.count() == 0 || certs.at(0).isNull() ) {
            qWarning() << "Can not load link certificate";
            QTimer::singleShot(LINK_CONFIG_RELOAD_TIMEOUT, this, [this]() {
                createLinkCert();
                setupSSL();
            });
            return false;
        }

        m_sslConfig.setLocalCertificate(certs.at(0));
    }
    else
    {
        qWarning() << "Missing link certificate";
        QTimer::singleShot(LINK_CONFIG_RELOAD_TIMEOUT, this, [this]() {
            createLinkCert();
            setupSSL();
        });
        return false;
    }

    m_sslConfig.setPeerVerifyMode(QSslSocket::QueryPeer);
    m_sslConfig.setProtocol(QSsl::TlsV1_2);

    return true;
}


void OnMyDiskLinkServer::initLinkSocket(LinkSocket *socket)
{
    socket->setEmitMessages(true);
    connect(socket, &LinkSocket::newMessage, this, &OnMyDiskLinkServer::newServiceMessage);
    connect(socket, &LinkSocket::disconnected, this, &OnMyDiskLinkServer::clientDisconnected);
}


void OnMyDiskLinkServer::linkSocketReady(LinkSocket *socket)
{
    QMutexLocker locker(&m_mutex);
    QString linkid = socket->getPeerID();
    //replace linkid with persistent id
    if (m_persistentIDs.contains(linkid))
        linkid = m_persistentIDs[linkid];
    disconnect(socket, &QSslSocket::errorOccurred, 0, 0); //disconnect
    if (!m_links.contains(linkid))
    {
        // Plain (non-SSL) data socket arrived before control socket registered persistent linkID
        if (socket->sslConfiguration().localCertificate().isNull()) {
            qInfo() << "Queueing pending data socket for link" << linkid;
            m_pendingLinks[linkid].append(socket);
            connect(socket, &LinkSocket::disconnected, this, [this, socket]() {
                QMutexLocker l(&m_mutex);
                for (auto it = m_pendingLinks.begin(); it != m_pendingLinks.end(); ++it)
                    it->removeAll(socket);
                l.unlock();
                socket->deleteLater();
            });
            locker.unlock();
            return;
        }

        QString user = socket->getUser();

        // Tunnel-only clients (omd-tunnel) present a self-signed cert with O=omd-tunnel.
        // They must not register as devices, create user roots, or trigger link events.
        bool tunnelOnly = false;
        QList<QString> org = socket->peerCertificate().subjectInfo(QSslCertificate::Organization);
        Q_FOREACH(QString o, org)
        {
            if (o.compare("omd-tunnel", Qt::CaseInsensitive) == 0) {
                tunnelOnly = true;
                break;
            }
        }

        OnMyDiskLink *link = new  OnMyDiskLink(linkid, user, socket);
        link->setTunnelOnly(tunnelOnly);
        if (!tunnelOnly)
            qInfo() << "Link created:" << linkid << "user:" << user;
        m_links.insert(linkid, link);
        locker.unlock();

        connect(link, &OnMyDiskLink::linkInactive, this, [this, linkid, user]{
              emit linkInactive(linkid, user);
        });


        // Init link
        initLinkSocket(socket);
        link->initLink(socket);
        if (!tunnelOnly)
            emit linkConnected(linkid, socket->getUser());
    }
    else
    {
        OnMyDiskLink* link = m_links[linkid];

        if (link->state() == OnMyDiskLink::Disconnected || link->state() == OnMyDiskLink::Inactive || link->linkSocket()->state() != QAbstractSocket::ConnectedState) {
            //Re-init link
            initLinkSocket(socket);
            link->initLink(socket);
            emit linkConnected(linkid, socket->getUser());
        } else {
            //Single-socket links: any unexpected extra socket is rejected
            qWarning() << "Unexpected extra data socket for" << linkid << "- rejecting";
            disconnect(socket, &LinkSocket::disconnected, 0, 0);
            socket->close();
            socket->deleteLater();
        }
    }
}


void OnMyDiskLinkServer::incomingConnection(qintptr handle)
{
    LinkSocket* client = new LinkSocket(false);
    client->setSslConfiguration(m_sslConfig);
	client->setSocketDescriptor(handle);
    //qDebug() << "Connecting..." << client;
    connect(client, &LinkSocket::connected, this, [this,  client]() {
        client->setSocketOption(QAbstractSocket::LowDelayOption, 1);
        client->setSocketOption(QAbstractSocket::SendBufferSizeSocketOption, CloudResource::m_maxFileSize * 2);
        client->setReadBufferSize(CloudResource::m_maxFileSize * 2);
        client->setKeepAlive();
    });

	connect(client, &LinkSocket::encrypted, this, [this,  client]() {

        QString uid = client->getUser();
        QString linkId = client->getPeerID();

         //block abusers here
        if (uid == "" || linkId == "489642b17c005b5ca654177aed423b22")
		{
            //qCritical() << "Bad certificate" << clientID;
			LinkMessage message;
			message.setMessageType(LinkMessage::BadCertificate);
			client->writeMessage(message);

			QTimer::singleShot(PING_INTERVAL, [client]() {
				if (client->state() == QAbstractSocket::ConnectedState)
					client->disconnectFromHost();
                client->deleteLater();
            });

			return;
		}

        linkSocketReady(client);
	});

    connect(client,  static_cast<void (QSslSocket::*)(const QList<QSslError>&)>(&QSslSocket::sslErrors), client, [this, client] (const QList<QSslError> &errors)  {
        //Q_FOREACH(QSslError err, errors)
        //    qCritical() << "sslErrorSignal" << err;
        Q_UNUSED(errors)
        client->ignoreSslErrors();
    });

    connect(client, &QSslSocket::errorOccurred, client, [this, client] (QAbstractSocket::SocketError err)  {
        Q_UNUSED(err)
        if (! client->isEncrypted()) {
            QString id = client->getPeerID(); //just check if was encrypted
            if (id.isEmpty()) {
                QTimer::singleShot(PING_INTERVAL, [client]() {
                    if (client->state() == QAbstractSocket::ConnectedState)
                        client->disconnectFromHost();
                    client->deleteLater();
                });
            }
        }
    });

	client->startServerEncryption();
}


void OnMyDiskLinkServer::createLinkCert()
{
    //generate private key
    qInfo() << "Generating new link private key";
    QFileInfo keyFileInfo(m_KeyPath);
    if (keyFileInfo.exists())
        keyFileInfo.absoluteDir().remove(keyFileInfo.fileName());

    if (!m_pkiHelper->genPrivateKeyToFile(2048, m_KeyPath, m_KeyPass.toLocal8Bit().data()))
    {
        qCritical() << "Can't generate new link server private key.";
    }

    qInfo() << "Creating new link certificate";
    bool isOk = false;
    QByteArray CSR = m_pkiHelper->genCSRToBytes(isOk, m_KeyPath, m_KeyPass.toLocal8Bit().data(), "link");

    if (CSR.count() == 0)
    {
         qCritical() << "Can't generate new link CSR.";
    }

    QByteArray linkCertBody;
    if (! m_pkiHelper->signCSR(CSR, 1, "link", linkCertBody))
    {
         qCritical() << "Can't sign CSR for link server";
    }

    QFile certFile(m_CertPath);
    if (certFile.open(QIODevice::WriteOnly))
    {
         certFile.write(linkCertBody);
         certFile.close();
         qInfo() << "Link server certificate created";
    } else {
         qCritical() << "Can't save signed link server certificate to " << m_CertPath;
    }
}

void OnMyDiskLinkServer::clientDisconnected()
{
    LinkSocket* socket = static_cast<LinkSocket*>( sender());
    QMutexLocker locker(&m_mutex);

	QString linkID = socket->getPeerID();
    if (m_persistentIDs.contains(linkID)) {
        QString persistentID = m_persistentIDs[linkID];
        m_persistentIDs.remove(linkID);
        linkID = persistentID;
    }

	if (m_links.contains(linkID))
    {
		OnMyDiskLink* link = m_links[linkID];

        // remove sites on this link
        foreach(const QString& key, m_sites.keys()) {
            SiteInfo site = m_sites[key];
            if (site.linkID == link->linkID())
                 m_sites.remove(key);
        }
        // remove search node on this link
        foreach(const QString& key, m_searchNodes.keys()) {
            SiteInfo site = m_searchNodes[key];
            if (site.linkID == link->linkID())
                 m_searchNodes.remove(key);
        }

        // remove ai node on this link
        QMutableHashIterator<QString, SiteInfo> ai_it(m_aiNodes);
        while (ai_it.hasNext()) {
            ai_it.next();
            if (ai_it.value().linkID == link->linkID())
                ai_it.remove();
        }

        m_links.remove(linkID);

        //Close tunnels involving this link and notify the other end
        QMutableHashIterator<QString, QPair<QString, QString>> it(m_tunnels);
        while (it.hasNext()) {
            it.next();
            QString otherID = "";
            if (it.value().first == linkID)
                otherID = it.value().second;
            else if (it.value().second == linkID)
                otherID = it.value().first;
            if (!otherID.isEmpty())
            {
                LinkMessage close;
                close.setMessageType(LinkMessage::TunnelClose);
                close.setResultCode(LinkMessage::OK);
                close.insertDataValue("ID", it.key());
                if (m_links.contains(otherID))
                    m_links[otherID]->linkSocket()->writeMessage(close);
                m_tunnelCreator.remove(it.key());
                m_tunnelActivity.remove(it.key());
                it.remove();
            }
        }

        locker.unlock();
        link->release();      


        emit linkDisconnected(linkID, link->user());
	}
}


void OnMyDiskLinkServer::newServiceMessage(LinkSocket *serverSocket, const LinkMessage& message)
{
    // Tunnel-only links (omd-tunnel) may only speak the tunnel protocol;
    // anything else must not reach the share/root/DB handling below.
    {
        QMutexLocker locker(&m_mutex);
        QString linkid = serverSocket->getPeerID();
        if (m_persistentIDs.contains(linkid))
            linkid = m_persistentIDs[linkid];
        OnMyDiskLink *link = m_links.value(linkid);
        if (link && link->tunnelOnly())
        {
            switch (message.getMessageType())
            {
            case LinkMessage::TunnelOpen:
            case LinkMessage::TunnelData:
            case LinkMessage::TunnelClose:
            case LinkMessage::TunnelSignaling:
            case LinkMessage::Ping:
            case LinkMessage::PingReply:
                break;
            default:
                qWarning() << "Ignoring" << message.getMessageType() << "from tunnel-only link" << linkid;
                return;
            }
        }
    }

    switch (message.getMessageType()) {
		case LinkMessage::FuseMounted:
		case LinkMessage::BadCertificate:
		case LinkMessage::SwitchPort:
		case LinkMessage::Null:
		case LinkMessage::PingReply:
		case LinkMessage::ConnectDataSockets:
		case LinkMessage::FilePos:
		case LinkMessage::FileInfo:
		case LinkMessage::FileSeek:
		case LinkMessage::MkDir:
		case LinkMessage::FileOpen:
		case LinkMessage::FileClose:
		case LinkMessage::FileRead:
		case LinkMessage::FileWrite:
		case LinkMessage::Remove:
			break;
		case LinkMessage::Ping:
        {
            //qInfo() << "Ping message recieved";
            //stop timer
            QMutexLocker locker(&m_mutex);
            QString linkID = serverSocket->getPeerID();
            if (m_persistentIDs.contains(linkID))
                linkID = m_persistentIDs[linkID];

            if (m_links.contains(linkID))
            {
                OnMyDiskLink* link = m_links[linkID];
                link->timer().start();
               // LinkMessage reply;
               // reply.setMessageType(LinkMessage::PingReply);
               // reply.setResultCode(LinkMessage::OK);
                locker.unlock();
                //serverSocket->writeMessage(reply);
            } else {
                qCritical() << "Ping: link not found" << linkID;
                serverSocket->close();
            }
        }
        break;

    case LinkMessage::RPC:
    case LinkMessage::RemoveShare:
    case LinkMessage::AddShare:
    case LinkMessage::UpdateShare: {
        QMutexLocker locker(&m_mutex);
         //replace linkid with persistent id
        QString linkid = serverSocket->getPeerID();
        if (m_persistentIDs.contains(linkid))
            linkid = m_persistentIDs[linkid];
        if ( m_links.contains(linkid))
        {
            OnMyDiskLink* link = m_links[linkid];
            QHash<QString, QVariant> data = message.getData();

            if (data.contains("version") )
            {
                link->setLinkVersion(data["version"].toString());
            }

            if (data.contains("sharename"))
            {
                //qInfo() << "Got UpdateShare message for " << message->getData()["sharename"].toString();
                link->setShareName(data["sharename"].toString());
            }

            if (data.contains("sharedWith"))
            {
                QStringList users = data["sharedWith"].toString().split(",", Qt::SplitBehaviorFlags::SkipEmptyParts);
                link->setSharedWith(users);
                qInfo() << "Shared with" << link->linkID() << ":" << users.join(",");
            }

            if (data.contains("localSearch") && data["localSearch"].toBool()) {
                SiteInfo searchNode;
                searchNode.linkID = link->linkID();
                searchNode.shareName = link->shareName();
                searchNode.owner = link->user();
                searchNode.users = link->sharedWith();
                m_searchNodes[link->linkID()] = searchNode;
                link->setSearchSupported(true);
            }

            //save the desired persistentID for this physical ID
            if (data.contains("linkID")) {
                if (data["linkID"].toString() != serverSocket->getPeerID()) {
                    linkid = data["linkID"].toString();
                    qInfo() << "Replacing" << serverSocket->getPeerID() <<  "with" << linkid;
                    if (!m_persistentIDs.contains(linkid)) {
                        if (m_links.contains(linkid) && m_links[linkid]->state() == OnMyDiskLink::Disconnected) {
                            m_links.remove(linkid);
                        }
                    }
                    if (!m_links.keys().contains(linkid) && !m_persistentIDs.contains(linkid)) {
                        m_persistentIDs[serverSocket->getPeerID()] = linkid;
                        m_links.remove(serverSocket->getPeerID());
                        link->setLinkID(linkid);
                        m_links[linkid] = link;
                    }
                    // Single-socket links: pending plain data sockets are rejected up front
                    if (m_pendingLinks.contains(linkid)) {
                        QList<LinkSocket*> pending = m_pendingLinks.take(linkid);
                        for (LinkSocket* s : pending) {
                            disconnect(s, &LinkSocket::disconnected, 0, 0);
                            s->close();
                            s->deleteLater();
                        }
                    }
                }
            }
            if (data.contains("localAI") &&  data["localAI"].toBool())
            {
                SiteInfo site;
                site.linkID = link->linkID();
                site.shareName = link->shareName();
                site.name = link->shareName();
                site.owner = link->user();
                QString shareTo = data["shareTo"].toString();
                site.users = shareTo.split(",",  Qt::SplitBehaviorFlags::SkipEmptyParts);
                qWarning() << "Registering AI node for user:" << link->user() << "LinkID:" << link->linkID();
                // Remove existing entry for this specific LinkID to avoid duplicates in MultiHash
                QMutableHashIterator<QString, SiteInfo> ai_it(m_aiNodes);
                while (ai_it.hasNext()) {
                    ai_it.next();
                    if (ai_it.value().linkID == site.linkID)
                        ai_it.remove();
                }
                m_aiNodes.insert(link->user(), site);
                link->setSearchSupported(true);
                qInfo() << "Registered AI node for user:" << link->user() << "LinkID:" << site.linkID;

                if (site.linkID == m_publicAiNodeID) {
                     qInfo() << "This is the PUBLIC AI node.";
                }
            }

            if (data.contains("sites"))
            {
                QVariantHash sites = data["sites"].toHash();

                //remove removed sites
                foreach(const QString& key, m_sites.keys()) {
                    if (m_sites[key].linkID == link->linkID() && !sites.contains(key) )
                        m_sites.remove(key);
                }

                //add/update sites
                foreach(QString key, sites.keys()) {
                        SiteInfo site;
                        site.linkID = link->linkID();
                        site.shareName = link->shareName();
                        site.name = key;
                        site.owner = link->user();
                        // Domain linking
                        QHash<QString, QVariant> attrs = sites[key].toHash();
                        site.domain = attrs.contains("domain") ? attrs["domain"].toString() : "";
                        m_sites[key] = site;
                }
            }

            locker.unlock();
            if (data.contains("sharename"))
                emit shareUpdated(linkid, link->user(), data["sharename"].toString(), data, message.getEntryData());


            if (data.contains("siteUpdated")) {
                emit UserEvents::instance().siteUpdated(link->user(), data["siteUpdated"].toString(), false);
            }

            if (data.contains("path")) {
                QString path = data["path"].toString();
                QStringList pathList = path.split("/", Qt::SplitBehaviorFlags::SkipEmptyParts);
                QString user = pathList.count() ? pathList.first() :  link->user();
                emit UserEvents::instance().userEvent(user, "indexing_completed", QJsonObject{{"path", path}});
            }

            link->startTimers();
        }
        break;
    }
    case LinkMessage::GetPublicLink:
        if (message.getData().contains("itemPath"))
        {
            //qInfo() << "Public Link created for" << message->getData();
            emit publicLinkAdded(serverSocket->getPeerID(), message.getData());
        }
        break;

    case LinkMessage::TunnelOpen:
    case LinkMessage::TunnelData:
    case LinkMessage::TunnelClose:
    case LinkMessage::TunnelSignaling: {
        QMutexLocker locker(&m_mutex);

        QString fromID = serverSocket->getPeerID();
        if (m_persistentIDs.contains(fromID))
            fromID = m_persistentIDs[fromID];

        QString tunnelID = message.getData().value("ID").toString();
        if (tunnelID.isEmpty())
            break;

        if (message.getMessageType() == LinkMessage::TunnelOpen)
        {
            if (m_tunnels.contains(tunnelID) && fromID == m_tunnels[tunnelID].second)
            {
                //reply from target device, forward to initiator
                QString initiatorID = m_tunnels[tunnelID].first;
                if (m_links.contains(initiatorID))
                    m_links[initiatorID]->linkSocket()->writeMessage(message);
                break;
            }

            QHash<QString, QVariant> data = message.getData();
            if (!data.contains("linkID") || !data.contains("port"))
            {
                LinkMessage reply = message;
                reply.setResultCode(LinkMessage::BadData);
                reply.setErrorMsg("TunnelOpen requires 'linkID' and 'port' attributes");
                serverSocket->writeMessage(reply);
                break;
            }

            QString targetID = data["linkID"].toString();
            if (!m_links.contains(targetID))
            {
                // Check if target is a registered web device
                if (isWebDevice(targetID)) {
                    // Authorization: same rule as for TLS targets below - only
                    // the owner may open a tunnel to the device.
                    if (m_links.contains(fromID))
                    {
                        const QString initiatorUser = m_links[fromID]->user();
                        const QString targetOwner = m_webDeviceOwner.value(targetID);
                        if (targetOwner.isEmpty() || initiatorUser != targetOwner)
                        {
                            LinkMessage reply = message;
                            reply.setResultCode(LinkMessage::LinkError);
                            reply.setErrorMsg("Tunnel access denied: " + initiatorUser + " is not allowed to connect to " + targetID);
                            serverSocket->writeMessage(reply);
                            qInfo() << "Tunnel access denied for" << initiatorUser << "to web device" << targetID;
                            break;
                        }
                    }

                    QJsonObject sigPayload;
                    sigPayload["ID"] = tunnelID;
                    sigPayload["linkID"] = targetID;
                    sigPayload["port"] = data["port"].toInt();
                    sigPayload["service"] = data["service"].toString();

                    WebSignal sig;
                    sig.tunnelID = tunnelID;
                    sig.kind = "tunnel-open";
                    sig.payload = QJsonDocument(sigPayload).toJson(QJsonDocument::Compact);
                    sig.timestamp = static_cast<quint64>(QDateTime::currentMSecsSinceEpoch());

                    m_tunnels[tunnelID] = QPair<QString, QString>(fromID, targetID);
                    m_tunnelActivity[tunnelID] = static_cast<quint64>(QDateTime::currentMSecsSinceEpoch());
                    if (m_links.contains(fromID))
                        m_tunnelCreator[tunnelID] = m_links[fromID]->user();

                    // Always queue for HTTP polling
                    {
                        QMutexLocker webLocker(&m_webMutex);
                        m_webSignals[tunnelID].append(sig);
                        m_webWaitCondition.wakeAll();
                        qDebug() << "[Gateway] Queued TunnelOpen for web device:" << targetID << "tunnel:" << tunnelID;
                    }

                    // Also deliver via WebSocket channel if available
                    pushWebSignal(targetID, tunnelID, sig.kind, sig.payload);
                    break;
                }

                LinkMessage reply = message;
                reply.setResultCode(LinkMessage::LinkError);
                reply.setErrorMsg("Target link not connected: " + targetID);
                serverSocket->writeMessage(reply);
                break;
            }

            // Authorization: only the target owner or a user from the owner's
            // sharedWith list may open a tunnel to the target device.
            if (m_links.contains(fromID))
            {
                const QString initiatorUser = m_links[fromID]->user();
                OnMyDiskLink* target = m_links[targetID];
                const QStringList allowedUsers = target->sharedWith();
                const bool isOwner = (initiatorUser == target->user());
                if (!isOwner && !allowedUsers.contains(initiatorUser))
                {
                    LinkMessage reply = message;
                    reply.setResultCode(LinkMessage::LinkError);
                    reply.setErrorMsg("Tunnel access denied: " + initiatorUser + " is not allowed to connect to " + targetID);
                    serverSocket->writeMessage(reply);
                    qInfo() << "Tunnel access denied for" << initiatorUser << "to" << targetID;
                    break;
                }
            }

            m_tunnels[tunnelID] = QPair<QString, QString>(fromID, targetID);
            m_tunnelActivity[tunnelID] = static_cast<quint64>(QDateTime::currentMSecsSinceEpoch());
            qDebug() << "Tunnel open" << tunnelID << "from" << fromID << "to" << targetID;
            // Relay validated caller identity: the target node re-writes the
            // port-tunnel requests towards its local AI service with these
            // trusted headers, so the service never trusts raw client headers.
            if (m_links.contains(fromID)) {
                LinkMessage msg = message; // insertDataValue is non-const
                const QString initiatorUser = m_links[fromID]->user();
                OnMyDiskLink* targetForId = m_links[targetID];
                const bool callerIsOwner = (initiatorUser == targetForId->user());
                const bool callerIsShared = targetForId->sharedWith().contains(initiatorUser);
                msg.insertDataValue("X-OMD-Caller-User", initiatorUser);
                msg.insertDataValue("X-OMD-Caller-Role",
                    callerIsOwner ? QStringLiteral("owner")
                    : callerIsShared ? QStringLiteral("shared")
                    : QStringLiteral("guest"));
                msg.insertDataValue("X-OMD-Target-Owner", targetForId->user());
                m_links[targetID]->linkSocket()->writeMessage(msg);
            } else {
                m_links[targetID]->linkSocket()->writeMessage(message);
            }
            break;
        }

        if (!m_tunnels.contains(tunnelID))
            break;

        QPair<QString, QString> ends = m_tunnels[tunnelID];
        QString otherID = (fromID == ends.first) ? ends.second : ends.first;

        // Track activity so idle/dead tunnels can be swept
        m_tunnelActivity[tunnelID] = static_cast<quint64>(QDateTime::currentMSecsSinceEpoch());

        if (message.getMessageType() == LinkMessage::TunnelClose)
        {
            m_tunnels.remove(tunnelID);
            m_tunnelCreator.remove(tunnelID);
            qInfo() << "[Gateway] Tunnel closed by device:" << tunnelID << "kind:" << message.getData().value("kind").toString();
        }

        if (m_links.contains(otherID))
            m_links[otherID]->linkSocket()->writeMessage(message);
        else if (otherID == QStringLiteral("web") || isWebDevice(otherID))
        {
            QString kind = message.getData().value("kind").toString();
            QByteArray payload = message.getPayLoad();
            QString creator = m_tunnelCreator.value(tunnelID);
            QString targetWebId = (!creator.isEmpty()) ? creator : otherID;

            // Always queue for HTTP polling so web clients polling /api/signaling
            // (webrtc-drive.js) reliably receive answers and ICE candidates
            {
                QMutexLocker webLocker(&m_webMutex);
                WebSignal sig{tunnelID, kind, payload, static_cast<quint64>(QDateTime::currentMSecsSinceEpoch())};
                m_webSignals[tunnelID].append(sig);
                m_webWaitCondition.wakeAll();
                qDebug() << "[Gateway] Queued" << kind << "from C++ node for tunnel:" << tunnelID << "(for HTTP polling)";
            }

            // Also deliver via WebSocket channel if available
            if (!targetWebId.isEmpty() && pushWebSignal(targetWebId, tunnelID, kind, payload))
            {
                qDebug() << "[Gateway] Pushed" << kind << "from C++ node to web client:" << targetWebId << "tunnel:" << tunnelID;
            }
        }
        break;
    }

    default:
        break;
    }

}

bool OnMyDiskLinkServer::sendWebSignaling(const QString &targetID, const QString &tunnelID, const QString &kind, const QByteArray &payload, const QString &senderId, quint16 port, const QString &service, const QString &sessionUser)
{
    QMutexLocker locker(&m_mutex);

    // Resolve targetID: try direct linkID first, then by username, then by persistentID mapping
    QString actualTargetID = targetID;
    if (!m_links.contains(actualTargetID))
    {
        for (auto it = m_links.begin(); it != m_links.end(); ++it) {
            if (it.value() && (it.value()->user() == targetID || it.key() == targetID)) {
                actualTargetID = it.key();
                break;
            }
        }
    }
    if (!m_links.contains(actualTargetID))
    {
        for (auto it = m_persistentIDs.begin(); it != m_persistentIDs.end(); ++it) {
            if (it.key() == targetID || it.value() == targetID) {
                if (m_links.contains(it.key())) {
                    actualTargetID = it.key();
                    break;
                }
                if (m_links.contains(it.value())) {
                    actualTargetID = it.value();
                    break;
                }
            }
        }
    }

    // If the target is not in m_links, check if it is a registered web device or target
    bool targetExists = m_links.contains(actualTargetID) || isWebDevice(actualTargetID) || m_tunnels.contains(tunnelID);
    if (!targetExists)
    {
        // Target not connected — expected during reconnects; stay quiet
        return false;
    }

    // Response path: if this is an answer, tunnel-ack, or candidate from the answering device back to the offerer
    if (kind == QStringLiteral("answer") || kind == QStringLiteral("tunnel-ack") ||
        (m_tunnels.contains(tunnelID) && !senderId.isEmpty() && m_tunnelCreator.contains(tunnelID) && senderId != m_tunnelCreator.value(tunnelID)))
    {
        WebSignal sig;
        sig.tunnelID = tunnelID;
        sig.kind = kind;
        sig.payload = payload;
        sig.timestamp = static_cast<quint64>(QDateTime::currentMSecsSinceEpoch());

        {
            QMutexLocker webLocker(&m_webMutex);
            m_webSignals[tunnelID].append(sig);
            m_webWaitCondition.wakeAll();
        }
        qDebug() << "[Gateway] Queued" << kind << "for tunnel:" << tunnelID << "(for HTTP polling)";

        QString creator = m_tunnelCreator.value(tunnelID);
        if (!creator.isEmpty())
        {
            pushWebSignal(creator, tunnelID, kind, payload);
        }
        return true;
    }

    if (!m_links.contains(actualTargetID))
    {
        // Non-browser initiator (e.g. the AI node's Python P2P client) is the
        // tunnel creator but has neither a link entry nor a web-device
        // registration: it only needs to drain the signals queued for its own
        // offer. The creator is authenticated by m_tunnelCreator, not by the
        // target link, so let it poll.
        if (kind == QStringLiteral("poll") && !senderId.isEmpty()
            && m_tunnelCreator.value(tunnelID) == senderId)
        {
            m_tunnelActivity[tunnelID] = static_cast<quint64>(QDateTime::currentMSecsSinceEpoch());
            return true;
        }

        // Check if this is a registered web device
        if (isWebDevice(actualTargetID)) {
            // Register tunnel so relay routing knows both ends
            bool isNewTunnel = !m_tunnels.contains(tunnelID);
            if (isNewTunnel)
            {
                m_tunnels[tunnelID] = QPair<QString, QString>(QStringLiteral("web"), actualTargetID);
                m_tunnelCreator[tunnelID] = senderId;
                m_tunnelActivity[tunnelID] = static_cast<quint64>(QDateTime::currentMSecsSinceEpoch());
            }

            if (kind == QStringLiteral("poll"))
                return true;

            // Always queue for HTTP polling
            {
                WebSignal sig;
                sig.tunnelID = tunnelID;
                sig.kind = kind;
                sig.payload = payload;
                sig.timestamp = static_cast<quint64>(QDateTime::currentMSecsSinceEpoch());

                QMutexLocker webLocker(&m_webMutex);
                m_webSignals[tunnelID].append(sig);
                m_webWaitCondition.wakeAll();
                qDebug() << "[Gateway] Queued signaling for web device:" << actualTargetID << "tunnel:" << tunnelID;
            }

            if (pushWebSignal(actualTargetID, tunnelID, kind, payload))
            {
                m_webDeviceSeen[actualTargetID] = static_cast<quint64>(QDateTime::currentMSecsSinceEpoch());
            }
            return true;
        }
        return false;
    }

    // Mark tunnel as known (so we don't send TunnelOpen twice)
    bool isNewTunnel = !m_tunnels.contains(tunnelID);
    if (isNewTunnel)
    {
        m_tunnels[tunnelID] = QPair<QString, QString>(QStringLiteral("web"), actualTargetID);
        m_tunnelCreator[tunnelID] = senderId;
        m_tunnelActivity[tunnelID] = static_cast<quint64>(QDateTime::currentMSecsSinceEpoch());
    }
    else
        m_tunnelActivity[tunnelID] = static_cast<quint64>(QDateTime::currentMSecsSinceEpoch());

    if (kind == QStringLiteral("poll"))
        return true;

    // ---- Web-initiated port tunnels: AUDIT-ONLY for now ---------------------
    // The intended policy: owner/sharedWith => any port; others => only the
    // AI-service ports of announced AI nodes (chat :4000; inference :11434/
    // :8188 on the PUBLIC AI node; terminal - owner/sharedWith only); FS JSON-
    // RPC tunnels (port 0) and web-device tunnels are authorized in-band by
    // the node (PairAuth/ConsentAuth/SetScope).
    //
    // NOT enforced at the moment: on the HTTP signaling path the initiator
    // identity is unreliable - real users may appear as session ids (32-hex)
    // or with an empty m_userName (see demo 2026-10-12 18:20: enforcing broke
    // legitimate FS tunnels even for the owner). Until identity resolution is
    // reworked, offending tunnel opens are logged for audit but allowed.
    if (isNewTunnel)
    {
        OnMyDiskLink* targetLink = m_links.value(actualTargetID, nullptr);
        if (targetLink && port > 0)
        {
            const QString initiator = sessionUser.isEmpty() ? senderId : sessionUser;
            const bool isOwner = (!initiator.isEmpty() && initiator == targetLink->user());
            const bool isShared = (!initiator.isEmpty() && targetLink->sharedWith().contains(initiator));
            if (!isOwner && !isShared)
            {
                const bool isTerminalTunnel = (service == QStringLiteral("terminal") || port == 22);
                const bool isChatPort = (port == CHAT_TUNNEL_PORT);
                const bool isInferencePort = (port == OLLAMA_TUNNEL_PORT || port == COMFY_TUNNEL_PORT);
                bool isAiNodeTarget = false;
                bool isPublicAiTarget = false;
                if (isChatPort || isInferencePort)
                {
                    Q_FOREACH(const SiteInfo &siteInfo, m_aiNodes) {
                        if (siteInfo.linkID == targetLink->linkID()) {
                            isAiNodeTarget = true;
                            if (siteInfo.users.contains(QStringLiteral("*")))
                                isPublicAiTarget = true;
                            break;
                        }
                    }
                    if (targetLink->linkID() == m_publicAiNodeID) {
                        isAiNodeTarget = true;
                        isPublicAiTarget = true;
                    }
                }
                const bool allowed = (!isTerminalTunnel && isAiNodeTarget) &&
                    (isChatPort || (isInferencePort && isPublicAiTarget));
                if (!allowed)
                    qWarning() << "[Gateway] Web tunnel (audit, allowed):" << initiator << "->" << actualTargetID
                               << "port" << port << "service" << service;
            }
        }
    }

    locker.unlock();

    // Post to main thread (where the command socket lives) via async slot
    QMetaObject::invokeMethod(this, "doSendToNodeSocket", Qt::QueuedConnection,
                              Q_ARG(QString, actualTargetID),
                              Q_ARG(QString, tunnelID),
                              Q_ARG(bool, isNewTunnel),
                              Q_ARG(QString, kind),
                              Q_ARG(QByteArray, payload),
                              Q_ARG(quint16, port),
                              Q_ARG(QString, service),
                              Q_ARG(QString, sessionUser));
    return true;
}

void OnMyDiskLinkServer::doSendToNodeSocket(const QString &linkID, const QString &tunnelID, bool sendOpen, const QString &kind, const QByteArray &payload, quint16 port, const QString &service, const QString &sessionUser)
{
    // Runs in main thread — safe to write to command socket
    QMutexLocker locker(&m_mutex);
    OnMyDiskLink *link = m_links.value(linkID, nullptr);
    if (!link || !link->linkSocket())
        return;
    LinkSocket *socket = link->linkSocket();
    locker.unlock();

    if (sendOpen)
    {
        LinkMessage openMsg;
        openMsg.setMessageType(LinkMessage::TunnelOpen);
        openMsg.setResultCode(LinkMessage::Request);
        openMsg.insertDataValue("ID", tunnelID);
        openMsg.insertDataValue("linkID", linkID);
        openMsg.insertDataValue("port", port);
        openMsg.insertDataValue("service", service);
        // Relay the authenticate-session identity of the HTTP initiator: the
        // node re-writes tunnel requests to its local AI service with these
        // trusted headers. sessionUser comes from checkAuth (m_userName),
        // never from the client-supplied "senderId" field.
        if (!sessionUser.isEmpty())
        {
            const bool callerIsOwner = (sessionUser == link->user());
            const bool callerIsShared = link->sharedWith().contains(sessionUser);
            openMsg.insertDataValue("X-OMD-Caller-User", sessionUser);
            openMsg.insertDataValue("X-OMD-Caller-Role",
                callerIsOwner ? QStringLiteral("owner")
                : callerIsShared ? QStringLiteral("shared")
                : QStringLiteral("guest"));
            openMsg.insertDataValue("X-OMD-Target-Owner", link->user());
        }
        socket->writeMessage(openMsg);
    }

    LinkMessage sig;
    sig.setMessageType(LinkMessage::TunnelSignaling);
    sig.setResultCode(LinkMessage::Request);
    sig.insertDataValue("ID", tunnelID);
    sig.insertDataValue("kind", kind);
    sig.setPayLoad(payload);
    socket->writeMessage(sig);
}

QList<WebSignal> OnMyDiskLinkServer::getWebSignals(const QString &tunnelID, const QString &requesterId, int timeoutMs)
{
    Q_UNUSED(requesterId)
    if (tunnelID.isEmpty())
        return QList<WebSignal>();

    QMutexLocker locker(&m_webMutex);
    if (!m_webSignals.contains(tunnelID) || m_webSignals[tunnelID].isEmpty())
    {
        if (timeoutMs > 0)
            m_webWaitCondition.wait(&m_webMutex, timeoutMs);
    }

    if (m_webSignals.contains(tunnelID))
    {
        return m_webSignals.take(tunnelID);
    }
    return QList<WebSignal>();
}

// --- Web device registration ---

// Try to deliver a signaling message to a web device via its live WebSocket
// channel. Returns false when no handler is installed or the device has no
// channel - caller must fall back to the polling queue (m_webSignals).
bool OnMyDiskLinkServer::pushWebSignal(const QString &targetID, const QString &tunnelID, const QString &kind, const QByteArray &payload)
{
    WebPushHandler handler;
    {
        QMutexLocker locker(&m_webMutex);
        handler = m_webPushHandler;
    }
    if (!handler)
        return false;

    // Called outside m_webMutex; the handler is responsible for its own
    // thread safety (it only does a lookup + queued invocation).
    bool delivered = false;
    try {
        delivered = handler(targetID, tunnelID, kind, payload);
    } catch (...) {
        delivered = false;
    }
    if (delivered)
        qDebug() << "[Gateway] Pushed" << kind << "to web device:" << targetID << "tunnel:" << tunnelID;
    return delivered;
}

bool OnMyDiskLinkServer::registerWebDevice(const QString &linkId, const QString &ownerId)
{
    if (linkId.isEmpty())
        return false;

    const QString owner = ownerId.isEmpty() ? linkId : ownerId;
    {
        QMutexLocker locker(&m_mutex);
        m_webDevices.insert(linkId);
        m_webDeviceOwner[linkId] = owner;
        m_webDeviceSeen[linkId] = static_cast<quint64>(QDateTime::currentMSecsSinceEpoch());
        qInfo() << "[Gateway] Web device registered:" << linkId << "owner:" << owner;
    }

    emit linkConnected(linkId, owner);
    return true;
}

void OnMyDiskLinkServer::unregisterWebDevice(const QString &linkId)
{
    QString owner;
    {
        QMutexLocker locker(&m_mutex);
        owner = m_webDeviceOwner.value(linkId, linkId);
        m_webDevices.remove(linkId);
        m_webDeviceOwner.remove(linkId);
        m_webDeviceSeen.remove(linkId);
        qInfo() << "[Gateway] Web device unregistered:" << linkId;
    }

    emit linkDisconnected(linkId, owner);
}

bool OnMyDiskLinkServer::isWebDevice(const QString &linkId) const
{
    return m_webDevices.contains(linkId);
}

bool OnMyDiskLinkServer::isWebDeviceOwner(const QString &linkId, const QString &ownerId)
{
    Q_UNUSED(ownerId)
    QMutexLocker locker(&m_mutex);
    return !linkId.isEmpty() && (m_webDevices.contains(linkId) || m_webDeviceOwner.contains(linkId));
}

QString OnMyDiskLinkServer::nodeOwnerId(const QString &linkId)
{
    if (linkId.isEmpty() || linkId == QStringLiteral("web"))
        return QString();
    if (m_links.contains(linkId))
    {
        OnMyDiskLink *link = m_links[linkId];
        return link ? link->user() : QString();
    }
    return m_webDeviceOwner.value(linkId);
}

bool OnMyDiskLinkServer::isTunnelParticipant(const QString &tunnelID, const QString &senderId)
{
    if (senderId.isEmpty())
        return false;
    if (m_tunnelCreator.value(tunnelID) == senderId)
        return true;
    if (!m_tunnels.contains(tunnelID))
        return false;
    QPair<QString, QString> ends = m_tunnels[tunnelID];
    return nodeOwnerId(ends.first) == senderId || nodeOwnerId(ends.second) == senderId;
}

void OnMyDiskLinkServer::sweepWebState()
{
    const quint64 now = static_cast<quint64>(QDateTime::currentMSecsSinceEpoch());

    {
        QMutexLocker locker(&m_mutex);
        // Expire stale web device bindings of devices that are not live.
        for (auto it = m_webDeviceSeen.begin(); it != m_webDeviceSeen.end(); )
        {
            if (!m_webDevices.contains(it.key()) && now >= it.value() + WEB_DEVICE_IDLE_TTL_MS)
            {
                qInfo() << "[Gateway] Web device binding expired:" << it.key();
                m_webDeviceOwner.remove(it.key());
                it = m_webDeviceSeen.erase(it);
            }
            else
                ++it;
        }
        // Drop creators of tunnels that are gone.
        for (auto it = m_tunnelCreator.begin(); it != m_tunnelCreator.end(); )
        {
            if (!m_tunnels.contains(it.key()))
                it = m_tunnelCreator.erase(it);
            else
                ++it;
        }
        // Drop tunnels with no relayed activity (dead P2P sessions whose ends
        // never sent TunnelClose) so they cannot block reconnects.
        for (auto it = m_tunnelActivity.begin(); it != m_tunnelActivity.end(); )
        {
            if (!m_tunnels.contains(it.key()) || now >= it.value() + TUNNEL_IDLE_TTL_MS)
            {
                if (m_tunnels.contains(it.key()))
                    qInfo() << "[Gateway] Sweeping idle tunnel:" << it.key();
                m_tunnels.remove(it.key());
                m_tunnelCreator.remove(it.key());
                it = m_tunnelActivity.erase(it);
            }
            else
                ++it;
        }
    }

    // Drop signaling messages that were never picked up.
    QMutexLocker webLocker(&m_webMutex);
    for (auto it = m_webSignals.begin(); it != m_webSignals.end(); )
    {
        QList<WebSignal> &pending = it.value();
        for (int i = pending.size() - 1; i >= 0; --i)
        {
            if (pending[i].timestamp > 0 && now >= pending[i].timestamp + WEB_SIGNAL_TTL_MS)
                pending.removeAt(i);
        }
        if (pending.isEmpty())
            it = m_webSignals.erase(it);
        else
            ++it;
    }
}

QList<WebSignal> OnMyDiskLinkServer::getPendingTunnelOpen(const QString &linkId, const QString &requesterId, int timeoutMs)
{
    Q_UNUSED(requesterId)
    if (linkId.isEmpty())
        return QList<WebSignal>();

    {
        QMutexLocker locker(&m_mutex);
        m_webDeviceSeen[linkId] = static_cast<quint64>(QDateTime::currentMSecsSinceEpoch());
    }

    QMutexLocker locker(&m_webMutex);
    if (timeoutMs > 0) {
        // Check if there's already something pending
        bool found = false;
        for (auto it = m_webSignals.constBegin(); it != m_webSignals.constEnd(); ++it) {
            QString tunnelID = it.key();
            bool isForThisDevice = (m_tunnels.value(tunnelID).second == linkId);
            if (isForThisDevice && !it.value().isEmpty()) {
                found = true;
                break;
            }
            for (const WebSignal &sig : it.value()) {
                if (sig.kind == "tunnel-open") {
                    QJsonObject payload = QJsonDocument::fromJson(sig.payload).object();
                    if (payload.value("ID").toString() == linkId || payload.value("linkID").toString() == linkId) {
                        found = true;
                        break;
                    }
                }
            }
            if (found) break;
        }
        if (!found)
            m_webWaitCondition.wait(&m_webMutex, timeoutMs);
    }

    // Drain all signals targeted at this linkId (tunnel-open, offer, candidate, etc.)
    QList<WebSignal> result;
    for (auto it = m_webSignals.begin(); it != m_webSignals.end(); ) {
        QString tunnelID = it.key();
        bool isForThisDevice = (m_tunnels.value(tunnelID).second == linkId);
        auto &pending = it.value();
        for (int i = pending.size() - 1; i >= 0; --i) {
            bool match = isForThisDevice;
            if (!match && pending[i].kind == "tunnel-open") {
                QJsonObject payload = QJsonDocument::fromJson(pending[i].payload).object();
                if (payload.value("ID").toString() == linkId || payload.value("linkID").toString() == linkId) {
                    match = true;
                }
            }
            if (match) {
                result.append(pending.takeAt(i));
            }
        }
        if (pending.isEmpty()) it = m_webSignals.erase(it);
        else ++it;
    }
    return result;
}





bool OnMyDiskLinkServer::hasActiveLink(const QString &user)
{
    QMutexLocker locker(&m_mutex);
    Q_FOREACH(OnMyDiskLink *link, m_links) {
        if (link->user() == user && link->state() == OnMyDiskLink::Connected)
            return true;
    }
    return false;
}

QJsonArray OnMyDiskLinkServer::getConnectedDevices(const QString &user, const QJsonArray &filterLinkIds)
{
    QMutexLocker locker(&m_mutex);
    QJsonArray list;
    QSet<QString> added;

    QSet<QString> targetFilter;
    for (const QJsonValue &v : filterLinkIds) {
        const QString id = v.toString().trimmed();
        if (!id.isEmpty()) targetFilter.insert(id);
    }

    qDebug() << "[Gateway] getConnectedDevices called for user:" << user
            << "filterLinkIds count:" << targetFilter.size()
            << "filterLinkIds:" << targetFilter
            << "active m_links count:" << m_links.count()
            << "active m_webDevices count:" << m_webDevices.count();

    for (auto it = m_links.begin(); it != m_links.end(); ++it) {
        OnMyDiskLink *link = it.value();
        const QString peerId = it.key();
        const QString persistentId = link ? (link->linkID().isEmpty() ? m_persistentIDs.value(peerId, peerId) : link->linkID()) : QString();
        const QString linkUser = link ? link->user() : QString();

        if (!link || link->state() == OnMyDiskLink::Disconnected) {
            continue;
        }

        bool matchFilter = targetFilter.isEmpty() ||
                           targetFilter.contains(peerId) ||
                           targetFilter.contains(persistentId) ||
                           (!linkUser.isEmpty() && targetFilter.contains(linkUser));

        if (!matchFilter)
            continue;

        QString reportId = persistentId;
        if (!linkUser.isEmpty() && (targetFilter.contains(linkUser) || reportId.isEmpty() || reportId == peerId)) {
            reportId = linkUser;
        }
        if (reportId.isEmpty()) reportId = peerId;

        if (!added.contains(reportId)) {
            added.insert(reportId);
            QJsonObject dev;
            dev["linkID"] = reportId;
            dev["peerID"] = peerId;
            dev["user"] = linkUser;
            dev["type"] = "node";
            dev["status"] = "online";
            list.append(dev);
            qDebug() << "[Gateway]   -> MATCHED & ADDED storage node reportId:" << reportId;
        }
    }

    for (const QString &webId : m_webDevices) {
        if (!targetFilter.isEmpty() && !targetFilter.contains(webId))
            continue;

        const QString owner = m_webDeviceOwner.value(webId, webId);
        if (!added.contains(webId)) {
            added.insert(webId);
            QJsonObject dev;
            dev["linkID"] = webId;
            dev["user"] = owner;
            dev["type"] = "web";
            dev["status"] = "online";
            list.append(dev);
            qDebug() << "[Gateway]   -> MATCHED & ADDED web device:" << webId;
        }
    }

    qDebug() << "[Gateway] getConnectedDevices returning total" << list.count() << "devices:" << list;
    return list;
}

bool OnMyDiskLinkServer::hasCodePermission(const QString &user)
{
    QMutexLocker locker(&m_mutex);
    if (user.isEmpty())
        return false;

    if (m_aiNodes.contains(user))
        return true;

    Q_FOREACH(const SiteInfo& siteInfo, m_aiNodes) {
        if (siteInfo.users.contains(user)) {
            return true;
        }
    }
    return false;
}

QList<QJsonObject> OnMyDiskLinkServer::getNearbyNodes(const QString &clientIp)
{
    QMutexLocker locker(&m_mutex);
    QList<QJsonObject> result;
    QHostAddress reqAddr(clientIp);

    for (auto it = m_links.begin(); it != m_links.end(); ++it)
    {
        OnMyDiskLink *link = it.value();
        if (!link || !link->linkSocket())
            continue;

        QHostAddress peerAddr = link->linkSocket()->peerAddress();
        bool match = false;
        if (!reqAddr.isNull() && !peerAddr.isNull()) {
            if (reqAddr.isLoopback() && peerAddr.isLoopback()) {
                match = true;
            } else if (reqAddr.isEqual(peerAddr, QHostAddress::TolerantConversion)) {
                match = true;
            } else if (!clientIp.isEmpty() && peerAddr.toString() == clientIp) {
                match = true;
            }
        } else if (!clientIp.isEmpty() && peerAddr.toString() == clientIp) {
            match = true;
        }

        if (match) {
            QString shortId = !link->user().isEmpty() ? link->user() : link->linkID();
            QJsonObject obj;
            obj["linkId"] = shortId;
            obj["peerId"] = it.key();
            obj["name"] = link->shareName().isEmpty() ? shortId : link->shareName();
            obj["version"] = link->linkVersion();
            obj["online"] = (link->state() != OnMyDiskLink::Disconnected);
            result.append(obj);
        }
    }
    return result;
}

