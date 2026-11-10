#ifndef P2PTUNNEL_H
#define P2PTUNNEL_H

#ifdef P2P_ENABLED

#include <QObject>
#include <QByteArray>
#include <QStringList>
#include <QTimer>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

#ifndef RTC_STATIC
#define RTC_STATIC
#endif
#include <rtc/rtc.hpp>

// P2PTunnel: WebRTC DataChannel wrapper (libdatachannel) used as a direct
// (peer-to-peer) path for tunnel traffic, bypassing the OMD gateway relay.
//
// Signaling is exchanged out-of-band through the gateway using
// LinkMessage::TunnelSignaling: kind is one of "offer", "answer",
// "candidate" or "done", and the payload carries the SDP / candidate text.
//
// libdatachannel invokes its callbacks from its own threads, so the signals
// emitted here are delivered to the receiver via queued connections
// (Qt AutoConnection), keeping all tunnel logic in the main thread.
class P2PTunnel : public QObject
{
    Q_OBJECT
public:
    enum class Role { Offerer, Answerer };

    explicit P2PTunnel(Role role, QObject *parent = nullptr);
    ~P2PTunnel() override;

    void setStunServers(const QStringList &urls);
    void setTurnServer(const QString &url, const QString &user, const QString &pass);
    bool start();
    void handleSignaling(const QString &kind, const QByteArray &payload);
    bool isOpen() const;
    void sendData(const QByteArray &data);  // binary frame — for TCP tunnel streams
    void sendText(const QByteArray &data);  // string frame — for JSON-RPC responses
    void close();

    static constexpr int KEEPALIVE_INTERVAL_MS = 30000;  // 30s per RFC 4960
    static constexpr int KEEPALIVE_MAX_MISSED = 3;       // 3 missed → 90s → dead

signals:
    void signalReady(const QString &kind, const QByteArray &payload);
    void dataReceived(const QByteArray &data);
    void opened(const QString &pairInfo = QString());
    void closed();
    void failed(const QString &error);

private:
    void setupCallbacks(const std::shared_ptr<rtc::DataChannel> &dc);
    void startKeepalive();
    void stopKeepalive();

    Role m_role;
    bool m_started = false;
    bool m_open = false;
    QStringList m_stun;
    QString m_turnUrl;
    QString m_turnUser;
    QString m_turnPass;
    std::shared_ptr<rtc::PeerConnection> m_pc;
    std::shared_ptr<rtc::DataChannel> m_dc;
    std::vector<rtc::Candidate> m_pendingCandidates;
    // Leaf mutex guarding state fields (m_started, m_open, m_pc, m_dc, m_pendingCandidates).
    // Never hold m_mutex while calling into libdatachannel (send / close / resetCallbacks)
    // to prevent AB-BA deadlocks with rtc::synchronized_callback::mutex.
    mutable std::mutex m_mutex;
    // Serializes multi-chunk DataChannel writes across worker threads without holding m_mutex.
    mutable std::mutex m_sendMutex;
    std::mutex m_signalingMutex;
    QTimer m_keepaliveTimer;
    bool m_keepaliveConnected = false;
    std::atomic<int> m_keepaliveMissed{0};
};

#endif // P2P_ENABLED

#endif // P2PTUNNEL_H
