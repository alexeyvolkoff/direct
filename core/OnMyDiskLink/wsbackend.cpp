#include "wsbackend.h"
#include <QJsonObject>
#include "simplecrypt.h"

WebSocketBackend::WebSocketBackend(QSharedPointer<ClientNotifier> notifier, QString remotePath, QString query, qint64 size, QObject *parent)
    : IResourceBackend(parent), m_remotePath(remotePath)
{
    m_size = size;
    m_resourceID = QUuid::createUuid().toString();
    m_notifier = notifier;
    m_storageID = notifier->getLinkId();
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


bool WebSocketBackend::RefreshInfo(CloudResource *resource)
{
    bool isOk = false;
    if (!resource)
        return  isOk;

    QJsonObject cmd;
    cmd["type"] = LinkMessage::FileInfo;
    cmd["ID"] = m_resourceID;
    cmd["filePath"] = m_remotePath;
    cmd["query"] = m_query;

    QPointer<WebSocketBackend> guard(this);
    QJsonObject response = m_notifier->fileApi(cmd);
    if (guard.isNull())
        return isOk;

    if (response["code"] != LinkMessage::LinkError)
    {
        QJsonObject resourceData = response["resourceData"].toObject();
        m_size = resourceData["size"].toInt();
        m_storageID = resourceData["storageID"].toString();
        m_contentType = resourceData["contentType"].toString();
        if (resourceData.contains("bytesToCache"))
        {
            m_bytesToCache = QByteArray::fromBase64(resourceData["bytesToCache"].toString().toUtf8());
            m_size = m_bytesToCache.size();
            resourceData["isDir"] = false;
            resourceData["isFile"] = true;
            resourceData["size"] = m_size;
        }
        resource->LoadData(jsonToData(resourceData));
        isOk = true;
    }

    return isOk;
}

bool WebSocketBackend::RefreshAllInfo(CloudResource *resource)
{
    bool isOk = false;

    if (!resource)
        return  isOk;

    QJsonObject cmd;
    cmd["type"] = LinkMessage::FileInfo;
    cmd["ID"] = m_resourceID;
    cmd["filePath"] = m_remotePath;
    cmd["query"] = m_query;
    cmd["children"] = true;

    QPointer<WebSocketBackend> guard(this);
    QJsonObject response = m_notifier->fileApi(cmd);
    if (guard.isNull())
        return isOk;

    if (response["code"] != LinkMessage::LinkError)
    {
        QJsonObject resourceData = response["resourceData"].toObject();
        m_size = resourceData["size"].toInt();
        m_storageID = resourceData["storageID"].toString();
        m_contentType = resourceData["contentType"].toString();
        if (resourceData.contains("bytesToCache"))
        {
            m_bytesToCache = QByteArray::fromBase64(resourceData["bytesToCache"].toString().toUtf8());
            resourceData["isDir"] = false;
            resourceData["isFile"] = true;
            m_size = m_bytesToCache.size();
        }
        resource->LoadData(jsonToData(resourceData));
        if (resource->isDir())
           resource->LoadEntryData(jsonToEntryData("name", response["entryData"].toArray()));
        isOk = true;
    }

    return isOk;
}

bool WebSocketBackend::remove(bool forced)
{
    QJsonObject cmd;
    cmd["type"] = LinkMessage::Remove;
    cmd["ID"] = m_resourceID;
    cmd["filePath"] = m_remotePath;
    cmd["forced"] = forced;

    QPointer<WebSocketBackend> guard(this);
    QJsonObject response = m_notifier->fileApi(cmd);
    if (guard.isNull())
        return false;

    if (response["code"] == LinkMessage::LinkError)
    {
        qCritical() << "Remove error on link" << m_storageID;
        return false;
    }
    return response["code"] == LinkMessage::OK;
}

bool WebSocketBackend::removeShare()
{
    QJsonObject cmd;
    cmd["type"] = LinkMessage::RemoveShare;
    cmd["ID"] = m_resourceID;
    cmd["filePath"] = m_remotePath;

    QPointer<WebSocketBackend> guard(this);
    QJsonObject response = m_notifier->fileApi(cmd);
    if (guard.isNull())
        return false;

    if (response["code"] == LinkMessage::LinkError)
    {
        qCritical() << "RemoveShare error on link" << m_storageID;
        return false;
    }
    return response["code"] == LinkMessage::OK;
}

bool WebSocketBackend::mkcol()
{
    QJsonObject cmd;
    cmd["type"] = LinkMessage::MkDir;
    cmd["ID"] = m_resourceID;
    cmd["filePath"] = m_remotePath;

    QPointer<WebSocketBackend> guard(this);
    QJsonObject response = m_notifier->fileApi(cmd);
    if (guard.isNull())
        return false;

    return response["code"] == LinkMessage::OK;
}

bool WebSocketBackend::rename(const QString &newName)
{
    Q_UNUSED(newName);
    QJsonObject cmd;
    cmd["type"] = LinkMessage::Rename;
    cmd["ID"] = m_resourceID;
    cmd["filePath"] = m_remotePath;
    QPointer<WebSocketBackend> guard(this);
    QJsonObject response = m_notifier->fileApi(cmd);
    if (guard.isNull())
        return false;

    if (response["code"] == LinkMessage::LinkError)
    {
        qCritical() << "Rename error on link" << m_storageID;
        return false;
    }
    return true;
}

bool WebSocketBackend::move(const QString &destPath)
{
    QJsonObject cmd;
    cmd["type"] = LinkMessage::Move;
    cmd["ID"] = m_resourceID;
    cmd["filePath"] = m_remotePath;
    cmd["destPath"] = destPath;

    QPointer<WebSocketBackend> guard(this);
    QJsonObject response = m_notifier->fileApi(cmd);
    if (guard.isNull())
        return false;

    if (response["code"] == LinkMessage::LinkError)
    {
        qCritical() << "Rename error on link" << m_storageID;
        return false;
    }
    return true;
}

bool WebSocketBackend::copy(const QString &destPath)
{
    QJsonObject cmd;
    cmd["type"] = LinkMessage::Copy;
    cmd["ID"] = m_resourceID;
    cmd["filePath"] = m_remotePath;
    cmd["destPath"] = destPath;
    QPointer<WebSocketBackend> guard(this);
    QJsonObject response = m_notifier->fileApi(cmd);
    if (guard.isNull())
        return false;

    if (response["code"] == LinkMessage::LinkError)
    {
        qCritical() << "Rename error on link" << m_storageID;
        return false;
    }
    return true;
}

QString WebSocketBackend::getPublicLink(QHash<QString, QVariant> linkData)
{
    //Generate link
    QString key = linkData.contains("key") ? linkData["key"].toString() : "";
    quint64 keyHash = key.isEmpty() ? SimpleCrypt::defaultKey : SimpleCrypt::stringToHash(key);
    SimpleCrypt crypt(keyHash);

    QString path = m_remotePath;
    if (path.endsWith("/"))
        path.chop(1);

    QString link = QString("/shared/") + crypt.encryptToString(QString("%1:%2").arg(m_storageID, QFileInfo(path).fileName())) ;
    qDebug() << "Public link for" << qPrintable(QString("%1: %2").arg(QFileInfo(path).fileName(), link)) ;

    //Set link
    linkData["link"] = link;

    QJsonObject cmd;
    cmd["type"] = LinkMessage::GetPublicLink; // For PWA it means just to save it
    cmd["ID"] = m_resourceID;
    cmd["filePath"] = path;
    cmd["linkData"] = dataToJson(linkData);
    QPointer<WebSocketBackend> guard(this);
    QJsonObject response = m_notifier->fileApi(cmd);
    if (guard.isNull())
        return "";

    if (response["code"] == LinkMessage::OK)
    {
        if (response.contains("linkData"))
            linkData = jsonToData(response["linkData"].toObject());
        
        return link;
    } else
        return "";
}


void WebSocketBackend::removePublicLink()
{
    QJsonObject cmd;
    cmd["type"] = LinkMessage::RemovePublicLink;
    cmd["ID"] = m_resourceID;
    cmd["filePath"] = m_remotePath;
    if (!m_linkID.isEmpty())
        cmd["linkID"] = m_linkID;

    QPointer<WebSocketBackend> guard(this);
    m_notifier->fileApi(cmd);
}

bool WebSocketBackend::invoke(const QString &rpc,  QHash<QString, QVariant> &params,  QByteArray &payload)
{
    QJsonObject cmd;
    cmd["type"] = LinkMessage::RPC;
    cmd["ID"] = m_resourceID;
    cmd["filePath"] = m_remotePath;
    cmd["rpc"] = rpc;
    cmd["params"] = dataToJson(params);
    cmd["payload"] = QString::fromUtf8(payload.toBase64());

    QPointer<WebSocketBackend> guard(this);
    QJsonObject response = m_notifier->fileApi(cmd);
    if (guard.isNull())
        return false;

    if (response["code"] != LinkMessage::LinkError)
    {
       params = jsonToData(response["params"].toObject());
       payload = QByteArray::fromBase64(response["payload"].toString().toUtf8());
       return true;
    } else {
        return false;
    }
}

bool WebSocketBackend::isSequential() const
{
    return false;
}

bool WebSocketBackend::open(QIODevice::OpenMode mode)
{
    QJsonObject cmd;
    cmd["type"] = LinkMessage::FileOpen;
    cmd["ID"] = m_resourceID;
    cmd["filePath"] = m_remotePath;
    cmd["query"] = m_query;

    if ((mode & QIODevice::ReadOnly) == QIODevice::ReadOnly)
        cmd["read"] = true;
    if ((mode & QIODevice::WriteOnly) == QIODevice::WriteOnly)
        cmd["write"] = true;
    if ((mode & QIODevice::Append) == QIODevice::Append)
        cmd["append"] = true;
    if ((mode & QIODevice::Truncate) == QIODevice::Truncate)
        cmd["truncate"] = true;

    QPointer<WebSocketBackend> guard(this);
    QJsonObject response = m_notifier->fileApi(cmd);
    if (guard.isNull())
        return false;

    if (response["code"] == LinkMessage::OK)
    {
        if ( !QIODevice::open(mode) )
            qDebug() << "Can not open in mode:" << mode;
        if (response.contains("fh"))
            m_fh = response["fh"].toInt();
        else
            m_fh = -1;
    }
    else
    {
        m_fh = -1;
    }
    return (m_fh != -1);
}

bool WebSocketBackend::seek(qint64 pos)
{
    QJsonObject cmd;
    cmd["type"] = LinkMessage::Copy;
    cmd["ID"] = m_resourceID;
    cmd["pos"] = pos;
    cmd["fh"] = m_fh;

    QPointer<WebSocketBackend> guard(this);
    QJsonObject response = m_notifier->fileApi(cmd);
    if (guard.isNull())
        return false;

    if (response["code"] == LinkMessage::LinkError)
        return false;

    if (response.contains("atEnd"))
        m_atEnd = response["atEnd"].toBool();

    if (response.contains("pos"))
    {
        qint64 pos  = response["pos"].toInt();
        QIODevice::seek(pos);
    }

    return response["code"] == LinkMessage::OK;
}

bool WebSocketBackend::isOpen() const
{
     return (m_fh > 0) && (m_notifier != nullptr);
}

void WebSocketBackend::close()
{
  //  TimeLogger logger("Close for " + m_remotePath);
    QIODevice::close();

    QJsonObject cmd;
    cmd["type"] = LinkMessage::FileClose;
    cmd["ID"] = m_resourceID;
    cmd["fh"] = m_fh;

    QPointer<WebSocketBackend> guard(this);
    m_notifier->fileApi(cmd);

	m_fh = 0;
    m_from = 0;

}


QByteArray WebSocketBackend::readFromPosition(qint64 from, qint64 maxlen)
{
    QByteArray payload;
    QJsonObject cmd;
    cmd["type"] = LinkMessage::FileRead;
    cmd["ID"] = m_resourceID;
    cmd["pos"] = from;
    cmd["maxReadSize"] = maxlen;
    cmd["fh"] = m_fh;
    QPointer<WebSocketBackend> guard(this);
    QJsonObject response = m_notifier->fileApi(cmd);
    if (guard.isNull())
        return payload;

    if (response["code"] == LinkMessage::LinkError)
    {
        return payload;
    }


    if (response["code"] == LinkMessage::OK)
    {
        if (response.contains("atEnd"))
            m_atEnd = response["atEnd"].toBool();

        payload = QByteArray::fromBase64(response["payload"].toString().toUtf8());

      //  qInfo() << "read " << payload.size() << " bytes";
		if ( QIODevice::isOpen())
			QIODevice::seek(from + payload.size());
    }

    return payload;
}



qint64 WebSocketBackend::readData(char *data, qint64 maxlen)
{
    QJsonObject cmd;
    cmd["type"] = LinkMessage::FileRead;
    cmd["ID"] = m_resourceID;
    cmd["pos"] = this->pos();
    cmd["maxReadSize"] = maxlen;
    cmd["fh"] = m_fh;

    QPointer<WebSocketBackend> guard(this);
    QJsonObject response = m_notifier->fileApi(cmd);
    if (guard.isNull())
        return 0;

    if (response["code"] == LinkMessage::LinkError)
         qCritical() << "readData error on link" << m_storageID; /*<< reply.getErrorMsg() << m_socket->linkState();*/

    QByteArray payload;

    if (response["code"] == LinkMessage::OK)
    {
        if (response.contains("atEnd"))
            m_atEnd = response["atEnd"].toBool();

        payload = QByteArray::fromBase64(response["payload"].toString().toUtf8());
        memcpy(data, payload.constData(), payload.size());
    }

    return payload.size();
}

qint64 WebSocketBackend::writeData(const char *data, qint64 len)
{
    QJsonObject cmd;
    cmd["type"] = LinkMessage::FileWrite;
    cmd["ID"] = m_resourceID;
    cmd["pos"] = this->pos();
    cmd["fh"] = m_fh;
    cmd["payload"] = QString(QByteArray::fromRawData(data, len).toBase64());

    QPointer<WebSocketBackend> guard(this);
    QJsonObject response = m_notifier->fileApi(cmd);
    if (guard.isNull())
        return 0;

    if (response["code"] == LinkMessage::LinkError)
        qCritical() << "WriteData error on link" << m_storageID;

    qint64 bytesWritten = 0;

    if (response["code"] == LinkMessage::OK)
    {
        if (response.contains("bytesWritten"))
        {
            bytesWritten  = response["bytesWritten"].toInt();
        }
    }

    return bytesWritten;
}


bool WebSocketBackend::atEnd() const
{
    return m_atEnd;
}

qint64 WebSocketBackend::size() const
{
    return m_size;
}

QString WebSocketBackend::storageID()
{
    return m_storageID;
}


QByteArray WebSocketBackend::bytesToCache() const
{
    return m_bytesToCache;
}


QString WebSocketBackend::contentType() const
{
    return m_contentType;
}


QHash<QString, QVariant> WebSocketBackend::jsonToData(const QJsonObject &obj)
{
    QHash<QString, QVariant> data;
    foreach(const QString& key, obj.keys())
    {
        QJsonValue val = obj[key];
        if (val.isObject()) {
            data.insert(key, QVariant(jsonToData(val.toObject())));
        } else {
            data.insert(key, val.toVariant());
        }
    }
    return data;
}

QHash<QString, QHash<QString, QVariant>> WebSocketBackend::jsonToEntryData(const QString &keyName, const QJsonArray& entries)
{
    QHash<QString, QHash<QString, QVariant>> entryFata;
    for (const QJsonValue& v : entries)
    {
        if (!v.isObject())
            continue;

        QJsonObject entry = v.toObject();
        QString name = entry.value(keyName).toString();
        if (name.isEmpty())
            continue;

        QHash<QString, QVariant> itemData;
        QStringList keys = entry.keys();
        for (const QString& key : keys)
        {
            itemData.insert(key, entry.value(key).toVariant());
        }
        entryFata.insert(name, itemData);
    }
    return entryFata;
}

QJsonObject WebSocketBackend::dataToJson(QHash<QString, QVariant> data)
{
    QJsonObject obj;
    foreach(const QString& key, data.keys())
    {
        QVariant val = data.value(key);
        if (val.type() == QVariant::Hash)
            obj.insert(key, dataToJson(val.toHash()));
        else
            obj.insert(key, QJsonValue::fromVariant(val));
    }
    return obj;
}

QByteArray WebSocketBackend::jsonToArray(const QJsonArray &jsonArray)
{
    QByteArray byteArray;

    // Reserve space for efficiency
    byteArray.reserve(jsonArray.size());

    // Append each byte from the array to QByteArray
    for (const QJsonValue &value : jsonArray) {
        byteArray.append(static_cast<char>(value.toInt())); // Cast to char
    }

    return byteArray;
}


