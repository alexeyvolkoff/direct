#ifndef LINKBACKEND_H
#define LINKBACKEND_H

#include <QObject>
#include <QPointer>
#include <QIODevice>
#include "cloudresource.h"
#include "linksocket.h"
#include "linkmessage.h"
#include "timelogger.h"
#include <QThread>
#include <iresourcebackend.h>


class LinkBackend : public IResourceBackend
{
    Q_OBJECT
public:
    explicit LinkBackend(QString LinkID, QString remotePath, QString query, qint64 size = 0, QObject *parent = nullptr);
    ~LinkBackend();
	bool RefreshInfo(CloudResource *resource);
	bool RefreshAllInfo(CloudResource *resource);
    bool remove(bool forced = false);
    bool removeShare();
    bool mkcol();    
    bool rename(const QString& newName);
    bool move(const QString& destPath);
    bool copy(const QString& destPath);
    QByteArray readFromPosition(qint64 from, qint64 maxlen);
    QString getPublicLink(QHash<QString, QVariant> linkData);
    void removePublicLink();
	int fileHandle() { return m_fh; }
    bool invoke(const QString& rpc, const QHash<QString, QVariant> &params,  const QByteArray &payload );

private:
    LinkMessage sendMessage( LinkMessage request);
    void writeMessage(LinkMessage request);
    QString m_remotePath; //sent to client
    QHash<QString, QString> m_parsedQuery;
    QString m_query;
    QString m_contentType;
    QByteArray m_bytesToCache;
    QPointer<LinkSocket> m_socket;

    bool m_atEnd = false;
    QString m_resourceID;
    QString m_storageID;
    qint64 m_size;
	qint64 m_fh;
    qint64 m_from;
    bool m_isStreaming = false;
    QMetaObject::Connection m_rpcConnection;


public slots:

    // QIODevice interface
public:
    bool isSequential() const;
    bool open(OpenMode mode);
    bool isOpen() const;
    void close();
    bool seek(qint64 pos);
    bool atEnd() const;

    LinkSocket* linkSocket();

protected:
    qint64 readData(char *data, qint64 maxlen);
    qint64 writeData(const char *data, qint64 len);

public:
    qint64 size() const;

public:
    QString storageID();
    QString contentType() const;
    QByteArray bytesToCache() const;
    void setRealPath(const QString& realPath) { m_remotePath = realPath; }
};

#endif // LINKBACKEND_H
