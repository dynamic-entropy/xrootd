#include "XrdHttpTpcPool.hh"

#include <XrdOuc/XrdOucEnv.hh>
#include <XrdSys/XrdSysError.hh>
#include <algorithm>
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

void TPCRequestManager::TPCQueue::TPCWorker::RunStatic(std::shared_ptr<TPCQueue> queue, std::unique_ptr<TPCWorker> myself) {
    myself->Run();
    myself.reset();
    if (queue->IsDone()) {
        queue->m_parent.Done(queue->m_identifier, queue.get());
    }
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

bool TPCRequestManager::TPCQueue::TPCWorker::RunCurl(CURLM *multi_handle, TPCRequestManager::TPCRequest &request) {
    request.MarkStarted();
    if (request.IsCancelled()) {
        request.SetDone("Transfer cancelled");
        return true;
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

    auto finish = [&](const std::string &msg, int curl_result = -1) {
        // Emit this transfer's firefly while its socket is still open.  libcurl
        // keeps the connection in this worker's cache for the next request.
        if (m_active_pmark) {
            m_active_pmark->endTransferMarks();
        }
        m_active_pmark = nullptr;
        request.SetDone(msg, curl_result);
    };

    mres = curl_multi_add_handle(multi_handle, curl);
    if (mres) {
        std::stringstream ss;
        ss << "Failed to add transfer to libcurl multi-handle: HTTP library "
              "failure="
           << curl_multi_strerror(mres);
        m_queue.m_parent.m_log.Log(LogMask::Error, "TPCWorker", ss.str().c_str());
        finish(ss.str());
        return true;
    }

    auto fail = [&](const std::string &msg, LogMask lvl) {
        curl_multi_remove_handle(multi_handle, curl);
        m_queue.m_parent.m_log.Log(lvl, "TPCWorker", msg.c_str());
        finish(msg);
    };

    CURLcode res = static_cast<CURLcode>(-1);
    int running_handles = 1;
    const int update_interval{1};
    time_t now = time(NULL);
    time_t last_update = now - update_interval;

    while (running_handles) {
        mres = curl_multi_perform(multi_handle, &running_handles);
        if (mres != CURLM_OK) {
            fail("Internal curl multi-handle error: " + std::string(curl_multi_strerror(mres)), LogMask::Error);
            return true;
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

        if (request.IsCancelled()) {
            fail("Transfer cancelled", LogMask::Info);
            return true;
        }
        if (running_handles == 0) {
            break;
        }

        mres = curl_multi_wait(multi_handle, NULL, 0, 1000, nullptr);
        if (mres != CURLM_OK) {
            fail("Error during curl_multi_wait: " + std::string(curl_multi_strerror(mres)), LogMask::Error);
            return true;
        }
    }

    request.UpdateRemoteConnDesc();

    if (res == static_cast<CURLcode>(-1)) {
        fail("Internal state error in libcurl - no transfer results returned", LogMask::Error);
        return true;
    }

    curl_multi_remove_handle(multi_handle, curl);
    finish("Transfer complete", res);
    return true;
}

void TPCRequestManager::TPCQueue::TPCWorker::Run() {
    auto &log = m_queue.m_parent.m_log;
    log.Log(LogMask::Info, "TPCWorker", "Worker for", m_queue.m_identifier.c_str(), "starting");

    // The multi-handle's connection cache is what lets later transfers in this
    // queue reuse the connections opened by earlier ones.
    CURLM *multi_handle = curl_multi_init();
    if (!multi_handle) {
        log.Log(LogMask::Error, "TPCWorker",
                "Unable to create"
                " a libcurl multi-handle; fatal error for worker");
    } else {
        // A pull through a redirector opens a second connection. Keep only
        // the one in use, so the redirector socket does not stay idle for
        // the whole transfer. The data connection remains for the next copy.
        curl_multi_setopt(multi_handle, CURLMOPT_MAXCONNECTS, 1L);
    }

    while (auto request = m_queue.Consume(*this, multi_handle != nullptr)) {
        RunCurl(multi_handle, *request);
    }
    if (multi_handle) {
        curl_multi_cleanup(multi_handle);
    }
    log.Log(LogMask::Info, "TPCWorker", "Worker for", m_queue.m_identifier.c_str(), "exiting");
}

TPCRequestManager::TPCRequest *TPCRequestManager::TPCQueue::Consume(TPCWorker &worker, bool can_run) {
    std::unique_lock<std::mutex> lk(m_mutex);
    if (worker.m_claimed) {
        worker.m_claimed = false;
        m_claims--;
    }

    if (can_run) {
        const auto deadline = std::chrono::steady_clock::now() + m_idle_timeout;
        while (true) {
            if (!m_ops.empty()) {
                auto result = m_ops.front();
                m_ops.pop_front();
                return result;
            }

            m_idle.push_back(&worker);
            bool woken = worker.m_cv.wait_until(lk, deadline, [&] { return worker.m_claimed; });
            auto it = std::find(m_idle.begin(), m_idle.end(), &worker);
            if (it != m_idle.end()) {
                m_idle.erase(it);
            }
            if (worker.m_claimed) {
                // The request may already have been taken by a worker that just
                // finished; the loop then waits again until the same deadline.
                worker.m_claimed = false;
                m_claims--;
                continue;
            }
            if (!woken) {
                break;
            }
        }
    }

    m_workers.erase(std::remove(m_workers.begin(), m_workers.end(), &worker), m_workers.end());
    if (!m_workers.empty()) {
        return nullptr;
    }
    if (!can_run) {
        for (TPCRequest *op : m_ops) {
            op->SetDone("TPC worker could not start");
        }
        m_ops.clear();
    }
    m_done.store(true, std::memory_order_release);
    return nullptr;
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
        m_claims--;
        m_parent.m_log.Log(LogMask::Error, "TPCQueue", "Unable to start a TPC worker thread:", e.what());
        return false;
    }
    return true;
}

// Queue a request for processing.  If the queue is full, the request will
// be rejected and false will be returned.
//
// Implementation notes:
// - If a worker is idle, the most recently idle one is handed the request.
// - Otherwise, a new worker is created unless the maximum number of workers
//   is reached, in which case the request waits for a worker to finish.
// - If the maximum number of pending operations is reached, the request will
//   be rejected.
bool TPCRequestManager::TPCQueue::Produce(TPCRequest &handler) {
    std::unique_lock<std::mutex> lk(m_mutex);
    if (IsDone()) {
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
    if (!SpawnWorker() && m_workers.empty()) {
        m_ops.pop_back();
        return false;
    }
    return true;
}

bool TPCRequestManager::TPCQueue::ProduceIfIdle(TPCRequest &handler) {
    std::unique_lock<std::mutex> lk(m_mutex);
    if (IsDone() || m_idle.empty()) {
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

void TPCRequestManager::Done(const std::string &ident, TPCQueue *queue) {
    std::unique_lock<std::shared_mutex> lock(m_mutex);

    auto iter = m_pool_map.find(ident);
    if (iter != m_pool_map.end() && iter->second.get() == queue) {
        m_pool_map.erase(iter);
        lock.unlock();
        m_log.Log(LogMask::Info, "TPCRequestManager", "Worker pool", ident.c_str(), "is idle and all workers have exited.");
    }
}

void TPCRequestManager::TPCRequest::Cancel() {
    m_cancelled.store(true, std::memory_order_relaxed);
    if (auto queue = m_queue.lock()) {
        if (queue->Withdraw(*this)) {
            SetDone("Transfer cancelled");
        }
    }
}

bool TPCRequestManager::TPCRequest::IsCancelled() const { return m_cancelled.load(std::memory_order_relaxed); }

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

TPCRequestManager::TPCRequestManager(XrdOucEnv &xrdEnv, XrdSysError &eDest) : m_log(eDest), m_xrdEnv(xrdEnv) {}

void TPCRequestManager::SetWorkerIdleTimeout(std::chrono::steady_clock::duration dur) { m_idle_timeout = dur; }

std::shared_ptr<TPCRequestManager::TPCQueue> TPCRequestManager::GetQueue(const std::string &ident, bool create) {
    {
        std::shared_lock<std::shared_mutex> guard(m_mutex);
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
    while (true) {
        auto queue = GetQueue(handler.GetLabel(), true);
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
