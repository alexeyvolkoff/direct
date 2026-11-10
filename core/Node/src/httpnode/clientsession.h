#ifndef CLIENTSESSION_H
#define CLIENTSESSION_H

#include <QObject>
#include <QMutex>
#include <QHash>
#include <QString>
#include <QVariant>
#include <QTimer>
#include <QThread>
#include "qhttpfwd.hpp"

class ClientSession: public QObject
{
    Q_OBJECT

public:
    explicit ClientSession(quint64 sessionTimeout, QObject* parent = nullptr);
    ~ClientSession();
    QString sessionID();
    void setSessionID(const QString &sessionID);
    QString userName();
    QString userID();
    bool isGuest();
    void setUserName(const QString &userName);
    QHash<QString, QVariant> getSessionVariables();
    void setSessionVariables(QHash<QString, QVariant> values);
    void setVar(QString varName, QVariant varValue);
    QVariant getVar(QString varName, QVariant defaultValue);
public slots:
    void restartTimer();

signals:
    void timeout();

private:
    QMutex m_mutex;
    QHash<QString, QVariant> m_sessionVars;
    QString m_sessionID = "";
    QString m_userName;
    QTimer *m_timer = nullptr;
    quint64 m_timerTime;
    void _restartTimer();
};

#endif // CLIENTSESSION_H
