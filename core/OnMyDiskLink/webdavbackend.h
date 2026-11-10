#ifndef WEBDAVBACKEND_H
#define WEBDAVBACKEND_H

#include <QObject>
#include <QIODevice>
#include <QHash>
#include <QByteArray>
#include <QString>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QUrlQuery>
#include <QUuid>
#include <QDateTime>
#include "cloudresource.h"
#include "iresourcebackend.h"

class WebDAVBackend : public IResourceBackend
{
    Q_OBJECT
public:
    explicit WebDAVBackend(const QString &baseUrl, QObject *parent = nullptr);
    ~WebDAVBackend();

    // IResourceBackend interface
    bool RefreshInfo(CloudResource *resource);
    bool RefreshAllInfo(CloudResource *resource);
    QByteArray readFromPosition(qint64 from, qint64 maxlen);
    QString storageID();
    int fileHandle() { return 0; }
    QByteArray bytesToCache() const { return m_bytesToCache; }

    bool remove(bool forced = false);
    bool mkcol();
    bool rename(const QString &newName);
    bool move(const QString &destPath);
    bool copy(const QString &destPath);

    // QIODevice interface
    bool isSequential() const;
    bool open(OpenMode mode);
    void close();
    bool seek(qint64 pos);
    bool atEnd() const;
    qint64 size() const;

    void setRemotePath(const QString &path);
    void setBaseUrl(const QString &baseUrl);
    void setPrefix(const QString &prefix) { m_prefix = normalizePath(prefix); }
    QString prefix() const { return m_prefix; }
    QString baseUrl() const { return m_baseUrl; }
    bool isReachable();
    void setWriteEof(bool eof) { m_writeEof = eof; }
    bool lastWriteOk() const { return m_lastWriteOk; }

protected:
    qint64 readData(char *data, qint64 maxlen);
    qint64 writeData(const char *data, qint64 len);

private:
    // WebDAV HTTP methods
    QByteArray propfind(const QString &path, int depth, int *httpStatus = nullptr);
    QByteArray httpGet(const QString &path, int *httpStatus = nullptr);
    bool httpPut(const QString &path, const QByteArray &data);
    bool httpDelete(const QString &path);
    bool httpMkcol(const QString &path);
    bool httpMove(const QString &path, const QString &destination);
    bool httpCopy(const QString &path, const QString &destination);

    // Low-level request helper
    QByteArray makeRequest(const QString &method, const QString &path,
                           const QByteArray &body = QByteArray(),
                           const QHash<QString, QString> &extraHeaders = QHash<QString, QString>(),
                           int *httpStatus = nullptr,
                           int timeoutMs = 30000);

    // Response parsing
    struct WebDAVEntry {
        QString href;
        QString name;
        qint64 size = 0;
        QDateTime lastModified;
        bool isDir = false;
        QString etag;
    };
    QList<WebDAVEntry> parsePropfindResponse(const QByteArray &xml, const QString &dirPath);

    // Convert parsed entries to CloudResource data
    QHash<QString, QVariant> entryToResourceData(const WebDAVEntry &entry);
    QHash<QString, QHash<QString, QVariant>> entryListToEntryData(const QList<WebDAVEntry> &entries);

    // Path helpers
    QString normalizePath(const QString &path) const;
    QString buildUrl(const QString &path) const;

    // State
    QString m_baseUrl;
    QString m_remotePath;
    QString m_storageID;
    QByteArray m_bytesToCache;
    QString m_prefix;
    qint64 m_size;
    qint64 m_pos;
    bool m_atEnd;
    bool m_isOpen;
    bool m_writeEof = true;
    bool m_lastWriteOk = true;

    // Network
    QNetworkAccessManager *m_net;
};

#endif // WEBDAVBACKEND_H
