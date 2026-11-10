#ifndef TEMPLATEPARSER_H
#define TEMPLATEPARSER_H

#include <QObject>
#include "sessionauth.h"
#include "template.h"

using namespace stefanfrings;

class TemplateParser: public QObject
{
    Q_OBJECT
private:
    SessionAuth* m_sessionAuth;
    QHash<QString, QVariant> m_staticVars;
    QHash<QString, bool> m_conditions;
    TemplateParser();
    bool m_keyLogin;
public:
    static     TemplateParser & instance();
    void setSessionAuth(SessionAuth *sessionAuth);
    void setStaticVar(const QString &var_name, QVariant var);
    void setCondition(const QString &name, bool value);
    void setKeyLogin(bool keyLogin) { m_keyLogin = keyLogin; }
    QByteArray parse(Template html_template);
    QByteArray parse(const QString &session_id, Template html_template);
    QVariant staticVar(const QString &var_name) {return m_staticVars[var_name]; }
    static QString strip(const QString &strValue);
    ~TemplateParser();
};

#endif // TEMPLATEPARSER_H
