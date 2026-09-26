#ifndef __XRDHTTPTPCPOOL_HH__
#define __XRDHTTPTPCPOOL_HH__

#include <curl/curl.h>

#include <atomic>
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

// A pool manager for TPC requests
//
// The manager keeps one worker pool, selected by an opaque label supplied with
// each transfer.  The label is not interpreted here, so the policy for how
// transfers share a pool can change without changing the pool itself.
//
// Transfers are packed onto as few workers as possible so libcurl can reuse TCP
// connections.  Idle workers shut down; an unused pool has no running threads.
namespace TPC {

class TPCRequestManager final {
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
        void Cancel();
        bool IsCancelled() const;
        void UpdateRemoteConnDesc();

       private:
        std::atomic<bool> m_cancelled{false};
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
    };

    TPCRequestManager(XrdOucEnv &xrdEnv, XrdSysError &eDest);

    bool Produce(TPCRequest &handler);

    void SetWorkerIdleTimeout(std::chrono::steady_clock::duration dur);
    void SetMaxWorkers(unsigned max_workers) { m_max_workers = max_workers; }
    void SetMaxIdleRequests(unsigned max_pending_ops) { m_max_pending_ops = max_pending_ops; }

   private:
    class TPCQueue : public std::enable_shared_from_this<TPCQueue> {
        class TPCWorker;

       public:
        TPCQueue(const std::string &identifier, TPCRequestManager &parent) : m_identifier(identifier), m_parent(parent) {}

        bool Produce(TPCRequest &handler);
        TPCRequest *TryConsume();
        TPCRequest *ConsumeUntil(std::chrono::steady_clock::duration dur, TPCWorker *worker);
        void Done(TPCWorker *);
        bool IsDone() const { return m_done.load(std::memory_order_acquire); }

       private:
        class TPCWorker final {
           public:
            TPCWorker(const std::string &label, TPCQueue &queue);
            TPCWorker(const TPCWorker &) = delete;

            void Run();
            static void RunStatic(std::shared_ptr<TPCQueue> queue, TPCWorker *myself);

            bool IsIdle() const { return m_idle; }
            void SetIdle(bool idle) { m_idle = idle; }
            std::condition_variable m_cv;

            std::string getLabel() const { return m_label; }

           private:
            bool RunCurl(CURLM *multi_handle, TPCRequest &request);

            bool m_idle{false};
            // Label for this worker. Always set to the m_identifier of the queue it serves.
            const std::string m_label;
            TPCQueue &m_queue;
        };

        std::atomic<bool> m_done{false};
        // Opaque label supplied with the transfer.  The pool does not interpret it.
        const std::string m_identifier;
        std::vector<std::unique_ptr<TPCWorker>> m_workers;
        std::deque<TPCRequest *> m_ops;
        std::mutex m_mutex;
        TPCRequestManager &m_parent;
    };

    void Done(const std::string &ident);

    static std::shared_mutex m_mutex;
    XrdSysError &m_log;  // Log object for the request manager
    static std::chrono::steady_clock::duration m_idle_timeout;
    static std::unordered_map<std::string, std::shared_ptr<TPCQueue>> m_pool_map;
    static unsigned m_max_pending_ops;
    static unsigned m_max_workers;
    static std::once_flag m_init_once;
    XrdOucEnv &m_xrdEnv;
};

}  // namespace TPC

#endif  // __XRDHTTPTPCPOOL_HH__
