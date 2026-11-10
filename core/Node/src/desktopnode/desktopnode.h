#ifndef ONMYDISKDESKTOPNODE_H
#define ONMYDISKDESKTOPNODE_H

#include <QObject>
#include "omdnode.h"
#include "onmydisklinkclient.h"
#include "omdnode.h"
#include "omdclient.h"
#include "hostresource.h"
#include "onmydisklink_global.h"
#include "desktopbackend.h"


class OnMyDiskDesktopNode : public OnMyDiskNode
{
 Q_OBJECT

private:
    //public links
    OnMyDiskLinkClient m_linkClient;
    //onmydisk links
    OnMyDiskClient m_client;
    DesktopBackend m_clientBackend;


public:
    OnMyDiskDesktopNode(QString configFile, QObject *parent = nullptr);
    void close();
};

#endif // ONMYDISKDESKTOPNODE_H
