#ifndef EMAIL_H
#define EMAIL_H

#include "emailaddress.h"

/**
 * @brief The Email class is responsible for storing the email information.
 */
class Email
{
public:
    enum EmailType {
        Notification = 0, //plain emaol
        Invite //iCal event
    };

    Email();
    Email(const Email &other);

    Email& operator=(const Email& other);

    Email(EmailAddress credentials,
          EmailAddress from,
          EmailAddress to,
          QString subject,
          QString content,
          EmailType msg_type = Notification);

    EmailAddress credentials() const;
    void setCredentials(const EmailAddress &credentials);

    EmailAddress from() const;
    void setFrom(const EmailAddress &from);

    EmailAddress to() const;
    void setTo(const EmailAddress &to);

    QString subject() const;
    void setSubject(const QString &subject);

    QString content() const;
    void setContent(const QString &content);

    void setEvent(const QString &iCalEvent);
    QString event() const;

    EmailType type() const;
    void setType(const EmailType &msg_type);

private:
    QString content_;
    QString subject_;
    QString event_;
    EmailType type_ = Notification;

    EmailAddress credentials_;
    EmailAddress from_;
    EmailAddress to_;
};

#endif // EMAIL_H
