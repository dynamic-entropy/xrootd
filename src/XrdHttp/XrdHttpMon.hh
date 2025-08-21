#include <array>
#include <atomic>
#include <chrono>
#include <string>

#include "XrdHttpReq.hh"

class XrdXrootdGStream;
class XrdSysLogger;

class XrdHttpMon {
   public:
    // Supported HTTP status codes
    enum StatusCodes {
        sc_100,
        sc_200,
        sc_201,
        sc_206,
        sc_302,
        sc_307,
        sc_400,
        sc_401,
        sc_403,
        sc_404,
        sc_405,
        sc_409,
        sc_416,
        sc_423,
        sc_500,
        sc_502,
        sc_504,
        sc_507,
        sc_UNKNOWN,
        sc_Count
    };

    // Per (operation, status code) statistics
    struct HttpInfo {
        std::atomic<uint64_t> count{0};
        // std::atomic<uint64_t> bytes{0};
        // std::atomic<std::chrono::system_clock::duration::rep> duration{0};
    };

    // Global stats table
    static std::array<std::array<HttpInfo, StatusCodes::sc_Count>, XrdHttpReq::ReqType::rtCount> statsInfo;

    XrdHttpMon(XrdSysLogger* logP, XrdXrootdGStream* gStream);

    static void* Start(void* args);

    static void Record(XrdHttpReq::ReqType op, StatusCodes sc);

    static StatusCodes ToStatusCode(int code);

    static std::string GetMonitoringJson();

   private:
    ~XrdHttpMon() {};

    XrdXrootdGStream* gStream;

    void Report();

    static std::string GetOperationString(XrdHttpReq::ReqType op);
    static std::string GetStatusCodeString(StatusCodes sc);
};
