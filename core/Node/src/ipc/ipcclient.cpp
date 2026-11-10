/******************************************************************************
**
**      ipcclient.cpp IPC implementation
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

#include "ipcclient.h"
#include <QDataStream>
#include <QByteArray>
#include <QDebug>



#if defined(Q_OS_WIN)
#include <windows.h>
#include <stdio.h>
#include <QLibrary>
#include "sddl.h"
#include <qt_windows.h>
typedef BOOL(WINAPI*PProcessIdToSessionId)(DWORD,DWORD*);
static PProcessIdToSessionId pProcessIdToSessionId = 0;
#endif
#if defined(Q_OS_UNIX)
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#endif

QDataStream & operator<<(QDataStream & ds, const ServiceRequest &message)
{
    ds.setVersion(QDataStream::Qt_5_0);
    quint32 msg = static_cast<quint32>(message.msg_type);
    quint32 size = static_cast<quint32>(message.msg_data.size());
    ds << msg;
    ds << size;
    ds.writeRawData(message.msg_data.data(), size);
    return ds;
}

QDataStream & operator>>(QDataStream & ds, ServiceRequest &message)
{
    //return if no data
    if (ds.atEnd()) return ds;
    ds.setVersion(QDataStream::Qt_5_0);
    quint32 msg;
    quint32 size;
    ds >> msg;
    ds >> size;
    message.msg_data.resize(size);
    ds.readRawData(message.msg_data.data(), size);
    message.msg_type = static_cast<RequestType>(msg);
    return ds;
}

QDataStream & operator<<(QDataStream & ds, const ServiceResponse &message)
{
    ds.setVersion(QDataStream::Qt_5_0);
    quint32 msg = static_cast<quint32>(message.msg_type);
    quint32 size = static_cast<quint32>(message.msg_data.size());
    ds << msg;
    ds << size;
    ds.writeRawData(message.msg_data.data(), size);
    return ds;
}



QDataStream & operator>>(QDataStream & ds, ServiceResponse &message)
{
    //return if no data
    if (ds.atEnd()) return ds;
    ds.setVersion(QDataStream::Qt_5_0);
    quint32 msg;
    quint32 size;
    ds >> msg;
    ds >> size;
    message.msg_data.resize(size);
    ds.readRawData(message.msg_data.data(), size);
    message.msg_type = static_cast<ServiceState>(msg);
    return ds;
}


QDataStream & operator>>(QDataStream & ds, MessageHead &message)
{
    //return if no data
    if (ds.atEnd()) return ds;
    ds.setVersion(QDataStream::Qt_5_0);
    ds >> message.msg_type;
    ds >> message.msg_size;
    return ds;
}



ServiceRequest::operator QString () const
{
   QString result;
   switch (msg_type)
   {
        case RequestType::GetStatus:
        {
            result = msg_data.isEmpty()?  "GetStatus." : "GetStatus: " + QString::fromUtf8(msg_data);
            break;
        }
        case RequestType::GetShares:
        {
            result = msg_data.isEmpty()?  "GetShares." : "GetShares: " + QString::fromUtf8(msg_data);
            break;
        }

        case RequestType::AddShare:
        {
            result = "AddShare: " + QString::fromUtf8(msg_data);
            break;
        }
        case RequestType::RemoveShare:
        {
            result = "RemoveShare: " + QString::fromUtf8(msg_data);
            break;
        }
        case RequestType::Show:
        {
            result = "Show";
            break;
        }

        default: result = "Unknown";
   }
   return result;
}


ServiceResponse::operator QString () const
{
   QString result;
   switch (msg_type)
   {
        case ServiceState::CertNeeded:
        {
            result = msg_data.isEmpty()?  "CertNeeded." : "CertNeeded: " + QString::fromUtf8(msg_data);
            break;
        }
        case ServiceState::Connected:
        {
            result = msg_data.isEmpty()?  "Connected." : "Connected: " + QString::fromUtf8(msg_data);
            break;
        }
        case ServiceState::Connecting:
        {
            result = msg_data.isEmpty()?  "Connecting..." : "Connecting: " + QString::fromUtf8(msg_data);
            break;
        }
        case ServiceState::ConnectionError:
        {
            result = msg_data.isEmpty()?  "ConnectionError." : "ConnectionError: " + QString::fromUtf8(msg_data);
            break;
        }
        case ServiceState::ShareAdded:
        {
            result = "ShareAdded: " + QString::fromUtf8(msg_data);
            break;
        }
        case ServiceState::ShareRemoved:
        {
            result = "ShareRemoved: " + QString::fromUtf8(msg_data);
            break;
        }
        case ServiceState::ShareList:
        {
            result = "Shares: " + QString::fromUtf8(msg_data);
            break;
        }
        case ServiceState::ShowGUI:
        {
            result = "ShowGUI";
            break;
        }
        default: result = "Unknown: " + QString::number((ulong) msg_type ) + " " + QString::fromUtf8(msg_data);;
   }
   return result;
}


QString getIPCSuffix(bool service)
{
    QString suffix = "";
    if (! service)
    {
    #if defined(Q_OS_WIN)
        if (!pProcessIdToSessionId) {
            QLibrary lib("kernel32");
            pProcessIdToSessionId = (PProcessIdToSessionId)lib.resolve("ProcessIdToSessionId");
        }
        if (pProcessIdToSessionId) {
            DWORD sessionId = 0;
            pProcessIdToSessionId(GetCurrentProcessId(), &sessionId);
            suffix = QString("-") + QString::number(sessionId, 16);
        }
    #else
        suffix = QString("-") + QString::number(::getuid(), 16);
    #endif
   }

  return suffix;
}


QString getIPCSocketName(bool service)
{

#ifdef Q_OS_UNIX
    QString socketName = "/tmp/" + QString(OMD_SERVICE_NAME);
#else
    QString socketName = QString(OMD_SERVICE_NAME);
#endif

  return socketName + getIPCSuffix(service);
}

bool isService()
{
 #if defined(Q_OS_WIN)
        DWORD dwSize = 0, dwResult = 0;
        HANDLE hToken;
        PTOKEN_USER Ptoken_User = NULL;
        LPWSTR SID = NULL;
        bool result = false;

        // Open a handle to the access token for the calling process.
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken))
        {
            qDebug("OpenProcessToken Error %u\n", GetLastError());
            goto Exit;
        }

        // Call GetTokenInformation to get the buffer size.
        if (!GetTokenInformation(hToken, TokenUser, NULL, dwSize, &dwSize))
        {
            dwResult = GetLastError();
            if (dwResult != ERROR_INSUFFICIENT_BUFFER)
            {
                qDebug("GetTokenInformation Error %u\n", dwResult);
                goto Exit;
            }
        }

        // Allocate the buffer.
        Ptoken_User = (PTOKEN_USER)GlobalAlloc(GPTR, dwSize);

        // Call GetTokenInformation again to get the group information.
        if (!GetTokenInformation(hToken, TokenUser, Ptoken_User, dwSize, &dwSize))
        {
            qDebug("GetTokenInformation Error %u\n", GetLastError());
            goto Exit;
        }

        if (!ConvertSidToStringSidW(Ptoken_User->User.Sid, &SID))
        {
            qDebug("ConvertSidToStringSidW failed. Error = %d", GetLastError());
            goto Exit;
        }


        if (_wcsicmp(L"S-1-5-18", SID) == 0)
        {
            qDebug("\nRunning under SYSTEM");
            result = true;
        }

    Exit:
        if (Ptoken_User) GlobalFree(Ptoken_User);
        return result;
#else
    return (::getuid() == 0); //root
#endif 

}


IPCClient::IPCClient(QObject *parent): QLocalSocket(parent)
{
    connect(this, SIGNAL(readyRead()), this, SLOT(responseReceived()));
}



void IPCClient::request(ServiceRequest r)
{
    QDataStream ds(this);
    ds << r;
    waitForBytesWritten(1000);
    flush();
}

ServiceResponse IPCClient::service(ServiceRequest r)
{
  request(r);
  return response();
}

ServiceResponse IPCClient::response()
{

    //header size
    int sizeOfHead = 2 * sizeof(quint32);
    //readout the buffer
    QMutexLocker locker(&m_mutex);
    while (m_buffer.size() < sizeOfHead)
    {
        locker.unlock();
        waitForReadyRead(1000);
        locker.relock();
    }

    QDataStream ds(m_buffer);
    MessageHead head;
    ds >> head; // read header from the begginning of buffer

    while (head.msg_size > (quint32) m_buffer.size() - sizeOfHead )
    {
        locker.unlock();
        waitForReadyRead(1000);
        locker.relock();
    }


    // now read the complete message from buffer
    ds.device()->reset();
    ServiceResponse response;
    ds >> response;
    m_buffer.remove(0, head.msg_size + sizeOfHead); //remove the complete message from buffer

    return response;

}

void IPCClient::responseReceived()
{
    //first read all;
    QMutexLocker locker(&m_mutex);
    m_buffer += readAll(); // store bytes to buffer
}


IPCClient::~IPCClient()
{

}
