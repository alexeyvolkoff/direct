#include "servicenode.h"
#include "localdb.h"



ServiceNode::ServiceNode(QString configFile, QObject *parent): OnMyDiskNode(configFile, parent),
    m_linkClient(&m_systemSettings, m_rootResource),
    m_client(&m_userSettings, m_rootResource)
{
    //IPC events
    connect(this, &OnMyDiskNode::addShare, &m_client, &OnMyDiskClient::addShare);
    connect(this, &OnMyDiskNode::removeShare, &m_client, &OnMyDiskClient::removeShare);
    connect(this, &OnMyDiskNode::connectCloud, &m_linkClient, &OnMyDiskLinkClient::connectToCloud, Qt::QueuedConnection);
    connect(this, &OnMyDiskNode::disconnectCloud, &m_client, &OnMyDiskClient::signOut, Qt::QueuedConnection);
    connect(this, &OnMyDiskNode::authRequest, &m_linkClient, &OnMyDiskLinkClient::authRequested);
    connect(this, &OnMyDiskNode::resetLink, &m_linkClient, &OnMyDiskLinkClient::clearDeviceId);
    connect(this, &OnMyDiskNode::quit, &m_client, &OnMyDiskClient::quit);

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
    connect(&m_client, &OnMyDiskClient::connectCloud, &m_linkClient, &OnMyDiskLinkClient::connectToCloud, Qt::QueuedConnection);
    connect(&m_client, &OnMyDiskClient::resetLink, &m_linkClient, &OnMyDiskLinkClient::resetDeviceId, Qt::DirectConnection);
    connect(&m_client, &OnMyDiskClient::authRequired, &m_linkClient, &OnMyDiskLinkClient::authRequested);
    connect(&m_client, &OnMyDiskClient::disconnectCloud, &m_linkClient, &OnMyDiskLinkClient::disconnectFromCloud, Qt::QueuedConnection);
    connect(&m_client, &OnMyDiskClient::shareAdded, &m_linkClient, &OnMyDiskLinkClient::addLocalShare, Qt::QueuedConnection);
    connect(&m_client, &OnMyDiskClient::shareRemoved, &m_linkClient, &OnMyDiskLinkClient::removeLocalShare, Qt::QueuedConnection);
    connect(&m_client, &OnMyDiskClient::settingsLoaded, &m_linkClient, &OnMyDiskLinkClient::updateSettings);
    connect(&m_linkClient, &OnMyDiskLinkClient::settingsChanged, &m_client, &OnMyDiskClient::applySettings, Qt::QueuedConnection);

    //link backend
    connect(&LocalDB::instance(), &LocalDB::error, &m_client, &OnMyDiskClient::linkError);
    connect(&LocalDB::instance(), &LocalDB::publicLinkAdded, &m_linkClient,  &OnMyDiskLinkClient::publicLinkAdded);

    //load/create device id
    m_linkClient.readSettings();
    //read settings and start
    m_client.readSettings();
}





void ServiceNode::close()
{

}
