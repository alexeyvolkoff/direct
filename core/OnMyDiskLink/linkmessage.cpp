#include "linkmessage.h"
#include "linkmessage.h"
#ifdef USE_MEM_POOL
#include "pool.hpp"
#endif

LinkMessage::LinkMessage(QObject *parent) : QObject(parent)
{
    m_messageType = Null;
    m_resultCode = NotYethandled;
}

LinkMessage::LinkMessage(const LinkMessage &message):
    QObject(message.parent())
{
    m_messageType = message.m_messageType;
    m_data = message.m_data;
    m_entryData = message.m_entryData;
    m_resultCode = message.m_resultCode;
    m_errorMsg = message.m_errorMsg;
    m_senderID = message.m_senderID;
    m_payLoad = message.m_payLoad;
}

LinkMessage &LinkMessage::operator =(const LinkMessage &message)
{
    if (this != &message) {
        this->setParent(message.parent());
        m_messageType = message.m_messageType;
        m_data = message.m_data;
        m_entryData = message.m_entryData;
        m_resultCode = message.m_resultCode;
        m_errorMsg = message.m_errorMsg;
        m_senderID = message.m_senderID;
        m_payLoad = message.m_payLoad;
    }
    return *this;
}


LinkMessage::~LinkMessage()
{
    clear();
}

void LinkMessage::clear()
{
    m_data.clear();
    m_entryData.clear();
    m_payLoad.clear();
    m_data.squeeze();
    m_entryData.squeeze();
    m_payLoad.squeeze();
    m_errorMsg = "";
    m_messageType = Null;
    m_resultCode = NotYethandled;
}

LinkMessage::LinkMessageType LinkMessage::getMessageType() const
{
    return m_messageType;
}

void LinkMessage::setMessageType(const LinkMessageType &value)
{
    m_messageType = value;
}

bool LinkMessage::emitable() const
{
    return (m_messageType == InitLink || m_messageType == PingReply || m_messageType == RPC
            || m_messageType == TunnelOpen || m_messageType == TunnelData || m_messageType == TunnelClose
            || m_messageType == TunnelSignaling);
}

bool LinkMessage::fileOperation() const
{
    return (m_messageType == FileOpen || m_messageType == FileRead || m_messageType == FileWrite || m_messageType == Ping);
}

bool LinkMessage::blocking() const
{
   return (m_messageType == FileWrite || m_messageType == FileOpen || m_messageType == FileClose  || m_messageType == FileInfo  || m_messageType == RPC  || m_messageType == FileRead);
}


QHash<QString, QVariant> LinkMessage::getData() const
{
    return m_data;
}

void LinkMessage::setData(const QHash<QString, QVariant> &value)
{
    m_data = value;
}

QString LinkMessage::getSenderID() const
{
    return m_senderID;
}

void LinkMessage::setSenderID(const QString &value)
{
    m_senderID = value;
}

LinkMessage::LinkMessageResult LinkMessage::getResultCode() const
{
    return m_resultCode;
}

void LinkMessage::setResultCode(const LinkMessageResult &value)
{
    m_resultCode = value;
}

QString LinkMessage::getErrorMsg() const
{
    return m_errorMsg;
}

void LinkMessage::setErrorMsg(const QString &value)
{
    m_errorMsg = value;
}

void LinkMessage::insertDataValue(QString paramName, QVariant paramValue)
{
    m_data.insert(paramName, paramValue);
}

void LinkMessage::insertEntryDataValue(QString entryName, QString paramName, QVariant paramValue)
{
    if (m_entryData.contains(entryName))
        m_entryData[entryName].insert(paramName, paramValue);
}

QHash<QString, QHash<QString, QVariant>>  LinkMessage::getEntryData() const
{
    return m_entryData;
}

void LinkMessage::setEntryData(const QHash<QString, QHash<QString, QVariant>>  &entryData)
{
    m_entryData = entryData;
}

void LinkMessage::addEntryDataRecord(const QString entryName, const QHash<QString, QVariant> &entryData)
{
    if (m_entryData.contains(entryName))
        m_entryData[entryName] = entryData;
    else
        m_entryData.insert(entryName, entryData);
}

QString LinkMessage::messageTypeString() const
{
    return QMetaEnum::fromType<LinkMessage::LinkMessageType>().valueToKey(this->m_messageType);
}

QString LinkMessage::messageResultCodeString() const
{
    return QMetaEnum::fromType<LinkMessage::LinkMessageResult>().valueToKey(this->m_resultCode);
}

QString LinkMessage::messageDataString() const
{
    QStringList dataString;
    Q_FOREACH(QString dataKey, m_data.keys())
    {
        dataString.append("{" + dataKey + ": " + m_data[dataKey].toString() + "}");
    }
    return dataString.join(",");
}

QString LinkMessage::toString() const
{
    QStringList messageBody;
    messageBody.append("Type: " + messageTypeString());
    messageBody.append("Result: " + messageResultCodeString());
    messageBody.append("Body: " + messageDataString());

    return messageBody.join(", ");
}

QByteArray LinkMessage::getPayLoad() const
{
    return m_payLoad;
}

void LinkMessage::setPayLoad(const QByteArray &payLoad)
{
    m_payLoad = payLoad;
}



#ifdef USE_MEM_POOL
void *LinkMessage::operator new(size_t size)
{
	return  getMessageAllocator()->allocate(size);
}

void LinkMessage::operator delete(void *p)
{
	getMessageAllocator()->free(p);
}
#endif

/*
QString LinkMessage::getIconSrc() const
{
    return m_iconSrc;
}

void LinkMessage::setIconSrc(const QString &iconSrc)
{
    m_iconSrc = iconSrc;
}

*/
