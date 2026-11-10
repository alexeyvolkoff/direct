#include "memfilebackend.h"


MemFileBackend::MemFileBackend(QObject *parent) : IResourceBackend(parent)
{
    m_buffer.setBuffer(&m_data);
	m_cacheFileId = "";
	m_storageID = QString("RAM_%1").arg(QUuid::createUuid().toString());
}

MemFileBackend::MemFileBackend(QByteArray data, QString contentType, QObject *parent) : IResourceBackend(parent)
{
    m_data = data;    
    m_contentType = contentType;
	m_cacheFileId = "";
    m_buffer.setBuffer(&m_data);
	m_storageID = QString("RAM_%1").arg(QUuid::createUuid().toString());
}

MemFileBackend::MemFileBackend(const QString &cacheId, const QString &contentType, QObject *parent) : IResourceBackend(parent)
{
	m_contentType = contentType;
	m_cacheFileId = cacheId;
	m_storageID = QString("CACHE_%1").arg(QUuid::createUuid().toString());
}

MemFileBackend::~MemFileBackend()
{
    if (m_buffer.isOpen())
        m_buffer.close();

    m_data.clear();
    //m_data.squeeze();
}

QString MemFileBackend::getContentType() const
{
    return m_contentType;
}

void MemFileBackend::setContentType(const QString &contentType)
{
    m_contentType = contentType;
}

bool MemFileBackend::isSequential() const
{
    return false;
}

void MemFileBackend::close()
{
	m_buffer.close();
}

bool MemFileBackend::seek(qint64 pos)
{
    if (! m_buffer.isOpen())
        m_buffer.open(QIODevice::ReadOnly);
    return m_buffer.seek(pos);
}

bool MemFileBackend::atEnd() const
{
    return m_buffer.atEnd();
}

bool MemFileBackend::resize(qint64 sz)
{
    m_data.resize(sz);
    return true;
}

QByteArray MemFileBackend::getBytes() const
{
    return m_data;
}


qint64 MemFileBackend::readData(char *data, qint64 maxlen)
{
    if (! m_buffer.isOpen())
            m_buffer.open(QIODevice::ReadOnly);

    QByteArray byteData = m_buffer.read(maxlen);
    //qInfo() << "read " << byteData.size() << " bytes from memory cache";
    memcpy(data, byteData.constData(), byteData.size());
    return byteData.size();
}

qint64 MemFileBackend::writeData(const char *data, qint64 len)
{
	//qInfo() << "Writing" << len << "bytes to cache";
    if (! m_buffer.isOpen())
        m_buffer.open(QIODevice::ReadWrite);
    return m_buffer.write(data, len);
}

QByteArray MemFileBackend::readFromPosition(qint64 from, qint64 maxlen)
{
    QByteArray data;
    if (! m_buffer.isOpen())
            m_buffer.open(QIODevice::ReadOnly);

    if (!m_buffer.seek(from))
        return data;

    return m_buffer.read(maxlen);
}


bool MemFileBackend::open(QIODevice::OpenMode mode)
{
    if (m_buffer.isOpen())
        m_buffer.close();
    if (isOpen())
        QIODevice::close();

    if (QIODevice::open(mode))
    {
        if (m_buffer.open(mode))
            return true;
        else
        {
            QIODevice::close();
            return false;
        }
    }
    return false;
}

bool MemFileBackend::RefreshInfo(CloudResource *resource)
{
    if (resource)
    {
        QHash<QString, QVariant> resourceData;
        resourceData.insert("size", m_data.size());
        resourceData.insert("bytesAvailable", m_data.size());
        resourceData.insert("bytesTotal", m_data.size());
        resourceData.insert("isFile", true);
        resourceData.insert("isDir", false);
        resourceData.insert("isReadable", true);
        resourceData.insert("exists", true);
        resourceData.insert("contentType", m_contentType);
        resource->LoadData(resourceData);
        return true;
    }
    return false;
}

bool MemFileBackend::RefreshAllInfo(CloudResource *resource)
{
    return RefreshInfo(resource);
}



qint64 MemFileBackend::size() const
{
	return m_data.size();
}

QString MemFileBackend::storageID()
{
    return m_storageID;
}
