
#include "XrdOfsPlugin.hh"

MyXrdOfsPlugin *ofs = nullptr;

XrdSfsDirectory *MyXrdOfsPlugin::newDir(char *user, int monid) {
    m_log.Say("INFO:", "MyXrdOfsPlugin::newDir");
    return m_next_sfs->newDir(user, monid);
}

XrdSfsDirectory *MyXrdOfsPlugin::newDir(XrdOucErrInfo &einfo) {
    m_log.Say("INFO:", "MyXrdOfsPlugin::newDir(einfo)");
    return m_next_sfs->newDir(einfo);
}

XrdSfsFile *MyXrdOfsPlugin::newFile(char *user, int monid) {
    m_log.Say("INFO:", "MyXrdOfsPlugin::newFile");
    XrdSfsFile *f = m_next_sfs->newFile(user, monid);
    if (!f) {
        m_log.Say("WARN:", "MyXrdOfsPlugin::newFile - underlying newFile returned null");
        return nullptr;
    }
    m_log.Say("INFO:", "MyXrdOfsPlugin::newFile - wrapping with FileWrapper");
    XrdSfsFile *fw = new FileWrapper(f, m_log);
    return fw;
}

XrdSfsFile *MyXrdOfsPlugin::newFile(XrdOucErrInfo &einfo) {
    m_log.Say("INFO:", "MyXrdOfsPlugin::newFile(einfo)");
    return m_next_sfs->newFile(einfo);
}

int MyXrdOfsPlugin::chksum(csFunc Func, const char *csName, const char *path, XrdOucErrInfo &eInfo,
                           const XrdSecEntity *client, const char *opaque) {
    m_log.Say("INFO:", "MyXrdOfsPlugin::chksum");
    return m_next_sfs->chksum(Func, csName, path, eInfo, client, opaque);
}

int MyXrdOfsPlugin::chmod(const char *path, XrdSfsMode mode, XrdOucErrInfo &eInfo, const XrdSecEntity *client,
                          const char *opaque) {
    m_log.Say("INFO:", "MyXrdOfsPlugin::chmod");
    return m_next_sfs->chmod(path, mode, eInfo, client, opaque);
}

void MyXrdOfsPlugin::Connect(const XrdSecEntity *client) {
    m_log.Say("INFO:", "MyXrdOfsPlugin::Connect");
    m_next_sfs->Connect(client);
}

void MyXrdOfsPlugin::Disc(const XrdSecEntity *client) {
    m_log.Say("INFO:", "MyXrdOfsPlugin::Disc");
    m_next_sfs->Disc(client);
}

void MyXrdOfsPlugin::EnvInfo(XrdOucEnv *envP) {
    m_log.Say("INFO:", "MyXrdOfsPlugin::EnvInfo");
    m_next_sfs->EnvInfo(envP);
}

int MyXrdOfsPlugin::exists(const char *path, XrdSfsFileExistence &eFlag, XrdOucErrInfo &eInfo,
                           const XrdSecEntity *client, const char *opaque) {
    m_log.Say("INFO:", "MyXrdOfsPlugin::exists");
    return m_next_sfs->exists(path, eFlag, eInfo, client, opaque);
}

int MyXrdOfsPlugin::FAttr(XrdSfsFACtl *faReq, XrdOucErrInfo &eInfo, const XrdSecEntity *client) {
    m_log.Say("INFO:", "MyXrdOfsPlugin::FAttr");
    return m_next_sfs->FAttr(faReq, eInfo, client);
}

int MyXrdOfsPlugin::FSctl(const int cmd, XrdSfsFSctl &args, XrdOucErrInfo &eInfo, const XrdSecEntity *client) {
    m_log.Say("INFO:", "MyXrdOfsPlugin::FSctl");
    return m_next_sfs->FSctl(cmd, args, eInfo, client);
}

int MyXrdOfsPlugin::fsctl(const int cmd, const char *args, XrdOucErrInfo &eInfo, const XrdSecEntity *client) {
    m_log.Say("INFO:", "MyXrdOfsPlugin::fsctl");
    return m_next_sfs->fsctl(cmd, args, eInfo, client);
}

int MyXrdOfsPlugin::getChkPSize() {
    m_log.Say("INFO:", "MyXrdOfsPlugin::getChkPSize");
    return m_next_sfs->getChkPSize();
}

int MyXrdOfsPlugin::getStats(char *buff, int blen) {
    m_log.Say("INFO:", "MyXrdOfsPlugin::getStats");
    return m_next_sfs->getStats(buff, blen);
}

const char *MyXrdOfsPlugin::getVersion() {
    m_log.Say("INFO:", "MyXrdOfsPlugin::getVersion");
    return XrdVERSION;
}

int MyXrdOfsPlugin::gpFile(gpfFunc &gpAct, XrdSfsGPFile &gpReq, XrdOucErrInfo &eInfo, const XrdSecEntity *client) {
    m_log.Say("INFO:", "MyXrdOfsPlugin::gpFile");
    return m_next_sfs->gpFile(gpAct, gpReq, eInfo, client);
}

int MyXrdOfsPlugin::mkdir(const char *path, XrdSfsMode mode, XrdOucErrInfo &eInfo, const XrdSecEntity *client,
                          const char *opaque) {
    m_log.Say("INFO:", "MyXrdOfsPlugin::mkdir");
    return m_next_sfs->mkdir(path, mode, eInfo, client, opaque);
}

int MyXrdOfsPlugin::prepare(XrdSfsPrep &pargs, XrdOucErrInfo &eInfo, const XrdSecEntity *client) {
    m_log.Say("INFO:", "MyXrdOfsPlugin::prepare");
    return m_next_sfs->prepare(pargs, eInfo, client);
}

int MyXrdOfsPlugin::rem(const char *path, XrdOucErrInfo &eInfo, const XrdSecEntity *client, const char *opaque) {
    m_log.Say("INFO:", "MyXrdOfsPlugin::rem");
    return m_next_sfs->rem(path, eInfo, client, opaque);
}

int MyXrdOfsPlugin::remdir(const char *path, XrdOucErrInfo &eInfo, const XrdSecEntity *client, const char *opaque) {
    m_log.Say("INFO:", "MyXrdOfsPlugin::remdir");
    return m_next_sfs->remdir(path, eInfo, client, opaque);
}

int MyXrdOfsPlugin::rename(const char *oPath, const char *nPath, XrdOucErrInfo &eInfo, const XrdSecEntity *client,
                           const char *opaqueO, const char *opaqueN) {
    m_log.Say("INFO:", "MyXrdOfsPlugin::rename");
    return m_next_sfs->rename(oPath, nPath, eInfo, client, opaqueO, opaqueN);
}

int MyXrdOfsPlugin::stat(const char *Name, struct stat *buf, XrdOucErrInfo &eInfo, const XrdSecEntity *client,
                         const char *opaque) {
    m_log.Say("INFO:", "MyXrdOfsPlugin::stat(struct stat)");
    return m_next_sfs->stat(Name, buf, eInfo, client, opaque);
}

int MyXrdOfsPlugin::stat(const char *path, mode_t &mode, XrdOucErrInfo &eInfo, const XrdSecEntity *client,
                         const char *opaque) {
    m_log.Say("INFO:", "MyXrdOfsPlugin::stat(mode_t)");
    return m_next_sfs->stat(path, mode, eInfo, client, opaque);
}

int MyXrdOfsPlugin::truncate(const char *path, XrdSfsFileOffset fsize, XrdOucErrInfo &eInfo, const XrdSecEntity *client,
                             const char *opaque) {
    m_log.Say("INFO:", "MyXrdOfsPlugin::truncate");
    return m_next_sfs->truncate(path, fsize, eInfo, client, opaque);
}

XrdVERSIONINFO(XrdSfsGetFileSystem2, MyXrdOfsPlugin);

extern "C" {
XrdSfsFileSystem *XrdSfsGetFileSystem2(XrdSfsFileSystem *nativeFS, XrdSysLogger *Logger, const char *configFn,
                                       XrdOucEnv *envP) {
    ofs = new MyXrdOfsPlugin(nativeFS, Logger, configFn, envP);
    return ofs;
}
}
