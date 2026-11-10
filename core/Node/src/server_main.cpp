#include <QCoreApplication>
#include <QStandardPaths>
#include <QSettings>
#include <QDateTime>
#include <QLocale>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QThread>
#include <QCommandLineParser>
#include <QCommandLineOption>
#include <QDesktopServices>
#include "version.h"
#include "httpserverbackend.h"
#include "unixdaemon.h"
#include "filelogger.h"

#include <unistd.h>
#include <iostream>
#include <fstream>
#include <execinfo.h>
#include <signal.h>
#include <stdlib.h>
#include <dlfcn.h>     // for dladdr
#include <cxxabi.h>    // for __cxa_demangle
#include <sys/syscall.h>

#ifndef SYS_gettid
#error "SYS_gettid unavailable on this system"
#endif
#define gettid() ((pid_t)syscall(SYS_gettid))



#ifdef USE_EPOLL
#include "qeventdispatcher_epoll.h"
#endif
///////////////////////////////////////////////////////////////////////////////

 int get_rss()
{
    unsigned long vsize;
    long rss;
    {
        std::string ignore;
        std::ifstream ifs("/proc/self/stat", std::ios_base::in);
        ifs >> ignore >> ignore >> ignore >> ignore >> ignore >> ignore >> ignore >> ignore >> ignore >> ignore
            >> ignore >> ignore >> ignore >> ignore >> ignore >> ignore >> ignore >> ignore >> ignore >> ignore
            >> ignore >> ignore >> vsize >> rss;
    }
    long page_size_kb = sysconf(_SC_PAGE_SIZE) / 1024; // in case x86-64 is configured to use 2MB pages
    return rss * page_size_kb;
}


__asm__(".symver realpath,realpath@GLIBC_2.33");

/* This structure mirrors the one found in /usr/include/asm/ucontext.h */
typedef struct _sig_ucontext {
	unsigned long     uc_flags;
	struct ucontext   *uc_link;
	stack_t           uc_stack;
	struct sigcontext uc_mcontext;
	sigset_t          uc_sigmask;
} sig_ucontext_t;

void dump_backtrace(int sig_num, siginfo_t * info, void * ucontext)
{
  Q_UNUSED(info)
  Q_UNUSED(ucontext)
  static int dumped = 0;

  if (dumped) _exit(128 + sig_num);
  dumped = 1;

  void *callstack[128];
  const int nMaxFrames = sizeof(callstack) / sizeof(callstack[0]);
  int nFrames = backtrace(callstack, nMaxFrames);

  const char *sigName = strsignal(sig_num);
  dprintf(STDERR_FILENO, "\nOoops: signal %d (%s)\n", sig_num, sigName ? sigName : "unknown");
  backtrace_symbols_fd(callstack, nFrames, STDERR_FILENO);
  dprintf(STDERR_FILENO, "Happy debugging!\n\n");

  abort();
}

int main(int argc, char ** argv)
{
	struct sigaction sigact;
	sigact.sa_sigaction = dump_backtrace;
	sigact.sa_flags = SA_RESTART | SA_SIGINFO;

    if (sigaction(SIGSEGV, &sigact, (struct sigaction *)NULL) != 0)
        qCritical() << "SIGSEGV handler was not set correctly";
    //SIGABRT is raised by glibc on detected heap corruption (malloc_printerr).
    //Without a handler the process dies without a backtrace.
    if (sigaction(SIGABRT, &sigact, (struct sigaction *)NULL) != 0)
        qCritical() << "SIGABRT handler was not set correctly";

#ifdef USE_EPOLL
    qInfo() << "Using epoll";
    QCoreApplication::setEventDispatcher(new QEventDispatcherEpoll);    
//#else
//    qInfo() << "Not using epoll";
#endif    

    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("On My Disk Daemon");
    QCoreApplication::setOrganizationName("B1NEON d.o.o.");
    QCoreApplication::setOrganizationDomain("bineon.team");
    QCoreApplication::setApplicationVersion(APP_VERSION);

    QCommandLineParser parser;
    parser.setApplicationDescription("OnMyDisk master node.");
    parser.addHelpOption();
    parser.addVersionOption();

    QString configFile = QString("/etc/onmydisk/server.conf");
    // no defalut share path
    QString share_paths;

    QCommandLineOption configOption(QStringList() << "c" << "config", QCoreApplication::translate("main", "Settings file."), QString(configFile));
    parser.addOption(configOption);

    QCommandLineOption detach(QStringList() << "d" << "detach", QCoreApplication::translate("main", "Run as daemon (detached)"));
    parser.addOption(detach);
    parser.process(app);



    if (parser.isSet(configOption))
    {
        configFile = parser.value(configOption);
    }

    if (! QFileInfo::exists(configFile))
    {
        qCritical() << "Config not found:" << configFile;
        exit(1);
    }

    QSettings settings(configFile, QSettings::IniFormat);

#ifndef QT_DEBUG
    QString logFile = settings.value("logging/logFile", "../logs/omd-server.log").toString();
    stefanfrings::FileLogger* logger = new stefanfrings::FileLogger(configFile, logFile, 10000);
    logger->installMsgHandler();
    QObject::connect(&app, &QCoreApplication::aboutToQuit, logger, &QObject::deleteLater);
#endif

    qInfo() << "";
    qInfo() << "On My Disk Server" << APP_VERSION;
    qInfo() << "Using config:" << configFile;
    qInfo() << "Qt compiled with: " << QSslSocket::sslLibraryBuildVersionString();
    qInfo() << "Runtime: " << QSslSocket::sslLibraryVersionString();

    //check share path in stored settings in a bundle
    share_paths = settings.value("Shares", "").toString();


    UnixDaemon* daemon = Q_NULLPTR;
    if (parser.isSet(detach))
    {
             #ifdef OSX
             // create startup launchd entry
             QSettings plist("/System/Library/LaunchDaemons/com.onmydisk.daemon.plist", QSettings::NativeFormat);
             plist.setValue("Label", "com.onmydisk.daemon");
             plist.setValue("ProgramArguments", QStringList() << QCoreApplication::applicationFilePath());
             plist.setValue("KeepAlive", true);
             plist.setValue("RunAtLoad", true);
             plist.setValue("StandardOutPath", "/var/log/onmydiskd.log");
             plist.setValue("StandardErrorPath", "/var/log/onmydiskd.log");
             plist.sync();
             if (plist.status() == QSettings::NoError)
                 qInfo() << "Launch entry created.\nRun 'sudo launchctl load /System/Library/LaunchDaemons/com.onmydisk.daemon.plist' or restart the machine.\n";
             else
                 qCritical() << "Error creating launch entry. Superuser required.\n";
             exit(0);
             #endif
             QString pidFile = settings.value("PID", "/tmp/onmydiskd/service.pid").toString();
             daemon = new UnixDaemon(pidFile);

             if (daemon->TryLock(1))
             {
                   qCritical() << "Unable to start server, pid file exist: " << pidFile;
                   delete daemon;
                   exit(1);
             }
             QObject::connect(daemon, &UnixDaemon::sigTERM, &app, &QCoreApplication::quit);
             QObject::connect(&app, &QCoreApplication::aboutToQuit, daemon, &QObject::deleteLater);
        }

       qInfo() << "Shared folders on server:" << share_paths;

       HttpServerBackend server(&settings, &app);

       if ( !server.isListening() ) {
            QTimer::singleShot(1000, &app,  [&app]() {
                 qCritical() << "Exiting due to misconfiguration...";
                 app.quit();
            });
       }

       app.exec();
}
