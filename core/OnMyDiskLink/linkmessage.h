#ifndef LINKMESSAGE_H
#define LINKMESSAGE_H

#include <QObject>
#include <QHash>
#include <QString>
#include <QVariant>
#include <QDataStream>
#include <QMetaEnum>
#include <QDebug>
#include "onmydisklink_global.h"

class LinkMessage : public QObject
{
    Q_OBJECT
public:
    enum LinkMessageType {
        Null = -1,
        BadCertificate,
        SwitchPort,
        Ping,
        PingReply,
        ConnectDataSockets,
        MkDir,
        Remove,
        FileInfo,
        FileOpen,
        FileClose,
        FilePos,
        FileSeek,
        FileRead,
        FileWrite,
        AddShare,
        RemoveShare,
        FuseMounted,
        CollectionChanged,
        ManageStorage,
        UpdateShare,
        GetPublicLink,
        RemovePublicLink,
        Copy,
        Move,
        Rename,
        InitLink,
        RPC,
        TunnelOpen,
        TunnelData,
        TunnelClose,
        TunnelSignaling
    };
    Q_ENUM(LinkMessageType)

    enum LinkMessageResult { NotYethandled = -1, OK, BadData, CantHandle, UnknownRequest, UnknownClient, LinkError, Request };
    Q_ENUM(LinkMessageResult)

    explicit LinkMessage(QObject *parent = nullptr);
    LinkMessage(const LinkMessage &message);
    LinkMessage& operator =(const LinkMessage& message);
    ~LinkMessage();
    void clear();

    LinkMessageType getMessageType() const;
    void setMessageType(const LinkMessageType &value);

    QHash<QString, QVariant> getData() const;
    void setData(const QHash<QString, QVariant> &value);

    QString getSenderID() const;
    void setSenderID(const QString &value);

    LinkMessageResult getResultCode() const;
    void setResultCode(const LinkMessageResult &value);

    QString getErrorMsg() const;
    void setErrorMsg(const QString &value);

    void insertDataValue(QString paramName, QVariant paramValue);
    void insertEntryDataValue(QString entryName, QString paramName, QVariant paramValue);
    QVariant getDataValue(const QString& paramName) const;

    friend QDataStream &operator<<(QDataStream &out, const LinkMessage &linkMessage);
    friend QDataStream &operator>>(QDataStream &in, LinkMessage &linkMessage);

    QHash<QString, QHash<QString, QVariant> > getEntryData() const;
    void setEntryData(const QHash<QString, QHash<QString, QVariant> > &entryData);

    void addEntryDataRecord(const QString entryName, const QHash<QString, QVariant> &entryData);

    QString messageTypeString() const;
    QString messageResultCodeString() const;
    QString messageDataString() const;
    QString toString() const;

    QByteArray getPayLoad() const;
    void setPayLoad(const QByteArray &payLoad);
    /*
    QString getIconSrc() const;
    void setIconSrc(const QString &iconSrc);
    */

#ifdef USE_MEM_POOL
	void * operator new(size_t size);
	void operator delete(void * p);
#endif

    bool emitable() const;
    bool fileOperation() const;
    bool blocking() const;

private:
    LinkMessageType m_messageType;
    QHash<QString, QVariant> m_data;
    QHash<QString, QHash<QString, QVariant>> m_entryData;
    LinkMessageResult m_resultCode;
    QString m_errorMsg;
    QString m_senderID;
    QByteArray m_payLoad;
    //QString m_iconSrc;

signals:

public slots:
};

inline QVariant LinkMessage::getDataValue(const QString &paramName) const
{
    if (m_data.contains(paramName))
        return m_data.value(paramName);
    else
        return QVariant();
}


inline QDataStream & operator<<(QDataStream & ds, const LinkMessage &linkMessage)
{
    //qInfo() << "Serialization of message: " << linkMessage.toString();
    //ds.setByteOrder( QDataStream::LittleEndian );
    ds << static_cast<quint32>(linkMessage.getMessageType()) << static_cast<quint32>(linkMessage.getResultCode()) << linkMessage.getPayLoad() << linkMessage.getData() << linkMessage.getEntryData() << linkMessage.getErrorMsg();// << linkMessage.getIconSrc();
    return ds;
}

inline QDataStream & operator>>(QDataStream & ds, LinkMessage &linkMessage)
{
    linkMessage.clear();
    //ds.setByteOrder( QDataStream::LittleEndian );
    QHash<QString, QVariant> data;
    QByteArray payLoad;
    QHash<QString, QHash<QString, QVariant>>  entryData;
    quint32 rawType, rawResultCode;
    QString errMsg;
    //QString iconSrc;
    ds >> rawType >> rawResultCode >> payLoad >> data >> entryData >> errMsg;// >> iconSrc;
    LinkMessage::LinkMessageType messageType = static_cast<LinkMessage::LinkMessageType>(rawType);
    LinkMessage::LinkMessageResult messageResult = static_cast<LinkMessage::LinkMessageResult>(rawResultCode);
    linkMessage.setMessageType(messageType);
    linkMessage.setPayLoad(payLoad);
    linkMessage.setData(data);
    linkMessage.setEntryData(entryData);
    linkMessage.setResultCode(messageResult);
    linkMessage.setErrorMsg(errMsg);
    //linkMessage.setIconSrc(iconSrc);
    //qInfo() << "Deserialization of message: " << linkMessage.toString();
    return ds;
}

Q_DECLARE_METATYPE(LinkMessage)

#endif // LINKMESSAGE_H

