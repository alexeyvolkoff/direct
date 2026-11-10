#ifndef LOCALFILEBACKEND_H
#define LOCALFILEBACKEND_H

#include <QObject>
#include <QFile>
#include <QMimeDatabase>
#include <QStorageInfo>
#include "cloudresource.h"
#include "iresourcebackend.h"

class  LocalFileBackend : public IResourceBackend
{
    Q_OBJECT
public:
	explicit LocalFileBackend(const QString &filePath, const QString &remotePath, QObject *parent = nullptr);
    ~LocalFileBackend();

private:
    QFile m_backendFile;
    QFileInfo m_fileInfo;
    int m_fileHandle;
	bool m_writing;
	bool m_isRoot;
	static thread_local QMimeDatabase m_db;
	QString m_storageID;
	QString m_remotePath;
	QHash<QString, QVariant> genData(QFileInfo fileInfo) const;    
    QHash<QString, QVariant> genCustomAttributes(QFileInfo fileInfo, int fileCount = -1) const;
    int getFileCount(QFileInfo fileInfo) const;
signals:

public slots:

    // LinkIODevice interface
public:
    bool open(OpenMode mode);
    void close();
    bool seek(qint64 pos);
    bool atEnd() const;
    QByteArray readFromPosition(qint64 from, qint64 maxlen);
    bool isSequential() const;
    QString childFilePath(QString childPath);
    QString getFullPath() const;
    QString fileName() const;
    QString readMeFileName(QFileInfo fileInfo) const;
    QString readMeFileName() const;
    QString coverFileName(QFileInfo fileInfo) const;
    QString coverFileName() const;
    static QString audioCoverFile(const QFileInfo &fileInfo);
    static bool hasAudioCover(const QFileInfo &fileInfo);
    QString transcodeFileName(QFileInfo fileInfo) const;
    QString transcodeFileName() const;
    // Support of 'robots.txt' in folders
    bool isExcludedFromIndexing(QFileInfo fileInfo) const;
    bool isImage() const;
    bool isImage(QFileInfo fileInfo) const;
    bool isAudio() const;
    bool isAudio(QFileInfo fileInfo) const;
    bool isVideo() const;
    bool isVideo(QFileInfo fileInfo) const;
    bool isMarkdown() const;
    bool isMarkdown(QFileInfo fileInfo) const;
    bool isVector() const;
    bool isVector(QFileInfo fileInfo) const;
    // WP 3.2 - Multiformat text processing
    bool isPdf() const;
    bool isPdf(QFileInfo fileInfo) const;
    bool isImagePng() const;
    bool isImagePng(QFileInfo fileInfo) const;
    bool isMetaDocs() const;
    bool isMetaDocs(QFileInfo fileInfo) const;
    bool mkcol();
    bool rm(bool forced = false);
    bool mv(const QString& destPath);
    bool cp(const QString& destPath);
    bool rename(const QString& newName);
    int fileHandle() {return m_fileHandle;}


    static void markFileWriting(const QString &filePath, bool isWriting);
    static void touchFileWriting(const QString &filePath, int timeoutMs = 30000);
    static bool isFileWriting(const QString &filePath);
    static QDateTime getMediaCreationDateTime(const QFileInfo &fileInfo);

protected:
    qint64 readData(char *data, qint64 maxlen);
    qint64 writeData(const char *data, qint64 len);

    // IResourceBackend interface
public:
    bool RefreshInfo(CloudResource *resource);
    bool RefreshAllInfo(CloudResource *resource);
    virtual bool RefreshAttributes(CloudResource *resource);
    QHash<QString, QHash<QString, QVariant> > getEntryData() const;

    // QIODevice interface
public:
    qint64 size() const;

    // IResourceBackend interface
public:
    QString storageID();
};

#endif // LOCALFILEBACKEND_H
