#include "remoteresource.h"
#include "webdavbackend.h"
#include <QTimer>

#ifdef SERVER
#include "notificationmanager.h"
#include "clientnotifier.h"
#include "onmydisklinkserver.h"
#endif

RemoteResource::RemoteResource(QString linkID, QString remotePath, QString query, bool sharedLink, RemoteResource *parent):
    CloudResource(), m_remotePath(remotePath), m_MemBackend(), m_linkBackend(linkID, remotePath, query)
{
    m_LinkID = linkID;
    m_resourceID = QUuid::createUuid().toString();
    m_copyesCount = 0;
    m_originalResource_ptr = nullptr;
    m_parentResource_ptr = parent;
    m_isCopy = false;
    m_isActual = false;
    m_sharedLink = sharedLink;
    m_deleting = false;

    m_lastModified = QDateTime::currentDateTime();
    m_lastRead = QDateTime::fromMSecsSinceEpoch(0);
    m_created = QDateTime::currentDateTime();

    m_exists = false;
    m_isReadable = false;
    m_isShareRoot =  false;
    m_isWritable =  m_parentResource_ptr ?  m_parentResource_ptr->m_isWritable:  false;
    m_overrideWritable = m_parentResource_ptr ?  m_parentResource_ptr->m_overrideWritable :  false;
    m_sharedLinkId = m_parentResource_ptr ? m_parentResource_ptr->m_sharedLinkId : "";
    m_owner = m_parentResource_ptr ? m_parentResource_ptr->m_owner : "";

    m_isDir = false;
    m_isFile = false;
    m_isExecutable = false;
    m_lastUpdated = 0;
	m_size = 0;
	m_blockSize = 0;
	m_parentExists = (m_parentResource_ptr != nullptr);
#ifdef SERVER
    QPointer<OnMyDiskLink> cLink = OnMyDiskLinkServer::instance().getLink(linkID);
    if (cLink) {
        m_currentBackendType = RemoteLinkBackend;
        m_currentBackend = &m_linkBackend;
    } else {
        QSharedPointer<ClientNotifier> notifier = NotificationManager::instance().getLinkNotifier(linkID);
        if (notifier) {
            m_currentBackendType = WSBackend;
            m_wsBackend = new WebSocketBackend(notifier, remotePath, query);
            m_currentBackend = m_wsBackend;
        } else {
            m_currentBackendType = RemoteLinkBackend;
            m_currentBackend = &m_linkBackend;
        }
    }
#else
    m_currentBackendType = RemoteLinkBackend;
    m_currentBackend = &m_linkBackend;
#endif
    m_query = query;

    m_remotePath = remotePath;
    //m_displayPath = remotePath;
    m_fileName = (remotePath.isEmpty() || (remotePath == "/")) ? "" : remotePath.split("/", Qt::SplitBehaviorFlags::SkipEmptyParts).last();
    if (m_parentResource_ptr) {
      if ( m_parentResource_ptr->m_displayPath.endsWith("/"))
        m_displayPath = m_parentResource_ptr->m_displayPath + m_fileName;
      else
        m_displayPath = m_parentResource_ptr->m_displayPath + "/" + m_fileName;

      // Inherit path context from parent
      QString parentRemPath = m_parentResource_ptr->m_remotePath;
      if (!parentRemPath.endsWith("/"))
          parentRemPath.append("/");
      m_remotePath = parentRemPath + m_fileName;
      m_customAttributes.insert("itemPath", m_remotePath);

      if (!m_parentResource_ptr->m_realPath.isEmpty()) {
           QString parentRealPath = m_parentResource_ptr->m_realPath;
           if (!parentRealPath.endsWith("/")) parentRealPath.append("/");
           m_realPath = parentRealPath + m_fileName;
           m_currentBackend->setRealPath(m_realPath);
      } else {
           m_currentBackend->setRealPath(m_remotePath);
      }

    } else
        m_displayPath = "/" + m_fileName;

    if (!m_sharedLinkId.isEmpty()) {
        m_customAttributes.insert("sharedLinkId", m_sharedLinkId);
    }
    if (!m_owner.isEmpty()) {
        m_customAttributes.insert("owner", m_owner);
    }


}

RemoteResource::RemoteResource(RemoteResource *resource, QString query):
    CloudResource(),  m_MemBackend(resource->getMemBackendData(), resource->getMemBackendContentType()),
    m_linkBackend(resource->GetLinkID(),
                  (resource->getCustomAttributes().contains("realPath") ? resource->getCustomAttributes()["realPath"].toString() : resource->remotePath()),
                  query, resource->size())
{
    m_LinkID = resource->GetLinkID();
    m_resourceID = resource->GetID() + QString("_copy_%1").arg(query);
    m_copyesCount = 0;
    m_deleting = false;
    m_originalResource_ptr = resource;
    m_parentResource_ptr = nullptr;
    m_query = query;
    m_isCopy = true;

    m_exists = resource->exists();
    m_isReadable = resource->isReadable();
    m_isWritable = resource->isWritable();
    m_overrideWritable = resource->m_overrideWritable;

    m_isDir = resource->isDir();
    m_isFile = resource->isFile();
    m_isExecutable = resource->m_isExecutable;
    m_lastUpdated = resource->m_lastUpdated;
    m_size = resource->m_size;
    m_blockSize = resource->m_blockSize;

    m_sharedLink = resource->m_sharedLink;
    m_contentType = resource->m_contentType;
    m_sharedLinkId = resource->m_sharedLinkId;
    m_overrideWritable = resource->m_overrideWritable;
    m_isShareRoot = resource->m_isShareRoot;
    m_isDirectShare = resource->m_isDirectShare;
    m_owner = resource->m_owner;
    m_lastModified = resource->m_lastModified;
    m_displayPath = resource->m_displayPath;

    m_remotePath = resource->remotePath();
    //qInfo() << "Created new copy Remote resource: " << m_resourceID << " from path " << m_remotePath;
    m_fileName = resource->fileName();

    m_lastUpdated = 0;
    m_parentExists = false;

    //deep copy of attributes
    QVariantHash attrs = resource->getCustomAttributes();
    foreach(const QString &key, attrs.keys())
        m_customAttributes[key] = attrs[key];
        
    m_realPath = resource->m_realPath;
    
    m_currentBackendType = resource->m_currentBackendType;
#ifdef SERVER
    QPointer<OnMyDiskLink> cLink = OnMyDiskLinkServer::instance().getLink(m_LinkID);
    if (cLink) {
        m_currentBackendType = RemoteLinkBackend;
        m_currentBackend = &m_linkBackend;
    } else if (m_currentBackendType == WSBackend ) {
        QSharedPointer<ClientNotifier> notifier = NotificationManager::instance().getLinkNotifier(m_LinkID);
        if (notifier) {
            QString backendPath = !m_realPath.isEmpty() ? m_realPath : m_remotePath;
            m_wsBackend = new WebSocketBackend(notifier, backendPath, m_query);
            m_currentBackend = m_wsBackend;
        } else {
            m_currentBackendType = RemoteLinkBackend;
            m_currentBackend = &m_linkBackend;
        }
    } else {
        m_currentBackendType = RemoteLinkBackend;
        m_currentBackend = &m_linkBackend;  
    }
    
    // Ensure LinkBackend uses the correct real path if available
    if (m_currentBackendType == RemoteLinkBackend && !m_realPath.isEmpty()) {
         m_currentBackend->setRealPath(m_realPath);
    }

    if (!m_sharedLinkId.isEmpty()) {
        m_customAttributes["sharedLinkId"] = m_sharedLinkId;
        m_customAttributes["itemPath"] = m_remotePath;
        // Only reset real path if we DON'T have a valid one already
         if (m_realPath.isEmpty())
            m_currentBackend->setRealPath(m_remotePath);
    }
#else
    if (m_currentBackendType == CloudResource::WebDAVBackendType) {
        if (resource->m_webdavBackend) {
            QHash<QString, QVariant> webdavData;
            webdavData["baseUrl"] = resource->m_webdavBackend->baseUrl();
            webdavData["prefix"] = resource->m_webdavBackend->prefix();
            initWebDAVBackend(webdavData);
        } else {
            m_currentBackendType = RemoteLinkBackend;
            m_currentBackend = &m_linkBackend;
        }
    } else {
        m_currentBackendType = RemoteLinkBackend;
        m_currentBackend = &m_linkBackend;
    }
#endif

    m_isActual = false;
}


bool RemoteResource::isActual()
{
    return m_isActual;
}

CloudResource::BackendType RemoteResource::getCurrentBackendType() const
{
    return m_currentBackendType;
}

RemoteResource::~RemoteResource()
{
    OnMyDiskLocker lock(&m_mutex);
    if (! lock.isLocked())
        qWarning() << "Desctroing locked!!!" << m_remotePath;
    m_deleting = true;
    #ifdef SERVER
    if (m_wsBackend) {
        delete m_wsBackend;
        m_wsBackend = nullptr;
    }
    #endif
    if (m_webdavBackend) {
        delete m_webdavBackend;
        m_webdavBackend = nullptr;
    }
    removeAllChildren();
}

bool RemoteResource::initWebDAVBackend(const QHash<QString, QVariant> &webdavData)
{
    QString baseUrl = webdavData.value("baseUrl").toString();
    if (baseUrl.isEmpty())
        return false;

    if (m_webdavBackend) {
        delete m_webdavBackend;
        m_webdavBackend = nullptr;
    }

    m_webdavBackend = new WebDAVBackend(baseUrl, nullptr);

    // Use explicitly passed prefix, or fall back to own remote path (for root resource)
    if (webdavData.contains("prefix")) {
        m_webdavBackend->setPrefix(webdavData.value("prefix").toString());
    } else {
        m_webdavBackend->setPrefix(m_remotePath);
    }

    m_webdavBackend->setRemotePath(m_remotePath);

    m_currentBackendType = CloudResource::WebDAVBackendType;
    m_currentBackend = m_webdavBackend;

    m_realPath = m_remotePath;
    m_currentBackend->setRealPath(m_realPath);

    return true;
}

QHash<QString, QVariant> RemoteResource::getData() const
{
    QHash<QString, QVariant> resourceData;

    resourceData.insert("size", size());
    resourceData.insert("lastModDate", m_lastModified);
    resourceData.insert("createdDate", m_created);
    resourceData.insert("lastReadDate", m_lastRead);
    resourceData.insert("contentType", m_contentType);

    resourceData.insert("exists", m_exists);
    resourceData.insert("isReadable", m_isReadable);
    resourceData.insert("isWritable", m_isWritable);
    resourceData.insert("isShareRoot", m_isShareRoot);
    resourceData.insert("overrideWritable", m_overrideWritable);

    resourceData.insert("isDir", m_isDir);
    resourceData.insert("isFile", m_isFile);
    resourceData.insert("isExecutable", m_isExecutable);

    resourceData.insert("blockSize", m_blockSize);
    resourceData.insert("bytesAvailable", m_bytesAvailable);
    resourceData.insert("bytesFree", m_bytesFree);
    resourceData.insert("bytesTotal", m_bytesTotal);
    resourceData.insert("bytesUsed", m_bytesUsed);
    resourceData.insert("nlink", m_nLinkValue);

    if (!m_link.isEmpty())
        resourceData.insert("link", m_link);

    resourceData.insert("customAttributes", m_customAttributes);
    resourceData.insert("linkData", m_linkData);
    resourceData.insert("linkID", m_LinkID);
    resourceData.insert("remotePath", m_remotePath);
    if (!m_sharedLinkId.isEmpty())
        resourceData.insert("sharedLinkId", m_sharedLinkId);

    resourceData.insert("etag", m_ETag);

    return resourceData;
}

QHash<QString, QHash<QString, QVariant> > RemoteResource::getEntryData() const
{
    QHash<QString, QHash<QString, QVariant> > entryData;
    QStringList entryList = m_childResources.keys();
    Q_FOREACH(QString entryName, entryList)
    {
        RemoteResource* child = m_childResources[entryName];
        if (child)
        {
            entryData.insert(entryName, child->getData());
        }
    }

    return entryData;
}

QHash<QString, QVariant> RemoteResource::getCustomAttributes() const
{
	return m_customAttributes;
}

QByteArray RemoteResource::getMemBackendData() const
{
    return m_MemBackend.getBytes();
}

QString RemoteResource::getMemBackendContentType() const
{
    return m_MemBackend.getContentType();
}

bool RemoteResource::isCopy() const
{
    return m_isCopy;
}

bool RemoteResource::isReadable() const
{
    return m_isReadable;
}

bool RemoteResource::isWritable() const
{
    return m_isWritable;
}

bool RemoteResource::isExecutable() const
{
    return m_isExecutable;
}


void RemoteResource::LoadEntryData(QHash<QString, QHash<QString, QVariant>> entryData)
{
    OnMyDiskLocker lock(&m_mutex);
    if (! lock.isLocked())
        qWarning() << "Can not lock on RemoteResource::LoadEntryData" << m_remotePath;

    Q_FOREACH(QString childName, entryData.keys())
    {
        //keep parent's owner and link id for children of mounted shares
        if (! m_sharedLinkId.isEmpty()) {
            entryData[childName]["sharedLinkId"] = m_sharedLinkId;
            entryData[childName]["overrideWritable"] = true;
            entryData[childName]["owner"] = m_owner;
            entryData[childName]["directShare"] = m_isDirectShare;
            entryData[childName]["isWritable"] = m_isWritable;
        }

        if (m_childResources.contains(childName))
        {
            m_childResources[childName]->LoadData(entryData[childName]);
        }
        else
        {
            QString childRemotePath = entryData[childName].contains("remotePath")
                ? entryData[childName]["remotePath"].toString()
                : (m_remotePath.endsWith("/") ? m_remotePath + childName : m_remotePath + "/" + childName);
            QString chilId =  entryData[childName].contains("linkID") ? entryData[childName]["linkID"].toString()  :  m_LinkID;
            RemoteResource* childResource = new RemoteResource(chilId, childRemotePath, QStringLiteral(""), false, this);
            if (m_currentBackendType == CloudResource::WebDAVBackendType && m_webdavBackend) {
                QHash<QString, QVariant> webdavData;
                webdavData["baseUrl"] = m_webdavBackend->baseUrl();
                webdavData["prefix"] = m_webdavBackend->prefix();
                childResource->initWebDAVBackend(webdavData);
            }
            childResource->LoadData(entryData[childName]);
            m_childResources.insert(childName, childResource);
        }
    }

    Q_FOREACH(QString childName, m_childResources.keys())
    {
        if (!entryData.keys().contains(childName)) {
            doRemoveChild(childName, true);
		}
    }
}

void RemoteResource::updatePathByParent()
{
    if (!m_parentResource_ptr) return;

    OnMyDiskLocker lockParent(&(m_parentResource_ptr->m_mutex));
    if (!lockParent.isLocked()) {
        qWarning() << "Can not lock parent on RemoteResource::updatePathByParent" << m_remotePath;
        return;
    }

    QString itemPath =  m_parentResource_ptr->m_customAttributes.contains("itemPath") ? m_parentResource_ptr->m_customAttributes["itemPath"].toString() : m_parentResource_ptr->m_remotePath;
    if (!itemPath.endsWith("/"))
        itemPath.append("/");
    itemPath = itemPath + m_fileName;
    
    // For reference items, the real path comes from server in customAttributes["itemPath"]
    // If it exists, use it; otherwise fallback to calculated path
    QString realPath;
    if (!m_realPath.isEmpty())
        realPath = m_realPath;
    else
        realPath = m_customAttributes.contains("itemPath") ? m_customAttributes["itemPath"].toString() : itemPath;
    
    // Only overwrite itemPath attribute if it's NOT a reference, or if we don't have a real path yet.
    // References should keep their Real Path in itemPath attribute.
    if (!m_customAttributes.contains("isRef")) {
         m_customAttributes["itemPath"] = itemPath; // display/virtual path
    } else {
         // ensure realPath is saved
         m_realPath = realPath;
    }

    m_remotePath = itemPath;
    m_displayPath =  m_parentResource_ptr->m_displayPath.endsWith("/") ?  m_parentResource_ptr->m_displayPath  + m_fileName :  m_parentResource_ptr->m_displayPath + "/" + m_fileName ;
    if ( m_parentResource_ptr->m_customAttributes.contains("hasSites") && (m_fileName == "sites"))
        m_customAttributes["sitesRoot"] = true;

    if (m_customAttributes.contains("isRef")) {
        // m_customAttributes["realPath"] is already set above
        m_currentBackend->setRealPath(realPath);
    } else {
        if (m_parentResource_ptr && m_parentResource_ptr->m_customAttributes.contains("realPath")) {
             QString parentRealPath = m_parentResource_ptr->m_customAttributes["realPath"].toString();
             if (!parentRealPath.endsWith("/")) parentRealPath.append("/");
             QString childRealPath = parentRealPath + m_fileName;
             m_customAttributes["realPath"] = childRealPath;
             m_currentBackend->setRealPath(childRealPath);
        } else {
             m_currentBackend->setRealPath(m_remotePath);
        }
    }
}

void RemoteResource::LoadData(QHash<QString, QVariant> resourceData)
{
    OnMyDiskLocker lock(&m_mutex);
    if (! lock.isLocked())
        qWarning() << "Can not lock on RemoteResource::LoadData" << m_remotePath;
    //Mounted link id
    if (resourceData.contains("sharedLinkId")) {
        m_sharedLinkId = resourceData["sharedLinkId"].toString();
#ifdef SERVER
        if (m_currentBackendType == WSBackend && m_wsBackend) {
             m_wsBackend->setLinkID(m_sharedLinkId);
        }
#endif
    }

    if (resourceData.contains("lastModDate"))
        m_lastModified = resourceData.value("lastModDate").toDateTime();

    if (resourceData.contains("lastModified"))
        m_lastModified = QDateTime::fromString(resourceData["lastModified"].toString(), Qt::ISODate);

    if (resourceData.contains("lastReadDate"))
        m_lastRead = resourceData.value("lastReadDate").toDateTime();
    if (resourceData.contains("createdDate"))
        m_created = resourceData.value("createdDate").toDateTime();
    if (resourceData.contains("remotePath"))
        m_remotePath = resourceData.value("remotePath").toString();
    
    if (resourceData.contains("linkID")) {
        QString newLinkID = resourceData.value("linkID").toString();
#ifdef SERVER
        if (m_LinkID != newLinkID && m_currentBackendType == WSBackend) {
            
            if (m_wsBackend)
                delete m_wsBackend;
            
            // Use realPath or itemPath if available (for ref items), fallback to m_remotePath
            QString backendPath = m_remotePath;
            if (m_customAttributes.contains("realPath"))
                backendPath = m_customAttributes["realPath"].toString();
            else if (m_customAttributes.contains("itemPath"))
                backendPath = m_customAttributes["itemPath"].toString();
            else if (resourceData.contains("itemPath"))
                backendPath = resourceData["itemPath"].toString();
                
            QSharedPointer<ClientNotifier> notifier = NotificationManager::instance().getLinkNotifier(newLinkID);
            if (notifier) {
                m_wsBackend = new WebSocketBackend(notifier, backendPath, m_query);
                if (!m_sharedLinkId.isEmpty())
                     m_wsBackend->setLinkID(m_sharedLinkId);
                m_currentBackend = m_wsBackend;
            } else {
                 m_currentBackendType = RemoteLinkBackend;
                 m_currentBackend = &m_linkBackend;
            }
        }
#endif
        m_LinkID = newLinkID;
    }

#ifndef SERVER
    // Handle WebDAV backend re-initialization
    if (resourceData.contains("webdav")) {
        QHash<QString, QVariant> webdavData = resourceData["webdav"].toHash();
        if (!webdavData.isEmpty()) {
            initWebDAVBackend(webdavData);
        }
    }
#endif

    if (resourceData.contains("exists"))
        m_exists = resourceData.value("exists").toBool();

    if (resourceData.contains("isReadable"))
        m_isReadable = resourceData.value("isReadable").toBool();

    if (resourceData.contains("isShareRoot"))
        m_isShareRoot = resourceData.value("isShareRoot").toBool();

    //Set content type only if not set manually
    if (resourceData.contains("contentType") && m_contentType.isEmpty())
        m_contentType = resourceData.value("contentType").toString();

    if (resourceData.contains("overrideWritable") && resourceData.contains("isWritable")) {
      m_overrideWritable = resourceData.value("overrideWritable").toBool();
      m_isWritable = resourceData.value("isWritable").toBool();
    }

    if (resourceData.contains("directShare"))
        m_isDirectShare = resourceData.value("directShare").toBool();

    if (resourceData.contains("isWritable") && !m_overrideWritable) {
        m_isWritable = resourceData.value("isWritable").toBool();
        //set writable according to parent's  and original resource writable
        if (m_parentResource_ptr &&  m_parentResource_ptr->m_overrideWritable)
            m_isWritable = m_isWritable && m_parentResource_ptr->m_isWritable;
        if (m_originalResource_ptr && m_originalResource_ptr->m_overrideWritable)
            m_isWritable = m_isWritable && m_originalResource_ptr->m_isWritable;
    }

    //Owner
    if (resourceData.contains("owner"))
        m_owner = resourceData["owner"].toString();

    if (resourceData.contains("isDir")) {
        m_isDir = resourceData.value("isDir").toBool();
    }
    if (resourceData.contains("isFile"))
        m_isFile = resourceData.value("isFile").toBool();
    if (resourceData.contains("isExecutable"))
        m_isExecutable = resourceData.value("isExecutable").toBool();

    if (resourceData.contains("blockSize"))
        m_blockSize = resourceData.value("blockSize").toLongLong();
    if (resourceData.contains("bytesAvailable"))
        m_bytesAvailable = resourceData.value("bytesAvailable").toLongLong();
    if (resourceData.contains("bytesFree"))
        m_bytesFree = resourceData.value("bytesFree").toLongLong();
    if (resourceData.contains("bytesTotal"))
        m_bytesTotal = resourceData.value("bytesTotal").toLongLong();
    if (resourceData.contains("bytesUsed"))
        m_bytesUsed = resourceData.value("bytesUsed").toLongLong();

    if (resourceData.contains("nlink"))
        m_nLinkValue = resourceData.value("nlink").toLongLong();

    if (resourceData.contains("link"))
        m_link = resourceData.value("link").toString();

    if (resourceData.contains("linkData") && !m_isDirectShare ) {
        m_linkData = resourceData.value("linkData").toHash();
        if (m_parentResource_ptr == nullptr && m_sharedLink && m_isShareRoot)  {
            m_isWritable = m_linkData["writable"].toBool();
            m_overrideWritable = true;
        }
    }

    if (resourceData.contains("size"))
        m_size = resourceData.value("size").toLongLong();

    QHash<QString, QVariant> attrs;

    if (resourceData.contains("customAttributes")) {
        attrs = resourceData.value("customAttributes").toHash();
        foreach(const QString &key, attrs.keys())
            m_customAttributes[key] = attrs[key];
    }

    if (resourceData.contains("isRef") && resourceData.value("isRef").toBool()) {
        m_customAttributes["isRef"] = true;
    }

    // Capture itemPath from root level if present (from updateChild)
    if (resourceData.contains("itemPath")) {
        m_customAttributes["itemPath"] = resourceData["itemPath"].toString();
    }

    if (!m_sharedLinkId.isEmpty()) {
        m_customAttributes["sharedLinkId"] = m_sharedLinkId;
    }

    if (resourceData.contains("fileName")) { //Renaming share?
        m_fileName = resourceData["fileName"].toString();
        if (m_parentResource_ptr ) {
            updatePathByParent();
        } else {
            if (m_customAttributes.contains("isRef")) {
                 m_realPath = m_customAttributes["itemPath"].toString();
                 m_currentBackend->setRealPath(m_realPath);
            }

            m_remotePath = QString("/%1/").arg(m_fileName);
            m_displayPath = m_remotePath;
            
            if (!m_customAttributes.contains("isRef")) {
                m_currentBackend->setRealPath(m_remotePath);
            }
        }
    }

    //siteAttributes
    if (m_parentResource_ptr) {
        if (m_parentResource_ptr->m_customAttributes.contains("isSite"))
            m_customAttributes.insert("isSite", true);
        if (m_parentResource_ptr->m_customAttributes.contains("siteUrl"))
            m_customAttributes.insert("siteUrl", m_parentResource_ptr->m_customAttributes["siteUrl"]);
        if (m_parentResource_ptr->m_customAttributes.contains("siteName"))
            m_customAttributes.insert("siteName", m_parentResource_ptr->m_customAttributes["siteName"]);
        if (m_parentResource_ptr->m_customAttributes.contains("domain"))
            m_customAttributes.insert("domain", m_parentResource_ptr->m_customAttributes["domain"]);
    }

    bool isRef = false;
    if (resourceData.contains("isRef") && resourceData.value("isRef").toBool()) {
        m_customAttributes["isRef"] = true;
        isRef = true;
    } else if (m_customAttributes.contains("isRef")) {
        isRef = m_customAttributes["isRef"].toBool();
    }

    if (attrs.contains("itemPath") || resourceData.contains("itemPath")) {
        QString itemPath = attrs.contains("itemPath") ? attrs["itemPath"].toString() : resourceData["itemPath"].toString();
        
        if (!isRef && !m_remotePath.startsWith(itemPath)) {
            if (!itemPath.startsWith("/"))
                itemPath.prepend("/");
            QStringList parts = m_remotePath.split("/", Qt::SplitBehaviorFlags::SkipEmptyParts);
            if (!parts.isEmpty())
                itemPath = "/" + parts.first() + itemPath;
            m_customAttributes["itemPath"] = itemPath;
            m_remotePath = itemPath;
            if (m_originalResource_ptr) { // save in original
                m_originalResource_ptr->m_customAttributes["itemPath"] = itemPath;
                m_originalResource_ptr->m_remotePath = itemPath;
            }
            m_currentBackend->setRealPath(itemPath);
        } else if (isRef) {
             // Only update Real Path if this is an explicit Ref update (has isRef flag, e.g. from Server)
             // or if we don't have a real path yet.
             // This prevents RefreshInfo (from Device) from overwriting the full path with a local path.
             if (resourceData.contains("isRef") || m_realPath.isEmpty()) {
                 m_customAttributes["itemPath"] = itemPath;
                 m_realPath = itemPath;
                 m_currentBackend->setRealPath(itemPath);
             }
        }
    }
    else if (m_parentResource_ptr ) {
        updatePathByParent();
    }

    // Force inheritance of realPath context from parent if available
    // This overrides any 'local' path that might have been set above from device data
    if (m_parentResource_ptr && !m_parentResource_ptr->m_realPath.isEmpty() && !isRef) {
          QString parentRealPath = m_parentResource_ptr->m_realPath;
          if (!parentRealPath.endsWith("/")) parentRealPath.append("/");
          QString childRealPath = parentRealPath + m_fileName;
          m_realPath = childRealPath;
          m_currentBackend->setRealPath(childRealPath);
    }

    if (m_isShareRoot) {
        m_sharedLinkId = m_LinkID;
        m_customAttributes["sharedLinkId"] = m_LinkID;
    }

    if (!m_owner.isEmpty())
        m_customAttributes["owner"] = m_owner;

    if (resourceData.contains("managed"))
        m_customAttributes["managed"] = resourceData["managed"].toBool();

    if (resourceData.contains("mounted"))
        m_customAttributes["mounted"] = resourceData["mounted"].toBool();

    if (resourceData.contains("isRef"))
        m_customAttributes["isRef"] = resourceData["isRef"].toBool();

    if (resourceData.contains("localFS"))
        m_customAttributes["localFS"] = resourceData["localFS"].toBool();

    if (resourceData.contains("partial"))
        m_customAttributes["partial"] = resourceData["partial"].toBool();

    if (resourceData.contains("sites"))
        m_customAttributes["hasSites"] = true;

    if (resourceData.contains("localSearch"))
        m_customAttributes["localSearch"] = resourceData["localSearch"].toBool();

    if (resourceData.contains("localAI"))
        m_customAttributes["localAI"] = resourceData["localAI"].toBool();


    if (resourceData.contains("aiEmbedName"))
        m_customAttributes["aiEmbedName"] = resourceData["aiEmbedName"].toString();

    if (resourceData.contains("version"))
        m_customAttributes["version"] = resourceData["version"].toString();

    m_ETag = generateETag(true, this->fileName() + this->getQuery(), lastModifiedString(), this->size());


    if (resourceData.contains("bytesToCache")) {
        m_exists = true;
    }

    m_isActual = true;
    updateUrl();
}

void RemoteResource::doRefreshAllInfo(bool forceRefresh)
{

    quint64 timestampNow = QDateTime::currentMSecsSinceEpoch();
    if ((timestampNow - m_lastUpdated) > 1000)
    {
        m_isActual = false;
    }



	if (!m_isActual || forceRefresh)
    {
        qDebug() << "RemoteResource::doRefreshAllInfo path:" << m_remotePath << "backend:" << m_currentBackendType;

        if (m_currentBackendType == CloudResource::MemBackend)
        {
            m_MemBackend.RefreshInfo(this);
        }
        else
        {
			//file info including children
            m_currentBackend->RefreshAllInfo(this);
            QByteArray data = m_currentBackend->bytesToCache();
            if (data.size() > 0 && m_MemBackend.open(QIODevice::WriteOnly))
            {
                m_MemBackend.write(data);
                m_MemBackend.close();
                m_currentBackend = &m_MemBackend;
                m_currentBackendType = MemBackend;
            }
        }
        m_isActual = true;
        m_ETag = generateETag(true, fileName() + m_query, lastModifiedString(), this->size());
        m_lastUpdated = QDateTime::currentMSecsSinceEpoch();
    }

}

void RemoteResource::doRefresh()
{

	quint64 timestampNow = QDateTime::currentMSecsSinceEpoch();
	if ((timestampNow - m_lastUpdated) > 1000)
	{
		m_isActual = false;
	}

	if (!m_isActual )
	{
		//TimeLogger logger("Refreshing " + fileName());
		if (m_currentBackendType == CloudResource::MemBackend)
		{
			m_MemBackend.RefreshInfo(this);
		}
		else
		{
			//only file info
            m_currentBackend->RefreshInfo(this);
            QByteArray data = m_currentBackend->bytesToCache();
			if (data.size() > 0 && m_MemBackend.open(QIODevice::WriteOnly))
			{
				m_MemBackend.write(data);
				m_MemBackend.close();
				m_currentBackend = &m_MemBackend;
				m_currentBackendType = MemBackend;
			}
		}

		m_isActual = true;
        m_ETag = generateETag(true, fileName() + m_query, lastModifiedString(), this->size());
		m_lastUpdated = QDateTime::currentMSecsSinceEpoch();
	}
}


void RemoteResource::setOutdated()
{
    m_isActual = false;
}

void RemoteResource::SetParentOutdated()
{
	if (m_parentResource_ptr)
        m_parentResource_ptr->setOutdated();
}

QString RemoteResource::getContentType() const
{
    return m_contentType;
}

bool RemoteResource::isOpen() const
{
    return m_currentBackend->isOpen();
}

qint64 RemoteResource::openResource(QIODevice::OpenMode openMode)
{
	if (m_currentBackend->open(openMode))
		return m_currentBackend->fileHandle();
	else
        return FILEHANDLE_ERROR;
}

QByteArray RemoteResource::read(qint64 maxlen)
{
    return m_currentBackend->read(maxlen);
}

QByteArray RemoteResource::read(qint64 from, qint64 maxlen)
{
    return m_currentBackend->readFromPosition(from, maxlen);
}

void RemoteResource::requestRead(qint64 from, qint64 maxlen)
{
    m_currentBackend->readFromPosition(from, maxlen);
}

qint64 RemoteResource::write(const QByteArray &data)
{
    return m_currentBackend->write(data);
}

bool RemoteResource::exists() const
{
    return m_exists;
}


bool RemoteResource::parentExists() const
{
    return m_parentExists;
}

bool RemoteResource::remove(bool forced)
{
    if (m_customAttributes.contains("isRef") && m_customAttributes["isRef"].toBool())
    {
         if (m_currentBackend->removeShare()) {
             // Successfully sent unshare command. 
             // Schedule removal from parent to update the tree immediately, 
             // ensuring frontend refresh doesn't show the stale item.
             // Using QTimer::singleShot to avoid deleting 'this' while in use.
             if (m_parentResource_ptr) {
                 QString name = m_fileName;
                 RemoteResource* parent = m_parentResource_ptr;
                 QTimer::singleShot(0, [parent, name](){
                     parent->removeChild(name);
                 });
             }
             return true;
         }
         return false;
    } 
    
    removeAllChildren();
    return m_currentBackend->remove(forced);
}

bool RemoteResource::isDir() const
{
    return m_isDir;
}

bool RemoteResource::isFile() const
{
    return m_isFile;
}

QStringList RemoteResource::entryList()
{
    //cleanup skipped
    QStringList entries = m_childResources.keys();
    Q_FOREACH(QString entry, entries)
    {
        if (CloudResource::isSkipped(entry))
            entries.removeOne(entry);
    }
    return entries;
}

QString RemoteResource::path() const
{
    return m_displayPath;
}

bool RemoteResource::invoke(const QString &rpc, QHash<QString, QVariant> &params, QByteArray &payload)
{
    return m_currentBackend->invoke(rpc, params,  payload);
}

QString RemoteResource::getRemotePath() const
{
    return m_remotePath;
}


void RemoteResource::refresh()
{
    doRefresh();
}

QString RemoteResource::getPublicLink()
{
    return m_link;
}

QString RemoteResource::getPublicLink(QHash<QString, QVariant> linkData)
{
    m_link = m_currentBackend->getPublicLink(linkData);
    if (! m_linkData.isEmpty())
        m_linkData = linkData;
    return m_link;
}

QHash<QString, QVariant> RemoteResource::getLinkData() const
{
    return m_linkData;
}

void RemoteResource::removePublicLink()
{
    m_currentBackend->removePublicLink();
    m_link = "";
}

CloudResource *RemoteResource::getChildClone(QStringList childPath, QString query, bool withChildren)
{
    if (m_originalResource_ptr)
        return m_originalResource_ptr->getChildClone(childPath, query, withChildren);
     else
        return doGetChildClone(childPath, query, withChildren);
}

CloudResource *RemoteResource::getChild(QStringList childPath, bool create)
{
     return doGetChild(childPath, create);
}

CloudResource *RemoteResource::getChild(const QString &childName, bool create)
{
    if ( m_childResources.contains(childName)) {
        return m_childResources[childName];
    } else
    {
        if (create)
        {
            QString childRemotePath = m_remotePath.endsWith("/") ? m_remotePath + childName : m_remotePath + "/" + childName ;
            RemoteResource* child = new RemoteResource(m_LinkID, childRemotePath, QStringLiteral(""), false, this);
            child->m_sharedLinkId = this->m_sharedLinkId;
            child->m_overrideWritable = this->m_overrideWritable;
            child->m_owner = this->m_owner;
            if (m_currentBackendType == CloudResource::WebDAVBackendType && m_webdavBackend) {
                QHash<QString, QVariant> webdavData;
                webdavData["baseUrl"] = m_webdavBackend->baseUrl();
                webdavData["prefix"] = m_webdavBackend->prefix();
                child->initWebDAVBackend(webdavData);
            }
            m_childResources.insert(childName, child);
            if (! child->m_isActual) {
                child->doRefresh();
            }
            return child;
        } else
            return nullptr;
    }

}

CloudResource* RemoteResource::doGetChild(QStringList childPath, bool create)
{
    //QMutexLocker lock(&m_mutex);

    if (childPath.size() > 0)
    {
		//this->doRefreshAllInfo();

        QString childName = childPath.takeFirst();

        if ( m_childResources.contains(childName)) {
            return m_childResources[childName]->doGetChild(childPath, create);
        } else
        {
            if (create)
            {
                QString childRemotePath = m_remotePath.endsWith("/") ? m_remotePath + childName : m_remotePath + "/" + childName;
                RemoteResource* child = new RemoteResource(m_LinkID, childRemotePath, QStringLiteral(""), false, this);
                child->m_sharedLinkId = this->m_sharedLinkId;
                child->m_overrideWritable = this->m_overrideWritable;
                child->m_owner = this->m_owner;

                // Propagate WebDAV backend from parent to child
                if (m_currentBackendType == CloudResource::WebDAVBackendType && m_webdavBackend) {
                    QHash<QString, QVariant> webdavData;
                    webdavData["baseUrl"] = m_webdavBackend->baseUrl();
                    webdavData["prefix"] = m_webdavBackend->prefix(); // inherit root prefix
                    child->initWebDAVBackend(webdavData);
                }

                m_childResources.insert(childName, child);
                if (! child->m_isActual) {
                    child->doRefresh();
                }
                return child->doGetChild(childPath, create);
            } else
                return nullptr;
        }
    } else {
        return this;
    }

}

CloudResource* RemoteResource:: doGetChildClone(QStringList childPath, QString query, bool withChildren)
{
    if (childPath.size() == 0)
    {
        OnMyDiskLocker lock(&m_mutex);
        if (! lock.isLocked())
            qWarning() << "Can not lock self clone" << m_remotePath;
        RemoteResource*	childResource = new RemoteResource(this, query);
        lock.unlock();

        if (m_parentResource_ptr == nullptr && !m_isShareRoot && !this->getEntryData().isEmpty()) {
            childResource->LoadData(this->getData());
            if (withChildren) {
                childResource->LoadEntryData(this->getEntryData());
            }
        } else if (! m_LinkID.isEmpty()  ) {
            if (! childResource->m_isActual || withChildren || !query.isEmpty()) {
                if (withChildren)
                    childResource->doRefreshAllInfo();
                else
                    childResource->doRefresh();
            }
        } else if (m_currentBackendType == CloudResource::WebDAVBackendType) {
            if (withChildren)
                childResource->doRefreshAllInfo();
            else
                childResource->doRefresh();
        } else {
            childResource->LoadData(this->getData());
            if (withChildren) {
                childResource->LoadEntryData(this->getEntryData());
            }
        }
        return childResource;
    }

    // Check child
    QString childName = childPath.first();

    OnMyDiskLocker lock(&m_mutex);
    if (! lock.isLocked())
        qWarning() << "Can not lock in RemoteResource::doGetChildClone" << m_remotePath;

    if (m_childResources.contains(childName)) {
        childPath.removeFirst();
        lock.unlock();
        return m_childResources[childName]->doGetChildClone(childPath, query, withChildren);
    } else {

        if (!m_remotePath.endsWith("/"))
            m_remotePath.append("/");

        QString childRemotePath = m_remotePath + childName;
        bool final = childPath.count() == 1;

        RemoteResource* childResource = new RemoteResource(m_LinkID, childRemotePath, final ? query : "", false, this);
        childResource->m_sharedLinkId = this->m_sharedLinkId;
        childResource->m_isShareRoot = this->m_isShareRoot;
        childResource->m_overrideWritable = this->m_overrideWritable;
        childResource->m_isWritable = this->m_isWritable;
        childResource->m_owner = this->m_owner;
        childResource->m_sharedLink = this->m_sharedLink;
        childResource->m_isCopy = final;

        // Propagate WebDAV backend from parent to child
        if (m_currentBackendType == CloudResource::WebDAVBackendType && m_webdavBackend) {
            QHash<QString, QVariant> webdavData;
            webdavData["baseUrl"] = m_webdavBackend->baseUrl();
            webdavData["prefix"] = m_webdavBackend->prefix(); // inherit root prefix
            childResource->initWebDAVBackend(webdavData);
        }

        if (!final)
            m_childResources[childName] = childResource;

        lock.unlock();

        if (withChildren && final)
            childResource->doRefreshAllInfo();
        else
            childResource->doRefresh();

        if (!final) {
            childPath.removeFirst();
            return childResource->doGetChildClone(childPath, query, withChildren);
        } else {
            return childResource;
        }
    }
}

QDateTime RemoteResource::created() const
{
    return m_created;
}

void RemoteResource::close()
{
    m_currentBackend->close();
}

qint64 RemoteResource::pos() const
{
    return m_currentBackend->pos();
}

bool RemoteResource::seek(qint64 pos)
{
    return m_currentBackend->seek(pos);
}

bool RemoteResource::atEnd() const
{
    return m_currentBackend->atEnd();
}

bool RemoteResource::preventDelete()
{
    return m_sharedLink;
}

QString RemoteResource::urlPath()
{
    return m_displayPath;
}

QString RemoteResource::remotePath() const
{
    return m_remotePath;
}

QString RemoteResource::owner(const QString &user) const
{
    return m_owner.isEmpty() ? user : m_owner;
}

QHash<QString, RemoteResource *> RemoteResource::getChildResources()
{
    return m_childResources;
}


void RemoteResource::setShareRoot(bool isShareRoot, const QString& contentType)
{
    m_isShareRoot = isShareRoot;
    if (!contentType.isEmpty()) {
        m_contentType = contentType;
        m_isCustomContentType = true;
    }
}


void RemoteResource::updateChild(QString childName, QString linkID, QString itemPath, QHash<QString, QVariant> data)
{
    OnMyDiskLocker lock(&m_mutex);
    if (! lock.isLocked())
        qWarning() << "Can not lock for update" << m_remotePath;

    RemoteResource* refResource;
    if (!m_childResources.contains(childName)) {
        refResource = new RemoteResource(linkID, itemPath, "", false, nullptr);
        m_childResources.insert(childName, refResource);
    } else {
        refResource = m_childResources[childName];
    }
    // Ensure itemPath is available in data for LoadData to preserve in customAttributes
    if (!itemPath.isEmpty()) {
        data["itemPath"] = itemPath;
        QVariantHash attrs;
        if (data.contains("customAttributes"))
             attrs = data["customAttributes"].toHash();
        
        attrs["itemPath"] = itemPath;
        // attrs["realPath"] = itemPath; // Removed exposure
        data["customAttributes"] = attrs;
    }
    refResource->LoadData(data);
}

void RemoteResource::removeChildrenByLink(QString linkId)
{
    OnMyDiskLocker lock(&m_mutex);
    if (! lock.isLocked())
        qWarning() << "Can not lock in removeChildrenByLink" << m_remotePath;
    foreach(const QString& childName, m_childResources.keys()) {
        if (m_childResources[childName]->storageID() == linkId)
            doRemoveChild(childName, true);
    }
}

QString RemoteResource::ETag() const
{
    return m_ETag;
}

QString RemoteResource::lastModifiedString() const
{
    return QLocale::c().toString(m_lastModified.toUTC(),"ddd, dd MMM yyyy hh:mm:ss") + " GMT";
}

QDateTime RemoteResource::lastModified() const
{
    return m_lastModified;
}

QDateTime RemoteResource::lastRead() const
{
    return m_lastRead;
}

QString RemoteResource::fileName() const
{
    return m_fileName;
}

QUrl RemoteResource::url(QUrl baseUrl) const
{
    if (m_sharedLinkId.isEmpty()) {
        baseUrl.setPath(m_remotePath);
    } else {
        baseUrl.setPath(m_displayPath);
    }

    if (m_query.length() > 0)
        baseUrl.setQuery(m_query);
    return baseUrl;
}

qint64 RemoteResource::bytesAvailable() const
{
    return m_bytesAvailable;
}

qint64 RemoteResource::bytesUsed() const
{
    return m_bytesUsed;
}

int RemoteResource::blockSize() const
{
    return m_blockSize;
}

qint64 RemoteResource::bytesFree() const
{
    return m_bytesFree;
}

qint64 RemoteResource::bytesTotal() const
{
    return m_bytesTotal;
}

QString RemoteResource::storageID() const
{
    return m_LinkID;
}


QString RemoteResource::GetLinkID() const
{
    return m_LinkID;
}

RemoteResource *RemoteResource::getParentResource_ptr() const
{
    return m_parentResource_ptr;
}

QString RemoteResource::getQuery() const
{
    return m_query;
}

QString RemoteResource::GetID() const
{
    return m_LinkID;
}

bool RemoteResource::mkcol()
{
    if (m_currentBackend->mkcol())
    {
      //qDebug() << "mkcol:" << m_remotePath;
      m_isDir = true;
      m_exists = true;
	  m_isWritable = true;
      return true;
    }
    else
      return false;
}


qint64 RemoteResource::size() const
{
	return m_size;
}

void RemoteResource::updateUrl()
{
    if (this->isDir() && !m_remotePath.endsWith("/"))
    {
        m_remotePath.append("/");
    }
    if (this->isDir() && !m_displayPath.endsWith("/"))
    {
        m_displayPath.append("/");
    }

    if (this->isFile() && m_remotePath.endsWith("/"))
    {
        m_remotePath.chop(1);
    }

    if (this->isFile() && m_displayPath.endsWith("/"))
    {
        m_displayPath.chop(1);
    }

}

bool RemoteResource::doRemoveChild(QString childName, bool forceDelete)
{
    if (m_childResources.contains(childName))
    {
        RemoteResource* toRemove = m_childResources[childName];
        if (toRemove)
        {
            if ((forceDelete || !toRemove->preventDelete() || toRemove->isCopy()))
            {
                m_childResources.remove(childName);
                toRemove->setDeleting(true);
                // Destructor may block in LinkSocket::releaseSocket() waiting for a busy
                // socket. Never delete it synchronously here while holding locks or
                // processing a service message - it deadlocks the main thread.
                QTimer::singleShot(0, [toRemove] () {
                    delete toRemove;
                });
            } else
                toRemove->removeAllChildren();
        }
    }

    return true;
}


void RemoteResource::setDeleting(bool deleting)
{
    m_deleting = deleting;
    if (deleting)
    {
        m_originalResource_ptr = nullptr;
        m_parentResource_ptr = nullptr;
    }
}


bool RemoteResource::removeChild(QString childName)
{
    return doRemoveChild(childName);
}

bool RemoteResource::removeAllChildren()
{

    Q_FOREACH(QString childName, m_childResources.keys())
    {
        doRemoveChild(childName, m_deleting);
    }

    return true;
}

bool RemoteResource::copy(const QString &destPath)
{
    return m_currentBackend->copy(destPath);
}

bool RemoteResource::move(const QString &destPath)
{
    return m_currentBackend->move(destPath);
}

bool RemoteResource::rename(const QString &newName)
{
    return m_currentBackend->rename(newName);
}

int RemoteResource::nLink() const
{
    return m_nLinkValue;
}


int RemoteResource::childCount()
{
  int c = m_childResources.count();
  Q_FOREACH(RemoteResource * child, m_childResources)
	  c += child->childCount();
  return c;
}

int RemoteResource::cacheSize()
{
	int s = m_MemBackend.size();
	Q_FOREACH(RemoteResource * child, m_childResources)
		s += child->cacheSize();
	return s;
}

