#ifndef REMOTERESOURCE_H
#define REMOTERESOURCE_H

#include <QObject>
#include <QUuid>
#include <memory>
#include <QPointer>
#include <QMutex>
#include <QRecursiveMutex>
#include "cloudresource.h"
#include "webdavbackend.h"
#include "linksocket.h"
#include "linkmessage.h"
#include "timelogger.h"
#include <QThread>
#include <QThread>
#include "memfilebackend.h"
#include "iresourcebackend.h"
#include "linkbackend.h"
#ifdef SERVER
#include "wsbackend.h"
#endif

class  RemoteResource: public CloudResource
{
public:    
    explicit RemoteResource(RemoteResource *resource, QString query = "");
    explicit RemoteResource(QString linkID, QString remotePath, QString query, bool sharedLink, RemoteResource* parent);
    ~RemoteResource();

    bool isCopy() const;

    // CloudResource interface
    QString ETag() const;
    QString lastModifiedString() const;
    QString getContentType() const;
    bool isOpen() const;
    bool seek(qint64 pos);
    qint64 pos() const;
	qint64 openResource(QIODevice::OpenMode m_openMode);
    void close();
    bool remove(bool forced = false);
    QByteArray read(qint64 maxlen);
    QByteArray read(qint64 from, qint64 maxlen);
    void requestRead(qint64 from, qint64 maxlen);
    qint64 write(const QByteArray &data);
    bool atEnd() const;
    bool exists() const;
    bool isReadable() const;
    bool isWritable() const;
    bool isExecutable() const;
    bool mkcol();
    bool parentExists() const;
    bool isDir() const;
    bool isFile() const;
	QStringList entryList();
    void refresh() ;
    CloudResource *getChildClone(QStringList childPath, QString query, bool getFromOriginal = false);
    CloudResource *getChild(QStringList childPath, bool create = true);
    CloudResource *getChild(const QString &childName, bool create = false);
    QString getPublicLink();
    QString getPublicLink(QHash<QString, QVariant> linkData);
    QHash<QString, QVariant> getLinkData() const;
    void removePublicLink();
    QDateTime created() const;
    QDateTime lastModified() const;
    QDateTime lastRead() const;
    QString fileName() const;
    QUrl url(QUrl baseUrl) const;
    int blockSize() const;
    qint64 bytesAvailable() const;
    qint64 bytesTotal() const;
    qint64 bytesFree() const;
    qint64 bytesUsed() const;
    QString storageID() const;

    void updateUrl();
    int nLink() const;
    void setUrlPrefix(QString prefix);

    void LoadData(QHash<QString, QVariant> resourceData);
    void LoadEntryData(QHash<QString, QHash<QString, QVariant>> entryData);
    QHash<QString, QVariant> getData() const;
    QHash<QString, QHash<QString, QVariant> > getEntryData() const;
    QHash<QString, QVariant> getCustomAttributes() const;
    QString GetID() const;
    IResourceBackend* backend(){return m_currentBackend; }

    // RemoteResource interface
    bool removeChild(QString childName);
    bool removeAllChildren();

    bool copy(const QString& destPath);
    bool move(const QString& destPath);
    bool rename(const QString& newName);

    QString GetLinkID() const;

	int childCount();
	int cacheSize();

private:
    QString m_fileName;
    QString m_ETag;
    QString m_contentType;
    bool m_sharedLink;
    QString m_remotePath; //sent to client
    QString m_displayPath; //shown
    bool m_exists;
    bool m_isReadable;
    bool m_isWritable;
    bool m_isExecutable;
    bool m_overrideWritable;
    qint64 m_size;
    QDateTime m_lastModified;
    QDateTime m_lastRead;
    QDateTime m_created;
    bool m_parentExists;
    bool m_isDir;
    bool m_isFile;
    int m_blockSize;
    qint64 m_bytesAvailable, m_bytesUsed, m_bytesFree, m_bytesTotal;
    QString m_resourceID;
    int m_copyesCount;   
    int m_nLinkValue;
    QString m_LinkID;
    QString m_link;
    QHash<QString, QVariant> m_linkData;
	bool m_isActual;
    QString m_sharedLinkId;
    QString m_owner;
    qint64 m_lastUpdated;
    bool m_deleting;
    bool m_isShareRoot = false;
    bool m_isDirectShare = false;
    bool m_isCustomContentType = false;
protected:
    QRecursiveMutex m_mutex;
    QHash<QString, RemoteResource*> m_childResources;
    QHash<QString, QVariant> m_customAttributes;
    RemoteResource* m_originalResource_ptr;
    RemoteResource* m_parentResource_ptr;
    bool m_isCopy;
    QString m_realPath; // Internal storage for fully qualified path (e.g. /Hostname/path/to/file)

    QString m_query;
    BackendType m_currentBackendType;


    void updatePathByParent();

    MemFileBackend m_MemBackend;
    LinkBackend m_linkBackend;
#ifdef SERVER
    WebSocketBackend* m_wsBackend = nullptr;
#endif
    WebDAVBackend* m_webdavBackend = nullptr;
    IResourceBackend* m_currentBackend;

    CloudResource *doGetChildClone(QStringList childPath, QString query, bool withChildren);
    CloudResource *doGetChild(QStringList childPath, bool create);

protected:

    bool preventDelete();
    QString urlPath();
    void doRefreshAllInfo(bool forceRefresh = false);
	void doRefresh();
    bool doRemoveChild(QString childName, bool forceDelete = false);
    void setDeleting(bool deleting);


signals:
    void timeToGo(QString resourceName);

    // CloudResource interface
public:
    qint64 size() const;
    void SetParentOutdated();
    QByteArray getMemBackendData() const;
    QString getQuery() const;
    RemoteResource *getParentResource_ptr() const;
    void setOutdated();
    QString getRemotePath() const;
    CloudResource::BackendType getCurrentBackendType() const;
    bool isActual();
    QString getMemBackendContentType() const;
    QString path() const;
    bool invoke(const QString& rpc, QHash<QString, QVariant>&params,  QByteArray& payload) ;
    QString remotePath() const;
    QString owner(const QString& user) const;
    
    // Explicit Real Path accessor - not exposed in attributes
    QString realPath() const { return m_realPath; }
    void setRealPath(const QString& path) { m_realPath = path; }

    QHash<QString, RemoteResource*> getChildResources();
    //void setCustomContentType(const QString& contentType);
    void setShareRoot(bool isShareRoot, const QString &contentType = "");
    bool isSharedRoot() { return m_isShareRoot; }
    QString sharedLinkId(){ return m_sharedLinkId; }
    void updateChild(QString childName,  QString linkID, QString itemPath, QHash<QString, QVariant> data);
    void removeChildrenByLink(QString linkId);
    bool initWebDAVBackend(const QHash<QString, QVariant> &webdavData);
};

#endif // REMOTERESOURCE_H
