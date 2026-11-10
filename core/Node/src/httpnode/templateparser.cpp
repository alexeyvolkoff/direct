#include "templateparser.h"
#include <QXmlStreamReader>

TemplateParser::TemplateParser() : QObject(nullptr), m_sessionAuth(nullptr)
{

}

TemplateParser &TemplateParser::instance()
{
    static TemplateParser parser;
    return  parser;
}


void TemplateParser::setSessionAuth(SessionAuth *sessionAuth)
{
    m_sessionAuth = sessionAuth;
}

void TemplateParser::setStaticVar(const QString &var_name, QVariant var)
{
    m_staticVars[var_name] = var;
}

void TemplateParser::setCondition(const QString &name, bool value)
{
    m_conditions[name] = value;
}

QByteArray TemplateParser::parse(Template html_template)
{
    //add all static variables
    Q_FOREACH(const QString& var_name, m_staticVars.keys()) {
        QVariant var = m_staticVars[var_name];
        html_template.setVariable(var_name, var.toString());
    }

    Q_FOREACH(const QString& cond_name, m_conditions.keys())
        html_template.setCondition(cond_name, m_conditions[cond_name]);

    return  html_template.toUtf8();
}

QByteArray TemplateParser::parse(const QString &session_id, Template html_template)
{
    //add all static variables
    Q_FOREACH(const QString& var_name, m_staticVars.keys()) {
        QVariant var = m_staticVars[var_name];
        html_template.setVariable(var_name, var.toString());
    }

    Q_FOREACH(const QString& cond_name, m_conditions.keys())
        html_template.setCondition(cond_name, m_conditions[cond_name]);



    bool locked = false;
    //add known session variables
    if (!session_id.isEmpty() && m_sessionAuth) {
        QPointer<ClientSession> session = m_sessionAuth->getSession(session_id);
        if (session) {
            //get item's attribute
            QHash<QString, QVariant> sessionVars = session->getSessionVariables();
            locked = sessionVars.value("locked", false).toBool();
            bool hasPreview = sessionVars.value("preview", true).toBool();
            QString title = strip(sessionVars.value("title", "").toString());
            QString appTitle = sessionVars.value("AppTitle").toString();
            if (appTitle.isEmpty()) appTitle = m_staticVars["FILES_APPTITLE"].toString();
            html_template.setVariable("APPTITLE", appTitle);
            QString slogan = sessionVars.value("Slogan").toString();
            if (slogan.isEmpty()) slogan = m_staticVars["FILES_SLOGAN"].toString();
            html_template.setVariable("SLOGAN", slogan);

            html_template.setVariable("USER", m_keyLogin ? QStringLiteral("anonymous") : sessionVars.value("User", QStringLiteral("anonymous")).toString());
            html_template.setVariable("OWNER", m_keyLogin ? QStringLiteral("anonymous") : sessionVars.value("Owner", "").toString());
            html_template.setVariable("ADMIN", sessionVars.value("isAdmin", false).toString());
            html_template.setVariable("GROUPADMIN", sessionVars.value("isGroupAdmin", false).toString());
            html_template.setVariable("GROUP", sessionVars.value("group", "").toString());
            html_template.setVariable("TOKEN", sessionVars.value("token", session_id).toString());
            html_template.setVariable("LINK", sessionVars.value("Link", "").toString());
            html_template.setVariable("ITEMPATH", sessionVars.value("Item", "").toString());
            html_template.setVariable("LINKURL", sessionVars.value("LinkUrl", "").toString());
            html_template.setVariable("SHOWITEM", sessionVars.value("ShowItem", "").toString());
            html_template.setVariable("CALLID", sessionVars.value("CallId", "").toString());
            html_template.setVariable("TITLE", title);
            qint64 coverVer = sessionVars.value("coverVersion", QDateTime::currentSecsSinceEpoch()).toLongLong();
            html_template.setVariable("COVERVERSION", QString::number(qAbs(coverVer)));
            html_template.setVariable("DESCRIPTION", strip(QString("%1 %2").arg(title, sessionVars.value("description", "").toString())));
            html_template.setVariable("SLUG", sessionVars.value("slug", "").toString());
            html_template.setVariable("EMAIL", sessionVars.value("email", "").toString());
            html_template.setVariable("TEMPLATEID", sessionVars.value("template_id", "").toString());
            html_template.setVariable("APPROVAL", sessionVars.value("approval", "").toString());
            html_template.setVariable("APP_KEY", sessionVars.value("application_key", "").toString());
            html_template.setVariable("AI_CHAT", sessionVars.value("ai_chat", true).toString());

            //Reset some on-time variables here
            session->setVar("ShowItem", "");

            if (hasPreview) {
                html_template.setVariable("IMG_WIDTH", QString::number(OG_IMG_WIDTH));
                html_template.setVariable("IMG_HEIGHT", QString::number(OG_IMG_HEIGHT));
                html_template.setVariable("CARD", "summary_large_image");
            } else {
                html_template.setVariable("IMG_WIDTH", QString::number(OG_IMG_WIDTH_MIN));
                html_template.setVariable("IMG_HEIGHT", QString::number(OG_IMG_HEIGHT_MIN));
                html_template.setVariable("CARD", "summary");
            }
        } else {
            html_template.setVariable("APPTITLE",  m_staticVars["FILES_APPTITLE"].toString());
            html_template.setVariable("SLOGAN", m_staticVars["FILES_SLOGAN"].toString());
        }
    } else {
        html_template.setVariable("APPTITLE",  m_staticVars["FILES_APPTITLE"].toString());
        html_template.setVariable("SLOGAN", m_staticVars["FILES_SLOGAN"].toString());
    }

    html_template.setVariable("LOCKED", locked ? "true" : "false");
    html_template.setCondition("LOCKED", locked);

    return  html_template.toUtf8();
}

QString TemplateParser::strip(const QString &strValue)
{
    QXmlStreamReader xml(strValue);

    QString plainValue = "";
    while (!xml.atEnd()) {
        if ( xml.readNext() == QXmlStreamReader::Characters ) {
            plainValue += xml.text();
        }
    }
    if (plainValue.isEmpty())
        plainValue = strValue;

    plainValue.replace("&", "&amp;");
    plainValue.replace("#", "");
    plainValue.replace("\"", "");
    plainValue.replace("'", "");
    plainValue.replace("{", "");
    plainValue.replace("}", "");
    plainValue.replace("[", "");
    plainValue.replace("]", "");
    return plainValue.trimmed();
}

TemplateParser::~TemplateParser()
{

}
