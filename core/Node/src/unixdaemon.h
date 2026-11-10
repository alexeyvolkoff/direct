#ifndef UNIXDAEMON_H
#define UNIXDAEMON_H

#include <QSocketNotifier>
#include <QLockFile>
#include "signal.h"


class UnixDaemon : public QObject
{
    Q_OBJECT

public:
    UnixDaemon(QString pidFile, QObject *parent = nullptr);
    bool TryLock(int timeout = 0);
    ~UnixDaemon();

    // Unix signal handlers.
    static void intSignalHandler(int unused);
    static void hupSignalHandler(int unused);
    static void termSignalHandler(int unused);

public slots:
    // Qt signal handlers.
    void handleSigInt();
    void handleSigHup();
    void handleSigTerm();
signals:
    void sigTERM();
    void sigHUP();
    void sigINT();

private:
    static int sigintFd[2];
    static int sighupFd[2];
    static int sigtermFd[2];

    QSocketNotifier *snInt;
    QSocketNotifier *snHup;
    QSocketNotifier *snTerm;
    QLockFile m_lockFile;
};


#endif // UNIXDAEMON_H
