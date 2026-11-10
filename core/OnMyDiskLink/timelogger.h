#ifndef TIMELOGGER_H
#define TIMELOGGER_H

#include <QObject>
#include <QTime>
#include <QDebug>
#include <QElapsedTimer>
#include "onmydisklink_global.h"

class  TimeLogger
{
public:
    explicit TimeLogger(QString logLine);
    void RestartTimer();
    void ReportTime(QString logLine);
    ~TimeLogger();

private:
    QString m_logLine;
    QElapsedTimer m_timer;

};

#endif // TIMELOGGER_H
