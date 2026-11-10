#ifndef LOCALLINKMESSAGEWRITER_H
#define LOCALLINKMESSAGEWRITER_H

#include <QDataStream>
#include <QBuffer>
#include <QCoreApplication>
#include <QEventLoop>
#include <QDebug>
#include "onmydisklink_global.h"
#include "linkmessage.h"
#include <QLocalSocket>

class LocalLinkMessageWriter
{
public:
    LocalLinkMessageWriter(QLocalSocket *io);
    bool Write(LinkMessage *message);

private:
    QBuffer buffer;
    QDataStream _stream;
    QLocalSocket *io;
};

#endif // LOCALLINKMESSAGEWRITER_H
