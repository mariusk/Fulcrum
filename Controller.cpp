#include "BTC.h"
#include "Controller.h"

Controller::Controller(const std::shared_ptr<Options> &o)
    : Mgr(nullptr), options(o)
{
    setObjectName("Controller");
    _thread.setObjectName(objectName());
}

Controller::~Controller() { Debug(__FUNCTION__); cleanup(); }

auto Controller::stats() const -> Stats
{
    auto st = srvmgr->statsSafe();
    Util::updateMap(st, bitcoindmgr->statsSafe());
    return st;
}

void Controller::startup()
{
    bitcoindmgr = std::make_unique<BitcoinDMgr>(options->bitcoind.first, options->bitcoind.second, options->rpcuser, options->rpcpassword);
    {
        // some setup code that waits for bitcoind to be ready before kicking off our "process" method
        auto waitForBitcoinD = [this] {
            auto constexpr waitTimer = "wait4bitcoind", callProcessTimer = "callProcess";
            int constexpr msgPeriod = 10000, // 10sec
                          smallDelay = 100;
            stopTimer(callProcessTimer);
            callOnTimerSoon(msgPeriod, waitTimer, []{ Log("Waiting for bitcoind..."); return true; }, false, Qt::TimerType::VeryCoarseTimer);
            // connection to kick off our 'process' method once the first auth is received
            auto connPtr = std::make_shared<QMetaObject::Connection>();
            *connPtr = connect(bitcoindmgr.get(), &BitcoinDMgr::gotFirstGoodConnection, this, [this, connPtr](quint64 id) mutable {
                if (connPtr) {
                    stopTimer(waitTimer);
                    if (!disconnect(*connPtr)) Fatal() << "Failed to disconnect 'authenticated' signal! FIXME!"; // this should never happen but if it does, app quits.
                    connPtr.reset(); // clear connPtr right away to 1. delte it asap and 2. so we are guaranteed not to reenter this block for this connection should there be a spurious signal emitted.
                    Debug() << "Auth recvd from bicoind with id: " << id << ", proceeding with processing ...";
                    callOnTimerSoonNoRepeat(smallDelay, callProcessTimer, [this]{process();}, true);
                }
            });
        };
        waitForBitcoinD();
        conns += connect(bitcoindmgr.get(), &BitcoinDMgr::allConnectionsLost, this, waitForBitcoinD);
    }

    bitcoindmgr->startup(); // may throw

    srvmgr = std::make_unique<SrvMgr>(options->interfaces, nullptr /* we are not parent so it won't follow us to our thread*/);
    srvmgr->startup(); // may throw Exception, waits for servers to bind

    start();  // start our thread
}

void Controller::cleanup()
{
    stop();
    if (srvmgr) { Log("Stopping SrvMgr ... "); srvmgr->cleanup(); srvmgr.reset(); }
    if (bitcoindmgr) { Log("Stopping BitcoinDMgr ... "); bitcoindmgr->cleanup(); bitcoindmgr.reset(); }
    sm.reset();
}



struct Controller::StateMachine
{
    enum State {
        Begin, GetBlockHashes, End
    };
    State state = Begin;
    int bl = 0;
    int ht = -1;
    int cur = 0;
    Util::VoidFunc AGAIN;
    std::vector<QByteArray> blockHashes;
};

void Controller::process()
{
    Debug() << "Process called...";

    // TESTING...
    using S = StateMachine::State;
    if (!sm) {
        sm = std::make_unique<StateMachine>(); // create statemachine if doest not exist
        sm->AGAIN = [this]{ QTimer::singleShot(0, this, [this]{process();});};
    }
    static const auto BOILERPLATE_ERR =  [](const RPC::Message & resp){
        Warning() << resp.method << ": error response: " << resp.toJsonString();
    };
    static const auto BOILERPLATE_FAIL = [](auto id, auto msg) {
        Warning() << id.toString() << ": FAIL: " << msg;
    };
    if (sm->state == S::Begin) {
        bitcoindmgr->submitRequest(this, IdMixin::newId(), "getblockcount", {}, [this](const RPC::Message & resp){ // testing
            QVariant var = resp.result();
            Trace() << resp.method << ": result reply: " << var.toInt();
            if (var.canConvert<int>()) {
                sm->ht = var.toInt();
                sm->state = S::GetBlockHashes;
                while (sm->cur < BitcoinDMgr::N_CLIENTS) { // fixme: should be ngoodclients
                    sm->AGAIN();
                    ++sm->cur;
                }
            } else {
                Warning() << resp.method << ": response not int!";
            }
        }, BOILERPLATE_ERR, BOILERPLATE_FAIL);
    } else if (sm->state == S::GetBlockHashes) {
        if (sm->bl >= sm->ht) {
            sm->state = S::End;
            sm->cur = 0;
            sm->AGAIN();
        }
        unsigned bnum = unsigned(sm->bl);
        bitcoindmgr->submitRequest(this, IdMixin::newId(), "getblockhash", {sm->bl++}, [this, bnum](const RPC::Message & resp){ // testing
            sm->cur = qMax(0, sm->cur-1); // decrement current q ct
            QVariant var = resp.result();
            auto res = QByteArray::fromHex(var.toByteArray());
            if (var.canConvert<QByteArray>() && res.length() == bitcoin::uint256::width()) {
                if (bnum && !(bnum % 1000)) {
                    Log("Processed block %u", bnum);
                }
                Trace() << resp.method << ": result for height: " << bnum << " len: " << res.length();
                if (sm->blockHashes.size() < bnum+1)
                    sm->blockHashes.resize(bnum+1);
                sm->blockHashes[bnum] = res;
                while (sm->cur < BitcoinDMgr::N_CLIENTS) { // fixme: should be ngoodclients
                    sm->AGAIN();
                    ++sm->cur;
                }
            } else {
                Warning() << resp.method << ": at height " << bnum << " response not valid (decoded size: " << res.length() << ")";
            }
        }, BOILERPLATE_ERR, BOILERPLATE_FAIL);
    } else if (sm->state == S::End) {
        Log() << "Downloaded " << sm->blockHashes.size() << " headers";
    }

    /*
    bitcoindmgr->submitRequest(this, IdMixin::newId(), "getmempoolinfo", {}, [](auto resp){ // testing
        Trace() << resp.id.toString() << ": result reply: " << resp.toJsonString();
    }, [](auto resp){
        Trace() << resp.id.toString() << ": error response: " << resp.toJsonString();
    }, [](auto id, auto msg) {
        Warning() << id.toString() << ": FAIL: " << msg;
    });
    */
}
