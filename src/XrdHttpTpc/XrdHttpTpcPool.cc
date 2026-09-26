#include "XrdHttpTpcPool.hh"

#include <XrdOuc/XrdOucEnv.hh>
#include <XrdSys/XrdSysError.hh>
#include <algorithm>
#include <sstream>
#include <string>
#include <thread>

#include "XrdHttpTpcTPC.hh"

using namespace TPC;

decltype(TPCRequestManager::m_pool_map) TPCRequestManager::m_pool_map;
decltype(TPCRequestManager::m_init_once) TPCRequestManager::m_init_once;
decltype(TPCRequestManager::m_mutex) TPCRequestManager::m_mutex;
decltype(TPCRequestManager::m_idle_timeout) TPCRequestManager::m_idle_timeout = std::chrono::minutes(1);
unsigned TPCRequestManager::m_max_pending_ops = 20;  // default max_pending_transfers_per_queue
unsigned TPCRequestManager::m_max_workers = 20;

TPCRequestManager::TPCQueue::TPCWorker::TPCWorker(const std::string &label, TPCQueue &queue)
    : m_label(label), m_queue(queue) {}

void TPCRequestManager::TPCQueue::TPCWorker::RunStatic(std::shared_ptr<TPCQueue> queue, TPCWorker *myself) {
    myself->Run();
    queue->Done(myself);
}

bool TPCRequestManager::TPCQueue::TPCWorker::RunCurl(CURLM *multi_handle, TPCRequestManager::TPCRequest &request) {
    if (request.IsCancelled()) {
        request.SetDone("Transfer cancelled");
        return true;
    }

    CURLMcode mres;
    auto curl = request.GetHandle();

    mres = curl_multi_add_handle(multi_handle, curl);
    if (mres) {
        std::stringstream ss;
        ss << "Failed to add transfer to libcurl multi-handle: HTTP library "
              "failure="
           << curl_multi_strerror(mres);
        m_queue.m_parent.m_log.Log(LogMask::Error, "TPCWorker", ss.str().c_str());
        request.SetDone(ss.str());
        return true;
    }

    auto fail = [&](const std::string &msg, LogMask lvl) {
        curl_multi_remove_handle(multi_handle, curl);
        m_queue.m_parent.m_log.Log(lvl, "TPCWorker", msg.c_str());
        request.SetDone(msg);
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
    request.SetDone("Transfer complete", res);
    return true;
}

void TPCRequestManager::TPCQueue::TPCWorker::Run() {
    m_queue.m_parent.m_log.Log(LogMask::Info, "TPCWorker", "Worker for", m_queue.m_identifier.c_str(), "starting");

    // Create the multi-handle and add in the current transfer to it.
    CURLM *multi_handle = curl_multi_init();
    if (!multi_handle) {
        m_queue.m_parent.m_log.Log(LogMask::Error, "TPCWorker",
                                   "Unable to create"
                                   " a libcurl multi-handle; fatal error for worker");
        return;
    }

    while (true) {
        auto request = m_queue.TryConsume();
        if (!request) {
            request = m_queue.ConsumeUntil(m_idle_timeout, this);
            if (!request) {
                m_queue.m_parent.m_log.Log(LogMask::Info, "TPCWorker", "Worker for", m_queue.m_identifier.c_str(), "exiting");
                break;
            }
        }
        RunCurl(multi_handle, *request);
    }
    curl_multi_cleanup(multi_handle);
}

void TPCRequestManager::TPCQueue::Done(TPCWorker *worker) {
    std::unique_lock<std::mutex> lock(m_mutex);
    auto it = std::remove_if(m_workers.begin(), m_workers.end(), [&](std::unique_ptr<TPCWorker> &other) { return other.get() == worker; });
    m_workers.erase(it, m_workers.end());

    if (!m_workers.empty()) {
        return;
    }
    m_done.store(true, std::memory_order_release);
    for (TPCRequest *op : m_ops) {
        op->SetDone("TPC worker pool shut down before the transfer started");
    }
    m_ops.clear();
    std::string ident = m_identifier;
    lock.unlock();
    m_parent.Done(ident);
}

void TPCRequestManager::Done(const std::string &ident) {
    m_log.Log(LogMask::Info, "TPCRequestManager", "Worker pool", ident.c_str(), "is idle and all workers have exited.");
    std::unique_lock<std::shared_mutex> lock(m_mutex);

    auto iter = m_pool_map.find(ident);
    if (iter != m_pool_map.end()) {
        m_pool_map.erase(iter);
    }
}

// Produce a request for processing.  If the queue is full, the request will
// be rejected and false will be returned.
//
// Implementation notes:
// - If a worker is idle, it will be woken up to process the request.
// - If no workers are idle, a new worker will be created to process the
//   request.
// - If the maximum number of workers is reached, the request will be queued
//   until a worker is available.
// - If the maximum number of pending operations is reached, the request will
//   be rejected.
// - If there are multiple idle workers, the oldest worker will be woken.  This
//   causes the newest workers to be idle for as long as possible and
//   potentially exit due to lack of work.  This is done to reduce the number of
//   "mostly idle" workers in the thread pool.
bool TPCRequestManager::TPCQueue::Produce(TPCRequest &handler) {
    std::unique_lock<std::mutex> lk(m_mutex);
    if (m_done.load(std::memory_order_acquire)) {
        return false;
    }
    if (m_ops.size() == m_max_pending_ops) {
        m_parent.m_log.Log(LogMask::Warning, "TPCQueue", "Queue is full; rejecting request");
        return false;
    }

    m_ops.push_back(&handler);
    for (auto &worker : m_workers) {
        if (worker->IsIdle()) {
            worker->m_cv.notify_one();
            return true;
        }
    }

    if (m_workers.size() < m_max_workers) {
        auto worker = std::make_unique<TPCRequestManager::TPCQueue::TPCWorker>(handler.GetLabel(), *this);
        auto self = shared_from_this();
        std::thread t(TPCRequestManager::TPCQueue::TPCWorker::RunStatic, self, worker.get());
        t.detach();
        m_workers.push_back(std::move(worker));
    }
    lk.unlock();

    return true;
}

TPCRequestManager::TPCRequest *TPCRequestManager::TPCQueue::TryConsume() {
    std::unique_lock<std::mutex> lk(m_mutex);
    if (m_ops.size() == 0) {
        return nullptr;
    }

    auto result = m_ops.front();
    m_ops.pop_front();

    return result;
}

// Wait for a request to be available for processing, or until the duration
// has elapsed.
//
// Returns the request that is available, or nullptr if the duration has
// elapsed.
TPCRequestManager::TPCRequest *TPCRequestManager::TPCQueue::ConsumeUntil(std::chrono::steady_clock::duration dur, TPCWorker *worker) {
    std::unique_lock<std::mutex> lk(m_mutex);
    worker->SetIdle(true);
    worker->m_cv.wait_for(lk, dur, [&] { return m_ops.size() > 0; });
    worker->SetIdle(false);
    if (m_ops.size() == 0) {
        return nullptr;
    }

    auto result = m_ops.front();
    m_ops.pop_front();

    return result;
}

void TPCRequestManager::TPCRequest::Cancel() { m_cancelled.store(true, std::memory_order_relaxed); }

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

// Send a request to a worker for processing.  If the worker is not available,
// the request will be queued until a worker is available.  If the queue is
// full, the request will be rejected and false will be returned.
bool TPCRequestManager::Produce(TPCRequestManager::TPCRequest &handler) {
    for (;;) {
        std::shared_ptr<TPCQueue> queue;
        {
            std::shared_lock<std::shared_mutex> guard(m_mutex);
            auto iter = m_pool_map.find(handler.GetLabel());
            if (iter != m_pool_map.end() && !iter->second->IsDone()) {
                queue = iter->second;
            }
        }
        if (!queue) {
            bool created_queue = false;
            std::string queue_name;
            {
                std::unique_lock<std::shared_mutex> guard(m_mutex);
                auto iter = m_pool_map.find(handler.GetLabel());
                if (iter == m_pool_map.end() || iter->second->IsDone()) {
                    queue = std::make_shared<TPCQueue>(handler.GetLabel(), *this);
                    if (iter != m_pool_map.end()) {
                        m_pool_map.erase(iter);
                    }
                    m_pool_map.emplace(handler.GetLabel(), queue);
                    created_queue = true;
                    queue_name = handler.GetLabel();
                } else {
                    queue = iter->second;
                }
            }
            if (created_queue) {
                m_log.Log(LogMask::Info, "RequestManager", "Created new TPC request queue for", queue_name.c_str());
            }
        }
        if (queue->Produce(handler)) {
            return true;
        }
        if (!queue->IsDone()) {
            return false;
        }
    }
}
