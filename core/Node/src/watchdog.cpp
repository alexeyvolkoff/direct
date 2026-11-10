#include "watchdog.h"
#include <QDebug>
#include <QThread>
#include <QCoreApplication>
#include <signal.h>

Watchdog::~Watchdog()
{
    delete m_wdTimer;
    delete m_wdIntervalTimer;

}

Watchdog::Watchdog(pthread_t mainThreadId, QObject *parent)
    :  QObject(parent), m_mainThreadID(mainThreadId)
{

}


void Watchdog::wdTimeout()
{
    qWarning() << "Watchdog timeout";
    // Send SIGSEGV signal to main thread
    pthread_kill(m_mainThreadID, SIGSEGV);
}

void Watchdog::wdPing()
{
    m_wdTimer->start(WD_TIMEOUT);
    emit ping();
}

void Watchdog::pingResponse()
{
    if (this->thread() != QThread::currentThread()) {
        QMetaObject::invokeMethod(this, "pingResponse", Qt::QueuedConnection);
    } else {
        m_wdTimer->stop();
        m_wdIntervalTimer->start(WD_INTERVAL);
    }
}

void Watchdog::start()
{
    m_wdIntervalTimer = new QTimer();
    m_wdTimer = new QTimer();
    connect(m_wdTimer, &QTimer::timeout, this, &Watchdog::wdTimeout);
    connect(m_wdIntervalTimer, &QTimer::timeout, this, &Watchdog::wdPing);
    m_wdIntervalTimer->start(WD_INTERVAL);
}
