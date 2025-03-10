/*
 *  Copyright(C) 2021 hikyuu.org
 *
 *  Create on: 2021-02-16
 *     Author: fasiondog
 */

#include <csignal>
#include <unordered_set>
#include "hikyuu/utilities/os.h"
#include "hikyuu/utilities/ini_parser/IniParser.h"
#include "hikyuu/utilities/node/NodeClient.h"
#include "hikyuu/global/GlobalSpotAgent.h"
#include "hikyuu/global/schedule/scheduler.h"
#include "hikyuu/global/sysinfo.h"
#include "hikyuu/hikyuu.h"
#include "Strategy.h"

namespace hku {

std::atomic_bool Strategy::ms_keep_running = true;

void Strategy::sig_handler(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        ms_keep_running = false;
        auto* scheduler = getScheduler();
        scheduler->stop();
        exit(0);
    }
}

Strategy::Strategy() : Strategy("Strategy", "") {}

Strategy::Strategy(const string& name, const string& config_file)
: m_name(name), m_config_file(config_file) {
    _initParam();
    if (m_config_file.empty()) {
        string home = getUserDir();
        HKU_ERROR_IF(home == "", "Failed get user home path!");
#if HKU_OS_WINOWS
        m_config_file = format("{}\\{}", home, ".hikyuu\\hikyuu.ini");
#else
        m_config_file = format("{}/{}", home, ".hikyuu/hikyuu.ini");
#endif
    }
}

Strategy::Strategy(const vector<string>& codeList, const vector<KQuery::KType>& ktypeList,
                   const string& name, const string& config_file)
: Strategy(name, config_file) {
    _initParam();
    m_context.setStockCodeList(codeList);
    m_context.setKTypeList(ktypeList);
}

Strategy::Strategy(const StrategyContext& context, const string& name, const string& config_file)
: Strategy(name, config_file) {
    _initParam();
    m_context = context;
}

Strategy::~Strategy() {
    ms_keep_running = false;
    event([]() {});
}

void Strategy::_initParam() {
    setParam<int>("spot_worker_num", 1);
    setParam<string>("quotation_server", string());
}

void Strategy::baseCheckParam(const string& name) const {
    if (name == "spot_worker_num") {
        HKU_ASSERT(getParam<int>(name) > 0);
    }
}

void Strategy::paramChanged() {}

void Strategy::_init() {
    StockManager& sm = StockManager::instance();

    // sm 尚未初始化，则初始化
    if (sm.thread_id() == std::thread::id()) {
        // 注册 ctrl-c 终止信号
        if (!runningInPython()) {
            std::signal(SIGINT, sig_handler);
        }

        CLS_INFO("{} is running! You can press Ctrl-C to terminte ...", m_name);

        // 初始化
        hikyuu_init(m_config_file, false, m_context);

        // 对上下文中的K线类型和其预加载参数进行检查，并给出警告
        auto ktypes = m_context.getKTypeList();
        if (ktypes.empty()) {
            ktypes = KQuery::getAllKType();
        }
        const auto& preload_params = sm.getPreloadParameter();
        for (const auto& ktype : ktypes) {
            std::string low_ktype = ktype;
            to_lower(low_ktype);
            HKU_ERROR_IF(!preload_params.tryGet(low_ktype, false),
                         "The K-line type in the context is not configured to be preloaded!");
        }

    } else {
        m_context = sm.getStrategyContext();
    }

    CLS_CHECK(!m_context.getStockCodeList().empty(), "The context does not contain any stocks!");

    // 先将行情接收代理停止，以便后面加入处理函数
    stopSpotAgent();
}

void Strategy::start(bool autoRecieveSpot) {
    HKU_WARN_IF(
      !m_on_recieved_spot && !m_on_change && !m_run_daily_func && m_run_daily_at_funcs.empty(),
      "No any process function is set!");

    _init();

    _runDailyAt();

    if (autoRecieveSpot) {
        auto& agent = *getGlobalSpotAgent();
        agent.addProcess([this](const SpotRecord& spot) { _receivedSpot(spot); });
        agent.addPostProcess([this](Datetime revTime) {
            if (m_on_recieved_spot) {
                event([this, revTime]() { m_on_recieved_spot(*this, revTime); });
            }
        });
        startSpotAgent(true, getParam<int>("spot_worker_num"),
                       getParam<string>("quotation_server"));
    }

    _runDaily();

    CLS_INFO("start even loop ...");
    _startEventLoop();
}

void Strategy::backtest(std::function<void(const Strategy&)>&& on_bar, const Datetime& start_date,
                        const Datetime& end_date, const KQuery::KType& ktype,
                        const string& ref_market) {
    m_backtesting = true;
    _init();

    auto process = std::move(on_bar);

    try {
        StockManager& sm = StockManager::instance();
        auto dats = sm.getTradingCalendar(KQueryByDate(start_date, end_date, ktype), ref_market);

        m_backtesting_minutes = Minutes(KQuery::getKTypeInMin(ktype));
        auto dates = getDateRange(start_date, end_date);
        for (const auto& date : dates) {
            if (!ms_keep_running) {
                break;
            }
            m_backtesting_now = date;
            process(*this);
        }
    } catch (const std::exception& e) {
        CLS_ERROR("{}", e.what());
    } catch (...) {
        CLS_ERROR("Unknown exception!");
    }

    m_backtesting_now = Null<Datetime>();
    m_backtesting = false;
}

Datetime Strategy::today() const {
    return m_backtesting ? m_backtesting_now.startOfDay() : Datetime::today();
}

Datetime Strategy::now() const {
    return m_backtesting ? m_backtesting_now : Datetime::now();
}

Datetime Strategy::nextDatetime() const {
    return m_backtesting ? m_backtesting_now + m_backtesting_minutes : Null<Datetime>();
}

KData Strategy::getKData(const Stock& stk, const Datetime& start_date, const Datetime& end_date,
                         const KQuery::KType& ktype, KQuery::RecoverType recover_type) const {
    KData ret;
    if (!m_backtesting) {
        ret = stk.getKData(KQueryByDate(start_date, end_date, ktype, recover_type));
        return ret;
    }

    Datetime next_date = m_backtesting_now + m_backtesting_minutes;
    if (end_date.isNull() || end_date > next_date) {
        ret = stk.getKData(KQueryByDate(start_date, next_date, ktype, recover_type));
    } else {
        ret = stk.getKData(KQueryByDate(start_date, end_date, ktype, recover_type));
    }
    return ret;
}

KData Strategy::getLastKData(const Stock& stk, size_t lastnum, const KQuery::KType& ktype,
                             KQuery::RecoverType recover_type) const {
    KData ret;
    KQuery query = KQueryByDate(Datetime::min(), nextDatetime(), ktype);
    size_t out_start = 0, out_end = 0;
    HKU_IF_RETURN(!stk.getIndexRange(query, out_start, out_end), ret);

    int64_t startidx = 0, endidx = 0;
    endidx = out_end;
    int64_t num = static_cast<int64_t>(lastnum);
    startidx = (endidx > num) ? endidx - num : out_start;

    query = KQueryByIndex(startidx, endidx, ktype, recover_type);
    ret = stk.getKData(query);
    return ret;
}

void Strategy::onChange(
  std::function<void(const Strategy&, const Stock&, const SpotRecord& spot)>&& changeFunc) {
    HKU_CHECK(changeFunc, "Invalid changeFunc!");
    m_on_change = std::move(changeFunc);
}

void Strategy::onReceivedSpot(
  std::function<void(const Strategy&, const Datetime&)>&& recievedFucn) {
    HKU_CHECK(recievedFucn, "Invalid recievedFucn!");
    m_on_recieved_spot = std::move(recievedFucn);
}

void Strategy::_receivedSpot(const SpotRecord& spot) {
    Stock stk = getStock(format("{}{}", spot.market, spot.code));
    if (!stk.isNull()) {
        if (m_on_change) {
            event([this, stk, spot]() { m_on_change(*this, stk, spot); });
        }
    }
}

void Strategy::runDaily(std::function<void(const Strategy&)>&& func, const TimeDelta& delta,
                        const std::string& market, bool ignoreMarket) {
    HKU_CHECK(func, "Invalid func!");
    m_run_daily_delta = delta;
    m_run_daily_market = market;
    m_ignoreMarket = ignoreMarket;

    if (ignoreMarket) {
        m_run_daily_func = [this, f = std::move(func)]() { event([this, f]() { f(*this); }); };

    } else {
        m_run_daily_func = [this, f = std::move(func)]() {
            const auto& sm = StockManager::instance();
            auto today = Datetime::today();
            int day = today.dayOfWeek();
            if (day == 0 || day == 6 || sm.isHoliday(today)) {
                return;
            }

            auto market_info = sm.getMarketInfo(m_run_daily_market);
            Datetime open1 = today + market_info.openTime1();
            Datetime close1 = today + market_info.closeTime1();
            Datetime open2 = today + market_info.openTime2();
            Datetime close2 = today + market_info.closeTime2();
            Datetime now = Datetime::now();
            if ((now >= open1 && now <= close1) || (now >= open2 && now <= close2)) {
                event([this, f]() { f(*this); });
            }
        };
    }
}

void Strategy::_runDaily() {
    HKU_IF_RETURN(!m_run_daily_func, void());

    auto* scheduler = getScheduler();
    if (m_ignoreMarket) {
        scheduler->addDurationFunc(std::numeric_limits<int>::max(), m_run_daily_delta,
                                   m_run_daily_func);
        return;
    }

    try {
        const auto& sm = StockManager::instance();
        auto market_info = sm.getMarketInfo(m_run_daily_market);
        auto today = Datetime::today();
        auto now = Datetime::now();
        TimeDelta now_time = now - today;
        if (now_time >= market_info.closeTime2()) {
            scheduler->addFuncAtTime(today.nextDay() + market_info.openTime1(), [this]() {
                m_run_daily_func();
                auto* sched = getScheduler();
                sched->addDurationFunc(std::numeric_limits<int>::max(), m_run_daily_delta,
                                       m_run_daily_func);
            });

        } else if (now_time >= market_info.openTime2()) {
            int64_t ticks = now_time.ticks() - market_info.openTime2().ticks();
            int64_t delta_ticks = m_run_daily_delta.ticks();
            if (ticks % delta_ticks == 0) {
                scheduler->addDurationFunc(std::numeric_limits<int>::max(), m_run_daily_delta,
                                           m_run_daily_func);
            } else {
                auto delay = TimeDelta::fromTicks((ticks / delta_ticks + 1) * delta_ticks - ticks);
                scheduler->addFuncAtTime(now + delay, [this]() {
                    m_run_daily_func();
                    auto* sched = getScheduler();
                    sched->addDurationFunc(std::numeric_limits<int>::max(), m_run_daily_delta,
                                           m_run_daily_func);
                });
            }

        } else if (now_time >= market_info.closeTime1()) {
            scheduler->addFuncAtTime(today + market_info.openTime2(), [this]() {
                m_run_daily_func();
                auto* sched = getScheduler();
                sched->addDurationFunc(std::numeric_limits<int>::max(), m_run_daily_delta,
                                       m_run_daily_func);
            });

        } else if (now_time < market_info.closeTime1() && now_time >= market_info.openTime1()) {
            int64_t ticks = now_time.ticks() - market_info.openTime1().ticks();
            int64_t delta_ticks = m_run_daily_delta.ticks();
            if (ticks % delta_ticks == 0) {
                scheduler->addDurationFunc(std::numeric_limits<int>::max(), m_run_daily_delta,
                                           m_run_daily_func);
            } else {
                auto delay = TimeDelta::fromTicks((ticks / delta_ticks + 1) * delta_ticks - ticks);
                scheduler->addFuncAtTime(now + delay, [this]() {
                    m_run_daily_func();
                    auto* sched = getScheduler();
                    sched->addDurationFunc(std::numeric_limits<int>::max(), m_run_daily_delta,
                                           m_run_daily_func);
                });
            }

        } else if (now_time < market_info.openTime1()) {
            scheduler->addFuncAtTime(today + market_info.openTime1(), [this]() {
                m_run_daily_func();
                auto* sched = getScheduler();
                sched->addDurationFunc(std::numeric_limits<int>::max(), m_run_daily_delta,
                                       m_run_daily_func);
            });

        } else {
            CLS_ERROR("Unknown process! now_time: {}", now_time);
        }
    } catch (const std::exception& e) {
        CLS_THROW("{}", e.what());
    }
}

void Strategy::runDailyAt(std::function<void(const Strategy&)>&& func, const TimeDelta& delta,
                          bool ignoreHoliday) {
    HKU_CHECK(func, "Invalid func!");
    HKU_CHECK(delta < Days(1), "TimeDelta must < Days(1)!");
    HKU_CHECK(m_run_daily_at_funcs.find(delta) == m_run_daily_at_funcs.end(),
              "A task already exists at this point in time!");

    std::function<void()> new_func;
    if (ignoreHoliday) {
        new_func = [this, f = std::move(func)]() {
            const auto& sm = StockManager::instance();
            auto today = Datetime::today();
            int day = today.dayOfWeek();
            if (day != 0 && day != 6 && !sm.isHoliday(today)) {
                event([this, f]() { f(*this); });
            }
        };

    } else {
        new_func = [this, f = std::move(func)]() { event([this, f]() { f(*this); }); };
    }

    m_run_daily_at_funcs[delta] = new_func;
}

void Strategy::_runDailyAt() {
    auto* scheduler = getScheduler();
    for (const auto& [time, func] : m_run_daily_at_funcs) {
        scheduler->addFuncAtTimeEveryDay(time, func);
    }
    m_run_daily_at_funcs.clear();
}

/*
 * 在主线程中处理事件队列，避免 python GIL
 */
void Strategy::_startEventLoop() {
    while (ms_keep_running) {
        event_type task;
        m_event_queue.wait_and_pop(task);
        if (task.isNullTask()) {
            ms_keep_running = false;
        } else {
            try {
                task();
            } catch (const std::exception& e) {
                CLS_ERROR("Failed run task! {}", e.what());
            } catch (...) {
                CLS_ERROR("Failed run task! Unknow error!");
            }
        }
    }
}

void HKU_API runInStrategy(const SYSPtr& sys, const Stock& stk, const KQuery& query,
                           const OrderBrokerPtr& broker, const TradeCostPtr& costfunc,
                           const std::vector<OrderBrokerPtr>& other_brokers) {
    HKU_ASSERT(sys && broker && sys->getTM());
    HKU_ASSERT(!stk.isNull());
    HKU_ASSERT(query != Null<KQuery>());
    HKU_CHECK(!sys->getParam<bool>("buy_delay") && !sys->getParam<bool>("sell_delay"),
              "Thie method only support buy|sell on close!");

    auto tm = crtBrokerTM(broker, costfunc, sys->name(), other_brokers);
    tm->fetchAssetInfoFromBroker(broker);
    sys->setTM(tm);
    sys->setSP(SlippagePtr());  // 清除移滑价差算法
    sys->run(stk, query);
}

void HKU_API runInStrategy(const PFPtr& pf, const KQuery& query, const OrderBrokerPtr& broker,
                           const TradeCostPtr& costfunc,
                           const std::vector<OrderBrokerPtr>& other_brokers) {
    HKU_ASSERT(pf && broker && pf->getTM());
    HKU_ASSERT(query != Null<KQuery>());

    auto se = pf->getSE();
    HKU_ASSERT(se);
    const auto& sys_list = se->getProtoSystemList();
    for (const auto& sys : sys_list) {
        HKU_CHECK(!sys->getSP(), "Exist Slippage part in sys, You must clear it! {}", sys->name());
        HKU_CHECK(!sys->getParam<bool>("buy_delay") && !sys->getParam<bool>("sell_delay"),
                  "Thie method only support buy|sell on close!");
    }

    auto tm = crtBrokerTM(broker, costfunc, pf->name(), other_brokers);
    tm->fetchAssetInfoFromBroker(broker);
    pf->setTM(tm);
    pf->run(query, true);
}

void HKU_API getDataFromBufferServer(const std::string& addr, const StockList& stklist,
                                     const KQuery::KType& ktype) {
    // SPEND_TIME(getDataFromBufferServer);
    const auto& preload = StockManager::instance().getPreloadParameter();
    string low_ktype = ktype;
    to_lower(low_ktype);
    HKU_ERROR_IF_RETURN(!preload.tryGet<bool>(low_ktype, false), void(),
                        "The {} kdata is not preload! Can't update!", low_ktype);

    NodeClient client(addr);
    try {
        HKU_CHECK(client.dial(), "Failed dial server!");
        json req;
        req["cmd"] = "market";
        req["ktype"] = ktype;
        json code_list;
        json date_list;
        for (const auto& stk : stklist) {
            if (!stk.isNull()) {
                code_list.emplace_back(stk.market_code());
                auto k = stk.getKData(KQueryByIndex(-1, Null<int64_t>(), ktype));
                if (k.empty()) {
                    date_list.emplace_back(Datetime::min().str());
                } else {
                    date_list.emplace_back(k[k.size() - 1].datetime.str());
                }
            }
        }
        req["codes"] = std::move(code_list);
        req["dates"] = std::move(date_list);

        json res;
        client.post(req, res);
        HKU_ERROR_IF_RETURN(res["ret"] != NodeErrorCode::SUCCESS, void(),
                            "Recieved error: {}, msg: {}", res["ret"].get<int>(),
                            res["msg"].get<string>());

        const auto& jdata = res["data"];
        // HKU_INFO("{}", to_string(jdata));
        for (auto iter = jdata.cbegin(); iter != jdata.cend(); ++iter) {
            const auto& r = *iter;
            try {
                string market_code = r["code"].get<string>();
                Stock stk = getStock(market_code);
                if (stk.isNull()) {
                    continue;
                }

                const auto& jklist = r["data"];
                for (auto kiter = jklist.cbegin(); kiter != jklist.cend(); ++kiter) {
                    const auto& k = *kiter;
                    KRecord kr(Datetime(k[0].get<string>()), k[1], k[2], k[3], k[4], k[5], k[6]);
                    stk.realtimeUpdate(kr, ktype);
                }

            } catch (const std::exception& e) {
                HKU_ERROR("Failed decode json: {}! {}", to_string(r), e.what());
            }
        }

    } catch (const std::exception& e) {
        HKU_ERROR("Failed get data from buffer server! {}", e.what());
    } catch (...) {
        HKU_ERROR_UNKNOWN;
    }
}

}  // namespace hku