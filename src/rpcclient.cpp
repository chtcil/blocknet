// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-2014 The Bitcoin developers
// Copyright (c) 2014-2015 The Dash developers
// Copyright (c) 2015-2017 The PIVX developers
// Copyright (c) 2015-2018 The Blocknet developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "rpcclient.h"

#include "rpcprotocol.h"
#include "ui_interface.h"
#include "util.h"

#include "xrouter/xrouterutils.h"

#include <set>
#include <stdint.h>

#include <boost/algorithm/string.hpp>

using namespace std;
using namespace json_spirit;

class CRPCConvertParam
{
public:
    std::string methodName; //! method whose params want conversion
    int paramIdx;           //! 0-based idx of param to convert
};
// ***TODO***
static const CRPCConvertParam vRPCConvertParams[] =
    {
        {"stop", 0},
        {"setmocktime", 0},
        {"getaddednodeinfo", 0},
        {"setgenerate", 0},
        {"setgenerate", 1},
        {"getnetworkhashps", 0},
        {"getnetworkhashps", 1},
        {"disconnectpeer", 0},
        {"sendtoaddress", 1},
        {"sendtoaddress", 4},
        {"sendtoaddressix", 1},
        {"settxfee", 0},
        {"getreceivedbyaddress", 1},
        {"getreceivedbyaccount", 1},
        {"listreceivedbyaddress", 0},
        {"listreceivedbyaddress", 1},
        {"listreceivedbyaddress", 2},
        {"listreceivedbyaccount", 0},
        {"listreceivedbyaccount", 1},
        {"listreceivedbyaccount", 2},
        {"getbalance", 1},
        {"getbalance", 2},
        {"getblockhash", 0},
        {"move", 2},
        {"move", 3},
        {"sendfrom", 2},
        {"sendfrom", 3},
        {"listtransactions", 1},
        {"listtransactions", 2},
        {"listtransactions", 3},
        {"listaccounts", 0},
        {"listaccounts", 1},
        {"walletpassphrase", 1},
        {"walletpassphrase", 2},
        {"getblocktemplate", 0},
        {"listsinceblock", 1},
        {"listsinceblock", 2},
        {"sendmany", 1},
        {"sendmany", 2},
        {"sendmany", 4},
        {"addmultisigaddress", 0},
        {"addmultisigaddress", 1},
        {"createmultisig", 0},
        {"createmultisig", 1},
        {"listunspent", 0},
        {"listunspent", 1},
        {"listunspent", 2},
        {"getblock", 1},
        {"getblockheader", 1},
        {"gettransaction", 1},
        {"getrawtransaction", 1},
        {"createrawtransaction", 0},
        {"createrawtransaction", 1},
        {"signrawtransaction", 1},
        {"signrawtransaction", 2},
        {"sendrawtransaction", 1},
        {"gettxout", 1},
        {"gettxout", 2},
        {"lockunspent", 0},
        {"lockunspent", 1},
        {"importprivkey", 2},
        {"importaddress", 2},
        {"verifychain", 0},
        {"verifychain", 1},
        {"keypoolrefill", 0},
        {"getrawmempool", 0},
        {"estimatefee", 0},
        {"estimatepriority", 0},
        {"prioritisetransaction", 1},
        {"prioritisetransaction", 2},
        {"spork", 1},
        {"mnbudget", 3},
        {"mnbudget", 4},
        {"mnbudget", 6},
        {"mnbudget", 8},
        {"mnbudgetvoteraw", 1},
        {"mnbudgetvoteraw", 4},
        {"reservebalance", 0},
        {"reservebalance", 1},
        {"setstakesplitthreshold", 0},
        {"autocombinerewards", 0},
        {"autocombinerewards", 1},
        {"dxGetOrderHistory", 2},
        {"dxGetOrderHistory", 3},
        {"dxGetOrderHistory", 4},
        {"dxGetOrderHistory", 5},
        {"dxGetOrderHistory", 6},
        {"dxGetOrderHistory", 7},
        {"dxGetOrderHistory", 8},
        {"dxGetOrderBook", 0},
        {"dxGetOrderBook", 3},
        {"dxGetOrderBook", 4},
        {"dxFlushCancelledOrders",0},
        {"dxFlushCancelledOrders",1},
        {"gettradingdata",0},
        {"gettradingdata",1},
        {"xrGetBlockCount",1},
        {"xrGetBlockHash",2},
        {"xrGetBlock",2},
        {"xrGetBlocks",2},
        {"xrGetTransaction",2},
        {"xrGetTransactions",2},
        {"xrDecodeRawTransaction",2},
        {"xrSendTransaction",2},
        {"xrServiceConsensus",0},
        {"xrUpdateConfigs",0},
        {"xrGetBalance",2},
        {"xrGetTxBloomFilter",2},
        {"xrGetTxBloomFilter",3},
        {"xrGetBlockAtTime",1},
        {"xrGetBlockAtTime",2},
        {"xrConnect",1},
    };

class CRPCConvertTable
{
private:
    std::set<std::pair<std::string, int> > members;

public:
    CRPCConvertTable();

    bool convert(const std::string& method, int idx)
    {
        return (members.count(std::make_pair(method, idx)) > 0);
    }
};

CRPCConvertTable::CRPCConvertTable()
{
    const unsigned int n_elem =
        (sizeof(vRPCConvertParams) / sizeof(vRPCConvertParams[0]));

    for (unsigned int i = 0; i < n_elem; i++) {
        members.insert(std::make_pair(vRPCConvertParams[i].methodName,
            vRPCConvertParams[i].paramIdx));
    }
}

static CRPCConvertTable rpcCvtTable;

/** Convert strings to command-specific RPC representation */
Array RPCConvertValues(const std::string& strMethod, const std::vector<std::string>& strParams)
{
    Array params;

    for (unsigned int idx = 0; idx < strParams.size(); idx++) {
        const std::string& strVal = strParams[idx];

        // insert string value directly
        if (!rpcCvtTable.convert(strMethod, idx)) {
            params.push_back(strVal);
        }

        // parse string as JSON, insert bool/number/object/etc. value
        else {
            Value jVal;
            if (!read_string(strVal, jVal)) {
                auto errmsg = strprintf("Failed to parse input parameters: %s, check the docs: help %s", strVal, strMethod);
                if (boost::algorithm::starts_with(strMethod, xrouter::xr)) { // if xrouter call
                    Object error;
                    error.emplace_back("error", errmsg);
                    error.emplace_back("code", xrouter::INVALID_PARAMETERS);
                    throw runtime_error(write_string(Value(error), json_spirit::pretty_print));
                } else
                    throw runtime_error(errmsg);
            }
            params.push_back(jVal);
        }
    }

    return params;
}
