#ifndef __XRDTPCPOOL_HH__
#define __XRDTPCPOOL_HH__

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <curl/curl.h>
#include "XrdTpcPMarkManager.hh"
// Forward dec'ls
class XrdOucEnv;
class XrdSysError;

// A pool manager for TPC requests
//
// The manager maintains a set of worker pools, one for each distinct identifier
// (typically, one per organization; this prevents the mixing of transfers from
// different organizations on the same TCP socket).  Each TPC transfer submitted
// must have an identifier; the transfer is then queued for the appropriate pool
// and subsequently executed by one of the worker threads.
//
// Transfers are packed to as few workers as possible in an attempt to reduce the
// number of TCP connections; however, if the transfer is not picked up quickly,
// a new worker will be spawned.  Idle workers will auto-shutdown; if not used,
// the pool will have no running threads.
namespace TPC {

class TPCRequestManager final {
  public:

    class TPCRequest {
      public:
        TPCRequest(const std::string &ident, CURL *handle)
            : m_ident(ident), m_curl(handle) {}

        int WaitFor(std::chrono::steady_clock::duration);

        CURL *GetHandle() const { return m_curl; }
        void SetProgress(off_t offset);
        void SetDone(int status, const std::string &msg);
        const std::string &GetIdentifier() const { return m_ident; }
        bool IsActive() const {
            return m_active.load(std::memory_order_relaxed);
        }
        void Cancel() { // TODO: implement.
        }
        std::string GetResults() const { return m_message; }
        off_t GetProgress() const {
            return m_progress_offset.load(std::memory_order_relaxed);
        }

      private:
        std::atomic<bool> m_active{false}; // Set to true when the request is active
        int m_status{-1}; // Set to the status code of the request
        std::atomic<off_t> m_progress_offset{0}; // Set to the current progress offset
        std::string m_ident; // The identifier for the request
        CURL *m_curl; // The CURL handle for the request
        std::condition_variable m_cv; // Condition variable for waiting on the request
        std::mutex m_mutex; // Mutex for synchronizing access to the request
        std::string m_message; // The message associated with the request
    };

    TPCRequestManager(XrdOucEnv &xrdEnv, XrdSysError &eDest);

    bool Produce(TPCRequest &handler);

    void SetWorkerIdleTimeout(std::chrono::steady_clock::duration dur);
    void SetMaxWorkers(unsigned max_workers) { m_max_workers = max_workers; }
    void SetMaxIdleRequests(unsigned max_pending_ops) {
        m_max_pending_ops = max_pending_ops;
    }

  private:
    class TPCQueue {
        class TPCWorker;

      public:
        TPCQueue(const std::string &ident, TPCRequestManager &parent)
            : m_label(ident), m_parent(parent) {}

        bool Produce(TPCRequest &handler);
        TPCRequest *TryConsume();
        TPCRequest *ConsumeUntil(std::chrono::steady_clock::duration dur,
                                      TPCWorker *worker);
        void Done(TPCWorker *);
        bool IsDone() const { return m_done; }

      private:
        class TPCWorker final {
          public:
            TPCWorker(const std::string &label, TPCQueue &queue);
            TPCWorker(const TPCWorker &) = delete;

            void Run();
            static void RunStatic(TPCWorker *myself);

            bool IsIdle() const { return m_idle; }
            void SetIdle(bool idle) { m_idle = idle; }
            std::condition_variable m_cv;
    
            static int closesocket_callback(void *clientp, curl_socket_t fd);
            static int opensocket_callback(void *clientp, curlsocktype purpose, struct curl_sockaddr *address);

          private:
            bool RunCurl(CURLM *multi_handle, TPCRequest &request);

            bool m_idle{false}; // State when worker has no requests to process but is alive
            const std::string m_label; // Label for the worker
            TPCQueue &m_queue; // Reference to the queue this worker is processing
            XrdTpc::PMarkManager pmarkManager;
        };

        static const long CONNECT_TIMEOUT = 60;
        bool m_done{false};
        const std::string m_label;
        std::vector<std::unique_ptr<TPCWorker>> m_workers; // List of workers
        std::deque<TPCRequest *> m_ops; // Queue of requests to be processed
        std::mutex m_mutex; // Mutex for the queue
        TPCRequestManager &m_parent; // Parent request manager
    };

    void Done(const std::string &ident);

    static std::shared_mutex m_mutex;

    XrdSysError &m_log; // Log object for the request manager

    static std::chrono::steady_clock::duration m_idle_timeout;
    static std::unordered_map<std::string, std::shared_ptr<TPCQueue>>
        m_pool_map;
    static unsigned m_max_pending_ops;
    static unsigned m_max_workers;
    static std::once_flag m_init_once;
};

} // namespace TPC

#endif // __XRDTPCPOOL_HH__