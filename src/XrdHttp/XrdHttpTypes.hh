/** @file  XrdHttpTypes.hh
 * @brief  Common HTTP types and enums shared across HTTP modules
 */

#ifndef XRDHTTPTYPES_HH
#define	XRDHTTPTYPES_HH

namespace XrdHttp {

  /// These are the HTTP/DAV requests that we support
  // Any changes here should also reflect in XrdHttpMon::verbCountersSchema to capture statistics of requests by verb
  // The count and order or verbs listed should be consistent with the monitoring counters
  enum ReqType : int {
    rtUnset = -1,
    rtUnknown = 0,
    rtMalformed,
    rtGET,
    rtHEAD,
    rtPUT,
    rtOPTIONS,
    rtPATCH,
    rtDELETE,
    rtPROPFIND,
    rtMKCOL,
    rtMOVE,
    rtPOST,
    rtCOPY,
    rtCount
  };

} // namespace XrdHttp

#endif	/* XRDHTTPTYPES_HH */
