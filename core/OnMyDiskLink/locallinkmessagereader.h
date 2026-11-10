#ifndef LOCALLINKMESSAGEREADER_H
#define LOCALLINKMESSAGEREADER_H

#include <QDataStream>
#include <QBuffer>
#include <QCoreApplication>
#include <QEventLoop>
#include <QDebug>
#include "onmydisklink_global.h"
#include "linkmessage.h"
#include <QLocalSocket>

class LocalLinkMessageReader
{
public:
    LocalLinkMessageReader(QLocalSocket *io);
    LinkMessage *Read();

private:
    // Blocking reads data from socket until buffer size becomes exactly n. No
    // additional data is read from the socket.
    int readMax(QLocalSocket *io, int n);
    QBuffer buffer;
    QDataStream _stream;
    QLocalSocket* io;
    bool readOK;
    quint64 blockSize;
};

#endif // LOCALLINKMESSAGEREADER_H
