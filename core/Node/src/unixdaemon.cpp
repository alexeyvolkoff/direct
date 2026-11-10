#include "unixdaemon.h"

#include <QDebug>

#include <sys/types.h>          /* See NOTES */
#include <sys/socket.h>
#include <unistd.h>

//needed to not get an undefined reference to static members
int UnixDaemon::sighupFd[2];
int UnixDaemon::sigtermFd[2];
int UnixDaemon::sigintFd[2];

UnixDaemon::UnixDaemon(QString pidFile, QObject *parent)
    : QObject(parent), m_lockFile(pidFile)
{
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sighupFd))
        qFatal("Couldn't create HUP socketpair");

    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sigtermFd))
        qFatal("Couldn't create TERM socketpair");

    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sigintFd))
        qFatal("Couldn't create INT socketpair");

    snInt = new QSocketNotifier(sigintFd[1], QSocketNotifier::Read, this);
    connect(snInt, &QSocketNotifier::activated, this, &UnixDaemon::handleSigInt);

    snHup = new QSocketNotifier(sighupFd[1], QSocketNotifier::Read, this);
    connect(snHup, &QSocketNotifier::activated, this, &UnixDaemon::handleSigHup);

    snTerm = new QSocketNotifier(sigtermFd[1], QSocketNotifier::Read, this);
    connect(snTerm, &QSocketNotifier::activated, this, &UnixDaemon::handleSigTerm);
}

bool UnixDaemon::TryLock(int timeout)
{
    return m_lockFile.tryLock(timeout);
}


UnixDaemon::~UnixDaemon() {
    m_lockFile.unlock();
    delete snHup;
    delete snTerm;
    delete snInt;
}


void UnixDaemon::intSignalHandler(int)
{
    char a = 1;
    if (::write(sigintFd[0], &a, sizeof(a)) != 0) {}
}


void UnixDaemon::hupSignalHandler(int)
{
    char a = 1;
    if (::write(sighupFd[0], &a, sizeof(a))) {}
}


void UnixDaemon::termSignalHandler(int)
{
    char a = 1;
    if (::write(sigtermFd[0], &a, sizeof(a))) {}
}


void UnixDaemon::handleSigInt()
{
    snInt->setEnabled(false);
    char tmp;
    if (::read(sigintFd[1], &tmp, sizeof(tmp))) {}

    qDebug().nospace() << "emit sigINT();";

    emit sigINT();
    snInt->setEnabled(true);
}




void UnixDaemon::handleSigTerm()
{
    snTerm->setEnabled(false);
    char tmp;
    if (::read(sigtermFd[1], &tmp, sizeof(tmp))) {}

    qDebug().nospace() << "emit sigTerm();";

    emit sigTERM();
    snTerm->setEnabled(true);
}


void UnixDaemon::handleSigHup()
{
    snHup->setEnabled(false);
    char tmp;
    if (::read(sighupFd[1], &tmp, sizeof(tmp))) {}

    emit sigHUP();
    qDebug().nospace() << "emit sigHUP();";

    snHup->setEnabled(true);
}

