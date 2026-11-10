#include "locallinkmessagewriter.h"

LocalLinkMessageWriter::LocalLinkMessageWriter(QLocalSocket *io)
{
    buffer.open(QIODevice::WriteOnly);
    this->io = io;
    _stream.setVersion(QDataStream::Qt_5_4);
    _stream.setDevice(&buffer);
    // Placeholder for the size. We will get the value
    // at the end.
    _stream << quint64(0);
}

bool LocalLinkMessageWriter::Write(LinkMessage* message)
{
    _stream << *message;
    // Write the real size.
    _stream.device()->seek(0);
    _stream << (quint64) buffer.size();

    if (!io || !io->isWritable())
        return false;

    try
    {
        // Flush to the device.
        //qInfo() << "Sending buffer with size: " << buffer.size();
        //qInfo() << "Buffer content: " << buffer.buffer();
        //qint64 bytesSent =
         io->write(buffer.buffer());
        //qInfo() << "Bytes written: " << bytesSent;
        //io->flush();
        return io->waitForBytesWritten(30000);
    }
    catch(...)
    {
        qCritical() << "Error writing to socket";
    }

    return false;
}
