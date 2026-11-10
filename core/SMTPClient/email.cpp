#include "email.h"

Email::Email()
{
}

Email::Email(const Email &other)
{
    this->setTo(other.to());
    this->setFrom(other.from());
    this->setContent(other.content());
    this->setEvent(other.event());
    this->setSubject(other.subject());
    this->setCredentials(other.credentials());
    this->setType(other.type());
}

Email &Email::operator=(const Email &other)
{
    if (this != &other)
    { // self-assignment check expected
        this->setTo(other.to());
        this->setFrom(other.from());
        this->setContent(other.content());
        this->setEvent(other.event());
        this->setSubject(other.subject());
        this->setCredentials(other.credentials());
        this->setType(other.type());
    }
    return *this;
}


Email::Email(EmailAddress credentials, EmailAddress from, EmailAddress to, QString subject, QString content, EmailType msg_type)
{
    credentials_ = credentials;
    from_ = from;
    to_ = to;
    subject_ = subject;
    content_ = content;
    type_ = msg_type;
}

EmailAddress Email::credentials() const
{
    return credentials_;
}

void Email::setCredentials(const EmailAddress &credentials)
{
    credentials_.setAddress(credentials.address());
    credentials_.setPassword(credentials.password());
}

EmailAddress Email::from() const
{
    return from_;
}

void Email::setFrom(const EmailAddress &from)
{
    from_ = from;
}

EmailAddress Email::to() const
{
    return to_;
}

void Email::setTo(const EmailAddress &to)
{
    to_ = to;
}

QString Email::subject() const
{
    return subject_;
}

void Email::setSubject(const QString &subject)
{
    subject_ = subject;
}

QString Email::content() const
{
    return content_;
}

void Email::setContent(const QString &content)
{
    content_ = content;
}

void Email::setEvent(const QString &iCalEvent)
{
    event_ = iCalEvent;
}

QString Email::event() const
{
    return  event_;
}

Email::EmailType Email::type() const
{
    return type_;
}

void Email::setType(const EmailType &msg_type)
{
  type_ = msg_type;
}
