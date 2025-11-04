#include "XrdSfs/XrdSfsInterface.hh"
#include "XrdVersion.hh"

XrdVERSIONINFO(XrdSfsGetFileSystem, MyXrdOfsPlugin);

class MyXrdOfsPlugin : public XrdSfsFileSystem {

    MyXrdOfsPlugin(XrdSfsFileSystem *nativeFS, XrdSysLogger *Logger,
                   const char *configFn, XrdOucEnv *envP)
        : nextFS_(nativeFS), logger_(Logger), configFn_(configFn), envP_(envP) {
    }

    XrdSfsFileSystem *nextFS_;
    XrdSysLogger *logger_;
    const char *configFn_;
    XrdOucEnv *envP_;
};
