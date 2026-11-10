#include "fuseprocess.h"

FuseProcess::FuseProcess(QString userName, LinkSocket *clientSocket, QObject *parent) : QProcess(parent), m_clientSocket(clientSocket)
{
    m_processLink = NULL;
    m_userName = userName;
    m_linkPort = 0;
    if (m_LocalServer.listen(QUuid::createUuid().toString()))
    {
        m_socketPath = m_LocalServer.fullServerName();
        qInfo() << "local link: " << m_socketPath;
        connect(&m_LocalServer, &QLocalServer::newConnection, this, &FuseProcess::appConnection);
    }
    connect(&m_startupTimer, &QTimer::timeout, this, &FuseProcess::timeOut);
}

FuseProcess::~FuseProcess()
{
    qInfo() << "Process is dead";
}

QString FuseProcess::userName() const
{
    return m_userName;
}

LinkMessage* FuseProcess::SendMessage(LinkMessage *message)
{
    LinkMessage *resp = new LinkMessage;
    resp->setResultCode(LinkMessage::LinkError);

    if (!m_processLink)
    {
        resp->setErrorMsg("App isn't connected yet");
        return resp;
    }

    if (!LocalLinkMessageWriter(m_processLink).Write(message))
    {
        resp->setErrorMsg("Unable to write message");
        return resp;
    }

    return LocalLinkMessageReader(m_processLink).Read();
}

void FuseProcess::startConnected(const QString &program, const QStringList &arguments)
{
    QStringList args(arguments);
    args << "--link" << m_socketPath;
    m_startupTimer.start(10000);
    this->start(program, args);
}

void FuseProcess::RedirectClient(LinkSocket *client)
{
    if (m_linkPort > 0)
    {
        qInfo() << "Redirecting to port " << m_linkPort;
        LinkMessage message;
        message.setMessageType(LinkMessage::SwitchPort);
        message.insertDataValue("fuseport", m_linkPort);
        if (client->writeMessage(message))
            qInfo() << "Redirect sent!";
        else
            qCritical() << "Unable to sent redirect";
    }


    if (client->state() == QAbstractSocket::ConnectedState)
        client->disconnectFromHost();
    client->deleteLater();
}

void FuseProcess::readyRead()
{
    LinkMessage* message = LocalLinkMessageReader(m_processLink).Read();

    switch (message->getMessageType()) {
    case LinkMessage::FuseMounted:
        if (message->getData().contains("port"))
        {
            bool isOk = false;
            m_linkPort = message->getData()["port"].toUInt(&isOk);
            if (!isOk)
                m_linkPort = 0;

            RedirectClient(m_clientSocket);
        }
        break;
    case LinkMessage::Null:
    case LinkMessage::BadCertificate:
    case LinkMessage::SwitchPort:
    case LinkMessage::Ping:
    case LinkMessage::PingReply:
    case LinkMessage::ConnectDataSockets:
    case LinkMessage::MkDir:
    case LinkMessage::Remove:
    case LinkMessage::FileInfo:
    case LinkMessage::FileOpen:
    case LinkMessage::FileClose:
    case LinkMessage::FilePos:
    case LinkMessage::FileSeek:
    case LinkMessage::FileRead:
    case LinkMessage::FileWrite:
    case LinkMessage::AddShare:
    case LinkMessage::RemoveShare:
        break;
    default:
        break;
    }
}

void FuseProcess::appConnection()
{
    qInfo() << "new sub-process connected";
    m_startupTimer.stop();
    m_processLink = m_LocalServer.nextPendingConnection();
    connect(m_processLink, &QLocalSocket::readyRead, this, &FuseProcess::readyRead);
}
