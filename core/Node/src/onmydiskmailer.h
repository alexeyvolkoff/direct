#ifndef ONMYDISKMAILER_H
#define ONMYDISKMAILER_H

#include <QObject>
#include <QSettings>
#include "SMTPClient/smtpclient.h"
#include "SMTPClient/emailaddress.h"
#include "SMTPClient/email.h"
#include "onmydisklink_global.h"


class OnMyDiskMailer : public QObject
{
    Q_OBJECT
private:
    QSettings* m_settings;
    SMTPClient *m_smtpClient;
    bool m_keyLogin;

    explicit OnMyDiskMailer();
    QString getOwnerOfLink(QString linkID, bool &isOk);
public:
    void init(QSettings *settings, bool keyLogin);
    static OnMyDiskMailer &instance();
    static bool isValidEmail(const QString& email);
    static QString createICalEvent(QString method, QString companyName, QString appName, QDateTime date,  QString title, QString linkUrl,  QString organizer, QString attendees);

    ~OnMyDiskMailer();

    //email notifications
    Email createEmail(QString subject, QString body, QString appName, QString to);
private slots:
    void onSmtpStatus(Status::e status, QString message);

public slots:
    void sendActivationLink(const QString &activationCode, const QString &email);
    void sendCallNotification(const CallInfo &callInfo);
    void sendLinkNotification(const QString &sender, const QString &senderEmail, const QString &link, const StringsHash &emails, const VariantHash &linkData);

    void sendGroupInvite(const QString &sender,  const QString &group, const QString &groupId, const QString &userEmail);
    void sendInvitation(const QString &email, const QString &code, const QString &owner, const QString &group);
    void publicLinkAdded(QString linkID, const VariantHash &linkResponseData);
    void sendCustomForm(const QString &user, StringsHash formData);
signals:

};

#endif // ONMYDISKMAILER_H
