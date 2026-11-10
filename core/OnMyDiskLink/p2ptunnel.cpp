#include "p2ptunnel.h"

#ifdef P2P_ENABLED

#include <QDebug>
#include <QMetaObject>
#include <QString>
#include <QThread>

static QString kindForType(rtc::Description::Type type)
{
    return type == rtc::Description::Type::Answer ? QStringLiteral("answer") : QStringLiteral("offer");
}

P2PTunnel::P2PTunnel(Role role, QObject *parent)
    : QObject(parent), m_role(role), m_started(false), m_open(false)
{
    m_keepaliveTimer.setParent(this);
    m_stun << QStringLiteral("stun:disk.direct:3478")
           << QStringLiteral("stun:stun.cloudflare.com:3478")
           << QStringLiteral("stun:stun.l.google.com:19302")
           << QStringLiteral("stun:stun1.l.google.com:19302");
}

P2PTunnel::~P2PTunnel()
{
    close();
}

void P2PTunnel::setStunServers(const QStringList &urls)
{
    if (!urls.isEmpty())
        m_stun = urls;
}

void P2PTunnel::setTurnServer(const QString &url, const QString &user, const QString &pass)
{
    m_turnUrl = url;
    m_turnUser = user;
    m_turnPass = pass;
}

bool P2PTunnel::start()
{
    QStringList stunList;
    QString turnUrl, turnUser, turnPass;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_started)
            return true;
        stunList = m_stun;
        turnUrl = m_turnUrl;
        turnUser = m_turnUser;
        turnPass = m_turnPass;
    }

    auto config = std::make_shared<rtc::Configuration>();
    config->maxMessageSize = 16 * 1024 * 1024; // 16 MB max message size
    for (const QString &url : stunList)
    {
        if (url.isEmpty())
            continue;
        QString fullUrl = url;
        if (!fullUrl.startsWith(QLatin1String("stun:")) && !fullUrl.startsWith(QLatin1String("stuns:")))
            fullUrl = QStringLiteral("stun:") + fullUrl;
        try
        {
            std::string host = "stun.l.google.com";
            uint16_t port = 19302;
            int colon = fullUrl.lastIndexOf(QLatin1Char(':'));
            if (colon > 0)
            {
                int startIdx = fullUrl.indexOf(QLatin1String("stun:"));
                if (startIdx >= 0)
                    startIdx += 5;
                else
                    startIdx = 0;
                host = fullUrl.mid(startIdx, colon - startIdx).toUtf8().constData();
                port = fullUrl.mid(colon + 1).toUShort();
            }
            rtc::IceServer server(host, port);
            config->iceServers.push_back(server);
        }
        catch (const std::exception &e)
        {
            qWarning() << "P2PTunnel: std::exception adding STUN" << fullUrl << e.what();
        }
        catch (...)
        {
            qWarning() << "P2PTunnel: unknown exception adding STUN" << fullUrl;
        }
    }

    if (!turnUrl.isEmpty())
    {
        QString fullTurn = turnUrl;
        try
        {
            int startIdx = fullTurn.indexOf(QLatin1String("turn:"));
            if (startIdx >= 0) startIdx += 5;
            else startIdx = 0;

            int colon = fullTurn.lastIndexOf(QLatin1Char(':'));
#ifdef P2P_ENABLED
            std::string host = "disk.direct";
#else
            std::string host = "disk.direct";
#endif
            uint16_t port = 3478;

            if (colon > startIdx)
            {
                host = fullTurn.mid(startIdx, colon - startIdx).toUtf8().constData();
                port = fullTurn.mid(colon + 1).toUShort();
            }
            else
            {
                host = fullTurn.mid(startIdx).toUtf8().constData();
            }

            if (port == 0) port = 3478;

            rtc::IceServer turnUdp(host, port, turnUser.toStdString(), turnPass.toStdString(), rtc::IceServer::RelayType::TurnUdp);
            config->iceServers.push_back(turnUdp);
            rtc::IceServer turnTcp(host, port, turnUser.toStdString(), turnPass.toStdString(), rtc::IceServer::RelayType::TurnTcp);
            config->iceServers.push_back(turnTcp);


        }
        catch (const std::exception &e)
        {
            qWarning() << "P2PTunnel: std::exception adding TURN" << turnUrl << e.what();
        }
        catch (...)
        {
            qWarning() << "P2PTunnel: unknown exception adding TURN" << turnUrl;
        }
    }

    std::shared_ptr<rtc::PeerConnection> pc;
    try
    {
        pc = std::make_shared<rtc::PeerConnection>(*config);
    }
    catch (const std::exception &e)
    {
        qWarning() << "P2PTunnel: PeerConnection exception:" << e.what();
        return false;
    }
    catch (...)
    {
        qWarning() << "P2PTunnel: PeerConnection unknown exception!";
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_pc = pc;
        m_started = true;
    }

    pc->onLocalDescription([this](rtc::Description description) {
        const QString kind = kindForType(description.type());
        std::string s = description;
        const QByteArray payload(s.data(), static_cast<int>(s.size()));
        emit signalReady(kind, payload);
    });

    pc->onLocalCandidate([this](rtc::Candidate candidate) {
        std::string s = candidate;
        const QByteArray payload(s.data(), static_cast<int>(s.size()));
        emit signalReady(QStringLiteral("candidate"), payload);
    });

    pc->onGatheringStateChange([this](rtc::PeerConnection::GatheringState state) {
        if (state == rtc::PeerConnection::GatheringState::Complete)
            emit signalReady(QStringLiteral("done"), QByteArray());
    });

    pc->onStateChange([this](rtc::PeerConnection::State state) {
        if (state == rtc::PeerConnection::State::Failed)
            emit failed(QStringLiteral("ICE connection failed"));
    });

    if (m_role == Role::Offerer)
    {
        std::shared_ptr<rtc::DataChannel> dc;
        try
        {
            dc = pc->createDataChannel("omd");
        }
        catch (const std::exception &e)
        {
            qWarning() << "P2PTunnel: DataChannel error:" << e.what();
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_started)
                return false;
            m_dc = dc;
        }
        setupCallbacks(dc);
    }
    else
    {
        pc->onDataChannel([this](std::shared_ptr<rtc::DataChannel> dc) {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (!m_started)
                    return;
                m_dc = dc;
            }
            setupCallbacks(dc);
        });
    }

    return true;
}

static QString formatCandPairStr(const std::string &raw)
{
    const QString candStr = QString::fromUtf8(raw.data(), static_cast<int>(raw.size())).trimmed();
    const QStringList parts = candStr.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (parts.size() >= 8 && parts.at(0).startsWith(QLatin1String("candidate:")))
    {
        const QString ip = parts.at(4);
        const QString port = parts.at(5);
        const QString proto = parts.at(2);
        const QString type = parts.at(7);
        return QStringLiteral("%1:%2 (%3 %4)").arg(ip, port, proto, type);
    }
    return candStr;
}

void P2PTunnel::setupCallbacks(const std::shared_ptr<rtc::DataChannel> &dc)
{
    if (!dc)
        return;
    dc->onOpen([this]() {
        std::shared_ptr<rtc::PeerConnection> pc;
        std::shared_ptr<rtc::DataChannel> chan;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_started)
                return;
            m_open = true;
            pc = m_pc;
            chan = m_dc;
        }
        QString pairInfo;
        if (pc)
        {
            try {
                rtc::Candidate loc, rem;
                if (pc->getSelectedCandidatePair(&loc, &rem)) {
                    std::string lStr = loc;
                    std::string rStr = rem;
                    pairInfo = QStringLiteral("%1 <-> %2").arg(formatCandPairStr(lStr), formatCandPairStr(rStr));
                }
            } catch (...) {}
        }
        startKeepalive();
        // Signal readiness so the offerer can start sending RPCs. Without
        // this, the first RPC (AuthInfo) can be sent before this side's
        // onMessage callback is connected and gets lost.
        if (chan)
        {
            try {
                std::lock_guard<std::mutex> sendLock(m_sendMutex);
                chan->send(std::string("__ready__"));
            } catch (...) {}
        }
        emit opened(pairInfo);
    });
    dc->onClosed([this]() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_open = false;
        }
        stopKeepalive();
        emit closed();
    });
    dc->onMessage([this](rtc::message_variant data) {
        QByteArray msg;
        if (std::holds_alternative<std::string>(data)) {
            const std::string &s = std::get<std::string>(data);
            msg = QByteArray(s.data(), static_cast<int>(s.size()));
        } else if (std::holds_alternative<rtc::binary>(data)) {
            const rtc::binary &b = std::get<rtc::binary>(data);
            msg = QByteArray(reinterpret_cast<const char *>(b.data()), static_cast<int>(b.size()));
        }

        // Keepalive: respond to __ping__ with __pong__
        const QByteArray trimmed = msg.trimmed();
        if (trimmed == "__ping__") {
            std::shared_ptr<rtc::DataChannel> chan;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_open)
                    chan = m_dc;
            }
            if (chan) {
                try {
                    std::lock_guard<std::mutex> sendLock(m_sendMutex);
                    chan->send(std::string("__pong__"));
                } catch (...) {}
            }
            m_keepaliveMissed = 0;
            return;
        }
        if (trimmed == "__pong__") {
            m_keepaliveMissed = 0;
            return;
        }
        if (trimmed == "__ready__") {
            m_keepaliveMissed = 0;
            return;
        }

        // Any incoming data confirms link is alive
        m_keepaliveMissed = 0;
        emit dataReceived(msg);
    });
}

void P2PTunnel::handleSignaling(const QString &kind, const QByteArray &payload)
{
    std::lock_guard<std::mutex> sigLock(m_signalingMutex);
    std::shared_ptr<rtc::PeerConnection> pc;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        pc = m_pc;
    }
    if (!pc)
        return;
    try
    {
        if (kind == QLatin1String("offer") || kind == QLatin1String("answer"))
        {
            rtc::Description desc(std::string(payload.constData(), static_cast<size_t>(payload.size())));
            pc->setRemoteDescription(desc);

            std::vector<rtc::Candidate> buffered;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                buffered.swap(m_pendingCandidates);
            }
            // Apply any ICE candidates that arrived before the remote description
            for (const auto &cand : buffered)
            {
                try
                {
                    pc->addRemoteCandidate(cand);
                }
                catch (const std::exception &ce)
                {
                    qWarning() << "P2PTunnel: error applying buffered candidate:" << ce.what();
                }
            }
        }
        else if (kind == QLatin1String("candidate"))
        {
            std::string candStr(payload.constData(), static_cast<size_t>(payload.size()));
            rtc::Candidate cand(candStr, "0");
            if (pc->remoteDescription().has_value())
            {
                pc->addRemoteCandidate(cand);
            }
            else
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_started)
                    m_pendingCandidates.push_back(cand);
            }
        }

    }
    catch (const std::exception &e)
    {
        qWarning() << "P2PTunnel: signaling error:" << e.what();
    }
}

bool P2PTunnel::isOpen() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_open && m_dc != nullptr;
}

void P2PTunnel::sendData(const QByteArray &data)
{
    std::shared_ptr<rtc::DataChannel> chan;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_open || !m_dc)
            return;
        chan = m_dc;
    }
    std::lock_guard<std::mutex> sendLock(m_sendMutex);
    try {
        size_t maxChunk = chan->maxMessageSize();
        if (maxChunk == 0 || maxChunk > 64 * 1024)
            maxChunk = 64 * 1024;

        if (static_cast<size_t>(data.size()) <= maxChunk) {
            rtc::binary bin(reinterpret_cast<const std::byte*>(data.constData()),
                            reinterpret_cast<const std::byte*>(data.constData() + data.size()));
            chan->send(bin);
        } else {
            const char *ptr = data.constData();
            size_t remaining = static_cast<size_t>(data.size());
            while (remaining > 0) {
                size_t len = std::min(remaining, maxChunk);
                rtc::binary bin(reinterpret_cast<const std::byte*>(ptr),
                                reinterpret_cast<const std::byte*>(ptr + len));
                chan->send(bin);
                ptr += len;
                remaining -= len;
            }
        }
    } catch (const std::exception &e) {
        qWarning() << "P2PTunnel::sendData exception:" << e.what();
    } catch (...) {
        qWarning() << "P2PTunnel::sendData unknown exception";
    }
}

void P2PTunnel::sendText(const QByteArray &data)
{
    std::shared_ptr<rtc::DataChannel> chan;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_open || !m_dc)
            return;
        chan = m_dc;
    }
    std::lock_guard<std::mutex> sendLock(m_sendMutex);
    try {
        // Send as a WebRTC string frame so the browser receives event.data
        // as a DOMString (not ArrayBuffer). This is required for JSON-RPC
        // responses: large directories fragment into multiple SCTP chunks,
        // and the JS TextDecoder cannot reassemble them across binary frames.
        size_t maxChunk = chan->maxMessageSize();
        if (maxChunk == 0 || maxChunk > 64 * 1024)
            maxChunk = 64 * 1024; // Safe SCTP slice size: 64KB

        if (static_cast<size_t>(data.size()) <= maxChunk) {
            const std::string s(data.constData(), static_cast<size_t>(data.size()));
            chan->send(s);
        } else {
            const char *ptr = data.constData();
            size_t remaining = static_cast<size_t>(data.size());
            while (remaining > 0) {
                size_t len = std::min(remaining, maxChunk);
                // Ensure we never split a multi-byte UTF-8 sequence across chunk boundaries.
                // In UTF-8, continuation bytes have the form 10xxxxxx (0x80..0xBF).
                // Backtrack len so the start byte and all its continuation bytes remain together in the next chunk.
                if (len < remaining) {
                    while (len > 0 && (static_cast<unsigned char>(ptr[len]) & 0xC0) == 0x80) {
                        len--;
                    }
                }
                const std::string s(ptr, len);
                chan->send(s);
                ptr += len;
                remaining -= len;
            }
        }
    } catch (const std::exception &e) {
        qWarning() << "P2PTunnel::sendText exception:" << e.what();
    } catch (...) {
        qWarning() << "P2PTunnel::sendText unknown exception";
    }
}

void P2PTunnel::close()
{
    // Swap shared_ptrs out under m_mutex and release m_mutex BEFORE calling
    // resetCallbacks() / close() / shared_ptr destructors: libdatachannel's
    // synchronized_callback holds its own mutex while invoking onClosed /
    // onOpen / onMessage, so holding m_mutex across resetCallbacks() causes
    // an AB-BA deadlock when wake-up / ICE failure triggers both sides at once.
    std::shared_ptr<rtc::DataChannel> dc;
    std::shared_ptr<rtc::PeerConnection> pc;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_open = false;
        m_started = false;
        m_pendingCandidates.clear();
        dc.swap(m_dc);
        pc.swap(m_pc);
    }
    stopKeepalive();
    if (dc) {
        dc->resetCallbacks();
        try { dc->close(); } catch (...) {}
    }
    if (pc) {
        pc->resetCallbacks();
        try { pc->close(); } catch (...) {}
    }
    dc.reset();
    pc.reset();
}

void P2PTunnel::startKeepalive()
{
    // libdatachannel fires onOpen() from its own worker thread, so the QTimer
    // must be started on the thread that owns this P2PTunnel object.
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, [this]() { startKeepalive(); }, Qt::QueuedConnection);
        return;
    }
    m_keepaliveMissed = 0;
    if (!m_keepaliveConnected) {
        m_keepaliveConnected = true;
        connect(&m_keepaliveTimer, &QTimer::timeout, this, [this]() {
            std::shared_ptr<rtc::DataChannel> chan;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_open)
                    chan = m_dc;
            }
            if (!chan) {
                stopKeepalive();
                return;
            }
            try {
                {
                    std::lock_guard<std::mutex> sendLock(m_sendMutex);
                    chan->send(std::string("__ping__"));
                }
                const int missed = ++m_keepaliveMissed;
                if (missed > KEEPALIVE_MAX_MISSED) {
                    qWarning() << "P2PTunnel: keepalive timeout —" << missed << "pings unanswered";
                    {
                        std::lock_guard<std::mutex> lock(m_mutex);
                        m_open = false;
                    }
                    stopKeepalive();
                    emit failed(QStringLiteral("keepalive timeout"));
                    return;
                }
            } catch (const std::exception &e) {
                qWarning() << "P2PTunnel: keepalive send error:" << e.what();
            }
        });
    }
    m_keepaliveTimer.start(KEEPALIVE_INTERVAL_MS);
}

void P2PTunnel::stopKeepalive()
{
    // onClosed() also arrives from the libdatachannel worker thread.
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, [this]() { stopKeepalive(); }, Qt::QueuedConnection);
        return;
    }
    if (m_keepaliveTimer.isActive()) {
        m_keepaliveTimer.stop();
    }
    m_keepaliveMissed = 0;
}

#endif // P2P_ENABLED
