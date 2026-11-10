#ifndef ONMYDISKLINK_GLOBAL_H
#define ONMYDISKLINK_GLOBAL_H

#include <QObject>
#include <QString>
#include <QMutex>
#include <QDateTime>
#include <QHash>
#include <QVariant>
#include <QtCore/qglobal.h>

#if defined(ONMYDISKLINK_LIBRARY)
#  define LINK_API Q_DECL_EXPORT
#else
// Sources are always compiled into the consuming binary (client/server)
// for all targets; do NOT fall back to Q_DECL_IMPORT here - on Windows
// dllimport would make the linker resolve __imp_ symbols that never exist
// (no DLL is produced).
#  define LINK_API
#endif

#define PING_INTERVAL 10000
#define RAG_REINDEX_INTERVAL_SECS 21600 /* 6h: periodic owner RAG re-index */
#define RAG_FLUSH_DEBOUNCE_MS 2000 /* collapse chunked upload indexes */
#define RAG_STARTUP_DELAY_SECS 30 /* defer initial share indexing until AI node is up */
#define PING_TIMEOUT 10000
#define LINK_THREAD_STACK_SIZE 1024000
#define HIGH_PRIORITY_THREADS 3
#define ONMYDISK_REALM "OnMyDisk 2.0"
#define EMPTY_PASSWORD ""
#define FILEHANDLE_ERROR -1

#define OG_IMG_WIDTH    1200
#define OG_IMG_HEIGHT   628
#define OG_IMG_WIDTH_MIN    240
#define OG_IMG_HEIGHT_MIN   240
#define OG_IMG_COVER_WIDTH  260
#define OG_IMG_COVER_HEIGHT 260

/* Processing video */
#define OG_VIDEO_COVER_IMG_FROM_SEC 5       // Default value, get image from 5th second of video file

#define SHARED_LINK_PREFIX "/shared/"       /* Simple direct links */
#define MOUNT_LINK_PREFIX  "/mount/"	    /* URL for mounting links */
#define ADMIN_ROOT         "admin"
#define CALL_ROOT          "call"
#define CALL_PREFIX        "/call"
#define CALL_LINK_PREFIX   "/call/"
#define SIGNED_WEBHOOK_PREFIX   "/signed"
#define SITES_PREFIX         "/sites"
#define SITES_LINK_PREFIX    "/sites/"
#define SEARCH_PREFIX        "/search"
#define PUBLIC_LINKS_PREFIX  "/public"
#define PERSONAL_LINKS_PREFIX  "/shared-by-me"
#define SUBMIT_FORM_PREFIX   "/submitform"
#ifdef QT_DEBUG
#define CHAT_DOMAIN         "onmychat.com"
#else
#define CHAT_DOMAIN         "onmychat.com"
#endif
#define CHAT_PREFIX         "/ai"

/* True when the request host belongs to the chat frontend. The same backend
 * serves both chat (onmychat.com) and files (direct.onmydisk.net) domains,
 * so this must be evaluated per request, not cached globally. */
static inline bool isChatHost(const QString &host)
{
    if (host.isEmpty())
        return false;
    QString h = host.trimmed().toLower();
    if (h.startsWith('[')) {
        // IPv6 literal; never a chat domain.
        return false;
    }
    const int colon = h.indexOf(':');
    if (colon > 0)
        h = h.left(colon);
    while (h.endsWith('.'))
        h.chop(1);
    const QString chatDomain = QString::fromLatin1(CHAT_DOMAIN);
    return h == chatDomain || h.endsWith(QStringLiteral(".") + chatDomain);
}

#define ONMYDISK_TRYLOCK_TIMEOUT 300

template <typename Mutex>
class OnMyDiskLocker {
  Mutex *m_mutex;
  bool locked_;
public:
  OnMyDiskLocker(Mutex *mutex) : m_mutex(mutex), locked_(m_mutex->tryLock(ONMYDISK_TRYLOCK_TIMEOUT)) {}
  ~OnMyDiskLocker() { if (locked_) m_mutex->unlock(); }
  bool isLocked() const { return locked_; }
  void unlock() { if (locked_) { m_mutex->unlock(); locked_ = false; } }
};

typedef struct {
    QString callId;
    QString callTopic;
    QString users;
    bool setMeet;
    QDateTime meet;
    QString organizer;
    QString organizerName;
} CallInfo;
Q_DECLARE_METATYPE(CallInfo)

typedef struct {
    QString linkID;
    QString shareName;
    QString name;
    QString type;
    QString siteTemplate;
    QString owner;
    QString domain;
    QStringList users;
    QStringList subscriptions;
} SiteInfo;
Q_DECLARE_METATYPE(SiteInfo)

typedef QHash<QString, QString> StringsHash;
Q_DECLARE_METATYPE(StringsHash)

typedef QHash<QString, QVariant> VariantHash;
Q_DECLARE_METATYPE(VariantHash)

typedef QHash<QString, SiteInfo> SitesHash;
Q_DECLARE_METATYPE(SitesHash)



#ifdef Q_OS_LINUX
#if QT_VERSION < 0x050F00
namespace  Qt {
    using  SplitBehaviorFlags = QString::SplitBehavior;
    Q_DECLARE_FLAGS(SplitBehavior, SplitBehaviorFlags)
    Q_DECLARE_OPERATORS_FOR_FLAGS(SplitBehavior)
}
#endif
#endif
#endif // ONMYDISKLINK_GLOBAL_H
