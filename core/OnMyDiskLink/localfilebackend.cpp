#include "localfilebackend.h"
#include "localdb.h"
#ifdef TAGLIB_SUPPORTED
#include <taglib/fileref.h>
#include <taglib/tag.h>
#include <taglib/mpegfile.h>
#include <taglib/mp4file.h>
#include <taglib/mp4tag.h>
#include <taglib/id3v2tag.h>
#include <taglib/id3v2frame.h>
#include <taglib/attachedpictureframe.h>
#include <taglib/flacfile.h>
#include <taglib/flacpicture.h>
#endif
#include "simplecrypt.h"
#include "localresource.h"
#include "exifstructures.h"
#include <QMutex>
#include <QMutexLocker>
#include <QDateTime>
#include <QHash>
#include <QtEndian>
#include <QRegExp>

static QMutex s_writingMutex;
static QHash<QString, int> s_openWriteHandles; // canonical/absolute path -> active handle count
static QHash<QString, qint64> s_writingLeases; // canonical/absolute path -> expire timestamp (ms)

struct MediaDateCacheEntry {
    qint64 mtimeMs;
    QDateTime mediaDate;
};
static QMutex s_mediaDateCacheMutex;
static QHash<QString, MediaDateCacheEntry> s_mediaDateCache;

static QDateTime parseExifDateTime(const QExifValue &val)
{
    if (val.isNull()) return QDateTime();
    QDateTime dt = val.toDateTime();
    if (dt.isValid() && dt.date().year() >= 1970 && dt.date().year() <= 2100) return dt;

    QString str = val.toString().trimmed();
    if (str.isEmpty()) return QDateTime();

    if (str.length() >= 19) {
        dt = QDateTime::fromString(str.left(19), "yyyy:MM:dd HH:mm:ss");
        if (dt.isValid() && dt.date().year() >= 1970 && dt.date().year() <= 2100) return dt;
    }
    dt = QDateTime::fromString(str, Qt::ISODate);
    if (dt.isValid() && dt.date().year() >= 1970 && dt.date().year() <= 2100) return dt;

    dt = QDateTime::fromString(str.left(19), "yyyy-MM-dd HH:mm:ss");
    if (dt.isValid() && dt.date().year() >= 1970 && dt.date().year() <= 2100) return dt;

    return QDateTime();
}

static QDateTime getMp4CreationDateTime(const QString &filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly))
        return QDateTime();

    qint64 fileSize = file.size();
    qint64 pos = 0;
    while (pos + 8 <= fileSize) {
        if (!file.seek(pos)) break;
        char header[8];
        if (file.read(header, 8) < 8) break;

        quint32 atomSize = qFromBigEndian<quint32>(reinterpret_cast<const uchar*>(header));
        QByteArray atomType(header + 4, 4);

        qint64 headerSize = 8;
        qint64 totalAtomSize = atomSize;
        if (atomSize == 1) {
            char extSize[8];
            if (file.read(extSize, 8) < 8) break;
            totalAtomSize = qFromBigEndian<quint64>(reinterpret_cast<const uchar*>(extSize));
            headerSize = 16;
        } else if (atomSize == 0) {
            totalAtomSize = fileSize - pos;
        }

        if (totalAtomSize < headerSize) break;

        if (atomType == "moov") {
            qint64 moovEnd = pos + totalAtomSize;
            qint64 subPos = pos + headerSize;
            while (subPos + 8 <= moovEnd) {
                if (!file.seek(subPos)) break;
                char subHeader[8];
                if (file.read(subHeader, 8) < 8) break;
                quint32 subSize = qFromBigEndian<quint32>(reinterpret_cast<const uchar*>(subHeader));
                QByteArray subType(subHeader + 4, 4);
                if (subSize < 8) break;

                if (subType == "mvhd") {
                    char mvhdBody[28];
                    qint64 bytesToRead = qMin<qint64>(subSize - 8, sizeof(mvhdBody));
                    if (file.read(mvhdBody, bytesToRead) >= 12) {
                        uchar version = static_cast<uchar>(mvhdBody[0]);
                        quint64 creationTime = 0;
                        if (version == 0) {
                            creationTime = qFromBigEndian<quint32>(reinterpret_cast<const uchar*>(mvhdBody + 4));
                        } else if (version == 1 && bytesToRead >= 16) {
                            creationTime = qFromBigEndian<quint64>(reinterpret_cast<const uchar*>(mvhdBody + 4));
                        }
                        if (creationTime > 2082844800ULL) { // seconds between 1904 and 1970
                            qint64 unixSecs = static_cast<qint64>(creationTime - 2082844800ULL);
                            QDateTime dt = QDateTime::fromSecsSinceEpoch(unixSecs, Qt::UTC);
                            if (dt.isValid() && dt.date().year() >= 1970 && dt.date().year() <= 2100) {
                                return dt.toLocalTime();
                            }
                        }
                    }
                    break;
                }
                subPos += subSize;
            }
            break;
        }
        pos += totalAtomSize;
    }
    return QDateTime();
}

static QDateTime getDateTimeFromFilename(const QString &fileName)
{
    // Pattern 1: YYYYMMDD_HHMMSS (e.g. IMG_20250514_142124 or VID_20250918_163657)
    static QRegExp rx1("(?:^|[^0-9])(20[0-3][0-9])(0[1-9]|1[0-2])(0[1-9]|[12][0-9]|3[01])[_-]([01][0-9]|2[0-3])([0-5][0-9])([0-5][0-9])(?:[^0-9]|$)");
    if (rx1.indexIn(fileName) >= 0) {
        int y = rx1.cap(1).toInt();
        int m = rx1.cap(2).toInt();
        int d = rx1.cap(3).toInt();
        int hh = rx1.cap(4).toInt();
        int mm = rx1.cap(5).toInt();
        int ss = rx1.cap(6).toInt();
        QDateTime dt(QDate(y, m, d), QTime(hh, mm, ss));
        if (dt.isValid()) return dt;
    }

    // Pattern 2: YYYY-MM-DD at HH.MM.SS (e.g. WhatsApp Image 2025-12-25 at 15.30.18)
    static QRegExp rx2("(20[0-3][0-9])-(0[1-9]|1[0-2])-(0[1-9]|[12][0-9]|3[01])(?:.*?)([01][0-9]|2[0-3])[.:]([0-5][0-9])[.:]([0-5][0-9])");
    if (rx2.indexIn(fileName) >= 0) {
        int y = rx2.cap(1).toInt();
        int m = rx2.cap(2).toInt();
        int d = rx2.cap(3).toInt();
        int hh = rx2.cap(4).toInt();
        int mm = rx2.cap(5).toInt();
        int ss = rx2.cap(6).toInt();
        QDateTime dt(QDate(y, m, d), QTime(hh, mm, ss));
        if (dt.isValid()) return dt;
    }

    return QDateTime();
}

static QString canonicalOrAbsPath(const QString &path)
{
    if (path.isEmpty()) return QString();
    QFileInfo fi(path);
    QString c = fi.canonicalFilePath();
    return c.isEmpty() ? fi.absoluteFilePath() : c;
}

QDateTime LocalFileBackend::getMediaCreationDateTime(const QFileInfo &fileInfo)
{
    if (!fileInfo.exists() || fileInfo.isDir())
        return QDateTime();

    QString key = canonicalOrAbsPath(fileInfo.absoluteFilePath());
    if (key.isEmpty()) return QDateTime();

    qint64 currentMtime = fileInfo.lastModified().toMSecsSinceEpoch();
    {
        QMutexLocker lock(&s_mediaDateCacheMutex);
        if (s_mediaDateCache.contains(key)) {
            const auto &entry = s_mediaDateCache[key];
            if (entry.mtimeMs == currentMtime) {
                return entry.mediaDate;
            }
        }
    }

    QString suffix = fileInfo.suffix().toLower();
    bool isImg = (suffix == "jpg" || suffix == "jpeg" || suffix == "jpe" || suffix == "jfif" ||
                  suffix == "png" || suffix == "heic" || suffix == "heif" || suffix == "webp" ||
                  suffix == "tiff" || suffix == "tif" || suffix == "dng" || suffix == "raw" ||
                  suffix == "cr2" || suffix == "nef" || suffix == "arw");
    bool isVid = (suffix == "mp4" || suffix == "mov" || suffix == "m4v" || suffix == "3gp" ||
                  suffix == "mkv" || suffix == "avi" || suffix == "webm" || suffix == "wmv");

    if (!isImg && !isVid)
        return QDateTime();

    QDateTime mediaDate;

    // A. For JPEG / TIFF, read EXIF
    if (suffix == "jpg" || suffix == "jpeg" || suffix == "jpe" || suffix == "jfif" || suffix == "tiff" || suffix == "tif") {
        QExifImageHeader header(fileInfo.absoluteFilePath());
        if (header.contains(QExifImageHeader::DateTimeOriginal)) {
            mediaDate = parseExifDateTime(header.value(QExifImageHeader::DateTimeOriginal));
        }
        if (!mediaDate.isValid() && header.contains(QExifImageHeader::DateTimeDigitized)) {
            mediaDate = parseExifDateTime(header.value(QExifImageHeader::DateTimeDigitized));
        }
        if (!mediaDate.isValid() && header.contains(QExifImageHeader::DateTime)) {
            mediaDate = parseExifDateTime(header.value(QExifImageHeader::DateTime));
        }
    }

    // B. For MP4 / MOV / M4V / 3GP, read mvhd creation time
    if (!mediaDate.isValid() && (suffix == "mp4" || suffix == "mov" || suffix == "m4v" || suffix == "3gp")) {
        mediaDate = getMp4CreationDateTime(fileInfo.absoluteFilePath());
    }

    // C. Fallback: filename timestamp
    if (!mediaDate.isValid()) {
        mediaDate = getDateTimeFromFilename(fileInfo.fileName());
    }

    {
        QMutexLocker lock(&s_mediaDateCacheMutex);
        s_mediaDateCache[key] = { currentMtime, mediaDate };
    }

    return mediaDate;
}

void LocalFileBackend::markFileWriting(const QString &filePath, bool isWriting)
{
    QString key = canonicalOrAbsPath(filePath);
    if (key.isEmpty()) return;
    QMutexLocker lock(&s_writingMutex);
    if (isWriting) {
        s_writingLeases[key] = QDateTime::currentMSecsSinceEpoch() + 30000;
    } else {
        s_writingLeases.remove(key);
    }
}

void LocalFileBackend::touchFileWriting(const QString &filePath, int timeoutMs)
{
    QString key = canonicalOrAbsPath(filePath);
    if (key.isEmpty()) return;
    QMutexLocker lock(&s_writingMutex);
    s_writingLeases[key] = QDateTime::currentMSecsSinceEpoch() + timeoutMs;
}

bool LocalFileBackend::isFileWriting(const QString &filePath)
{
    QString key = canonicalOrAbsPath(filePath);
    if (key.isEmpty()) return false;
    QMutexLocker lock(&s_writingMutex);
    if (s_openWriteHandles.value(key, 0) > 0)
        return true;
    qint64 expireTime = s_writingLeases.value(key, 0);
    if (expireTime > 0) {
        qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (now < expireTime) {
            return true;
        } else {
            s_writingLeases.remove(key);
        }
    }
    return false;
}

thread_local QMimeDatabase LocalFileBackend::m_db;

LocalFileBackend::LocalFileBackend(const QString &filePath, const QString &remotePath, QObject *parent)
	: IResourceBackend(parent), m_backendFile(filePath), m_fileInfo(filePath),
	  m_remotePath(remotePath)
{
    QByteArray idBase;
	QStorageInfo storageInfo(m_fileInfo.filePath());
    idBase.append(QHostInfo::localHostName().toUtf8());
    idBase.append(storageInfo.displayName().toUtf8());
    m_storageID = QString(idBase.toBase64());
	m_writing = false;
	m_isRoot = m_fileInfo.isDir() && m_remotePath.length() > 1 && m_remotePath.count('/') == 1;
}


bool LocalFileBackend::isImage() const
{
   return isImage(m_fileInfo);
}

bool LocalFileBackend::isImage(QFileInfo fileInfo) const
{
    static const QString images = "jpeg,jpg,png,bmp,gif";
    if (! fileInfo.suffix().isEmpty())
        return images.indexOf(fileInfo.suffix().toLower()) != -1;
    else
        return false;
}

bool LocalFileBackend::isAudio() const
{
   return isAudio(m_fileInfo);
}

bool LocalFileBackend::isAudio(QFileInfo fileInfo) const
{
    static const QString audio = "mp3,wav,flac,ogg,m4a,m4b";
    if (! fileInfo.suffix().isEmpty())
        return audio.indexOf(fileInfo.suffix().toLower()) != -1;
    else
        return false;
}

bool LocalFileBackend::isVideo() const
{
   return isVideo(m_fileInfo);
}

bool LocalFileBackend::isVideo(QFileInfo fileInfo) const
{
    static const QString video = "mp4,ogv,mov,webm,mkv,avi";
    if (! fileInfo.suffix().isEmpty())
        return video.indexOf(fileInfo.suffix().toLower()) != -1;
    else
        return false;
}

bool LocalFileBackend::isMarkdown() const
{
    return isMarkdown(m_fileInfo);
}

bool LocalFileBackend::isMarkdown(QFileInfo fileInfo) const
{
    static const QString markdownText = "md,txt";
    if (! fileInfo.suffix().isEmpty())
        return markdownText.indexOf(fileInfo.suffix().toLower()) != -1;
    else
        return false;

}

bool LocalFileBackend::isVector() const
{
     return isVector(m_fileInfo);
}

bool LocalFileBackend::isVector(QFileInfo fileInfo) const
{
    static const QString vectorformats = "svg";
    if (! fileInfo.suffix().isEmpty())
        return vectorformats.indexOf(fileInfo.suffix().toLower()) != -1;
    else
        return false;
}

// WP 3.2 - Multiformat text processing
bool LocalFileBackend::isPdf() const
{
   return isPdf(m_fileInfo);
}

bool LocalFileBackend::isPdf(QFileInfo fileInfo) const
{
    static const QString audio = "pdf";
    if (! fileInfo.suffix().isEmpty())
        return audio.indexOf(fileInfo.suffix().toLower()) != -1;
    else
        return false;
}

bool LocalFileBackend::isImagePng() const
{
   return isImagePng(m_fileInfo);
}

bool LocalFileBackend::isImagePng(QFileInfo fileInfo) const
{
    static const QString images = "png";
    if (! fileInfo.suffix().isEmpty())
        return images.indexOf(fileInfo.suffix().toLower()) != -1;
    else
        return false;
}

bool LocalFileBackend::isMetaDocs() const
{
   return isMetaDocs(m_fileInfo);
}

bool LocalFileBackend::isMetaDocs(QFileInfo fileInfo) const
{
    // Front-end:
    // item.isDocument(), item.isSpreadSheet(), item.isPresentation()
    // doc,docx,odt,epub,fodt,fb2,fods,ods,ots,csv,xls,xlsm,xlsx,xlt,xltm,xltx,fodp,odp,otp,pot,potm,potx,pps,ppsm,ppsx,ppt,pptm,pptx
    // only csv currently does not exist in array vs frontend
    static const QString metaDocs = "doc,docx,odt,epub,fodt,fb2,fods,ods,ots,xls,xlsm,xlsx,xlt,xltm,xltx,fodp,odp,otp,pot,potm,potx,pps,ppsm,ppsx,ppt,pptm,pptx";
    if (! fileInfo.suffix().isEmpty())
        return metaDocs.indexOf(fileInfo.suffix().toLower()) != -1;
    else
        return false;
}


LocalFileBackend::~LocalFileBackend()
{
    if (this->isOpen())
        this->close();
}

int LocalFileBackend::getFileCount(QFileInfo fileInfo) const
{
    QDir dir( fileInfo.absoluteFilePath());
    dir.setFilter( QDir::AllEntries | QDir::NoDotAndDotDot );
    return  dir.count();
}


QHash<QString, QVariant> LocalFileBackend::genData(QFileInfo fileInfo) const
{
    QHash<QString, QVariant> resourceData;
	if (! fileInfo.exists())
		fileInfo.refresh();

	resourceData.reserve(32);
    QString contentType = "";
    int fileCount = -1;

    if (fileInfo.exists())
    {
        resourceData.insert("size", fileInfo.size());
        QDateTime mediaDate = getMediaCreationDateTime(fileInfo);
        if (mediaDate.isValid()) {
            resourceData.insert("lastModDate", mediaDate);
            resourceData.insert("createdDate", mediaDate);
        } else {
            resourceData.insert("lastModDate", fileInfo.lastModified());
            #if QT_VERSION >= 0x050F00
            resourceData.insert("createdDate", fileInfo.birthTime());
            #else
            resourceData.insert("createdDate", fileInfo.created());
            #endif
        }
        resourceData.insert("lastReadDate", fileInfo.lastRead());

        if (!fileInfo.isDir() ) {
            QMimeType mime = m_db.mimeTypeForFile(fileInfo.absoluteFilePath());
            contentType = mime.name();
        } else
            contentType =  "folder";

        resourceData.insert("contentType",  contentType);
        resourceData.insert("exists", fileInfo.exists());
        resourceData.insert("isReadable", fileInfo.isReadable());
        resourceData.insert("isWritable", fileInfo.isWritable());
        resourceData.insert("parentExists", fileInfo.absoluteDir().exists());
        resourceData.insert("isDir", fileInfo.isDir());
        resourceData.insert("isFile", fileInfo.isFile());
        resourceData.insert("isExecutable", fileInfo.isExecutable());

		if (m_isRoot) //add storage information only for root folders
		{
			QStorageInfo storageInfo(m_fileInfo.filePath());

			#if QT_VERSION >= 0x050600
			resourceData.insert("blockSize", storageInfo.blockSize());
			#else
			resourceData.insert("blockSize", 512);
			#endif
			resourceData.insert("bytesAvailable", storageInfo.bytesAvailable());
			resourceData.insert("bytesFree", storageInfo.bytesFree());
			resourceData.insert("bytesTotal", storageInfo.bytesTotal());
			resourceData.insert("bytesUsed", storageInfo.bytesTotal() > storageInfo.bytesAvailable() ? storageInfo.bytesTotal() - storageInfo.bytesAvailable() : 0);
		}

        if (fileInfo.isDir())
        {
            fileCount = getFileCount(fileInfo);
            QDir d(fileInfo.absoluteFilePath());
            resourceData.insert("nlink", fileCount);
        }
        else
            resourceData.insert("nlink", 1);
    }
    else
    {
        resourceData.insert("exists", false);
        resourceData.insert("parentExists", fileInfo.absoluteDir().exists());
    }

    resourceData.insert("storageID", m_storageID);
    //get custom attributes
    QHash<QString, QVariant> customAttributes = genCustomAttributes(fileInfo, fileCount);


    //get link data from db
    if (LocalDB::instance().dbConnected() ) // don't ask for link  on server or for virtual items
    {
         QString itemPath = m_fileInfo == fileInfo ? m_remotePath : m_remotePath + fileInfo.fileName();
         if (fileInfo.isDir() && !itemPath.endsWith("/")) itemPath.append("/");
         QHash<QString, QVariant> linkData;
         QString link = LocalDB::instance().getPublicLinkForFile(itemPath, linkData);
         if (!link.isEmpty()) {
             resourceData.insert("link",  link);
             resourceData.insert("linkData", linkData);
             customAttributes.insert("itemPath", itemPath);
         }
    }
    resourceData.insert("customAttributes",  customAttributes);

    return resourceData;
}


QHash<QString, QVariant> LocalFileBackend::genCustomAttributes(QFileInfo fileInfo, int fileCount) const
{
    QHash<QString, QVariant> customAttributes;
    QDateTime mediaDate = getMediaCreationDateTime(fileInfo);
    if (mediaDate.isValid()) {
        customAttributes.insert("mediaDate", mediaDate.toString(Qt::ISODate));
    }
    QString readMeFile = readMeFileName(fileInfo);
    QFile readMe(readMeFile);
    customAttributes["customtitle"] = false;
    customAttributes["customdescr"] = false;
    customAttributes["longdescr"] = false;
    QString contentType = "";
    if (!fileInfo.isDir() ) {
        QMimeType mime = m_db.mimeTypeForFile(fileInfo.absoluteFilePath());
        contentType = mime.name();
    } else {
        contentType =  "folder";
        //read cinfig file from folder
        QString conifFileName = fileInfo.absoluteFilePath() + "/.conf";
        QFileInfo configInfo(conifFileName);
        if  (configInfo.exists()) {
            QSettings confSettings(conifFileName, QSettings::IniFormat);
            confSettings.beginGroup("settings");
            foreach (const QString &key, confSettings.allKeys()) {
                customAttributes.insert(key, confSettings.value(key));
            }
            //add site attrs
            customAttributes.insert("isSite", true);
            customAttributes.insert("siteName", fileInfo.baseName());
            customAttributes.insert("siteUrl", customAttributes.contains("domain") ? QString("https://%1").arg(customAttributes["domain"].toString()) : QString("/sites/%1").arg(fileInfo.baseName()));
        }
    }

    //check Readme.md for file description
    if (readMe.open(QFile::ReadOnly))
    {
        //Read first line as title
        QByteArray  line = readMe.readLine();
        if (!line.isEmpty()) {
            customAttributes["title"] = QString::fromUtf8(line).replace("#", "").trimmed();
            customAttributes["customtitle"] = true;
        } else {
            customAttributes["title"] = "";
            customAttributes["customtitle"] = false;
        }
        QByteArray descr;
        QByteArray dLine = readMe.readLine();
        //skip first header divider
        if (! (QString::fromUtf8(dLine).trimmed().startsWith("==") || (QString::fromUtf8(dLine).trimmed().startsWith("--"))))
            descr.append(dLine);
        int maxLineCount = 3;
        int lastLinePos = 0;
        int lastLineLength = 0;

        while (maxLineCount && !readMe.atEnd()) {
            --maxLineCount;
            QByteArray dLine = readMe.readLine();
            if (QString::fromUtf8(dLine).trimmed() == "")
                continue;
            if (QString::fromUtf8(dLine).trimmed().startsWith("==")) {
                if (lastLineLength)
                    descr.remove(lastLinePos, lastLineLength);
                break;
            }
            if (QString::fromUtf8(dLine).trimmed().startsWith("--")) {
                if (lastLineLength)
                    descr.remove(lastLinePos, lastLineLength);
                break;
            }
            if (QString::fromUtf8(dLine).trimmed().startsWith("#"))
                break;
           lastLinePos = descr.length();
           lastLineLength = dLine.length();
           descr.append(dLine);
        }
        if (!descr.isEmpty()) {
            customAttributes["description"] = QString::fromUtf8(descr).trimmed();
            customAttributes["longdescr"] = !readMe.atEnd();
            customAttributes["customdescr"] = true;
        } else {
            customAttributes["description"] = "";
            customAttributes["longdescr"] = false;
        }
        //save attrs
        readMe.close();
        customAttributes["descrVersion"] =  SimpleCrypt::stringToHash(QFileInfo(readMeFile).lastModified().toString());
    } else {
        if (fileInfo.isDir() && fileCount == -1)
            fileCount = getFileCount(fileInfo);
        customAttributes["title"] = fileInfo.fileName();
        customAttributes["description"] = contentType;
        customAttributes["longdescr"] = false;
    }

    if (isAudio(fileInfo)) {
        #ifdef TAGLIB_SUPPORTED

        TagLib::FileRef f(fileInfo.absoluteFilePath().toStdString().data(), true, TagLib::AudioProperties::Average);
        if (f.tag())
        {
            customAttributes["album"] = f.tag()->album().toCString(true);
            customAttributes["artist"] = f.tag()->artist().toCString(true);
            if (!customAttributes["customtitle"].toBool() && !f.tag()->title().isEmpty()) {
                customAttributes["title"] = f.tag()->title().toCString(true);
                customAttributes["customtitle"] = true;
            }
            if (!customAttributes["customdescr"].toBool() && !f.tag()->comment().isEmpty()) {
                customAttributes["description"] = f.tag()->comment().toCString(true);
                customAttributes["customdescr"] = true;
            }
        }
       #endif
        customAttributes["cover"] = hasAudioCover(fileInfo);
    } else {
        bool hasPreview = contentType.startsWith("image");
        QString coverImage = coverFileName(fileInfo);
        bool hasCover = QFileInfo::exists(coverImage);
        // Ser cover=true for video if user enabled ffmped genetator
        if (isVideo(fileInfo)) {
            hasCover = hasCover | LocalResource::generateVidoCovers;
            customAttributes["videoProcessor"] = LocalResource::generateVidoCovers;
            if (customAttributes["videoProcessor"].toBool()) {
                /*
                 * Transcode/downscale part
                 * Check if we have transcoded file in original size, in that case name will be:
                 * <video_file_name>.transcode.webm
                 * Video can also be downscaled and transcoded, in that case name will be:
                 * <video_file>_downscale_<width>_<height>.transcode.webm
                 * this will be handled on front end where we have all files in fileNavigator.fileList
                */
                QString transcodedVideoFullPath = transcodeFileName(fileInfo);
                // check <video_file>.transcode.webm
                customAttributes["videoTranscoded"] = (transcodedVideoFullPath != "") && QFileInfo::exists(transcodedVideoFullPath);
                customAttributes["videoTranscodeName"] = (customAttributes["videoTranscoded"].toBool()) ? QFileInfo(transcodedVideoFullPath).fileName() : "";
            }
        }
        customAttributes["preview"] = hasPreview || hasCover;
        customAttributes["cover"] = hasCover;
        if (hasCover) {
            customAttributes["coverImage"] = QFileInfo(coverImage).fileName();
            customAttributes["coverVersion"] =  SimpleCrypt::stringToHash(QFileInfo(coverImage).lastModified().toString());
        }
    }

    // Support of 'robots.txt' in folders
    customAttributes["excludeFromIndexing"] = isExcludedFromIndexing(fileInfo);

    return customAttributes;
}

// Support of 'robots.txt' in folders
bool LocalFileBackend::isExcludedFromIndexing(QFileInfo fileInfo) const
{
    bool excludeFromIndexing = false;
    QString absolutePath = fileInfo.absolutePath();
    QString robotsTxtPath =  absolutePath  + "/robots.txt";
    QString absoluteFilePath = fileInfo.absoluteFilePath();

    if (QFileInfo(absoluteFilePath).isDir())
        absoluteFilePath += "/";

    QFileInfo robotsTxt(robotsTxtPath);

    if (robotsTxt.exists() && robotsTxt.isFile()) {
        QFile inputFile(robotsTxtPath);
        if (inputFile.open(QIODevice::ReadOnly))
        {
           QTextStream in(&inputFile);
           while (!in.atEnd())
           {
              QString line = in.readLine();
              if (line.trimmed().startsWith("Disallow:")) {
                  // Disallow: /private/
                  QString disallowLine = line.trimmed();
                  // First character should be / we should remove one
                  QString disallowPath = disallowLine.right(disallowLine.length() - 9).trimmed();
                  if (QString(absoluteFilePath).endsWith(disallowPath)) {
                      qDebug() <<  "The item: " << absoluteFilePath << " will be excluded fromm indexing";
                      excludeFromIndexing = true;
                  }
              }
           }
           inputFile.close();
        }
    }

    return excludeFromIndexing;
}

bool LocalFileBackend::open(QIODevice::OpenMode mode)
{
    if (m_backendFile.open(mode))
    {
        QIODevice::open(mode);
		//revert back to file backend when write open mode
		if (((mode & QIODevice::WriteOnly) == QIODevice::WriteOnly)
			|| ((mode & QIODevice::Append) == QIODevice::Append)
			|| ((mode & QIODevice::ReadWrite) == QIODevice::ReadWrite)
			|| ((mode & QIODevice::Truncate) == QIODevice::Truncate))
		{
			m_writing = true;
            QString key = canonicalOrAbsPath(m_backendFile.fileName());
            if (!key.isEmpty()) {
                QMutexLocker lock(&s_writingMutex);
                s_openWriteHandles[key] = s_openWriteHandles.value(key, 0) + 1;
            }
		}

        m_fileHandle = ++CloudResource::m_Handle;
        return true;
	} else
		m_fileHandle = FILEHANDLE_ERROR;
    return false;
}

void LocalFileBackend::close()
{
	if (m_backendFile.isOpen())
	{
		--CloudResource::m_Handle;
		m_fileHandle = 0;
		m_backendFile.close();
		if (m_writing) {
			m_backendFile.flush();
            QString key = canonicalOrAbsPath(m_backendFile.fileName());
            if (!key.isEmpty()) {
                QMutexLocker lock(&s_writingMutex);
                int count = s_openWriteHandles.value(key, 0) - 1;
                if (count <= 0)
                    s_openWriteHandles.remove(key);
                else
                    s_openWriteHandles[key] = count;
            }
            m_writing = false;
        }
	}
}

bool LocalFileBackend::seek(qint64 pos)
{
    return m_backendFile.seek(pos);
}

bool LocalFileBackend::atEnd() const
{
    return m_backendFile.atEnd();
}


qint64 LocalFileBackend::readData(char *data, qint64 maxlen)
{
    return m_backendFile.read(data, maxlen);
}

qint64 LocalFileBackend::writeData(const char *data, qint64 len)
{
    return m_backendFile.write(data, len);
}

bool LocalFileBackend::RefreshInfo(CloudResource *resource)
{
	m_fileInfo.refresh();
    if (resource)
    {
        QVariantHash data = this->genData(m_fileInfo);
        resource->LoadData(data);
    }
    return false;
}

bool LocalFileBackend::RefreshAllInfo(CloudResource *resource)
{
    m_fileInfo.refresh();

    if (resource)
    {
		//bool isRoot =  resource->is;
		resource->LoadData(this->genData(m_fileInfo));
        if (m_fileInfo.isDir())
        {
			resource->LoadEntryData(getEntryData());
        }
    }

    return false;
}

bool LocalFileBackend::RefreshAttributes(CloudResource *resource)
{
    if (resource)
    {
        QHash<QString, QVariant> resourceData;
        resourceData["customAttributes"] = this->genCustomAttributes(m_fileInfo);
        resource->LoadData(resourceData);
    }
    return true;
}


QHash<QString, QHash<QString, QVariant> > LocalFileBackend::getEntryData() const
{
	QFileInfoList infoList = QDir(m_fileInfo.absoluteFilePath()).entryInfoList(QDir::NoDotAndDotDot | QDir::AllEntries | QDir::Readable, QDir::DirsFirst | QDir::Name | QDir::IgnoreCase);
	QHash<QString, QHash<QString, QVariant> > entryData;

	entryData.reserve(infoList.count());

	Q_FOREACH(QFileInfo fileInfo, infoList)
	{
		entryData.insert(fileInfo.fileName(), genData(fileInfo));
	}

	return entryData;

}

qint64 LocalFileBackend::size() const
{
    return m_fileInfo.size();
}

QString LocalFileBackend::storageID()
{
    return m_storageID;
}

QByteArray LocalFileBackend::readFromPosition(qint64 from, qint64 maxlen)
{
    QByteArray data;

    if (!m_backendFile.seek(from))
        return data;

    return m_backendFile.read(maxlen);
}


bool LocalFileBackend::isSequential() const
{
    return false;
}

QString LocalFileBackend::childFilePath(QString childPath)
{
    QString childFilePath =  m_fileInfo.absoluteFilePath();
    if (!childPath.startsWith(QDir::separator()))
        childFilePath.append(QDir::separator());
    childFilePath.append(childPath);

    return childFilePath;
}

QString LocalFileBackend::getFullPath() const
{
    return m_fileInfo.absoluteFilePath();
}


QString LocalFileBackend::fileName() const
{
    return m_fileInfo.fileName();
}

QString LocalFileBackend::readMeFileName(QFileInfo fileInfo) const
{
    if (isMarkdown(fileInfo))
        return fileInfo.absoluteFilePath(); // the file itself is readme
    else
        return fileInfo.isDir() ? fileInfo.absoluteFilePath() + "/Readme.md" : fileInfo.absolutePath() + "/" + fileInfo.baseName() + ".Readme.md";
}

QString LocalFileBackend::readMeFileName() const
{
    return readMeFileName(m_fileInfo);
}

QString LocalFileBackend::coverFileName(QFileInfo fileInfo) const
{
    if (isImage(fileInfo))
       return fileInfo.absoluteFilePath(); // the file itself is icon
    else if (fileInfo.isDir()) {
       QString coverPng = fileInfo.absoluteFilePath() + "/cover.png";
       if (QFileInfo::exists(coverPng))
           return coverPng;
       QString coverJpg = fileInfo.absoluteFilePath() + "/cover.jpg";
       if (QFileInfo::exists(coverJpg))
           return coverJpg;
       return fileInfo.absoluteFilePath() + "/folder.jpg";
    } else {
       return fileInfo.absolutePath() + "/" + fileInfo.baseName() + ".cover.jpg";
    }
}


QString LocalFileBackend::coverFileName() const
{
    return coverFileName(m_fileInfo);
}

QString LocalFileBackend::audioCoverFile(const QFileInfo &fileInfo)
{
    // 1. Check file-specific cover: e.g. track.cover.jpg
    QString specificCover = fileInfo.absolutePath() + "/" + fileInfo.baseName() + ".cover.jpg";
    if (QFileInfo::exists(specificCover))
        return specificCover;

    // 2. Same directory (common album art names)
    QDir dir = fileInfo.dir();
    const QStringList candidates = { "cover.png", "cover.jpg", "cover.jpeg", "folder.jpg", "folder.png", "album.jpg", "albumart.jpg", "front.jpg", "front.png" };
    for (const QString &c : candidates) {
        if (dir.exists(c))
            return dir.absoluteFilePath(c);
    }

    // 3. Parent directory (e.g. for multi-disc albums like "Album/DISC 1/track.mp3" -> "Album/cover.png")
    QDir parentDir = dir;
    if (parentDir.cdUp()) {
        for (const QString &c : candidates) {
            if (parentDir.exists(c))
                return parentDir.absoluteFilePath(c);
        }
    }
    return QString();
}

bool LocalFileBackend::hasAudioCover(const QFileInfo &fileInfo)
{
    QString extCover = audioCoverFile(fileInfo);
    if (!extCover.isEmpty() && QFileInfo::exists(extCover))
        return true;

    #ifdef TAGLIB_SUPPORTED
    QString suffix = fileInfo.suffix().toLower();
    if (suffix == "mp3") {
        TagLib::String s(QDir::toNativeSeparators(fileInfo.absoluteFilePath()).toUtf8().constData(), TagLib::String::UTF8);
        TagLib::MPEG::File f(s.toCString(true), false);
        if (f.hasID3v2Tag() && !f.ID3v2Tag()->frameListMap()["APIC"].isEmpty())
            return true;
    } else if (suffix == "m4a" || suffix == "mp4") {
        TagLib::String s(QDir::toNativeSeparators(fileInfo.absoluteFilePath()).toUtf8().constData(), TagLib::String::UTF8);
        TagLib::MP4::File f(s.toCString(true), false);
        if (f.tag() && !f.tag()->itemMap()["covr"].toCoverArtList().isEmpty())
            return true;
    } else if (suffix == "flac") {
        TagLib::String s(QDir::toNativeSeparators(fileInfo.absoluteFilePath()).toUtf8().constData(), TagLib::String::UTF8);
        TagLib::FLAC::File f(s.toCString(true), false);
        if (!f.pictureList().isEmpty())
            return true;
    }
    #endif

    return false;
}

QString LocalFileBackend::transcodeFileName(QFileInfo fileInfo) const
{
    // LocalResource::generateVidoCovers
    // If video file is already webm format we don't need transcode video file name
    if (isVideo(fileInfo) && !fileInfo.fileName().contains(".webm"))
        return fileInfo.absolutePath() + "/" + fileInfo.baseName() + ".transcode.webm";
    else
        return "";
}

QString LocalFileBackend::transcodeFileName() const
{
    return transcodeFileName(m_fileInfo);
}

bool LocalFileBackend::rm(bool forced)
{
    //readme
    if (QFileInfo::exists(readMeFileName()))
        m_fileInfo.absoluteDir().remove(readMeFileName());
    //cover
    if (QFileInfo::exists(coverFileName()))
        m_fileInfo.absoluteDir().remove(coverFileName());

    if (m_fileInfo.isFile())
    {
        m_fileInfo.absoluteDir().remove(m_fileInfo.fileName());
        return !QFileInfo::exists(m_fileInfo.absoluteFilePath());
    }
    else
    {
       if (forced)
       {
            QDir d(m_fileInfo.absoluteFilePath());
            d.removeRecursively();
       } else
            m_fileInfo.absoluteDir().rmdir(m_fileInfo.fileName());
       return !QFileInfo::exists(m_fileInfo.absoluteFilePath());
    }
}

static bool copyRecursively(const QString &srcFilePath,
                            const QString &dstFilePath);

static bool moveFile(const QString &srcFilePath,
                            const QString &destPath)
{
    QString destFilePath  = QFileInfo(destPath).absolutePath() + "/" + QFileInfo(srcFilePath).fileName();
    QDir d;
    //remove dest
    if (QFileInfo::exists(destFilePath)) {
        if (QFileInfo(destFilePath).isDir())
            QDir(destFilePath).removeRecursively();
        else
            QFile::remove(destFilePath);
    }
    //move
    if (d.rename(srcFilePath, destFilePath))
        return true;

    // Fallback for cross-filesystem moves
    if (copyRecursively(srcFilePath, destFilePath)) {
        QFileInfo srcInfo(srcFilePath);
        if (srcInfo.isDir())
            QDir(srcFilePath).removeRecursively();
        else
            QFile::remove(srcFilePath);
        return true;
    }
    return false;
}

bool LocalFileBackend::mv(const QString &destPath)
{
    bool result = true;
    if (m_fileInfo.isFile()) {
        //copy readme and cover
        if (! isMarkdown() && QFileInfo(readMeFileName()).exists())
             result = moveFile(readMeFileName(), destPath);
        if (! isImage() && result && QFileInfo(coverFileName()).exists())
             result = moveFile(coverFileName(), destPath);
    }
    if (result)
        result = moveFile(getFullPath(), destPath);
    return result;
}

static bool copyRecursively(const QString &srcFilePath,
                            const QString &dstFilePath)
{
    QFileInfo srcFileInfo(srcFilePath);
    if (srcFileInfo.isDir()) {
        QDir targetDir(dstFilePath);
        if (! targetDir.exists())
            if (! targetDir.mkpath(dstFilePath))
                return false;
        QDir sourceDir(srcFilePath);
        QStringList fileNames = sourceDir.entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
        foreach (const QString &fileName, fileNames) {
            const QString newSrcFilePath
                    = srcFilePath + "/" + fileName;
            const QString newDstFilePath
                    = dstFilePath + "/" + fileName;
            if (!copyRecursively(newSrcFilePath, newDstFilePath))
                return false;
        }
    } else {
        if (!QFile::copy(srcFilePath, dstFilePath))
            return false;
    }
    return true;
}

bool LocalFileBackend::cp(const QString &destPath)
{
    if (m_fileInfo.isFile())
    {
       //remove dest
       if (QFileInfo::exists(destPath))
            QFile::remove(destPath);
       if ( QFile::copy(getFullPath(), destPath)) {
           //find readme and cover
           QFileInfo destInfo(destPath);
           QString destReadme = destInfo.absolutePath() + "/" + destInfo.baseName() + ".Readme.md";
           QString destCover = destInfo.absolutePath() + "/" + destInfo.baseName() + ".cover.jpg";

           //remove readme
           if (destPath != destPath && QFileInfo::exists(destPath))
               QFile::remove(destReadme);

           //remove cover
           if (destPath != destCover && QFileInfo::exists(destCover))
               QFile::remove(destCover);

           //copy readme and cover
           if (! isMarkdown())
                QFile::copy(readMeFileName(), destReadme);
           if (! isImage())
                QFile::copy(coverFileName(), destCover);
           return true;
       } else
           return false;
    } else {
        return copyRecursively(getFullPath(), destPath);
    }
}

bool LocalFileBackend::rename(const QString &newName)
{
   if (QFile::rename(getFullPath(), newName))
   {
       if (m_fileInfo.isFile())
       {
           //find readme and cover
           QString destReadme = m_fileInfo.absolutePath() + "/" + QFileInfo(newName).baseName() + ".Readme.md";
           QString destCover = m_fileInfo.absolutePath() + "/" +  QFileInfo(newName).baseName() + ".cover.jpg";

           //remove readme
           if (QFileInfo::exists(destReadme))
               QFile::remove(destReadme);

           //remove cover
           if (QFileInfo::exists(destCover))
               QFile::remove(destCover);

           //copy readme and cover
           if (! isMarkdown())
                QFile::copy(readMeFileName(), destReadme);
           if (! isImage())
                QFile::copy(coverFileName(), destCover);
       }
      return true;
   } else
       return false;
}

bool LocalFileBackend::mkcol()
{
    return m_fileInfo.dir().mkpath(m_fileInfo.fileName());
}
