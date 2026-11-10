#ifndef MEMFILEBACKEND_H
#define MEMFILEBACKEND_H

#include <QObject>
#include <QFileDevice>
#include <QBuffer>
#include <QFileInfo>
#include <QUuid>
#include <iresourcebackend.h>
#include "cloudresource.h"

class  MemFileBackend : public IResourceBackend
{
    Q_OBJECT
public:
    explicit MemFileBackend(QObject *parent = nullptr);
    explicit MemFileBackend(QByteArray data, QString contentType, QObject *parent = nullptr);
	explicit MemFileBackend(const QString& cacheId, const QString& contentType, QObject *parent = nullptr);
	~MemFileBackend();
signals:

public slots:

private:
    QByteArray m_data;
    QBuffer m_buffer;
    QString m_storageID;
    QString m_contentType;
	QString m_cacheFileId;

    // QIODevice interface
public:
    bool isSequential() const;
    void close();
    bool seek(qint64 pos);
    bool atEnd() const;    
    // QFileDevice interface
public:
    bool resize(qint64 sz);
    QByteArray getBytes() const;


    // QIODevice interface
protected:
    qint64 readData(char *data, qint64 maxlen);
    qint64 writeData(const char *data, qint64 len);

    // LinkIODevice interface
public:
    QByteArray readFromPosition(qint64 from, qint64 maxlen);

    // QIODevice interface
public:
    bool open(OpenMode mode);

    // IResourceBackend interface
public:
    bool RefreshInfo(CloudResource *resource);
    bool RefreshAllInfo(CloudResource *resource);
    int fileHandle() { return 1; }
    // QIODevice interface
public:
    qint64 size() const;

    // IResourceBackend interface
public:
    QString storageID();
    void setContentType(const QString &contentType);
    QString getContentType() const;
};

#endif // MEMFILEBACKEND_H
