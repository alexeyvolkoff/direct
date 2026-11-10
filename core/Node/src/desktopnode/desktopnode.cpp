#include "desktopnode.h"
#include "localdb.h"
#include <QDesktopServices>
#include "userevents.h"

OnMyDiskDesktopNode::OnMyDiskDesktopNode(QString configFile, QObject *parent) : OnMyDiskNode(configFile, parent),
 m_linkClient(&m_systemSettings, m_rootResource),
#ifdef DESKTOP
 m_client(&m_userSettings, m_rootResource)
#else
 m_client(&m_systemSettings, m_rootResource)
#endif
{
    //remove window title

    //IPC events
    connect(this, &OnMyDiskNode::addShare, &m_client, &OnMyDiskClient::addShare);
    connect(this, &OnMyDiskNode::removeShare, &m_client, &OnMyDiskClient::removeShare);
    connect(this, &OnMyDiskNode::connectCloud, &m_linkClient, &OnMyDiskLinkClient::connectToCloud, Qt::QueuedConnection);
    connect(this, &OnMyDiskNode::disconnectCloud, &m_client, &OnMyDiskClient::signOut, Qt::QueuedConnection);
    connect(this, &OnMyDiskNode::showCloud, &m_clientBackend, &DesktopBackend::showCloud);
    connect(this, &OnMyDiskNode::showStorage, &m_clientBackend, &DesktopBackend::showStorage);
    connect(this, &OnMyDiskNode::linkStorage, &m_clientBackend, &DesktopBackend::linkStorage);
    connect(this, &OnMyDiskNode::showShare, &m_clientBackend, &DesktopBackend::showShare);
    connect(this, &OnMyDiskNode::parseUrl, &m_clientBackend, &DesktopBackend::parseUrl, Qt::QueuedConnection);
    connect(this, &OnMyDiskNode::resetLink, &m_linkClient, &OnMyDiskLinkClient::clearDeviceId);
    connect(this, &OnMyDiskNode::quit, &m_client, &OnMyDiskClient::quit);


    // Backend events
    connect(&m_clientBackend, &DesktopBackend::addShare, &m_client, &OnMyDiskClient::addShare, Qt::QueuedConnection);
    connect(&m_clientBackend, &DesktopBackend::removeShare, &m_client, &OnMyDiskClient::removeShare);
    connect(&m_clientBackend, &DesktopBackend::settingsChanged, &m_client, &OnMyDiskClient::applySettings);
    connect(&m_clientBackend, &DesktopBackend::signOut, &m_client, &OnMyDiskClient::signOut);
    connect(&m_clientBackend, &DesktopBackend::quit, &m_client, &OnMyDiskClient::quit);
    connect(&m_client, &OnMyDiskClient::shareAdded, &m_clientBackend, &DesktopBackend::shareAdded);
    connect(&m_client, &OnMyDiskClient::shareRemoved, &m_clientBackend, &DesktopBackend::shareRemoved);
    connect(&m_client, &OnMyDiskClient::stateChanged, &m_clientBackend, &DesktopBackend::stateChanged);
    connect(&m_client, &OnMyDiskClient::storageIdUpdated, &m_clientBackend, &DesktopBackend::storageIdUpdated);
    connect(&m_client, &OnMyDiskClient::showCloud, &m_clientBackend, &DesktopBackend::showCloud);
    connect(&m_client, &OnMyDiskClient::showStorage, &m_clientBackend, &DesktopBackend::showStorage);
    connect(&m_client, &OnMyDiskClient::settingsLoaded, &m_linkClient, &OnMyDiskLinkClient::updateSettings);
    connect(&m_linkClient, &OnMyDiskLinkClient::settingsChanged, &m_client, &OnMyDiskClient::applySettings, Qt::QueuedConnection);
    connect(&m_client, &OnMyDiskClient::settingsLoaded, &m_clientBackend, &DesktopBackend::settingsLoaded);
    connect(&m_client, &OnMyDiskClient::settingsSaved, &m_clientBackend, &DesktopBackend::writeSettings);

    //State change and cert load event
    connect(&m_client, &OnMyDiskClient::stateChanged, this, &OnMyDiskNode::stateChanged);
    connect(&m_linkClient, &OnMyDiskLinkClient::sslOK, this, &OnMyDiskNode::certLoaded);


    //Link client events
    connect(&m_linkClient, &OnMyDiskLinkClient::linkConnected, &m_client, &OnMyDiskClient::linkConnected);
    connect(&m_linkClient, &OnMyDiskLinkClient::linkDisconnected, &m_client, &OnMyDiskClient::linkDisconnected);
    connect(&m_linkClient, &OnMyDiskLinkClient::connectionError, &m_client, &OnMyDiskClient::connectionError);
    connect(&m_linkClient, &OnMyDiskLinkClient::sslError, &m_client, &OnMyDiskClient::sslError);
    connect(&m_linkClient, &OnMyDiskLinkClient::privateKeyError, &m_client, &OnMyDiskClient::pkiError);
    connect(&m_linkClient, &OnMyDiskLinkClient::privateKeyStatus, &m_client, &OnMyDiskClient::pkiStatusChange);
    connect(&m_linkClient, &OnMyDiskLinkClient::sslOK, &m_client, &OnMyDiskClient::certLoaded);
    connect(&m_linkClient, &OnMyDiskLinkClient::accountCreated, &m_client, &OnMyDiskClient::accountCreated);
    connect(&m_linkClient, &OnMyDiskLinkClient::deviceIdUpdated, &m_client, &OnMyDiskClient::deviceIdUpdated);

    connect(&m_client, &OnMyDiskClient::connectCloud, &m_linkClient, &OnMyDiskLinkClient::connectToCloud, Qt::QueuedConnection);
    connect(&m_client, &OnMyDiskClient::resetLink, &m_linkClient, &OnMyDiskLinkClient::resetDeviceId, Qt::DirectConnection);
    connect(&m_client, &OnMyDiskClient::authRequired, &m_linkClient, &OnMyDiskLinkClient::authRequested);
    connect(&m_client, &OnMyDiskClient::disconnectCloud, &m_linkClient, &OnMyDiskLinkClient::disconnectFromCloud, Qt::QueuedConnection);
    connect(&m_client, &OnMyDiskClient::shareAdded, &m_linkClient, &OnMyDiskLinkClient::addLocalShare, Qt::QueuedConnection);
    connect(&m_client, &OnMyDiskClient::shareRemoved, &m_linkClient, &OnMyDiskLinkClient::removeLocalShare, Qt::QueuedConnection);


    //new signin and users evenrs API
    connect(&UserEvents::instance(), &UserEvents::signedIn, &m_linkClient, &OnMyDiskLinkClient::signedIn);
    connect(&UserEvents::instance(), &UserEvents::addShareRequested, &m_client, &OnMyDiskClient::shareAddRequested);
    connect(&UserEvents::instance(), &UserEvents::removeShareRequested, &m_client, &OnMyDiskClient::shareRemoveRequested);

    //link backend
    connect(&LocalDB::instance(), &LocalDB::error, &m_client, &OnMyDiskClient::linkError);
    connect(&LocalDB::instance(), &LocalDB::publicLinkAdded, &m_linkClient,  &OnMyDiskLinkClient::publicLinkAdded);

    //load/create device id
    m_linkClient.readSettings();
    //read settings and start
    m_client.readSettings();
    m_clientBackend.loaded();
}

void OnMyDiskDesktopNode::close()
{

}



