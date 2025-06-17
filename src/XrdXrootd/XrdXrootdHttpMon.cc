#include "XrdXrootdHttpMon.hh"

#include <stdio.h>

#include <thread>

#include "XrdSys/XrdSysError.hh"
#include "XrdXrootd/XrdXrootdGStream.hh"

namespace {
const char* http_json_fmt =
    "{\"StatusCode\":\"%d\",\"StatusCount\":\"%d\",\"TotalCount\":\"%d\"}";

XrdSysError eDest(0, "Ouc");
}  // namespace

XrdXrootdHttpMon::XrdXrootdHttpMon(XrdSysLogger* logP,
                                   XrdXrootdGStream& gStream)
    : gStream(gStream) {
  eDest.logger(logP);
};

void XrdXrootdHttpMon::Report(XrdXrootdHttpMon::HttpInfo& info) {
  char buff[1024];

  int n = snprintf(buff, sizeof(buff), http_json_fmt, info.status,
                   info.statusCount, info.totalCount);

  fprintf(stderr, "HTTPMON::XrdHttpMon: buff= %s\n", buff);

  if (!gStream.Insert(buff, n + 1)) {
    eDest.Emsg("HttpMon", "Gstream Buffer Rejected");
  }
  fprintf(stderr, "HTTPMON::gStream.Insert= %d\n", gStream.Insert(buff, n + 1));
}

void* XrdXrootdHttpMon::Start(void* instance) {
  XrdXrootdHttpMon* new_instance = static_cast<XrdXrootdHttpMon*>(instance);
  while (1) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    XrdXrootdHttpMon::HttpInfo info(1, 2, 3);
    new_instance->Report(info);
  }
}