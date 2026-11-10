#include "hostresource.h"
#include "localdb.h"

HostResource::HostResource(QString hostName, LocalResource *docRoot, LocalResource *themeRoot) :
	RootResource(docRoot, themeRoot), m_hostName(hostName), m_sharedResource(this)
{     

}


HostResource::~HostResource()
{
    //qInfo() << "Local resource " << this->fileName() << " gone away..";
}

CloudResource *HostResource::getChildClone(QStringList childPath, QString query, bool withChildren)
{
    QString rootDir = childPath.count() > 0 ? childPath.first() : "";
    if (rootDir == "shared")
    {
        childPath.removeFirst();
        m_sharedResource.refresh();
		if (childPath.count() > 0) {
            return  m_sharedResource.getChildClone(childPath, query, withChildren);
		} else {
            CloudResource* shared = new SharedResource(&m_sharedResource, this);
            return  shared;
        }
    }
    else
        return RootResource::getChildClone(childPath, query, withChildren);
}

int HostResource::childCount()
{
    QMutexLocker lock(&m_mutex);

	int c = m_localResources.count();
	Q_FOREACH(LocalResource* child,  m_localResources)
	{
		if (child)
		{
			c += child->childCount();
		}
	}
	return c;
}

int HostResource::cacheSize()
{
    QMutexLocker lock(&m_mutex);

    int s = m_docRootResource->cacheSize();
	Q_FOREACH(LocalResource* child,  m_localResources)
	{
		if (child)
		{
			s += child->cacheSize();
		}
	}
	return s;
}

CloudResource *HostResource::getChild(QStringList childPath, bool create )
{
    QMutexLocker lock(&m_mutex);
    QString rootDir = childPath.count() > 0 ? childPath.first() : "";
    if (rootDir == "shared")
    {
        childPath.removeFirst();
        m_sharedResource.refresh();
        if (childPath.count() > 0)
            return  m_sharedResource.getChild(childPath, false);
        else
            return  &m_sharedResource;
    }
    else
        return RootResource::getChild(childPath, create);

}



CloudResource *HostResource::getChild(const QString childName, bool create)
{
	QMutexLocker lock(&m_mutex);
    if (childName == "shared")
    {
        m_sharedResource.refresh();
       return  &m_sharedResource;
    }
    else if (m_localResources.contains(childName)) {
        LocalResource *child =  m_localResources[childName];
        if (create)
            child->refresh();
        return child;
    } else
         return RootResource::getChild(childName, create);
}



QHash<QString, QVariant> HostResource::getData() const
{
    QHash<QString, QVariant> resourceData;

    resourceData.insert("size", size());
    resourceData.insert("lastModDate", lastModified());
    resourceData.insert("createdDate", created());
    resourceData.insert("lastReadDate", lastRead());
#ifdef EMBEDDED
    resourceData.insert("contentType", "embedded");
#else
    resourceData.insert("contentType", "desktop");
#endif
    resourceData.insert("exists", true);
    resourceData.insert("isReadable", true);
    resourceData.insert("isWritable", false);
    resourceData.insert("parentExists", true);
    resourceData.insert("isDir", true);
    resourceData.insert("isFile", false);
    resourceData.insert("isExecutable", true);

    #if QT_VERSION >= 0x050600
    resourceData.insert("blockSize", blockSize());
    #else
    resourceData.insert("blockSize", 512);
    #endif

    resourceData.insert("bytesAvailable", bytesAvailable());
    resourceData.insert("bytesFree", bytesFree());
    resourceData.insert("bytesTotal", bytesTotal());
    resourceData.insert("bytesUsed", bytesTotal() > bytesAvailable() ? bytesTotal() - bytesAvailable() : 0);
    resourceData.insert("nlink", m_localResources.size());
    resourceData.insert("storageID", QString(QByteArray(m_hostName.toUtf8()).toBase64()));

    return resourceData;
}



QHash<QString, QHash<QString, QVariant> > HostResource::getEntryData() const
{
    QHash<QString, QHash<QString, QVariant> > entryData;

	QStringList entryList = m_localResources.keys();

	Q_FOREACH(QString entryName, entryList)
    {
        if (entryName == "root") continue; //skip hidden root
		LocalResource* child = m_localResources[entryName];
		if (child)
        {
            if (! child->isActual())
                child->refreshData();
            entryData.insert(entryName, child->getData());
        }
    }

    QStringList remoteEntryList = m_remoteResources.keys();
    Q_FOREACH(QString entryName, remoteEntryList)
    {
        if (entryName == "root") continue; //skip hidden root
        RemoteResource* child = m_remoteResources[entryName];
        if (child)
        {
            if (! child->isActual())
                child->refresh();
            entryData.insert(entryName, child->getData());
        }
    }

    return entryData;
}

QHash<QString, QVariant> HostResource::getEntryAttributes(const QString& path, bool dirsOnly)
{
    QStringList pathList = path.split("/", Qt::SkipEmptyParts);
    QHash<QString, QVariant>  entryData;
    CloudResource *c = this->getChildClone(pathList, "", true);
    Q_FOREACH(QString entryName, c->entryList()) {
        CloudResource *entry = c->getChild(entryName);
        if (!dirsOnly || entry->isDir())
            entryData.insert(entryName, c->getChild(entryName)->getCustomAttributes());
    }
    delete c;
    return entryData;
}

bool HostResource::removeAllChildren()
{
	QMutexLocker lock(&m_mutex);
	Q_FOREACH(LocalResource* child,  m_localResources)
	{
		if (child)
		{
			child->removeAllChildren();
		}
	}
	return  true;
}

void HostResource::lockFile(const QString &fileName)
{
	QMutexLocker lock(&m_mutex);
    if (! m_fileLocks.contains(fileName))
        m_fileLocks.append(fileName);
}

bool HostResource::fileLocked(const QString &fileName)
{
	QMutexLocker lock(&m_mutex);
	bool locked = m_fileLocks.contains(fileName);
	return locked;
}


void HostResource::unlockFile(const QString &fileName)
{
	QMutexLocker lock(&m_mutex);
	if (m_fileLocks.contains(fileName)) {
            m_fileLocks.removeAll(fileName);
    }
}

void HostResource::removeShare(const QString &share)
{
    QMutexLocker lock(&m_mutex);
    m_sharedResource.removeSharedLinksFromShare(share);
    if (m_localResources.contains(share)) {
        LocalResource* child = m_localResources[share];
        m_localResources.remove(share);
        delete child;
    }
    m_sharedResource.refresh();
}


QString HostResource::localToRemote(const QString &path)
{
	QMutexLocker lock(&m_mutex);
    QStringList entryList = m_localResources.keys();

    Q_FOREACH(QString entryName, entryList)
    {
            QString localSharePath = m_localResources[entryName]->getLocalPath();
            if (path.startsWith(localSharePath))
            {
                QString remotePart = QDir::fromNativeSeparators(path.mid(localSharePath.length()));
                QString remotePath = QString("/%1/%2/%3").arg(m_hostName, entryName, remotePart);
                return remotePath;
            }
     }
    return "";
}

