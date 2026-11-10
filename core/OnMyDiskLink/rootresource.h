#ifndef ROOTRESOURCE_H
#define ROOTRESOURCE_H

#include "cloudresource.h"
#include "localresource.h"
#include "remoteresource.h"

class RootResource : public CloudResource
{
private:
	QString m_sharedlinkID = "";
	QString m_sharedRootPath = "";
    QString m_linkVersion = "";
    bool m_isCopy = false;
    RemoteResource * m_sharedRootItem = nullptr;
    bool m_sharedRoot = false;
    QString m_owner = "";
    QHash<QString, QVariant> m_attributes;
    RemoteResource *childByLink(const QString &linkId);
public:
    explicit RootResource(LocalResource *docRoot, LocalResource *themeRoot);
	RootResource(RootResource* orig);
	~RootResource();
    void addLocalResource(QString shareName, QString sharePath);
    void removeLocalResources();
    void removeShare(QString shareName);
    bool hasRemoteShare(QString shareName);
    void removeRemoteShare(QString shareName);
    void addRemoteReference(QString refFolderName, QString refName,  QString linkID, QString itemPath, QHash<QString, QVariant> data);
    void addLinkedResource(QString shareName,   QString linkID, QString itemPath, QHash<QString, QVariant> shareData, QString contentType = "folder-shared");
    void addWebDAVResource(QString shareName, QHash<QString, QVariant> shareData);
    void updateRemoteResource(QString shareName, QString linkID, QHash<QString, QVariant> shareData, QHash<QString, QHash<QString, QVariant> > shareEntryData);
    void addSharedRoot(QString linkID, QString itemPath, QString owner, bool directShare, bool writable); //called from HTTP thread
    void clearSharedRoot(); //called from HTTP thread
    void updateSharedRoot(); // called from link thread
    void removeRemoteResources(QString linkID);
    void removeRemoteResources(QString refFolderName, QString linkID);
    void removeRemoteResourcesByName(QString refFolderName, QString name);
    QStringList  getSharesByLink(QString linkID);
    bool isSharedRoot() { return m_sharedRoot; }
	QString sharedRootName();
    QString sharedLinkId() { return m_sharedlinkID; }
    QString sharedRootPath() { return m_sharedRootPath; }
    void addCustomThemeItem(QString itemName, QString itemPath);
    void addCustomRootItem(QString itemName, QString itemPath);
    virtual void setSessionId(QString sessionId);
    QString getLocalPath(QStringList childPath);
    QString getRemotePath(QStringList childPath);
    QString linkVersion(QString shareName);
    QString rootPath(QString shareName);

	// CloudResource interface
public:
    QString ETag() const;
    QString lastModifiedString() const;
    void SetParentOutdated();
    QString getContentType() const;
    bool exists() const;
    bool isReadable() const;
    bool isWritable() const;
    bool mkcol();
    bool parentExists() const;
    bool isDir() const;
    bool isFile() const;
	QStringList entryList();
    QStringList remoteEntryList();
    void refresh();
    CloudResource *getChild(QStringList childPath, bool create = true);
    CloudResource *getChild(const QString &childName, bool create = false);
    CloudResource *getChildClone(QStringList childPath, QString query = "", bool withChildren = false);
    QDateTime created() const;
    QDateTime lastModified() const;
    QString fileName() const;
    bool isExecutable() const;
    QUrl url(QUrl baseUrl) const;
    QString storageID() const;
    QHash<QString, QVariant> getCustomAttributes() const { return m_attributes; }
    void  setCustomAttribute(const QString& name, const QVariant& value)  {  m_attributes[name] = value; }
    QString linkForPath(QStringList requestPath);
    QString linkForRef(QStringList requestPath);
    QString rootPathForLink(const QString& linkId);
	void acquire(RootResource* other);
    RemoteResource* sharedRootItem() { return m_sharedRootItem; }

protected:
	QRecursiveMutex m_mutex;
    QDateTime m_lastModified;
    QDateTime m_lastRead;
    QDateTime m_created;
    QString m_resourceID;
    bool m_indexed = false;
	int m_copyCount;
    LocalResource *m_docRootResource;
    LocalResource *m_themeResource;

    QHash<QString, LocalResource*> m_localResources;
    QHash<QString, RemoteResource*> m_remoteResources;
    void doRefreshAllInfo(bool forceRefresh = false);
    void removeRemoteResource(QString shareName);
    void removeLocalResource(QString shareName);
    // QIODevice interface
public:
    void close();
    qint64 pos() const;
    qint64 size() const;
    bool seek(qint64 pos);
    bool atEnd() const;

    // useful methods
    int remoteChildCount();
	int childCount();
	int cacheSize();
    CloudResource *touch(QStringList childPath);

    // CloudResource interface
public:
    bool isOpen() const;
    QByteArray read(qint64 maxlen);
    QByteArray read(qint64 pos, qint64 maxlen);
    qint64 write(const QByteArray &data);
	qint64 openResource(QIODevice::OpenMode openMode);
    void updateUrl();
    QString GetID() const;
    bool removeChild(QString childName);
    QDateTime lastRead() const;
    bool resize(qint64 newSize);
    int blockSize() const;
    qint64 bytesAvailable() const;
    qint64 bytesFree() const;
    qint64 bytesTotal() const;
    qint64 bytesUsed() const;
    int nLink() const;
    bool removeAllChildren();
    bool resizeImage(int width, int height);
    void LoadData(QHash<QString, QVariant> resourceData);
    void LoadEntryData(QHash<QString, QHash<QString, QVariant> > entryList);
    QString path() const;
    bool isCopy() const;
    QString remotePath() const;

    QHash<QString, QVariant> getData() const;
    QHash<QString, QHash<QString, QVariant> > getEntryData() const;

    void requestRead(qint64 from, qint64 maxlen);
    bool isIndexed() {return m_indexed;}
    void setIndexed(bool indexed) {m_indexed = indexed;}
    void addLocalResource(QString shareName, QString sharePath, QVariantHash attrs);
    QString findSharedPath(QString filePath);
};


#endif // ROOTRESOURCE_H
