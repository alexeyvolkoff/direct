#ifndef IRESOURCEBACKEND_H
#define IRESOURCEBACKEND_H

#include <QObject>
#include <QIODevice>
#include <QHash>

class CloudResource;

class IResourceBackend : public QIODevice
{
    Q_OBJECT
public:
    explicit IResourceBackend(QObject *parent = nullptr);
    virtual QByteArray readFromPosition(qint64 from, qint64 maxlen) = 0;
    virtual bool RefreshInfo(CloudResource *resource) = 0;
    virtual bool RefreshAllInfo(CloudResource *resource) = 0;
    virtual QString storageID() = 0;
	virtual int fileHandle() = 0;
    virtual QByteArray bytesToCache()  const { return QByteArray(); }
    virtual void setRealPath(const QString& realPath) { Q_UNUSED(realPath); }
    virtual bool invoke(const QString& rpc,  const QHash<QString, QVariant>&params,  const QByteArray& payload )  { Q_UNUSED(rpc); Q_UNUSED(params); Q_UNUSED(payload); return false; }
    virtual void removePublicLink() {}
    virtual bool remove(bool forced = false) { Q_UNUSED(forced); return false; }
    virtual bool removeShare() { return false; }
    virtual QString getPublicLink(QHash<QString, QVariant> linkData) { Q_UNUSED(linkData); return ""; }
    virtual bool mkcol() { return false; }
    virtual bool rename(const QString& newName) { Q_UNUSED(newName); return false; }
    virtual bool move(const QString& destPath) { Q_UNUSED(destPath); return false; }
    virtual bool copy(const QString& destPath) { Q_UNUSED(destPath); return false; }

public slots:
signals:
    void dataReady(const QByteArray& data, int size);
    void responseReady(const QHash<QString, QVariant> &params, const QByteArray& payload);
    void listReady(const QHash<QString, QHash<QString, QVariant>> &entryList);

};

#include "cloudresource.h"
#endif // IRESOURCEBACKEND_H
