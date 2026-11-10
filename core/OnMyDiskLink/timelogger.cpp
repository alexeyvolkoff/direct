#include "timelogger.h"

TimeLogger::TimeLogger(QString logLine)
{
    m_logLine = logLine;
    m_timer.start();
}

void TimeLogger::RestartTimer()
{
    m_timer.restart();
}

void TimeLogger::ReportTime(QString logLine)
{
    qInfo() << logLine << " executed at " << m_timer.elapsed() << " ms";
}

TimeLogger::~TimeLogger()
{
    qInfo() << m_logLine << " is finished in " << m_timer.elapsed() << " ms";
}
