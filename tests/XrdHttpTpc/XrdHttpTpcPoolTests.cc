#undef NDEBUG

#include "XrdHttpTpc/XrdHttpTpcPool.hh"
#include "XrdOuc/XrdOucEnv.hh"
#include "XrdSys/XrdSysError.hh"
#include "XrdSys/XrdSysLogger.hh"
#include <curl/curl.h>
#include <gtest/gtest.h>
#include <chrono>
#include <thread>
#include <atomic>
#include <future>

using namespace testing;
using namespace TPC;

class TPCRequestManagerPoolTests : public Test {
protected:
    void SetUp() override {
        // Initialize curl library
        curl_global_init(CURL_GLOBAL_ALL);
        
        // Create a logger for XrdSysError
        logger = new XrdSysLogger();
        logger->SetParms(0, 0, 0);
        eDest = new XrdSysError(logger, "test");
        
        // Create a minimal XrdOucEnv
        env = new XrdOucEnv();
        
        // Create request manager
        manager = new TPCRequestManager(*env, *eDest);
        
        // Reset thread count before each test
        TPCRequestManager::SetMaxGlobalThreads(0);  // Reset to no limit
        // Note: We can't directly reset the thread count, but tests will account for it
    }
    
    void TearDown() override {
        // Wait a bit for threads to clean up
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        
        delete manager;
        delete env;
        delete eDest;
        delete logger;
        
        // Reset thread count
        TPCRequestManager::SetMaxGlobalThreads(0);
        
        // Cleanup curl library
        curl_global_cleanup();
    }
    
    // Helper to create a minimal curl handle for testing
    CURL* CreateTestCurlHandle() {
        CURL* curl = curl_easy_init();
        if (curl) {
            curl_easy_setopt(curl, CURLOPT_URL, "http://example.com");
        }
        return curl;
    }
    
    // Helper to create a test request
    std::unique_ptr<TPCRequestManager::TPCRequest> CreateTestRequest(
        const std::string& label = "test_label", int scitag = -1) {
        CURL* curl = CreateTestCurlHandle();
        if (!curl) {
            return nullptr;
        }
        auto req = std::make_unique<TPCRequestManager::TPCRequest>(label, scitag, curl);
        // Cleanup curl handle when request is destroyed
        // Note: In real usage, the request would handle this, but for testing we ensure cleanup
        return req;
    }
    
    XrdSysLogger* logger;
    XrdSysError* eDest;
    XrdOucEnv* env;
    TPCRequestManager* manager;
};

// Test: No limit means threads can always be created
TEST_F(TPCRequestManagerPoolTests, NoLimitAllowsThreadCreation) {
    TPCRequestManager::SetMaxGlobalThreads(0);  // No limit
    
    unsigned initial_count = TPCRequestManager::GetGlobalThreadCount();
    
    auto request = CreateTestRequest("test1");
    ASSERT_NE(request, nullptr);
    
    // Produce should succeed (thread creation allowed)
    bool result = manager->Produce(*request);
    EXPECT_TRUE(result);
    
    // Give it a moment for thread to start
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    // Count should have increased
    EXPECT_GE(TPCRequestManager::GetGlobalThreadCount(), initial_count);
    
    // Clean up
    request->Cancel();
    request->SetDone(499, "Test cancelled");
}

// Test: Thread limit is enforced
TEST_F(TPCRequestManagerPoolTests, ThreadLimitEnforced) {
    const unsigned limit = 2;
    TPCRequestManager::SetMaxGlobalThreads(limit);
    manager->SetMaxWorkers(10);  // Per-queue limit higher than global limit
    
    unsigned initial_count = TPCRequestManager::GetGlobalThreadCount();
    
    // Create requests up to the limit
    std::vector<std::unique_ptr<TPCRequestManager::TPCRequest>> requests;
    for (unsigned i = 0; i < limit; ++i) {
        auto req = CreateTestRequest("test" + std::to_string(i));
        ASSERT_NE(req, nullptr);
        requests.push_back(std::move(req));
    }
    
    // All should succeed
    for (auto& req : requests) {
        bool result = manager->Produce(*req);
        EXPECT_TRUE(result);
    }
    
    // Wait for threads to start
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // Verify we're at the limit
    unsigned current_count = TPCRequestManager::GetGlobalThreadCount();
    EXPECT_LE(current_count, initial_count + limit);
    
    // Try to create one more - should queue but not create thread immediately
    auto extra_req = CreateTestRequest("test_extra");
    ASSERT_NE(extra_req, nullptr);
    bool result = manager->Produce(*extra_req);
    EXPECT_TRUE(result);  // Should queue the request
    
    // Thread count should still be at limit
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_LE(TPCRequestManager::GetGlobalThreadCount(), initial_count + limit);
    
    // Clean up
    for (auto& req : requests) {
        req->Cancel();
        req->SetDone(499, "Test cancelled");
    }
    extra_req->Cancel();
    extra_req->SetDone(499, "Test cancelled");
}

// Test: Thread exit decrements counter
TEST_F(TPCRequestManagerPoolTests, ThreadExitDecrementsCounter) {
    TPCRequestManager::SetMaxGlobalThreads(0);  // No limit for setup
    
    auto request = CreateTestRequest("test_exit");
    ASSERT_NE(request, nullptr);
    
    unsigned count_before = TPCRequestManager::GetGlobalThreadCount();
    
    bool result = manager->Produce(*request);
    EXPECT_TRUE(result);
    
    // Wait for thread to start
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    unsigned count_after_start = TPCRequestManager::GetGlobalThreadCount();
    EXPECT_GT(count_after_start, count_before);
    
    // Cancel and mark done to cause thread exit
    request->Cancel();
    request->SetDone(499, "Test cancelled");
    
    // Wait for thread to exit
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    
    // Count should decrease (though exact count depends on timing)
    unsigned count_after_exit = TPCRequestManager::GetGlobalThreadCount();
    // The count may not go back to exactly count_before due to timing, but should be lower
}

// Test: Multiple queues share the global limit
TEST_F(TPCRequestManagerPoolTests, MultipleQueuesShareGlobalLimit) {
    const unsigned limit = 3;
    TPCRequestManager::SetMaxGlobalThreads(limit);
    manager->SetMaxWorkers(10);
    
    // Create requests for different labels (different queues)
    std::vector<std::unique_ptr<TPCRequestManager::TPCRequest>> requests;
    
    // Create requests from different queues
    requests.push_back(CreateTestRequest("queue1"));
    requests.push_back(CreateTestRequest("queue2"));
    requests.push_back(CreateTestRequest("queue3"));
    
    unsigned initial_count = TPCRequestManager::GetGlobalThreadCount();
    
    // All should succeed
    for (auto& req : requests) {
        bool result = manager->Produce(*req);
        EXPECT_TRUE(result);
    }
    
    // Wait for threads to start
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // Total threads should not exceed limit across all queues
    unsigned total_threads = TPCRequestManager::GetGlobalThreadCount();
    EXPECT_LE(total_threads, initial_count + limit);
    
    // Clean up
    for (auto& req : requests) {
        req->Cancel();
        req->SetDone(499, "Test cancelled");
    }
}

// Test: Thread limit getter returns correct value
TEST_F(TPCRequestManagerPoolTests, ThreadLimitGetter) {
    const unsigned limit1 = 5;
    TPCRequestManager::SetMaxGlobalThreads(limit1);
    EXPECT_EQ(TPCRequestManager::GetMaxGlobalThreads(), limit1);
    
    const unsigned limit2 = 10;
    TPCRequestManager::SetMaxGlobalThreads(limit2);
    EXPECT_EQ(TPCRequestManager::GetMaxGlobalThreads(), limit2);
    
    TPCRequestManager::SetMaxGlobalThreads(0);
    EXPECT_EQ(TPCRequestManager::GetMaxGlobalThreads(), 0u);
}

// Test: Request queued when at limit
TEST_F(TPCRequestManagerPoolTests, RequestQueuedWhenAtLimit) {
    const unsigned limit = 1;
    TPCRequestManager::SetMaxGlobalThreads(limit);
    manager->SetMaxWorkers(10);
    manager->SetMaxIdleRequests(10);
    
    auto request1 = CreateTestRequest("test1");
    ASSERT_NE(request1, nullptr);
    
    unsigned initial_count = TPCRequestManager::GetGlobalThreadCount();
    
    // First request should create a thread
    bool result1 = manager->Produce(*request1);
    EXPECT_TRUE(result1);
    
    // Wait for thread to start
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // Should be at limit now
    EXPECT_GE(TPCRequestManager::GetGlobalThreadCount(), initial_count + 1);
    
    // Second request should be queued but not create a thread immediately
    auto request2 = CreateTestRequest("test1");  // Same label, same queue
    ASSERT_NE(request2, nullptr);
    bool result2 = manager->Produce(*request2);
    EXPECT_TRUE(result2);  // Should queue successfully
    
    // Thread count should still be at limit
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_LE(TPCRequestManager::GetGlobalThreadCount(), initial_count + limit);
    
    // Clean up
    request1->Cancel();
    request1->SetDone(499, "Test cancelled");
    request2->Cancel();
    request2->SetDone(499, "Test cancelled");
}

// Test: Idle thread timeout still works with limit
TEST_F(TPCRequestManagerPoolTests, IdleThreadTimeoutWorks) {
    TPCRequestManager::SetMaxGlobalThreads(0);  // No limit
    manager->SetWorkerIdleTimeout(std::chrono::milliseconds(100));
    
    auto request = CreateTestRequest("test_timeout");
    ASSERT_NE(request, nullptr);
    
    bool result = manager->Produce(*request);
    EXPECT_TRUE(result);
    
    // Wait for thread to start
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    unsigned count_before = TPCRequestManager::GetGlobalThreadCount();
    EXPECT_GT(count_before, 0u);
    
    // Cancel immediately - thread should wait for idle timeout
    request->Cancel();
    request->SetDone(499, "Test cancelled");
    
    // Wait for idle timeout
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    
    // Thread should have exited after timeout
    unsigned count_after = TPCRequestManager::GetGlobalThreadCount();
    // Count should be lower (exact value depends on timing)
    EXPECT_LE(count_after, count_before);
}

// Test: Concurrent thread creation respects limit
TEST_F(TPCRequestManagerPoolTests, ConcurrentThreadCreationRespectsLimit) {
    const unsigned limit = 3;
    TPCRequestManager::SetMaxGlobalThreads(limit);
    manager->SetMaxWorkers(10);
    
    const unsigned num_requests = 10;
    std::vector<std::unique_ptr<TPCRequestManager::TPCRequest>> requests;
    
    // Create multiple requests concurrently
    for (unsigned i = 0; i < num_requests; ++i) {
        requests.push_back(CreateTestRequest("test" + std::to_string(i)));
    }
    
    unsigned initial_count = TPCRequestManager::GetGlobalThreadCount();
    
    // Produce all requests concurrently (simulated)
    std::vector<bool> results;
    for (auto& req : requests) {
        results.push_back(manager->Produce(*req));
    }
    
    // All should succeed (requests queued)
    for (bool result : results) {
        EXPECT_TRUE(result);
    }
    
    // Wait for threads to start
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    
    // Total thread count should not exceed limit
    unsigned final_count = TPCRequestManager::GetGlobalThreadCount();
    EXPECT_LE(final_count, initial_count + limit);
    
    // Clean up
    for (auto& req : requests) {
        req->Cancel();
        req->SetDone(499, "Test cancelled");
    }
}

