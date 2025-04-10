
#include "XrdTpcPool.hh"
#include "XrdTpcTPC.hh"

#include <XrdOuc/XrdOucEnv.hh>
#include <XrdSys/XrdSysError.hh>

#include <string>
#include <thread>

#include <fcntl.h>

using namespace TPC;

decltype(TPCRequestManager::m_pool_map) TPCRequestManager::m_pool_map;
decltype(TPCRequestManager::m_init_once)
TPCRequestManager::m_init_once;
decltype(TPCRequestManager::m_mutex) TPCRequestManager::m_mutex;
decltype(TPCRequestManager::m_idle_timeout)
TPCRequestManager::m_idle_timeout = std::chrono::minutes(1);
unsigned TPCRequestManager::m_max_pending_ops = 20;
unsigned TPCRequestManager::m_max_workers = 20;

TPCRequestManager::TPCQueue::TPCWorker::TPCWorker(
    const std::string &label, TPCQueue &queue)
    : m_label(label), m_queue(queue) {}

void TPCRequestManager::TPCQueue::TPCWorker::RunStatic(
    TPCWorker *myself) {
    myself->Run();
}

bool TPCRequestManager::TPCQueue::TPCWorker::RunCurl(
    CURLM *multi_handle, TPCRequestManager::TPCRequest &request) {

    CURLMcode mres;
    mres = curl_multi_add_handle(multi_handle, request.GetHandle());
    if (mres) {
        std::stringstream ss;
        ss << "Failed to add transfer to libcurl multi-handle: HTTP library failure=" << curl_multi_strerror(mres);
        m_queue.m_parent.m_log.Log(LogMask::Error, "TPCWorker", ss.str().c_str());
        request.SetDone(500, ss.str());
        return false;
    }
    request.SetProgress(0);

    CURLcode res = static_cast<CURLcode>(-1);
    int running_handles = 1;
    while (true) {
        mres = curl_multi_perform(multi_handle, &running_handles);
        if (mres != CURLM_OK) {
            std::stringstream ss;
            ss << "Internal curl multi-handle error: " << curl_multi_strerror(mres);
            m_queue.m_parent.m_log.Log(LogMask::Error, "TPCWorker", ss.str().c_str());
            request.SetDone(500, ss.str());
            return false;
        }

        CURLMsg *msg;
        do {
            int msgq = 0;
            msg = curl_multi_info_read(multi_handle, &msgq);
            if (msg && (msg->msg == CURLMSG_DONE)) {
                CURL *easy_handle = msg->easy_handle;
                res = msg->data.result;
                curl_multi_remove_handle(multi_handle, easy_handle);
            }
        } while (msg);

        mres = curl_multi_wait(multi_handle, NULL, 0, 1000, nullptr);
        if (mres != CURLM_OK) {
            break;
        }
    } while (running_handles);

    if (res == static_cast<CURLcode>(-1)) { // No transfers returned?!?
        curl_multi_remove_handle(multi_handle, request.GetHandle());
        std::stringstream ss;
        ss << "Internal state error in libcurl - no transfer results returned";
        m_queue.m_parent.m_log.Log(LogMask::Error, "TPCWorker", ss.str().c_str());
        request.SetDone(500, ss.str());
        return false;
    }

    request.SetDone(res, "Transfer complete");
    curl_multi_remove_handle(multi_handle, request.GetHandle());
    return true;
}

void TPCRequestManager::TPCQueue::TPCWorker::Run() {
    m_queue.m_parent.m_log.Log(LogMask::Info, "TPCWorker", "Worker for",
                               m_queue.m_label.c_str(), "starting");


    // Create the multi-handle and add in the current transfer to it.
    CURLM *multi_handle = curl_multi_init();
    if (!multi_handle) {
        m_queue.m_parent.m_log.Log(LogMask::Error, "TPCWorker", "Unable to create"
            " a libcurl multi-handle; fatal error for worker");
        m_queue.Done(this);
    }

    while (true) {
        auto request = m_queue.TryConsume();
        if (!request) {
            request = m_queue.ConsumeUntil(m_idle_timeout, this);
            if (!request) {
                break;
            }
        }
        if (!RunCurl(multi_handle, *request)) {
            m_queue.m_parent.m_log.Log(LogMask::Error, "TPCWorker", "Worker's multi-handle"
                " caused an internal error.  Worker immediately exiting");
            m_queue.Done(this);
        }
    }

    m_queue.m_parent.m_log.Log(LogMask::Info, "TPCWorker", "Worker for",
                               m_queue.m_label.c_str(), "exiting");
    m_queue.Done(this);
}

void TPCRequestManager::TPCQueue::Done(TPCWorker *worker) {
    std::unique_lock lock(m_mutex);
    m_done = true;
    auto it = std::remove_if(m_workers.begin(), m_workers.end(), [&](std::unique_ptr<TPCWorker> &other) {
        return other.get() == worker;
    });
    m_workers.erase(it, m_workers.end());

    if (m_workers.empty()) {
        lock.unlock();
        m_parent.Done(m_label);
    }
}

void TPCRequestManager::Done(const std::string &ident) {
    m_log.Log(LogMask::Info, "TPCRequestManager", "Worker pool",
              ident.c_str(), "is idle and all workers have exited.");
    std::unique_lock lock(m_mutex);

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
    std::unique_lock lk{m_mutex};
    if (m_ops.size() == m_max_pending_ops) {
        m_parent.m_log.Log(LogMask::Warning, "TPCQueue",
                           "Queue is full; rejecting request");
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
        auto worker = std::make_unique<
            TPCRequestManager::TPCQueue::TPCWorker>(
            handler.GetIdentifier(), *this);
        std::thread t(
            TPCRequestManager::TPCQueue::TPCWorker::RunStatic,
            worker.get());
        t.detach();
        m_workers.push_back(std::move(worker));
    }
    lk.unlock();

    return true;
}

TPCRequestManager::TPCRequest *
TPCRequestManager::TPCQueue::TryConsume() {
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
TPCRequestManager::TPCRequest *
TPCRequestManager::TPCQueue::ConsumeUntil(
    std::chrono::steady_clock::duration dur, TPCWorker *worker) {
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

void TPCRequestManager::TPCRequest::SetProgress(off_t offset) {
    if (offset == 0) {
        m_active.store(true, std::memory_order_relaxed);
    }
    m_progress_offset.store(offset, std::memory_order_relaxed);
}

void TPCRequestManager::TPCRequest::SetDone(int status,
                                                      const std::string &msg) {
    std::unique_lock lock(m_mutex);
    m_status = status;
    m_message = msg;
    m_cv.notify_one();
}

int TPCRequestManager::TPCRequest::WaitFor(
    std::chrono::steady_clock::duration dur) {
    std::unique_lock lock(m_mutex);
    m_cv.wait_for(lock, dur, [&] { return m_status >= 0; });

    return m_status;
}

TPCRequestManager::TPCRequestManager(XrdOucEnv &xrdEnv,
                                               XrdSysError &eDest)
    : m_log(eDest) {
}

void TPCRequestManager::SetWorkerIdleTimeout(
    std::chrono::steady_clock::duration dur) {
    m_idle_timeout = dur;
}

// Send a request to a worker for processing.  If the worker is not available,
// the request will be queued until a worker is available.  If the queue is
// full, the request will be rejected and false will be returned.
bool TPCRequestManager::Produce(
    TPCRequestManager::TPCRequest &handler) {

    std::shared_ptr<TPCQueue> queue;
    // Get the queue from our per-label map.  To avoid a race condition,
    // if the queue we get has already been shut down, we release the lock
    // and try again (with the expectation that the queue will eventually
    // get the lock and remove itself from the map).
    while (true) {
        m_mutex.lock_shared();
        std::lock_guard guard{m_mutex, std::adopt_lock};
        auto iter = m_pool_map.find(handler.GetIdentifier());
        if (iter != m_pool_map.end()) {
            queue = iter->second;
            if (!queue->IsDone())
                break;
        } else {
            break;
        }
    }
    if (!queue) {
        auto created_queue = false;
        std::string queue_name = "";
        {
            std::lock_guard guard(m_mutex);
            auto iter = m_pool_map.find(handler.GetIdentifier());
            if (iter == m_pool_map.end()) {
                queue = std::make_shared<TPCQueue>(handler.GetIdentifier(),
                                                        *this);
                m_pool_map.insert(iter, {handler.GetIdentifier(), queue});
                created_queue = true;
                queue_name = handler.GetIdentifier();
            } else {
                queue = iter->second;
            }
        }
        if (created_queue) {
            m_log.Log(LogMask::Info, "RequestManager",
                      "Created new TPC request queue for", queue_name.c_str());
        }
    }
    return queue->Produce(handler);
}
