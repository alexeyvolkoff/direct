#include "linksocket.h"
#include "linkcommunicator.h"
#include "onmydisklinkserver.h"
#include <QHostAddress>
#include <QMutexLocker>
#include <QElapsedTimer>
#include <QCoreApplication>

#ifdef Q_OS_LINUX
#include <sys/types.h>
#include <sys/socket.h>  // For setsockopt and socket options
#include <netinet/in.h>
#include <netinet/tcp.h>
#endif

#ifdef Q_OS_WIN
#include <ws2tcpip.h>
#include <mstcpip.h>    // SIO_KEEPALIVE_VALS (per-connection keepalive tuning)
#endif

LinkSocket::LinkSocket(bool emitMessages):
    QSslSocket(nullptr)
{
    connect(this, &LinkSocket::encrypted, this, &LinkSocket::linkEncrypted);
    connect(&m_pingTimer, &QTimer::timeout, this, &LinkSocket::timeToPingSlot);
    m_blockSize = 0;
    m_state = lsInactive;
    m_working = false;
    m_emitMessages = emitMessages;
    m_type = LinkMessage::Null;
    m_requestCount = 0;
    m_responseType = LinkMessage::Null;
    connect(this, &LinkSocket::readyRead, this, &LinkSocket::dataReady);
}


LinkSocket::~LinkSocket()
{
    //qDebug() << "deleted" << this;
    //avoid warning message
}

QString LinkSocket::getPeerID() const
{
    return m_ID;
}



bool LinkSocket::doWrite(const LinkMessage &message)
{
    if (QThread::currentThread() != this->thread()) {
        qWarning() << "LinkSocket::doWrite called from foreign thread" << QThread::currentThread() << "socket thread" << this->thread() << "- queuing to socket thread";
        bool result = false;
        QMetaObject::invokeMethod(this, [this, message, &result]() {
            result = this->doWrite(message);
        }, Qt::BlockingQueuedConnection);
        return result;
    }

    if (state() != QAbstractSocket::ConnectedState) {
        qCritical() << "Socket closed before write" << this;
        return false;
    }

    m_outBuffer.clear();


    QDataStream _outStream (&m_outBuffer, QIODevice::ReadWrite);
    _outStream << quint64(0);
    _outStream << message;
    // Write the real size.
    _outStream.device()->seek(0);
    _outStream << (quint64) m_outBuffer.size();



    try
    {
        // Flush to the device.
        //qInfo() << "Sending:" << message.getMessageType();
        int bytesSent = write(m_outBuffer);
        //qInfo() << this << "Bytes written: " << bytesSent << message.getMessageType();
        //waitForBytesWritten();
        flush();

        m_outBuffer.clear();

        if (bytesSent == -1 || state() != QAbstractSocket::ConnectedState)
            qCritical() << "Socket write error" << this << state();
        return bytesSent != -1;
    }
    catch(...)
    {
        qCritical() << "Error writing to socket";
    }

    return false;
}


//use it only for commands with no response
bool LinkSocket::writeMessage(const LinkMessage &message)
{
    if (QThread::currentThread() != this->thread()) {
        QMetaObject::invokeMethod(this, "writeMessageQueued", Qt::QueuedConnection, Q_ARG(LinkMessage, message));
        return true;
    }

    // do some sanity checks
    QMutexLocker lock(&m_mutex);

    if (m_working)
    {
        qDebug() << "BUSY with" << m_type ;
    }

    return doWrite(message);
}



void LinkSocket::dataReady()
{
    // Serialize the whole receive path: this slot runs in the socket's owner
    // thread via readyRead, but is also invoked directly from other threads
    // (sendMessage) that deliberately do not hold m_mutex. Concurrent entry on
    // the single command socket races on m_buffer/m_msg and corrupts the heap
    // (double free), which previously surfaced as glibc abort in OpenSSL
    // BUF_MEM_grow_clean/CRYPTO_clear_realloc.
    QMutexLocker readLock(&m_readMutex);

    if (m_state == lsError) {
        readAll();
        return;
    }

    m_buffer.append(readAll());

    QDataStream _stream (&m_buffer, QIODevice::ReadOnly);
    if (m_blockSize == 0 && m_buffer.size() >= (qint64) sizeof(quint64))
    {
        //read size and avaliable data
        _stream.device()->seek(0);
        _stream >> m_blockSize;
        //qDebug() << "Reading block size: " << m_blockSize;
    }
    //qDebug() << "Bytes in buffer: " << buffer.size();
    while (isOpen() && m_blockSize > 0 &&  (quint64) m_buffer.size() >=  m_blockSize)
    {
        _stream.device()->seek(sizeof(quint64));
        //LinkMessage _response;
        m_msg.setSenderID(m_ID);

        _stream >> m_msg;

       // qDebug() << this << "Received: " << m_response.getMessageType() << m_emitMessages;
        m_buffer.remove(0, m_blockSize) ;
        m_buffer.squeeze();

        if (m_buffer.size() >= (qint64) sizeof(quint64)) {
            _stream.device()->seek(0);
            _stream >> m_blockSize;
        } else
            m_blockSize = 0;

        if (m_msg.getMessageType() == LinkMessage::Null) {
            qCritical() << "Empty response" << this << m_msg.getSenderID() << m_requestCount;
        }

        if (m_emitMessages || m_msg.emitable()) {
            // Keep m_working=true during streaming: writeReply/writeChunk need it.
            // It will be cleared by releaseSocket() when the stream ends.
            emit newMessage(this, m_msg);

            if (m_msg.getMessageType() == LinkMessage::Ping) {
                m_msg.setMessageType(LinkMessage::PingReply);
                m_msg.setResultCode(LinkMessage::OK);
                writeMessage(m_msg);
            }
            m_msg.clear();
        } else {
            m_responseList.append(m_msg);
        }
    }
}

void LinkSocket::releaseSocket()
{
    //Never block the caller: this can be called from the message handling chain
    //(newServiceMessage -> shareUpdated -> ...) or from object destructors, while
    //the socket is busy with a long operation on another thread (file read/write
    //holds the mutex up to 30 s). Blocking here locks the main thread in futex()
    //until the watchdog kills the process. Defer the release until the socket is
    //free instead of blocking.
    if (!m_mutex.tryLock()) {
        QMetaObject::invokeMethod(this, &LinkSocket::releaseSocket, Qt::QueuedConnection);
        return;
    }

    if (m_requestCount > 0)
        --m_requestCount;

    if (m_requestCount == 0)
        m_state = lsInactive;

    m_working = false;
    m_type = LinkMessage::Null;
    m_mutex.unlock();
    emit socketReady();
}

LinkMessage LinkSocket::sendMessage(const LinkMessage &message, int timeout)
{
    if (QThread::currentThread() != this->thread()) {
        LinkMessage response;
        bool ok = QMetaObject::invokeMethod(this, [this, message, timeout, &response]() {
            response = this->sendMessage(message, timeout);
        }, Qt::BlockingQueuedConnection);
        if (!ok) {
            response.setResultCode(LinkMessage::LinkError);
            response.setErrorMsg("Failed to invoke sendMessage across threads");
        }
        return response;
    }

    QMutexLocker lock(&m_mutex);
    if (m_state == lsError)
	{
		if (bytesAvailable())
		{
			qWarning() << "Cleanup after" << m_type << m_ID;
			readAll();
		}
	}

    LinkMessage response;
    if (m_working) {
        lock.unlock();
        QElapsedTimer elapsedTimer;
        elapsedTimer.start();
        while(m_working && state() == QAbstractSocket::ConnectedState && elapsedTimer.elapsed() < timeout && m_state != lsError) {
             waitForReadyRead(1000);
        }
        lock.relock();
    }
    if (m_working) {
        response.setResultCode(LinkMessage::LinkError);
        response.setErrorMsg("Link timeout.");
        qCritical() << this << this->thread() << "BUSY:"  << m_type << "requested" << message.getMessageType() << m_requestCount;
        return response;
    }
    bool wasReserved = (m_state == lsReserved);
    if (! message.fileOperation())
        m_state = lsActive;
    else
        m_state = lsOpen;

    m_working = true;
    m_emitMessages = false;
    if (!wasReserved)
        m_requestCount ++;


    doWrite(message);

    if (bytesAvailable()) {
        //Never run the message handling chain under the mutex: dataReady()
        //emits newMessage and handlers may call back into this socket
        //(releaseSocket(), etc.) or lock other resources - deadlock.
        lock.unlock();
        dataReady();
        lock.relock();
    }

    if (m_responseList.empty() )  {
        QElapsedTimer elapsedTimer;
        elapsedTimer.start();
        while(m_responseList.empty() && state() == QAbstractSocket::ConnectedState && elapsedTimer.elapsed() < timeout && m_state != lsError) {
            //Run unlocked: waitForReadyRead()/dataReady() may dispatch the
            //newMessage handling chain (service messages etc.) which must not
            //run while LinkSocket::m_mutex is held.
            lock.unlock();
            waitForReadyRead(1000);
            if (bytesAvailable())
                dataReady();
            lock.relock();
        }
    }

    bool ready = false;
    if (m_responseList.count() > 0) {
        response = m_responseList.takeFirst();
        m_responseType = response.getMessageType();
        if (response.getResultCode() == LinkMessage::LinkError)
            m_state = lsError;
        else  if (! response.fileOperation() ) {
            if (m_requestCount <= 1)
                m_state = lsInactive;
        }
         ready = (! response.fileOperation() || response.getResultCode() == LinkMessage::LinkError);
    } else {
        response.setResultCode(LinkMessage::LinkError);
        response.setErrorMsg("No response");
        m_state = lsError;
    }

    if (m_requestCount)
        --m_requestCount;
    m_working = false;

    if (ready) {
        lock.unlock();
        emit socketReady();
    }

    return response;
}

void LinkSocket::sendAsyncMessage(const LinkMessage &message)
{
    if (QThread::currentThread() != this->thread()) {
        QMetaObject::invokeMethod(this, "sendAsyncMessage", Qt::QueuedConnection, Q_ARG(LinkMessage, message));
        return;
    }

    // do some sanity checks
    QMutexLocker lock(&m_mutex);

    if (m_working)
    {
        qDebug() << "Async send: BUSY with" << m_type ;
    }

    bool wasReserved = (m_state == lsReserved);
    m_type = message.getMessageType();
    m_state = lsOpen;

    m_working = true;
    if (!wasReserved)
        m_requestCount++;
    doWrite(message);
}

void LinkSocket::setEmitMessages(bool emitMessages)
{
    m_emitMessages = emitMessages;
  //  qDebug() << this <<"set emit" << emitMessages;
    if (m_emitMessages) {
        m_outBuffer.reserve(2048);
        m_buffer.reserve(2048);
    }
}

void LinkSocket::startPingTimer(int timeout)
{
    if (QThread::currentThread() != this->thread()) {
        QMetaObject::invokeMethod(&m_pingTimer, [this, timeout]() {
            m_pingTimer.start(timeout);
        }, Qt::QueuedConnection);
        return;
    }
    m_pingTimer.start(timeout);
}

void LinkSocket::stopPingTimer()
{
    if (QThread::currentThread() != this->thread()) {
        QMetaObject::invokeMethod(&m_pingTimer, [this]() {
            m_pingTimer.stop();
        }, Qt::QueuedConnection);
        return;
    }
   m_pingTimer.stop();
}

LinkSocket::LinkSocketState LinkSocket::linkState()
{
   LinkSocket::LinkSocketState result = lsActive;
    if (m_mutex.tryLock(100)) {
        result = m_state;
        m_mutex.unlock();
    }
    return result;
}

bool LinkSocket::reserveSocket()
{   bool reserved = false;
    if (m_mutex.tryLock(100)) {
        if (m_state != lsOpen && m_state != lsReserved) {
        //if (m_state == lsInactive) {
            m_state = lsReserved;
            reserved = true;
        }
        m_mutex.unlock();
    } else
        reserved = false;
    return reserved;
}

bool LinkSocket::socketAvailable()
{   bool available = false;
    if (m_mutex.tryLock(100)) {
        if (m_state != lsOpen  && m_state != lsReserved) {
        //if (m_state == lsInactive) {
            available = true;
        }
        m_mutex.unlock();
    } else
        available = false;
    return available;
}

void LinkSocket::cleanup()
{
    QMutexLocker lock(&m_mutex);
    if (m_requestCount == 0 && m_responseList.count() == 0 && m_state != lsInactive) {
        m_state = lsInactive;
        m_working = false;
    }
    if (m_requestCount || m_state != lsInactive )
        qInfo() << this << m_state << m_requestCount << m_responseList.count() << "working:" << m_working;
}

void LinkSocket::stop()
{
    m_state = lsError;
    emit stopped();
}

void LinkSocket::setKeepAlive()
{
#ifdef Q_OS_LINUX
    uint socket = socketDescriptor();
    int optval = 1;
    socklen_t optlen = sizeof(optval);
    // Enable TCP keepalive
    setsockopt(socket, SOL_SOCKET, SO_KEEPALIVE, &optval, optlen);

    // Set keepalive idle time (seconds) - 15s keeps router NAT connection state active
    optval = 15;
    setsockopt(socket, IPPROTO_TCP, TCP_KEEPIDLE, &optval, optlen);

    // Set keepalive interval (seconds)
    optval = 5;
    setsockopt(socket, IPPROTO_TCP, TCP_KEEPINTVL, &optval, optlen);

    // Set the number of keepalive probes before considering the connection dead
    optval = 3;  // Total: 15s + 15s = 30s before fast failure
    setsockopt(socket, IPPROTO_TCP, TCP_KEEPCNT, &optval, optlen);
#else
#ifdef Q_OS_WIN
    // Windows: TCP_KEEPIDLE/TCP_KEEPINTVL/TCP_KEEPCNT setsockopt()s do not
    // exist; the per-connection idle/probe intervals must be set via
    // SIO_KEEPALIVE_VALS. With SO_KEEPALIVE only (the previous fallback) the
    // system default idle is 7200 s, so a NAT-expired control socket stayed
    // half-open for hours: the node went zombie and only a restart helped.
    // 15 s idle keeps NAT mappings alive; 5 s between probes; the probe count
    // is not configurable on Windows (fixed at 10) => detection in ~65 s.
    tcp_keepalive ka;
    ka.onoff = 1;
    ka.keepalivetime = 15000;      // ms
    ka.keepaliveinterval = 5000;   // ms
    DWORD bytesReturned = 0;
    qintptr rawSocket = socketDescriptor();
    if (rawSocket > 0)
    {
        WSAIoctl((SOCKET)rawSocket, SIO_KEEPALIVE_VALS,
                 &ka, sizeof(ka), nullptr, 0, &bytesReturned, nullptr, nullptr);
    }
#else
    setSocketOption(QAbstractSocket::KeepAliveOption, 1);
#endif
#endif

}


void LinkSocket::linkEncrypted()
{
    if (this->peerCertificate().isNull()) {
        //qWarning() << "No peer certificate. Port scanning (?) attempt from" << peerAddress().toString();
        return;
    }

    m_ID = QString(QCryptographicHash::hash(this->peerCertificate().toPem(), QCryptographicHash::Md5).toHex());

    if (m_ID == "7d6c72712d30cdba63eb8ae48bc8df88")
        m_ID = "8d80ffdd5ba03566e8e6178b7de9cd38";

    if (this->peerCertificate().subjectInfo(QSslCertificate::CommonName).count() > 0)
        m_user = this->peerCertificate().subjectInfo(QSslCertificate::CommonName).at(0);
}



void LinkSocket::timeToPingSlot()
{
    emit timeToPing();
}


void LinkSocket::DisconnectFromCloud()
{
    if (QThread::currentThread() != this->thread()) {
       // Called from another thread
        QMetaObject::invokeMethod(this, "DisconnectFromCloud", Qt::QueuedConnection);
    }
    else
    {
         //qDebug() << "Disconnecting socket";
         this->close();
    }
}
