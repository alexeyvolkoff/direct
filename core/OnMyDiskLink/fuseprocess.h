#ifndef FUSEPROCESS_H
#define FUSEPROCESS_H

#include <QObject>
#include <QProcess>
#include <QLocalServer>
#include <QSharedPointer>
#include <QUuid>
#include <QTimer>
#include "onmydisklink_global.h"
#include "linksocket.h"
#include "linkmessage.h"
#include "locallinkmessagereader.h"
#include "locallinkmessagewriter.h"

class LINK_API FuseProcess : public QProcess
{
    Q_OBJECT
public:
    explicit FuseProcess(QString userName, LinkSocket* clientSocket, QObject *parent = nullptr);
    ~FuseProcess();
    QString userName() const;
    LinkMessage *SendMessage(LinkMessage *message);
    void startConnected(const QString& program, const QStringList &arguments);
    void RedirectClient(LinkSocket* client);

signals:
    void timeOut();

public slots:
    void readyRead();

private:
    QString m_userName;
    QLocalServer m_LocalServer;
    QLocalSocket* m_processLink;
    QString m_socketPath;
    QTimer m_startupTimer;
    LinkSocket* m_clientSocket;
    quint16 m_linkPort;

private slots:
    void appConnection();
};

#endif // FUSEPROCESS_H
