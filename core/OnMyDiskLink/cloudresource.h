#ifndef CLOUDRESOURCE_H
#define CLOUDRESOURCE_H

#include <QObject>
#include <QIODevice>
#include <QFile>
#include <QDir>
#include <QBuffer>
#include <QFileInfo>
#include <QMimeType>
#include <QMimeDatabase>
#include <QLocale>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDebug>
#include <QMutex>
#include <QMutexLocker>
#include <QTimer>
#include <QString>
#include <QUrl>
#include <QStorageInfo>
#include <QHostInfo>
#include <QUuid>
#include <QRandomGenerator>
#include "linkmessage.h"
#include "linksocket.h"
#include "timelogger.h"
#include "iresourcebackend.h"

/* paket size: 2MB, 200K for embedded */

#ifdef OPENWRT
#define PACKET_SIZE 262144
#else
#define PACKET_SIZE 2097152
#endif
#define PACKETS_AHEAD 15

/* Disk cache 1GB */
#define DISK_CACHE_SIZE 1073741824

/* RAM cache 512MB */
#define RAM_CACHE_SIZE 536870912


enum ResourceType {collection, document};

class  CloudResource
{
public:

    enum BackendType {
        RemoteLinkBackend = 0,
        FileBackend,
        MemBackend,
        WSBackend,
        WebDAVBackendType
    };


    explicit CloudResource();
    virtual ~CloudResource(){}
    virtual QString ETag() const = 0;
    virtual QString lastModifiedString() const = 0;
    virtual QString getContentType() const = 0;
    virtual bool isOpen() const = 0;
    virtual qint64 size() const = 0;
    virtual bool seek(qint64 pos) = 0;
    virtual qint64 pos() const = 0;
	virtual qint64 openResource(QIODevice::OpenMode openMode) = 0;
    virtual void close() = 0;
    virtual QByteArray read(qint64 maxlen) = 0;
    virtual QByteArray read(qint64 from, qint64 maxlen) = 0;
    virtual qint64 write(const QByteArray &data) = 0;
    virtual bool atEnd() const = 0;
    virtual bool exists() const = 0;
    virtual bool isReadable() const = 0;
    virtual bool isWritable() const= 0;
    virtual bool isExecutable() const = 0;
    virtual bool mkcol() = 0;
    virtual bool parentExists() const = 0;
    virtual bool remove(bool forced = false) { Q_UNUSED (forced); return  false; }
    virtual bool isDir() const = 0;
    virtual bool isFile() const= 0;
	virtual QStringList entryList() = 0;
    virtual void refresh() = 0;
    virtual CloudResource* getChild(QStringList childPath,  bool create = true) = 0; /* depreciated */
    virtual CloudResource* getChild(const QString& childName,  bool create = false);
    virtual CloudResource* getChildClone(QStringList childPath, QString query = "", bool withChildren = false) = 0;
    virtual QString getPublicLink() { return ""; }
    virtual QString getPublicLink(QHash<QString, QVariant> linkData) {Q_UNUSED(linkData); return ""; }
    virtual QString path() const = 0;
    virtual bool removeChild(QString childName) = 0;
    virtual bool removeAllChildren() = 0;
    virtual void removePublicLink() { }
    virtual QDateTime created() const = 0;
    virtual QDateTime lastModified() const = 0;
    virtual QDateTime lastRead() const = 0;
    virtual QString fileName() const  = 0;
    virtual QUrl url(QUrl baseUrl) const = 0;
    virtual int blockSize() const = 0;
    virtual qint64 bytesAvailable() const = 0;
    virtual qint64 bytesFree() const = 0;
    virtual qint64 bytesTotal() const = 0;
    virtual qint64 bytesUsed() const = 0;
    virtual QString storageID() const = 0;
    virtual void updateUrl() = 0;
    virtual QString toString();
    virtual int nLink() const = 0;
    virtual QString GetID() const = 0;
    virtual QString GetLinkID() const { return ""; }
    virtual bool isCopy() const = 0;
    virtual void SetParentOutdated() = 0;
    virtual void LoadData(QHash<QString, QVariant> resourceData) = 0;
    virtual void LoadEntryData(QHash<QString, QHash<QString, QVariant>> entryData) = 0;
    virtual QHash<QString, QVariant> getData() const = 0;
    virtual QHash<QString, QHash<QString, QVariant> > getEntryData() const = 0;
    virtual QHash<QString, QVariant> getCustomAttributes() const;
    virtual QHash<QString, QVariant> getLinkData() const;

    //Common methods
    virtual ResourceType getResourceType() const;
    virtual QString getResourceTypeString()  const;
    virtual void copyTo(CloudResource* dstResource, QHash<QString, QString>  *failedPaths);
    virtual void moveTo(CloudResource* dstResource, QHash<QString, QString>  *failedPaths);
    virtual bool copy(const QString& destPath) { Q_UNUSED(destPath); return false; }
    virtual bool move(const QString& destPath) { Q_UNUSED(destPath); return false; }
    virtual bool rename(const QString& newName) { Q_UNUSED(newName); return false; }
    virtual QString getPermissions() const;
    virtual QString getBoundary();
    virtual void setSessionId(QString sessionId) { m_sessionId = sessionId; }
    virtual QString sessionId() { return m_sessionId; }
    virtual QString generateETag(bool quoted, QString path, QString lastModDate, qint64 size) const;
    virtual bool invoke(const QString& rpc, QHash<QString, QVariant>&params,  QByteArray& payload )  { Q_UNUSED(rpc); Q_UNUSED(params); Q_UNUSED(payload); return false; }
    virtual QString owner(const QString& user = "") const;
    virtual QString remotePath() const;
    virtual IResourceBackend* backend() {return nullptr; }

    //Static data
    static int m_Handle;
    static quint64 m_maxFileSize;

    static bool isImage(const QString& fileName);
    static bool isSkipped(const QString& fileName);
    static bool isUnlisted(const QString& fileName);
    static bool isPlain(const QString& fileName);
    static bool isConvertible(const QString& fileName);
    static bool isForbidden(const QString& name);
private:
    QString m_boundary = "";
    QString m_sessionId = "";
    const QString boundaryCharacters = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    void generateBoundary(quint16 length);
};


#endif // CLOUDRESOURCE_H
