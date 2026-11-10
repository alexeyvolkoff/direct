/**
  @file
  @author Stefan Frings
*/

#ifndef DUALFILELOGGER_H
#define DUALFILELOGGER_H

#include <QString>
#include <QSettings>
#include <QtGlobal>
#include "logglobal.h"
#include "logger.h"
#include "filelogger.h"

namespace stefanfrings {

/**
  Logs messages into two log files simultaneously.
  May be used to create two logfiles with different configuration settings.
  @see FileLogger for a description of the two underlying loggers.
*/

class DECLSPEC DualFileLogger : public Logger {
    Q_OBJECT
    Q_DISABLE_COPY(DualFileLogger)
public:

    /**
      Constructor.
      @param refreshInterval Interval of checking for changed config settings in msec, or 0=disabled
      @param parent Parent object.
    */
    DualFileLogger(const QString &configFile, const int refreshInterval=10000, QObject *parent = 0);

    /**
      Decorate and log the message, if type>=minLevel.
      This method is thread safe.
      @param type Message type (level)
      @param message Message text
      @param file Name of the source file where the message was generated (usually filled with the macro __FILE__)
      @param function Name of the function where the message was generated (usually filled with the macro __LINE__)
      @param line Line Number of the source file, where the message was generated (usually filles with the macro __func__ or __FUNCTION__)
      @see LogMessage for a description of the message decoration.
    */
    virtual void log(const QtMsgType type, const QString& message, const QString &file="", const QString &function="", const int line=0);

    /**
      Clear the thread-local data of the current thread.
      This method is thread safe.
      @param buffer Whether to clear the backtrace buffer
      @param variables Whether to clear the log variables
    */
    virtual void clear(const bool buffer=true, const bool variables=true);

private:

    /** First logger */
    FileLogger* firstLogger;

    /** Second logger */
    FileLogger* secondLogger;

};

} // end of namespace

#endif // DUALFILELOGGER_H
