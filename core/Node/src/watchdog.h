#ifndef WATCHDOG_H
#define WATCHDOG_H

#include <QObject>
#include <QTimer>
#include <pthread.h>

#define WD_INTERVAL 10000
#define WD_TIMEOUT 5000

class Watchdog : public QObject
{
    Q_OBJECT
private:
    QTimer *m_wdTimer;
    QTimer *m_wdIntervalTimer;
    pthread_t m_mainThreadID;
public:
    ~Watchdog();
    explicit Watchdog(pthread_t mainThreadId, QObject *parent = nullptr);

private slots:
    void wdTimeout();
    void wdPing();

public slots:
    void pingResponse();
    void start();
signals:
    void ping();
};


void killProcess();

#endif // WATCHDOG_H
