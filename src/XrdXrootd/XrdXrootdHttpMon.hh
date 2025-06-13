

class XrdXrootdGStream;
class XrdSysLogger;

class XrdXrootdHttpMon {
 public:
  struct HttpInfo {
    short status;
    int statusCount;
    int totalCount;

    HttpInfo(short status, int statusCount, int totalCount)
        : status(status), statusCount(statusCount), totalCount(totalCount) {};

    ~HttpInfo() {};
  };

  XrdXrootdHttpMon(XrdSysLogger* logP, XrdXrootdGStream& gStream);

  void Report(XrdXrootdHttpMon::HttpInfo& info);

 private:
  ~XrdXrootdHttpMon() {};

  XrdXrootdGStream& gStream;
};