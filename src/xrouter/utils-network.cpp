//******************************************************************************
//******************************************************************************
#include "xrouterutils.h"
#include "xrouterdef.h"
#include "xroutererror.h"
#include "xrouterlogger.h"

#include "rpcserver.h"
#include "rpcprotocol.h"
#include "rpcclient.h"
#include "base58.h"
#include "wallet.h"
#include "init.h"
#include "key.h"
#include "core_io.h"

#include <stdio.h>
#include <cstdio>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <array>
#include <ctime>

#include <boost/asio.hpp>
#include <boost/iostreams/concepts.hpp>
#include <boost/iostreams/stream.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/locale.hpp>
#include <boost/archive/iterators/base64_from_binary.hpp>
#include <boost/archive/iterators/binary_from_base64.hpp>
#include <boost/archive/iterators/insert_linebreaks.hpp>
#include <boost/archive/iterators/transform_width.hpp>
#include <boost/archive/iterators/ostream_iterator.hpp>

using namespace std;
using namespace boost;
using namespace boost::asio;

//*****************************************************************************
//*****************************************************************************
namespace xrouter
{

int readHTTP(std::basic_istream<char>& stream, map<string, string>& mapHeadersRet, string& strMessageRet)
{
    mapHeadersRet.clear();
    strMessageRet = "";

    // Read status
    int nProto = 0;
    int nStatus = ReadHTTPStatus(stream, nProto);

    // Read header
    int nLen = ReadHTTPHeaders(stream, mapHeadersRet);
    if (nLen < 0 || nLen > (int)MAX_SIZE)
        return HTTP_INTERNAL_SERVER_ERROR;

    // Read message
    if (nLen > 0)
    {
        vector<char> vch(nLen);
        stream.read(&vch[0], nLen);
        strMessageRet = string(vch.begin(), vch.end());
    }

    string sConHdr = mapHeadersRet["connection"];

    if ((sConHdr != "close") && (sConHdr != "keep-alive"))
    {
        if (nProto >= 1)
            mapHeadersRet["connection"] = "keep-alive";
        else
            mapHeadersRet["connection"] = "close";
    }

    return nStatus;
}

std::string base64_encode(const std::string& s)
{
    namespace bai = boost::archive::iterators;
    std::string base64_padding[] = {"", "==","="};


    std::stringstream os;

    // convert binary values to base64 characters
    typedef bai::base64_from_binary
    // retrieve 6 bit integers from a sequence of 8 bit bytes
    <bai::transform_width<const char *, 6, 8> > base64_enc; // compose all the above operations in to a new iterator

    std::copy(base64_enc(s.c_str()), base64_enc(s.c_str() + s.size()),
            std::ostream_iterator<char>(os));

    os << base64_padding[s.size() % 3];
    return os.str();
}

std::string CallRPC(const std::string & rpcuser, const std::string & rpcpasswd,
               const std::string & rpcip, const std::string & rpcport,
               const std::string & strMethod, const Array & params)
{
    boost::asio::ip::tcp::iostream stream;
    stream.expires_from_now(boost::posix_time::seconds(GetArg("-rpcxroutertimeout", 60)));
    stream.connect(rpcip, rpcport);
    if (stream.error() != boost::system::errc::success) {
        LogPrint("net", "Failed to make rpc connection to %s:%s error %d: %s", rpcip, rpcport,
                stream.error(), stream.error().message());
        throw runtime_error(strprintf("no response from server %s:%s - %s", rpcip.c_str(), rpcport.c_str(),
                                      stream.error().message().c_str()));
    }

    // HTTP basic authentication
    string strUserPass64 = base64_encode(rpcuser + ":" + rpcpasswd);
    map<string, string> mapRequestHeaders;
    mapRequestHeaders["Authorization"] = string("Basic ") + strUserPass64;

    // Send request
    string strRequest = JSONRPCRequest(strMethod, params, 1);

    if (fDebug)
        LOG() << "HTTP: req  " << strMethod << " " << strRequest;

    string strPost = HTTPPost(strRequest, mapRequestHeaders);
    stream << strPost << std::flush;

    // Receive reply
    map<string, string> mapHeaders;
    string strReply;
    int nStatus = readHTTP(stream, mapHeaders, strReply);

    if (fDebug)
        LOG() << "HTTP: resp " << nStatus << " " << strReply;

    if (nStatus == HTTP_UNAUTHORIZED)
        throw runtime_error("incorrect rpcuser or rpcpassword (authorization failed)");
    else if (nStatus >= 400 && nStatus != HTTP_BAD_REQUEST && nStatus != HTTP_NOT_FOUND && nStatus != HTTP_INTERNAL_SERVER_ERROR)
        throw XRouterError("server returned HTTP error " + std::to_string(nStatus), BAD_REQUEST);
    else if (strReply.empty())
        throw runtime_error("no response from server");

    return strReply;
}

std::string CallURL(const std::string & ip, const std::string & port, const std::string & url)
{
    // Connect to localhost
    boost::asio::ip::tcp::iostream stream;
    stream.expires_from_now(boost::posix_time::seconds(GetArg("-rpcxroutertimeout", 60)));
    stream.connect(ip, port);
    if (stream.error() != boost::system::errc::success) {
        LogPrint("net", "Failed to make connection to %s:%s error %d: %s", ip, port,
                stream.error(), stream.error().message());
        throw runtime_error(strprintf("no response from server %s:%s - %s", ip.c_str(), port.c_str(),
                                      stream.error().message().c_str()));
    }

    // Send request
    ostringstream s;
    s << "GET " << url << "\r\n" << "Host: 127.0.0.1\r\n";
    string strRequest = s.str();

    if (fDebug)
        LOG() << "HTTP: req  " << strRequest;

    stream << strRequest << std::flush;

    // Receive reply
    map<string, string> mapHeaders;
    string strReply;
    int nStatus = readHTTP(stream, mapHeaders, strReply);

    LOG() << "HTTP: resp " << nStatus << " " << strReply;

    if (nStatus >= 400 && nStatus != HTTP_BAD_REQUEST && nStatus != HTTP_NOT_FOUND && nStatus != HTTP_INTERNAL_SERVER_ERROR)
        throw runtime_error("server returned HTTP error " + std::to_string(nStatus));
    else if (strReply.empty())
        throw runtime_error("no response from server");

    return strReply;
}

std::string CallCMD(const std::string & cmd, int & exit) {
    std::array<char, 128> buffer;
    FILE *pipe = popen(std::string(cmd + " 2>&1").c_str(), "r");
    if (!pipe)
        throw std::runtime_error("popen() failed!");

    std::string result;
    while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
        result += buffer.data();
    }

    auto n = pclose(pipe);

#ifdef WIN32
    exit = n & 0xff;
#else
    exit = WEXITSTATUS(n);
#endif

    return result;
}

} // namespace xrouter
