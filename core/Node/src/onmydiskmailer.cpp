#include "onmydiskmailer.h"
#include "templateparser.h"
#include "onmydisklink_global.h"
#include "onmydisklinkserver.h"
#include "simplecrypt.h"
#include <QThread>

bool  OnMyDiskMailer::isValidEmail(const QString& email ) {
    QRegExp regex("\\b[A-Z0-9._%+-]+@[A-Z0-9.-]+\\.[A-Z]{2,4}\\b");
    regex.setCaseSensitivity(Qt::CaseInsensitive);
    regex.setPatternSyntax(QRegExp::RegExp);
    return regex.exactMatch(email);
}


QString OnMyDiskMailer::createICalEvent(QString method, QString companyName, QString appName, QDateTime date,  QString title, QString linkUrl,  QString organizer, QString attendees) {
    QString dtstamp =  QDateTime::currentDateTime().toString("yyyyMMddThhmmssZ");
    QString iCalEvent = QString("BEGIN:VCALENDAR\n"
                    "METHOD:" + method + "\n"
                    "VERSION:2.0\n"
                    "PRODID:-//" + companyName  + "//" + appName + "//EN\n"
                    "CALSCALE:GREGORIAN\n"
                    "METHOD:REQUEST\n"
                    "BEGIN:VEVENT\n"
                    "DTSTAMP:"  + dtstamp + "\n"
                    "UID:" + QUuid::createUuid().toString() + "\n"
                    "DTSTART:" + date.toString("yyyyMMddThhmmssZ") + "\n"
                    "DTEND:" + date.addSecs(3600).toString("yyyyMMddThhmmssZ") + "\n"
                    "SUMMARY:" + title + "\n"
                    "LOCATION:" + linkUrl + "\n"
                    "CREATED:" + dtstamp + "\n"
                    "LAST-MODIFIED:" + dtstamp + "\n"
                    "CLASS:PUBLIC\n"
                    "STATUS:CONFIRMED\n"
                    "PRIORITY:5\n"
                    + organizer + attendees +
                    "BEGIN:VALARM\n"
                    "DESCRIPTION:REMINDER\n"
                    "TRIGGER;RELATED=START:-PT15M\n"
                    "ACTION:DISPLAY\n"
                    "END:VALARM\n"
                    "END:VEVENT\n"
                    "END:VCALENDAR\n");
    return iCalEvent;
}


OnMyDiskMailer::OnMyDiskMailer()
    : QObject{nullptr}
{

}

QString OnMyDiskMailer::getOwnerOfLink(QString linkID, bool &isOk)
{
    //return m_dbAccessHelper->getLoginByKey(linkID, isOk);
    isOk = false;
    QString owner = "";
    QPointer<OnMyDiskLink> link = OnMyDiskLinkServer::instance().getLink(linkID);
    if (link && !link.isNull())
    {
        owner = link->user();
        isOk = true;
    }
    return owner;
}




OnMyDiskMailer &OnMyDiskMailer::instance()
{
  static OnMyDiskMailer mailer;
  return mailer;
}

void OnMyDiskMailer::init(QSettings *settings, bool keyLogin)
{
    m_settings = settings;
    m_keyLogin = keyLogin;
    m_smtpClient = new SMTPClient(m_settings->value("smtpehlo", "smtpclient").toString(), m_settings->value("smtphost", "mail.bineon.team").toString(), m_settings->value("smtpport", 587).toUInt(), m_settings->value("smtptls", true).toBool());
    connect(m_smtpClient, &SMTPClient::status, this, &OnMyDiskMailer::onSmtpStatus, Qt::UniqueConnection);
}


OnMyDiskMailer::~OnMyDiskMailer()
{
  delete m_smtpClient;
}

static QString personalSignature()
{
    return QString(
        "<div style=\"margin-top: 28px; padding-top: 16px; border-top: 1px solid #e2e8f0; color: #475569; font-size: 14px; line-height: 1.6;\">"
        "<p>Please feel free to reply directly to this email if you have any questions, ideas, or need assistance.</p>"
        "<p style=\"margin-top: 14px;\">Best regards,<br/>"
        "<b>Alexey Volkov</b><br/>"
        "<span style=\"color: #2563eb; font-weight: 500;\">OnMyDisk</span></p>"
        "</div>"
    );
}

Email OnMyDiskMailer::createEmail(QString subject, QString body, QString appName, QString to)
{
    Q_UNUSED(appName)
    // Create the credentials EmailAddress
    QString login = m_settings->value("smtplogin", "noreply@onmydisk.net").toString();
    QString pass = m_settings->value("smtppass", "").toString();
    QString senderName = m_settings->value("smtpsendername", "Alexey Volkov (OnMyDisk)").toString();

    EmailAddress credentials(login, "", pass);

    // Create the from EmailAddress with personal name
    EmailAddress fromAddress("<" + login + ">", senderName);

    // Create the to EmailAddress
    EmailAddress toAddress(to);

    // Create the email
    Email email(credentials,
                fromAddress,
                toAddress,
                subject,
                body);

    return email;

}

void OnMyDiskMailer::onSmtpStatus(Status::e status, QString message)
{
    switch (status) {
        case Status::Success:
            //qInfo() << "SMTP:" << qPrintable(message);
            break;
        case Status::Failed:
            qCritical() << "SMTP error:" << qPrintable(message);
            break;
        }
}

void OnMyDiskMailer::sendActivationLink(const QString& activationCode, const QString& email)
{
    if (QThread::currentThread() != this->thread()) {
        // Called from another thread
        QMetaObject::invokeMethod(this, "sendActivationLink", Qt::QueuedConnection, Q_ARG(QString, activationCode), Q_ARG(QString, email));
    } else {
        QString serverName =  TemplateParser::instance().staticVar("APPTITLE").toString();
        if (serverName.isEmpty()) serverName = "OnMyDisk";

        QString subject = QString("%1 Confirmation Code").arg(serverName);
        QString body = QString(
            "<div style=\"font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Helvetica, Arial, sans-serif; max-width: 560px; margin: 0 auto; padding: 24px; color: #1e293b; line-height: 1.6;\">"
            "<h2 style=\"color: #0f172a; margin-top: 0;\">Welcome to %1!</h2>"
            "<p>Hi there,</p>"
            "<p>Please use the following one-time security code to activate your account or confirm your operation:</p>"
            "<div style=\"margin: 20px 0; padding: 14px 24px; font-size: 28px; font-weight: bold; letter-spacing: 6px; color: #2563eb; background: #eff6ff; border-radius: 8px; display: inline-block;\">%2</div>"
            "<p style=\"font-size: 13px; color: #64748b;\">If you did not request this code, you can safely ignore this email.</p>"
            "%3"
            "</div>"
        ).arg(serverName, activationCode, personalSignature());

        Email emailToSend = createEmail(subject, body, serverName, email);

        // Try to send the email
        qInfo() << "Sending activation code to" << email;
        m_smtpClient->sendEmail(emailToSend);
    }
}

void OnMyDiskMailer::sendCallNotification(const CallInfo &callInfo)
{
    if (QThread::currentThread() != this->thread()) {
        // Called from another thread
        QMetaObject::invokeMethod(this, "sendCallNotification", Qt::QueuedConnection, Q_ARG(CallInfo, callInfo));
    } else {
        QString server =  TemplateParser::instance().staticVar("SERVER").toString();
        QString serverName =  TemplateParser::instance().staticVar("MEET_APPTITLE").toString();
        QString companyName =  TemplateParser::instance().staticVar("COMPANY").toString();
        QString serverEMail = m_settings->value("smtplogin", "noreply@onmydisk.net").toString();

        QString ending = "";
        QDateTime meetDate;
        QString title = !callInfo.callTopic.isEmpty() ?  callInfo.callTopic : serverName + " meeting";

        if (!callInfo.meet.isNull()) {
            meetDate = callInfo.meet.toLocalTime();
            meetDate.setTimeZone(QTimeZone("CET"));
            QString meetDateStr = meetDate.toString("ddd, d MMM yyyy hh:mm (t)");
            ending += " taking place on <b>" + meetDateStr + "</b>";
        }
        ending += ".";

        QString meetingName =  !callInfo.callTopic.isEmpty() ?  + " <b>" + callInfo.callTopic + "</b>" : "";

        QString linkUrl = "https://" + server + CALL_LINK_PREFIX + callInfo.callId;
        QString emailMessage = QString(
            "<div style=\"font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Helvetica, Arial, sans-serif; max-width: 560px; margin: 0 auto; padding: 24px; color: #1e293b; line-height: 1.6;\">"
            "<h2 style=\"color: #0f172a; margin-top: 0;\">Online Meeting Invitation</h2>"
            "<p>Hi there,</p>"
            "<p>You have been invited to%1 online meeting%2</p>"
            "<p>Please follow this link to join the meeting:<br/>"
            "<a href=\"%3\" style=\"color: #2563eb; font-weight: 500;\">%3</a></p>"
            "%4"
            "</div>").arg(meetingName, ending, linkUrl, personalSignature());

        QHash<QString, QString>  emails;
        QStringList attendees = callInfo.users.split(" ", Qt::SplitBehaviorFlags::SkipEmptyParts).join(",").split(",", Qt::SplitBehaviorFlags::SkipEmptyParts);

        Q_FOREACH(QString attendee, attendees) {
            if ( isValidEmail( attendee)) {
                QString user = attendee.left(attendee.indexOf("@"));
                emails[attendee] = user;
            } else {
                //find email in user profile
                bool found = false;
                QJsonObject profile;
                Q_UNUSED(profile)
                if (found) {
                      QString displayname = profile["displayname"].toString();
                        emails[profile["email"].toString()] = !displayname.isEmpty() ?  displayname : attendee;
                }
            }
        }

        //organizer
        QString organizerName = !callInfo.organizerName.isEmpty() ? callInfo.organizerName : callInfo.organizer;
        if ( isValidEmail( organizerName))
            organizerName = organizerName.left(organizerName.indexOf("@"));

        if (callInfo.setMeet && callInfo.meet.isValid() )
        {
            QString attendees = "";
            Q_FOREACH(QString email, emails.keys()) {
                if (m_keyLogin)
                  attendees += "ATTENDEE;ROLE=REQ-PARTICIPANT;CN=\"" + emails[email] +"\":mailto:" + email + "\n";
                else
                  attendees += "ATTENDEE;ROLE=REQ-PARTICIPANT;PARTSTAT=NEEDS-ACTION;RSVP=TRUE;CN=\"" + emails[email] +"\":mailto:" + email + "\n";
            }
            QString organizer =  m_keyLogin ? "ORGANIZER;CN=" + serverName + " user" : "ORGANIZER;CN=" + organizerName + ":mailto:" + callInfo.organizer + "\n";
            QString recipients = emails.keys().join(", ");
            QString iCalEvent = createICalEvent("REQUEST", companyName, serverName, callInfo.meet, title, linkUrl, organizer, attendees);

             Email emailToSend = createEmail(title, emailMessage, serverName, recipients);
             emailToSend.setEvent(iCalEvent);
             //send meeting request
             m_smtpClient->sendInvite(emailToSend);

             //send event to organizer
             if (!organizer.isEmpty())
             {
                attendees = "";
                organizer = "ORGANIZER;CN=" + serverName + ":mailto:" + serverEMail + "\n";
                Q_FOREACH(QString email, emails.keys()) {
                  attendees += "ATTENDEE;ROLE=REQ-PARTICIPANT;CN=\"" + emails[email] +"\":mailto:" + email + "\n";
                }
                iCalEvent = createICalEvent("REQUEST", companyName, serverName, callInfo.meet, title, linkUrl, organizer, attendees);
                QString emailMessage = QString(
                    "<div style=\"font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Helvetica, Arial, sans-serif; max-width: 560px; margin: 0 auto; padding: 24px; color: #1e293b; line-height: 1.6;\">"
                    "<h2 style=\"color: #0f172a; margin-top: 0;\">Meeting Reminder</h2>"
                    "<p>Hi there,</p>"
                    "<p>This is a reminder that you organized an online meeting%1%2</p>"
                    "<p>Please follow this link to join:<br/>"
                    "<a href=\"%3\" style=\"color: #2563eb; font-weight: 500;\">%3</a></p>"
                    "%4"
                    "</div>").arg(meetingName, ending, linkUrl, personalSignature());
                Email emailToSend = createEmail(title, emailMessage, serverName,  callInfo.organizer);
                emailToSend.setEvent(iCalEvent);
                //send calendar event
                m_smtpClient->sendInvite(emailToSend);
             }
        } else {

            Email emailToSend = createEmail(title, emailMessage, serverName, emails.keys().join(", "));
            // Try to send the email
            m_smtpClient->sendEmail(emailToSend);
       }
    }
}

void OnMyDiskMailer::sendLinkNotification(const QString &sender, const QString &senderEmail, const QString &link,  const StringsHash &emails, const VariantHash &linkData)
{
    if (QThread::currentThread() != this->thread()) {
        // Called from another thread
        QMetaObject::invokeMethod(this, "sendLinkNotification", Qt::QueuedConnection, Q_ARG(QString, sender), Q_ARG(QString, senderEmail), Q_ARG(QString, link), Q_ARG(StringsHash, emails), Q_ARG(VariantHash, linkData));
    } else {
        QString server =  TemplateParser::instance().staticVar("SERVER").toString();
        QString serverName =  TemplateParser::instance().staticVar("APPTITLE").toString();
        QString companyName =  TemplateParser::instance().staticVar("COMPANY").toString();
        QString serverEMail = m_settings->value("smtplogin", "noreply@onmydisk.net").toString();

        QString linkUrl = "https://" + server + link;
        //check if meeting is set
        QString title = linkData["title"].toString();
        if (linkData["setMeet"].toBool())
        {
            //check the date
            QString ending = "";
            QString iCalEvent = "";
            QString organizer =  m_keyLogin ? "ORGANIZER;CN=" + serverName + " user" : "ORGANIZER;CN=" + sender + ":mailto:" + senderEmail + "\n";
            QString attendees = "";
            bool setDate = !linkData["meet"].isNull();
            if (setDate)
            {
                QDateTime meetDate = linkData["meet"].toDateTime();
                meetDate.setTimeZone(QTimeZone("CET"));
                QString meetDateStr = meetDate.toLocalTime().toString("ddd, d MMM yyyy hh:mm (t)");

                Q_FOREACH(QString email, emails.keys())
                {
                   if (m_keyLogin)
                    attendees += "ATTENDEE;ROLE=REQ-PARTICIPANT;CN=\"" + emails[email] +"\":mailto:" + email + "\n";
                   else
                    attendees += "ATTENDEE;ROLE=REQ-PARTICIPANT;PARTSTAT=NEEDS-ACTION;RSVP=TRUE;CN=\"" + emails[email] +"\":mailto:" + email + "\n";
                }

                iCalEvent = createICalEvent("REQUEST", companyName, serverName, linkData["meet"].toDateTime(), title, linkUrl, organizer, attendees);
                ending = " taking place on <b>" + meetDateStr + "</b>";
            };
            ending += ".";

            QString emailMessage = QString(
                "<div style=\"font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Helvetica, Arial, sans-serif; max-width: 560px; margin: 0 auto; padding: 24px; color: #1e293b; line-height: 1.6;\">"
                "<h2 style=\"color: #0f172a; margin-top: 0;\">Discussion Invitation</h2>"
                "<p>Hi there,</p>"
                "<p>You have been invited to discuss <b>%1</b> at an online meeting%2</p>"
                "<p>Please follow this link to join the meeting:<br/>"
                "<a href=\"%3\" style=\"color: #2563eb; font-weight: 500;\">%3</a></p>"
                "%4"
                "</div>").arg(title, ending, linkUrl, personalSignature());


            QString recipients = emails.keys().join(", ");
            Email emailToSend = createEmail(title, emailMessage, serverName, recipients);
            qInfo() << "Sending link to" << recipients;
            if (setDate)
            {
                emailToSend.setEvent(iCalEvent);
                //send meeting request
                m_smtpClient->sendInvite(emailToSend);

                //Publish event in organizer's calendar
                if (!m_keyLogin)
                {
                    attendees = "";
                    organizer = "ORGANIZER;CN=" + serverName + ":mailto:" + serverEMail + "\n";
                    Q_FOREACH(QString email, emails.keys())
                    attendees += "ATTENDEE;ROLE=REQ-PARTICIPANT;CN=\"" + emails[email] +"\":mailto:" + email + "\n";

                    QString emailMessage = QString(
                        "<div style=\"font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Helvetica, Arial, sans-serif; max-width: 560px; margin: 0 auto; padding: 24px; color: #1e293b; line-height: 1.6;\">"
                        "<h2 style=\"color: #0f172a; margin-top: 0;\">Meeting Reminder</h2>"
                        "<p>Hi there,</p>"
                        "<p>This is a reminder that you scheduled an online discussion regarding <b>%1</b>%2</p>"
                        "<p>Please follow this link to join:<br/>"
                        "<a href=\"%3\" style=\"color: #2563eb; font-weight: 500;\">%3</a></p>"
                        "%4"
                        "</div>").arg(title, ending, linkUrl, personalSignature());
                    iCalEvent = createICalEvent("REQUEST", companyName, serverName, linkData["meet"].toDateTime(), title, linkUrl, organizer, attendees);
                    Email emailToSend = createEmail(title, emailMessage, serverName, senderEmail);
                    emailToSend.setEvent(iCalEvent);
                    //send calendar event
                    m_smtpClient->sendInvite(emailToSend);
                }
            } else
                 m_smtpClient->sendEmail(emailToSend);
        } else {

            QString recipients = emails.keys().join(", ");
            QString byUser = m_keyLogin ? serverName + " user" :  sender;
            QString emailMessage = QString(
                "<div style=\"font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Helvetica, Arial, sans-serif; max-width: 560px; margin: 0 auto; padding: 24px; color: #1e293b; line-height: 1.6;\">"
                "<h2 style=\"color: #0f172a; margin-top: 0;\">Shared Content</h2>"
                "<p>Hi there,</p>"
                "<p><b>%1</b> has shared <b>%2</b> with you.</p>"
                "<p>Please follow this link to view or download the files:<br/>"
                "<a href=\"%3\" style=\"color: #2563eb; font-weight: 500;\">%3</a></p>"
                "%4"
                "</div>").arg(byUser, title, linkUrl, personalSignature());
            Email emailToSend = createEmail(title, emailMessage, serverName, recipients);
            // send the email
            qInfo() << "Sending link to" << recipients;
            m_smtpClient->sendEmail(emailToSend);
        }
    }
}

void OnMyDiskMailer::sendGroupInvite(const QString &sender, const QString &group, const QString &groupId, const QString &userEmail)
{
    if (QThread::currentThread() != this->thread()) {
        // Called from another thread
        QMetaObject::invokeMethod(this, "sendGroupInvite", Qt::QueuedConnection, Q_ARG(QString, sender), Q_ARG(QString, group),  Q_ARG(QString, groupId), Q_ARG(QString, userEmail));
    } else {
        QString serverName =  TemplateParser::instance().staticVar("APPTITLE").toString();
        QString joinUrl = TemplateParser::instance().staticVar("SERVERURL").toString() + "/signup/" + groupId;
        QString title = QString("%1 - Group Invitation").arg(serverName);

        if (!userEmail.isEmpty())
        {
            // Create the email object
            QString emailMessage = QString(
                "<div style=\"font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Helvetica, Arial, sans-serif; max-width: 560px; margin: 0 auto; padding: 24px; color: #1e293b; line-height: 1.6;\">"
                "<h2 style=\"color: #0f172a; margin-top: 0;\">Group Invitation</h2>"
                "<p>Hi there,</p>"
                "<p><b>%2</b> invites you to join the group <b>%1</b> on %3 private collaborative storage.</p>"
                "<p>Please follow this link to register and join:<br/>"
                "<a href=\"%4\" style=\"color: #2563eb; font-weight: 500;\">%4</a></p>"
                "%5"
                "</div>").arg(group, sender, serverName, joinUrl, personalSignature());
            Email emailToSend = createEmail(title, emailMessage, serverName, userEmail);
            // Try to send the email
            qInfo() << "Sending group invite to" << userEmail;
            m_smtpClient->sendEmail(emailToSend);
        }
    }
}






void OnMyDiskMailer::publicLinkAdded(QString linkID, const VariantHash &linkResponseData)
{

#ifndef QT_DEBUG
  bool ok = false;
  QString userId = getOwnerOfLink(linkID, ok);
  if (! ok) {
      return;
  }
  QString userName = userId;
  QString userEMail = m_settings->value("smtplogin", "noreply@onmydisk.net").toString();
  Q_UNUSED(userId)
#else
    Q_UNUSED(linkID)
    QString userName = TemplateParser::instance().staticVar("APPTITLE").toString();
    QString userEMail = m_settings->value("smtplogin", "noreply@onmydisk.net").toString();
#endif

  QHash<QString, QVariant> linkData = linkResponseData["linkData"].toHash();
  QString link = linkResponseData["link"].toString();
  bool notify = linkData["notify"].toBool();
  if (notify)
  {
      QHash<QString, QString>  emails;
      QStringList attendees = linkData["emails"].toString().split(" ", Qt::SplitBehaviorFlags::SkipEmptyParts).join(",").split(",", Qt::SplitBehaviorFlags::SkipEmptyParts);


      Q_FOREACH(QString attendee, attendees) {
          if ( isValidEmail( attendee)) {
                    QString user = attendee.left(attendee.indexOf("@"));
                    emails[attendee] = user;
          } else {
              //find email in user profile
              bool found = false;
              QJsonObject profile;
              Q_UNUSED(profile)
              if (found) {
                    QString displayname = profile["displayname"].toString();
                    emails[profile["email"].toString()] = !displayname.isEmpty() ?  displayname : attendee;
              }
          }
      }

     #ifndef QT_DEBUG
      //add sender to recepients
      if (!m_keyLogin && !emails.keys().contains(userEMail)) {
          emails[userEMail] = userName;
      }
     #endif
     //send notifications
     sendLinkNotification(userName, userEMail, link, emails, linkData);
  }
}

void OnMyDiskMailer::sendCustomForm(const QString &user, StringsHash formData)
{

    if (QThread::currentThread() != this->thread()) {
        // Called from another thread
        QMetaObject::invokeMethod(this, "sendCustomForm", Qt::QueuedConnection, Q_ARG(QString, user),  Q_ARG(StringsHash,  formData));
    } else {
        bool found = false;
        QJsonObject profile;
        Q_UNUSED(profile)
        QString email;
        if (found) {
            email = profile["email"].toString();
        } else {
            qCritical() << "User not found" << user;
            return;
        }

        QString serverName =  TemplateParser::instance().staticVar("APPTITLE").toString();
        QString title = QString("%1 - Form Submitted").arg(serverName);

        // Create the email object
        QString emailMessage = QString(
            "<div style=\"font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Helvetica, Arial, sans-serif; max-width: 560px; margin: 0 auto; padding: 24px; color: #1e293b; line-height: 1.6;\">"
            "<h2 style=\"color: #0f172a; margin-top: 0;\">Website Form Submitted</h2>"
            "<p>Hi there,<br/>The following form has been submitted from your site:</p>");
        foreach(QString key, formData.keys())
              emailMessage += QString("<p><b>%1</b>: %2</p>").arg(key, formData[key]);

        emailMessage += personalSignature() + "</div>";

        Email emailToSend = createEmail(title, emailMessage, serverName, email);
        // Try to send the email
        qInfo() << "Sending submitted form to" << email;
        m_smtpClient->sendEmail(emailToSend);
    }

}


void OnMyDiskMailer::sendInvitation(const QString &email, const QString &code, const QString &owner, const QString &group)
{
    if (QThread::currentThread() != this->thread()) {
        // Called from another thread
        QMetaObject::invokeMethod(this, "sendInvitation", Qt::QueuedConnection, Q_ARG(QString, email), Q_ARG(QString, code), Q_ARG(QString, owner), Q_ARG(QString, group));
    } else {
        QString serverName =  TemplateParser::instance().staticVar("APPTITLE").toString();
        QString joinUrl = TemplateParser::instance().staticVar("SERVERURL").toString() + "/signup/" + code;
        QString title = QString("%1 - Invitation").arg(serverName);

        if (!email.isEmpty())
        {
            // Create the email object
            QString emailMessage = QString(
                "<div style=\"font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Helvetica, Arial, sans-serif; max-width: 560px; margin: 0 auto; padding: 24px; color: #1e293b; line-height: 1.6;\">"
                "<h2 style=\"color: #0f172a; margin-top: 0;\">Invitation to %3</h2>"
                "<p>Hi there,</p>"
                "<p><b>%1</b> invites you to join the group <b>%2</b> on %3.</p>"
                "<p>Please follow this link to register and join:<br/>"
                "<a href=\"%4\" style=\"color: #2563eb; font-weight: 500;\">%4</a></p>"
                "%5"
                "</div>").arg(owner, group, serverName, joinUrl, personalSignature());
            Email emailToSend = createEmail(title, emailMessage, serverName, email);
            // Try to send the email
            qInfo() << "Sending invitation to" << email;
            m_smtpClient->sendEmail(emailToSend);
        }
    }
}
