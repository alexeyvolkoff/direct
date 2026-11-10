#include "cloudresource.h"

int CloudResource::m_Handle = 100;
quint64 CloudResource::m_maxFileSize = PACKET_SIZE;

static const QStringList images = { "jpeg" ,  "jpg" , "png" , "bmp"};
static const QStringList convertibleDocs = {"docx", "odt", "pdf", "epub", "fb2", "csv"};
static const QStringList forbidenNames = {"admin", "administrator", "anonymous", "onmydisk", "onmydiskadmin", "onmydiskadministrator", "root", "user", "robot"};
static const QStringList plainTextDocs = {"txt", "htm", "html", "xml", "md", "markdown"};
static const QStringList unlistedFolders = {"shared", "shared-by-me","root", "public", "sites", "recent", "favorites", "links", "ref"};


bool CloudResource::isImage(const QString& fileName)
{
    QString suffix = QFileInfo(fileName).suffix();
    if (suffix.isEmpty()) return false;
    return images.contains(suffix.toLower());
}


bool CloudResource::isConvertible(const QString& fileName)
{
    QString suffix = QFileInfo(fileName).suffix();
    if (suffix.isEmpty()) return false;
    return convertibleDocs.contains(suffix.toLower());
}

bool CloudResource::isForbidden(const QString& name)
{
   return forbidenNames.contains(name.toLower());
}


bool CloudResource::isSkipped(const QString& fileName)
{
    return (unlistedFolders.contains(fileName) || fileName.startsWith(".") || fileName.startsWith("Gemfile") || fileName.startsWith("_site")  ||  fileName.endsWith("Readme.md") || fileName.endsWith("cover.jpg") || fileName.endsWith("cover.png") || fileName.endsWith("robots.txt") || fileName == "folder.jpg" || fileName == "cover.png" || fileName == "cover.jpg" ||
            fileName.startsWith("node_modules") || fileName.startsWith("$RECYCLE") || fileName.endsWith("Zone.Identifier"))  ; //skip md and cover files from indexing, they are used for parent's descriptions
}

bool CloudResource::isUnlisted(const QString& fileName)
{
    return unlistedFolders.contains(fileName);
}

bool CloudResource::isPlain(const QString& fileName)
{
    QString suffix = QFileInfo(fileName).suffix();
    if (suffix.isEmpty()) return false;
    return plainTextDocs.contains(suffix.toLower());
}


CloudResource::CloudResource()
{

}

CloudResource *CloudResource::getChild(const QString &childName, bool create)
{
    Q_UNUSED(childName)
    Q_UNUSED(create)
    return nullptr;
}


void CloudResource::generateBoundary(quint16 length)
{
    m_boundary = "onmydisk:";
    for(int i=0; i<length; ++i)
    {
        int index = QRandomGenerator::system()->generate()  % boundaryCharacters.length();
        m_boundary.append(boundaryCharacters.at(index));
    }
}


QString CloudResource::getBoundary()
{
    if (m_boundary.length() == 0)
        generateBoundary(16);

    return m_boundary;
}


ResourceType CloudResource::getResourceType() const
{
    if (this->isDir())
        return ResourceType::collection;
    else
        return ResourceType::document;

}

QString CloudResource::getResourceTypeString() const
{
    if (this->isDir())
        return QStringLiteral("dir");
    else
        return QStringLiteral("file");
}

void CloudResource::copyTo(CloudResource *dstResource, QHash<QString, QString> *failedPaths)
{
    if (this->isDir())
    {
        if (!dstResource->exists()) {
            if (!dstResource->mkcol())
            {
                failedPaths->insert(dstResource->fileName(), "HTTP/1.1 403 Forbidden");
                return;
            }
        }

        Q_FOREACH(QString childName, this->entryList())
        {
            CloudResource* childSrcResource = this->getChild(childName);
            CloudResource* dstChildRsource = dstResource->getChild(childName, true);
            childSrcResource->copyTo(dstChildRsource, failedPaths);
        }
    }
    else
    {
        if (!this->openResource(QIODevice::ReadOnly))
        {
            failedPaths->insert(this->fileName(), "HTTP/1.1 403 Forbidden");
            return;
        }

        if (!dstResource->openResource(QIODevice::WriteOnly))
        {
            this->close();
            failedPaths->insert(dstResource->fileName(), "HTTP/1.1 403 Forbidden");
            qCritical() << "Move: cannot open dest path" << dstResource->path();
            return;
        }

        while (!this->atEnd()) {
            QByteArray part = this->read(m_maxFileSize);
            if (part.size() == 0)
            {
                failedPaths->insert(dstResource->fileName(), "HTTP/1.1 403 Forbidden");
                break;
            }
            dstResource->write(part);
        }

        dstResource->close();
        this->close();
    }
}

void CloudResource::moveTo(CloudResource *dstResource, QHash<QString, QString> *failedPaths)
{
    this->copyTo(dstResource, failedPaths);

    if (failedPaths->count() == 0)
        this->remove();
}

QString CloudResource::toString()
{
    QStringList output;
    output.append("ID: " + GetID());
    output.append("Name: " + fileName());
    output.append(QString("Exists: %1").arg(exists()));
    output.append(QString("isDir: %1").arg(isDir()));
    output.append(QString("isFile: %1").arg(isFile()));

    return output.join(", ");
}

QHash<QString, QVariant> CloudResource::getCustomAttributes() const
{
	QHash<QString, QVariant> empty;
    return empty;
}

QHash<QString, QVariant> CloudResource::getLinkData() const
{
    QHash<QString, QVariant> empty;
    return empty;
}

QString CloudResource::getPermissions() const
{
    QString perms = "drwxr-xr-x";
    if (this->isFile())
    {
        perms[0]='-';
        perms[3]='-';
        perms[6]='-';
        perms[9]='-';
    }

    if (!this->isReadable())
    {
        perms[1]='-';
        perms[4]='-';
        perms[7]='-';
    }

    if (!this->isWritable())
    {
        perms[2]='-';
        perms[5]='-';
        perms[8]='-';
    }


    if (!this->isExecutable())
    {
        perms[3]='-';
        perms[6]='-';
        perms[9]='-';
    }

    return perms;
}

QString CloudResource::generateETag(bool quoted, QString path, QString lastModDate, qint64 size) const
{
    QString etagData = path;
    etagData += lastModDate;
    etagData += size;

    QString etagHash = QCryptographicHash::hash((etagData.toUtf8()),QCryptographicHash::Md5).toHex();
    if (quoted && !etagHash.startsWith("\""))
    {
        etagHash = etagHash.prepend("\"");
    }

    if (quoted && !etagHash.endsWith("\""))
    {
        etagHash = etagHash.append("\"");
    }

    return etagHash;
}

//defaults to current user
QString CloudResource::owner(const QString &user) const
{
    return user;
}

//defaults to path
QString CloudResource::remotePath() const
{
    return path();
}



