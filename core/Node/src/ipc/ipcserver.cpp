/******************************************************************************
**
**      IPCServer.cpp - On My Disk IPC server
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

#include "ipcserver.h"
#include <QLocalSocket>
#include <QStorageInfo>
#include <QCoreApplication>
#include <QDataStream>
#include <ipcclient.h>

IPCServer::IPCServer(bool isService,  QObject *parent)
  : QObject(parent)
{

    m_socketName = getIPCSocketName(isService);

    #ifdef Q_OS_UNIX
    QFile::remove(m_socketName);
    #endif

    if (isService)
    {
        m_ipcServer.setSocketOptions(QLocalServer::WorldAccessOption);
    }
    //IPC requests listener
    if (! m_ipcServer.listen(m_socketName))
    {
       qCritical() << "Can not start IPC server on local socket/named pipe:" << m_socketName;
       qCritical() << "Service is already running, exit";
       exit(-1);
    }

    connect(&m_ipcServer, SIGNAL(newConnection()), this, SLOT(clientConnected()));

    m_state = ServiceState::Connecting;
    m_statusMessage = tr("Service started");
}



void IPCServer::clientConnected()
{
    QLocalSocket *ipc_socket = m_ipcServer.nextPendingConnection();
    connect(ipc_socket, SIGNAL(readyRead()), this, SLOT(serviceRequested()));
    connect(ipc_socket, SIGNAL(disconnected()), this, SLOT(clientDisconnected()));
    m_clients.append(ipc_socket);
    m_ipcBuffers[ipc_socket].clear(); //create and clear bufffer for connection
}

void IPCServer::clientDisconnected()
{
    QLocalSocket *ipc_socket =  static_cast<QLocalSocket *> (sender());
    m_clients.removeOne(ipc_socket);
    m_ipcBuffers.remove(ipc_socket); //remove buffer
    ipc_socket->deleteLater();
}


void IPCServer::setState(ServiceState state, QString message)
{
    QMutexLocker locker(&m_mutex);
    if (m_state != state  )
    {
        m_state = state;
        m_statusMessage = message;
    }
    locker.unlock();
    //todo signal event
}




void IPCServer::handleServiceRequest(QLocalSocket *ipc_socket, ServiceRequest request)
{

    switch (request.msg_type)
    {
        case RequestType::GetStatus:
        {
            sendStatus(ipc_socket);
            break;
        }

        case RequestType::Show:
        {
            emit showStorage();
            break;
        }

        case RequestType::AddShare:
        {
            QString share = QString::fromUtf8(request.msg_data.data());

            emit addShare(share);
            emit showShare(share);
            break;
        }

        case RequestType::RemoveShare:
        {
            QString share = QString::fromUtf8(request.msg_data.data());

            emit removeShare(share);
            break;
        }

        case RequestType::Connect:
        {
            emit connectClient();
            break;
        }

        case RequestType::Disconnect:
        {
            emit disconnectClient();
            break;
        }
        case RequestType::Auth:
        {
            QString strAuth = QString::fromUtf8(request.msg_data.data());
            int p = strAuth.indexOf(":");
            if (p == -1) {
                ServiceResponse response(ServiceState::ConnectionError, tr("Invalid authentication"));
                sendResponse(ipc_socket, response);
                return;
            }
            QString user = strAuth.left(p);
            QString password = strAuth.mid(p + 1);

            //Request auth
            emit authRequest(user, password);
            sendTrying(ipc_socket);

            break;
        }
        case RequestType::Link:
        {
            emit linkStorage();
            break;
        }

        case RequestType::Url:
        {
            QString url = QString::fromUtf8(request.msg_data.data());
            emit parseUrl(url);
            break;
        }

        case RequestType::Quit:
        {
            emit disconnectClient();
            emit quit();
            break;
        }
        default:   break;
    }

}

//New request received
void IPCServer::serviceRequested()
{

    QLocalSocket *ipc_socket =  static_cast<QLocalSocket *> (sender());
    //header size
    int sizeOfHead = 2 * sizeof(quint32);

    //first read all;
    QMutexLocker locker(&m_mutex);
    m_ipcBuffers[ipc_socket] += ipc_socket->readAll(); // store bytes to buffer

    while (m_ipcBuffers[ipc_socket].size())
    {

        //readout the buffer

        if ( m_ipcBuffers[ipc_socket].size() < sizeOfHead) break; //nothing to read yet
        QDataStream ds(m_ipcBuffers[ipc_socket]);
        MessageHead head;
        ds >> head; // read header from the begginning of buffer
        if (head.msg_size + sizeOfHead > (quint32) m_ipcBuffers[ipc_socket].size() ) break; //  not complete message yet

        // now read the complete request from buffer
        ds.device()->reset();
        ServiceRequest request;
        ds >> request; //read block
        ds.resetStatus();

        m_ipcBuffers[ipc_socket].remove(0, head.msg_size + sizeOfHead); //remove the complete message from buffer

        handleServiceRequest(ipc_socket, request);

  }
  locker.unlock();
}

void IPCServer::sendResponse(QLocalSocket* ipc_socket, ServiceResponse response)
{
    QDataStream out(ipc_socket);
    out.setVersion(QDataStream::Qt_5_0);
    out << response;

    if (! ipc_socket->waitForBytesWritten())
    {
        qCritical() << "SendResponse: Data not sent!";
    }

    ipc_socket->flush();

}

void IPCServer::sendStatus(QLocalSocket *ipc_socket)
{
    QStringList statusMessage;
    statusMessage << m_statusMessage;
    statusMessage << "User: " + m_user;
    statusMessage << "DeviceId: " + m_deviceId;
    statusMessage << "Valid to: " + m_validTo;
    ServiceResponse response(m_state, statusMessage.join(";"));
    sendResponse(ipc_socket, response);
}

void IPCServer::sendTrying(QLocalSocket *ipc_socket)
{
    QStringList statusMessage;
    statusMessage << "\n";
    statusMessage << "Authorizing, please check status later...";
    ServiceResponse response(m_state, statusMessage.join(";"));
    sendResponse(ipc_socket, response);
}

//Sends response/notification to  all connected GUI  clients
void IPCServer::broadcastResponse(ServiceResponse response)
{
    foreach (QLocalSocket* ipc_socket , m_clients) {
       sendResponse(ipc_socket, response);
    }
}


IPCServer::~IPCServer()
{
#ifdef Q_OS_UNIX
 QFile::remove(m_socketName);
#endif
}


void IPCServer::stateChanged(OnMyDisk::ConnectionState status, QString statusMessage)
{
    ServiceState state = ServiceState::Connecting;
    switch (status) {
        case OnMyDisk::Connecting: state = ServiceState::Connecting; break;
        case OnMyDisk::Connected: state = ServiceState::Connected; break;
        case OnMyDisk::ConnectionError: state = ServiceState::ConnectionError; break;
        case OnMyDisk::Authorization: state = ServiceState::Connecting; break;
        case OnMyDisk::AuthorizationError: state = ServiceState::CertNeeded; break;
        case OnMyDisk::Disconnected: state = ServiceState::Disconnected; break;
    }
    setState(state, statusMessage);
}

void IPCServer::certLoaded(QString userName, QString deviceId, QString linkId, QString validFrom, QString validTo)
{
    Q_UNUSED(linkId)
    Q_UNUSED(validFrom)

    m_user = userName;
    m_deviceId = deviceId;
    m_validTo = validTo;
}
