add_executable(xrdhttptpc-unit-tests XrdHttpTpcTests.cc)

add_library(XrdHttpTpcUtils
  ${PROJECT_SOURCE_DIR}/src/XrdHttpTpc/XrdHttpTpcUtils.cc
)

target_link_libraries(xrdhttptpc-unit-tests XrdHttpTpcUtils GTest::GTest GTest::Main)

gtest_discover_tests(xrdhttptpc-unit-tests PROPERTIES DISCOVERY_TIMEOUT 10)

# Add Pool tests
if(ENABLE_HTTP AND CURL_FOUND)
  add_executable(xrdhttptpc-pool-tests XrdHttpTpcPoolTests.cc
    ${PROJECT_SOURCE_DIR}/src/XrdHttpTpc/XrdHttpTpcPool.cc
    ${PROJECT_SOURCE_DIR}/src/XrdHttpTpc/XrdHttpTpcPMarkManager.cc
  )

  target_link_libraries(xrdhttptpc-pool-tests
    XrdServer
    XrdUtils
    XrdHttpUtils
    CURL::libcurl
    GTest::GTest
    GTest::Main
    ${CMAKE_THREAD_LIBS_INIT}
    ${CMAKE_DL_LIBS}
  )

  # Include directories
  target_include_directories(xrdhttptpc-pool-tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/src/XrdHttpTpc
  )

  gtest_discover_tests(xrdhttptpc-pool-tests PROPERTIES DISCOVERY_TIMEOUT 30)
endif()
