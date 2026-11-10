#ifndef SERVICENODE_H
#define SERVICENODE_H

#include <QObject>
#include "omdnode.h"
#include "omdclient.h"
#include "onmydisklinkclient.h"

class ServiceNode: public OnMyDiskNode
{
  Q_OBJECT

private:
       //public links
       OnMyDiskLinkClient m_linkClient;
       //onmydisk links
       OnMyDiskClient m_client;

public:
    ServiceNode(QString configFile, QObject *parent = nullptr);
    void close();

signals:
    void stateChanged(OnMyDisk::ConnectionState status,  QString statusMessage);
    void authRequest(QString user,  QString password);

};

#endif // SERVICENODE_H
