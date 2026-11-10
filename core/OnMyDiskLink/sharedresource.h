#ifndef SHAREDRESOURCE_H
#define SHAREDRESOURCE_H
#include "localresource.h"
#include "rootresource.h"
#include <QMutex>


class SharedResource : public LocalResource
{
private:
    RootResource* m_rootResource;
    QMap<QString, CloudResource*> m_sharedItems;
    QMap<QString, CloudResource*> m_itemsByPath;
    QMutex m_mutex;
    void updateSharedItems();
public:
    explicit SharedResource(RootResource* parentResource);
    explicit SharedResource(SharedResource* originalResource,  RootResource* parentResource);
    ~SharedResource();
    QHash<QString, QVariant> getData() const;
    void LoadEntryData(QHash<QString, QHash<QString, QVariant> > entryList);
    QHash<QString, QHash<QString, QVariant> > getEntryData() const;
    void refresh();
    void removeSharedLinksFromShare(const QString& share);
	QStringList entryList();
    CloudResource *getChild(QStringList childPath, bool create = false);
    CloudResource *getChild(const QString &childName, bool create = false);
    CloudResource *getChildClone(QStringList childPath, QString query = "", bool withChildren = true);
};

#endif // SHAREDRESOURCE_H
