#include "onmydisklink.h"
#include "version.h"

OnMyDiskLink::OnMyDiskLink(const QString &linkID, const QString &user,  LinkSocket* socket): QObject(nullptr),
    m_linkID(linkID), m_user(user), m_socket(socket),  m_state(Disconnected)
{
    connect(&m_timer, &QTimer::timeout, this, &OnMyDiskLink::pingTimeout);
    connect(&m_inactivityTimer, &QTimer::timeout, this, &OnMyDiskLink::inactivityTimeout);
    m_timer.setSingleShot(true);
    m_inactivityTimer.setSingleShot(true);
    m_timer.setInterval(3*PING_INTERVAL + PING_TIMEOUT); // client pings with PING_INTERVAL, give it 3 intervals + timeout margin
    m_inactivityTimer.setInterval(LINK_INACTIVITY_TIMEOUT); // time since last user activity.
}

OnMyDiskLink::~OnMyDiskLink()
{
    m_timer.stop();
    m_inactivityTimer.stop();
    if (m_socket)
        delete m_socket;
}

void OnMyDiskLink::inactivityTimeout()
{
    OnMyDiskLocker locker(&m_mutex);
    if (!locker.isLocked()) {
         qWarning() << "Could not lock while inactivityTimeout" << m_linkID;
    }

    if (m_state != Inactive) {
        m_state = Inactive;
        if (!tunnelOnly())
            qInfo() << "Link inactive:" << m_linkID << "user:" << m_user;
        m_inactivityTimer.stop();
        emit linkInactive();
    }
}

void OnMyDiskLink::disconnectLink()
{
    OnMyDiskLocker locker(&m_mutex);
    if (!locker.isLocked()) {
         qWarning() << "Could not lock while disconnectLink" << m_linkID;
    }

    //disconnect on readyRead event
    if (m_socket && m_socket->state() == QAbstractSocket::ConnectedState) {
        m_socket->setEmitMessages(false);
        m_socket->DisconnectFromCloud();
    }

    m_state = Disconnected;
    emit this->linkReleased();
}


void OnMyDiskLink::pingTimeout()
{
    qWarning() << "Link timeout:" << m_linkID << "user:" << m_user;
    m_timer.stop();
    m_inactivityTimer.stop();
    m_socket->disconnectFromHost();
}

OnMyDiskLink::ServerLinkState OnMyDiskLink::state()
{
    OnMyDiskLocker locker(&m_mutex);
    if (!locker.isLocked()) {
         qWarning() << "Could not lock in OnMyDiskLink::state()" << m_linkID;
    }
    return m_state;
}


void OnMyDiskLink::setSearchSupported(bool searchSupported)
{
    OnMyDiskLocker locker(&m_mutex);
    if (!locker.isLocked()) {
        qWarning() << "Could not lock while setSearchSupported" << m_linkID;
        return;
    }
    m_searchSopported = searchSupported;
}

void OnMyDiskLink::addShare(QString sharePath)
{
    LinkMessage msg;
    msg.setMessageType(LinkMessage::AddShare);
    msg.insertDataValue("sharePath", sharePath);
    m_socket->writeMessage(msg);
}

void OnMyDiskLink::removeShare(QString sharePath)
{
    LinkMessage msg;
    msg.setMessageType(LinkMessage::RemoveShare);
    msg.insertDataValue("sharePath", sharePath);
    m_socket->writeMessage(msg);
}

void OnMyDiskLink::release()
{
    OnMyDiskLocker locker(&m_mutex);
    if (!locker.isLocked()) {
         qWarning() << "Could not lock while releasing link" << m_linkID;
    }
    m_timer.stop();
    m_inactivityTimer.stop();
    m_state = Disconnected;
    if (!tunnelOnly())
        qInfo() << "Link disconnected:" << m_linkID << "user:" << m_user;
    emit this->linkReleased();
}

void OnMyDiskLink::invoke(const QString &cmd, const QVariantHash &params)
{
    LinkMessage message;
    message.setMessageType(LinkMessage::RPC);
    message.insertDataValue("cmd", cmd);
    message.insertDataValue("params", params);
    m_socket->writeMessage(message);
}

void OnMyDiskLink::initLink(LinkSocket *socket)
{
    OnMyDiskLocker locker(&m_mutex);
    if (!locker.isLocked()) {
         qWarning() << "Could not lock in initLink" << m_linkID;
    }
    if (m_socket && m_socket != socket) {
        if (m_socket->state() == QAbstractSocket::ConnectedState)
            m_socket->disconnectFromHost();
        m_socket->deleteLater();
    }
    m_socket = socket;
    if (m_state == Disconnected) {
        m_state = Inactive;
        locker.unlock();
        //Single-socket link: just send host info with the count of 1 data socket
        LinkMessage msg;
        msg.setMessageType(LinkMessage::ConnectDataSockets);
        msg.insertDataValue("maxConnections", 0); //single command socket, no data sockets
        m_socket->writeMessage(msg);
    } else {
        locker.unlock();
    }
}

void OnMyDiskLink::startTimers()
{
    if (QThread::currentThread() != this->thread()) {
           // Called from another thread
            QMetaObject::invokeMethod(this, "startTimers", Qt::QueuedConnection);
    }
    else {
        m_timer.start();
    }
}

void OnMyDiskLink::stopTimers()
{
    if (QThread::currentThread() != this->thread()) {
            // Called from another thread
            QMetaObject::invokeMethod(this, "stopTimers", Qt::QueuedConnection);
    }
    else {
        m_timer.stop();
    }
}
