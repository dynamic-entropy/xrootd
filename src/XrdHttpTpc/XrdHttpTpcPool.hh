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
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

// Forward dec'ls
class XrdOucEnv;
class XrdSysError;
namespace XrdHttpTpc {
class PMarkManager;
}

// One shared pool of TPC worker threads.
//
// Queues
// ------
// MakeQueueKey() names a queue.  The pool does not parse that string.  TPC
// builds it from the remote link (scheme, host, port), the SciTag, the VO,
// the local bind address, and a hash of the credential.  The hash covers the
// outgoing TransferHeader/Copy-Header values, any URL userinfo, the X.509
// name, and the raw credential bytes.  The secret itself is not part of the
// string, because the name is logged.  Two transfers share a queue only when
// that whole identity matches, so a cached connection cannot move to another
// host, token, certificate, SciTag, or source address.
//
// Workers
// -------
// Each worker owns one libcurl multi-handle and, while the connection is
// healthy, keeps it after the transfer (CURLMOPT_MAXCONNECTS is 1).  The
// worker is published as idle before the client is told the transfer
// finished, and the most recently idle worker is woken first.  The next
// transfer on that queue therefore reuses the connection.  A transfer that
// fails, is cancelled, is redirected, or ends with an HTTP error drops that
// cache before the next request is accepted.  The client thread only queues
// the easy handle and waits; it still emits performance markers.  The queue
// lock is not held across curl calls.
//
// Caps
// ----
// tpc.max_active_transfers_per_queue (default 20) is the most workers one
// queue may run.  Further transfers for that link wait on that queue, so a
// slow transfer does not occupy a worker that belongs to another link.
// tpc.max_active_transfers (default 4096) is the most workers in the whole
// pool.  Past that, a transfer still waits on its own queue instead of being
// rejected.  A queue that has work and no worker is starved: the oldest idle
// worker of another queue is reaped, and a worker that finishes while some
// other queue is starved exits if this queue still has another worker or has
// nothing left queued.  The freed slot is given to the oldest starved queue.
// A queue that already has a worker keeps one worker, so a busy link is not
// frozen in order to feed a new one.
// tpc.max_waiting_transfers_per_queue (default 20) bounds each queue.  Past
// that, Produce() returns false and the handler answers 429.
// tpc.worker_idle_timeout (default 60s) is how long an idle worker, and the
// connection it holds, stays up when nobody else is waiting for a slot.
//
// Shutdown
// --------
// The destructor stops every queue, wakes workers blocked in curl, and waits
// until they have left.  Queued requests are finished so the client threads
// unblock.
namespace TPC {

struct QueueKeyInput {
    // Remote URL the transfer connects to.  Only scheme, host, and port are
    // part of the key; the path is not.  userinfo is hashed into the
    // credential and is not copied into the key.
    std::string url;
    // -1 when the client sent no SciTag.  0 is a real tag.
    int scitag{-1};
    std::string vorg;
    std::string x509_name;
    // Raw credential bytes (may contain NULs).  Hashed, never logged.
    std::string creds;
    // Request headers.  Only TransferHeader* and Copy-Header* affect the key.
    std::vector<std::pair<std::string, std::string>> headers;
    // Local address CURLOPT_INTERFACE will bind, or empty.
    std::string bind_ip;
};

// A stable queue name.  See the comment on the pool above.
std::string MakeQueueKey(const QueueKeyInput &in);

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
        // The worker arms this for the duration of RunCurl so Cancel() can
        // interrupt curl_multi_wait.  arg is the worker; it stays alive until
        // DisarmWake() because the worker disarms before it returns the request.
        void ArmWake(void (*fn)(void *), void *arg);
        void DisarmWake();

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
        // A queue with matching identifier is created if it does not already exist.
        std::string m_label;
        CURL *m_curl;
        std::condition_variable m_cv;
        std::mutex m_mutex;
        std::string m_message;
        XrdHttpTpc::PMarkManager *m_pmark{nullptr};
        std::weak_ptr<TPCQueue> m_queue;
        std::mutex m_wake_mu;
        void (*m_wake_fn)(void *){nullptr};
        void *m_wake_arg{nullptr};
    };

    TPCRequestManager(XrdOucEnv &xrdEnv, XrdSysError &eDest);
    ~TPCRequestManager();

    TPCRequestManager(const TPCRequestManager &) = delete;
    TPCRequestManager &operator=(const TPCRequestManager &) = delete;

    // Queue the request.  Returns false if its queue is full, the pool is
    // shutting down, or a worker thread could not be started.
    bool Produce(TPCRequest &handler);
    // Hand the request to an idle worker of its queue.  Returns false, without
    // queueing the request, if the queue has no idle worker.
    bool ProduceIfIdle(TPCRequest &handler);

    void SetWorkerIdleTimeout(std::chrono::steady_clock::duration dur);
    void SetMaxWorkers(unsigned max_workers) { m_max_workers = max_workers; }
    void SetMaxGlobalWorkers(unsigned max_workers);
    void SetMaxIdleRequests(unsigned max_pending_ops) { m_max_pending_ops = max_pending_ops; }

    // Stop every queue and wait until the worker threads have exited.
    void Shutdown();

   private:
    class TPCQueue : public std::enable_shared_from_this<TPCQueue> {
        class TPCWorker;

       public:
        TPCQueue(const std::string &identifier, TPCRequestManager &parent) : m_identifier(identifier), m_parent(parent) {}

        // Returns false if the queue is full or has shut down; IsDone() tells which.
        bool Produce(TPCRequest &handler);
        bool ProduceIfIdle(TPCRequest &handler);
        bool Withdraw(TPCRequest &handler);
        void Stop();
        bool IsDone() const { return m_done.load(std::memory_order_acquire); }
        // Called with no queue lock.  Spawns one worker if this queue has
        // queued work and no one to run it.  Returns false when a global slot
        // was not available.
        bool TrySpawnForStarved();

       private:
        friend class TPCRequestManager;

        class TPCWorker final {
           public:
            explicit TPCWorker(TPCQueue &queue) : m_queue(queue) {}
            TPCWorker(const TPCWorker &) = delete;

            void Run();
            static void RunStatic(std::shared_ptr<TPCQueue> queue, std::unique_ptr<TPCWorker> myself);
            static void WakeThunk(void *arg);

            // Guarded by the queue's m_mutex.  Set when the worker is handed a
            // request (or spawned for one) that it has not yet taken.
            bool m_claimed{false};
            // Guarded by the queue's m_mutex.  The worker exits instead of
            // taking another request.
            bool m_reap{false};
            // The multi-handle could not be created, or its replacement could
            // not.  Guarded by the queue's m_mutex once the worker is leaving.
            bool m_force_stop{false};
            std::condition_variable m_cv;
            void Wake();

           private:
            struct CurlOutcome {
                std::string message;
                int curl_result{-1};
                bool reuse{false};
                // The transfer touched the cached connection and it must not
                // be used again.
                bool discard{false};
            };

            CurlOutcome RunCurl(CURLM *multi_handle, TPCRequest &request);
            // Drop every connection cached on this multi-handle and replace it.
            // The easy handle is left alone, so per-transfer options stay with
            // the caller.
            CURLM *DiscardCached(CURLM *multi_handle);
            static int closesocket_callback(void *clientp, curl_socket_t fd);

            // Set only while RunCurl drives a transfer; cleared before the
            // request is finished.
            XrdHttpTpc::PMarkManager *m_active_pmark{nullptr};
            TPCQueue &m_queue;
            std::mutex m_wake_mu;
            CURLM *m_multi{nullptr};
        };

        // Block until this worker has a request, or return nullptr when it
        // must exit.  On nullptr the worker has already left the queue.
        TPCRequest *WaitForRequest(TPCWorker &worker);
        // After a transfer: take the next queued request, publish this worker
        // as idle, or leave the queue.  The previous request is not finished
        // here; the caller finishes it after this returns so an idle worker is
        // visible first.  A returned request is the one to run next.
        // leave is set when the worker must exit after the previous request
        // is finished.
        TPCRequest *HandOff(TPCWorker &worker, bool &leave);
        void DetachWorker(TPCWorker &worker);
        // The helpers below require m_mutex.
        bool ClaimIdleWorker();
        bool SpawnWorker();
        bool NeedsWorker() const;

        std::atomic<bool> m_done{false};
        // Set while this queue sits on the manager's starved list.
        std::atomic<bool> m_listed{false};
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
    bool AcquireSlot();
    void ReleaseSlot();
    void NoteStarved(const std::shared_ptr<TPCQueue> &queue);
    void DispatchStarved();
    void ReapOneIdle();
    bool HasStarved() const { return m_starved_count.load(std::memory_order_acquire) > 0; }
    bool IsShutdown() const { return m_shutdown.load(std::memory_order_acquire); }

    static std::shared_mutex m_mutex;
    XrdSysError &m_log;  // Log object for the request manager
    static std::chrono::steady_clock::duration m_idle_timeout;
    static std::unordered_map<std::string, std::shared_ptr<TPCQueue>> m_pool_map;
    static unsigned m_max_pending_ops;
    static unsigned m_max_workers;
    static std::atomic<unsigned> m_max_global_workers;
    static std::atomic<unsigned> m_live;
    static std::atomic<bool> m_shutdown;
    static std::mutex m_drain_mu;
    static std::condition_variable m_drain_cv;
    static std::deque<std::weak_ptr<TPCQueue>> m_starved;
    static std::atomic<int> m_starved_count;
    XrdOucEnv &m_xrdEnv;
};

}  // namespace TPC

#endif  // __XRDHTTPTPCPOOL_HH__
