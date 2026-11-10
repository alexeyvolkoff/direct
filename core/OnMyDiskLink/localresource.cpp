#include "localresource.h"
#include <QImage>
#include <QPainter>
#include <QFile>
#include <QStorageInfo>
#include <QFileInfo>
#include <QImageReader>
#include <QUrlQuery>
#include <QCache>
#include <QMutex>
#include "onmydisklinkclient.h"
#include "localdb.h"
#if  (SERVER || ROUTER || EMBEDDED) && !defined(P2P_ENABLED) && !defined(NO_WEB_UI)
#include "template.h"
#include "templateparser.h"
#endif
#include "onmydisklink_global.h"

#ifdef USE_MEM_POOL
#include "pool.hpp"
#endif
#ifdef TAGLIB_SUPPORTED
#include <taglib/fileref.h>
#include <taglib/tag.h>
#include <taglib/mpegfile.h>
#include <taglib/mp4tag.h>
#include <taglib/mp4file.h>
#include <taglib/id3v2frame.h>
#include <taglib/id3v2tag.h>
#include <taglib/attachedpictureframe.h>
#include <taglib/flacfile.h>
#include <taglib/flacpicture.h>
#endif
#include <QProcess>

bool LocalResource::generateVidoCovers = false;

LocalResource::LocalResource(QString filePath, QString remotePath, QString itemName, QString query, bool preventdDelete, LocalResource *parentResource) :
    CloudResource(), m_remotePath(remotePath),
    m_FileBackend(filePath, m_remotePath), m_MemBackend()
{
    m_resourceID = QUuid::createUuid().toString();    
    m_storageID = m_FileBackend.storageID();
    m_itemName = itemName;
    m_copyesCount = 0;        
	m_originalResource_ptr =  nullptr;
	m_parentResource_ptr = parentResource;
    m_isCopy = false;
    m_isActual = false;
    m_preventDelete = preventdDelete;
    m_isFile = false;
    m_isDir = false;
    m_isExecutable = false;
    m_isReadable = false;
    m_isWritable = false;
    m_isExecutable = false;
    m_deleting = false;
    m_exists = false;
	m_isWriting = false;

    m_lastModified = QDateTime::currentDateTime();
    m_lastRead = QDateTime::fromMSecsSinceEpoch(0);
    m_created = QDateTime::currentDateTime();
    m_link = "";

    if (m_parentResource_ptr)
        m_parentExists = m_parentResource_ptr->parentExists();
    else
        m_parentExists = false;

    m_currentBackendType = FileBackend;
    m_currentBackend = &m_FileBackend;
    m_query = query;
    m_link = "";

	//Update only this item attributes
	m_FileBackend.RefreshInfo(this);
    updateUrl();

	m_ETag = generateETag(true, m_FileBackend.getFullPath() + m_query, m_lastModified.toString(), this->size());

    if (! m_query.isEmpty() )
    {
        doRefresh();
        processQuery();
		m_isCopy = true;
    }
}


LocalResource::LocalResource(LocalResource *resource, QString query):
    CloudResource(),m_remotePath(resource->getRemotePath()),
    m_FileBackend(resource->getLocalPath(), resource->getRemotePath()),
    m_MemBackend(resource->getMemBackendData(), resource->getMemBackendContentType())
{
    m_resourceID = resource->GetID() + QString("_copy_%1").arg(query);
    m_storageID = resource->storageID();
    m_itemName = resource->m_itemName;
    m_copyesCount = 0;
    m_originalResource_ptr = nullptr;
    m_parentResource_ptr = nullptr;
    m_query = query;
    m_isCopy = true;
    m_preventDelete = false;
    m_isFile = resource->isFile();
    m_isDir =  resource->isDir();
    m_isExecutable = resource->isExecutable();
    m_isReadable = resource->isReadable();
    m_exists = resource->exists();
    m_deleting = false;
	m_isWriting = false;

    m_lastModified = QDateTime::currentDateTime();
    m_lastRead = QDateTime::fromMSecsSinceEpoch(0);
    m_created = QDateTime::currentDateTime();

    m_link = "";
    m_parentExists = false;
    m_currentBackendType = resource->m_currentBackendType;
    switch (m_currentBackendType) {
    case CloudResource::RemoteLinkBackend:
        m_currentBackend = &m_FileBackend;
        break;
    case CloudResource::FileBackend:
        m_currentBackend = &m_FileBackend;
        break;
    case CloudResource::MemBackend:
        m_currentBackend = &m_MemBackend;
        break;
    default:
        break;
    }

	// Copy children
	Q_FOREACH(QString childName, resource->m_childResources.keys()) {
		LocalResource *orig_child = resource->m_childResources[childName];
        LocalResource *child = new LocalResource(orig_child);
		m_childResources.insert(childName, child);
	}

     //qInfo() << "Clone  creating" << m_remotePath;
    if (! m_query.isEmpty() ) {
        doRefresh();
        processQuery();
    } else
        LoadData(resource->getData());
    m_isActual = false;
}

LocalResource::~LocalResource()
{
    m_deleting = true;
	removeAllChildren();
}

QString LocalResource::ETag() const
{
    return m_ETag;
}

QString LocalResource::lastModifiedString() const
{
    return QLocale::c().toString(m_lastModified.toUTC(),"ddd, dd MMM yyyy hh:mm:ss") + " GMT";
}

QDateTime LocalResource::LastModifiedDayTime() const
{
    return m_lastModified.toUTC();
}


void LocalResource::doRefreshAllInfo(bool forceRefresh)
{

    if (! forceRefresh && m_isActual && (m_currentBackendType == CloudResource::FileBackend))
    {
        m_isActual = QFileInfo(m_FileBackend.getFullPath()).lastModified() == m_lastModified;
    }

	if (!m_isActual || forceRefresh || !m_exists)
    {
        //TimeLogger logger("Refreshing " + fileName());

        if (m_currentBackendType == CloudResource::MemBackend)
        {
            m_FileBackend.RefreshInfo(this);
            m_MemBackend.RefreshInfo(this);
        }
        else
        {
            m_FileBackend.RefreshAllInfo(this);
        }

		m_ETag = generateETag(true, m_FileBackend.getFullPath() + m_query, m_lastModified.toString(), this->size());
        m_isActual = true;
    }
}

void LocalResource::doRefresh()
{
	if (m_currentBackendType == CloudResource::MemBackend)
	{
		m_FileBackend.RefreshInfo(this);
		m_MemBackend.RefreshInfo(this);
	}
	else
	{
		m_FileBackend.RefreshInfo(this);
	}

	m_ETag = generateETag(true, m_FileBackend.getFullPath() + m_query, m_lastModified.toString(), this->size());
    m_isActual = true;
}

void LocalResource::doRefreshAttributes()
{
    if (m_currentBackendType == CloudResource::FileBackend)
    {
        m_FileBackend.RefreshAttributes(this);
    }
    m_ETag = generateETag(true, m_FileBackend.getFullPath() + m_query, m_lastModified.toString(), this->size());
}

bool LocalResource::doRemoveChild(QString childName)
{
    if (m_childResources.contains(childName))
    {
        LocalResource* toRemove = m_childResources.take(childName);

        if (toRemove)
        {
          toRemove->setDeleting(true);
          toRemove->removeAllChildren();
          delete toRemove;
        }

        m_childResources.remove(childName);
    }

    return true;
}

void LocalResource::setDeleting(bool deleting)
{
    m_deleting = deleting;
    if (deleting) {
        m_originalResource_ptr = nullptr;
        m_parentResource_ptr = nullptr;
    }
}

LocalResource* LocalResource::getParent()
{
    LocalResource* parent = m_parentResource_ptr;
	 while (parent && !parent->isDir())
		 parent = parent->m_parentResource_ptr;
	 return parent;
}

void LocalResource::SetParentOutdated()
{
    if (m_parentResource_ptr)
        m_parentResource_ptr->setOutdated();
}

void LocalResource::updateUrl()
{
    if (! m_remotePath.startsWith("/"))
    {
        m_remotePath.prepend("/");
    }

    if (this->isDir() && !m_remotePath.endsWith("/"))
    {
        m_remotePath.append("/");
    }

    if (this->isFile() && m_remotePath.endsWith("/"))
    {
        m_remotePath.chop(1);
    }
}

QDateTime LocalResource::lastRead() const
{
    return m_lastRead;
}


int LocalResource::nLink() const
{
    return m_nLinkValue;
}

bool LocalResource::removeAllChildren()
{
	Q_FOREACH(LocalResource* toRemove, m_childResources)
	{
        toRemove->setDeleting(true);
		toRemove->removeAllChildren();
		delete toRemove;
	}

	m_childResources.clear();
    //m_childResources.squeeze();
	return true;
}

QString LocalResource::GetID() const
{
    return m_resourceID;
}

bool LocalResource::removeChild(QString childName)
{
    return doRemoveChild(childName);
}

bool LocalResource::isOpen() const
{
    return m_currentBackend->isOpen();
}

/*
qint64 LocalResource::pos() const
{
    return m_currentBackend->pos();
}
*/

qint64 LocalResource::size() const
{
    return m_currentBackend->size();
}

bool LocalResource::seek(qint64 pos)
{
    return m_currentBackend->seek(pos);
}

bool LocalResource::atEnd() const
{
    return m_currentBackend->atEnd();
}

void LocalResource::close()
{
    m_currentBackend->close();
	if (m_isWriting)
	{
		m_isWriting = false;
        m_isReadable = true;
        m_isWritable = true;
	}
}

QByteArray LocalResource::read(qint64 maxlen)
{
    return m_currentBackend->read(maxlen);
}

QByteArray LocalResource::read(qint64 pos, qint64 maxlen)
{

    if (m_currentBackend->seek(pos))
    {
        return m_currentBackend->readFromPosition(pos, maxlen);
    }
    else
        return QByteArray();
}

qint64 LocalResource::write(const QByteArray &data)
{
    return m_FileBackend.write(data.constData(), data.size());
}


bool LocalResource::isDir() const
{
    return m_isDir;
}

bool LocalResource::isFile() const
{
    return m_isFile;
}

QStringList LocalResource::entryList()
{
    return m_childResources.keys();
}


void LocalResource::refresh()
{
    doRefreshAllInfo(true);
}

CloudResource* LocalResource::getChildClone(QStringList childPath, QString query, bool withChildren)
{
    if (m_originalResource_ptr)
        return m_originalResource_ptr->getChildClone(childPath, query, withChildren);
    else
        return doGetChildClone(childPath, query, withChildren);
}

QString LocalResource::getPublicLink()
{    
    return m_link;
}

QString LocalResource::getPublicLink(QHash<QString, QVariant> linkData)
{
    if (LocalDB::instance().dbConnected())
    {
       m_linkData = linkData;
       m_linkData["title"] = getCustomAttributes()["title"].toString();
       m_link = LocalDB::instance().addPublicLinkForFile(getRemotePath(), getLocalPath(), m_linkData);
    }

    if (m_originalResource_ptr) {
        m_originalResource_ptr->m_linkData = m_linkData;
        m_originalResource_ptr->m_link = m_link;
    }

    linkData = m_linkData;
    return m_link;
}

void LocalResource::removePublicLink()
{
    if (LocalDB::instance().dbConnected())
    {
         LocalDB::instance().removePublicLinkForFile( getRemotePath());
    }
    m_link = "";
    if (m_originalResource_ptr) {
        m_originalResource_ptr->m_link = "";
    }

}

CloudResource *LocalResource::getChild(QStringList childPath, bool create)
{
    return doGetChild(childPath, create);
}

CloudResource *LocalResource::getChild(const QString &childName, bool create)
{

    if ( m_childResources.contains(childName)) {
        LocalResource* child = m_childResources[childName];
        if (create)
            child->doRefreshAllInfo(true);
         return child;
    } else {
            if (create)
            {
                QString childFilePath = m_FileBackend.childFilePath(childName);
                QString childRemotePath = m_remotePath.endsWith("/") ? m_remotePath + childName : m_remotePath + "/" + childName ;
                LocalResource* child = new LocalResource(childFilePath, childRemotePath, childName, QStringLiteral(""), false, this);
                child->doRefreshAllInfo(true);
                m_childResources.insert(childName, child);
                return child;
            } else
                return nullptr;
    }
}


CloudResource* LocalResource::doGetChildClone(QStringList childPath, QString query, bool withChildren)
{
    // Own clone
    if (childPath.count() == 0) {
        LocalResource* childResource = new LocalResource(this, query);
        childResource->m_isCopy = true;
        childResource->m_originalResource_ptr = this;
        childResource->m_preventDelete = false;
        if (! childResource->m_isActual || withChildren) {
          if (withChildren)
            childResource->doRefreshAllInfo(true);
          else
            childResource->doRefresh();
        }
        return childResource;
    }

    // Check existing child
    QString childName = childPath.first();
    if (m_childResources.contains(childName) ) {
        childPath.removeFirst();
        return m_childResources[childName]->doGetChildClone(childPath, query, withChildren);
    }

    // Wish me luck!
    bool final = childPath.count() == 1;

    QString childFilePath = m_FileBackend.childFilePath(childName);
    QString childRemotePath = m_remotePath.endsWith("/") ? m_remotePath + childName : m_remotePath + "/" + childName;

    LocalResource* childResource = new LocalResource(childFilePath, childRemotePath, childName, final ? query : "", false, this);
    childResource->m_isCopy = true;
    childResource->m_preventDelete = false;
    if (! childResource->m_isActual || withChildren) {
        if (withChildren || final)
            childResource->doRefreshAllInfo(true);
        else
            childResource->doRefresh();
    }

    if (!final)
        m_childResources[childName] = childResource;

    if (!final) {
        childPath.removeFirst();
        return childResource->doGetChildClone(childPath, query, withChildren);
    } else {
        return childResource;
    }

    //return childResource;
}

CloudResource *LocalResource::doGetChild(QStringList childPath, bool create)
{
    if (childPath.size() > 0)
    {
        QString childName = childPath.takeFirst();

        if ( m_childResources.keys().contains(childName)) {
            LocalResource* child  = m_childResources[childName];
            return child->doGetChild(childPath, create);
        } else
        {
            if (create)
            {
                QString childFilePath = m_FileBackend.childFilePath(childName);
                QString childRemotePath = m_remotePath + childName;
                LocalResource* child = new LocalResource(childFilePath, childRemotePath, childName, QStringLiteral(""), false, this);
                m_childResources.insert(childName, child);
                return child->doGetChild(childPath, create);
            } else
                return nullptr;
        }
    } else {
        if (!m_exists || !m_isActual || create) {
            doRefreshAllInfo(true);
        }
        return this;
    }
}

QString LocalResource::getQuery() const
{
    return m_query;
}

bool LocalResource::processQuery()
{
    QStringList queryPairs = m_query.split('&', Qt::SplitBehaviorFlags::SkipEmptyParts);
    QHash<QString, QString> parsedQuery;
    Q_FOREACH(QString queryPair, queryPairs)
    {
        QStringList keyValue = queryPair.split('=', Qt::SplitBehaviorFlags::SkipEmptyParts);
        if (keyValue.size() == 2)
            parsedQuery.insert(keyValue.at(0), keyValue.at(1));
        else
            parsedQuery.insert(keyValue.at(0), "true");
    }

	if (isImage() && parsedQuery.contains("resize"))
    {
        if (LocalFileBackend::isFileWriting(m_FileBackend.getFullPath()))
        {
            qWarning() << "LocalResource::processQuery: file is open for writing, skipping thumbnail generation:" << m_FileBackend.getFullPath();
            return false;
        }
        bool isOk = false;
        int size = parsedQuery.value("width", parsedQuery.value("height", parsedQuery.value("size", "240"))).toInt(&isOk);
        if (!isOk || size <= 0) size = 240;
        if (doResizeImg(size, "jpg"))
        {
             m_currentBackend = &m_MemBackend;
             m_currentBackendType = MemBackend;                 
        }
    }

    if (LocalResource::generateVidoCovers && isVideo() && parsedQuery.contains("getVideoDuration"))
    {
        addVideoDurationToAttributes();
    }

    if (LocalResource::generateVidoCovers && isVideo() && parsedQuery.contains("getVideoDimensions"))
    {
        addVideoDimensionsToAttributes();
    }

    if (LocalResource::generateVidoCovers && isVideo() && parsedQuery.contains("transcodeVideo"))
    {
        transcodeVideo();
    }

    if (parsedQuery.contains("excludeFromIndexing"))
    {
        bool excludeFromIndexing = QVariant(parsedQuery.value("excludeFromIndexing").trimmed()).toBool();
        updateExcludeFromIndexing(excludeFromIndexing);
    }

    if (LocalResource::generateVidoCovers && isVideo() && parsedQuery.contains("downscaleVideo"))
    {
        QString downscale = parsedQuery.value("downscale").trimmed();
        QString downscaleWidth = parsedQuery.value("width").trimmed();
        QString downscaleHeight = parsedQuery.value("height").trimmed();
        downscaleVideo(downscale, downscaleWidth, downscaleHeight);
    }

    if (LocalResource::generateVidoCovers && isVideo() && parsedQuery.contains("continueTranscodeVideo"))
    {
        continueTranscodeVideo();
    }

    if (LocalResource::generateVidoCovers && isVideo() && parsedQuery.contains("transcodeAbort"))
    {
        // bool isOk = false;
        QString transcodePid = parsedQuery.value("transcodePid").trimmed();
        QString mode = parsedQuery.value("mode").trimmed();  // transcode|downscale
        QString downscaleWidth = parsedQuery.value("width").trimmed();
        QString downscaleHeight = parsedQuery.value("height").trimmed();
        if (transcodePid != "")
            transcodeAbort(transcodePid, mode, downscaleWidth, downscaleHeight);
    }

    if (LocalResource::generateVidoCovers && isVideo() && parsedQuery.contains("processVideo"))
    {
        bool isOk = false;
        // qDebug() << "LocalResource::processQuery parsedQuery: " << parsedQuery;
        int size = parsedQuery.value("imageSize").toInt(&isOk);
        int fromSecond = parsedQuery.value("fromSecond").toInt(&isOk);
        QString processVideo = parsedQuery.value("processVideo");
        if (isOk)
        {
            if (processVideo == "getImage") {
                if (doResizeVideo(size, "jpg", fromSecond, true, false)) {
                    m_currentBackend = &m_MemBackend;
                    m_currentBackendType = MemBackend;
                    return true;
                }
            }
            if (processVideo == "getCover") {
                if (doResizeVideo(size, "jpg", fromSecond, false, true)) {
                    m_currentBackend = &m_MemBackend;
                    m_currentBackendType = MemBackend;
                    return true;
                }
            }
        }
    }

    if (parsedQuery.contains("getIcon"))
    {
        bool isOk = false;
        int size = parsedQuery.value("getIcon").toInt(&isOk);
		//bool openDir = parsedQuery.contains("folder-open");
        if (isOk)
        {
           if (hasIcon() || m_customAttributes["cover"].toBool())
           {

            #ifdef Q_OS_MAC
            QString iconType = "jpg";
            #else
            QString iconType = "png";
            #endif

            if (doResizeImg(size, iconType))
			{
				m_currentBackend = &m_MemBackend;
				m_currentBackendType = MemBackend;
				return true;
            } else
				qWarning() << "Could not resize" << fileName();

			//qInfo() << "Load icon" << fileName();
           }
        }
    }

	//parse theme color scheme
	if (parsedQuery.contains("css"))
	{
		if (makeCss())
		{
			m_currentBackend = &m_MemBackend;
			m_currentBackendType = MemBackend;
			return true;
		}
	}

    // WP 3.2 - Multiformat text processing
    if (parsedQuery.contains("indexing"))
    {
        if (isImage() && !isImagePng()) {
            addImageMetadataToAttributes();
        } else if (isImagePng()) {
            addImagePngMetadataToAttributes();
        }
        if (isPdf()) {
            addPdfMetadataToAttributes();
        }
        if (isMetaDocs()) {
            addMetaDocsMetadataToAttributes();
        }
        if (isVideo()) {
            addVideoMetadataToAttributes();
        }
    }


    //qInfo() << "No resized image" << m_FileBackend.getFullPath() << parsedQuery;
    return parseMetaTags();
}


/* parse OG meta tags
 */
bool LocalResource::parseMetaTags()
{
    QUrlQuery q(m_query);

    if (!m_exists || !m_isActual)
        doRefreshAttributes();

    if (!m_exists)
        return false;

    if (q.hasQueryItem("cover"))
    {
        //Cover is square image
        if (m_MemBackend.open(QIODevice::WriteOnly))
        {
            //Cover image assigned?
            if (m_customAttributes["cover"].toBool() && makeCoverImage())
            {
                m_currentBackend = &m_MemBackend;
                m_currentBackendType = MemBackend;
            }
            else if (hasIcon() && makeCoverImage())
            {
                m_currentBackend = &m_MemBackend;
                m_currentBackendType = MemBackend;
            }
            //generate cover from mime icon
            else if (makeMimeImage())
            {
                m_currentBackend = &m_MemBackend;
                m_currentBackendType = MemBackend;
            }
            else
            {
                // CRITICAL FIX: never let m_currentBackend point to raw media file!
                QImage blank(OG_IMG_WIDTH_MIN, OG_IMG_WIDTH_MIN, QImage::Format_ARGB32);
                blank.fill(Qt::transparent);
                blank.save(&m_MemBackend, "png", 90);
                m_MemBackend.setContentType(QString("image/png"));
                m_MemBackend.RefreshInfo(this);
                m_contentType = QString("image/png");
                m_currentBackend = &m_MemBackend;
                m_currentBackendType = MemBackend;
            }
        }
    }
    else if (q.hasQueryItem("poster") || q.hasQueryItem("preview"))
    {
        //Poster is 1200x674 image
        if (m_MemBackend.open(QIODevice::WriteOnly))
        {
            //Cover image assigned?
            if (m_customAttributes["cover"].toBool() && makePosterImage())
            {
                m_currentBackend = &m_MemBackend;
                m_currentBackendType = MemBackend;
            }
            else if (isImage() && makePosterImage())
            {
                m_currentBackend = &m_MemBackend;
                m_currentBackendType = MemBackend;
            }
            //generate cover from mime icon
            else if (makeMimeImage())
            {
                m_currentBackend = &m_MemBackend;
                m_currentBackendType = MemBackend;
            }
            else
            {
                QImage blank(OG_IMG_WIDTH_MIN, OG_IMG_HEIGHT_MIN, QImage::Format_ARGB32);
                blank.fill(Qt::transparent);
                blank.save(&m_MemBackend, "png", 90);
                m_MemBackend.setContentType(QString("image/png"));
                m_MemBackend.RefreshInfo(this);
                m_contentType = QString("image/png");
                m_currentBackend = &m_MemBackend;
                m_currentBackendType = MemBackend;
            }
        }
    }
    else if (q.hasQueryItem("readme"))
    {
        if (m_MemBackend.open(QIODevice::WriteOnly))
        {
            QFile readmeFile(m_FileBackend.readMeFileName());
            QByteArray data;
            if (readmeFile.open(QFile::ReadOnly)) {
                data = readmeFile.readAll();
                readmeFile.close();
            } else
                data = m_customAttributes["description"].toByteArray();

             m_MemBackend.write(data);
             m_MemBackend.setContentType("text/plain; charset=UTF-8");
             m_MemBackend.RefreshInfo(this);
             m_MemBackend.close();
             m_contentType = QString("text/plain; charset=UTF-8");
             m_ETag = generateETag(true, m_FileBackend.getFullPath() + m_query, m_lastModified.toString(), m_MemBackend.size());
             m_currentBackend = &m_MemBackend;
             m_currentBackendType = MemBackend;
        }
    }
    else if (q.hasQueryItem("description"))
    {
        if (m_MemBackend.open(QIODevice::WriteOnly))
        {
            QByteArray data = m_customAttributes["description"].toByteArray();
            m_MemBackend.write(data);
            m_MemBackend.setContentType(QString("text/plain; charset=UTF-8"));
            m_MemBackend.RefreshInfo(this);
            m_MemBackend.close();
            m_contentType = QString("text/plain; charset=UTF-8");
            m_ETag = generateETag(true, m_FileBackend.getFullPath() + m_query, m_lastModified.toString(), m_MemBackend.size());
            m_currentBackend = &m_MemBackend;
            m_currentBackendType = MemBackend;
        }
    }
    else if (q.hasQueryItem("title"))
    {
        if (m_MemBackend.open(QIODevice::WriteOnly))
        {            
            QByteArray data = m_customAttributes["title"].toByteArray();
            m_MemBackend.write(data);
            m_MemBackend.setContentType(QString("text/plain; charset=UTF-8"));
            m_MemBackend.RefreshInfo(this);
            m_MemBackend.close();
            m_contentType = QString("text/plain; charset=UTF-8");
            m_ETag = generateETag(true, m_FileBackend.getFullPath() + m_query, m_lastModified.toString(), m_MemBackend.size());
            m_currentBackend = &m_MemBackend;
            m_currentBackendType = MemBackend;
        }
    }
    else if (q.hasQueryItem("preview"))
    {
        //preview is poster, resized omage or mime icon
        if (m_customAttributes["cover"].toBool() && makePosterImage())
        {
            m_currentBackend = &m_MemBackend;
            m_currentBackendType = MemBackend;
        }
        else if (isImage() && makePosterImage())
        {
            m_currentBackend = &m_MemBackend;
            m_currentBackendType = MemBackend;
        }
        else if (makeMimeImage())
        {
            m_currentBackend = &m_MemBackend;
            m_currentBackendType = MemBackend;
        }
    }

    return (m_currentBackendType == MemBackend);
}

bool LocalResource::parseTemplate(const QString& contentType)
{
#if (SERVER || EMBEDDED || ROUTER) && !defined(P2P_ENABLED) && !defined(NO_WEB_UI)
  QFile htmlFile(m_FileBackend.getFullPath());
  if (htmlFile.open(QFile::ReadOnly))
  {

    if (m_MemBackend.open(QIODevice::WriteOnly))
    {
        QString session = sessionId();

        QString source = QString::fromUtf8(htmlFile.readAll());
        Template html_template(source, m_FileBackend.getFullPath());

        //qDebug() << "Parsing template " << m_FileBackend.getFullPath();
        //qDebug() << "Session " << sessionId();
        QByteArray parsedHtml = TemplateParser::instance().parse(session, html_template);
        //m_MemBackend.setContentType("text/html; charset=UTF-8");
        m_MemBackend.setContentType(contentType);
        m_MemBackend.write(parsedHtml);
        m_MemBackend.RefreshInfo(this);
        m_MemBackend.close();
        m_contentType = contentType;
        m_ETag = generateETag(true, m_FileBackend.getFullPath() + m_query, m_lastModified.toString(), m_MemBackend.size());
        m_currentBackend = &m_MemBackend;
        m_currentBackendType = MemBackend;
    }
    htmlFile.close();
  }
  return (m_currentBackendType = MemBackend);
#else
  Q_UNUSED(contentType)
  return false;
#endif
}

LocalResource::BackendType LocalResource::getCurrentBackendType() const
{
    return m_currentBackendType;
}

QByteArray LocalResource::getMemBackendData() const
{
    return m_MemBackend.getBytes();
}

QString LocalResource::getMemBackendContentType() const
{
	return m_MemBackend.getContentType();
}

bool LocalResource::isImage() const
{
    return m_FileBackend.isImage();
}

bool LocalResource::isAudio() const
{
    return m_FileBackend.isAudio();
}

bool LocalResource::isVideo() const
{
    return m_FileBackend.isVideo();
}

bool LocalResource::isVector() const
{
    return m_FileBackend.isVector();
}

// WP 3.2 - Multiformat text processing
bool LocalResource::isPdf() const
{
    return m_FileBackend.isPdf();
}

bool LocalResource::isImagePng() const
{
    return m_FileBackend.isImagePng();
}

bool LocalResource::isMetaDocs() const
{
    return m_FileBackend.isMetaDocs();
}

bool LocalResource::hasIcon() const
{
    return m_FileBackend.isImage() || m_FileBackend.isAudio() || m_FileBackend.isVector() || (LocalResource::generateVidoCovers && m_FileBackend.isVideo());
}

void LocalResource::addSymbolicLink(QString name, QString filePath)
{
	if (! m_childResources.contains(name)) {
        LocalResource * linkItem = new LocalResource(filePath, QString("/%1").arg(name), name, "", true, this);
        linkItem->doRefresh();
		m_childResources.insert(name, linkItem);
    }
}

QDateTime LocalResource::created() const
{
    return m_created;
}

QDateTime LocalResource::lastModified() const
{
    return m_lastModified;
}

QString LocalResource::fileName() const
{
    return m_itemName;
}

QString LocalResource::path() const
{
    return getRemotePath();
}

bool LocalResource::isExecutable() const
{
    return m_isExecutable;
}

QString LocalResource::getContentType() const
{
    return m_contentType;
}

QString LocalResource::getLocalPath() const
{
    return m_FileBackend.getFullPath();
}

QString LocalResource::getRemotePath() const
{
    return  m_remotePath;
}

bool LocalResource::preventDelete() const
{
    return m_preventDelete;
}

void LocalResource::LoadData(QHash<QString, QVariant> resourceData)
{
    if (resourceData.contains("lastModDate"))
        m_lastModified = resourceData.value("lastModDate").toDateTime();
    if (resourceData.contains("lastReadDate"))
        m_lastRead = resourceData.value("lastReadDate").toDateTime();
    if (resourceData.contains("createdDate"))
        m_created = resourceData.value("createdDate").toDateTime();
    if (resourceData.contains("contentType"))
        m_contentType = resourceData.value("contentType").toString();
    if (resourceData.contains("exists"))
        m_exists = resourceData.value("exists").toBool();

    if (resourceData.contains("parentExists"))
        m_parentExists = resourceData.value("parentExists").toBool();
    if (resourceData.contains("isReadable"))
        m_isReadable = resourceData.value("isReadable").toBool();
    if (resourceData.contains("isWritable"))
        m_isWritable = resourceData.value("isWritable").toBool();
    if (resourceData.contains("parentExists"))
        m_parentExists = resourceData.value("parentExists").toBool();
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
        m_nLinkValue = resourceData.value("nlink").toInt();
    if (resourceData.contains("storageID"))
        m_storageID = resourceData.value("storageID").toString();

    if (resourceData.contains("customAttributes")) {
        QVariantHash attrs = resourceData.value("customAttributes").toHash();
        foreach(const QString &key, attrs.keys())
            m_customAttributes[key] = attrs[key];
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

	//update fileinfo for just created file
    if (m_isFile && size() == 0)
    {
		QFileInfo(m_FileBackend.getFullPath()).refresh();
    }

    if (resourceData.contains("link"))
        m_link = resourceData.value("link").toString();

    if (resourceData.contains("linkData"))
        m_linkData = resourceData.value("linkData").toHash();

    if (!m_exists && m_parentResource_ptr)
        m_isWritable = m_parentResource_ptr->m_isWritable;

    m_ETag = generateETag(true, m_FileBackend.getFullPath() + this->getQuery(), m_lastModified.toString(), this->size());

    updateUrl();
}

void LocalResource::LoadEntryData(QHash<QString, QHash<QString, QVariant> > entryList)
{
    Q_FOREACH(QString childName, entryList.keys())
    {
        if (m_childResources.contains(childName))
        {
            m_childResources[childName]->LoadData(entryList[childName]);
        }
        else
        {
            QString childPath = m_FileBackend.childFilePath(childName);
            QString childRemotePath = m_remotePath  +childName;
            LocalResource* childResource = new LocalResource(childPath, childRemotePath, childName, QStringLiteral(""), false, this);//, entryList[childName], QHash<QString, QHash<QString, QVariant>>());
            childResource->LoadData(entryList[childName]);
            m_childResources.insert(childName, childResource);
        }
    }

    Q_FOREACH(QString childName, m_childResources.keys())
    {
		//do not remove "symbolic links"
		if (!m_childResources[childName]->preventDelete() && !entryList.keys().contains(childName))
            doRemoveChild(childName);
    }
}

bool LocalResource::isCopy() const
{
    return m_isCopy;
}

qint64 LocalResource::pos() const
{
    return m_currentBackend->pos();
}

QImage LocalResource::loadResizedImg(const QString &fileName, int size)
{
    //Calculate aspect ratios
    QImage resizedImg;
    QImageReader reader(fileName);
    QSize s = reader.size();
    QExifImageHeader header(fileName);
    QExifValue orientation = header.value(QExifImageHeader::Orientation);
    QTransform rotation;

    long orientationValue = orientation.toLong();

    if( orientationValue > 1 )
    {
        switch (orientationValue) {
        case 2:
            rotation.rotate(180, Qt::YAxis);
            break;
        case 3:
            rotation.rotate(180, Qt::ZAxis);
            break;
        case 4:
            rotation.rotate(180, Qt::XAxis);
            break;
        case 5:
            rotation.rotate(180, Qt::YAxis).rotate(90, Qt::ZAxis);
            s = QSize(s.height(), s.width());
            break;
        case 6:
            rotation.rotate(90, Qt::ZAxis);
            //s = QSize(s.height(), s.width());
            break;
        case 7:
            rotation.rotate(180, Qt::XAxis).rotate(90, Qt::ZAxis);
            s = QSize(s.height(), s.width());
            break;
        case 8:
            rotation.rotate(270, Qt::ZAxis);
           // s = QSize(s.height(), s.width());
            break;
        default:
            break;
        }
    }

    //if (size > s.width() || size > s.height()) return resizedImg;

    float rw =  s.width() < s.height() ? s.width() / (float) s.height() : 1;
    float rh =  s.height() < s.width() ? s.height() / (float) s.width() : 1;
    //Load scaled image
    reader.setScaledSize(QSize(size *rw, size *rh));
    resizedImg = reader.read();

    if (resizedImg.isNull()) {
        //qWarning() << "Cannot load image" << fileName;
        return resizedImg;
    }

    if( orientationValue > 1 )
    {
        resizedImg = resizedImg.transformed(rotation, Qt::FastTransformation);
    }
    return  resizedImg;
}

QImage LocalResource::loadResizedImg(int size)
{
    QString filePath = m_FileBackend.getFullPath();
    return loadResizedImg(filePath, size);
}

bool LocalResource::doResizeVideo(int size, const QString &type, int fromSecond, bool deleteTmp, bool coverRefresh)
{
    bool isSaved = false;
    bool isOpen = false;
    QImage resizedImg;

    if (size == OG_IMG_WIDTH)
        resizedImg = loadVideoArt(size, OG_IMG_HEIGHT, fromSecond, deleteTmp, coverRefresh);
    else
        resizedImg = loadVideoArt(size, size, fromSecond, deleteTmp, coverRefresh);

    if (resizedImg.isNull()) {
        m_customAttributes["cover"] = false;        
        if (m_exists)
            return makeMimeImage(size, size);
        else
            return false;
    }

    if (m_MemBackend.open(QIODevice::WriteOnly)) {
        isOpen = true;
        if (! resizedImg.save(&m_MemBackend, type.toUpper().toUtf8().data(), 90))
            qWarning() << "Can not save resized image to cache" << this->fileName();
    }

    if (m_MemBackend.size() > 0) //saved ok!
    {
        isSaved = true;
        m_MemBackend.setContentType(QString("image/%1").arg(type));
        m_MemBackend.RefreshInfo(this);
        m_ETag = generateETag(true, m_FileBackend.getFullPath() + m_query, m_lastModified.toString(), m_MemBackend.size());
    }
    // qInfo() << "Cache size:" << m_MemBackend.size();

    if (isOpen)
        m_MemBackend.close();

    // qDebug() << "Resizing" << fileName() << "done";
    return isSaved;
}

// Static in-memory thumbnail cache across RPC requests to avoid repeating CPU-heavy image scaling
static QCache<QString, QByteArray> s_thumbnailCache(200);
static QMutex s_thumbnailCacheMutex;

bool LocalResource::doResizeImg(int size, const QString& type)
{
    if (LocalFileBackend::isFileWriting(m_FileBackend.getFullPath()))
    {
        qWarning() << "LocalResource::doResizeImg: file is open for writing, skipping:" << m_FileBackend.getFullPath();
        return false;
    }

    QString cacheKey = m_FileBackend.getFullPath() + ":" + QString::number(size) + ":" + type + ":" + QString::number(m_lastModified.toMSecsSinceEpoch());
    {
        QMutexLocker locker(&s_thumbnailCacheMutex);
        QByteArray *cached = s_thumbnailCache.object(cacheKey);
        if (cached && !cached->isEmpty()) {
            if (m_MemBackend.open(QIODevice::WriteOnly)) {
                m_MemBackend.write(*cached);
                m_MemBackend.close();
                m_MemBackend.setContentType(QString("image/%1").arg(type));
                m_MemBackend.RefreshInfo(this);
                m_ETag = generateETag(true, m_FileBackend.getFullPath() + m_query, m_lastModified.toString(), m_MemBackend.size());
                m_contentType = QString("image/%1").arg(type);
                return true;
            }
        }
    }

    bool isSaved = false;
    bool isOpen = false;
    QImage resizedImg;
    // qInfo() << "Saving resized" << fileName() << "to cache as:" << type;
    if (LocalResource::generateVidoCovers && isVideo()) {
        resizedImg = loadVideoArt(size, size, OG_VIDEO_COVER_IMG_FROM_SEC, false, false);
    } else if (isAudio()) {
        resizedImg = loadAudioArt(size, size);
    } else {
        resizedImg = loadResizedImg(size);
    }

    if (resizedImg.isNull()) {
        m_customAttributes["cover"] = false;        
        if (m_exists && type == "png")
            return makeMimeImage(size, size);
        else
            return false;
    }
    if (m_MemBackend.open(QIODevice::WriteOnly)) {
        isOpen = true;
        if (! resizedImg.save(&m_MemBackend, type.toUpper().toUtf8().data(), 90))
            qWarning() << "Can not save resized image to cache" << this->fileName();
    }

    if (m_MemBackend.size() > 0) //saved ok!
    {
        isSaved = true;
        m_MemBackend.setContentType(QString("image/%1").arg(type));
        m_MemBackend.RefreshInfo(this);
        m_ETag = generateETag(true, m_FileBackend.getFullPath() + m_query, m_lastModified.toString(), m_MemBackend.size());
        m_contentType = QString("image/%1").arg(type);
        {
            QMutexLocker locker(&s_thumbnailCacheMutex);
            s_thumbnailCache.insert(cacheKey, new QByteArray(m_MemBackend.getBytes()));
        }
    }
    // qInfo() << "Cache size:" << m_MemBackend.size();

    if (isOpen)
        m_MemBackend.close();

    // qDebug() << "Resizing" << fileName() << "done";
    return isSaved;
}


bool LocalResource::makeCoverImage()
{
    QString filePath = hasIcon() ? m_FileBackend.getFullPath() : m_FileBackend.coverFileName() ;
    return makeImage(filePath, OG_IMG_WIDTH, OG_IMG_WIDTH);
}

bool LocalResource::makePosterImage()
{
    QString filePath = hasIcon() ? m_FileBackend.getFullPath() : m_FileBackend.coverFileName();
    if (isVideo()) {
        return makeImage(filePath, 1920, 1920);
    }
    return makeImage(filePath, OG_IMG_WIDTH, OG_IMG_HEIGHT);
}

QImage LocalResource::loadAudioArt(int imgWidth, int imgHeight)
{
    QImage resizedImg;
    #ifdef TAGLIB_SUPPORTED
    QString fileType = QFileInfo(m_FileBackend.getFullPath()).suffix().toLower();
    if (fileType == "mp3")
    {
        TagLib::String s(QDir::toNativeSeparators(m_FileBackend.getFullPath()).toUtf8().constData(), TagLib::String::UTF8);
        TagLib::FileName fp(s.toCString(true));
        TagLib::MPEG::File f(fp);
        if (f.hasID3v2Tag()) {
            // Look for picture frames only
            TagLib::ID3v2::FrameList listOfMp3Frames = f.ID3v2Tag()->frameListMap()["APIC"];
            if (!listOfMp3Frames.isEmpty()) {
                for (TagLib::ID3v2::FrameList::ConstIterator it = listOfMp3Frames.begin(); it != listOfMp3Frames.end() ; it++) {
                    TagLib::ID3v2::AttachedPictureFrame *pictureFrame = static_cast<TagLib::ID3v2::AttachedPictureFrame*>(*it);
                    if (pictureFrame && pictureFrame->picture().size() > 0) {
                        QImage original;
                        bool success = original.loadFromData((const uchar *)pictureFrame->picture().data(), pictureFrame->picture().size());
                        if (success) {
                            resizedImg = original.scaled(imgWidth, imgHeight, Qt::KeepAspectRatio, Qt::SmoothTransformation);
                            break;
                        }
                    }
                }
            }
        }
    } else if (fileType == "flac") {
        TagLib::String s(QDir::toNativeSeparators(m_FileBackend.getFullPath()).toUtf8().constData(), TagLib::String::UTF8);
        TagLib::FileName fp(s.toCString(true));
        TagLib::FLAC::File f(fp);
        if (!f.pictureList().isEmpty()) {
            TagLib::FLAC::Picture *pic = f.pictureList().front();
            if (pic && pic->data().size() > 0) {
                QImage original;
                if (original.loadFromData((const uchar *)pic->data().data(), pic->data().size())) {
                    resizedImg = original.scaled(imgWidth, imgHeight, Qt::KeepAspectRatio, Qt::SmoothTransformation);
                }
            }
        }
    } else {
        TagLib::String s(QDir::toNativeSeparators(m_FileBackend.getFullPath()).toUtf8().constData(), TagLib::String::UTF8);
        TagLib::FileName fp(s.toCString(true));
        TagLib::MP4::File f(fp);
        TagLib::MP4::Tag* tag = f.tag();
        if (tag) {
            TagLib::MP4::ItemMap itemsMap = tag->itemMap();
            TagLib::MP4::Item coverItem = itemsMap["covr"];
            TagLib::MP4::CoverArtList coverArtList = coverItem.toCoverArtList();
            if (!coverArtList.isEmpty()) {
                QImage original;
                TagLib::MP4::CoverArt coverArt = coverArtList.front();
                if (coverArt.data().size() > 0) {
                    bool success = original.loadFromData((const uchar *) coverArt.data().data(), coverArt.data().size());
                    if (success) {
                        resizedImg = original.scaled(imgWidth, imgHeight, Qt::KeepAspectRatio, Qt::SmoothTransformation);
                    }
                }
            }
        }
    }
    #else
    Q_UNUSED(imgWidth)
    Q_UNUSED(imgHeight)
    #endif

    // Fallback: check if there is an album cover image file in the directory or parent directory
    if (resizedImg.isNull()) {
        QString extCover = LocalFileBackend::audioCoverFile(QFileInfo(m_FileBackend.getFullPath()));
        if (!extCover.isEmpty() && QFileInfo::exists(extCover)) {
            QImage original(extCover);
            if (!original.isNull()) {
                resizedImg = original.scaled(imgWidth, imgHeight, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            }
        }
    }

    return resizedImg;
}

// WP 3.2 - Multiformat text processing
/*
 * QExifImageHeader doesn't support reading XMP (Extensible Metadata Platform) metadata directly.
 * To read XMP metadata from a JPEG file we will use Exiv2 library.
 * Free and compatible with all OSS
*/
void LocalResource::addImageMetadataToAttributes()
{
    QString imageFilePath = m_FileBackend.getFullPath();
    QString imagePath = QFileInfo(m_FileBackend.getFullPath()).path();
    QString imageName = QFileInfo(m_FileBackend.getFullPath()).baseName();
    QString metadataImageFile = QString("%1/.tmp/%2.metadata.log").arg(imagePath, imageName);
    QFileInfo checkMetaFile(metadataImageFile);
    bool metaExists = checkMetaFile.exists() && checkMetaFile.isFile();

    // Check if .tmp directory exists in video location, if not create .tmp directory, .tmp folder will be used to store log file
    if (!checkTmpFolder(imagePath))
        return;

    if (!metaExists)  {
        QProcess _EXIV2;
        QString program = "exiv2";
        QStringList arguments;

        // exiv2 -p a <image_file_path>
        arguments << "-p" << "a" << imageFilePath;
        // qDebug() << "arguments:  " << arguments;

        _EXIV2.setProgram(program);
        _EXIV2.setArguments(arguments);
        _EXIV2.setWorkingDirectory(imagePath);
        _EXIV2.setStandardOutputFile(metadataImageFile, QIODevice::Truncate);
        _EXIV2.start();

        if (_EXIV2.waitForFinished(-1)) {
            metaExists = checkMetaFile.exists() && checkMetaFile.isFile();
        } else {
           qDebug() << "Process failed to finish!";
        }
    }
    if (metaExists) {
       QFile metadataFile(metadataImageFile);
       if (!metadataFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
           qDebug() << "Failed to open file: " << metadataFile.errorString();
           // return 1;
       }

       QTextStream in(&metadataFile);
       QString gpsLatitude, gpsLatitudeRef = "";
       QString gpsLongitude, gpsLongitudeRef = "";
       QString gpsAltitude, gpsAltitudeRef = "";
       while (!in.atEnd()) {
           QString line = in.readLine();
           if (line.startsWith("Exif.Image.ImageDescription", Qt::CaseSensitive))
                m_customAttributes.insert("metaTitle", getExiv2ValueFromLine(line));
           if (line.startsWith("Exif.Image.Make", Qt::CaseSensitive))
                m_customAttributes.insert("metaMake", getExiv2ValueFromLine(line));
           if (line.startsWith("Exif.Image.Model", Qt::CaseSensitive))
                m_customAttributes.insert("metaModel", getExiv2ValueFromLine(line));
           if (line.startsWith("Exif.Image.DateTime", Qt::CaseSensitive))
                m_customAttributes.insert("metaCreated", getExiv2ValueFromLine(line));
           if (line.startsWith("Exif.Image.Artist", Qt::CaseSensitive))
                m_customAttributes.insert("metaAuthor", getExiv2ValueFromLine(line));
           if (line.startsWith("Exif.Image.Copyright", Qt::CaseSensitive))
                m_customAttributes.insert("metaCopyright", getExiv2ValueFromLine(line));
           if (line.startsWith("Exif.Image.XPComment", Qt::CaseSensitive))
                m_customAttributes.insert("metaComment", getExiv2ValueFromLine(line));
           if (line.startsWith("Exif.Image.XPKeywords", Qt::CaseSensitive))
                m_customAttributes.insert("metaAuthor", getExiv2ValueFromLine(line));
           if (line.startsWith("Exif.Image.XPSubject", Qt::CaseSensitive))
                m_customAttributes.insert("metaSubject", getExiv2ValueFromLine(line));
           if (line.startsWith("Exif.GPSInfo.GPSLatitude", Qt::CaseSensitive)) {
               if (line.startsWith("Exif.GPSInfo.GPSLatitudeRef"))
                   gpsLatitudeRef = getExiv2ValueFromLine(line);
               else
                   gpsLatitude = getExiv2ValueFromLine(line);
           }
           if (line.startsWith("Exif.GPSInfo.GPSLongitude", Qt::CaseSensitive)) {
               if (line.startsWith("Exif.GPSInfo.GPSLongitudeRef"))
                   gpsLongitudeRef = getExiv2ValueFromLine(line);
               else
                   gpsLongitude = getExiv2ValueFromLine(line);
           }
           if (line.startsWith("Exif.GPSInfo.GPSAltitude", Qt::CaseSensitive)) {
               if (line.startsWith("Exif.GPSInfo.GPSAltitudeRef"))
                   gpsAltitudeRef = getExiv2ValueFromLine(line);
               else
                   gpsAltitude = getExiv2ValueFromLine(line);
           }
       }
       metadataFile.close();
       // Now we should have GPS info
       if (gpsLatitude != "" && gpsLatitudeRef != "")
            m_customAttributes.insert("metaLatitude", getExiv2Latitude(gpsLatitude, gpsLatitudeRef));
       if (gpsLongitude != "" && gpsLongitudeRef != "")
            m_customAttributes.insert("metaLongitude", getExiv2Longitude(gpsLongitude, gpsLongitudeRef));
       if (gpsAltitude != "" && gpsAltitudeRef != "")
            m_customAttributes.insert("metaAltitude", gpsExiv2Altitude(gpsAltitude, gpsAltitudeRef));
    }
}

/*
 * We will use MagickImage tools to get metadata informations
 * CLI: identify (Linux)
 *      convert <source_image_file_path> <destination_image_file_path>
 * To get only EXIF data
 *      identify -format '%[EXIF:*]' <image_file_path>
 *      identify -format "%[*]" <image_file_path>
*/
void LocalResource::addImagePngMetadataToAttributes()
{
    QString imageFilePath = m_FileBackend.getFullPath();
    QString imagePath = QFileInfo(m_FileBackend.getFullPath()).path();
    QString imageName = QFileInfo(m_FileBackend.getFullPath()).baseName();
    QString metadataImageFile = QString("%1/.tmp/%2.metadata.log").arg(imagePath, imageName);
    QFileInfo checkMetaFile(metadataImageFile);
    bool metaExists = checkMetaFile.exists() && checkMetaFile.isFile();

    // Check if .tmp directory exists in video location, if not create .tmp directory, .tmp folder will be used to store log file
    if (!checkTmpFolder(imagePath))
        return;

    if (!metaExists)  {
        QProcess _IDENTIFY;
        QString program = "identify";
        QStringList arguments;

        // identify -verbose <image_file_path>
        // arguments << "-verbose" << imageFilePath;
        // identify -format '%[EXIF:*]' <image_file_path>
        // QString downscaleTo = QString("scale='iw/%1:ih/%2\'").arg(downscale, downscale);
        // arguments << "-format" << "%[EXIF:*]";
        // "%\\[EXIF:*\\]"
        // arguments << "-format" << "\\%[EXIF:*\\]" << imageFilePath;
        // we have problem with '%[EXIF:*]' as argument. square brackets!
        arguments << "-verbose" << imageFilePath;
        //qDebug() << "arguments:  " << arguments;

        _IDENTIFY.setProgram(program);
        _IDENTIFY.setArguments(arguments);
        _IDENTIFY.setWorkingDirectory(imagePath);
        _IDENTIFY.setStandardOutputFile(metadataImageFile, QIODevice::Truncate);
        _IDENTIFY.start();

        if (_IDENTIFY.waitForFinished(-1)) {
           //qDebug() << "Process finished successfully!";
           metaExists = checkMetaFile.exists() && checkMetaFile.isFile();
        } else {
           qDebug() << "Process failed to finish!";
        }
    }

    if (metaExists)  {
       QFile metadataFile(metadataImageFile);
       if (!metadataFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
           qDebug() << "Failed to open file: " << metadataFile.errorString();
           // return 1;
       }

       QTextStream in(&metadataFile);
       QStringList exifFields;
       exifFields << "Artist" << "Copyright" << "DateTime" << "ImageDescription" << "Make" << "MakerNote" << "Model" << "UserComment"
                  << "WinXP-Author" << "WinXP-Comments" << "WinXP-Keywords" << "WinXP-Subject" << "WinXP-Title";
       QString gpsAltitude, gpsAltitudeRef, gpsLatitude, gpsLatitudeRef, gpsLongitude, gpsLongitudeRef = "";
       //qDebug() << "Lets start reading";
       while (!in.atEnd()) {
           QString line = in.readLine().trimmed();
           // qDebug() << "line: " << line;
           foreach(const QString &exifField, exifFields) {
               QString checkField = "exif:" + exifField + ":";
               if (line.startsWith(checkField, Qt::CaseSensitive)) {
                   //qDebug() << "exif:line: " << line;
                   m_customAttributes.insert(metaToCustomAttributes(exifField), line.right(line.length() - checkField.length()).trimmed());
               }
           }
           //qDebug() << "Now check GPS";
           if (line.startsWith("exif:GPS", Qt::CaseSensitive)) {
               //qDebug() << "exifGPS:line: " << line;
               if (line.startsWith("exif:GPSAltitude:", Qt::CaseSensitive))
                   gpsAltitude = line.right(line.length() - QString("exif:GPSAltitude:").length()).trimmed();
               if (line.startsWith("exif:GPSAltitudeRef:", Qt::CaseSensitive))
                   gpsAltitudeRef = line.right(line.length() - QString("exif:GPSAltitudeRef:").length()).trimmed();
               if (line.startsWith("exif:GPSLatitude:", Qt::CaseSensitive))
                   gpsLatitude = line.right(line.length() - QString("exif:GPSLatitude:").length()).trimmed();
               if (line.startsWith("exif:GPSLatitudeRef:", Qt::CaseSensitive))
                   gpsLatitudeRef = line.right(line.length() - QString("exif:GPSLatitudeRef:").length()).trimmed();
               if (line.startsWith("exif:GPSLongitude:", Qt::CaseSensitive))
                   gpsLongitude = line.right(line.length() - QString("exif:GPSLongitude:").length()).trimmed();
               if (line.startsWith("exif:GPSLongitudeRef:", Qt::CaseSensitive))
                   gpsLongitudeRef = line.right(line.length() - QString("exif:GPSLongitudeRef:").length()).trimmed();
           }
       }
       // Now we should have GPS info
       if (gpsLatitude != "" && gpsLatitudeRef != "")
            m_customAttributes.insert(metaToCustomAttributes("Latitude"), getMagickLatitude(gpsLatitude, gpsLatitudeRef));
       if (gpsLongitude != "" && gpsLongitudeRef != "")
            m_customAttributes.insert(metaToCustomAttributes("Longitude"), getMagickLongitude(gpsLongitude, gpsLongitudeRef));
       if (gpsAltitude != "" && gpsAltitudeRef != "")
            m_customAttributes.insert(metaToCustomAttributes("Altitude"), gpsMagickAltitude(gpsAltitude, gpsAltitudeRef));

       metadataFile.close();
   }
}

/*
       exif:GPSLatitude=44/1, 22/1, 20647/2500
       exif:GPSLatitudeRef=N
*/
QString LocalResource::getMagickLatitude(const QString &latitude, const QString &latitudeRef)
{
    QString getValue = "";
    QStringList parseValues = latitude.split(",");
    //qDebug() << "getMagickLatitude: " << latitude << ", " << latitudeRef;

    if (parseValues.size() == 3) {
        bool ok1, ok2;

        QStringList latitudeValues = QString(parseValues[0]).split("/");
        if (latitudeValues[1] == "1")
            getValue += latitudeValues[0]+";";
        else {
            qDebug() << "getMagickLatitude.SRDJANP TODO: Latitude degrise should be devided";
        }

        latitudeValues = QString(parseValues[1]).trimmed().split("/");
        if (latitudeValues[1] == "1")
            getValue += latitudeValues[0]+";";
        else {
            qDebug() << "getMagickLatitude.SRDJANP1 TODO: Latitude minutes should be devided";
        }

        latitudeValues = QString(parseValues[2]).trimmed().split("/");
        double numerator = latitudeValues[0].toDouble(&ok1);
        double denominator = latitudeValues[1].toDouble(&ok2);
        if (ok1 && ok2) {
            if (denominator != 0.0) {
                getValue += QString::number(numerator / denominator);
            }
        }
        getValue += " " + latitudeRef;
    }
    return getValue;
}

/*
       exif:GPSLongitude=20/1, 57/1, 362861/10000
       exif:GPSLongitudeRef=E
*/
QString LocalResource::getMagickLongitude(const QString &longitude, const QString &longitudeRef)
{
    QString getValue = "";
    QStringList parseValues = longitude.split(",");
    //qDebug() << "getMagickLongitude: " << longitude << ", " << longitudeRef;

    if (parseValues.size() == 3) {
        bool ok1, ok2;

        QStringList longitudeValues = QString(parseValues[0]).split("/");
        if (longitudeValues[1] == "1")
            getValue += longitudeValues[0]+";";
        else {
            qDebug() << "getMagickLatitude.SRDJANP TODO: longitude degrise should be devided";
        }

        longitudeValues = QString(parseValues[1]).trimmed().split("/");
        if (longitudeValues[1] == "1")
            getValue += longitudeValues[0]+";";
        else {
            qDebug() << "getMagickLatitude.SRDJANP1 TODO: longitude minutes should be devided";
        }

        longitudeValues = QString(parseValues[2]).trimmed().split("/");
        double numerator = longitudeValues[0].toDouble(&ok1);
        double denominator = longitudeValues[1].toDouble(&ok2);
        if (ok1 && ok2) {
            if (denominator != 0.0) {
                getValue += QString::number(numerator / denominator);
            }
        }
        getValue += " " + longitudeRef;
    }
    return getValue;
}

/*
    exif:GPSAltitude: 167500/1000
    exif:GPSAltitudeRef: 0
*/
QString LocalResource::gpsMagickAltitude(const QString &altitude, const QString &altitudeRef)
{
    QString getValue = "";
    QStringList parseValues = QString(altitude).trimmed().split("/");
    //qDebug() << "gpsMagickAltitude: " << altitude << ", " << altitudeRef;
    bool ok1, ok2;
    int numerator = parseValues[0].toInt(&ok1);
    int denominator = parseValues[1].toInt(&ok2);
    if (ok1 && ok2) {
        if (denominator != 0) {
            getValue += QString::number(numerator / denominator);
        }
    }
    if (altitudeRef != "0")
        getValue = "-" + getValue;
    return getValue;
}

QString LocalResource::getExiv2ValueFromLine(const QString &line) {
    QString getValue = "";
    // Splitted by space we should expect:
    // 0 : ExifFieldName
    // 1 : Type
    // 2 : Size
    // 3+: Value which need spaces to be put back in the string if it is > 4
    // Qt::SplitBehavior::
    QStringList lineCols = line.split(QRegExp("\\s+"), Qt::SplitBehaviorFlags::SkipEmptyParts);
    if (lineCols.length() >= 4) {
       if (lineCols.length() == 4)
           getValue = lineCols[3];
       else {
           for(int i=3; i<lineCols.length(); i++) {
               if (getValue == "")
                   getValue = lineCols[i];
               else
                   getValue += " " + lineCols[i];
           }
       }
    }
    return getValue;
}

// 44deg 22' 8\" North      N/S +/-
// 44; 22; 8.258800000016566658
QString LocalResource::getExiv2Latitude(const QString &latitude, const QString &latitudeRef)
{
    QString getValue = "";
    // Exiv2::Rational gpsLatitude = exifData["Exif.GPSInfo.GPSLatitude"].toRational();
    getValue = latitude + " " + QString(latitudeRef).left(1);
    return getValue;
}

// 20deg 57' 36" East       E/W +/-
// 20; 57; 36.28609999999773899
QString LocalResource::getExiv2Longitude(const QString &longitude, const QString &longitudeRef)
{
    QString getValue = "";
    getValue = longitude + " " + longitudeRef.left(1).left(1);
    return getValue;
}

// Expected input: 167.5 m (Above sea level/Below sea level)
// Output: 167.5
QString LocalResource::gpsExiv2Altitude(const QString &altitude, const QString &altitudeRef)
{
    QString getValue = "";
    QString inputString = altitude;
    QRegExp regex("[^\\d.]"); // Match anything that is NOT a digit or a decimal point.
    getValue = inputString.remove(regex);
    if (altitudeRef.contains("Below"))
        getValue = "-" + getValue;
    return getValue;
}

/*  output
 *  ;FFMETADATA1
 *  parse: location location-eng title genre comment
*/
void LocalResource::addVideoMetadataToAttributes()
{
    QString videoPath = QFileInfo(m_FileBackend.getFullPath()).path();
    QString filePath = QFileInfo(m_FileBackend.getFullPath()).filePath();
    QString fileName = QFileInfo(m_FileBackend.getFullPath()).baseName();
    QString metadataVideoFile = QString("%1/.tmp/%2.metadata.log").arg(videoPath, fileName);
    QFileInfo checkMetaFile(metadataVideoFile);
    bool metaExists = checkMetaFile.exists() && checkMetaFile.isFile();

    QStringList parseMetadataTags;

    // Here we will define which tags we'll read ffroom metadata file
    parseMetadataTags << "location" << "location-eng" << "title" << "genre" << "comment";

    // Check if .tmp directory exists in video location, if not create .tmp directory, .tmp folder will be used to store log file
    if (!checkTmpFolder(videoPath))
        return;

    if (!metaExists)  {
        QProcess _FFMPEG;

        QString program = "ffmpeg";
        QStringList arguments;

        // ffmpeg -i <input_video_file> -f ffmetadata <metadata_output.txt>
        // Important: setStandardErrorFile will write complete ffmpeg output, hard to parse
        // -y : Overwrite existing metadata
        // metadataVideoFile : Output file as one of the arguments will return only metadata
        arguments << "-i" << filePath << "-f" << "ffmetadata" << "-y" << metadataVideoFile;

        _FFMPEG.setProgram(program);
        _FFMPEG.setArguments(arguments);
        _FFMPEG.setWorkingDirectory(videoPath);
        _FFMPEG.start();

        if (_FFMPEG.waitForFinished(-1)) {
           //qDebug() << "Process finished successfully!";
            metaExists = checkMetaFile.exists() && checkMetaFile.isFile();
        } else {
            qDebug() << "Process failed to finish!";
        }
    }

    if (metaExists)  {
       QFile metadataFile(metadataVideoFile);
       if (!metadataFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
           qDebug() << "Failed to open file: " << metadataFile.errorString();
           // return 1;
       }

       QTextStream in(&metadataFile);
       while (!in.atEnd()) {
           QString line = in.readLine();
           // Check if line containes one of the defined parseMetadataTags
           foreach(const QString &metaTag, parseMetadataTags) {
               if (line.startsWith(metaTag, Qt::CaseSensitive)) {
                   m_customAttributes.insert(metaToCustomAttributes(metaTag), line.right(line.length() - metaTag.length() - 1));
               }
           }
       }
       metadataFile.close();
    }
}

/*
 * We will use pdfinfo tools to get metadata informations
 * CLI: pdfinfo <source_pdf_file_path> (Linux)
*/
void LocalResource::addPdfMetadataToAttributes()
{
    QString pdfFilePath = m_FileBackend.getFullPath();
    QString pdfPath = QFileInfo(m_FileBackend.getFullPath()).path();
    QString pdfName = QFileInfo(m_FileBackend.getFullPath()).baseName();
    QString metadataPdfFile = QString("%1/.tmp/%2.metadata.log").arg(pdfPath, pdfName);
    QFileInfo checkMetaFile(metadataPdfFile);
    bool metaExists = checkMetaFile.exists() && checkMetaFile.isFile();

    // Check if .tmp directory exists in pdf location, if not create .tmp directory, .tmp folder will be used to store log file
    if (!checkTmpFolder(pdfPath))
        return;

    if (!metaExists)  {
        QProcess _PDFINFO;
        QString program = "pdfinfo";
        QStringList arguments;

        arguments << pdfFilePath;
        //qDebug() << "arguments:  " << arguments;

        _PDFINFO.setProgram(program);
        _PDFINFO.setArguments(arguments);
        _PDFINFO.setWorkingDirectory(pdfPath);
        _PDFINFO.setStandardOutputFile(metadataPdfFile, QIODevice::Truncate);
        _PDFINFO.start();

        if (_PDFINFO.waitForFinished(-1)) {
           // qDebug() << "_PDFINFO Process finished successfully!";
           metaExists = checkMetaFile.exists() && checkMetaFile.isFile();
        } else {
           qDebug() << "Process failed to finish!";
        }
    }
    if (metaExists)  {
        QFile metadataFile(metadataPdfFile);
        if (!metadataFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
            qDebug() << "Failed to open file: " << metadataFile.errorString();
            // return 1;
        }

        QTextStream in(&metadataFile);
        QStringList metadataFields;
        metadataFields << "Title" << "Subject" << "Keywords" << "Author" << "Creator" << "CreationDate";

        while (!in.atEnd()) {
            QString line = in.readLine().trimmed();
            foreach(const QString &metadataField, metadataFields) {
                QString checkField = metadataField + ":";
                if (line.startsWith(checkField, Qt::CaseSensitive)) {
                    m_customAttributes.insert(metaToCustomAttributes(metadataField), line.right(line.length() - checkField.length()).trimmed());
                }
            }
        }
        metadataFile.close();
    }
}

/*
 * Convert Meta documents to Pdf and get pdfinfo metadata for Meta documents (Word, Excel, PowerPoint...)
 * and add to customAtttributes
*/
void LocalResource::addMetaDocsMetadataToAttributes()
{
    QString metaDocsFullPath = m_FileBackend.getFullPath();
    QString metaDocsPath = QFileInfo(m_FileBackend.getFullPath()).path();
    QString metaDocsName = QFileInfo(m_FileBackend.getFullPath()).baseName();
    QString metaDocsOutputPdf = QString("%1/.tmp/").arg(metaDocsPath);
    QString metaDocsPdfFile = QString("%1/.tmp/%2.pdf").arg(metaDocsPath, metaDocsName);
    QFileInfo checkMetaFile(metaDocsPdfFile);
    bool metaExists = checkMetaFile.exists() && checkMetaFile.isFile();

    if (!metaExists)  {
        QProcess _METADOCS;
        QString program = "libreoffice";
        QStringList arguments;

        // pdf only or pdf:writer_pdf_Export
        arguments << "--headless" << "--convert-to" << "pdf:writer_pdf_Export" << metaDocsFullPath << "--outdir" << metaDocsOutputPdf;

        _METADOCS.setProgram(program);
        _METADOCS.setArguments(arguments);
        _METADOCS.setWorkingDirectory(metaDocsPath);
        _METADOCS.start();

        if (_METADOCS.waitForFinished(5000)) {
           // qDebug() << "Converting MetaDocs to Pdf has been finished successfully!";
           metaExists = checkMetaFile.exists() && checkMetaFile.isFile();
        } else {
           _METADOCS.kill();
           _METADOCS.waitForFinished(1000);
           qWarning() << "Converting MetaDocs to Pdf timed out for:" << metaDocsFullPath;
        }
    }
    if (metaExists)  {
        // Get custom attributes for pdf file
        QHash<QString, QVariant> customAttributes = getAttributesForMetaDocsPdf(metaDocsPdfFile);

        // get only meta attributes and to file custom attributes
        foreach (const QString &key, customAttributes.keys()) {
            QVariant value = customAttributes.value(key);
            if (key.startsWith("meta")) {
                m_customAttributes[key] = value;
            }
        }
    }
}

QHash<QString, QVariant> LocalResource::getAttributesForMetaDocsPdf(const QString& metaPdfFilePath)
{
    QHash<QString, QVariant> customAttributes;
    QString pdfFilePath = metaPdfFilePath;
    QString pdfPath = QFileInfo(metaPdfFilePath ).path();
    QString pdfName = QFileInfo(metaPdfFilePath).baseName();
    QString metadataPdfFile = QString("%1/.tmp/%2.metadata.log").arg(pdfPath, pdfName);
    QFileInfo checkMetaFile(metadataPdfFile);
    bool metaExists = checkMetaFile.exists() && checkMetaFile.isFile();

    // Check if .tmp directory exists in pdf location, if not create .tmp directory, .tmp folder will be used to store log file
    if (!checkTmpFolder(pdfPath))
        return customAttributes;

    if (!metaExists)  {
        QProcess _PDFINFO;
        QString program = "pdfinfo";
        QStringList arguments;

        arguments << pdfFilePath;
        //qDebug() << "arguments:  " << arguments;

        _PDFINFO.setProgram(program);
        _PDFINFO.setArguments(arguments);
        _PDFINFO.setWorkingDirectory(pdfPath);
        _PDFINFO.setStandardOutputFile(metadataPdfFile, QIODevice::Truncate);
        _PDFINFO.start();

        if (_PDFINFO.waitForFinished(-1)) {
           //qDebug() << "_PDFINFO Process finished successfully!";
           metaExists = checkMetaFile.exists() && checkMetaFile.isFile();
        } else {
           qDebug() << "Process failed to finish!";
        }
    }
    if (metaExists)  {
        QFile metadataFile(metadataPdfFile);
        if (!metadataFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
            qDebug() << "Failed to open file: " << metadataFile.errorString();
            // return 1;
        }

        QTextStream in(&metadataFile);
        QStringList metadataFields;
        metadataFields << "Title" << "Subject" << "Keywords" << "Author" << "Creator" << "CreationDate";

        while (!in.atEnd()) {
            QString line = in.readLine().trimmed();
            foreach(const QString &metadataField, metadataFields) {
                QString checkField = metadataField + ":";
                if (line.startsWith(checkField, Qt::CaseSensitive)) {
                    customAttributes.insert(metaToCustomAttributes(metadataField), line.right(line.length() - checkField.length()).trimmed());
                }
            }
        }
        metadataFile.close();
    }

    return customAttributes;
}

// input: title
// output: metaTitle
QString LocalResource::metaToCustomAttributes(const QString& meta)
{
    //qDebug() << "metaToCustomAttributes: " << meta;
    QString attributeTitle = metadataAttributeTitlePrefix;
    if (meta.contains("-", Qt::CaseSensitive)) {
        QStringList metas = meta.split( "-" );
        foreach(const QString &metaTag, metas) {
            attributeTitle += capitalizeFirstLetter(metaTag);
        }
    } else {
        attributeTitle += capitalizeFirstLetter(meta);
    }

    return attributeTitle;
}

QString LocalResource::capitalizeFirstLetter(const QString &str) {
    //qDebug() << "capitalizeFirstLetter: " << str;

    if (str.isEmpty()) {
        return str; // Handle empty string case
    }

    QString firstLetter = str.at(0).toUpper();
    QString restOfWord = str.right(str.length() - 1).toLower();

    return firstLetter + restOfWord;
}

void LocalResource::addVideoDurationToAttributes()
{
    QString filePath = QFileInfo(m_FileBackend.getFullPath()).filePath();
    QProcess _FFPROBE;

    QString program = "ffprobe";
    QStringList arguments;

    // ffprobe -i <full_path_file_name> -v quiet -show_entries format=duration -hide_banner -of default=noprint_wrappers=1:nokey=1 -sexagesimal
    // Output: 0:01:35.650000
    arguments << "-i" << filePath << "-v" <<  "quiet" << "-show_entries" << "format=duration" << "-hide_banner" << "-of" << "default=noprint_wrappers=1:nokey=1" << "-sexagesimal";
    // qDebug() << program << arguments;
    _FFPROBE.start(program, arguments);

    bool successProcess = _FFPROBE.waitForFinished(-1);  // wait until job is finished
    // If ffprobe does not exists on system _FFPROBE QProcess will fail successProcess will be false
    if (successProcess) {
        QString output(_FFPROBE.readAllStandardOutput());
        QString duration = output.trimmed();
        // Remove miliseconds part 0:01:35.650000 ->  0:01:35
        QStringList durationInSeconds = duration.split('.');
        if (durationInSeconds.size() > 0)
            duration = durationInSeconds.at(0);
        m_customAttributes.insert("videoDuration", duration);
        // qDebug() << "Video duration added to customAttributes: " << m_customAttributes["videoDuration"].toString();
    } else {
        qWarning() << "ffprobe get video duration failed: " << _FFPROBE.errorString();
    }
}

void LocalResource::updateExcludeFromIndexing(bool excludeFromIndexing)
{
    qDebug() << "updateExcludeFromIndexing: " << excludeFromIndexing;;
    QString itemPath = QFileInfo(m_FileBackend.getFullPath()).path();
    QString itemAbsolutePath = QFileInfo(m_FileBackend.getFullPath()).absoluteFilePath();
    qDebug() << "itemPath: " << itemPath << "itemAbsolutePath: " <<  itemAbsolutePath;
    QString robotsTxtFile = itemPath += "/robots.txt";
    qDebug() << "ROBOTS.TXT: " << robotsTxtFile;

    QFile robotsFile(robotsTxtFile);
    QTextStream writeRobotsTxtFile(&robotsFile);

    if (robotsFile.exists())
    {
        qDebug() << "The robots file does not exists";
        if (!robotsFile.open(QIODevice::ReadWrite | QIODevice::Text)) {
            qDebug() << "The robots file: " << robotsTxtFile << " could not be open for reading and writing.";
            return;
        }
        QTextStream robotsTxtStream(&robotsFile);
        bool inRobotsTxt = false;
        while (!robotsTxtStream.atEnd())
        {
           QString line = robotsTxtStream.readLine();
           qDebug() << "The line from robots.txt: " << line;
           if (line.contains(itemAbsolutePath)) {
               inRobotsTxt = true;
               if (line.startsWith("#") && excludeFromIndexing)
                   robotsTxtStream << line.remove(0, 1);

           }
           if (!inRobotsTxt) {
               if (excludeFromIndexing)
                    robotsTxtStream << "#" << line;
           }
        }

        if (!inRobotsTxt && excludeFromIndexing)
            writeRobotsTxtFile << "Disallow: " + itemAbsolutePath + "\n";

        robotsFile.close();
    } else {
        qDebug() << "The robots file does not exists";
        if (!robotsFile.open(QIODevice::Append | QIODevice::ReadWrite)) {
            qDebug() << "The robots file: " << robotsTxtFile << " could not be open for writing.";
            return;
        }
        writeRobotsTxtFile << "User-agent: *\n";
        if (excludeFromIndexing)
            writeRobotsTxtFile << "Disallow: " + itemAbsolutePath + "\n";
        robotsFile.close();
    }
}

void LocalResource::addVideoDimensionsToAttributes()
{
    QString filePath = QFileInfo(m_FileBackend.getFullPath()).filePath();
    QProcess _FFPROBE;

    QString program = "ffprobe";
    QStringList arguments;

    // ffprobe -v error -select_streams v:0 -show_entries stream=width,height -of csv=s=x:p=0 <full_path_file_name>
    // Output: 1920x1080
    arguments << "-v" << "error" << "-select_streams" << "v:0" << "-show_entries" << "stream=width,height" << "-of" << "csv=s=x:p=0" << filePath;
    // qDebug() << program << arguments;
    _FFPROBE.start(program, arguments);

    bool successProcess = _FFPROBE.waitForFinished(-1);  // wait until job is finished
    // If ffprobe does not exists on system _FFPROBE QProcess will fail successProcess will be false
    if (successProcess) {
        QString output(_FFPROBE.readAllStandardOutput());
        m_customAttributes.insert("videoDimensions", output.trimmed());
        // qDebug() << "Video dimensions added to customAttributes: " << m_customAttributes["videoDimensions"].toString();
    } else {
        qWarning() << "ffprobe get video dimensions failed: " << _FFPROBE.errorString();
    }
}

void LocalResource::transcodeVideo()
{
    QString videoPath = QFileInfo(m_FileBackend.getFullPath()).path();
    QString filePath = QFileInfo(m_FileBackend.getFullPath()).filePath();
    QString fileName = QFileInfo(m_FileBackend.getFullPath()).baseName();
    QString transcodeVideoFile = QString("%1/%2.transcode.webm").arg(videoPath, fileName);
    QString transcodeLogFile = QString("%1/.tmp/%2.transcode.log").arg(videoPath, fileName);

    // Check if .tmp directory exists in video location, if not create .tmp directory, .tmp folder will be used to store log file
    if (!checkTmpFolder(videoPath))
        return;

    QProcess _FFMPEG;
    QString program = "ffmpeg";
    QStringList arguments;
    qint64 pid;
    arguments << "-y" << "-i" << filePath << transcodeVideoFile;
    //qDebug() << "transcodeVideoFile: " << transcodeVideoFile << ", transcodeLogFile: " << transcodeLogFile;
    //qDebug() << program << arguments;
    _FFMPEG.setProgram(program);
    _FFMPEG.setArguments(arguments);
    _FFMPEG.setWorkingDirectory(videoPath);
    _FFMPEG.setStandardErrorFile(transcodeLogFile, QIODevice::Truncate);
    _FFMPEG.startDetached(&pid);
    m_customAttributes.insert("transcodePid", pid);
    //qDebug() << "transcode process has been started pid: " << pid;
}

void LocalResource::downscaleVideo(const QString& downscale, const QString& downscaleWidth, const QString& downscaleHeight)
{
    QString videoPath = QFileInfo(m_FileBackend.getFullPath()).path();
    QString filePath = QFileInfo(m_FileBackend.getFullPath()).filePath();
    QString fileName = QFileInfo(m_FileBackend.getFullPath()).baseName();
    // QString fileSuffix .suffix()
    QString fileSuffix = QFileInfo(m_FileBackend.getFullPath()).suffix();
    QString downscaleVideoFile = QString("%1/%2_downscale_%3_%4.%5").arg(videoPath, fileName, downscaleWidth, downscaleHeight, fileSuffix);
    QString downscaleLogFile = QString("%1/.tmp/%2.downscale.log").arg(videoPath, fileName);

    // Check if .tmp directory exists in video location, if not create .tmp directory, .tmp folder will be used to store log file
    if (!checkTmpFolder(videoPath))
        return;

    QProcess _FFMPEG;
    QString program = "ffmpeg";
    QStringList arguments;
    qint64 pid;
    QString downscaleTo = QString("scale='iw/%1:ih/%2\'").arg(downscale, downscale);
    arguments << "-y" << "-i" << filePath << "-vf" << downscaleTo << downscaleVideoFile;
    //qDebug() << program << arguments;
    _FFMPEG.setProgram(program);
    _FFMPEG.setArguments(arguments);
    _FFMPEG.setWorkingDirectory(videoPath);
    _FFMPEG.setStandardErrorFile(downscaleLogFile, QIODevice::Truncate);
    _FFMPEG.startDetached(&pid);
    m_customAttributes.insert("transcodePid", pid);
    //qDebug() << "Downscale video process has been started pid: " << pid;
}

void LocalResource::continueTranscodeVideo()
{
    QString videoPath = QFileInfo(m_FileBackend.getFullPath()).path();
    QString filePath = QFileInfo(m_FileBackend.getFullPath()).filePath();
    QString fileName = QFileInfo(m_FileBackend.getFullPath()).baseName();
    // Downscaled video name is <video_name>_downscale_<width>_<height>.<video_ext>
    QString transcodeVideoFile = QString("%1/%2.transcode.webm").arg(videoPath, fileName);
    QString transcodeLogFile = QString("%1/.tmp/%2.transcode.log").arg(videoPath, fileName);

    // Check if .tmp directory exists in video location, if not create .tmp directory, .tmp folder will be used to store log file
    if (!checkTmpFolder(videoPath))
        return;

    QProcess _FFMPEG;
    QString program = "ffmpeg";
    QStringList arguments;
    qint64 pid;
    arguments << "-y" << "-i" << filePath << transcodeVideoFile;
    //qDebug() << "transcodeVideoFile: " << transcodeVideoFile<< ", transcodeLogFile: " << transcodeLogFile;
    //qDebug() << program << arguments;
    _FFMPEG.setProgram(program);
    _FFMPEG.setArguments(arguments);
    _FFMPEG.setWorkingDirectory(videoPath);
    // Log from QProcess is basically StandardErrorFile
    _FFMPEG.setStandardErrorFile(transcodeLogFile, QIODevice::Truncate);
    _FFMPEG.startDetached(&pid);
    m_customAttributes.insert("transcodePid", pid);
    //qDebug() << "transcode process has been started pid: " << pid;
}

void LocalResource::transcodeAbort(const QString& transcodePid, const QString& mode, const QString& downscaleWidth, const QString& downscaleHeight)
{
    QString videoPath = QFileInfo(m_FileBackend.getFullPath()).path();
    QString fileName = QFileInfo(m_FileBackend.getFullPath()).baseName();
    QString fileSuffix = QFileInfo(m_FileBackend.getFullPath()).suffix();
    QString transcodeLogFile = "";
    QString transcodeVideoFile = "";
    // <video_path>/.tmp/<video_file_name>.transcode.log, <video_path>/.tmp/<video_file_downscaled>.transcode.log
    // <video_path>/<video_file_name>.transcode.webm, <video_path>/<video_file_dowsncaled>.webm
    if (mode == "transcode") {
        if (downscaleWidth != "" && downscaleHeight != "") {
            transcodeLogFile = QString("%1/.tmp/%2_downscale_%3_%4.transcode.log").arg(videoPath, fileName, downscaleWidth, downscaleHeight);
            transcodeVideoFile = QString("%1/%2_downscale_%3_%4.transcode.webm").arg(videoPath, fileName, downscaleWidth, downscaleHeight);
        } else {
            transcodeLogFile = QString("%1/.tmp/%2.transcode.log").arg(videoPath, fileName);
            transcodeVideoFile = QString("%1/%2.transcode.webm").arg(videoPath, fileName);
        }
    } else { // downscale
        // <video_path>/.tmp/<video_file_name>.downscale.log
        // <video_path>/<video_file_name>_downscale_<width>_<height>.<video_file_suffix>
        transcodeLogFile = QString("%1/.tmp/%2.downscale.log").arg(videoPath, fileName);
        transcodeVideoFile = QString("%1/%2_downscale_%3_%4.%5").arg(videoPath, fileName, downscaleWidth, downscaleHeight, fileSuffix);
    }
    m_customAttributes.insert("abort", false);

    // Check if transcode log file exists
    if (QFile::exists(transcodeLogFile) && transcodePid != "")
    {
        QProcess _TRANSCODEABORT;
        QStringList arguments;
        QString program;
#ifdef Q_OS_WIN32
        program = "tskill";
        arguments << transcodePid;
#else
        program = "kill";
        arguments << "-9" << transcodePid;
#endif
        _TRANSCODEABORT.setProgram(program);
        _TRANSCODEABORT.setArguments(arguments);
        // qDebug() << program << arguments;
        _TRANSCODEABORT.start(program, arguments);
        bool successProcess = _TRANSCODEABORT.waitForFinished(-1);  // wait until job is finished
        if (successProcess)
        {
            // Remove log file
            if (QFile::remove(transcodeLogFile))
                qDebug() << "Transcoded log file has been removed removed: " << transcodeLogFile;
            else
                qWarning() << "Failed to remove transcode log file: " << transcodeLogFile;
            // Transcode/Downscale file is not correct now, remove it
            if (QFile::exists(transcodeVideoFile))
            {
                if (QFile::remove(transcodeVideoFile)) {
                    qDebug() << "Transcode video file has been removed: " << transcodeVideoFile;
                    m_customAttributes["abort"] = true;
                } else
                    qWarning() << "Failed to remove transcoded video file: " << transcodeVideoFile;
            }
        } else {
            qWarning() << "Failed to abort transcode process with PID: " << transcodePid;
        }
    }
}

bool LocalResource::checkTmpFolder(QString path)
{
    QString tmpPath = QString("%1/.tmp").arg(path);
    QDir tmpDir;
    if (!tmpDir.exists(tmpPath)) {
        if (!tmpDir.mkpath(tmpPath)) {
            qWarning() << "Failed to create: " << tmpPath << " directory!";
        }
    }

    return tmpDir.exists(tmpPath);
}


QImage LocalResource::loadVideoArt(int imgWidth, int imgHeight, int fromSecond, bool deleteTmp, bool coverRefresh)
{
    QImage resizedImg;
    QImage originalImg;
    QString fileName = QFileInfo(m_FileBackend.getFullPath()).baseName();
    QString filePath = QFileInfo(m_FileBackend.getFullPath()).filePath();
    QProcess _FFMPEG;
    QString videoPath = QFileInfo(m_FileBackend.getFullPath()).path();
    QString imgFullPathName;
    QDir tmpDir;
    QString tmpPath = QString("%1/.tmp").arg(videoPath);
    if (deleteTmp) {
        // Check if .tmp directory exists in video location, if not create .tmp directory
        if (!tmpDir.exists(tmpPath)) {
            if (!tmpDir.mkpath(tmpPath)) {
                // Something is wrong we can't create .tmp directory
                qWarning() << "Failed to create: " << tmpPath << " directory!";
                return resizedImg;
            }
        }
        imgFullPathName = QString("%1/.tmp/%2.%3.jpg").arg(videoPath, fileName, QString().number(fromSecond)); // <FULL_VIDEO_PATH>/.tmp/<VIDEO_FILE_NAME>.<fromSecond>.jpg
    } else
        imgFullPathName = QString("%1/%2.cover.jpg").arg(videoPath, fileName);  // <FULL_VIDEO_PATH>/<VIDEO_FILE_NAME>.cover.jpg

    QString takeFromVideo = "00:";
    if (fromSecond >= 60) {
        int getFromMinute = fromSecond / 60;
        fromSecond = fromSecond % 60;
        takeFromVideo += QString("%1:").arg(getFromMinute, 2, 10, QLatin1Char('0'));
    } else {
        takeFromVideo += "00:";
    }
    takeFromVideo += QString("%1").arg(fromSecond, 2, 10, QLatin1Char('0'));

    // Check if we need to refresh video cover image
    if (coverRefresh && QFile::exists(imgFullPathName)) {
        QFile::remove(imgFullPathName);
    }

    if (!QFile::exists(imgFullPathName)) {
        // Use ffmpeg to get image from video

        QString program = "ffmpeg";
        QStringList arguments;
        arguments << "-y" << "-i" << filePath << "-ss" << takeFromVideo << "-vframes" << "1" << "-q:v" << "1" << imgFullPathName;
        // qDebug() << program << arguments;
        _FFMPEG.start(program, arguments);

        bool successProcess = _FFMPEG.waitForFinished(-1);  // wait until job is finished
        // If ffmpeg does not exists on system _FFMPEG QProcess will fail successProcess will be false
        if (successProcess) {
            bool success = originalImg.load(imgFullPathName);

            if (success) {
                resizedImg = originalImg.scaled(imgWidth, imgHeight, Qt::KeepAspectRatio, Qt::SmoothTransformation);
                if (deleteTmp) {
                    // Remove video image from file system
                    QFile::remove(imgFullPathName);
                    //qInfo() << "Temp image file: " << imgFullPathName << " has been removed. ";
                }
            } else {
                qWarning() << "Failed image load after ffmpeg convert!";
                resizedImg = getMimeImage(imgHeight);
            }
        } else {
            qWarning() << "ffmpeg video to image convert failed: " << _FFMPEG.errorString() << LocalResource::generateVidoCovers;
            LocalResource::generateVidoCovers = false; //save the time and CPU resources
            resizedImg = getMimeImage(imgHeight);
        }
    } else {
        bool success = originalImg.load(imgFullPathName);
        if (success) {
            resizedImg = originalImg.scaled(imgWidth, imgHeight, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        } else {
            qWarning() << "Failed image load cover image!" ;
            resizedImg = getMimeImage(imgHeight);
        }
    }

    return resizedImg;
}

bool LocalResource::makeImage(const QString &imgFile, int imgWidth, int imgHeight)
{
    bool isOpen = false;
    bool isSaved = false;
    QImage resizedImg;
    if (isAudio()) {
        resizedImg = loadAudioArt(imgWidth, imgHeight);
    } else if (LocalResource::generateVidoCovers && isVideo()) {
        resizedImg = loadVideoArt(imgWidth, imgHeight, OG_VIDEO_COVER_IMG_FROM_SEC, false, false);
    } else {
        if (! QFileInfo::exists(imgFile)) {
            qWarning() << "Image not found" << imgFile;
            return false;
        }
        QImageReader reader(imgFile);
        QSize s = reader.size();
        //Calculate aspect ratio

        if (s.width() < imgWidth || s.height() < imgHeight) {
            float ry = (s.height() > s.width()) ? s.height() / (float) s.width() : 1;
            float rx = (s.height() <= s.width()) ? s.width() / (float) s.height() : 1;
            resizedImg = QImage(imgFile).scaled(imgWidth*rx, imgHeight*ry);
        } else {
            float r = (s.height() > s.width()) ? s.height() / (float) s.width() : s.width() / (float) s.height();
            resizedImg = loadResizedImg(imgFile, imgWidth*r);
        }
    }

    //Check resized image
    if (resizedImg.isNull())
    {
            qCritical() << "Failed to resize" <<  imgFile << imgWidth << imgHeight;
            m_customAttributes["cover"] = false;
            resizedImg = getMimeImage(imgHeight);
    }

    QImage finalImg = resizedImg;
    if (finalImg.isNull()) {
        finalImg = QImage(imgWidth, imgHeight, QImage::Format_ARGB32);
        finalImg.fill(Qt::transparent);
    }
    if (!isVideo() && resizedImg.width() >= imgWidth && resizedImg.height() >= imgHeight) {
        int cropOffsetX = (resizedImg.width() - imgWidth) / 2;
        int cropOffsetY = (resizedImg.height() - imgHeight) / 2;
        QImage croppedImg = resizedImg.copy(cropOffsetX, cropOffsetY, imgWidth, imgHeight);
        if (!croppedImg.isNull()) {
            finalImg = croppedImg;
        }
    }

    if (m_MemBackend.open(QIODevice::WriteOnly))
    {
            isOpen = true;
            if (! finalImg.save(&m_MemBackend, "jpeg", 90))
                qWarning() << "Can not save image to cache" << imgFile;
    }

    if (m_MemBackend.size() > 0) //saved ok!
    {
        isSaved = true;
        m_MemBackend.setContentType(QString("image/jpeg"));
        m_MemBackend.RefreshInfo(this);
        m_ETag = generateETag(true, m_FileBackend.getFullPath() + m_query, m_lastModified.toString(), m_MemBackend.size());
        m_contentType = QString("image/jpeg");
    }
    //qInfo() << "Cache size:" << m_MemBackend.size();

    if (isOpen)
        m_MemBackend.close();

    return isSaved;
}


bool LocalResource::makeMimeImage()
{
    return makeMimeImage(OG_IMG_WIDTH_MIN, OG_IMG_HEIGHT_MIN);
}

bool LocalResource::makeMimeImage(int imgWidth, int imgHeight)
{

    bool isSaved = false;

    if (imgHeight <= 128)
    {
        QImage img = getMimeImage( imgHeight);
        if (m_MemBackend.open(QIODevice::WriteOnly))
        {
            if (! img.save(&m_MemBackend, "png", 90)) {
                qWarning() << "Can not save scaled preview image to cache" << this->fileName();
                m_MemBackend.close();
                return false;
            }
            isSaved = true;
            m_MemBackend.setContentType(QString("image/png"));
            m_MemBackend.RefreshInfo(this);
            m_ETag = generateETag(true, m_FileBackend.getFullPath() + m_query, m_lastModified.toString(), m_MemBackend.size());
            m_contentType = QString("image/png");
            m_MemBackend.close();
        }

    } else {
        //load mimetipe icon
        QString mimeType =   getContentType().replace("/", "-");
        if (mimeType.isEmpty())
            mimeType = "folder";

        QImage img(QString(":/filemanager/mimetypes/128/%1.png").arg(mimeType));
        if (img.isNull()) {
            img = QImage(":/filemanager/mimetypes/128/document.png");
            if (img.isNull())
                img = QImage(":/filemanager/mimetypes/128/unknown.png");
        }
        if (img.isNull()) {
            qWarning() << "Can not load mime image" << mimeType;
            return false;
        }

        QImage mimeImage(imgWidth, imgHeight, QImage::Format_ARGB32);
        mimeImage.fill(Qt::transparent);
        QPainter painter(&mimeImage);
        //painter.drawImage((150 - img.width()) / 2, (150 - img.height()) / 2, img);
        painter.drawImage((imgWidth - img.width()) / 2, (imgHeight - img.height()) / 2, img);
        painter.end();
        //mimeImage = mimeImage.scaled(OG_IMG_WIDTH_MIN, OG_IMG_HEIGHT_MIN);
        if (m_MemBackend.open(QIODevice::WriteOnly))
        {
            if (! mimeImage.save(&m_MemBackend, "png", 90)) {
                qWarning() << "Can not save scaled preview image to cache" << this->fileName();
                m_MemBackend.close();
                return false;
            }
            isSaved = true;
            m_MemBackend.setContentType(QString("image/png"));
            m_MemBackend.RefreshInfo(this);
            m_contentType = QString("image/png");
            m_ETag = generateETag(true, m_FileBackend.getFullPath() + m_query, m_lastModified.toString(), m_MemBackend.size());
            m_MemBackend.close();
        }
    }
    return isSaved;
}

QImage LocalResource::getMimeImage(int size)
{
    int iconSize = 128;
    if (size <= 64)
        iconSize = 64;
    if (size <= 24)
        iconSize = 24;

    QString mimeType =  getContentType().replace("/", "-");
    if (mimeType.isEmpty())
        mimeType = "folder";

    QImage img(QString(":/filemanager/mimetypes/%1/%2.png").arg(QString::number(iconSize), mimeType));

    if (img.isNull()) {
        //return default icon for "document"
        QImage docImage(QString(":/filemanager/mimetypes/%1/document.png").arg(QString::number(iconSize)));
        img = docImage;
    }

    return img;
}

bool LocalResource::makeCss()
{
	  QString filePath = m_FileBackend.getFullPath();
      QFile origFile(filePath);
	  QByteArray decoded;
	  bool isSaved = false;
	  if (origFile.open(QFile::ReadOnly)) {
		  decoded.append(":root {\n");
		  QTextStream stream(&origFile);
		   while (!stream.atEnd())
		   {
				QString line = stream.readLine();
				int p = line.indexOf("=");
				if (p == -1) continue;
				QString varName = line.left(p).trimmed();
				QString varValue = line.mid(p + 1).trimmed();
                decoded.append(QString("\t%1: %2;\n").arg(varName, varValue).toUtf8());

		   }
		   decoded.append("}");
		   origFile.close();

		   if (m_MemBackend.open(QIODevice::WriteOnly))
		   {
			   //qInfo() << "Saving resized" << fileName() << "to cache as:" << type;
			   if (! m_MemBackend.write(decoded))
				   qWarning() << "Can not save decoded css to cache" << this->fileName();
			   if (m_MemBackend.size() > 0) //saved ok!
			   {
				   isSaved = true;
				   m_MemBackend.setContentType("text/css");
				   m_MemBackend.RefreshInfo(this);
                   m_contentType = QString("text/css");
				   m_ETag = generateETag(true, m_FileBackend.getFullPath() + m_query, m_lastModified.toString(), m_MemBackend.size());
			   }
			   //qInfo() << "Cache size:" << m_MemBackend.size();

			   m_MemBackend.close();
		   }
	  }
	  return isSaved;
}

int LocalResource::childCount()
{
  int c = m_childResources.count();
  //qDebug() << m_remotePath << m_childResources.count() <<  m_virtualChildResources.count();
  Q_FOREACH(LocalResource * child, m_childResources) {
	  c += child->childCount();
  }
  return c;
}

int LocalResource::cacheSize()
{
	int s = m_MemBackend.size();
	Q_FOREACH(LocalResource * child, m_childResources)
		s += child->cacheSize();
	return s;
}


qint64 LocalResource::openResource(QIODevice::OpenMode openMode)
{
    if ((openMode & QIODevice::ReadOnly) && !m_query.isEmpty() && (m_query.contains("resize") || m_query.contains("getIcon") || m_query.contains("cover") || m_query.contains("thumb")))
    {
        if (LocalFileBackend::isFileWriting(m_FileBackend.getFullPath()))
        {
            qWarning() << "LocalResource::openResource: file is open for writing, refusing thumbnail read:" << m_FileBackend.getFullPath();
            return FILEHANDLE_ERROR;
        }
    }

    bool writable = m_isWritable;

    //revert back to file backend when write open mode
    if (((openMode & QIODevice::WriteOnly) == QIODevice::WriteOnly)
        || ((openMode & QIODevice::Append) == QIODevice::Append)
        || ((openMode & QIODevice::ReadWrite) == QIODevice::ReadWrite)
        || ((openMode & QIODevice::Truncate) == QIODevice::Truncate))
    {
        if (! m_exists)
        {
            m_isFile = true;
        }
        writable = true;
        m_currentBackend = &m_FileBackend;
		m_isWriting = true;
    } else {

        //parse html templates - it works only on server!!
        #if (SERVER || EMBEDDED || ROUTER) && !defined(P2P_ENABLED) && !defined(NO_WEB_UI)
        if (QFileInfo(m_FileBackend.getFullPath()).suffix().startsWith("htm"))
            parseTemplate("text/html; charset=UTF-8");
        else if (QFileInfo(m_FileBackend.getFullPath()).suffix().startsWith("json"))
            parseTemplate("application/json; charset=UTF-8");
        #endif
    }

    if (m_currentBackend->open(openMode))
    {
		m_exists = m_isWriting;
        m_isWritable = writable;

		return m_currentBackend->fileHandle();
    }
    else
		return FILEHANDLE_ERROR;
}


bool LocalResource::isReadable() const
{
    return m_isReadable;
}

bool LocalResource::isWritable() const
{
    return m_isWritable;
}

QUrl LocalResource::url(QUrl baseUrl) const
{
    baseUrl.setPath(m_remotePath);
    if (m_query.length() > 0)
        baseUrl.setQuery(m_query);
    return baseUrl;
}

qint64 LocalResource::bytesAvailable() const
{
    return m_bytesAvailable;
}

qint64 LocalResource::bytesUsed() const
{
    return m_bytesUsed;
}

int LocalResource::blockSize() const
{
    return m_blockSize;
}

qint64 LocalResource::bytesFree() const
{
    return m_bytesFree;
}

qint64 LocalResource::bytesTotal()const
{
    return m_bytesTotal;
}

bool LocalResource::remove(bool forced)
{
    if (!m_preventDelete)
        return m_FileBackend.rm(forced);
    else
        return false;
}

ResourceType LocalResource::getResourceType() const
{
    if (this->isDir())
        return ResourceType::collection;
    else
        return ResourceType::document;
}

QString LocalResource::storageID() const
{
    return m_storageID;
}

QString LocalResource::absoluteFilePath() const
{
    return m_FileBackend.getFullPath();
}


QHash<QString, QVariant> LocalResource::getData() const
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
    resourceData.insert("parentExists", m_parentExists);
    resourceData.insert("isDir", m_isDir);
    resourceData.insert("isFile", m_isFile);
    resourceData.insert("isExecutable", m_isExecutable);

    resourceData.insert("blockSize", m_blockSize);
    resourceData.insert("bytesAvailable", m_bytesAvailable);
    resourceData.insert("bytesFree", m_bytesFree);
    resourceData.insert("bytesTotal", m_bytesTotal);
    resourceData.insert("bytesUsed", m_bytesUsed);
    resourceData.insert("nlink", m_nLinkValue);

    resourceData.insert("storageID", m_storageID);
    resourceData.insert("etag", m_ETag);

    resourceData.insert("customAttributes", m_customAttributes);


    if (m_link != "")
    {
        resourceData.insert("link", m_link);
        resourceData.insert("linkData", m_linkData);
    }

    if (m_currentBackendType == CloudResource::MemBackend)
    {
        resourceData.insert("bytesToCache", m_MemBackend.getBytes());
    }

    return resourceData;
}

QHash<QString, QHash<QString, QVariant> > LocalResource::getEntryData() const
{
	return  m_FileBackend.getEntryData();
}

QHash<QString, QVariant> LocalResource::getCustomAttributes() const
{
    return m_customAttributes;
}

QHash<QString, QVariant> LocalResource::getLinkData() const
{
    return m_linkData;
}

bool LocalResource::copy(const QString &destPath)
{
    return m_FileBackend.cp(destPath);
}

bool LocalResource::move(const QString &destPath)
{
    return m_FileBackend.mv(destPath);
}

bool LocalResource::rename(const QString &newName)
{
    return m_FileBackend.rename(newName);
}


bool LocalResource::isActual()
{
    return m_isActual;
}

void LocalResource::setOutdated()
{
    m_isActual = false;
}

LocalResource *LocalResource::getParentResource_ptr() const
{
	if (m_originalResource_ptr)
		return  m_originalResource_ptr->getParentResource_ptr();
	else
		return m_parentResource_ptr;
}

bool LocalResource::exists() const
{
    return m_exists;
}

bool LocalResource::mkcol()
{
    if (m_FileBackend.mkcol())
    {
      m_isDir = true;
      m_exists = true;
	  m_isReadable = true;
	  m_isWritable = true;
	  return true;
    }
    else
      return false;
}

#ifdef USE_MEM_POOL
void *LocalResource::operator new(size_t size)
{
	return  getAllocator()->allocate(size);
}

void LocalResource::operator delete(void *p)
{
	getAllocator()->free(p);
}
#endif

bool LocalResource::parentExists() const
{
    return m_parentExists;
}

