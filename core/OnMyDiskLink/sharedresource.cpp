#include "sharedresource.h"
#include "localdb.h"


void SharedResource::updateSharedItems()
{
    QMap<QString, QString> links = LocalDB::LocalDB::instance().publicLinkItems();
    Q_FOREACH(QString key, links.keys())
    {
        QString itemPath = links[key];
        if (! m_itemsByPath.keys().count(itemPath))
        {
            CloudResource* child =  m_rootResource->getChildClone(itemPath.split("/", Qt::SplitBehaviorFlags::SkipEmptyParts));
            if (child->exists())
            {
                QString childName = child->fileName();
                int c = 0;
                while (m_sharedItems.keys().count(childName) > 0)
                    childName = QString("%1-%2").arg(childName, QString::number(++c));

                m_sharedItems.insert(childName, child);
                m_itemsByPath.insert(itemPath, child);
            } else {
                links.remove(key);
            }

            m_rootResource->close();
        }
    }

    Q_FOREACH(QString childName, m_sharedItems.keys())
    {
        CloudResource* child = m_sharedItems[childName];
        QString itemPath = child->path();
        if (! links.values().contains(itemPath))
        {
            delete child;
            m_sharedItems.remove(childName);
            m_itemsByPath.remove(itemPath);
        }
    }
}

SharedResource::SharedResource(RootResource* parentResource): LocalResource(QString("/shared"), QString("/shared"), QString("shared"), "", true, nullptr),
    m_rootResource(parentResource)
{

}

SharedResource::SharedResource(SharedResource *originalResource, RootResource *parentResource): LocalResource(originalResource),
    m_rootResource(parentResource)
{
	LoadData(originalResource->getData());
	LoadEntryData(originalResource->getEntryData());
}

SharedResource::~SharedResource()
{
    Q_FOREACH(CloudResource* child, m_sharedItems)
    {
        delete child;
    }
    m_itemsByPath.clear();
    m_sharedItems.clear();
}

CloudResource *SharedResource::getChild(const QString& childName, bool create)
{
    Q_UNUSED(create)
    QMutexLocker locker(&m_mutex);

    if (! m_sharedItems.contains(childName))
          return nullptr;
    return m_sharedItems[childName];
}


CloudResource *SharedResource::getChild(QStringList childPath, bool create)
{
    QMutexLocker locker(&m_mutex);

    if (childPath.count() > 0)
    {
      QString childName = childPath[0];

      if (! m_sharedItems.contains(childName))
          return nullptr;

      childPath.removeFirst();
      if (childPath.count() > 0)
        return m_sharedItems[childName]->getChild(childPath, create);
      else
        return m_sharedItems[childName];
    } else
        return nullptr;
}

CloudResource *SharedResource::getChildClone(QStringList childPath, QString query, bool withChildren)
{
    QMutexLocker locker(&m_mutex);

    if (childPath.count() > 0)
    {
      updateSharedItems();
      QString childName = childPath[0];

      if (! m_sharedItems.contains(childName))
          return nullptr;

      childPath.removeFirst();
      if (childPath.count() > 0)
        return m_sharedItems[childName]->getChildClone(childPath, query, withChildren);
      else
        return m_rootResource->getChildClone(m_sharedItems[childName]->path().split("/", Qt::SplitBehaviorFlags::SkipEmptyParts), query, withChildren);
    }
    return nullptr;
}



QHash<QString, QVariant> SharedResource::getData() const
{
    QHash<QString, QVariant> resourceData;

    resourceData.insert("size", m_rootResource->size());
    resourceData.insert("lastModDate", m_rootResource->lastModified());
    resourceData.insert("createdDate", m_rootResource->created());
    resourceData.insert("lastReadDate", m_rootResource->lastRead());
    resourceData.insert("contentType", "inode/directory");
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

    resourceData.insert("bytesAvailable", m_rootResource->bytesAvailable());
    resourceData.insert("bytesFree", m_rootResource->bytesFree());
    resourceData.insert("bytesTotal", m_rootResource->bytesTotal());
    resourceData.insert("bytesUsed", m_rootResource->bytesTotal() > m_rootResource->bytesAvailable() ? m_rootResource->bytesTotal() - m_rootResource->bytesAvailable() : 0);
    resourceData.insert("nlink", m_rootResource->size());
    resourceData.insert("storageID", m_rootResource->storageID());

    return resourceData;
}


void SharedResource::refresh()
{
    QMutexLocker locker(&m_mutex);
    LoadData(getData());
    LoadEntryData(getEntryData());
}

void SharedResource::removeSharedLinksFromShare(const QString &share)
{
    QMutexLocker locker(&m_mutex);
    Q_FOREACH(QString key, m_itemsByPath.keys())
    {
        if (key.startsWith( QString("/%1").arg(share))) {
            CloudResource* child = m_itemsByPath[key];
            m_itemsByPath.remove(key);

            Q_FOREACH(QString key, m_sharedItems.keys())
            {
                if (m_sharedItems[key] == child){
                    m_sharedItems.remove(key);
                }
            }
            delete child;
        }
    }
}

QStringList SharedResource::entryList()
{
	QMutexLocker locker(&m_mutex);
    return m_sharedItems.keys();
}



void SharedResource::LoadEntryData(QHash<QString, QHash<QString, QVariant> > entryList)
{
    Q_UNUSED(entryList)
    updateSharedItems();
}


QHash<QString, QHash<QString, QVariant>> SharedResource::getEntryData() const
{

    QHash<QString, QHash<QString, QVariant> > entryData;

    QStringList entryList = m_sharedItems.keys();
    Q_FOREACH(QString entryName, entryList)
    {
        CloudResource* child = m_sharedItems[entryName];
        if (child)
        {
            entryData.insert(entryName, child->getData());
        }
    }    

    return entryData;
}
