#include "webdavbackend.h"
#include <QEventLoop>
#include <QTimer>
#include <QUrl>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QXmlStreamReader>
#include <QMimeDatabase>
#include <QMutex>
#include <QMutexLocker>

namespace {
QMutex s_webdavUploadMutex;
QHash<QString, QByteArray> s_webdavUploadBuffers;
}

WebDAVBackend::WebDAVBackend(const QString &baseUrl, QObject *parent)
    : IResourceBackend(parent)
    , m_baseUrl(baseUrl)
    , m_size(0)
    , m_pos(0)
    , m_atEnd(false)
    , m_isOpen(false)
{
    m_storageID = QUuid::createUuid().toString();
    m_net = new QNetworkAccessManager(this);
    if (!m_baseUrl.endsWith("/"))
        m_baseUrl += "/";
}

WebDAVBackend::~WebDAVBackend()
{
}

void WebDAVBackend::setRemotePath(const QString &path)
{
    m_remotePath = path;
}

void WebDAVBackend::setBaseUrl(const QString &baseUrl)
{
    m_baseUrl = baseUrl;
    if (!m_baseUrl.endsWith("/"))
        m_baseUrl += "/";
}

QString WebDAVBackend::normalizePath(const QString &path) const
{
    if (path.isEmpty() || path == "/")
        return "/";
    QString p = path.startsWith("/") ? path : "/" + path;
    if (p.endsWith("/") && p.length() > 1)
        p.chop(1);
    return p;
}

QString WebDAVBackend::buildUrl(const QString &path) const
{
    QString p = normalizePath(path);
    if (!m_prefix.isEmpty() && p.startsWith(m_prefix)) {
        p = p.mid(m_prefix.length());
        p = normalizePath(p);
    }
    
    // Percent-encode each segment of the path so spaces and non-ASCII chars in filenames work cleanly over HTTP
    QStringList segments = p.split("/", Qt::SplitBehaviorFlags::SkipEmptyParts);
    QStringList encodedSegments;
    for (const QString &seg : segments) {
        encodedSegments.append(QUrl::toPercentEncoding(seg));
    }
    QString encodedPath = "/" + encodedSegments.join("/");
    if (p.endsWith("/") && !encodedPath.endsWith("/"))
        encodedPath += "/";

    QString url = m_baseUrl;
    if (url.endsWith("/") && encodedPath.startsWith("/"))
        url.chop(1);
    return url + encodedPath;
}

bool WebDAVBackend::isReachable()
{
    int status = -1;
    propfind("/", 0, &status);
    return status >= 200 && status < 300;
}

// ---- Low-level request ----

QByteArray WebDAVBackend::makeRequest(const QString &method, const QString &path,
                                      const QByteArray &body,
                                      const QHash<QString, QString> &extraHeaders,
                                      int *httpStatus, int timeoutMs)
{
    m_net->disconnect();
    QUrl url(buildUrl(path));
    QNetworkRequest request(url);
    request.setRawHeader("Accept", "*/*");

    for (auto it = extraHeaders.constBegin(); it != extraHeaders.constEnd(); ++it) {
        request.setRawHeader(it.key().toUtf8(), it.value().toUtf8());
    }

    QNetworkReply *reply = nullptr;
    if (method == "GET") {
        reply = m_net->get(request);
    } else if (method == "PUT") {
        request.setRawHeader("Content-Type", "application/octet-stream");
        reply = m_net->put(request, body);
    } else if (method == "DELETE") {
        reply = m_net->deleteResource(request);
    } else if (method == "MKCOL") {
        reply = m_net->sendCustomRequest(request, "MKCOL", body);
    } else if (method == "MOVE") {
        reply = m_net->sendCustomRequest(request, "MOVE", body);
    } else if (method == "COPY") {
        reply = m_net->sendCustomRequest(request, "COPY", body);
    } else if (method == "PROPFIND") {
        if (!request.hasRawHeader("Depth"))
            request.setRawHeader("Depth", "0");
        if (!request.hasRawHeader("Content-Type"))
            request.setRawHeader("Content-Type", "application/xml; charset=utf-8");
        reply = m_net->sendCustomRequest(request, "PROPFIND", body);
    } else {
        if (httpStatus) *httpStatus = -1;
        return QByteArray();
    }

    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    timer.start(timeoutMs);
    loop.exec();

    QByteArray responseData;
    int status = -1;
    if (timer.isActive()) {
        timer.stop();
        status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        responseData = reply->readAll();
    } else {
        reply->abort();
    }

    if (httpStatus) *httpStatus = status;
    reply->deleteLater();
    return responseData;
}

// ---- WebDAV methods ----

QByteArray WebDAVBackend::propfind(const QString &path, int depth, int *httpStatus)
{
    QHash<QString, QString> headers;
    headers["Depth"] = QString::number(depth);
    headers["Content-Type"] = "application/xml; charset=utf-8";
    QByteArray body = "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                      "<D:propfind xmlns:D=\"DAV:\">"
                      "<D:prop>"
                      "<D:displayname/>"
                      "<D:getcontentlength/>"
                      "<D:getlastmodified/>"
                      "<D:resourcetype/>"
                      "<D:getetag/>"
                      "<D:getcontenttype/>"
                      "</D:prop>"
                      "</D:propfind>";

    QHash<QString, QString> allHeaders;
    allHeaders["Depth"] = QString::number(depth);
    allHeaders["Content-Type"] = "application/xml; charset=utf-8";

    return makeRequest("PROPFIND", path, body, allHeaders, httpStatus);
}

QByteArray WebDAVBackend::httpGet(const QString &path, int *httpStatus)
{
    return makeRequest("GET", path, QByteArray(), QHash<QString, QString>(), httpStatus);
}

bool WebDAVBackend::httpPut(const QString &path, const QByteArray &data)
{
    int status = -1;
    makeRequest("PUT", path, data, QHash<QString, QString>(), &status);
    return status >= 200 && status < 300;
}

bool WebDAVBackend::httpDelete(const QString &path)
{
    int status = -1;
    makeRequest("DELETE", path, QByteArray(), QHash<QString, QString>(), &status);
    return status >= 200 && status < 300;
}

bool WebDAVBackend::httpMkcol(const QString &path)
{
    int status = -1;
    makeRequest("MKCOL", path, QByteArray(), QHash<QString, QString>(), &status);
    return status >= 200 && status < 300;
}

bool WebDAVBackend::httpMove(const QString &path, const QString &destination)
{
    QString destUrl = buildUrl(destination);
    QHash<QString, QString> headers;
    headers["Destination"] = destUrl;
    headers["Overwrite"] = "T";
    int status = -1;
    makeRequest("MOVE", path, QByteArray(), headers, &status);
    return status >= 200 && status < 300;
}

bool WebDAVBackend::httpCopy(const QString &path, const QString &destination)
{
    QString destUrl = buildUrl(destination);
    QHash<QString, QString> headers;
    headers["Destination"] = destUrl;
    headers["Overwrite"] = "T";
    int status = -1;
    makeRequest("COPY", path, QByteArray(), headers, &status);
    return status >= 200 && status < 300;
}

// ---- PROPFIND response parsing ----

QList<WebDAVBackend::WebDAVEntry> WebDAVBackend::parsePropfindResponse(const QByteArray &xml, const QString &dirPath)
{
    QList<WebDAVEntry> entries;
    QXmlStreamReader reader(xml);

    QString currentHref;
    WebDAVEntry current;
    bool insideProp = false;

    auto flushEntry = [&]() {
        if (!current.href.isEmpty()) {
            // Skip the directory itself (href == dirPath)
            QString hrefPath = QUrl(current.href).path();
            QString normalizedDir = normalizePath(dirPath);
            if (hrefPath == normalizedDir || hrefPath == normalizedDir + "/") {
                current = WebDAVEntry();
                return;
            }
            // Extract name from href (last path segment)
            if (current.name.isEmpty()) {
                QString path = QUrl(current.href).path();
                if (path.endsWith("/"))
                    path.chop(1);
                int lastSlash = path.lastIndexOf("/");
                current.name = (lastSlash >= 0) ? path.mid(lastSlash + 1) : path;
            }
            entries.append(current);
            current = WebDAVEntry();
        }
    };

    while (!reader.atEnd() && !reader.hasError()) {
        QXmlStreamReader::TokenType token = reader.readNext();
        if (token == QXmlStreamReader::StartElement) {
            QStringRef name = reader.name();
            QStringRef ns = reader.namespaceUri();

            if (ns == "DAV:" || ns == "D") {
                if (name == "response") {
                    flushEntry();
                    current = WebDAVEntry();
                } else if (name == "href") {
                    // same namespace as DAV:
                    current.href = reader.readElementText();
                } else if (name == "prop") {
                    insideProp = true;
                } else if (insideProp) {
                    if (name == "displayname") {
                        current.name = reader.readElementText();
                    } else if (name == "getcontentlength") {
                        current.size = reader.readElementText().toLongLong();
                    } else if (name == "getlastmodified") {
                        current.lastModified = QDateTime::fromString(
                            reader.readElementText(), Qt::RFC2822Date);
                    } else if (name == "getetag") {
                        current.etag = reader.readElementText();
                    } else if (name == "resourcetype") {
                        // Check for collection inside resourcetype
                        QXmlStreamReader::TokenType innerToken = reader.readNext();
                        while (!(innerToken == QXmlStreamReader::EndElement &&
                                 reader.name() == "resourcetype")) {
                            if (innerToken == QXmlStreamReader::StartElement) {
                                if (reader.name() == "collection")
                                    current.isDir = true;
                            }
                            innerToken = reader.readNext();
                        }
                        continue; // already consumed end of resourcetype
                    }
                }
            }
        } else if (token == QXmlStreamReader::EndElement) {
            QStringRef name = reader.name();
            QStringRef ns = reader.namespaceUri();
            if (ns == "DAV:" || ns == "D") {
                if (name == "prop") {
                    insideProp = false;
                } else if (name == "response") {
                    flushEntry();
                }
            }
        }
    }

    flushEntry();

    if (reader.hasError()) {
        qWarning() << "WebDAV: XML parse error:" << reader.errorString();
    }

    return entries;
}

// ---- Data conversion ----

QHash<QString, QVariant> WebDAVBackend::entryToResourceData(const WebDAVEntry &entry)
{
    QHash<QString, QVariant> data;
    data["exists"] = true;
    data["isReadable"] = true;
    data["isWritable"] = true;
    data["isDir"] = entry.isDir;
    data["isFile"] = !entry.isDir;
    data["size"] = entry.size;
    QString mime = "folder";
    if (!entry.isDir) {
        mime = QMimeDatabase().mimeTypeForFile(entry.name, QMimeDatabase::MatchExtension).name();
        if (mime.isEmpty())
            mime = "application/octet-stream";
    }
    data["contentType"] = mime;
    data["createdDate"] = entry.lastModified;
    data["lastModDate"] = entry.lastModified;
    data["lastModified"] = entry.lastModified.toString(Qt::ISODate);
    data["etag"] = entry.etag;

    QHash<QString, QVariant> customAttrs;
    customAttrs["webdavHref"] = entry.href;
    customAttrs["webdavETag"] = entry.etag;
    data["customAttributes"] = QVariant(customAttrs);

    return data;
}

QHash<QString, QHash<QString, QVariant>> WebDAVBackend::entryListToEntryData(const QList<WebDAVEntry> &entries)
{
    QHash<QString, QHash<QString, QVariant>> entryData;
    for (const WebDAVEntry &entry : entries) {
        if (!entry.name.isEmpty() && entry.name != "." && entry.name != "..") {
            entryData.insert(entry.name, entryToResourceData(entry));
        }
    }
    return entryData;
}

// ---- IResourceBackend interface ----

bool WebDAVBackend::RefreshInfo(CloudResource *resource)
{
    if (!resource)
        return false;

    QString path = m_remotePath;
    if (path.isEmpty())
        path = resource->remotePath();

    int status = -1;
    QByteArray response = propfind(path, 0, &status);
    if (status < 200 || status >= 300)
        return false;

    QList<WebDAVEntry> entries = parsePropfindResponse(response, path);
    if (entries.isEmpty())
        return false;

    // First entry is the resource itself
    const WebDAVEntry &entry = entries.first();
    m_size = entry.size;
    m_storageID = QString("webdav-%1").arg(entry.etag.isEmpty() ? path : entry.etag);

    QHash<QString, QVariant> data = entryToResourceData(entry);
    resource->LoadData(data);
    return true;
}

bool WebDAVBackend::RefreshAllInfo(CloudResource *resource)
{
    if (!resource)
        return false;

    QString path = m_remotePath;
    if (path.isEmpty())
        path = resource->remotePath();

    int status = -1;
    QByteArray response = propfind(path, 1, &status);
    qDebug() << "WebDAV RefreshAllInfo path:" << path << "status:" << status;
    if (status < 200 || status >= 300) {
        qDebug() << "WebDAV propfind failed:" << response;
        return false;
    }

    QList<WebDAVEntry> entries = parsePropfindResponse(response, path);
    qDebug() << "WebDAV parsed entries:" << entries.size();
    if (entries.isEmpty())
        return false;

    // First entry is the resource itself
    const WebDAVEntry &selfEntry = entries.first();
    m_size = selfEntry.size;
    m_storageID = QString("webdav-%1").arg(selfEntry.etag.isEmpty() ? path : selfEntry.etag);

    QHash<QString, QVariant> data = entryToResourceData(selfEntry);
    resource->LoadData(data);

    // Remaining entries are children (for Depth: 1)
    if (entries.size() > 1) {
        QList<WebDAVEntry> children = entries.mid(1);
        QHash<QString, QHash<QString, QVariant>> entryData = entryListToEntryData(children);
        resource->LoadEntryData(entryData);
    }

    return true;
}

QByteArray WebDAVBackend::readFromPosition(qint64 from, qint64 maxlen)
{
    if (maxlen <= 0 || from < 0)
        return QByteArray();

    QHash<QString, QString> headers;
    headers["Range"] = QString("bytes=%1-%2").arg(from).arg(from + maxlen - 1);

    int status = -1;
    QByteArray data = makeRequest("GET", m_remotePath, QByteArray(), headers, &status);
    if (status == 206) {
        return data.size() > maxlen ? data.left(maxlen) : data;
    }
    if (status < 200 || status >= 300)
        return QByteArray();

    if (from >= data.size())
        return QByteArray();

    qint64 end = qMin(from + maxlen, static_cast<qint64>(data.size()));
    return data.mid(from, end - from);
}

bool WebDAVBackend::remove(bool forced)
{
    Q_UNUSED(forced);
    return httpDelete(m_remotePath);
}

bool WebDAVBackend::mkcol()
{
    return httpMkcol(m_remotePath);
}

bool WebDAVBackend::rename(const QString &newName)
{
    if (newName.isEmpty())
        return false;

    QString parentPath = m_remotePath;
    int lastSlash = parentPath.lastIndexOf("/");
    if (lastSlash > 0)
        parentPath = parentPath.left(lastSlash);
    else
        parentPath = "/";

    QString destPath = parentPath + "/" + newName;
    return httpMove(m_remotePath, destPath);
}

bool WebDAVBackend::move(const QString &destPath)
{
    return httpMove(m_remotePath, destPath);
}

bool WebDAVBackend::copy(const QString &destPath)
{
    return httpCopy(m_remotePath, destPath);
}

// ---- QIODevice interface ----

bool WebDAVBackend::isSequential() const
{
    return false;
}

bool WebDAVBackend::open(OpenMode mode)
{
    if (m_isOpen)
        return false;

    if ((mode & QIODevice::WriteOnly) && (mode & QIODevice::Truncate)) {
        QMutexLocker locker(&s_webdavUploadMutex);
        s_webdavUploadBuffers.remove(buildUrl(m_remotePath));
        m_bytesToCache.clear();
    }

    m_isOpen = true;
    m_pos = 0;
    m_atEnd = false;
    m_lastWriteOk = true;
    return IResourceBackend::open(mode);
}

void WebDAVBackend::close()
{
    if (m_isOpen && (openMode() & QIODevice::WriteOnly)) {
        const QString urlKey = buildUrl(m_remotePath);
        if (m_writeEof) {
            QByteArray payload;
            {
                QMutexLocker locker(&s_webdavUploadMutex);
                if (s_webdavUploadBuffers.contains(urlKey)) {
                    payload = s_webdavUploadBuffers.take(urlKey);
                } else {
                    payload = m_bytesToCache;
                }
            }
            m_lastWriteOk = httpPut(m_remotePath, payload);
            if (!m_lastWriteOk)
                qWarning() << "WebDAV: PUT failed on close for" << m_remotePath;
        }
    }
    m_bytesToCache.clear();
    m_writeEof = true;
    m_isOpen = false;
    m_pos = 0;
    m_atEnd = false;
    IResourceBackend::close();
}

bool WebDAVBackend::seek(qint64 pos)
{
    if (!m_isOpen)
        return false;
    m_pos = pos;
    m_atEnd = false;
    return true;
}

bool WebDAVBackend::atEnd() const
{
    return m_atEnd || m_pos >= m_size;
}

qint64 WebDAVBackend::size() const
{
    return m_size;
}

qint64 WebDAVBackend::readData(char *data, qint64 maxlen)
{
    if (m_atEnd || m_pos >= m_size)
        return -1;

    QByteArray chunk = readFromPosition(m_pos, maxlen);
    if (chunk.isEmpty()) {
        m_atEnd = true;
        return -1;
    }

    qint64 bytesRead = qMin(static_cast<qint64>(chunk.size()), maxlen);
    memcpy(data, chunk.constData(), bytesRead);
    m_pos += bytesRead;

    if (m_pos >= m_size)
        m_atEnd = true;

    return bytesRead;
}

qint64 WebDAVBackend::writeData(const char *data, qint64 len)
{
    const QString urlKey = buildUrl(m_remotePath);
    {
        QMutexLocker locker(&s_webdavUploadMutex);
        QByteArray &buf = s_webdavUploadBuffers[urlKey];
        if (m_pos == buf.size()) {
            buf.append(data, static_cast<int>(len));
        } else {
            if (m_pos + len > buf.size())
                buf.resize(static_cast<int>(m_pos + len));
            if (len > 0)
                memcpy(buf.data() + m_pos, data, static_cast<size_t>(len));
        }
        m_size = buf.size();
    }
    m_pos += len;
    return len;
}

QString WebDAVBackend::storageID()
{
    if (m_storageID.isEmpty())
        m_storageID = QUuid::createUuid().toString();
    return m_storageID;
}
