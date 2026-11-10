#include "emailaddress.h"

EmailAddress::EmailAddress()
{
}

EmailAddress::EmailAddress(const EmailAddress &other)
{
    this->setAddress(other.address());
    this->setPassword(other.password());
    this->setName(other.name());
}

EmailAddress::EmailAddress(QString address, QString name, QString password)
{
    address_  = address;
    password_ = password;
    name_ = name;
}

EmailAddress &EmailAddress::operator=(const EmailAddress &other)
{
    if (this != &other)
    { // self-assignment check expected
        this->setAddress(other.address());
        this->setPassword(other.password());
        this->setName(other.name());
    }
    return *this;
}


QString EmailAddress::address() const
{
    return address_;
}

void EmailAddress::setAddress(const QString &address)
{
    address_ = address;
}

QString EmailAddress::password() const
{
    return password_;
}

void EmailAddress::setPassword(const QString &password)
{
    password_ = password;
}

QString EmailAddress::name() const
{
    return name_;
}

void EmailAddress::setName(const QString &name)
{
  name_ = name;
}
