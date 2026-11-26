#include "XrdHttpTpcPool.hh"

#include <XrdOuc/XrdOucEnv.hh>
#include <XrdSys/XrdSysError.hh>

#include <openssl/sha.h>

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <unistd.h>

#include "XrdHttpTpcPMarkManager.hh"
#include "XrdHttpTpcTPC.hh"

using namespace TPC;

decltype(TPCRequestManager::m_pool_map) TPCRequestManager::m_pool_map;
decltype(TPCRequestManager::m_mutex) TPCRequestManager::m_mutex;
decltype(TPCRequestManager::m_idle_timeout) TPCRequestManager::m_idle_timeout = std::chrono::minutes(1);
unsigned TPCRequestManager::m_max_pending_ops = 20;  // tpc.max_waiting_transfers_per_queue
unsigned TPCRequestManager::m_max_workers = 20;      // tpc.max_active_transfers_per_queue
std::atomic<unsigned> TPCRequestManager::m_max_global_workers{4096};  // tpc.max_active_transfers
std::atomic<unsigned> TPCRequestManager::m_live{0};
std::atomic<bool> TPCRequestManager::m_shutdown{false};
std::mutex TPCRequestManager::m_drain_mu;
std::condition_variable TPCRequestManager::m_drain_cv;
std::deque<std::weak_ptr<TPCRequestManager::TPCQueue>> TPCRequestManager::m_starved;
std::atomic<int> TPCRequestManager::m_starved_count{0};

namespace {

std::string Lower(std::string value) {
    for (char &ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value;
}

std::string Fingerprint(const std::string &data) {
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char *>(data.data()), data.size(), digest);
    static const char *hex = "0123456789abcdef";
    std::string out(32, '0');
    for (int i = 0; i < 16; ++i) {
        out[static_cast<size_t>(i) * 2] = hex[digest[i] >> 4];
        out[static_cast<size_t>(i) * 2 + 1] = hex[digest[i] & 0xf];
    }
    return out;
}

// One log field.  '|' separates fields, so it cannot appear inside one.
std::string Field(const std::string &value) {
    std::string out = value;
    for (char &ch : out) {
        if (ch == '|' || ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t') {
            ch = '_';
        }
    }
    return out.empty() ? "-" : out;
}

// scheme, host, port, and userinfo.  The path is ignored on purpose: two
// files on the same endpoint share a connection.
bool ParseRemote(const std::string &url, std::string &scheme, std::string &host, int &port, std::string &userinfo) {
    const auto scheme_end = url.find("://");
    if (scheme_end == std::string::npos || scheme_end == 0) {
        return false;
    }
    scheme = Lower(url.substr(0, scheme_end));
    if (scheme != "http" && scheme != "https") {
        return false;
    }
    size_t pos = scheme_end + 3;
    if (pos >= url.size()) {
        return false;
    }

    const auto slash = url.find_first_of("/?#", pos);
    const auto at = url.find('@', pos);
    if (at != std::string::npos && (slash == std::string::npos || at < slash)) {
        userinfo = url.substr(pos, at - pos);
        pos = at + 1;
    }
    if (pos >= url.size()) {
        return false;
    }

    size_t host_end = pos;
    if (url[pos] == '[') {
        const auto bracket = url.find(']', pos);
        if (bracket == std::string::npos) {
            return false;
        }
        host = Lower(url.substr(pos, bracket + 1 - pos));
        host_end = bracket + 1;
    } else {
        while (host_end < url.size() && url[host_end] != ':' && url[host_end] != '/' && url[host_end] != '?' && url[host_end] != '#') {
            ++host_end;
        }
        host = Lower(url.substr(pos, host_end - pos));
    }
    if (host.empty() || host == "[]") {
        return false;
    }

    port = scheme == "https" ? 443 : 80;
    if (host_end < url.size() && url[host_end] == ':') {
        size_t digit = host_end + 1;
        if (digit >= url.size() || !std::isdigit(static_cast<unsigned char>(url[digit]))) {
            return false;
        }
        long value = 0;
        while (digit < url.size() && std::isdigit(static_cast<unsigned char>(url[digit]))) {
            value = value * 10 + (url[digit] - '0');
            if (value > 65535) {
                return false;
            }
            ++digit;
        }
        if (value <= 0) {
            return false;
        }
        port = static_cast<int>(value);
    }
    return true;
}

bool HeaderSelectsCredential(const std::string &name) {
    return name.rfind("transferheader", 0) == 0 || name.rfind("copy-header", 0) == 0;
}

template <typename T>
void EraseWorker(std::vector<T *> &workers, T *worker) {
    workers.erase(std::remove(workers.begin(), workers.end(), worker), workers.end());
}

}  // namespace

std::string TPC::MakeQueueKey(const QueueKeyInput &in) {
    std::string scheme, host, userinfo;
    int port = 0;
    const bool parsed = ParseRemote(in.url, scheme, host, port, userinfo);

    std::vector<std::pair<std::string, std::string>> headers;
    headers.reserve(in.headers.size());
    for (const auto &header : in.headers) {
        std::string name = Lower(header.first);
        if (HeaderSelectsCredential(name)) {
            headers.emplace_back(std::move(name), header.second);
        }
    }
    std::sort(headers.begin(), headers.end());

    const bool identified = !headers.empty() || !userinfo.empty() || !in.x509_name.empty() || !in.creds.empty();
    std::string credential = "-";
    if (!parsed) {
        // The raw URL can carry a secret.  Hash the whole thing and do not
        // share the queue with any transfer we could parse.
        credential = Fingerprint(in.url + "\n" + in.x509_name + "\n" + in.creds);
    } else if (identified) {
        std::string blob;
        for (const auto &header : headers) {
            blob += header.first;
            blob.push_back('=');
            blob += header.second;
            blob.push_back('\n');
        }
        blob += "userinfo=";
        blob += userinfo;
        blob += "\nname=";
        blob += in.x509_name;
        blob += "\ncreds=";
        blob.append(in.creds.data(), in.creds.size());
        credential = Fingerprint(blob);
    }

    std::ostringstream key;
    if (parsed) {
        key << scheme << "://" << host << ':' << port;
    } else {
        key << "tpc|unparsed";
    }
    key << "|s=" << (in.scitag < 0 ? std::string("-") : std::to_string(in.scitag))
        << "|v=" << Field(in.vorg)
        << "|b=" << Field(in.bind_ip)
        << "|c=" << credential;
    return key.str();
}

void TPCRequestManager::TPCQueue::TPCWorker::RunStatic(std::shared_ptr<TPCQueue> queue, std::unique_ptr<TPCWorker> myself) {
    myself->Run();
    myself.reset();
    if (queue->IsDone()) {
        queue->m_parent.Done(queue->m_identifier, queue.get());
    }
    queue->m_parent.ReleaseSlot();
}

void TPCRequestManager::TPCQueue::TPCWorker::WakeThunk(void *arg) {
    static_cast<TPCWorker *>(arg)->Wake();
}

void TPCRequestManager::TPCQueue::TPCWorker::Wake() {
    std::lock_guard<std::mutex> lock(m_wake_mu);
#if LIBCURL_VERSION_NUM >= 0x074400
    if (m_multi) {
        curl_multi_wakeup(m_multi);
    }
#endif
}

int TPCRequestManager::TPCQueue::TPCWorker::closesocket_callback(void *clientp, curl_socket_t fd) {
    auto *worker = static_cast<TPCWorker *>(clientp);
    // libcurl copies this callback onto the connection.  Cached connections can
    // close after the HTTP thread has destroyed the request's PMarkManager, so
    // only call endPmark while RunCurl still owns the transfer.
    if (worker && worker->m_active_pmark) {
        worker->m_active_pmark->endPmark(fd);
    }
    return close(fd);
}

TPCRequestManager::TPCQueue::TPCWorker::CurlOutcome
TPCRequestManager::TPCQueue::TPCWorker::RunCurl(CURLM *multi_handle, TPCRequestManager::TPCRequest &request) {
    // Disarm on every return so Cancel() cannot wake a multi-handle the
    // worker has already finished with.
    struct Disarm {
        TPCRequest &request;
        ~Disarm() { request.DisarmWake(); }
    } disarm{request};
    request.ArmWake(&TPCWorker::WakeThunk, this);

    request.MarkStarted();
    if (request.IsCancelled() || m_queue.IsDone() || m_queue.m_parent.IsShutdown()) {
        return {"Transfer cancelled", -1, false, false};
    }

    CURLMcode mres;
    auto curl = request.GetHandle();
    // Bind CLOSESOCKET to this worker.  libcurl snapshots the callback on the
    // connection at open time, and the worker outlives connections cached on
    // the multi-handle.  The per-request TPCLogRecord does not.
    m_active_pmark = request.GetPMark();
    curl_easy_setopt(curl, CURLOPT_CLOSESOCKETFUNCTION, closesocket_callback);
    curl_easy_setopt(curl, CURLOPT_CLOSESOCKETDATA, this);
#if LIBCURL_VERSION_NUM >= 0x074100
    // libcurl stops reusing a connection idle for 118 seconds by default; keep
    // it for as long as the worker that caches it.
    curl_easy_setopt(curl, CURLOPT_MAXAGE_CONN,
                     static_cast<long>(std::chrono::duration_cast<std::chrono::seconds>(m_idle_timeout).count()));
#endif
    // The caller configures the easy handle for this transfer.  These two are
    // connection-cache switches, and a previous HEAD on this handle may have
    // set them.  A healthy transfer is allowed to return its connection.
    curl_easy_setopt(curl, CURLOPT_FORBID_REUSE, 0L);
    curl_easy_setopt(curl, CURLOPT_FRESH_CONNECT, 0L);

    auto finish = [&](const std::string &msg, int curl_result, bool reuse, bool discard) {
        // Emit this transfer's firefly while its socket is still open.  libcurl
        // keeps the connection in this worker's cache unless the caller then
        // discards it.
        if (m_active_pmark) {
            m_active_pmark->endTransferMarks();
        }
        m_active_pmark = nullptr;
        return CurlOutcome{msg, curl_result, reuse, discard};
    };

    mres = curl_multi_add_handle(multi_handle, curl);
    if (mres) {
        std::stringstream ss;
        ss << "Failed to add transfer to libcurl multi-handle: HTTP library "
              "failure="
           << curl_multi_strerror(mres);
        m_queue.m_parent.m_log.Log(LogMask::Error, "TPCWorker", ss.str().c_str());
        return finish(ss.str(), -1, false, false);
    }

    auto fail = [&](const std::string &msg, LogMask lvl) {
        curl_multi_remove_handle(multi_handle, curl);
        m_queue.m_parent.m_log.Log(lvl, "TPCWorker", msg.c_str());
        return finish(msg, -1, false, true);
    };

    CURLcode res = static_cast<CURLcode>(-1);
    int running_handles = 1;
    const int update_interval{1};
    time_t now = time(NULL);
    time_t last_update = now - update_interval;

    while (running_handles) {
        mres = curl_multi_perform(multi_handle, &running_handles);
        if (mres != CURLM_OK) {
            return fail("Internal curl multi-handle error: " + std::string(curl_multi_strerror(mres)), LogMask::Error);
        }
        if (m_active_pmark) {
            m_active_pmark->markCurlHandle(curl);
        }

        now = time(NULL);
        if (now - last_update >= update_interval) {
            request.UpdateRemoteConnDesc();
            last_update = now;
        }

        CURLMsg *msg;
        do {
            int msgq = 0;
            msg = curl_multi_info_read(multi_handle, &msgq);
            if (msg && (msg->msg == CURLMSG_DONE)) {
                res = msg->data.result;
                break;
            }
        } while (msg);

        if (request.IsCancelled() || m_queue.IsDone() || m_queue.m_parent.IsShutdown()) {
            return fail("Transfer cancelled", LogMask::Info);
        }
        if (running_handles == 0) {
            break;
        }

        // curl_multi_wakeup interrupts curl_multi_poll.  curl_multi_wait
        // ignores it, so a cancel would sit until this timeout.
#if LIBCURL_VERSION_NUM >= 0x074200
        mres = curl_multi_poll(multi_handle, NULL, 0, 1000, nullptr);
#else
        mres = curl_multi_wait(multi_handle, NULL, 0, 1000, nullptr);
#endif
        if (mres != CURLM_OK) {
            return fail("Error while waiting for the transfer: " + std::string(curl_multi_strerror(mres)), LogMask::Error);
        }
    }

    request.UpdateRemoteConnDesc();

    if (res == static_cast<CURLcode>(-1)) {
        return fail("Internal state error in libcurl - no transfer results returned", LogMask::Error);
    }

    curl_multi_remove_handle(multi_handle, curl);

    // A failed, cancelled, or redirected transfer must not leave its
    // connection for the next request.  libcurl has already decided whether
    // to cache it by the time we get here, so the caller drops the cache.
    bool reuse = res == CURLE_OK && !request.IsCancelled();
    if (reuse) {
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        long redirects = 0;
        curl_easy_getinfo(curl, CURLINFO_REDIRECT_COUNT, &redirects);
        if (status <= 0 || status >= 400 || redirects > 0) {
            reuse = false;
        }
    }
    return finish("Transfer complete", res, reuse, !reuse);
}

CURLM *TPCRequestManager::TPCQueue::TPCWorker::DiscardCached(CURLM *multi_handle) {
    {
        std::lock_guard<std::mutex> lock(m_wake_mu);
        m_multi = nullptr;
    }
    if (multi_handle) {
        curl_multi_cleanup(multi_handle);
    }
    CURLM *fresh = curl_multi_init();
    if (fresh) {
        curl_multi_setopt(fresh, CURLMOPT_MAXCONNECTS, 1L);
    }
    std::lock_guard<std::mutex> lock(m_wake_mu);
    m_multi = fresh;
    return fresh;
}

void TPCRequestManager::TPCQueue::TPCWorker::Run() {
    auto &log = m_queue.m_parent.m_log;
    log.Log(LogMask::Info, "TPCWorker", "Worker for", m_queue.m_identifier.c_str(), "starting");

    // The multi-handle's connection cache is what lets later transfers in this
    // queue reuse the connections opened by earlier ones.
    CURLM *multi_handle = curl_multi_init();
    if (multi_handle) {
        // A pull through a redirector opens a second connection. Keep only
        // the one in use, so the redirector socket does not stay idle for
        // the whole transfer. The data connection remains for the next copy
        // when the transfer stayed on one host.
        curl_multi_setopt(multi_handle, CURLMOPT_MAXCONNECTS, 1L);
    }
    {
        std::lock_guard<std::mutex> lock(m_wake_mu);
        m_multi = multi_handle;
    }

    if (!multi_handle) {
        log.Log(LogMask::Error, "TPCWorker",
                "Unable to create"
                " a libcurl multi-handle; fatal error for worker");
        std::unique_lock<std::mutex> lk(m_queue.m_mutex);
        m_force_stop = true;
        m_queue.DetachWorker(*this);
    }

    while (multi_handle && !m_force_stop) {
        TPCRequest *request = m_queue.WaitForRequest(*this);
        if (!request) {
            break;
        }
        CurlOutcome outcome = RunCurl(multi_handle, *request);
        if (outcome.discard) {
            multi_handle = DiscardCached(multi_handle);
        }
        bool leave = false;
        TPCRequest *next = nullptr;
        if (!multi_handle) {
            m_force_stop = true;
        }
        next = m_queue.HandOff(*this, leave);
        // The worker is idle, or already holds the next request, before the
        // client is released.  The client can submit the following transfer
        // and be given this connection.
        request->SetDone(outcome.message, outcome.curl_result);
        if (leave || !multi_handle) {
            if (next) {
                next->SetDone("TPC worker could not start");
            }
            break;
        }
        while (next) {
            request = next;
            outcome = RunCurl(multi_handle, *request);
            if (outcome.discard) {
                multi_handle = DiscardCached(multi_handle);
            }
            next = nullptr;
            if (!multi_handle) {
                m_force_stop = true;
            }
            next = m_queue.HandOff(*this, leave);
            request->SetDone(outcome.message, outcome.curl_result);
            if (leave || !multi_handle) {
                if (next) {
                    next->SetDone("TPC worker could not start");
                }
                next = nullptr;
                leave = true;
            }
        }
        if (leave || !multi_handle) {
            break;
        }
    }

    {
        std::unique_lock<std::mutex> lk(m_queue.m_mutex);
        if (std::find(m_queue.m_workers.begin(), m_queue.m_workers.end(), this) != m_queue.m_workers.end()) {
            m_queue.DetachWorker(*this);
        }
    }
    {
        std::lock_guard<std::mutex> lock(m_wake_mu);
        CURLM *multi = m_multi;
        m_multi = nullptr;
        if (multi) {
            curl_multi_cleanup(multi);
        }
    }
    log.Log(LogMask::Info, "TPCWorker", "Worker for", m_queue.m_identifier.c_str(), "exiting");
}

TPCRequestManager::TPCRequest *TPCRequestManager::TPCQueue::WaitForRequest(TPCWorker &worker) {
    std::unique_lock<std::mutex> lk(m_mutex);
    const auto deadline = std::chrono::steady_clock::now() + m_idle_timeout;
    while (true) {
        if (worker.m_claimed) {
            worker.m_claimed = false;
            if (m_claims > 0) {
                m_claims--;
            }
        }
        if (worker.m_reap && !m_ops.empty()) {
            // Work arrived after an idle reap was requested.  Run it.
            worker.m_reap = false;
        }
        const bool stopping = worker.m_force_stop || worker.m_reap || m_done.load(std::memory_order_relaxed) || m_parent.IsShutdown();
        if (!stopping && !m_ops.empty()) {
            // An extra worker yields when another link has nothing to run on.
            // The last worker of this queue keeps draining it.
            if (!(m_parent.HasStarved() && m_workers.size() > 1)) {
                auto result = m_ops.front();
                m_ops.pop_front();
                EraseWorker(m_idle, &worker);
                return result;
            }
        }
        if (stopping || (m_ops.empty() && m_parent.HasStarved()) || (m_parent.HasStarved() && m_workers.size() > 1)) {
            DetachWorker(worker);
            return nullptr;
        }

        if (std::find(m_idle.begin(), m_idle.end(), &worker) == m_idle.end()) {
            m_idle.push_back(&worker);
        }
        bool woken = worker.m_cv.wait_until(lk, deadline, [&] {
            return worker.m_claimed || worker.m_reap || m_done.load(std::memory_order_acquire);
        });
        EraseWorker(m_idle, &worker);
        if (worker.m_claimed) {
            // The request may already have been taken by a worker that just
            // finished; the loop then waits again until the same deadline.
            continue;
        }
        if (!woken || worker.m_reap || m_done.load(std::memory_order_relaxed)) {
            DetachWorker(worker);
            return nullptr;
        }
    }
}

TPCRequestManager::TPCRequest *TPCRequestManager::TPCQueue::HandOff(TPCWorker &worker, bool &leave) {
    leave = false;
    std::unique_lock<std::mutex> lk(m_mutex);
    if (worker.m_claimed) {
        worker.m_claimed = false;
        if (m_claims > 0) {
            m_claims--;
        }
    }
    if (worker.m_reap && !m_ops.empty() && !worker.m_force_stop) {
        worker.m_reap = false;
    }
    const bool stopping = worker.m_force_stop || worker.m_reap || m_done.load(std::memory_order_relaxed) || m_parent.IsShutdown();
    const bool yield = m_parent.HasStarved() && (m_ops.empty() || m_workers.size() > 1);
    if (stopping || yield) {
        leave = true;
        DetachWorker(worker);
        return nullptr;
    }
    if (!m_ops.empty()) {
        auto *result = m_ops.front();
        m_ops.pop_front();
        EraseWorker(m_idle, &worker);
        return result;
    }
    // Idle before the caller finishes the request, so the client that
    // submits the next transfer finds this worker and its connection.
    if (std::find(m_idle.begin(), m_idle.end(), &worker) == m_idle.end()) {
        m_idle.push_back(&worker);
    }
    return nullptr;
}

void TPCRequestManager::TPCQueue::DetachWorker(TPCWorker &worker) {
    EraseWorker(m_idle, &worker);
    EraseWorker(m_workers, &worker);
    if (!m_workers.empty()) {
        return;
    }
    if (worker.m_force_stop || m_parent.IsShutdown()) {
        const char *msg = m_parent.IsShutdown() ? "Transfer cancelled" : "TPC worker could not start";
        for (TPCRequest *op : m_ops) {
            op->SetDone(msg);
        }
        m_ops.clear();
    }
    m_done.store(true, std::memory_order_release);
}

bool TPCRequestManager::TPCQueue::ClaimIdleWorker() {
    if (m_idle.empty()) {
        return false;
    }
    // The most recently idle worker is the one most likely to still hold a live
    // connection; the others keep idling and eventually exit.
    auto worker = m_idle.back();
    m_idle.pop_back();
    worker->m_claimed = true;
    worker->m_reap = false;
    m_claims++;
    worker->m_cv.notify_one();
    return true;
}

bool TPCRequestManager::TPCQueue::SpawnWorker() {
    auto worker = std::make_unique<TPCWorker>(*this);
    worker->m_claimed = true;
    m_workers.push_back(worker.get());
    m_claims++;
    try {
        std::thread t(TPCWorker::RunStatic, shared_from_this(), std::move(worker));
        t.detach();
    } catch (const std::system_error &e) {
        m_workers.pop_back();
        if (m_claims > 0) {
            m_claims--;
        }
        m_parent.m_log.Log(LogMask::Error, "TPCQueue", "Unable to start a TPC worker thread:", e.what());
        return false;
    }
    return true;
}

bool TPCRequestManager::TPCQueue::NeedsWorker() const {
    return !m_done.load(std::memory_order_relaxed) && !m_parent.IsShutdown() && !m_ops.empty() && m_workers.empty() && m_claims == 0;
}

bool TPCRequestManager::TPCQueue::TrySpawnForStarved() {
    std::unique_lock<std::mutex> lk(m_mutex);
    if (!NeedsWorker()) {
        return true;
    }
    if (!m_parent.AcquireSlot()) {
        return false;
    }
    if (!SpawnWorker()) {
        const bool empty = m_workers.empty();
        if (empty) {
            for (TPCRequest *op : m_ops) {
                op->SetDone("TPC worker could not start");
            }
            m_ops.clear();
            m_done.store(true, std::memory_order_release);
        }
        lk.unlock();
        m_parent.ReleaseSlot();
        return true;
    }
    return true;
}

// Queue a request for processing.  If the queue is full, the request will
// be rejected and false will be returned.
//
// - An idle worker of this queue, the one idle most recently, is preferred.
// - Otherwise a new worker is started when this queue is under its cap and
//   the pool is under the global cap.
// - At the per-queue cap, or at the global cap while this queue already has
//   a worker, the request waits here.  It does not sit behind another link.
// - At the global cap with no worker, the queue is starved and is started
//   when some other queue gives up a slot.
bool TPCRequestManager::TPCQueue::Produce(TPCRequest &handler) {
    bool starved = false;
    {
        std::unique_lock<std::mutex> lk(m_mutex);
        if (m_done.load(std::memory_order_acquire) || m_parent.IsShutdown()) {
            return false;
        }
        if (m_ops.size() >= m_max_pending_ops) {
            m_parent.m_log.Log(LogMask::Warning, "TPCQueue", "Queue is full; rejecting request");
            return false;
        }

        m_ops.push_back(&handler);
        if (ClaimIdleWorker() || m_ops.size() <= m_claims || m_workers.size() >= m_max_workers) {
            return true;
        }
        if (!m_parent.AcquireSlot()) {
            starved = m_workers.empty() && m_claims == 0;
            if (!starved) {
                return true;
            }
        } else if (!SpawnWorker()) {
            m_ops.pop_back();
            lk.unlock();
            m_parent.ReleaseSlot();
            return false;
        } else {
            return true;
        }
    }
    m_parent.NoteStarved(shared_from_this());
    return true;
}

bool TPCRequestManager::TPCQueue::ProduceIfIdle(TPCRequest &handler) {
    std::unique_lock<std::mutex> lk(m_mutex);
    if (m_done.load(std::memory_order_acquire) || m_parent.IsShutdown() || m_idle.empty()) {
        return false;
    }
    if (m_ops.size() >= m_max_pending_ops) {
        return false;
    }
    m_ops.push_back(&handler);
    ClaimIdleWorker();
    return true;
}

bool TPCRequestManager::TPCQueue::Withdraw(TPCRequest &handler) {
    std::unique_lock<std::mutex> lk(m_mutex);
    auto it = std::find(m_ops.begin(), m_ops.end(), &handler);
    if (it == m_ops.end()) {
        return false;
    }
    m_ops.erase(it);
    return true;
}

void TPCRequestManager::TPCQueue::Stop() {
    std::unique_lock<std::mutex> lk(m_mutex);
    m_done.store(true, std::memory_order_release);
    for (TPCRequest *op : m_ops) {
        op->SetDone("Transfer cancelled");
    }
    m_ops.clear();
    for (TPCWorker *worker : m_workers) {
        worker->m_reap = true;
        worker->m_cv.notify_one();
        // Wake curl while this lock is held.  The worker cannot destroy
        // itself until it acquires the same lock to leave the queue.
        worker->Wake();
    }
}

void TPCRequestManager::Done(const std::string &ident, TPCQueue *queue) {
    std::unique_lock<std::shared_mutex> lock(m_mutex);

    auto iter = m_pool_map.find(ident);
    if (iter != m_pool_map.end() && iter->second.get() == queue) {
        m_pool_map.erase(iter);
        lock.unlock();
        m_log.Log(LogMask::Info, "TPCRequestManager", "Worker pool", ident.c_str(), "is idle and all workers have exited.");
    }
}

bool TPCRequestManager::AcquireSlot() {
    if (m_shutdown.load(std::memory_order_acquire)) {
        return false;
    }
    unsigned max_workers = m_max_global_workers.load(std::memory_order_relaxed);
    unsigned live = m_live.load(std::memory_order_relaxed);
    while (live < max_workers) {
        if (m_live.compare_exchange_weak(live, live + 1, std::memory_order_acq_rel, std::memory_order_relaxed)) {
            return true;
        }
    }
    return false;
}

void TPCRequestManager::ReleaseSlot() {
    {
        std::lock_guard<std::mutex> lock(m_drain_mu);
        m_live.fetch_sub(1, std::memory_order_acq_rel);
    }
    m_drain_cv.notify_all();
    if (!m_shutdown.load(std::memory_order_acquire)) {
        DispatchStarved();
    }
}

void TPCRequestManager::NoteStarved(const std::shared_ptr<TPCQueue> &queue) {
    if (!queue || m_shutdown.load(std::memory_order_acquire)) {
        return;
    }
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    if (m_shutdown.load(std::memory_order_relaxed)) {
        return;
    }
    if (!queue->m_listed.exchange(true, std::memory_order_acq_rel)) {
        m_starved.push_back(queue);
        m_starved_count.fetch_add(1, std::memory_order_acq_rel);
    }
    ReapOneIdle();
}

void TPCRequestManager::ReapOneIdle() {
    for (auto &entry : m_pool_map) {
        std::unique_lock<std::mutex> lk(entry.second->m_mutex, std::try_to_lock);
        if (!lk || entry.second->m_idle.empty() || !entry.second->m_ops.empty()) {
            continue;
        }
        TPCQueue::TPCWorker *worker = entry.second->m_idle.front();
        worker->m_reap = true;
        worker->m_cv.notify_one();
        return;
    }
}

void TPCRequestManager::DispatchStarved() {
    std::shared_ptr<TPCQueue> queue;
    {
        std::unique_lock<std::shared_mutex> lock(m_mutex);
        if (m_shutdown.load(std::memory_order_relaxed)) {
            return;
        }
        while (!m_starved.empty() && !queue) {
            queue = m_starved.front().lock();
            m_starved.pop_front();
            m_starved_count.fetch_sub(1, std::memory_order_acq_rel);
            if (queue) {
                queue->m_listed.store(false, std::memory_order_release);
            }
        }
    }
    if (!queue || queue->IsDone()) {
        return;
    }
    if (!queue->TrySpawnForStarved()) {
        NoteStarved(queue);
    }
}

void TPCRequestManager::TPCRequest::ArmWake(void (*fn)(void *), void *arg) {
    std::lock_guard<std::mutex> lock(m_wake_mu);
    m_wake_fn = fn;
    m_wake_arg = arg;
}

void TPCRequestManager::TPCRequest::DisarmWake() {
    std::lock_guard<std::mutex> lock(m_wake_mu);
    m_wake_fn = nullptr;
    m_wake_arg = nullptr;
}

void TPCRequestManager::TPCRequest::Cancel() {
    m_cancelled.store(true, std::memory_order_release);
    void (*fn)(void *) = nullptr;
    void *arg = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_wake_mu);
        fn = m_wake_fn;
        arg = m_wake_arg;
    }
    if (fn && arg) {
        fn(arg);
    }
    if (auto queue = m_queue.lock()) {
        if (queue->Withdraw(*this)) {
            SetDone("Transfer cancelled");
        }
    }
}

bool TPCRequestManager::TPCRequest::IsCancelled() const { return m_cancelled.load(std::memory_order_acquire); }

CURL *TPCRequestManager::TPCRequest::GetHandle() const { return m_curl; }

std::string TPCRequestManager::TPCRequest::GetMessage() {
    std::unique_lock<std::mutex> lock(m_mutex);
    return m_message;
}

int TPCRequestManager::TPCRequest::GetCurlResult() {
    std::unique_lock<std::mutex> lock(m_mutex);
    return m_curl_result;
}

std::string TPCRequestManager::TPCRequest::GetLabel() const { return m_label; }

// Logic from State::GetConnectionDescription
void TPCRequestManager::TPCRequest::UpdateRemoteConnDesc() {
#if LIBCURL_VERSION_NUM >= 0x071500
    // Retrieve IP address and port from the curl handle
    const char *curl_ip = nullptr;
    CURLcode rc = curl_easy_getinfo(m_curl, CURLINFO_PRIMARY_IP, &curl_ip);
    if (rc != CURLE_OK || !curl_ip) {
        return;  // Failed to get IP, cannot update connection descriptor
    }

    long curl_port = 0;
    rc = curl_easy_getinfo(m_curl, CURLINFO_PRIMARY_PORT, &curl_port);
    if (rc != CURLE_OK || curl_port == 0) {
        return;  // Failed to get port, cannot update connection descriptor
    }

    // Format the connection string according to HTTP-TPC spec
    // IPv6 addresses must be enclosed in square brackets
    std::stringstream ss;
    if (strchr(curl_ip, ':') == nullptr) {
        ss << "tcp:" << curl_ip << ":" << curl_port;
    } else {
        ss << "tcp:[" << curl_ip << "]:" << curl_port;
    }

    {
        std::unique_lock<std::mutex> lock(m_conn_mutex);
        m_conn_list = ss.str();
    }
#else
    // For older libcurl versions, do nothing
    return;
#endif
}

std::string TPCRequestManager::TPCRequest::GetRemoteConnDesc() {
    std::unique_lock<std::mutex> lock(m_conn_mutex);
    return m_conn_list;
}

void TPCRequestManager::TPCRequest::SetDone(const std::string &msg, int curl_result) {
    std::unique_lock<std::mutex> lock(m_mutex);
    if (m_finished) {
        return;
    }
    m_curl_result = curl_result;
    m_message = msg;
    m_finished = true;
    m_cv.notify_one();
}

bool TPCRequestManager::TPCRequest::WaitFor(std::chrono::steady_clock::duration dur) {
    std::unique_lock<std::mutex> lock(m_mutex);
    return m_cv.wait_for(lock, dur, [&] { return m_finished; });
}

void TPCRequestManager::TPCRequest::WaitUntilFinished() {
    std::unique_lock<std::mutex> lock(m_mutex);
    m_cv.wait(lock, [&] { return m_finished; });
}

TPCRequestManager::TPCRequestManager(XrdOucEnv &xrdEnv, XrdSysError &eDest) : m_log(eDest), m_xrdEnv(xrdEnv) {
    // The pool is process-wide.  A previous manager in this process has
    // already waited for its workers, so a new one may accept transfers.
    if (m_live.load(std::memory_order_acquire) == 0) {
        m_shutdown.store(false, std::memory_order_release);
    }
}

TPCRequestManager::~TPCRequestManager() { Shutdown(); }

void TPCRequestManager::Shutdown() {
    m_shutdown.store(true, std::memory_order_release);
    std::vector<std::shared_ptr<TPCQueue>> queues;
    {
        std::unique_lock<std::shared_mutex> lock(m_mutex);
        queues.reserve(m_pool_map.size());
        for (auto &entry : m_pool_map) {
            queues.push_back(entry.second);
        }
        m_pool_map.clear();
        m_starved.clear();
        m_starved_count.store(0, std::memory_order_release);
    }
    for (auto &queue : queues) {
        queue->Stop();
    }
    std::unique_lock<std::mutex> lock(m_drain_mu);
    m_drain_cv.wait(lock, [&] { return m_live.load(std::memory_order_acquire) == 0; });
}

void TPCRequestManager::SetWorkerIdleTimeout(std::chrono::steady_clock::duration dur) { m_idle_timeout = dur; }

void TPCRequestManager::SetMaxGlobalWorkers(unsigned max_workers) {
    m_max_global_workers.store(max_workers, std::memory_order_relaxed);
}

std::shared_ptr<TPCRequestManager::TPCQueue> TPCRequestManager::GetQueue(const std::string &ident, bool create) {
    {
        std::shared_lock<std::shared_mutex> guard(m_mutex);
        if (m_shutdown.load(std::memory_order_acquire)) {
            return nullptr;
        }
        auto iter = m_pool_map.find(ident);
        if (iter != m_pool_map.end() && !iter->second->IsDone()) {
            return iter->second;
        }
    }
    if (!create) {
        return nullptr;
    }
    std::shared_ptr<TPCQueue> queue;
    {
        std::unique_lock<std::shared_mutex> guard(m_mutex);
        if (m_shutdown.load(std::memory_order_relaxed)) {
            return nullptr;
        }
        auto iter = m_pool_map.find(ident);
        if (iter != m_pool_map.end() && !iter->second->IsDone()) {
            return iter->second;
        }
        queue = std::make_shared<TPCQueue>(ident, *this);
        m_pool_map[ident] = queue;
    }
    m_log.Log(LogMask::Info, "RequestManager", "Created new TPC request queue for", ident.c_str());
    return queue;
}

// Send a request to a worker for processing.  If no worker is available, the
// request waits in its queue until one is.  If the queue is full, the request
// is rejected and false is returned.
bool TPCRequestManager::Produce(TPCRequestManager::TPCRequest &handler) {
    if (m_shutdown.load(std::memory_order_acquire)) {
        return false;
    }
    while (true) {
        auto queue = GetQueue(handler.GetLabel(), true);
        if (!queue) {
            handler.m_queue.reset();
            return false;
        }
        handler.m_queue = queue;
        if (queue->Produce(handler)) {
            return true;
        }
        if (!queue->IsDone()) {
            handler.m_queue.reset();
            return false;
        }
    }
}

bool TPCRequestManager::ProduceIfIdle(TPCRequestManager::TPCRequest &handler) {
    if (m_shutdown.load(std::memory_order_acquire)) {
        return false;
    }
    auto queue = GetQueue(handler.GetLabel(), false);
    if (!queue) {
        return false;
    }
    handler.m_queue = queue;
    if (queue->ProduceIfIdle(handler)) {
        return true;
    }
    handler.m_queue.reset();
    return false;
}
