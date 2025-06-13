#include <stdio.h>

#include "XrdSys/XrdSysError.hh"
#include "XrdXrootd/XrdXrootdGStream.hh"
#include "XrdXrootdHttpMon.hh"

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