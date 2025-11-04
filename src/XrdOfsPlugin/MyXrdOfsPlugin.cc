#include "MyXrdOfsPlugin.hh"

MyXrdOfsPlugin *ofs = nullptr;

extern "C" {
XrdSfsFileSystem *XrdSfsGetFileSystem2(XrdSfsFileSystem *nativeFS,
                                       XrdSysLogger *Logger,
                                       const char *configFn, XrdOucEnv *envP) {

    return ofs;
}
}