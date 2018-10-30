// Copyright (c) 2018 The Blocknet developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "json/json_spirit_utils.h"
#include "json/json_spirit_value.h"

#include "xseries.h"
#include "xbridge/xbridgetransactiondescr.h"
#include "xbridge/xbridgeapp.h"
#include "sync.h"
#include "xbridge/util/xutil.h"

extern CurrencyPair TxOutToCurrencyPair(const CTxOut & txout, std::string& snode_pubkey);

//******************************************************************************
//******************************************************************************
namespace {
    // Helper functions to filter transactions in a query
    //
    template<class A> bool is_equivalent_pair(const A& a, const xQuery& b) {
        return (a.toCurrency == b.toCurrency.to_string()
                && a.fromCurrency == b.fromCurrency.to_string()) ||
               (a.toCurrency == b.fromCurrency.to_string()
                && a.fromCurrency == b.toCurrency.to_string());
    }
    template<typename T> bool is_too_small(T amount) {
        return ::fabs(amount) < std::numeric_limits<double>::epsilon();
    }
    void transaction_filter(std::vector<CurrencyPair>& matches,
                            const xbridge::TransactionDescr& tr,
                            const xQuery& query)
    {
        if (not(tr.state == xbridge::TransactionDescr::trFinished)) return;
        if (not query.period.contains(tr.txtime)) return;
        if (not is_equivalent_pair(tr, query)) return;
        if (is_too_small(tr.fromAmount) || is_too_small(tr.toAmount)) return;
        matches.emplace_back(query.with_txids == xQuery::WithTxids::Included
                             ? tr.id.GetHex() : xid_t{},
                             ccy::Asset{ccy::Currency{tr.fromCurrency,
                                         xbridge::TransactionDescr::COIN}, tr.fromAmount},
                             ccy::Asset{ccy::Currency{tr.toCurrency,
                                         xbridge::TransactionDescr::COIN},   tr.toAmount},
                             tr.txtime);
    }
    void updateXSeriesHelper(std::vector<xAggregate>& series,
                             const xSeriesCache::xRange& r,
                             const xQuery& q,
                             xQuery::Transform tf)
    {
        for (auto it=r.begin(); it != r.end(); ++it) {
            auto offset = it->timeEnd - q.period.begin();
            size_t idx = (offset.total_seconds() - 1) / q.granularity.total_seconds();
            series.at(idx).update(tf == xQuery::Transform::Invert ? it->inverse() : *it, q.with_txids);
        }
    }
    std::vector<CurrencyPair> find_blockchain_trading_data(time_period query)
    {
        std::vector<CurrencyPair> records;
        {
            LOCK(cs_main);

            CBlockIndex * pindex = chainActive.Tip();
            auto ts = from_time_t(pindex->GetBlockTime());
            while (pindex->pprev != nullptr && query.end() < ts) {
                pindex = pindex->pprev;
                ts = from_time_t(pindex->GetBlockTime());
            }
            for (; pindex->pprev != nullptr && query.contains(ts);
                 pindex = pindex->pprev, ts = from_time_t(pindex->GetBlockTime()))
            {
                CBlock block;
                if (not ReadBlockFromDisk(block, pindex))
                    continue; // throw?
                for (const CTransaction & tx : block.vtx)
                {
                    for (const CTxOut & out : tx.vout)
                    {
                        std::string snode_pubkey{};
                        CurrencyPair p = TxOutToCurrencyPair(out, snode_pubkey);
                        if (p.tag == CurrencyPair::Tag::Valid) {
                            p.timeStamp = ts;
                            records.emplace_back(p);
                        }
                    }
                }
            }
        }
        std::sort(records.begin(), records.end(), // ascending by updated time
                  [](const CurrencyPair& a, const CurrencyPair& b) {
                      return a.timeStamp < b.timeStamp; });

        return records;
    }
    std::vector<CurrencyPair> find_local_trading_data(const xQuery& query)
    {
        auto records = xbridge::App::instance().history_matches(transaction_filter, query);
        std::sort(records.begin(), records.end(), // ascending by updated time
                  [](const CurrencyPair& a, const CurrencyPair& b) {
                      return a.timeStamp < b.timeStamp; });

        return records;
    }

    ptime get_end_time(int64_t end_secs, time_duration cache_granularity) {
        const int64_t psec = cache_granularity.total_seconds();
        if (end_secs < 0 || psec < 1)
            return from_time_t(0);
        return from_time_t(((end_secs + psec - 1) / psec) * psec);
    }
    ptime get_end_time(ptime end_time, time_duration cache_granularity) {
        auto epoch_duration = end_time - from_time_t(0);
        return get_end_time(epoch_duration.total_seconds(), cache_granularity);
    }
}

//******************************************************************************
//******************************************************************************
void xSeriesCache::set_find_blockchain_trades(GetBlockchainTradingDataFunc func)
{
    find_blockchain_trades = func;
}

void xSeriesCache::set_find_local_trades(GetLocalTradingDataFunc func)
{
    find_local_trades = func;
}

xSeriesCache::xSeriesCache()
{
    set_find_blockchain_trades(find_blockchain_trading_data);
    set_find_local_trades(find_local_trading_data);
}

std::vector<xAggregate>
xSeriesCache::getChainXAggregateSeries(const xQuery& query)
{
    xQuery q{query};
    const size_t query_seconds = q.period.length().total_seconds();
    const size_t granularity_seconds = q.granularity.total_seconds();
    const size_t num_intervals = std::min(query_seconds / granularity_seconds,
                                          static_cast<size_t>(q.interval_limit.count()));
    time_duration adjusted_duration = boost::posix_time::seconds{
        static_cast<long>(num_intervals * granularity_seconds)};
    q.period = time_period{q.period.end() - adjusted_duration, q.period.end()};
    std::vector<xAggregate> series{};
    series.resize(num_intervals,xAggregate{q.fromCurrency, q.toCurrency});

    auto t = q.period.begin();
    for (size_t i = 0; i < num_intervals; ++i) {
        t += q.granularity;
        series[i].timeEnd = t;
    }

    if (not m_cache_period.contains(q.period))
        updateSeriesCache(q.period);

    updateXSeries(series, q.fromCurrency, q.toCurrency,
                  q, xQuery::Transform::None);
    if (q.with_inverse == xQuery::WithInverse::Included) {
        updateXSeries(series, q.toCurrency, q.fromCurrency,
                      q, xQuery::Transform::Invert);
    }
    return series;
}

//******************************************************************************
//******************************************************************************
xSeriesCache::xAggregateContainer&
xSeriesCache::getXAggregateContainer(const pairSymbol& key)
{
    auto f = mSparseSeries.find(key);
    if (f == mSparseSeries.end()) {
        mSparseSeries[key] = {};
        f = mSparseSeries.find(key);
    }
    return f->second;
}

//******************************************************************************
//******************************************************************************
void xSeriesCache::updateSeriesCache(const time_period& period)
{
    // TODO(amdn) cached aggregate series for blocks that are purged from the chain
    // need to be invalidated... this code invalidates on every query to
    // ensure results are up-to-date, cache can be enabled when invalidation
    // hook is in place
    LOCK(m_xSeriesCacheUpdateLock);
    std::vector<CurrencyPair> pairs = find_blockchain_trades(period);

    mSparseSeries.clear();
    for (const auto& p : pairs) {
        pairSymbol key = p.to.currency().to_string() +"/"+ p.from.currency().to_string();
        auto& q = getXAggregateContainer(key);
        if (q.empty() || q.back().timeEnd <= p.timeStamp) {
            q.emplace_back(xAggregate{p.from.currency(), p.to.currency()});
            q.back().timeEnd = get_end_time(p.timeStamp,m_cache_granularity);
        }
        q.back().update(p,xQuery::WithTxids::Included);
    }
    m_cache_period = period;
}

//******************************************************************************
//******************************************************************************
xAggregate xAggregate::inverse() const {
    xAggregate x{*this};
    x.open = inverse(open);
    x.high  = inverse(high);
    x.low   = inverse(low);
    x.close = inverse(close);
    x.toVolume = fromVolume;
    x.fromVolume = toVolume;
    return x;
}

void xAggregate::update(const CurrencyPair& x, xQuery::WithTxids with_txids) {
    price_t price{x.price<price_t>()};
    if (open == 0) {
        open = high = low = price;
    }
    high = std::max(high,price);
    low  = std::min(low,price);
    close = price;
    fromVolume += x.from;
    toVolume += x.to;
    if (with_txids == xQuery::WithTxids::Included)
        orderIds.push_back(x.xid());
}

void xAggregate::update(const xAggregate& x, xQuery::WithTxids with_txids) {
    if (open == 0) {
        open = high = low = x.open;
    }
    high = std::max(high,x.high);
    low  = std::min(low,x.low);
    close = x.close;
    fromVolume += x.fromVolume;
    toVolume += x.toVolume;
    if (with_txids == xQuery::WithTxids::Included) {
        for (const auto& it : x.orderIds)
            orderIds.push_back(it);
    }
}

//******************************************************************************
//******************************************************************************
void xSeriesCache::updateXSeries(std::vector<xAggregate>& series,
                                 const ccy::Currency& from,
                                 const ccy::Currency& to,
                                 const xQuery& q,
                                 xQuery::Transform tf)
{
    pairSymbol key = to.to_string() +"/"+ from.to_string();
    auto& xac = getXAggregateContainer(key);
    const auto& range = getXAggregateRange(xac.begin(), xac.end(), q.period);
    updateXSeriesHelper(series, range, q, tf);
}

//******************************************************************************
//******************************************************************************
std::vector<xAggregate> xSeriesCache::getXAggregateSeries(const xQuery& query)
{
    auto local_matches = find_local_trades(query);

    //--Retrieve matching aggregate transactions from blockchain (cached)
    std::vector<xAggregate> series = getChainXAggregateSeries(query);

    //--Update aggregate from blockchain with transactions from local history
    auto it = series.begin();
    auto end = series.end();
    if (it != end) {
        for (const auto& x : local_matches) {
            it = std::lower_bound(it, end, x.timeStamp,
                                  [](const xAggregate& a, const ptime& b) {
                                      return a.timeEnd <= b; });
            it->update(x,query.with_txids);
        }
    }
    return series;
}
