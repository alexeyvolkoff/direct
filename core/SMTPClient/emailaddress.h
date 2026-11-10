#ifndef EMAILADDRESS_H
#define EMAILADDRESS_H

#include <QString>

/**
 * @brief The EmailAddress class is used to store a email address
 * and a password when necessary.
 */
class EmailAddress
{
public:
    EmailAddress();
    EmailAddress(const EmailAddress& other);
    EmailAddress(QString address, QString name = "", QString password = "");

    EmailAddress& operator=(const EmailAddress& other);

    QString address() const;
    void setAddress(const QString &address);

    QString password() const;
    void setPassword(const QString &password);
    QString name() const;
    void setName(const QString &name);

private:
    QString address_;
    QString password_;
    QString name_;
};

#endif // EMAILADDRESS_H
