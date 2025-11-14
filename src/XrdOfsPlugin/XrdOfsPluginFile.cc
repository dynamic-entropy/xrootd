#include "XrdOfsPlugin.hh"
#include <string>

FileWrapper::~FileWrapper() { m_log.Say("INFO:", "FileWrapper::~FileWrapper"); }

int FileWrapper::open(const char *fileName, XrdSfsFileOpenMode openMode, mode_t createMode, const XrdSecEntity *client,
                      const char *opaque) {
    m_log.Say("INFO:", "FileWrapper::open");

    bool verified{false};
    int rc;
    while (!verified) {
        // modify tried field to add more FileWrapper::tried
        rc = m_wrapped->open(fileName, openMode, createMode, client, opaque);
        verified = open_verify();
    }

    m_log.Say("INFO:", "MyXrdOfsPlugin::open: redirecting to ", error.getErrText(), ":",
              std::to_string(error.getErrInfo()).c_str());
    return rc;
}

bool FileWrapper::open_verify(){
    // use xrdfs client to read?
    return true; // on success

    if (tried =="") tried = error.getErrInfo();
    else tried += "," + error.getErrInfo();
    return 0; // on failure

}

int FileWrapper::close() {
    m_log.Say("INFO:", "FileWrapper::close");
    return m_wrapped->close();
}

int FileWrapper::checkpoint(cpAct act, struct iov *range, int n) {
    m_log.Say("INFO:", "FileWrapper::checkpoint");
    return m_wrapped->checkpoint(act, range, n);
}

int FileWrapper::fctl(const int cmd, const char *args, XrdOucErrInfo &out_error) {
    m_log.Say("INFO:", "FileWrapper::fctl");
    return m_wrapped->fctl(cmd, args, out_error);
}

const char *FileWrapper::FName() {
    m_log.Say("INFO:", "FileWrapper::FName");
    return m_wrapped->FName();
}

int FileWrapper::getMmap(void **Addr, off_t &Size) {
    m_log.Say("INFO:", "FileWrapper::getMmap");
    return m_wrapped->getMmap(Addr, Size);
}

XrdSfsXferSize FileWrapper::pgRead(XrdSfsFileOffset offset, char *buffer, XrdSfsXferSize rdlen, uint32_t *csvec,
                                   uint64_t opts) {
    m_log.Say("INFO:", "FileWrapper::pgRead(offset, buffer)");
    return m_wrapped->pgRead(offset, buffer, rdlen, csvec, opts);
}

XrdSfsXferSize FileWrapper::pgRead(XrdSfsAio *aioparm, uint64_t opts) {
    m_log.Say("INFO:", "FileWrapper::pgRead(aioparm)");
    return m_wrapped->pgRead(aioparm, opts);
}

XrdSfsXferSize FileWrapper::pgWrite(XrdSfsFileOffset offset, char *buffer, XrdSfsXferSize rdlen, uint32_t *csvec,
                                    uint64_t opts) {
    m_log.Say("INFO:", "FileWrapper::pgWrite(offset, buffer)");
    return m_wrapped->pgWrite(offset, buffer, rdlen, csvec, opts);
}

XrdSfsXferSize FileWrapper::pgWrite(XrdSfsAio *aioparm, uint64_t opts) {
    m_log.Say("INFO:", "FileWrapper::pgWrite(aioparm)");
    return m_wrapped->pgWrite(aioparm, opts);
}

int FileWrapper::read(XrdSfsFileOffset fileOffset, XrdSfsXferSize amount) {
    m_log.Say("INFO:", "FileWrapper::read(offset, amount)");
    return m_wrapped->read(fileOffset, amount);
}

XrdSfsXferSize FileWrapper::read(XrdSfsFileOffset fileOffset, char *buffer, XrdSfsXferSize buffer_size) {
    m_log.Say("INFO:", "FileWrapper::read(offset, buffer)");
    return m_wrapped->read(fileOffset, buffer, buffer_size);
}

int FileWrapper::read(XrdSfsAio *aioparm) {
    m_log.Say("INFO:", "FileWrapper::read(aioparm)");
    return m_wrapped->read(aioparm);
}

XrdSfsXferSize FileWrapper::write(XrdSfsFileOffset fileOffset, const char *buffer, XrdSfsXferSize buffer_size) {
    m_log.Say("INFO:", "FileWrapper::write(offset, buffer)");
    return m_wrapped->write(fileOffset, buffer, buffer_size);
}

int FileWrapper::write(XrdSfsAio *aioparm) {
    m_log.Say("INFO:", "FileWrapper::write(aioparm)");
    return m_wrapped->write(aioparm);
}

int FileWrapper::sync() {
    m_log.Say("INFO:", "FileWrapper::sync");
    return m_wrapped->sync();
}

int FileWrapper::sync(XrdSfsAio *aiop) {
    m_log.Say("INFO:", "FileWrapper::sync(aiop)");
    return m_wrapped->sync(aiop);
}

int FileWrapper::stat(struct stat *buf) {
    m_log.Say("INFO:", "FileWrapper::stat");
    return m_wrapped->stat(buf);
}

int FileWrapper::truncate(XrdSfsFileOffset fileOffset) {
    m_log.Say("INFO:", "FileWrapper::truncate");
    return m_wrapped->truncate(fileOffset);
}

int FileWrapper::getCXinfo(char cxtype[4], int &cxrsz) {
    m_log.Say("INFO:", "FileWrapper::getCXinfo");
    return m_wrapped->getCXinfo(cxtype, cxrsz);
}

int FileWrapper::SendData(XrdSfsDio *sfDio, XrdSfsFileOffset offset, XrdSfsXferSize size) {
    m_log.Say("INFO:", "FileWrapper::SendData");
    return m_wrapped->SendData(sfDio, offset, size);
}
