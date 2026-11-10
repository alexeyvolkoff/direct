#include "clientsession.h"
#include <QMutexLocker>

ClientSession::ClientSession(quint64 sessionTimeout, QObject *parent): QObject(parent)
{
     m_timerTime = sessionTimeout;
     m_userName = QStringLiteral("anonymous");
}

ClientSession::~ClientSession()
{
    if (m_timer) {
        m_timer->stop();
        disconnect(m_timer, &QTimer::timeout, this, &ClientSession::timeout);
        delete m_timer;
    }
    m_sessionVars.clear();
}

void ClientSession::setVar(QString varName, QVariant varValue)
{
    QMutexLocker  lock(&m_mutex);
    m_sessionVars[varName] = varValue;
}

QVariant ClientSession::getVar(QString varName, QVariant defaultValue)
{
    QMutexLocker  lock(&m_mutex);
    if (m_sessionVars.contains(varName))
        return m_sessionVars[varName];
    else
        return defaultValue;
}

QString ClientSession::sessionID()
{
    QMutexLocker  lock(&m_mutex);
    return m_sessionID;
}

void ClientSession::setSessionID(const QString &sessionID)
{
    QMutexLocker  lock(&m_mutex);
    m_sessionID = sessionID;
}

void ClientSession::restartTimer()
{
    if (QThread::currentThread() != this->thread()) {
       // Called from different thread
		QMetaObject::invokeMethod(this, "restartTimer", Qt::QueuedConnection);
    }
    else
        _restartTimer();
}

QString ClientSession::userName()
{
    QMutexLocker locker(&m_mutex);
    return m_userName;
}

QString ClientSession::userID()
{
 QMutexLocker locker(&m_mutex);
 if (isGuest())
    return m_sessionID;
 else
    return m_userName;
}

bool ClientSession::isGuest()
{
    return (m_sessionVars.contains("isGuest") && m_sessionVars["isGuest"].toBool());
}

void ClientSession::setUserName(const QString &userName) {
    QMutexLocker  locker(&m_mutex);
    m_userName = userName;
}

QHash<QString, QVariant> ClientSession::getSessionVariables()
{
   QMutexLocker  locker(&m_mutex);
   return m_sessionVars;
}

void ClientSession::setSessionVariables(QHash<QString, QVariant> values)
{
  QMutexLocker  locker(&m_mutex);
  Q_FOREACH(QString key, values.keys()) {
    m_sessionVars[key] = values[key];
    if (key == "User")
        m_userName = values[key].toString();
  }
}

void ClientSession::_restartTimer()
{
    if (m_timer == nullptr) {
        m_timer = new QTimer(this);
        connect(m_timer, &QTimer::timeout, this, &ClientSession::timeout);
    }
    if (m_timer && m_timerTime > 0)
    {
        m_timer->start(m_timerTime);
    }
}
