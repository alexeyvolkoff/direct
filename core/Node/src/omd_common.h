#ifndef OMD_COMMON_H
#define OMD_COMMON_H

#include <QObject>


namespace OnMyDisk
{
    Q_NAMESPACE         // required for meta object creation

    class DriveInfo  {
    private:
        QString m_label = "";
        QString m_type = "";
        QString m_size = "";
    public:
        DriveInfo() {}
        DriveInfo(QString label, QString type, QString size): m_label(label), m_type(type), m_size(size) {}
        DriveInfo(const DriveInfo& info ): m_label(info.m_label), m_type(info.m_type), m_size(info.m_size) {}
        QString label() const { return m_label; }
        QString type() const { return m_type; }
        QString size() const { return m_size; }
    };


    enum ConnectionState {
        Disconnected = 0,
        Authorization,
        Connecting,
        ConnectionError,
        AuthorizationError,
        Connected
    };

    Q_ENUM_NS(ConnectionState)  // register the enum in meta object data

}

Q_DECLARE_METATYPE(OnMyDisk::DriveInfo)


#endif // OMD_COMMON_H
