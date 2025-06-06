

class XrdXrootdGStream;
class XrdSysLogger;

class XrdXrootdHttpMon {
	struct HttpInfo{
		const char* status;
	};

	public:
	XrdXrootdHttpMon(XrdSysLogger* logP, XrdXrootdGStream& gStream):
	gStream(gStream){

	};

	private:	
		XrdXrootdGStream& gStream;
};