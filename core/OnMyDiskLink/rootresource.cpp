#include "rootresource.h"
#include <QTimer>

RootResource::RootResource(LocalResource *docRoot, LocalResource *themeRoot)
	: CloudResource(),
      m_docRootResource(docRoot),
      m_themeResource(themeRoot)

{
	m_resourceID = QUuid::createUuid().toString();
	m_sharedlinkID = "";
	m_sharedRootPath = "";
    m_sharedRootItem = nullptr;
	doRefreshAllInfo();
    m_isCopy = false;
}

RootResource::RootResource(RootResource *orig)
	: CloudResource(),
      m_docRootResource(orig->m_docRootResource),
      m_themeResource(orig->m_themeResource)
{
	m_resourceID = orig->m_resourceID;
	m_lastModified = orig->m_lastModified;
	m_lastRead = orig->m_lastRead;
	m_created = orig->m_lastRead;

	m_sharedlinkID = orig->m_sharedlinkID;
	m_sharedRootPath = orig->m_sharedRootPath;
    m_sharedRootItem = nullptr;
    m_attributes = orig->m_attributes;

	Q_FOREACH(LocalResource* childResource, orig->m_localResources)
	{
        m_localResources.insert(childResource->fileName(), new LocalResource(childResource));
	}

	Q_FOREACH(RemoteResource* childResource, orig->m_remoteResources)
	{
        m_remoteResources.insert(childResource->fileName(), new RemoteResource(childResource));
	}
    m_isCopy = false;
}

RootResource::~RootResource()
{
    //avoid double deletion of shared root subitems
    if (m_sharedRootItem )
    {
        if (m_sharedRootItem->isDir())
        {
            QHash<QString, RemoteResource *> items = m_sharedRootItem->getChildResources();
            foreach(RemoteResource *item, items) {
                m_remoteResources.remove(item->fileName());
            }
        } else
             m_remoteResources.remove(m_sharedRootItem->fileName());

        delete m_sharedRootItem;
        m_sharedRootItem = nullptr;
    }
    //remove all
    removeAllChildren();

}

void RootResource::addLocalResource(QString shareName, QString sharePath)
{
    QMutexLocker lock(&m_mutex);
    if (! m_localResources.contains(shareName))
    {
      LocalResource *localResource = new LocalResource(sharePath, QString("/%1").arg(shareName), shareName, QStringLiteral(""), false, nullptr);
      m_localResources.insert(shareName, localResource);
    }
}

void RootResource::addLocalResource(QString shareName, QString sharePath, QVariantHash attrs)
{
	QMutexLocker lock(&m_mutex);
    LocalResource *localResource;
    if (! m_localResources.contains(shareName))
    {
      localResource = new LocalResource(sharePath, QString("/%1").arg(shareName), shareName, QStringLiteral(""), false, nullptr);
      m_localResources.insert(shareName, localResource);
    } else
      localResource = m_localResources[shareName];

    foreach(const QString& key, attrs.keys())
        localResource->setCustomAttribute(key, attrs[key]);

}

QString RootResource::findSharedPath(QString filePath)
{
    QMutexLocker lock(&m_mutex);
    QString sharedFile = "";
    foreach(RemoteResource *r, m_remoteResources) {
        if (r->isSharedRoot() && filePath.startsWith(r->remotePath())) {
             int p = r->remotePath().length();
             QString remainingPath =  filePath.mid(p);
             sharedFile = r->path() + remainingPath;
            break;
        }
    }
    return sharedFile;
}

void RootResource::removeLocalResource(QString shareName)
{
    if (m_localResources.contains(shareName))
    {
        LocalResource* r = m_localResources.take(shareName);
        delete r;
    }
}

void RootResource::removeLocalResources()
{
    QMutexLocker lock(&m_mutex);
    Q_FOREACH(QString shareName, m_localResources.keys())
    {
        removeLocalResource(shareName);
    }
}

void RootResource::removeShare(QString shareName)
{
    QMutexLocker lock(&m_mutex);
    removeLocalResource(shareName);
}

bool RootResource::hasRemoteShare(QString shareName)
{
    return m_remoteResources.contains(shareName);
}

void RootResource::removeRemoteShare(QString shareName)
{
    QMutexLocker lock(&m_mutex);
    if (m_remoteResources.contains("shared-by-me")) {
         m_remoteResources["shared-by-me"]->removeChild(shareName);
    }
    removeRemoteResource(shareName);
}



void RootResource::addRemoteReference(QString refFolderName, QString refName,  QString linkID, QString itemPath, QHash<QString, QVariant> data)
{
    QMutexLocker lock(&m_mutex);
    RemoteResource* refFolderResource;
    if (! m_remoteResources.contains(refFolderName))
    {
       refFolderResource = new RemoteResource("", QString("/%1").arg(refFolderName), "", false, nullptr);
       QHash<QString, QVariant> refFolderData;
       refFolderData["isWritable"] = false;
       refFolderData["isReadable"] = true;
       refFolderData["isDir"] = true;
       refFolderData["exists"] = true;
       refFolderData["isRef"] = true;
       refFolderData["mounted"] = true;
       refFolderResource->LoadData(refFolderData);

       m_remoteResources.insert(refFolderName, refFolderResource);
    } else {
       refFolderResource = m_remoteResources[refFolderName];
    }
    data["isRef"] = true;
    refFolderResource->updateChild(refName, linkID, itemPath, data);
}


void RootResource::addWebDAVResource(QString shareName, QHash<QString, QVariant> shareData)
{
    QMutexLocker lock(&m_mutex);
    QString itemPath = QString("/%1").arg(shareName);

    if (m_remoteResources.contains(shareName)) {
        RemoteResource *existing = m_remoteResources[shareName];
        if (existing) {
            existing->initWebDAVBackend(shareData.value("webdav").toHash());
            existing->LoadData(shareData);
        }
        return;
    }

    
    RemoteResource *remoteResource = new RemoteResource("webdav", itemPath, "", false, nullptr);
    remoteResource->setShareRoot(true, "folder-shared");

    if (remoteResource->initWebDAVBackend(shareData.value("webdav").toHash())) {
        remoteResource->LoadData(shareData);
        if (!m_remoteResources.contains(shareName))
            m_remoteResources.insert(shareName, remoteResource);
    } else {
        delete remoteResource;
    }
}

void RootResource::addLinkedResource(QString shareName, QString linkID, QString itemPath,  QHash<QString, QVariant> shareData, QString contentType)
{
    QMutexLocker lock(&m_mutex);
    if (! m_remoteResources.contains(shareName))
    {
        RemoteResource* remoteResource = new RemoteResource(linkID, itemPath, "", false, nullptr);
        remoteResource->setShareRoot(true, contentType);
        remoteResource->LoadData(shareData);
        m_remoteResources.insert(shareName, remoteResource);
    } else {
       m_remoteResources[shareName]->LoadData(shareData); //apply access rights changes
    }
}

void RootResource::updateRemoteResource(QString shareName, QString linkID, QHash<QString, QVariant> shareData, QHash<QString, QHash<QString, QVariant> > shareEntryData)
{
    QMutexLocker lock(&m_mutex);

    RemoteResource* remoteResource = childByLink(linkID);
    if (! remoteResource)
    {
        QString resourceRemotePath = QString("/%1").arg(shareName);
        remoteResource = new RemoteResource(linkID, resourceRemotePath, "", false, nullptr);// shareData, shareEntryData);
        m_remoteResources.insert(shareName, remoteResource);
    } else {
        shareData["fileName"] = shareName;
        m_remoteResources.remove(remoteResource->fileName());
        m_remoteResources.insert(shareName, remoteResource);
    }
    remoteResource->LoadData(shareData);
    remoteResource->LoadEntryData(shareEntryData);
}


void RootResource::updateSharedRoot()
{
    m_sharedRootItem->refresh();
    m_attributes = m_sharedRootItem->getCustomAttributes();
}

void RootResource::addSharedRoot(QString linkID, QString itemPath, QString owner, bool directShare, bool writable)
{
	m_sharedRootPath = itemPath;
	m_sharedlinkID = linkID;
	m_sharedRoot = true;
    m_owner = owner;
    QHash<QString, QVariant> shareData;
    shareData["overrideWritable"] = true;
    shareData["sharedLinkId"] = linkID;
    if (directShare) {
        shareData["isWritable"] = writable;
        shareData["directShare"] = directShare;
    }
    shareData["owner"] = owner;

    if (!m_sharedRootItem)
        m_sharedRootItem = new RemoteResource(m_sharedlinkID, m_sharedRootPath, "", true, nullptr);
    m_sharedRootItem->setShareRoot(true);
    m_sharedRootItem->LoadData(shareData);

    //m_sharedRootItem->setSessionId(sessionId());
}

void RootResource::clearSharedRoot()
{
    if (m_sharedRootItem) {
        QHash<QString, QVariant> shareData;
        shareData["exists"] = false;
        m_sharedRootItem->LoadData(shareData);
        m_sharedRootItem->setShareRoot(false);
    }
}


bool RootResource::removeAllChildren()
{
    QMutexLocker lock(&m_mutex);
	Q_FOREACH(QString childName, m_remoteResources.keys())
	{
		removeRemoteResource(childName);
	}

	Q_FOREACH(QString childName, m_localResources.keys())
	{
		removeLocalResource(childName);
	}

	return true;
}

void RootResource::removeRemoteResource(QString shareName)
{
    QMutexLocker lock(&m_mutex);

    if (m_remoteResources.contains(shareName))
    {
        RemoteResource* r = m_remoteResources[shareName];
        m_remoteResources.remove(shareName);
        // Destructor may block in LinkSocket::releaseSocket() waiting for a busy
        // socket. Never delete it synchronously here while holding locks or
        // processing a service message - it deadlocks the main thread.
        QTimer::singleShot(0, [r] () {
                    delete r;
        });
    }
}

void RootResource::removeRemoteResources(QString linkID)
{
    QMutexLocker lock(&m_mutex);

    Q_FOREACH(QString shareName, m_remoteResources.keys())
    {
        QString shareLinkID = m_remoteResources[shareName]->GetLinkID();
        if (shareLinkID == linkID)
        {
            removeRemoteResource(shareName);
        }
    }
}


void RootResource::removeRemoteResources(QString refFolderName, QString linkID)
{
    QMutexLocker lock(&m_mutex);
    if (m_remoteResources.contains(refFolderName))
    {
        m_remoteResources[refFolderName]->removeChildrenByLink(linkID);
    }
}

void RootResource::removeRemoteResourcesByName(QString refFolderName, QString name)
{
    QMutexLocker lock(&m_mutex);
    if (m_remoteResources.contains(refFolderName))
    {
        m_remoteResources[refFolderName]->removeChild(name);
    }

}


QStringList RootResource::getSharesByLink(QString linkID)
{
    QStringList result;
    Q_FOREACH(QString shareName, m_remoteResources.keys())
    {
        QString shareLinkID = m_remoteResources[shareName]->GetLinkID();
        if (shareLinkID == linkID)
        {
            result << shareName;
        }
    }
	return  result;
}

QString RootResource::sharedRootName()
{
    return (m_sharedRootItem ) ? m_sharedRootItem->fileName() :  QFileInfo(m_sharedRootPath).fileName();
}

void RootResource::addCustomThemeItem(QString itemName, QString itemPath)
{
    m_themeResource->addSymbolicLink(itemName, itemPath);
}

void RootResource::addCustomRootItem(QString itemName, QString itemPath)
{
    m_docRootResource->addSymbolicLink(itemName, itemPath);
}

void RootResource::setSessionId(QString sessionId)
{
   CloudResource::setSessionId(sessionId);
   m_docRootResource->setSessionId(sessionId);
}

QString RootResource::ETag() const
{
    return generateETag(true, "/", m_lastModified.toString(), 4096);
}

QString RootResource::lastModifiedString() const
{
    return QLocale::c().toString(m_lastModified.toUTC(),"ddd, dd MMM yyyy hh:mm:ss") + " GMT";
}

QString RootResource::path() const
{
    return "/";
}

void RootResource::doRefreshAllInfo(bool forceRefresh)
{
    Q_UNUSED(forceRefresh)

//m_lastModified = QDateTime::fromMSecsSinceEpoch(0);
    m_lastModified = QDateTime::currentDateTime();
    m_lastRead = QDateTime::fromMSecsSinceEpoch(0);
    m_created = QDateTime::currentDateTime();

    Q_FOREACH(CloudResource* childResource, m_localResources)
    {
        if (m_lastModified.secsTo(childResource->lastModified()) > 0)
            m_lastModified = childResource->lastModified();

        if (m_lastRead.secsTo(childResource->lastRead()) > 0)
            m_lastRead = childResource->lastRead();

        if (m_created.secsTo(childResource->created()) < 0)
            m_created = childResource->created();
    }

    Q_FOREACH(CloudResource* childResource, m_remoteResources)
    {
        if (m_lastModified.secsTo(childResource->lastModified()) > 0)
            m_lastModified = childResource->lastModified();

        if (m_lastRead.secsTo(childResource->lastRead()) > 0)
            m_lastRead = childResource->lastRead();

        if (m_created.secsTo(childResource->created()) < 0)
            m_created = childResource->created();
    }
}

void RootResource::SetParentOutdated()
{
    QMutexLocker lock(&m_mutex);
    doRefreshAllInfo();
}

QString RootResource::getContentType() const
{
    return QString("httpd/unix-directory");
}

bool RootResource::exists() const
{
    return true;
}

bool RootResource::isReadable() const
{
    return true;
}

bool RootResource::isWritable() const
{
    if (m_sharedRoot && m_sharedRootItem)
      return m_sharedRootItem->isWritable();
    else
      return false;
}

bool RootResource::mkcol()
{
    if (m_sharedRoot && m_sharedRootItem && m_sharedRootItem->isWritable())
        return m_sharedRootItem->mkcol();
    else
        return false;
}

bool RootResource::parentExists() const
{
    return false;
}


bool RootResource::isDir() const
{
    return true;
}

bool RootResource::isFile() const
{
    return false;
}

QStringList RootResource::entryList()
{
    QStringList childNames;
    QMutexLocker lock(&m_mutex);


    Q_FOREACH(const QString& childName, m_remoteResources.keys())
    {
        if (!CloudResource::isSkipped(childName))
            childNames.append(childName);
    }

    Q_FOREACH(const QString& childName, m_localResources.keys())
    {
        if (!childNames.contains(childName) && !CloudResource::isSkipped(childName))
            childNames.append(childName);
    }
    return childNames;
}

QStringList RootResource::remoteEntryList()
{
    QMutexLocker lock(&m_mutex);

    QStringList childNames;

    Q_FOREACH(RemoteResource* childResource, m_remoteResources)
    {
        childNames.append(childResource->fileName());
    }
    return childNames;
}

void RootResource::refresh()
{
    doRefreshAllInfo();
}

CloudResource *RootResource::getChild(QStringList childPath, bool create)
{
    QMutexLocker lock(&m_mutex);

    if (childPath.size() == 0) {
        if (m_sharedRoot && m_sharedRootItem)
            return m_sharedRootItem;
        else
            return this;
    }

    QString childName = "/";
    if (childPath.size() > 0)
        childName = childPath.at(0);

    CloudResource *child = nullptr;
    if (m_remoteResources.contains(childName))
    {
        child = m_remoteResources[childName];
    }

    if (m_localResources.contains(childName))
    {
        child = m_localResources[childName];
    }

    lock.unlock();
    if (child) {
        childPath.removeFirst();
        return child->getChild(childPath, create);
	// do not need to create non-existing files in docroot and theme
    } else if (m_sharedRootItem &&  m_sharedRootItem->fileName() == childName) {
        childPath.removeFirst();
        return m_sharedRootItem->getChild(childPath, false);
    } else if (childName == "customtheme") {
		childPath.removeFirst();
        return m_themeResource->getChild(childPath, false);
    } else {
        return m_docRootResource->getChild(childPath, false);
    }
}

CloudResource *RootResource::getChild(const QString &childName, bool create)
{
    Q_UNUSED(create)
    QMutexLocker lock(&m_mutex);

    if (m_sharedRoot && m_sharedRootItem && m_sharedRootItem->fileName() == childName)
            return m_sharedRootItem;

    CloudResource *child = nullptr;

    if (m_localResources.contains(childName)) {
        child = m_localResources[childName];
    } else if (m_remoteResources.contains(childName))
    {
        child =  m_remoteResources[childName];
    }

    if (create && child) {
        lock.unlock();
        child->refresh();
    }

    return child;
}

CloudResource *RootResource::getChildClone(QStringList childPath, QString query, bool withChildren)
{
    QMutexLocker lock(&m_mutex);

    if (childPath.size() == 0) {
        if (m_sharedRoot && m_sharedRootItem)
            return m_sharedRootItem->getChildClone(childPath, query, withChildren);
        else
            return this;
    }

    QString  childName = childPath.first();

    CloudResource *childRoot = nullptr;

    if (m_remoteResources.contains(childName))
    {
        childRoot = m_remoteResources[childName];
        //return m_remoteResources[childName]->getChildClone(childPath, query, withChildren);
    } else if (m_localResources.contains(childName))
    {
        childRoot = m_localResources[childName];
    }



    if (childRoot) {
        childPath.removeFirst();
        lock.unlock(); // Unlock before recursive call to avoid deadlock
        return  childRoot->getChildClone(childPath, query, withChildren);
    } else if (childName == "customtheme") {
		childPath.removeFirst();
        return m_themeResource->getChildClone(childPath, query, withChildren);
    } else  if (m_sharedRootItem &&  m_sharedRootItem->fileName() == childName) {
        childPath.removeFirst();
        RemoteResource* sharedRoot = m_sharedRootItem;
        lock.unlock();
        return sharedRoot->getChildClone(childPath, query, withChildren);
    }  else {
        return m_docRootResource->getChildClone(childPath, query, withChildren);
    }
}

QDateTime RootResource::created() const
{
    return m_created;
}

QDateTime RootResource::lastModified() const
{
    return m_lastModified;
}

QString RootResource::fileName() const
{
    return QString("/");
}

bool RootResource::isExecutable() const
{
    return true;
}

QUrl RootResource::url(QUrl baseUrl) const
{
    baseUrl.setPath("/");
    return baseUrl;
}

QString RootResource::storageID() const
{
    return QString("");
}

QString RootResource::linkForPath(QStringList parsedPath)
{
	if (parsedPath.size() == 0)
		return "";

    QString linkID = "";

    QString rootShare = parsedPath.first();

    if (m_remoteResources.contains(rootShare))
    {
        linkID = m_remoteResources[rootShare]->GetLinkID();
    }

    return linkID;
}

QString RootResource::linkForRef(QStringList requestPath)
{
  CloudResource *c = getChild(requestPath, false);
  if (c) {
    return c->storageID();
  } else
    return "";
}

QString RootResource::rootPathForLink(const QString &linkId)
{
	QString rootPath = "";
	Q_FOREACH(RemoteResource *r, m_remoteResources)
	{
		if (r->GetLinkID() == linkId) {
			rootPath = r->fileName();
			break;
		}
	}
	return  rootPath;
}

RemoteResource * RootResource::childByLink(const QString &linkId)
{
    RemoteResource *child = nullptr;
    Q_FOREACH(RemoteResource *r, m_remoteResources)
    {
        if (r->GetLinkID() == linkId && !r->isSharedRoot()) {
            child = r;
            break;
        }
    }
    return  child;
}


void RootResource::acquire(RootResource *other)
{
	Q_FOREACH(RemoteResource *r, other->m_remoteResources)
    {
        other->m_remoteResources.remove(r->fileName());
        m_remoteResources.insert(r->fileName(), r);
    }
}

void RootResource::close()
{

}

qint64 RootResource::pos() const
{
    return -1;
}

qint64 RootResource::size() const
{
    return 0;
}

bool RootResource::seek(qint64 pos)
{
    Q_UNUSED(pos);
    return false;
}

bool RootResource::atEnd() const
{
    return false;
}

int RootResource::remoteChildCount()
{
    int c = 0;
    foreach(const QString& name, m_remoteResources.keys())
        if (!isUnlisted(name))
            ++c;
    return c;
}


bool RootResource::isOpen() const
{
    return false;
}

QByteArray RootResource::read(qint64 maxlen)
{
    Q_UNUSED(maxlen)
    return QByteArray("");
}

qint64 RootResource::write(const QByteArray &data)
{
    Q_UNUSED(data)
    return 0;
}


void RootResource::requestRead(qint64 from, qint64 maxlen)
{
    Q_UNUSED(from)
    Q_UNUSED(maxlen)
}

qint64 RootResource::openResource(QIODevice::OpenMode openMode)
{
    Q_UNUSED(openMode)
	return 1;
}

void RootResource::updateUrl()
{

}

QByteArray RootResource::read(qint64 pos, qint64 maxlen)
{
    Q_UNUSED(pos)
    Q_UNUSED(maxlen)
    return QByteArray("");
}

QString RootResource::GetID() const
{
    return m_resourceID;
}

bool RootResource::removeChild(QString childName)
{
    Q_UNUSED(childName)
    return false;
}

QDateTime RootResource::lastRead() const
{
    return m_lastRead;
}

bool RootResource::resize(qint64 newSize)
{
    Q_UNUSED(newSize)
    return false;
}

int RootResource::blockSize() const
{
    if (m_docRootResource->exists())
        return m_docRootResource->blockSize();

    if (m_localResources.count() > 0)
        return m_localResources.values().at(0)->blockSize();

    if (m_remoteResources.count() > 0)
        return m_remoteResources.values().at(0)->blockSize();

    return 512;
}

qint64 RootResource::bytesAvailable() const
{
    qint64 totalAvailable = 0;
    if (m_docRootResource->exists())
        totalAvailable += m_docRootResource->bytesAvailable();

    QList<QString> knownStorages;
    Q_FOREACH(CloudResource* childResource, m_localResources)
    {
        if (!knownStorages.contains(childResource->storageID()))
        {
            totalAvailable += childResource->bytesAvailable();
            knownStorages.append(childResource->storageID());
        }
    }

    Q_FOREACH(CloudResource* childResource, m_remoteResources)
    {
        if (!knownStorages.contains(childResource->storageID()))
        {
            totalAvailable += childResource->bytesAvailable();
            knownStorages.append(childResource->storageID());
        }
    }

    return totalAvailable;
}

qint64 RootResource::bytesFree() const
{
    qint64 totalFree = 0;
    if (m_docRootResource->exists())
        totalFree += m_docRootResource->bytesFree();

    QList<QString> knownStorages;
    Q_FOREACH(CloudResource* childResource, m_localResources)
    {
        if (!knownStorages.contains(childResource->storageID()))
        {
            totalFree += childResource->bytesFree();
            knownStorages.append(childResource->storageID());
        }
    }

    Q_FOREACH(CloudResource* childResource, m_remoteResources)
    {
        if (!knownStorages.contains(childResource->storageID()))
        {
            totalFree += childResource->bytesFree();
            knownStorages.append(childResource->storageID());
        }
    }

    return totalFree;
}

qint64 RootResource::bytesTotal() const
{
    qint64 totalTotal = 0;
    if (m_docRootResource->exists())
        totalTotal += m_docRootResource->bytesTotal();

    QList<QString> knownStorages;
    Q_FOREACH(CloudResource* childResource, m_localResources)
    {
        if (!knownStorages.contains(childResource->storageID()))
        {
            totalTotal += childResource->bytesTotal();
            knownStorages.append(childResource->storageID());
        }
    }

    Q_FOREACH(CloudResource* childResource, m_remoteResources)
    {
        if (!knownStorages.contains(childResource->storageID()))
        {
            totalTotal += childResource->bytesTotal();
            knownStorages.append(childResource->storageID());
        }
    }

    return totalTotal;
}

qint64 RootResource::bytesUsed() const
{
    return bytesTotal() > bytesAvailable() ? bytesTotal() - bytesAvailable() : 0;
}

int RootResource::nLink() const
{
    return 2 + m_localResources.count() + m_remoteResources.count();
}

bool RootResource::resizeImage(int width, int height)
{
    Q_UNUSED(width)
    Q_UNUSED(height)
    return false;
}

void RootResource::LoadData(QHash<QString, QVariant> resourceData)
{
    Q_UNUSED(resourceData)
}

void RootResource::LoadEntryData(QHash<QString, QHash<QString, QVariant> > entryList)
{
    Q_UNUSED(entryList)
}

bool RootResource::isCopy() const
{
    return m_isCopy;
}

QHash<QString, QVariant> RootResource::getData() const
{
    return QHash<QString, QVariant>();
}

QHash<QString, QHash<QString, QVariant> > RootResource::getEntryData() const
{
    return QHash<QString, QHash<QString, QVariant> >();
}

QString RootResource::getLocalPath(QStringList childPath)
{
    QMutexLocker lock(&m_mutex);
    QString localPath = "/";
    if (childPath.size() == 0)
        return  localPath;

    QString childName = "";
    if (childPath.size() > 0)
        childName = childPath.at(0);
    else
        return localPath;

    if (m_localResources.keys().contains(childName))
    {
        childPath.removeFirst();
        localPath = m_localResources[childName]->getLocalPath() + "/" + childPath.join("/");
    }

    return localPath;
}

QString RootResource::getRemotePath(QStringList childPath)
{
    QString remotePath = path();
    if (childPath.size() == 0)
        return  remotePath;

    CloudResource *c = getChildClone(childPath);
    c->refresh();
    remotePath = c->remotePath();
    if (c->isCopy())
        delete c;
    return remotePath;
}

QString RootResource::linkVersion(QString shareName)
{
    if (m_remoteResources.contains(shareName)) {
        return m_remoteResources[shareName]->getCustomAttributes()["version"].toString();
    } else
        return "";
}

QString RootResource::rootPath(QString shareName)
{
    if (m_remoteResources.contains(shareName)) {
        return m_remoteResources[shareName]->remotePath();
    } else
        return "";
}


QString RootResource::remotePath() const
{
    if (m_sharedRoot && m_sharedRootItem) {
        return m_sharedRootItem->getCustomAttributes()["itemPath"].toString();
    } else {
        return "";
    }
}




int RootResource::childCount()
{
	int c = m_localResources.count();
	Q_FOREACH(RemoteResource* child,  m_remoteResources)
	{
		if (child)
		{
			c += child->childCount();
		}
	}
	return c;
}

int RootResource::cacheSize()
{
    int s = m_docRootResource->cacheSize();
	Q_FOREACH(RemoteResource* child,  m_remoteResources)
	{
		if (child)
		{
			s += child->cacheSize();
		}
    }
    return s;
}

CloudResource* RootResource::touch(QStringList childPath)
{
    QMutexLocker lock(&m_mutex);

    if (childPath.size() == 0) {
        return nullptr;
    }

    QString childName = "/";
    if (childPath.size() > 0)
        childName = childPath.at(0);

    if (childName == "customtheme") {
        return nullptr;
    }

    CloudResource *childRoot = nullptr;
    if (m_remoteResources.contains(childName))
    {
        childRoot = m_remoteResources[childName];
    }

    if (m_localResources.contains(childName))
    {
        childRoot = m_localResources[childName];
    }

    lock.unlock();

    if(childRoot) {
        childPath.removeFirst();
        return childRoot->getChildClone(childPath, "", false);
    } else if (m_sharedRootItem ) {
       if (m_sharedRootItem->fileName() == childName)
            childPath.removeFirst();
      return m_sharedRootItem->getChildClone(childPath, "", false);
    } else
      return getChildClone(childPath, "", false);
}
