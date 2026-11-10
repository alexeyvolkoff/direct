#include "smtpclient.h"

/**
 * @brief Set the values, create the QSslSocket object and make the connections.
 */
SMTPClient::SMTPClient(QString ehlo, QString host, quint16 port, bool tls)
{
    // Set host and port
    m_host = host;
    m_port = port;
    m_ehlo = ehlo;
    m_tls = tls;
    m_state = Close;
}

SMTPClient::~SMTPClient()
{
}


void SMTPClient::doSendEmail()
{
    QString newContent = "";
    QString domain = m_ehlo.isEmpty() ? QStringLiteral("volkoff.info") : m_ehlo;
    QString messageId = QString("<%1.%2@%3>")
        .arg(QDateTime::currentMSecsSinceEpoch())
        .arg(qrand() % 1000000)
        .arg(domain);
    QString dateStr = QDateTime::currentDateTimeUtc().toString(Qt::RFC2822Date);

    // Create headers compliant with RFC 5322
    newContent.append("Date: " + dateStr + "\n");
    newContent.append("Message-ID: " + messageId + "\n");
    newContent.append("MIME-Version: 1.0\n");
    newContent.append("To: "   + m_email.to().address()   + "\n"); // To
    newContent.append("From: " + m_email.from().name() + " " + m_email.from().address() + "\n"); // From
    newContent.append("Reply-To: " + m_email.from().address() + "\n");
    newContent.append("Content-Type: text/html; charset=UTF-8\n");
    newContent.append("Subject: " + m_email.subject() + "\n\n");   // Subject
    newContent.append("<html>\n");
    newContent.append("<body>\n");
    newContent.append(m_email.content() + "\n");                          // Content/Body
    newContent.append("</body>\n");
    newContent.append("</html>\n");

    newContent.replace(QString::fromLatin1("\n"), QString::fromLatin1("\r\n"));
    newContent.replace(QString::fromLatin1("\r\n.\r\n"), QString::fromLatin1("\r\n..\r\n"));

    m_email.setContent(newContent);

    m_state = Init;

    // Connect to host
    QSslSocket* socket = new QSslSocket (nullptr);

    connect(socket, &QSslSocket::readyRead, this, &SMTPClient::readyRead);
    connect(socket, &QSslSocket::encrypted, this, &SMTPClient::encrypted);
    connect(socket, &QSslSocket::disconnected, socket, &QObject::deleteLater);
    connect(socket, &QSslSocket::errorOccurred, this, &SMTPClient::onConnectionError);

    connect(socket,  static_cast<void (QSslSocket::*)(const QList<QSslError>&)>(&QSslSocket::sslErrors), socket, [this, socket] (const QList<QSslError> &errors)  {
        //Q_FOREACH(QSslError err, errors)
        //    qCritical() << "sslErrorSignal" << err;
        Q_UNUSED(errors)
        socket->ignoreSslErrors();
    });

    socket->connectToHost(m_host, m_port);
}

void SMTPClient::doSend()
{
    if (m_emailsList.isEmpty()) return; //late notification

    m_email = m_emailsList.takeFirst();
    m_recepients =  m_email.to().address().split(",", Qt::SplitBehaviorFlags::SkipEmptyParts);

    if (m_recepients.isEmpty()) return; //emty recepients list

    m_state = Init;

    switch (m_email.type()) {
        case Email::Notification:
            doSendEmail(); break;
        case Email::Invite:
            doSendInvite(); break;
    }
}

/**
 * @brief Send the email using a SMTP protocol.
 * @param email - Contain the data used to send the email (e.g. credentials, subject, body).
 */
bool SMTPClient::sendEmail(Email email)
{
    email.setType(Email::Notification);
    m_emailsList.append(email);
    if (m_state == Close)
     doSend();
    return true;
}

void SMTPClient::doSendInvite()
{
    QString newContent = "";
    QString boundary = "----Meeting Booking----" + QString::number(QDateTime::currentDateTime().toMSecsSinceEpoch());

    QString domain = m_ehlo.isEmpty() ? QStringLiteral("volkoff.info") : m_ehlo;
    QString messageId = QString("<%1.%2@%3>")
        .arg(QDateTime::currentMSecsSinceEpoch())
        .arg(qrand() % 1000000)
        .arg(domain);
    QString dateStr = QDateTime::currentDateTimeUtc().toString(Qt::RFC2822Date);

    // Create a header used by the email server
    newContent.append("Date: " + dateStr + "\n");
    newContent.append("Message-ID: " + messageId + "\n");
    newContent.append("To: "   + m_email.to().address()   + "\n"); // To
    newContent.append("From: " + m_email.from().name() + " " + m_email.from().address() + "\n"); // From
    newContent.append("Reply-To: " + m_email.from().address() + "\n"); // From
    newContent.append("MIME-Version: 1.0\n");
    newContent.append("Content-Type: multipart/alternative; boundary=\"" + boundary + "\"\n");
    newContent.append("Subject: " + m_email.subject() + "\n\n");   // Subject
    //Create Email Body
    newContent.append("--" + boundary + "\r\n");
    newContent.append("Content-Type: text/html; charset=UTF-8\n");
    newContent.append("Content-Transfer-Encoding: 8bit\n\n");
    newContent.append("<html>\n");
    newContent.append("<body>\n");
    newContent.append(m_email.content() + "\n");
    newContent.append("</body>\n");
    newContent.append("</html>\n");
    //Add event
    newContent.append("\n--" + boundary + "\r\n");
    newContent.append("Content-Type: text/calendar;name=\"invite.ics\"; method=REQUEST; charset=UTF-8\n");
    newContent.append("Content-Transfer-Encoding: base64\n\n");
    newContent.append(m_email.event().toUtf8().toBase64() + "\n");
    newContent.append("\n--" + boundary + "--\r\n");

    //fix formatting
    newContent.replace(QString::fromLatin1("\n"), QString::fromLatin1("\r\n"));
    newContent.replace(QString::fromLatin1("\r\n.\r\n"), QString::fromLatin1("\r\n..\r\n"));

    m_email.setContent(newContent);

    m_state = Init;

    // Connect to SMTP host
    QSslSocket* socket = new QSslSocket (nullptr);
    connect(socket, &QSslSocket::errorOccurred, this, &SMTPClient::onConnectionError);
    connect(socket, &QSslSocket::readyRead, this, &SMTPClient::readyRead);
    connect(socket, &QSslSocket::encrypted, this, &SMTPClient::encrypted);
    connect(socket, &QSslSocket::disconnected, socket, &QObject::deleteLater);
    socket->connectToHost(m_host, m_port);
}


void SMTPClient::onConnectionError(QAbstractSocket::SocketError socketError)
{
    if (socketError != QAbstractSocket::RemoteHostClosedError) {
        qCritical() << socketError;
        QSslSocket* socket = static_cast<QSslSocket*>(sender());
        emit status(Status::Failed, socket->errorString());
        socket->close();
    }
    m_emailsList.empty();
    m_state = Close;
}

bool SMTPClient::sendInvite(Email email)
{
    email.setType(Email::Invite);
    m_emailsList.append(email);
    if (m_state == Close)
     doSend();
    return true;
}



/**
 * @brief Get the responses from the server.
 *
 * SMTP server response codes:
 *
 * 250 – This SMTP server response simply means everything went
 * well and your message was delivered to the recipient server.
 *
 * 421 – Your message was temporarily deferred by the recipient
 * server. This is usually a result of too many connections in
 * a short timeframe or too many messages.
 *
 * 450 – Your message was not delivered because the other user
 * mailbox was not available. This can happen if the mailbox is
 * locked or is not routable.
 *
 * 451 – This response is sent when the message simply failed.
 * Often times this is not caused by you, but rather because of
 * a far-end server problem.
 *
 * 452 – This kind of response is sent back when there isn’t
 * enough system storage to send the message. Your message is
 * deferred until storage opens up and it can then be delivered.
 *
 * 550 – The message has failed because the other user’s mailbox
 * is unavailable or because the recipient server rejected your message.
 *
 * 551 – The mailbox your message was intended for does not exist on
 * the recipient server.
 *
 * 552 – The mailbox your message was sent to does not have enough
 * storage to accept your message.
 *
 * 553 – You message was not delivered because the name of the mailbox
 * you sent to does not exist.
 *
 * 554 – This is a very vague message failure response that can refer to
 * any number of problems either on your end or with the recipient server.
 *
 * References:
 * https://morf.lv/simple-tls-ssl-smtp-client-for-qt5
 * https://sendgrid.com/blog/smtp-server-response-codes-explained/
 * https://pt.wikipedia.org/wiki/Simple_Mail_Transfer_Protocol
 */
void SMTPClient::readyRead()
{
    QSslSocket* socket_ = static_cast<QSslSocket*>(sender());
    QTextStream textStream(socket_);
    QString responseCode;
    QString response;

    do
    {
        responseCode = socket_->readLine();
        response = responseCode;
    } while ( socket_->canReadLine() && responseCode[3] != ' ' );

    responseCode.truncate(3);

    if ( m_state == Init && responseCode == "220" )
    {
        textStream.flush();

        if (m_tls) {
            textStream << QString("EHLO %1").arg(m_ehlo) <<"\r\n";
            m_state = HandShake;
        } else {
            textStream << QString("HELO %1").arg(m_ehlo) <<"\r\n";
            m_state = Mail;
        }
    }
    else if (m_state == HandShake && responseCode == "250")
    {
        textStream << "STARTTLS" << "\r\n";
        textStream.flush();

        m_state = Tls;
    }
    else if (m_state == Tls && responseCode == "220")
    {
        socket_->startClientEncryption();
        return;
    }
    else if (m_state == Auth && responseCode == "250")
    {
        if (m_email.credentials().password().isEmpty())
        {
            textStream << "MAIL FROM:<" << m_email.from().address() << ">\r\n";
            textStream.flush();

            m_state = Rcpt;
        }
        else
        {
            textStream << "AUTH LOGIN" << "\r\n";
            textStream.flush();

            m_state = User;
        }
    }
    else if (m_state == User && responseCode == "334")
    {
        // Use XOAUTH2 protocol, which means that password and username has to be sent in base64 coding
        textStream << QByteArray().append(m_email.credentials().address().toUtf8()).toBase64()  << "\r\n";
        textStream.flush();

        m_state = Pass;
    }
    else if (m_state == Pass && responseCode == "334")
    {
        textStream << QByteArray().append(m_email.credentials().password().toUtf8()).toBase64() << "\r\n";
        textStream.flush();

        m_state = Mail;
    }
    else if ( m_state == Mail && (responseCode == "235" || responseCode == "250"))
    {
        //qDebug() << "mail from" << m_email.from().address();
        textStream << "MAIL FROM:<" << m_email.from().address() << ">\r\n";
        textStream.flush();

        m_state = Rcpt;
    }
    else if ( m_state == Rcpt && responseCode == "250" )
    {
        QString recepient = m_recepients.takeFirst();
        textStream << "RCPT TO:<" << recepient << ">\r\n";
        textStream.flush();

        m_state = Data;
    }
    else if ( m_state == Data && responseCode == "250" )
    {
        textStream << "DATA\r\n";
        textStream.flush();

        m_state = Body;
    }
    else if ( m_state == Body && responseCode == "354" )
    {
        textStream << m_email.content() << "\r\n.\r\n";
        textStream.flush();

        m_state = m_recepients.count() > 0  ? Mail :  Quit;
    }
    else if ( m_state == Quit && responseCode == "250" )
    {
        textStream << "QUIT\r\n";
        textStream.flush();

        m_state = Close;

        emit status(Status::Success, "Email successfully sent to " + m_email.to().address());

        //continue sending if not empty
        if (! m_emailsList.isEmpty()) {
            QMetaObject::invokeMethod(this, "doSend", Qt::QueuedConnection);
        }
    }
    else if ( responseCode == "221")
    {
        socket_->close();
        return;
    }
    else
    {
        m_state = Close;
        emit status(Status::Failed, response);
    }
}

void SMTPClient::encrypted()
{
    QSslSocket* socket_ = static_cast<QSslSocket*>(sender());
    QTextStream textStream(socket_);

    textStream << QString("EHLO %1").arg(m_ehlo) <<"\r\n";
    textStream.flush();

    m_state = Auth;
}


