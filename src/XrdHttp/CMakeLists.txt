if(NOT BUILD_HTTP)
  return()
endif()

# Note that XrdHttpUtils is marked as a shared library as XrdHttp plugins
# are expected to link against it for the XrdHttpExt class implementations.

add_library(XrdHttpMon SHARED
  XrdHttpMon.cc
  XrdHttpMon.hh
)

set_target_properties(XrdHttpMon
  PROPERTIES VERSION 1.0.0 SOVERSION 1)

target_link_libraries(XrdHttpMon
  PRIVATE
    XrdUtils
)

add_library(XrdHttpUtils SHARED
  XrdHttpChecksum.cc         XrdHttpChecksum.hh
  XrdHttpChecksumHandler.cc  XrdHttpChecksumHandler.hh
  XrdHttpExtHandler.cc       XrdHttpExtHandler.hh
  XrdHttpProtocol.cc         XrdHttpProtocol.hh
  XrdHttpReadRangeHandler.cc XrdHttpReadRangeHandler.hh
  XrdHttpReq.cc              XrdHttpReq.hh
                             XrdHttpTypes.hh
                             XrdHttpSecXtractor.hh
  XrdHttpSecurity.cc
                             XrdHttpStatic.hh
                             XrdHttpTrace.hh
  XrdHttpUtils.cc            XrdHttpUtils.hh
  XrdHttpHeaderUtils.cc      XrdHttpHeaderUtils.hh
)

set_target_properties(XrdHttpUtils
  PROPERTIES VERSION 2.0.0 SOVERSION 2)

target_link_libraries(XrdHttpUtils
  PRIVATE
    XrdServer
    XrdUtils
    XrdCrypto
    XrdHttpMon
  PUBLIC
    OpenSSL::SSL
    OpenSSL::Crypto
)

set(XrdHttp XrdHttp-${PLUGIN_VERSION})
add_library(${XrdHttp} MODULE XrdHttpModule.cc)
target_link_libraries(${XrdHttp} PRIVATE XrdUtils XrdHttpUtils)
set_target_properties(${XrdHttp} PROPERTIES SUFFIX ".so")

install(
  TARGETS
    ${XrdHttp}
    XrdHttpUtils
    XrdHttpMon
  LIBRARY
    DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
