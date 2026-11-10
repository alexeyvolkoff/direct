#ifndef HOSTRESOURCE_H
#define HOSTRESOURCE_H

#include "cloudresource.h"
#include <memory>
#include <QNetworkInterface>
#include <QTimer>
#include <QThread>
#include <QFileSystemWatcher>
#include <QFileDevice>
#include <QDir>
#include <QFileInfo>
#include <QPointer>
#include "rootresource.h"
#include "sharedresource.h"

#include <QSemaphore>
#if defined(Q_OS_WIN)
#define HOST_USERS_ROOT "C:/Users"
#else
#define HOST_USERS_ROOT "/home"
#endif

class  HostResource : public RootResource
{
public:
    explicit HostResource(QString hostName, LocalResource *docRoot, LocalResource *themeRoot);
    ~HostResource();

private:
    QString m_hostName;
    QString m_storageID;
    SharedResource m_sharedResource;

    QList<QString> m_fileLocks;

public:
	// CloudResource interface
	QHash<QString, QVariant> getData() const;
    QHash<QString, QHash<QString, QVariant> > getEntryData() const;

    QString hostName() { return m_hostName; }
	void setHostName(const QString& hostName) { m_hostName = hostName;}
	QString localToRemote(const QString& path);
    QString storageID() const { return m_storageID; }
    void setStorageId(const QString& storageID) { m_storageID = storageID; }

    CloudResource *getChild(QStringList childPath, bool create = false);
    CloudResource *getChild(const QString childName, bool create = false);
    CloudResource *getChildClone(QStringList childPath, QString query = "", bool withChildren = true);

	int childCount();	
	int cacheSize();
	bool removeAllChildren();
	void lockFile(const QString& fileName);
	bool fileLocked(const QString& fileName);
	void unlockFile(const QString& fileName);
    void removeShare(const QString& share);
    QHash<QString, QVariant>  getEntryAttributes(const QString &path, bool dirsOnly = true);
};

#endif // HOSTRESOURCE_H
