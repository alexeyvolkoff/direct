#ifndef LOCALFILE_H
#define LOCALFILE_H

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
#include "memfilebackend.h"
#include "exifstructures.h"
#include "iresourcebackend.h"
#include "localfilebackend.h"



class  LocalResource : public CloudResource
{
public:
    explicit LocalResource(QString filePath, QString remotePath, QString itemName, QString query, bool preventdDelete, LocalResource* parentResource = nullptr);
    explicit LocalResource(LocalResource *resource, QString query = "");
    ~LocalResource();
    QString ETag() const;
    QString lastModifiedString() const;
    QDateTime LastModifiedDayTime() const;
    QString getContentType() const;
    QString getLocalPath() const;
    QString getRemotePath() const;
    void close();
    //qint64 pos() const;
    qint64 size() const;
    bool seek(qint64 pos);
	qint64 openResource(QIODevice::OpenMode  openMode);
    void SetParentOutdated();
    QString GetID() const;
    bool removeChild(QString childName);
    void updateUrl();
    QDateTime lastRead() const;
    int nLink() const;
    bool isOpen() const;
    bool atEnd() const;
    QByteArray read(qint64 maxlen);
    QByteArray read(qint64 pos, qint64 maxlen);
    qint64 write(const QByteArray &data);
    bool exists() const;
    bool mkcol();
    bool parentExists() const;
    bool isDir() const;
    bool isFile() const;
	QStringList entryList();
    void refresh();
    CloudResource* getChildClone(QStringList childPath, QString query, bool withChildren = false);
    QString getPublicLink();
    QString getPublicLink(QHash<QString, QVariant> linkData);
    void removePublicLink();
    CloudResource *getChild(QStringList childPath, bool create);
    CloudResource *getChild(const QString &childName, bool create = false);
    QDateTime created() const;
    QDateTime lastModified() const;
    QString fileName() const ;
    QString path() const;
    bool isExecutable() const;
    bool isReadable() const;
    bool isWritable() const;
    QUrl url(QUrl baseUrl) const;
    qint64 bytesAvailable() const;
    qint64 bytesUsed() const;
    int blockSize() const;
    qint64 bytesFree() const;
    qint64 bytesTotal() const;
    bool remove(bool forced = false);
    ResourceType getResourceType() const;
    QString storageID() const;
    QString absoluteFilePath() const;
    QHash<QString, QVariant> getData() const;
    QHash<QString, QHash<QString, QVariant>> getEntryData() const;
    QHash<QString, QVariant> getCustomAttributes() const;
    QHash<QString, QVariant> getLinkData() const;
    bool copy(const QString& destPath);
    bool move(const QString& destPath);
    bool rename(const QString& newName);


	bool isActual();
    void setOutdated();
    void setCustomAttribute(const QString& name, const QVariant& value)  {  m_customAttributes[name] = value; }
private:
    QString m_remotePath;
    LocalFileBackend m_FileBackend;
    MemFileBackend m_MemBackend;
    IResourceBackend* m_currentBackend;
    QString m_itemName;
    bool m_isCopy;
    bool m_deleting;
    QHash<QString, QVariant> m_customAttributes;

    QHash<QString, LocalResource*> m_childResources;
    LocalResource* m_originalResource_ptr;
    LocalResource* m_parentResource_ptr;
   // bool doLoadIcon(int size, bool openDir);
	QString m_ETag;
    QString m_link;
    QHash<QString, QVariant> m_linkData;
	QString m_contentType;
	QString m_storageID;
    bool m_preventDelete;
    bool m_exists;
    bool m_isReadable;
    bool m_isWritable;
    bool m_isExecutable;
	bool m_isWriting;
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
	bool m_isActual;
	BackendType m_currentBackendType;
	QString m_query;

    CloudResource *doGetChildClone(QStringList childPath, QString query, bool withChildren);
    CloudResource *doGetChild(QStringList childPath, bool create = true);
    void doRefreshAllInfo(bool forceRefresh = false);
	void doRefresh();
    void doRefreshAttributes();
    bool doRemoveChild(QString childName);
	LocalResource *getParent();
	bool processQuery();
    bool parseTemplate(const QString &contentType);
    bool parseMetaTags();

    QImage loadAudioArt(int imgWidth, int imgHeight);
    QImage loadVideoArt(int imgWidth, int imgHeight, int fromSecond, bool deleteTmp, bool coverRefresh);
    friend class PreviewGenerator;
    // WP 3.2 - Multiformat text processing
    void addVideoMetadataToAttributes();
    void addImageMetadataToAttributes();
    void addImagePngMetadataToAttributes();
    void addPdfMetadataToAttributes();
    void addMetaDocsMetadataToAttributes();
    QHash<QString, QVariant> getAttributesForMetaDocsPdf(const QString& metaPdfFilePath);
    // WP 3.2 - Multiformat text processing
    void addVideoDurationToAttributes();
    void updateExcludeFromIndexing(bool excludeFromIndexing);
    void addVideoDimensionsToAttributes();
    void transcodeVideo();
    void downscaleVideo(const QString& downscale, const QString& downscaleWidth, const QString& downscaleHeight);
    void continueTranscodeVideo();
    void transcodeAbort(const QString& transcodePid, const QString& mode, const QString& downscaleWidth, const QString& downscaleHeight);
    bool checkTmpFolder(QString path);
    QString m_transcodeAbortPid;
    // WP 3.2 - Multiformat text processing
    QString metaToCustomAttributes(const QString& meta);
    QString capitalizeFirstLetter(const QString &str);
    QString getExiv2ValueFromLine(const QString &line);
    QString getExiv2Latitude(const QString &latitude, const QString &latitudeRef);
    QString getExiv2Longitude(const QString &longitude, const QString &longitudeRef);
    QString gpsExiv2Altitude(const QString &altitude, const QString &altitudeRef);
    QString getMagickLatitude(const QString &latitude, const QString &latitudeRef);
    QString getMagickLongitude(const QString &longitude, const QString &longitudeRef);
    QString gpsMagickAltitude(const QString &altitude, const QString &altitudeRef);
    const QString metadataAttributeTitlePrefix = "meta";
protected:
    QImage loadResizedImg(const QString &fileName, int size);
    QImage loadResizedImg(int size);
    bool doResizeImg(int size, const QString &type);
    bool doResizeVideo(int size, const QString &type, int fromSecond, bool deleteTmp, bool coverRefresh);
    bool makeImage(const QString& imgFile, int imgWidth, int imgHeight);
    bool makeCoverImage();
    bool makePosterImage();
    bool makeMimeImage();
    bool makeMimeImage(int imgWidth, int imgHeight);
    QImage getMimeImage(int size);
    bool makeCss();

    void setDeleting(bool deleting);
    // CloudResource interface
    bool preventDelete() const;
public:
    int copyCount() { return m_copyesCount;}
	int childCount();
	int cacheSize();

	bool removeAllChildren();

    // CloudResource interface
    void LoadData(QHash<QString, QVariant> resourceData);
    void LoadEntryData(QHash<QString, QHash<QString, QVariant> > entryList);
    bool isCopy() const;
    qint64 pos() const;
    BackendType getCurrentBackendType() const;
    QByteArray getMemBackendData() const;

    LocalResource *getParentResource_ptr() const;
    QString getQuery() const;
    QString getMemBackendContentType() const;
	bool isImage() const;
    bool isAudio() const;
    bool isVideo() const;
    bool isVector() const;
    // WP 3.2 - Multiformat text processing
    bool isPdf() const;
    bool isImagePng() const;
    bool isMetaDocs() const;
    bool hasIcon() const;
	void addSymbolicLink(QString name, QString path);
    void refreshData() { doRefresh(); }
    QString localFilePath() {return  m_FileBackend.getFullPath(); }
#ifdef USE_MEM_POOL
	void * operator new(size_t size);
	void operator delete(void * p);
#endif
    static bool generateVidoCovers;
};



#endif // LOCALFILE_H
