#ifndef __XRDHTTPTPCPOOL_HH__
#define __XRDHTTPTPCPOOL_HH__

#include <curl/curl.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// Forward dec'ls
class XrdOucEnv;
class XrdSysError;
namespace XrdHttpTpc {
class PMarkManager;
}

// A pool manager for TPC requests
//
// The manager keeps one worker pool, selected by an opaque label supplied with
// each transfer.  The label is not interpreted here, so the policy for how
// transfers share a pool can change without changing the pool itself.
//
// Every worker owns a libcurl multi-handle whose connection cache outlives the
// transfer.  The most recently idle worker is woken first: it is the most
// likely to hold a live connection, and the others age out after
// m_idle_timeout.  An unused pool has no running threads.
namespace TPC {

class TPCRequestManager final {
    class TPCQueue;

   public:
    class TPCRequest {
       public:
        TPCRequest(const std::string &label, CURL *handle) : m_label(label), m_curl(handle) {}

        bool WaitFor(std::chrono::steady_clock::duration);
        void WaitUntilFinished();
        CURL *GetHandle() const;
        std::string GetLabel() const;
        std::string GetMessage();
        int GetCurlResult();
        std::string GetRemoteConnDesc();
        void SetDone(const std::string &msg, int curl_result = -1);
        // A request still waiting in its queue is withdrawn and finished at
        // once; a running one is stopped by its worker.
        void Cancel();
        bool IsCancelled() const;
        void UpdateRemoteConnDesc();
        void MarkStarted() { m_started.store(true, std::memory_order_release); }
        bool HasStarted() const { return m_started.load(std::memory_order_acquire); }
        // Valid only while the HTTP thread is blocked in WaitFor/WaitUntilFinished.
        void SetPMark(XrdHttpTpc::PMarkManager *pmark) { m_pmark = pmark; }
        XrdHttpTpc::PMarkManager *GetPMark() const { return m_pmark; }

       private:
        friend class TPCRequestManager;

        std::atomic<bool> m_cancelled{false};
        std::atomic<bool> m_started{false};
        bool m_finished{false};
        int m_curl_result{-1};
        std::string m_conn_list;
        std::mutex m_conn_mutex;
        std::atomic<off_t> m_progress_offset{0};
        // Label assigned to the request. Determines which queue it will be placed into.
        // A queue with matching identifier is created if it does not already exists.
        std::string m_label;
        CURL *m_curl;
        std::condition_variable m_cv;
        std::mutex m_mutex;
        std::string m_message;
        XrdHttpTpc::PMarkManager *m_pmark{nullptr};
        std::weak_ptr<TPCQueue> m_queue;
    };

    TPCRequestManager(XrdOucEnv &xrdEnv, XrdSysError &eDest);

    // Queue the request.  Returns false if its queue is full.
    bool Produce(TPCRequest &handler);
    // Hand the request to an idle worker of its queue.  Returns false, without
    // queueing the request, if the queue has no idle worker.
    bool ProduceIfIdle(TPCRequest &handler);

    void SetWorkerIdleTimeout(std::chrono::steady_clock::duration dur);
    void SetMaxWorkers(unsigned max_workers) { m_max_workers = max_workers; }
    void SetMaxIdleRequests(unsigned max_pending_ops) { m_max_pending_ops = max_pending_ops; }

   private:
    class TPCQueue : public std::enable_shared_from_this<TPCQueue> {
        class TPCWorker;

       public:
        TPCQueue(const std::string &identifier, TPCRequestManager &parent) : m_identifier(identifier), m_parent(parent) {}

        // Returns false if the queue is full or has shut down; IsDone() tells which.
        bool Produce(TPCRequest &handler);
        bool ProduceIfIdle(TPCRequest &handler);
        bool Withdraw(TPCRequest &handler);
        bool IsDone() const { return m_done.load(std::memory_order_acquire); }

       private:
        class TPCWorker final {
           public:
            explicit TPCWorker(TPCQueue &queue) : m_queue(queue) {}
            TPCWorker(const TPCWorker &) = delete;

            void Run();
            static void RunStatic(std::shared_ptr<TPCQueue> queue, std::unique_ptr<TPCWorker> myself);

            // Guarded by the queue's m_mutex.  Set when the worker is handed a
            // request (or spawned for one) that it has not yet taken.
            bool m_claimed{false};
            std::condition_variable m_cv;

           private:
            bool RunCurl(CURLM *multi_handle, TPCRequest &request);
            static int closesocket_callback(void *clientp, curl_socket_t fd);

            // Set only while RunCurl drives a transfer; cleared before SetDone.
            XrdHttpTpc::PMarkManager *m_active_pmark{nullptr};
            TPCQueue &m_queue;
        };

        // Return the next request for the worker, waiting while it is idle.
        // Returns nullptr once the worker must exit; it has then left the queue.
        TPCRequest *Consume(TPCWorker &worker, bool can_run);
        // The helpers below require m_mutex.
        bool ClaimIdleWorker();
        bool SpawnWorker();

        std::atomic<bool> m_done{false};
        // Opaque label supplied with the transfer.  The pool does not interpret it.
        const std::string m_identifier;
        std::vector<TPCWorker *> m_workers;
        // Idle workers, the most recently idle at the back.
        std::vector<TPCWorker *> m_idle;
        std::deque<TPCRequest *> m_ops;
        // Workers that have been handed a request but not yet taken it.
        size_t m_claims{0};
        std::mutex m_mutex;
        TPCRequestManager &m_parent;
    };

    std::shared_ptr<TPCQueue> GetQueue(const std::string &ident, bool create);
    void Done(const std::string &ident, TPCQueue *queue);

    static std::shared_mutex m_mutex;
    XrdSysError &m_log;  // Log object for the request manager
    static std::chrono::steady_clock::duration m_idle_timeout;
    static std::unordered_map<std::string, std::shared_ptr<TPCQueue>> m_pool_map;
    static unsigned m_max_pending_ops;
    static unsigned m_max_workers;
    XrdOucEnv &m_xrdEnv;
};

}  // namespace TPC

#endif  // __XRDHTTPTPCPOOL_HH__
