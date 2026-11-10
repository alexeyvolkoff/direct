/******************************************************************************
**
**      ipcclient.h - On My Disk System Service (daemon) communication protocol
**
**      Copyright 2016 On My Disk Team
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
#ifndef IPC_CLIENT_H
#define IPC_CLIENT_H

#define OMD_SERVICE_NAME "omdservice"

#include <QObject>
#include <QLocalSocket>
#include <QMutex>



enum class ServiceState: quint32
{
        Connecting = 0, //service is connecting, just wait
        CertNeeded,     //service needs credentials or pin
        ConnectionError, //error, message is provided in data
        Connected,      //service is connected
        Disconnected,    //disconnected
        ShareAdded,     //shared dir added
        ShareRemoved,    //shared dir removed
        ShareList,      //share list returned in data
        ShowGUI
} ;


enum class RequestType: quint32
{
        GetStatus = 0,  //returns ServiceStatus
        GetShares,      //list of shares
        AddShare,       //add nshare
        RemoveShare,    //remove share
        Show,           //Show GUI
        Connect,
        Disconnect,
        Link,
        Url,
        Auth,
        Quit
};


class ServiceRequest
{

public:

    //constructors
    ServiceRequest(): msg_type(RequestType::GetStatus) {}
    ServiceRequest(RequestType msg): msg_type(msg) {}
    ServiceRequest(RequestType msg, QByteArray data): msg_type(msg),  msg_data(data)  {}
    ServiceRequest(RequestType msg, QString data): msg_type(msg),  msg_data(data.toUtf8())  {}
    ServiceRequest(RequestType msg, QStringList data): msg_type(msg),  msg_data(data.join(";").toUtf8()) {}
    ServiceRequest(const ServiceRequest& message):  msg_type(message.msg_type), msg_data(message.msg_data) {}
    //data
    RequestType msg_type;
    QByteArray msg_data;
    //operators
    friend QDataStream & operator<<(QDataStream & ds, const ServiceRequest &message);
    friend QDataStream & operator>>(QDataStream & ds, ServiceRequest &message);
    operator QString() const;

};


class ServiceResponse
{
public:
    //constructors
    ServiceResponse(): msg_type(ServiceState::Connecting) {}
    ServiceResponse(ServiceState msg): msg_type(msg) {}
    ServiceResponse(ServiceState msg, QByteArray data = QByteArray()): msg_type(msg),  msg_data(data)  {}
    ServiceResponse(ServiceState msg, QString data = ""): msg_type(msg),  msg_data(data.toUtf8())  {}
    ServiceResponse(QStringList data): msg_type(ServiceState::ShareList),  msg_data(data.join(";").toUtf8()) {}
    ServiceResponse(const ServiceResponse& message): msg_type(message.msg_type), msg_data(message.msg_data) {}
    //data
    ServiceState msg_type;
    QByteArray msg_data;
    //operators
    friend QDataStream & operator<<(QDataStream & ds, const ServiceResponse &message);
    friend QDataStream & operator>>(QDataStream & ds, ServiceResponse &message);
    operator QString() const;


};


//just to read message header
class MessageHead
{
public:
    //data
    quint32 msg_type;
    quint32 msg_size;
    //operators
    friend QDataStream & operator>>(QDataStream & ds, MessageHead &message);
};





class IPCClient: public QLocalSocket
{

    Q_OBJECT
private:
    QMutex m_mutex;
    QByteArray m_buffer;
public:
    explicit IPCClient(QObject * parent = Q_NULLPTR);
    ~IPCClient();
    //Async request
    void   request(ServiceRequest r);
    //Blocking request
    ServiceResponse service(ServiceRequest r);
    //Blocking read
    ServiceResponse response();
private slots:
    void responseReceived();
};


QString getIPCSuffix(bool service);
QString getIPCSocketName(bool service);

bool isService();

#endif // OMD_SERVICE_H
