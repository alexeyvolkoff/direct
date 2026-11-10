#include "locallinkmessagereader.h"

LocalLinkMessageReader::LocalLinkMessageReader(QLocalSocket *io)
{
    readOK = false;
    this->io = io;
    buffer.open(QIODevice::ReadWrite);
    _stream.setVersion(QDataStream::Qt_5_4);
    _stream.setDevice(&buffer);


    // Read the size.
    if (readMax(io, sizeof(blockSize)) > 0)
    {
        buffer.seek(0);

        _stream >> blockSize;
        //qInfo() << "Reading buffer with size: " << blockSize;
        // Read the rest of the data.
        if (readMax(io, blockSize) > 0)
        {
            readOK = buffer.seek(sizeof(blockSize));
        }
    }
    else
        qWarning() << "Can't read buffer size";
}

/*
LinkMessage* LocalLinkMessageReader::Read()
{
    //qInfo() << "Buffer size: " << buffer.size();
    //qInfo() << "Buffer content: " << buffer.buffer();
    LinkMessage* message = new LinkMessage();
    message->setSenderID(QString("local:%1").arg(io->fullServerName()));
    if (readOK)
        _stream >> *message;
    else
    {
        message->setResultCode(LinkMessage::LinkError);
    }
    return message;
}*/

int LocalLinkMessageReader::readMax(QLocalSocket *io, int n)
{
    try
    {
        int bytes_read = 0;
        while (io->isReadable() && (buffer.size() < n)  )
        {
            if (!io->bytesAvailable()) {
                if (!io->waitForReadyRead(30000)){
                    qWarning() << "Blockreader timeout.";
                    return -1;
                }
            }
            QByteArray bytes = io->read(n - buffer.size());
            if (bytes.count() == 0)
            {
                qWarning() << "Blockreader error while reading from socket.";
                return -1;
            }

            int bcount = buffer.write(bytes);
            if (bcount == -1)
            {
                qWarning() << "Blockreader error while writing to buffer.";
                return -1;
            }
            bytes_read += bcount;
            // qDebug() << "buffer size: " << buffer.size() << ", we need: " << n;
        }

        if (!io->isReadable())
        {
            qWarning() << "Socket was closed while reading";
            return -1;
        }
        else
            return bytes_read;
    }
    catch(...)
    {
        qWarning() << "Error reading from socket";
    }

    return -1;
}
