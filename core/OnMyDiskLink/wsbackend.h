#ifndef WSBACKEND_H
#define WSBACKEND_H

#include <QObject>
#include <QIODevice>
#include "cloudresource.h"
#include "linksocket.h"
#include "linkmessage.h"
#include "timelogger.h"
#include <QThread>
#include "clientnotifier.h"
#include <QPointer>
#include <iresourcebackend.h>


class WebSocketBackend : public IResourceBackend
{
    Q_OBJECT
public:
    explicit WebSocketBackend(QSharedPointer<ClientNotifier> notifier, QString remotePath, QString query, qint64 size = 0, QObject *parent = nullptr);
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
    bool invoke(const QString& rpc, QHash<QString, QVariant> &params,  QByteArray &payload );

private:
    QString m_remotePath; //sent to client
    QHash<QString, QString> m_parsedQuery;
    QString m_query;
    QString m_linkID;
    QString m_contentType;
    QByteArray m_bytesToCache;
    QSharedPointer<ClientNotifier> m_notifier;

    bool m_atEnd = false;
    QString m_resourceID;
    QString m_storageID;
    qint64 m_size;
	qint64 m_fh;
    qint64 m_from;

public slots:

    // QIODevice interface
public:
    bool isSequential() const;
    bool open(OpenMode mode);
    bool isOpen() const;
    void close();
    bool seek(qint64 pos);
    bool atEnd() const;

protected:
    qint64 readData(char *data, qint64 maxlen);
    qint64 writeData(const char *data, qint64 len);

    // QIODevice interface
public:
    qint64 size() const;

public:
    QString storageID();
    QString contentType() const;
    QByteArray bytesToCache() const;
    void setRealPath(const QString& realPath) { m_remotePath = realPath; }
    void setLinkID(const QString& linkID) { m_linkID = linkID; }

    //converter to internal data
    static QHash<QString, QVariant> jsonToData(const QJsonObject& obj);
    static QHash<QString, QHash<QString, QVariant>> jsonToEntryData(const QString& keyName, const QJsonArray& entries);
    static QJsonObject dataToJson(QHash<QString, QVariant> data);
    static QByteArray jsonToArray(const QJsonArray& jsonArray);
};

#endif // WSBACKEND_H
