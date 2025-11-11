
set(XrdOfsPlugin XrdOfsPlugin-${PLUGIN_VERSION})

add_library(${XrdOfsPlugin} MODULE
  ${PROJECT_SOURCE_DIR}/src/XrdOfs/XrdOfsFS.cc
  XrdOfsPlugin.hh
  XrdOfsPluginFile.cc
  XrdOfsPluginFileSystem.cc
)

target_link_libraries(${XrdOfsPlugin} PRIVATE XrdServer XrdUtils)

install(TARGETS ${XrdOfsPlugin} LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
