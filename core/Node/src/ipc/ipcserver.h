/******************************************************************************
**
**      ipcserver.h - On My Disk IPC server
**
**      Copyright 2017 On My Disk Team
**
**     Licensed under the Apache License, Version 2.0 (the "License");
**   you may not use this file except in compliance with the License.
**   You may obtain a copy of the License at
**
**       http://www.apache.org/licenses/LICENSE-2.0
**
**   Unless required by applicable law or agreed to in writing, software
**   distributed under the License is distributed on an "AS IS" BASIS,
**   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
**   See the License for the specific language governing permissions and
**   limitations under the License.
**
*******************************************************************************/

#ifndef CONSOLECLIENT_H
#define CONSOLECLIENT_H

#include <QObject>
#include "ipcclient.h"
#include <QTextStream>
#include <QIODevice>
#include <QLocalServer>
#include <QMutex>
#include "omd_common.h"



class IPCServer : public QObject
{
    Q_OBJECT

public:
    explicit IPCServer(bool isService, QObject *parent = Q_NULLPTR);
    ~IPCServer();

public slots:
    void stateChanged(OnMyDisk::ConnectionState status,  QString statusMessage);
    void certLoaded(QString userName, QString deviceId, QString linkId, QString validFrom, QString validTo);

private slots:
    //ipc communication with GUI client
    void serviceRequested();
    void broadcastResponse(ServiceResponse response);
    void clientDisconnected();
    void clientConnected();
    void setState(ServiceState state, QString message);
private:
    //User data
    QString m_user;
    QString m_deviceId;
    QString m_validTo;

    //IPC
    QLocalServer m_ipcServer;
    QList<QLocalSocket *>  m_clients;
    QMap<QLocalSocket *, QByteArray>  m_ipcBuffers;
    quint16 m_port;
    QRecursiveMutex m_mutex;
    //data
    ServiceState m_state;
    QString m_statusMessage;
    QString m_socketName;

    void writeSettings();
    void readSettings();

    void sendResponse(QLocalSocket *ipc_socket, ServiceResponse response);
    void sendStatus(QLocalSocket *ipc_socket);
    void sendTrying(QLocalSocket *ipc_socket);
    void handleServiceRequest(QLocalSocket *ipc_socket, ServiceRequest request);

signals:
    void addShare(QString share);
    void removeShare(QString share);
    void showStorage();
    void showCloud();
    void showShare(QString share);
    void connectClient();
    void parseUrl(QString share);
    void disconnectClient();
    void resetLink();
    void authRequest(QString user, QString password);
    void linkStorage();
    void quit();
};


#endif // CONSOLECLIENT_H
