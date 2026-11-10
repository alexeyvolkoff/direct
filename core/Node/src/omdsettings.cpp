#include "omdsettings.h"
#include <QMetaObject>
#include <QMetaProperty>


OnMyDiskSettings::OnMyDiskSettings(QObject *parent): QObject(parent)
{

}

OnMyDiskSettings::OnMyDiskSettings(const QJsonObject &json, QObject *parent): QObject(parent)
{
    for(int i = staticMetaObject.propertyOffset(); i < metaObject()->propertyCount(); i++) {
            auto key = OnMyDiskSettings::staticMetaObject.property(i).name();
            if (json.contains(key))
                setProperty(key, json.value(key).toVariant());
    }
}

OnMyDiskSettings::OnMyDiskSettings(const OnMyDiskSettings &other): QObject(other.parent())
{
    for(int i = staticMetaObject.propertyOffset(); i < metaObject()->propertyCount(); i++) {
            auto key = OnMyDiskSettings::staticMetaObject.property(i).name();
            setProperty(key, other.property(key));
    }
}

OnMyDiskSettings &OnMyDiskSettings::operator =(const OnMyDiskSettings &other)
{
    for(int i = staticMetaObject.propertyOffset(); i < metaObject()->propertyCount(); i++) {
            auto key = OnMyDiskSettings::staticMetaObject.property(i).name();
            setProperty(key, other.property(key));
    }
    return *this;
}

OnMyDiskSettings &OnMyDiskSettings::operator =(const QJsonObject &json)
{
    for(int i = staticMetaObject.propertyOffset(); i < metaObject()->propertyCount(); i++) {
            auto key = OnMyDiskSettings::staticMetaObject.property(i).name();
            if (json.contains(key))
                setProperty(key, json.value(key).toVariant());
    }
    return *this;
}

#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>

void OnMyDiskSettings::readSettings(QSettings* settings)
{
    for(int i = staticMetaObject.propertyOffset(); i < metaObject()->propertyCount(); i++) {
            auto key = OnMyDiskSettings::staticMetaObject.property(i).name();
            if (OnMyDiskSettings::staticMetaObject.property(i).isStored())
                setProperty(key, settings->value(key, property(key)));
    }

#if ((EMBEDDED || SERVICE || defined(DESKTOP) || defined(SERVER)) && !defined(ROUTER))
    // Read rcloneShares manually from JSON string
    QString sharesJson = settings->value("rcloneShares", "").toString();
    if (!sharesJson.isEmpty()) {
        QJsonDocument doc = QJsonDocument::fromJson(sharesJson.toUtf8());
        if (doc.isArray()) {
            m_rcloneShares = doc.array().toVariantList();
        }
    }

    // Migration logic: if shares list is empty but we have an old config port, migrate it.
    if (m_rcloneShares.isEmpty() && m_rcloneWebdavPort > 0) {
        QVariantMap share;
        share["name"] = "ProtonDrive";
        share["type"] = "protondrive";
        share["port"] = m_rcloneWebdavPort;
        m_rcloneShares.append(share);
    }
#endif

    #ifdef P2P_ENABLED
    if (m_server == "localhost" || m_server.isEmpty() || m_server == "disk.direct")
        m_server = QString("disk.direct");
    if (m_server == "disk.direct" && (m_port == 0 || m_port == 9000))
        m_port = DEFAULT_PORT;
    #elif defined(STAGING)
    if (m_server == "localhost" || m_server.isEmpty())
        m_server = QString("disk.direct");
    #endif
    #ifdef INNBOX
    qInfo() << "resetting server";
    if (m_server == "storage.innbox.net")
        m_server = QString("storage.innbox.app");
    #endif
#if ((EMBEDDED || SERVICE || defined(DESKTOP) || defined(SERVER)) && !defined(ROUTER))
    if (m_aiBase == "http://localhost:8000" || m_aiBase.isEmpty())
        m_aiBase = DEFAULT_AI_URL;
#endif
}

void OnMyDiskSettings::writeSettings(QSettings *settings)
{
    for(int i = staticMetaObject.propertyOffset(); i < metaObject()->propertyCount(); i++) {
            auto key = OnMyDiskSettings::staticMetaObject.property(i).name();
            if (OnMyDiskSettings::staticMetaObject.property(i).isStored())
                settings->setValue(key, property(key));
    }

#if ((EMBEDDED || SERVICE || defined(DESKTOP) || defined(SERVER)) && !defined(ROUTER))
    // Write rcloneShares as JSON string
    QJsonArray arr = QJsonArray::fromVariantList(m_rcloneShares);
    QJsonDocument doc(arr);
    settings->setValue("rcloneShares", QString::fromUtf8(doc.toJson(QJsonDocument::Compact)));
#endif
}

OnMyDiskSettings::operator const QJsonObject() const
{
    QJsonObject json;
    for(int i = staticMetaObject.propertyOffset(); i < metaObject()->propertyCount(); i++) {
           auto key = OnMyDiskSettings::staticMetaObject.property(i).name();
           json[key] = QJsonValue::fromVariant(property(key));
    }
    return json;
}
