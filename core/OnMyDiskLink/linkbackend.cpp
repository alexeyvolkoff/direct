#include "linkbackend.h"
#ifdef SERVER
#include "onmydisklinkserver.h"
#include "onmydisklink.h"
#endif
#include <memory>


LinkBackend::LinkBackend(QString LinkID, QString remotePath, QString query, qint64 size, QObject *parent)
    : IResourceBackend(parent), m_remotePath(remotePath)
{
    m_size = size;
    m_resourceID = QUuid::createUuid().toString();
    m_storageID = LinkID;
    m_query = query;
	m_fh = 0;
    m_from = 0;

    if (!query.isNull() && query.isEmpty()) {
        QStringList queryPairs = query.split('&', Qt::SplitBehaviorFlags::SkipEmptyParts);
        Q_FOREACH(QString queryPair, queryPairs)
        {
            QStringList keyValue = queryPair.split('=', Qt::SplitBehaviorFlags::SkipEmptyParts);
            if (keyValue.size() == 2)
                m_parsedQuery.insert(keyValue.at(0), keyValue.at(1));
            else
                m_parsedQuery.insert(keyValue.at(0), "true");
        }
    }
}

LinkBackend::~LinkBackend()
{
    QObject::disconnect(m_rpcConnection);
    m_socket = nullptr;
}

LinkSocket *LinkBackend::linkSocket()
{
#ifdef SERVER
    //Look the link up through the gateway registry (single-socket links)
    if (m_socket.isNull() || (m_socket != nullptr && m_socket->state() != QAbstractSocket::ConnectedState)) {
        QPointer<OnMyDiskLink> link = OnMyDiskLinkServer::instance().getLink(m_storageID);
        if (!link.isNull())
            m_socket = link->linkSocket();
        else
            m_socket = nullptr;
    }
    return m_socket;
#else
    //Node-side builds never serve remote resources over the legacy link plane
    return nullptr;
#endif
}

bool LinkBackend::RefreshInfo(CloudResource *resource)
{
    bool isOk = false;

    if (!resource)
        return isOk;

    m_socket = linkSocket();
    if (m_socket.isNull()) {
        return false;
    }

    LinkMessage message;
    message.setMessageType(LinkMessage::FileInfo);
    message.insertDataValue("ID", m_resourceID);
    message.insertDataValue("fullpath", m_remotePath);

    if (m_query.contains("fromPos"))
        message.insertDataValue("query", "");
    else
        message.insertDataValue("query", m_query);

    message.insertDataValue("children", false);

    LinkMessage reply = sendMessage(message);

    if (reply.getResultCode() == LinkMessage::OK)
    {
        QHash<QString, QVariant> resourceData = reply.getData();
        m_size = resourceData["size"].toLongLong();
        m_storageID = resourceData["storageID"].toString();
        m_contentType = resourceData["contentType"].toString();
        if (resourceData.contains("bytesToCache") && reply.getData()["bytesToCache"].toByteArray().size() > 0)
        {
            m_bytesToCache = reply.getData()["bytesToCache"].toByteArray();
            resourceData["isDir"] = false;
            resourceData["isFile"] = true;
        }

        resource->LoadData(resourceData);
        isOk = true;
    }
    m_socket = nullptr;
    reply.clear();
    return isOk;
}

bool LinkBackend::RefreshAllInfo(CloudResource *resource)
{
    bool isOk = false;

    if (!resource)
        return isOk;

    m_socket = linkSocket();
    if (m_socket.isNull()) {
        return false;
    }

    LinkMessage message;
    message.setMessageType(LinkMessage::FileInfo);
    message.insertDataValue("ID", m_resourceID);
    message.insertDataValue("fullpath", m_remotePath);

    if (m_query.contains("fromPos"))
        message.insertDataValue("query", "");
    else
        message.insertDataValue("query", m_query);

    message.insertDataValue("children", true);

    LinkMessage reply = sendMessage(message);

    if (reply.getResultCode() == LinkMessage::OK)
    {
        QHash<QString, QVariant> resourceData = reply.getData();
        m_size = resourceData["size"].toLongLong();
        m_storageID = resourceData["storageID"].toString();
        m_contentType = resourceData["contentType"].toString();
        if (resourceData.contains("bytesToCache"))
        {
            m_bytesToCache = reply.getData()["bytesToCache"].toByteArray();
            if (m_bytesToCache.size() > 0)
            {
                resourceData["isDir"] = false;
                resourceData["isFile"] = true;
            }
        }

        resource->LoadData(resourceData);
        if (resource->isDir())
            resource->LoadEntryData(reply.getEntryData());
        isOk = true;
    }
    m_socket = nullptr;
    reply.clear();
    return isOk;
}

bool LinkBackend::remove(bool forced)
{
   // TimeLogger logger("Remove for " + m_remotePath);
    m_socket = linkSocket();
    if (m_socket.isNull()) {
        qWarning() << "LinkBackend::remove: No socket. Invalid thread";
        return  false;
    }

    LinkMessage message;
    message.setMessageType(LinkMessage::Remove);
    message.insertDataValue("ID", m_resourceID);
    message.insertDataValue("filePath", m_remotePath);
    message.insertDataValue("forced", forced);

    LinkMessage reply = sendMessage(message);
    m_socket = nullptr;

    if (reply.getResultCode() == LinkMessage::LinkError)
    {
        qCritical() << "Remove error on link" << m_storageID;
        return false;
    }
    return reply.getResultCode() == LinkMessage::OK;;
}

bool LinkBackend::removeShare()
{
    m_socket = linkSocket();
    if (m_socket.isNull()) {
        qWarning() << "LinkBackend::removeShare: No socket. Invalid thread";
        return  false;
    }

    LinkMessage message;
    message.setMessageType(LinkMessage::RemoveShare);
    message.insertDataValue("ID", m_resourceID);
    message.insertDataValue("filePath", m_remotePath);

    LinkMessage reply = sendMessage(message);
    m_socket = nullptr;

    if (reply.getResultCode() == LinkMessage::LinkError)
    {
        qCritical() << "RemoveShare error on link" << m_storageID;
        return false;
    }
    return reply.getResultCode() == LinkMessage::OK;
}

bool LinkBackend::mkcol()
{
   // TimeLogger logger("MkCol for " + m_remotePath);
    m_socket = linkSocket();
    if (m_socket.isNull()) {
        qWarning() << "LinkBackend::mkcol: No socket. Invalid thread";
        return  false;
    }

    LinkMessage message;
    message.setMessageType(LinkMessage::MkDir);
    message.insertDataValue("ID", m_resourceID);
    message.insertDataValue("filePath", m_remotePath);

    LinkMessage reply = sendMessage(message);

    m_socket = nullptr;
    return reply.getResultCode() == LinkMessage::OK;
}

bool LinkBackend::rename(const QString &newName)
{
    m_socket = linkSocket();
    if (m_socket.isNull()) {
        qWarning() << "LinkBackend::rename: No socket. Invalid thread";
        return false;
    }
    LinkMessage message;
    message.setMessageType(LinkMessage::Rename);
    message.insertDataValue("ID", m_resourceID);
    message.insertDataValue("filePath", m_remotePath);
    message.insertDataValue("newName", newName);

    LinkMessage reply = sendMessage(message);

    m_socket = nullptr;

    if (reply.getResultCode() == LinkMessage::LinkError)
    {
        qCritical() << "Rename error on link" << m_storageID;
        return false;
    }
    return true;
}

bool LinkBackend::move(const QString &destPath)
{
    m_socket = linkSocket();
    if (m_socket.isNull()) {
        qWarning() << "LinkBackend::move: No socket. Invalid thread";
        return  false;
    }

    LinkMessage message;
    message.setMessageType(LinkMessage::Move);
    message.insertDataValue("ID", m_resourceID);
    message.insertDataValue("filePath", m_remotePath);
    message.insertDataValue("destPath", destPath);

    LinkMessage reply = sendMessage(message);

    m_socket = nullptr;
    if (reply.getResultCode() == LinkMessage::LinkError)
    {
        qCritical() << "Move error on link" << m_storageID;
        return false;
    }

    return true;

}

bool LinkBackend::copy(const QString &destPath)
{
    m_socket = linkSocket();
    if (m_socket.isNull()) {
        qWarning() << "LinkBackend::copy: No socket. Invalid thread";
        return  false;
    }
    LinkMessage message;
    message.setMessageType(LinkMessage::Copy);
    message.insertDataValue("ID", m_resourceID);
    message.insertDataValue("filePath", m_remotePath);
    message.insertDataValue("destPath", destPath);

    LinkMessage reply = sendMessage(message);

    m_socket = nullptr;

    if (reply.getResultCode() == LinkMessage::LinkError)
    {
        qCritical() << "Copy error on link" << m_storageID;
        return false;
    }
    return true;
}

QString LinkBackend::getPublicLink(QHash<QString, QVariant> linkData)
{
   // TimeLogger logger("GetLink for " + m_remotePath);
    m_socket = linkSocket();
    if (m_socket.isNull()) {
        qWarning() << "LinkBackend::getPublicLink: No socket. Invalid thread";
        return  "";
    }
    LinkMessage message;
    message.setMessageType(LinkMessage::GetPublicLink);
    message.insertDataValue("ID", m_resourceID);
    message.insertDataValue("filePath", m_remotePath);
    message.insertDataValue("linkData", linkData);

    LinkMessage reply = sendMessage(message);

    if (reply.getResultCode() == LinkMessage::OK)
    {
		//m_lastData = reply.getData();
		return reply.getData()["link"].toString();
    } else
    {
        qDebug() << reply.getResultCode() << reply.getErrorMsg();
        return "";
    }
}


void LinkBackend::removePublicLink()
{
   // TimeLogger logger("RemoveLink for " + m_remotePath);

    m_socket = linkSocket();
    if (m_socket.isNull()) {
        qWarning() << "LinkBackend::removePublicLink: No socket. Invalid thread";
        return;
    }
    LinkMessage message;
    message.setMessageType(LinkMessage::RemovePublicLink);
    message.insertDataValue("ID", m_resourceID);
    message.insertDataValue("filePath", m_remotePath);

    LinkMessage reply = sendMessage(message);
}

bool LinkBackend::invoke(const QString &rpc,  const QHash<QString, QVariant> &params,  const QByteArray &payload)
{
    m_socket = linkSocket();
    if (m_socket.isNull()) {
        qWarning() << "LinkBackend::invoke: No socket. Invalid thread";
        return false;
    }
    LinkMessage message;
    message.setMessageType(LinkMessage::RPC);
    message.insertDataValue("ID", m_resourceID);
    message.insertDataValue("filePath", m_remotePath);
    message.insertDataValue("rpc", rpc);
    message.insertDataValue("params", params);
    message.setPayLoad(payload);


    m_rpcConnection = connect(m_socket, &LinkSocket::newMessage, this, [this] (LinkSocket *socket, const LinkMessage& reply) {
        if (socket == nullptr || reply.getMessageType() != LinkMessage::RPC) {
            return;
        }

        QHash<QString, QVariant>  replyParams = reply.getDataValue("params").toHash();
        bool isStreaming = replyParams.contains("stream") && replyParams["stream"].toBool();
        QByteArray replyPayload = reply.getPayLoad();
        emit responseReady(replyParams, replyPayload);
        if (!isStreaming) {
            m_isStreaming = false;
            socket->releaseSocket();
            QObject::disconnect(m_rpcConnection);
        }
    });

    m_isStreaming = true;
    m_socket->sendAsyncMessage(message);
    return true;
}





/*QHash<QString, QHash<QString, QVariant> > LinkBackend::lastEntryData() const
{
    return m_lastEntryData;
}

QHash<QString, QVariant> LinkBackend::lastData() const
{
    return m_lastData;
}*/

bool LinkBackend::isSequential() const
{
    return false;
}

bool LinkBackend::open(QIODevice::OpenMode mode)
{
  //  TimeLogger logger("Open for " + m_remotePath);
    m_socket = linkSocket();
    if (m_socket.isNull()) {
        qWarning() << "LinkBackend::open: No socket. Invalid thread";
        return false;
    }


    LinkMessage message;
    message.setMessageType(LinkMessage::FileOpen);
    message.insertDataValue("ID", m_resourceID);
    message.insertDataValue("filePath", m_remotePath);
    message.insertDataValue("query", m_query);

    if ((mode & QIODevice::ReadOnly) == QIODevice::ReadOnly)
        message.insertDataValue("read", true);
    if ((mode & QIODevice::WriteOnly) == QIODevice::WriteOnly)
        message.insertDataValue("write", true);
    if ((mode & QIODevice::Append) == QIODevice::Append)
        message.insertDataValue("append", true);
    if ((mode & QIODevice::Truncate) == QIODevice::Truncate)
        message.insertDataValue("truncate", true);

    //qDebug() << "Open mode:" << mode;

    LinkMessage reply = sendMessage(message);

    if (reply.getResultCode() != LinkMessage::LinkError)
    {
		if ( !  QIODevice::open(mode) )
			qDebug() << "Can not open in mode:" << mode;
        if (reply.getData().contains("fh"))
            m_fh = reply.getData()["fh"].toLongLong();
        else
            m_fh = -1;
    }
    else
    {
        qDebug() << "Cannot open:" << reply.getResultCode();
        qDebug() << "Error:" << reply.getErrorMsg();
		m_fh = -1;
    }

    return (m_fh != -1);
}

bool LinkBackend::seek(qint64 pos)
{
   //TimeLogger logger("Seek for " + m_remotePath);
    //qInfo() << "Seek " << m_pathPrefix + m_remotePath << " to " << pos;

    if (m_socket.isNull()) {
        qWarning() << "LinkBackend::seek: No socket. Invalid thread";
        return false;
    }

    LinkMessage message;
    message.setMessageType(LinkMessage::FileSeek);
    message.insertDataValue("ID", m_resourceID);
    message.insertDataValue("pos", pos);
    message.insertDataValue("fh", m_fh);

    LinkMessage reply = sendMessage(message);

    if (reply.getResultCode() == LinkMessage::LinkError)
        return false;
        //qCritical() << "seek error";

    if (reply.getData().keys().contains("atEnd"))
        m_atEnd = reply.getData()["atEnd"].toBool();

    if (reply.getData().keys().contains("pos"))
    {
        bool isOk = false;
        qint64 pos  = reply.getData()["pos"].toLongLong(&isOk);
        if (isOk)
            QIODevice::seek(pos);
    }

    return reply.getResultCode() == LinkMessage::OK;
}

bool LinkBackend::isOpen() const
{
     return (m_fh > 0) && (!m_socket.isNull() && m_socket->isValid() && m_socket->state() == QAbstractSocket::ConnectedState );
}

void LinkBackend::close()
{
  //  TimeLogger logger("Close for " + m_remotePath);
    if (m_socket.isNull()) {
        //qWarning() << "LinkBackend::close: No socket. Invalid thread";
        return;
    }
    QIODevice::close();

    QObject::disconnect(m_rpcConnection);

    if (m_fh != 0) {
        LinkMessage message;
        message.setMessageType(LinkMessage::FileClose);
        message.insertDataValue("ID", m_resourceID);
        message.insertDataValue("fh", m_fh);
        sendMessage(message);
        m_fh = 0;
    }

    m_socket->releaseSocket();
    m_isStreaming = false;
    m_socket = nullptr;

    m_from = 0;

}


QByteArray LinkBackend::readFromPosition(qint64 from, qint64 maxlen)
{
    //TimeLogger logger("Read from for " + m_remotePath);
    //qInfo() << "Read from pos" << maxlen << " bytes from" << from ;
    QByteArray payload;

    if (m_socket.isNull()) {
        qWarning() << "LinkBackend::readFromPosition: No socket. Invalid thread";
        return payload;
    }


    LinkMessage message;
    message.setMessageType(LinkMessage::FileRead);
    message.insertDataValue("ID", m_resourceID);
    message.insertDataValue("pos", from);
    message.insertDataValue("maxReadSize", maxlen);
	message.insertDataValue("fh", m_fh);

    LinkMessage reply = sendMessage(message);

	if (reply.getResultCode() == LinkMessage::LinkError)
		return payload;


    if (reply.getResultCode() == LinkMessage::OK)
    {
        if (reply.getData().keys().contains("atEnd"))
            m_atEnd = reply.getData()["atEnd"].toBool();

        payload = reply.getPayLoad();

      //  qInfo() << "read " << payload.size() << " bytes";
		if ( QIODevice::isOpen())
			QIODevice::seek(from + payload.size());
    }

    return payload;
}



qint64 LinkBackend::readData(char *data, qint64 maxlen)
{
    //TimeLogger logger("Read from for " + m_remotePath);
    if (m_socket.isNull()) {
        qWarning() << "LinkBackend::readData: No socket. Invalid thread";
        return 0;
    }

    LinkMessage message;
    message.setMessageType(LinkMessage::FileRead);
    message.insertDataValue("ID", m_resourceID);
    message.insertDataValue("pos", this->pos());
    message.insertDataValue("maxReadSize", maxlen);
	message.insertDataValue("fh", m_fh);


    LinkMessage reply = sendMessage( message);

    if (reply.getResultCode() == LinkMessage::LinkError)
         qCritical() << "readData error on link" << m_storageID << reply.getErrorMsg() << m_socket->linkState();


    QByteArray payload;

    if (reply.getResultCode() == LinkMessage::OK)
    {
        if (reply.getData().keys().contains("atEnd"))
            m_atEnd = reply.getData()["atEnd"].toBool();

        payload = reply.getPayLoad();
        memcpy(data, payload.constData(), payload.size());
    }

    return payload.size();
}

qint64 LinkBackend::writeData(const char *data, qint64 len)
{
    //qInfo() << "Write " << len << "bytes to" << m_remotePath;
    if (m_socket.isNull()) {
        qWarning() << "LinkBackend::writeData: No socket. Invalid thread";
        return 0;
    }

    LinkMessage message;
    message.setMessageType(LinkMessage::FileWrite);
    message.insertDataValue("ID", m_resourceID);
    message.insertDataValue("pos", this->pos());
	message.insertDataValue("fh", m_fh);
    message.setPayLoad(QByteArray::fromRawData(data, len));

    LinkMessage reply = sendMessage(message);

	if (reply.getResultCode() == LinkMessage::LinkError) {
        qCritical() << "WriteData error on link" << m_storageID;
	}

    qint64 bytesWritten = 0;

    if (reply.getResultCode() == LinkMessage::OK)
    {
        if (reply.getData().keys().contains("bytesWritten"))
        {
            bool isOk = false;
            bytesWritten  = reply.getData()["bytesWritten"].toLongLong(&isOk);
            if (!isOk)
                bytesWritten = 0;
        }
    }

    return bytesWritten;
}


bool LinkBackend::atEnd() const
{
    return m_atEnd;
}

qint64 LinkBackend::size() const
{
    return m_size;
}

QString LinkBackend::storageID()
{
    return m_storageID;
}


QByteArray LinkBackend::bytesToCache() const
{
    return m_bytesToCache;
}

QString LinkBackend::contentType() const
{
    return m_contentType;
}

LinkMessage LinkBackend::sendMessage(LinkMessage request)
{
    LinkMessage responseMessage;

    if (!m_socket.isNull() && m_socket->isValid() && m_socket->state() == QAbstractSocket::ConnectedState)
    {
        responseMessage = m_socket->sendMessage(request);
        return responseMessage;
    }

    responseMessage.setResultCode(LinkMessage::LinkError);
    responseMessage.setErrorMsg("Socket is disconnected or died.");

    return responseMessage;
}

void LinkBackend::writeMessage( LinkMessage request)
{
    if (!m_socket.isNull() && m_socket->isValid() && m_socket->state() == QAbstractSocket::ConnectedState)
    {
        m_socket->writeMessage(request);
    }
}

