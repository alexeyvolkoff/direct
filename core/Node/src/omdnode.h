#ifndef OMDNODE_H
#define OMDNODE_H

#include <QObject>
#include <QSettings>
#include "hostresource.h"
#include "omd_common.h"

class OnMyDiskNode : public QObject
{
    Q_OBJECT

protected:
    QSettings m_systemSettings;
    QSettings m_userSettings;
    LocalResource* m_docRoot;
    LocalResource* m_themeRoot;
    HostResource* m_rootResource;
public:
    explicit OnMyDiskNode(QString configFile, QObject *parent = nullptr);
    ~OnMyDiskNode();

signals:
    void addShare(QString share);
    void removeShare(QString share);
    void showShare(QString share);
    void connectCloud();
    void disconnectCloud();
    void showStorage();
    void showCloud();
    void linkStorage();
    void resetLink();
    void parseUrl(QString share);
    void authRequest(QString user, QString password);
    void stateChanged(OnMyDisk::ConnectionState status,  QString statusMessage);
    void certLoaded(QString userName, QString deviceId, QString linkId, QString validFrom, QString validTo);
    void quit();

};

#endif // OMDNODE_H
