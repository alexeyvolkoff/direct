#include "linkcommunicator.h"
#include "cloudresource.h"


#ifdef USE_MEM_POOL
#include "pool.hpp"
#endif

LinkMessageCommunicator::LinkMessageCommunicator(LinkSocket *io, int timeout):QObject(nullptr)
{
    readOK = false;
    readError = false;
    m_timeout = timeout;
	retryCount = 0;
    blockSize = 0;
    messageSize = 0;
	bytesAvailable = 0;
    connect(io, &LinkSocket::readyRead, this, &LinkMessageCommunicator::dataReady);
    this->io = io;
}


void LinkMessageCommunicator::dataReady()
{
	//sanity check
	if (! (io && io->isValid() && io->state() == QAbstractSocket::ConnectedState))
	{
		qCritical() << "Socket closed" ;
		readError = true;
		return;
	}

    quint64 bytes  = io->bytesAvailable();

    if (bytes == 0) return;

	// Read the size.
    if ( blockSize == 0)
    {
		//Read block size
        buffer.append(io->read(sizeof(quint64)));
		QDataStream s(buffer);
		s.setVersion(QDataStream::Qt_5_4);
		s >> blockSize;

		//Set message size
        messageSize = blockSize - sizeof(quint64);
        //qInfo() << this << "Reading buffer with size: " << messageSize << io;
    }

    bytes  = io->bytesAvailable();
    //qInfo() << this << "Bytes available:" <<  bytes << ", required:" << messageSize ;
    if (bytes >= messageSize)
    {
        //Read the message, leave the remeining bytes for queued read
        buffer.append(io->read(messageSize));
       // disconnect(io, &LinkSocket::readyRead, this, &LinkMessageCommunicator::dataReady);

        //Check if complete message is available
        //if ((quint64) buffer.size() >= blockSize)
        //{
        readOK = true;
        //m_wait.wakeAll();
        disconnect(io, &LinkSocket::readyRead, this, &LinkMessageCommunicator::dataReady);
        m_loop.quit();
    }
}



LinkMessage LinkMessageCommunicator::sendMessage(const LinkMessage& outMessage)
{

    LinkMessage message;
    message.setSenderID(io->getPeerID());

	//qInfo() << "Sending:" << outMessage.getMessageType();
	//send message
	if (! send(outMessage) )
    {
		qCritical() << "Write error for" << io->getPeerID();
		message.setResultCode(LinkMessage::LinkError);
		return message;
    }

    retryCount = 0;
    int retryLimit = m_timeout / 1000;

    //qInfo() << "Bytes available:" <<  io->bytesAvailable() ;

    if (!readOK) {
        m_loop.exec();
    }

	//Wait for remeining response

    /*
    while (!readOK && !readError && io && io->isValid() && io->state() == QAbstractSocket::ConnectedState)
    {
		if (io->bytesAvailable()) {
			//qWarning() << "Already got response" << io->getPeerID();
			dataReady();
		}
		// wait if we didn't receive the complete buffer
		if (!readOK) {
            if ((qint64) bytesAvailable == io->bytesAvailable())
				io->waitForReadyRead(1000);
            if ((qint64) bytesAvailable == io->bytesAvailable())
				retryCount++;
			readError = retryCount == retryLimit;
		}
		bytesAvailable = io->bytesAvailable();
    }*/


	if (readOK)
	{
		QDataStream s(buffer);
		s.setVersion(QDataStream::Qt_5_4);
		s >> blockSize;
		s >> message;

		if (message.getMessageType() != m_type)
		{
			if (message.getMessageType() != LinkMessage::PingReply)
				qWarning() << "Expected" << m_type << "got" << message.getMessageType();
		}

       //qInfo() << "Received:" << message.getMessageType() << ", remaining:" << io->bytesAvailable();
			//someone else is waiting
	}
	else
	{
        qCritical() << "Read error" << m_type << io << "error:" << io->errorString();
		message.setResultCode(LinkMessage::LinkError);
	}

   // qInfo() << "Read OK for" <<io->getPeerID();
    return message;
}


bool LinkMessageCommunicator::send(const LinkMessage &message)
{
	// do some sanity checks
	if (!io || !io->isValid() || io->state() != QAbstractSocket::ConnectedState)
		return false;

	m_type = message.getMessageType();

    //qInfo() << "Sending" << m_type;
	//write buffer
	_outStream.setVersion(QDataStream::Qt_5_4);
	_outStream.setDevice(&outBuffer);
    outBuffer.open(QIODevice::WriteOnly);
    _outStream << quint64(0);

	_outStream << message;
	// Write the real size.
	_outStream.device()->seek(0);
    _outStream << (quint64) outBuffer.size();

    try
    {
		// Flush to the device.
		//qInfo() << "Sending buffer with size: " << buffer.size();
		int bytesSent = io->write(outBuffer.buffer());
		//qInfo() << "Bytes written: " << bytesSent;
        //io->waitForBytesWritten();
        io->flush();

        //outBuffer.buffer().clear();
        //outBuffer.buffer().squeeze();
		return bytesSent != -1;
    }
    catch(...)
    {
		qCritical() << "Error writing to socket";
    }

    return false;
}

LinkMessageCommunicator::~LinkMessageCommunicator()
{

}


