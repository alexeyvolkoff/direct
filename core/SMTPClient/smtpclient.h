#ifndef SMTPCLIENT_H
#define SMTPCLIENT_H

#include <QTimer>
#include <QObject>
#include <QtNetwork/QSslSocket>
#include <QtNetwork/QAbstractSocket>
#include <QEventLoop>

#include "email.h"

namespace Status {
    enum e {
        Success,
        Failed
    };
}

/**
 * @brief The SMTPClient class is used to
 */
class SMTPClient : public QObject
{
    Q_OBJECT

public:
    // Default port 587 for TLS or 465 for SSL cryptography connection
    SMTPClient(QString ehlo, QString host,
               quint16 port = 587, bool tls = true);

    ~SMTPClient();

    bool sendEmail(Email email);
    bool sendInvite(Email email);
    void waitForReady();

signals:
    void status(Status::e status, QString errorMessage = "");

private slots:
    void readyRead();
    void encrypted();
    void doSend();
    void onConnectionError(QAbstractSocket::SocketError socketError);

private:
    enum states{
        Tls,
        HandShake,
        Auth,
        User,
        Pass,
        Rcpt,
        Mail,
        Data,
        Init,
        Body,
        Quit,
        Close
    };

    QString m_host;
    QString m_ehlo;
    quint16 m_port;
    bool m_tls;

    QList<Email> m_emailsList;
    Email m_email;
    QStringList m_recepients;

    int m_state;

    void doSendInvite();
    void doSendEmail();
};

#endif // SMTPCLIENT_H
