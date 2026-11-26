#undef NDEBUG

#include "XrdHttpTpc/XrdHttpTpcPool.hh"

#include "XrdOuc/XrdOucEnv.hh"
#include "XrdSys/XrdSysError.hh"
#include "XrdSys/XrdSysLogger.hh"

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <mutex>
#include <poll.h>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {

struct CurlGlobal {
    CurlGlobal() { curl_global_init(CURL_GLOBAL_DEFAULT); }
    ~CurlGlobal() { curl_global_cleanup(); }
};

CurlGlobal curl_global;

bool SendAll(int fd, const std::string &data) {
    size_t off = 0;
    while (off < data.size()) {
        ssize_t n = ::send(fd, data.data() + off, data.size() - off, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (n == 0) {
            return false;
        }
        off += static_cast<size_t>(n);
    }
    return true;
}

std::string HttpResponse(int status, const std::string &reason, const std::string &extra, const std::string &body) {
    std::ostringstream ss;
    ss << "HTTP/1.1 " << status << ' ' << reason << "\r\n"
       << "Content-Length: " << body.size() << "\r\n"
       << "Connection: keep-alive\r\n"
       << extra
       << "\r\n"
       << body;
    return ss.str();
}

// One loopback HTTP/1.1 server.  Responses keep the connection open so a
// worker that reuses it does not cause another accept.
class LocalHttp {
  public:
    explicit LocalHttp(std::function<void(int, const std::string &)> handler) : handler_(std::move(handler)) {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        EXPECT_GE(listen_fd_, 0);
        int yes = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        EXPECT_EQ(::bind(listen_fd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)), 0);
        EXPECT_EQ(::listen(listen_fd_, 64), 0);
        socklen_t len = sizeof(addr);
        EXPECT_EQ(::getsockname(listen_fd_, reinterpret_cast<sockaddr *>(&addr), &len), 0);
        port_ = ntohs(addr.sin_port);
        thread_ = std::thread([this] { Loop(); });
    }

    ~LocalHttp() { Stop(); }

    LocalHttp(const LocalHttp &) = delete;
    LocalHttp &operator=(const LocalHttp &) = delete;

    int Port() const { return port_; }
    std::string Url(const std::string &path) const { return "http://127.0.0.1:" + std::to_string(port_) + path; }
    int Accepts() const { return accepts_.load(); }
    int Requests() const { return requests_.load(); }
    int Finished() const { return finished_.load(); }

    std::vector<std::string> Seen() const {
        std::lock_guard<std::mutex> lock(count_mu_);
        return seen_;
    }

    bool WaitRequests(int n, std::chrono::milliseconds budget) {
        std::unique_lock<std::mutex> lock(count_mu_);
        return count_cv_.wait_for(lock, budget, [&] { return requests_.load() >= n || stop_.load(); });
    }

    bool WaitFinished(int n, std::chrono::milliseconds budget) {
        std::unique_lock<std::mutex> lock(count_mu_);
        return count_cv_.wait_for(lock, budget, [&] { return finished_.load() >= n || stop_.load(); });
    }

    void Release() {
        std::lock_guard<std::mutex> lock(hold_mu_);
        release_ = true;
        hold_cv_.notify_all();
    }

    void Hold() {
        std::unique_lock<std::mutex> lock(hold_mu_);
        hold_cv_.wait(lock, [&] { return release_ || stop_.load(); });
    }

    void Stop() {
        bool expected = false;
        if (!stop_.compare_exchange_strong(expected, true)) {
            if (thread_.joinable()) {
                thread_.join();
            }
            return;
        }
        Release();
        if (listen_fd_ >= 0) {
            ::shutdown(listen_fd_, SHUT_RDWR);
        }
        if (thread_.joinable()) {
            thread_.join();
        }
        std::vector<std::thread> conns;
        {
            std::lock_guard<std::mutex> lock(conn_mu_);
            conns.swap(conns_);
        }
        for (auto &conn : conns) {
            if (conn.joinable()) {
                conn.join();
            }
        }
        if (listen_fd_ >= 0) {
            ::close(listen_fd_);
            listen_fd_ = -1;
        }
    }

  private:
    static std::string ReadHeaders(int fd, const std::atomic<bool> &stop) {
        std::string data;
        while (data.find("\r\n\r\n") == std::string::npos) {
            if (stop.load()) {
                return {};
            }
            pollfd pfd{};
            pfd.fd = fd;
            pfd.events = POLLIN;
            int rc = ::poll(&pfd, 1, 200);
            if (rc < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return {};
            }
            if (rc == 0) {
                continue;
            }
            char buf[2048];
            ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) {
                return {};
            }
            data.append(buf, static_cast<size_t>(n));
            if (data.size() > 1024 * 1024) {
                return {};
            }
        }
        return data;
    }

    void Serve(int fd) {
        accepts_.fetch_add(1);
        while (!stop_.load()) {
            std::string req = ReadHeaders(fd, stop_);
            if (req.empty()) {
                break;
            }
            {
                std::lock_guard<std::mutex> lock(count_mu_);
                seen_.push_back(req);
                requests_.fetch_add(1);
            }
            count_cv_.notify_all();
            handler_(fd, req);
        }
        ::close(fd);
        finished_.fetch_add(1);
        count_cv_.notify_all();
    }

    void Loop() {
        while (!stop_.load()) {
            pollfd pfd{};
            pfd.fd = listen_fd_;
            pfd.events = POLLIN;
            int rc = ::poll(&pfd, 1, 200);
            if (rc <= 0) {
                continue;
            }
            int fd = ::accept(listen_fd_, nullptr, nullptr);
            if (fd < 0) {
                continue;
            }
            // A cached connection stays open, so the next client must be
            // accepted by another thread.
            std::lock_guard<std::mutex> lock(conn_mu_);
            conns_.emplace_back([this, fd] { Serve(fd); });
        }
    }

    std::function<void(int, const std::string &)> handler_;
    int listen_fd_{-1};
    int port_{0};
    std::thread thread_;
    std::mutex conn_mu_;
    std::vector<std::thread> conns_;
    std::atomic<bool> stop_{false};
    std::atomic<int> accepts_{0};
    std::atomic<int> requests_{0};
    std::atomic<int> finished_{0};
    mutable std::mutex count_mu_;
    std::condition_variable count_cv_;
    std::mutex hold_mu_;
    std::condition_variable hold_cv_;
    bool release_{false};
    std::vector<std::string> seen_;
};

size_t DiscardBody(char *ptr, size_t size, size_t nmemb, void *userdata) {
    auto *body = static_cast<std::string *>(userdata);
    body->append(ptr, size * nmemb);
    return size * nmemb;
}

struct Transfer {
    Transfer(const std::string &url, const std::string &label) : curl(curl_easy_init()), request(label, curl) {
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, static_cast<long>(CURL_HTTP_VERSION_1_1));
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, DiscardBody);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    }

    ~Transfer() {
        if (submitted) {
            request.Cancel();
            request.WaitUntilFinished();
        }
        if (curl) {
            curl_easy_cleanup(curl);
        }
    }

    Transfer(const Transfer &) = delete;
    Transfer &operator=(const Transfer &) = delete;

    bool Submit(TPC::TPCRequestManager &mgr) {
        submitted = mgr.Produce(request);
        return submitted;
    }

    long NewConnections() const {
        long value = -1;
        curl_easy_getinfo(curl, CURLINFO_NUM_CONNECTS, &value);
        return value;
    }

    CURL *curl;
    TPC::TPCRequestManager::TPCRequest request;
    std::string body;
    bool submitted{false};
};

class PoolEnv : public ::testing::Test {
  protected:
    PoolEnv() : logger_(STDERR_FILENO, 0), err_(&logger_, "tpctest"), mgr_(env_, err_) {
        mgr_.SetMaxWorkers(8);
        mgr_.SetMaxGlobalWorkers(64);
        mgr_.SetMaxIdleRequests(8);
        mgr_.SetWorkerIdleTimeout(30s);
    }

    XrdSysLogger logger_;
    XrdSysError err_;
    XrdOucEnv env_;
    TPC::TPCRequestManager mgr_;
};

std::string KeyFor(const std::string &url) {
    TPC::QueueKeyInput in;
    in.url = url;
    return TPC::MakeQueueKey(in);
}

void RespondOk(int fd, const std::string &) { SendAll(fd, HttpResponse(200, "OK", "", "ok")); }

}  // namespace

TEST(QueueKey, SameLinkDiffersOnlyByPath) {
    TPC::QueueKeyInput a;
    TPC::QueueKeyInput b;
    a.url = "https://Disk.Example:8443/a/file";
    b.url = "https://disk.example:8443/b/other?x=1";
    EXPECT_EQ(TPC::MakeQueueKey(a), TPC::MakeQueueKey(b));
}

TEST(QueueKey, DefaultPortIsImplicit) {
    TPC::QueueKeyInput plain;
    TPC::QueueKeyInput numbered;
    plain.url = "http://host/a";
    numbered.url = "http://host:80/b";
    EXPECT_EQ(TPC::MakeQueueKey(plain), TPC::MakeQueueKey(numbered));

    plain.url = "https://host/a";
    numbered.url = "https://host:443/b";
    EXPECT_EQ(TPC::MakeQueueKey(plain), TPC::MakeQueueKey(numbered));
    plain.url = "https://host:8443/a";
    EXPECT_NE(TPC::MakeQueueKey(plain), TPC::MakeQueueKey(numbered));
}

TEST(QueueKey, HostPortSchemeAndAddressStayApart) {
    TPC::QueueKeyInput in;
    in.url = "https://one.example/f";
    const std::string https = TPC::MakeQueueKey(in);
    in.url = "http://one.example/f";
    EXPECT_NE(https, TPC::MakeQueueKey(in));
    in.url = "https://two.example/f";
    EXPECT_NE(https, TPC::MakeQueueKey(in));
    in.url = "https://one.example:9443/f";
    EXPECT_NE(https, TPC::MakeQueueKey(in));
    in.url = "http://[::1]/f";
    const std::string v6 = TPC::MakeQueueKey(in);
    in.url = "http://[::1]:8080/f";
    EXPECT_NE(v6, TPC::MakeQueueKey(in));
    EXPECT_NE(v6.find("[::1]"), std::string::npos);
}

TEST(QueueKey, CredentialSciTagVoAndBindDoNotShare) {
    TPC::QueueKeyInput base;
    base.url = "https://se.example/path";
    const std::string plain = TPC::MakeQueueKey(base);

    TPC::QueueKeyInput tagged = base;
    tagged.scitag = 0;
    EXPECT_NE(plain, TPC::MakeQueueKey(tagged));
    tagged.scitag = 7;
    const std::string sci = TPC::MakeQueueKey(tagged);
    tagged.scitag = 8;
    EXPECT_NE(sci, TPC::MakeQueueKey(tagged));

    TPC::QueueKeyInput org = base;
    org.vorg = "atlas";
    EXPECT_NE(plain, TPC::MakeQueueKey(org));
    org.vorg = "cms";
    EXPECT_NE(TPC::MakeQueueKey(org), plain);

    TPC::QueueKeyInput bound = base;
    bound.bind_ip = "10.0.0.8";
    EXPECT_NE(plain, TPC::MakeQueueKey(bound));

    TPC::QueueKeyInput token = base;
    token.headers.emplace_back("TransferHeaderAuthorization", "Bearer token-a");
    const std::string keyed = TPC::MakeQueueKey(token);
    EXPECT_NE(plain, keyed);
    EXPECT_EQ(keyed.find("token-a"), std::string::npos);
    EXPECT_EQ(keyed.find("Bearer"), std::string::npos);

    TPC::QueueKeyInput other = token;
    other.headers[0].second = "Bearer token-b";
    EXPECT_NE(keyed, TPC::MakeQueueKey(other));

    // Header names are matched regardless of case, so the same token shares.
    TPC::QueueKeyInput same = base;
    same.headers.emplace_back("transferheaderauthorization", "Bearer token-a");
    EXPECT_EQ(keyed, TPC::MakeQueueKey(same));

    TPC::QueueKeyInput named = base;
    named.x509_name = "/DC=ch/CN=one";
    const std::string first_dn = TPC::MakeQueueKey(named);
    EXPECT_NE(plain, first_dn);
    named.x509_name = "/DC=ch/CN=two";
    EXPECT_NE(first_dn, TPC::MakeQueueKey(named));
    EXPECT_EQ(TPC::MakeQueueKey(named).find("CN=two"), std::string::npos);

    TPC::QueueKeyInput creds = base;
    creds.creds = std::string("cert-a\0bytes", 12);
    const std::string cred_key = TPC::MakeQueueKey(creds);
    EXPECT_NE(plain, cred_key);
    creds.creds = std::string("cert-b\0bytes", 12);
    EXPECT_NE(cred_key, TPC::MakeQueueKey(creds));

    TPC::QueueKeyInput userinfo = base;
    userinfo.url = "https://alice:secret@se.example/path";
    const std::string with_user = TPC::MakeQueueKey(userinfo);
    EXPECT_NE(plain, with_user);
    EXPECT_EQ(with_user.find("secret"), std::string::npos);
    EXPECT_EQ(with_user.find("alice"), std::string::npos);
}

TEST_F(PoolEnv, SequentialTransfersReuseOneConnection) {
    LocalHttp server(RespondOk);
    const std::string label = KeyFor(server.Url("/file"));

    {
        Transfer first(server.Url("/file-a"), label);
        ASSERT_TRUE(first.Submit(mgr_));
        first.request.WaitUntilFinished();
        EXPECT_EQ(first.request.GetCurlResult(), 0);
        EXPECT_EQ(first.NewConnections(), 1);
        EXPECT_EQ(first.body, "ok");
    }
    Transfer second(server.Url("/file-b"), label);
    ASSERT_TRUE(second.Submit(mgr_));
    second.request.WaitUntilFinished();
    EXPECT_EQ(second.request.GetCurlResult(), 0);
    EXPECT_EQ(second.NewConnections(), 0);
    EXPECT_EQ(server.Accepts(), 1);
    EXPECT_EQ(server.Requests(), 2);
}

TEST_F(PoolEnv, HeadThenTransferReuseTheIdleWorker) {
    LocalHttp server([&](int fd, const std::string &req) {
        if (req.compare(0, 5, "HEAD ") == 0) {
            SendAll(fd, HttpResponse(200, "OK", "", ""));
        } else {
            SendAll(fd, HttpResponse(200, "OK", "", "ok"));
        }
    });
    const std::string label = KeyFor(server.Url("/src"));

    Transfer prime(server.Url("/src"), label);
    ASSERT_TRUE(prime.Submit(mgr_));
    prime.request.WaitUntilFinished();
    ASSERT_EQ(prime.request.GetCurlResult(), 0);

    Transfer head(server.Url("/src"), label);
    curl_easy_setopt(head.curl, CURLOPT_NOBODY, 1L);
    ASSERT_TRUE(mgr_.ProduceIfIdle(head.request));
    head.submitted = true;
    head.request.WaitUntilFinished();
    EXPECT_EQ(head.request.GetCurlResult(), 0);
    curl_easy_setopt(head.curl, CURLOPT_NOBODY, 0L);

    Transfer body(server.Url("/src"), label);
    ASSERT_TRUE(body.Submit(mgr_));
    body.request.WaitUntilFinished();
    EXPECT_EQ(body.request.GetCurlResult(), 0);
    EXPECT_EQ(body.NewConnections(), 0);
    EXPECT_EQ(server.Accepts(), 1);
    EXPECT_EQ(server.Requests(), 3);
}

TEST_F(PoolEnv, DifferentCredentialsOpenDifferentConnections) {
    LocalHttp server(RespondOk);
    TPC::QueueKeyInput one;
    TPC::QueueKeyInput two;
    one.url = two.url = server.Url("/data");
    one.headers.emplace_back("TransferHeaderAuthorization", "Bearer alpha");
    two.headers.emplace_back("TransferHeaderAuthorization", "Bearer beta");
    ASSERT_NE(TPC::MakeQueueKey(one), TPC::MakeQueueKey(two));

    curl_slist *alpha = curl_slist_append(nullptr, "Authorization: Bearer alpha");
    curl_slist *beta = curl_slist_append(nullptr, "Authorization: Bearer beta");
    Transfer first(server.Url("/data"), TPC::MakeQueueKey(one));
    curl_easy_setopt(first.curl, CURLOPT_HTTPHEADER, alpha);
    ASSERT_TRUE(first.Submit(mgr_));
    first.request.WaitUntilFinished();

    Transfer second(server.Url("/data"), TPC::MakeQueueKey(two));
    curl_easy_setopt(second.curl, CURLOPT_HTTPHEADER, beta);
    ASSERT_TRUE(second.Submit(mgr_));
    second.request.WaitUntilFinished();
    curl_slist_free_all(alpha);
    curl_slist_free_all(beta);
    EXPECT_EQ(second.request.GetCurlResult(), 0);
    EXPECT_EQ(second.NewConnections(), 1);
    EXPECT_EQ(server.Accepts(), 2);

    const auto seen = server.Seen();
    ASSERT_EQ(seen.size(), 2u);
    EXPECT_NE(seen[0].find("Bearer alpha"), std::string::npos);
    EXPECT_NE(seen[1].find("Bearer beta"), std::string::npos);
    EXPECT_EQ(seen[1].find("Bearer alpha"), std::string::npos);
}

TEST_F(PoolEnv, PerTransferHeadersAndRangeAreReset) {
    LocalHttp server(RespondOk);
    const std::string label = KeyFor(server.Url("/r"));

    curl_slist *first_headers = nullptr;
    first_headers = curl_slist_append(first_headers, "X-Test: one");
    first_headers = curl_slist_append(first_headers, "Expect: 100-continue");
    Transfer first(server.Url("/r"), label);
    curl_easy_setopt(first.curl, CURLOPT_HTTPHEADER, first_headers);
    curl_easy_setopt(first.curl, CURLOPT_RANGE, "0-3");
    ASSERT_TRUE(first.Submit(mgr_));
    first.request.WaitUntilFinished();
    EXPECT_EQ(first.request.GetCurlResult(), 0);

    curl_slist *second_headers = nullptr;
    second_headers = curl_slist_append(second_headers, "X-Test: two");
    Transfer second(server.Url("/r"), label);
    curl_easy_setopt(second.curl, CURLOPT_HTTPHEADER, second_headers);
    ASSERT_TRUE(second.Submit(mgr_));
    second.request.WaitUntilFinished();
    EXPECT_EQ(second.NewConnections(), 0);
    EXPECT_EQ(server.Accepts(), 1);

    const auto seen = server.Seen();
    ASSERT_EQ(seen.size(), 2u);
    EXPECT_NE(seen[0].find("X-Test: one"), std::string::npos);
    EXPECT_NE(seen[0].find("Range: bytes=0-3"), std::string::npos);
    EXPECT_NE(seen[1].find("X-Test: two"), std::string::npos);
    EXPECT_EQ(seen[1].find("X-Test: one"), std::string::npos);
    EXPECT_EQ(seen[1].find("Range:"), std::string::npos);
    EXPECT_EQ(seen[1].find("100-continue"), std::string::npos);
    curl_slist_free_all(first_headers);
    curl_slist_free_all(second_headers);
}

TEST_F(PoolEnv, HttpErrorDoesNotReuseTheConnection) {
    std::atomic<int> calls{0};
    LocalHttp server([&](int fd, const std::string &) {
        int n = calls.fetch_add(1);
        if (n == 0) {
            SendAll(fd, HttpResponse(500, "ERR", "", "bad"));
        } else {
            SendAll(fd, HttpResponse(200, "OK", "", "ok"));
        }
    });
    const std::string label = KeyFor(server.Url("/e"));

    Transfer failed(server.Url("/e"), label);
    ASSERT_TRUE(failed.Submit(mgr_));
    failed.request.WaitUntilFinished();
    EXPECT_EQ(server.Accepts(), 1);

    Transfer again(server.Url("/e"), label);
    ASSERT_TRUE(again.Submit(mgr_));
    again.request.WaitUntilFinished();
    EXPECT_EQ(again.request.GetCurlResult(), 0);
    EXPECT_EQ(again.body, "ok");
    EXPECT_EQ(again.NewConnections(), 1);
    EXPECT_EQ(server.Accepts(), 2);
}

TEST_F(PoolEnv, RedirectDoesNotReuseTheConnection) {
    LocalHttp server([&](int fd, const std::string &req) {
        if (req.find("GET /redir ") != std::string::npos) {
            SendAll(fd, HttpResponse(302, "Found", "Location: http://127.0.0.1:" + std::to_string(server.Port()) + "/landed\r\n", ""));
        } else {
            SendAll(fd, HttpResponse(200, "OK", "", "ok"));
        }
    });
    const std::string label = KeyFor(server.Url("/redir"));

    Transfer redirected(server.Url("/redir"), label);
    curl_easy_setopt(redirected.curl, CURLOPT_FOLLOWLOCATION, 1L);
    ASSERT_TRUE(redirected.Submit(mgr_));
    redirected.request.WaitUntilFinished();
    EXPECT_EQ(redirected.request.GetCurlResult(), 0);
    const int after = server.Accepts();
    ASSERT_GE(after, 1);

    Transfer next(server.Url("/plain"), label);
    ASSERT_TRUE(next.Submit(mgr_));
    next.request.WaitUntilFinished();
    EXPECT_EQ(next.request.GetCurlResult(), 0);
    EXPECT_GT(server.Accepts(), after);
    EXPECT_EQ(next.NewConnections(), 1);
}

TEST_F(PoolEnv, WorkerSurvivesAFailedTransfer) {
    LocalHttp server([&](int fd, const std::string &) { SendAll(fd, HttpResponse(200, "OK", "", "ok")); });
    const std::string label = KeyFor(server.Url("/ok"));

    Transfer refused(std::string("http://127.0.0.1:1/nope"), label);
    curl_easy_setopt(refused.curl, CURLOPT_CONNECTTIMEOUT, 1L);
    ASSERT_TRUE(refused.Submit(mgr_));
    refused.request.WaitUntilFinished();
    EXPECT_NE(refused.request.GetCurlResult(), 0);

    Transfer ok(server.Url("/ok"), label);
    ASSERT_TRUE(ok.Submit(mgr_));
    ok.request.WaitUntilFinished();
    EXPECT_EQ(ok.request.GetCurlResult(), 0);
    EXPECT_EQ(ok.body, "ok");
}

TEST_F(PoolEnv, DistinctEndpointsAreNotThrottledToThePerKeyCap) {
    mgr_.SetMaxWorkers(1);
    mgr_.SetMaxGlobalWorkers(16);

    constexpr int n = 4;
    auto hold = std::make_shared<std::atomic<bool>>(true);
    auto cv = std::make_shared<std::condition_variable>();
    auto mu = std::make_shared<std::mutex>();
    std::vector<std::unique_ptr<LocalHttp>> servers;
    std::vector<std::unique_ptr<Transfer>> jobs;
    for (int i = 0; i < n; ++i) {
        (void)i;
        servers.push_back(std::make_unique<LocalHttp>([=](int fd, const std::string &) {
            std::unique_lock<std::mutex> lock(*mu);
            cv->wait(lock, [&] { return !hold->load(); });
            SendAll(fd, HttpResponse(200, "OK", "", "ok"));
        }));
        jobs.push_back(std::make_unique<Transfer>(servers.back()->Url("/f"), KeyFor(servers.back()->Url("/f"))));
    }
    for (auto &job : jobs) {
        ASSERT_TRUE(job->Submit(mgr_));
    }
    for (auto &server : servers) {
        ASSERT_TRUE(server->WaitRequests(1, 2s)) << "a distinct endpoint did not start while another was in progress";
    }
    hold->store(false);
    cv->notify_all();
    for (auto &job : jobs) {
        job->request.WaitUntilFinished();
        EXPECT_EQ(job->request.GetCurlResult(), 0);
    }
}

TEST_F(PoolEnv, StalledTransferDoesNotBlockAnotherLink) {
    mgr_.SetMaxWorkers(1);
    LocalHttp stalled([&](int fd, const std::string &) {
        stalled.Hold();
        SendAll(fd, HttpResponse(200, "OK", "", "slow"));
    });
    LocalHttp fast(RespondOk);
    Transfer slow(stalled.Url("/slow"), KeyFor(stalled.Url("/slow")));
    ASSERT_TRUE(slow.Submit(mgr_));
    ASSERT_TRUE(stalled.WaitRequests(1, 2s));

    Transfer quick(fast.Url("/fast"), KeyFor(fast.Url("/fast")));
    ASSERT_TRUE(quick.Submit(mgr_));
    const auto started = std::chrono::steady_clock::now();
    quick.request.WaitUntilFinished();
    const auto elapsed = std::chrono::steady_clock::now() - started;
    EXPECT_EQ(quick.request.GetCurlResult(), 0);
    EXPECT_LT(elapsed, 1s);
    EXPECT_EQ(stalled.Requests(), 1);
    stalled.Release();
}

TEST_F(PoolEnv, FullQueueRejectsAndAQueuedCancelFinishes) {
    mgr_.SetMaxWorkers(1);
    mgr_.SetMaxIdleRequests(1);
    LocalHttp server([&](int fd, const std::string &) {
        server.Hold();
        SendAll(fd, HttpResponse(200, "OK", "", "ok"));
    });
    const std::string label = KeyFor(server.Url("/q"));

    Transfer running(server.Url("/q"), label);
    ASSERT_TRUE(running.Submit(mgr_));
    ASSERT_TRUE(server.WaitRequests(1, 2s));

    Transfer waiting(server.Url("/q"), label);
    ASSERT_TRUE(waiting.Submit(mgr_));
    Transfer rejected(server.Url("/q"), label);
    EXPECT_FALSE(rejected.Submit(mgr_));

    waiting.request.Cancel();
    EXPECT_TRUE(waiting.request.WaitFor(1s));
    EXPECT_EQ(waiting.request.GetMessage(), "Transfer cancelled");
    EXPECT_EQ(server.Requests(), 1);
    server.Release();
}

TEST_F(PoolEnv, CancellingARunningTransferWakesTheWorker) {
    LocalHttp server([&](int fd, const std::string &) {
        server.Hold();
        SendAll(fd, HttpResponse(200, "OK", "", "ok"));
    });
    Transfer job(server.Url("/c"), KeyFor(server.Url("/c")));
    ASSERT_TRUE(job.Submit(mgr_));
    ASSERT_TRUE(server.WaitRequests(1, 2s));
    const auto started = std::chrono::steady_clock::now();
    job.request.Cancel();
    EXPECT_TRUE(job.request.WaitFor(1s));
    EXPECT_LT(std::chrono::steady_clock::now() - started, 1s);
    EXPECT_NE(job.request.GetMessage().find("cancelled"), std::string::npos);
    server.Release();
}

TEST_F(PoolEnv, GlobalCapHandsASlotToAWaitingLink) {
    mgr_.SetMaxWorkers(4);
    mgr_.SetMaxGlobalWorkers(1);
    mgr_.SetWorkerIdleTimeout(30s);

    LocalHttp busy([&](int fd, const std::string &) {
        busy.Hold();
        SendAll(fd, HttpResponse(200, "OK", "", "busy"));
    });
    LocalHttp quiet(RespondOk);
    Transfer held(busy.Url("/a"), KeyFor(busy.Url("/a")));
    ASSERT_TRUE(held.Submit(mgr_));
    ASSERT_TRUE(busy.WaitRequests(1, 2s));

    Transfer waiting(quiet.Url("/b"), KeyFor(quiet.Url("/b")));
    ASSERT_TRUE(waiting.Submit(mgr_));
    std::this_thread::sleep_for(200ms);
    EXPECT_EQ(quiet.Requests(), 0);

    busy.Release();
    EXPECT_TRUE(quiet.WaitRequests(1, 2s));
    waiting.request.WaitUntilFinished();
    EXPECT_EQ(waiting.request.GetCurlResult(), 0);
}

TEST_F(PoolEnv, IdleWorkerIsReapedAndTheNextTransferReconnects) {
    mgr_.SetWorkerIdleTimeout(1s);
    LocalHttp server(RespondOk);
    const std::string label = KeyFor(server.Url("/i"));
    {
        Transfer first(server.Url("/i"), label);
        ASSERT_TRUE(first.Submit(mgr_));
        first.request.WaitUntilFinished();
        EXPECT_EQ(first.request.GetCurlResult(), 0);
    }
    ASSERT_TRUE(server.WaitFinished(1, 5s));
    Transfer second(server.Url("/i"), label);
    ASSERT_TRUE(second.Submit(mgr_));
    second.request.WaitUntilFinished();
    EXPECT_EQ(second.request.GetCurlResult(), 0);
    EXPECT_EQ(second.NewConnections(), 1);
    EXPECT_GE(server.Accepts(), 2);
}

TEST_F(PoolEnv, ShutdownUnblocksARunningTransfer) {
    LocalHttp server([&](int fd, const std::string &) {
        server.Hold();
        SendAll(fd, HttpResponse(200, "OK", "", "ok"));
    });
    Transfer job(server.Url("/s"), KeyFor(server.Url("/s")));
    ASSERT_TRUE(job.Submit(mgr_));
    ASSERT_TRUE(server.WaitRequests(1, 2s));
    const auto started = std::chrono::steady_clock::now();
    mgr_.Shutdown();
    EXPECT_TRUE(job.request.WaitFor(2s));
    EXPECT_LT(std::chrono::steady_clock::now() - started, 2s);
    Transfer later(server.Url("/s"), KeyFor(server.Url("/s")));
    EXPECT_FALSE(later.Submit(mgr_));
    server.Release();
}
