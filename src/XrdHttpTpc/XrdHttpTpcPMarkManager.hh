//------------------------------------------------------------------------------
// This file is part of XrdHttpTpcTPC
//
// Copyright (c) 2023 by European Organization for Nuclear Research (CERN)
// Author: Cedric Caffy <ccaffy@cern.ch>
// File Date: Oct 2023
//------------------------------------------------------------------------------
// XRootD is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// XRootD is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with XRootD.  If not, see <http://www.gnu.org/licenses/>.
//------------------------------------------------------------------------------
#ifndef XROOTD_PMARKMANAGER_HH
#define XROOTD_PMARKMANAGER_HH

#include "XrdNet/XrdNetPMark.hh"
#include "XrdSec/XrdSecEntity.hh"
#include "XrdNet/XrdNetAddrInfo.hh"
#include "XrdNet/XrdNetAddr.hh"
#include "XrdHttp/XrdHttpExtHandler.hh"

#include <curl/curl.h>

#include <map>
#include <memory>
#include <queue>
#include <set>

namespace TPC {
  enum class TpcType;
}

/**
 * This class will manage packet marking handles for TPC transfers
 *
 * Each time a socket is opened by curl (via the opensocket_callback), the manager
 * connects it and registers it for packet marking. A transfer that reuses a cached
 * connection never opens a socket, so the active socket is registered the same way
 * once libcurl reports it.
 *
 * Once the transfer will start we will start the packet marking by creating XrdNetPMark::Handle
 * objects from the socket information previously registered. Ending those handles
 * emits the firefly and leaves the socket open for a later transfer.
 *
 * In the case of multi-stream HTTP TPC transfers, a packet marking handle will be created for each stream.
 * The first one will be created as a basic one. The other will be created using the first packet marking handle as a basis.
 */
namespace XrdHttpTpc
{
class PMarkManager {
public:

  /**
   * This class allows to create and keep a XrdSecEntity object
   * from the socket file descriptor and address
   * Everything is done on the constructor
   *
   * These infos will be used later on when we create new PMark handles
   */
  class SocketInfo {
  public:
    SocketInfo(int fd, const struct sockaddr * sockP);
    XrdNetAddr netAddr;
    XrdSecEntity client;
  };

  PMarkManager(XrdHttpExtReq &req, const TPC::TpcType type);

  /**
   * Will connect the socket attached to the file descriptor within a certain timeout and add the file descriptor to the.
   * packet marking manager so packet marking can be done achieved later on
   * If pmark is not enabled, this function will just return true without trying to connect (libcurl will
   * perform the connection). False will be returned in case the connection could not be done and
   * the file descritor will be closed.
   *
   * @param fd the fd associated to the socket
   * @param sockP the connection information
   * @param sockPLen the length of the connection information
   * @param timeout_sec the timeout after which the connection is considered failed
   * @param err the error stream allowing to understand from where the issue comes from
   * @return true if the connection could be established or if pmark is disabled, false otherwise
   */
  bool connect(int fd, const struct sockaddr * sockP, size_t sockPLen, uint32_t timeout_sec, std::stringstream & err);

  /**
   * @return true if packet marking can happen i.e the packet marking is enabled in
   * the configuration and a scitag was provided by the user.
   */
  bool isEnabled() const;

  /**
   * Calling this function will indicate that the connections that will happen will be related to the
   * data transfer. The addFd(...) function will then register any socket that is created after this function
   * will be called.
   */
  void startTransfer();

  /**
   * Creates the different packet marking handles allowing to mark the transfer packets
   *
   * Call this after the curl_multi_perform() has been called.
   */
  void beginPMarks();

  /**
   * Passed to libcurl so a reused connection can be marked before the request
   * is sent.  The pointed-to object must outlive the transfer.
   */
  struct CurlMarkCtx {
    PMarkManager *manager;
    CURL *curl;
  };

  /**
   * Ask libcurl to call markCurlHandle() once this handle's connection is
   * chosen, including a connection taken from the cache.
   */
  void armReuseMark(CurlMarkCtx &ctx);

  /**
   * Start a firefly for the socket this transfer is using, including a socket
   * libcurl reused from its connection cache. A socket already marked for this
   * transfer is left unchanged.
   */
  void markCurlHandle(CURL *curl);

  /**
   * Emit the firefly for every socket marked for this transfer. The sockets
   * stay open.
   */
  void endTransferMarks();

  /**
   * This function deletes the PMark handle associated to the fd passed in parameter.
   * The socket is not closed. Call this while the socket is still open: the firefly
   * reads its byte counters from the socket as the handle is destroyed.
   * @param fd the fd of the socket whose firefly should be emitted
   */
  void endPmark(int fd);

  virtual ~PMarkManager() = default;
private:
  /**
  * Add the connected socket information that will be used for packet marking to this manager class
  * Note: these info will only be added if startTransfer(...) has been called. It allows
  * to ensure that the connection will be related to the data transfers and not for anything else. We only want
  * to mark the traffic of the transfers.
  * @param fd the socket file descriptor
  * @param sockP the structure describing the address of the socket
  */
  void addFd(int fd, const struct sockaddr * sockP);

  static int prereqCallback(void *clientp, char *conn_primary_ip, char *conn_local_ip,
                            int conn_primary_port, int conn_local_port);

  // The queue of socket information from which we will create the packet marking handles
  std::queue<SocketInfo> mSocketInfos;
  // File descriptors currently sitting in mSocketInfos.  A reused socket is
  // reported on every perform, and must be queued only once.
  std::set<int> mQueuedFds;
  // The map of socket FD and packet marking handles
  std::map<int,std::unique_ptr<XrdNetPMark::Handle>> mPmarkHandles;
  // The instance of the packet marking functionality
  XrdNetPMark * mPmark;
  // The XrdHttpTPC request information
  XrdHttpExtReq & mReq;
  // Is true when startTransfer(...) has been called
  bool mTransferWillStart;
  // Is true if this transfer is a HTTP TPC PULL transfer, false otherwise
  // by default is true
  TPC::TpcType mTpcType;
};
} // namespace XrdHttpTpc

#endif //XROOTD_PMARKMANAGER_HH
