//
// Fulcrum - A fast & nimble SPV Server for Bitcoin Cash
// Copyright (C) 2019-2020  Calin A. Culianu <calin.culianu@gmail.com>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program (see LICENSE.txt).  If not, see
// <https://www.gnu.org/licenses/>.
//
#include "SrvMgr.h"

#include "BitcoinD.h"
#include "PeerMgr.h"
#include "Servers.h"
#include "Storage.h"
#include "Util.h"

#include <utility>

SrvMgr::SrvMgr(const std::shared_ptr<const Options> & options,
               const std::shared_ptr<Storage> & s,
               const std::shared_ptr<BitcoinDMgr> & bdm,
               QObject *parent)
    : Mgr(parent), options(options), storage(s), bitcoindmgr(bdm)
{
}

SrvMgr::~SrvMgr()
{
    Debug() << __FUNCTION__ ;
    cleanup();
}

// will throw Exception on error from within TcpServer::tryStart
void SrvMgr::startup()
{
    if (servers.empty()) {
        startServers();
    } else {
        Error() << __PRETTY_FUNCTION__ << " called with servers already active! FIXME!";
    }
}

void SrvMgr::cleanup()
{
    peermgr.reset(); // unique_ptr, kill peermgr (if any)
    servers.clear(); // unique_ptrs auto-delete all servers
}

// throw Exception on error
void SrvMgr::startServers()
{
    if (options->peerDiscovery) {
        Log() << "SrvMgr: starting PeerMgr ...";
        peermgr = std::make_unique<PeerMgr>(storage, options);
        peermgr->startup(); // may throw
        connect(this, &SrvMgr::allServersStarted, peermgr.get(), &PeerMgr::on_allServersStarted);
    } else peermgr.reset();

    const auto num = options->interfaces.length() + options->sslInterfaces.length() + options->adminInterfaces.length();
    Log() << "SrvMgr: starting " << num << " " << Util::Pluralize("service", num) << " ...";
    const auto firstSsl = options->interfaces.size();
    int i = 0;
    for (const auto & iface : options->interfaces + options->sslInterfaces) {
        if (i < firstSsl)
            // TCP
            servers.emplace_back(std::make_unique<Server>(iface.first, iface.second, options, storage, bitcoindmgr));
        else
            // SSL
            servers.emplace_back(std::make_unique<ServerSSL>(iface.first, iface.second, options, storage, bitcoindmgr));
        Server *srv = servers.back().get();

        // connect blockchain.headers.subscribe signal
        connect(this, &SrvMgr::newHeader, srv, &Server::newHeader);
        // track client lifecycles for per-ip-address connection limits and other stuff
        connect(srv, &ServerBase::clientConnected, this, &SrvMgr::clientConnected);
        connect(srv, &ServerBase::clientDisconnected, this, &SrvMgr::clientDisconnected);
        // if srv receives this message, it will delete the client then we will get a signal back that it is now gone
        connect(this, &SrvMgr::clientExceedsConnectionLimit, srv, qOverload<IdMixin::Id>(&ServerBase::killClient));
        // tally tx broadcasts (lifetime)
        connect(srv, &Server::broadcastTxSuccess, this, [this](unsigned bytes){ ++numTxBroadcasts; txBroadcastBytesTotal += bytes; });

        if (peermgr) {
            connect(srv, &ServerBase::gotRpcAddPeer, peermgr.get(), &PeerMgr::on_rpcAddPeer);
            connect(peermgr.get(), &PeerMgr::updated, srv, &ServerBase::onPeersUpdated);
        }

        srv->tryStart();
        ++i;
    }
    // next do admin RPC, if any
    for (const auto & iface : options->adminInterfaces) {
        adminServers.emplace_back( std::make_unique<AdminServer>(this, iface.first, iface.second, options, storage, bitcoindmgr) );
        AdminServer *asrv = adminServers.back().get();
        if (peermgr) {
            connect(asrv, &ServerBase::gotRpcAddPeer, peermgr.get(), &PeerMgr::on_rpcAddPeer);
            connect(peermgr.get(), &PeerMgr::updated, asrv, &ServerBase::onPeersUpdated);
        }
        asrv->tryStart();
    }

    emit allServersStarted();
}

void SrvMgr::clientConnected(IdMixin::Id cid, const QHostAddress &addr)
{
    addrIdMap.insertMulti(addr, cid);
    const auto maxPerIP = options->maxClientsPerIP;
    if (addrIdMap.count(addr) > maxPerIP) {
        // the below ends up linearly searching through excluded subnets --  this branch is only really taken if the
        // limit is hit .. O(N) where N is probably very small should hopefully be fast enough
        if (Options::Subnet matched; ! options->isAddrInPerIPLimitExcludeSet(addr, &matched) ) {
            Log() << "Connection limit (" << maxPerIP << ") exceeded for " << addr.toString()
                  << ", connection refused for client " << cid;
            emit clientExceedsConnectionLimit(cid);
        } else {
            Debug() << "Client " << cid << " from " << addr.toString() << " would have exceeded the connection limit ("
                    << maxPerIP << ") but its IP matches subnet " << matched.toString() << " from 'subnets_to_exclude_from_per_ip_limits'";
        }
    }
}

void SrvMgr::clientDisconnected(IdMixin::Id cid, const QHostAddress &addr)
{
    if (auto count = addrIdMap.remove(addr, cid); count > 1) {
        Warning() << "Multiple clients with id: " << cid << ", address " << addr.toString() << " in addrIdMap in " << __func__ << " -- FIXME!";
    } else if (count) {
        //Debug() << "Client id " << cid << " addr " << addr.toString() << " removed from addrIdMap";
        if (const auto size = addrIdMap.size(); size >= 64 && size * 2 <= addrIdMap.capacity()) {
            // save space if we are over 2x capacity vs size
            addrIdMap.squeeze();
        }
    }
}

auto SrvMgr::stats() const -> Stats
{
    QVariantMap m;
    m["donationAddress"] = options->donationAddress;
    m["bannerFile"] = options->bannerFile.toUtf8(); // so we get a nice 'null' if not specified
    QVariantMap serversMap;
    const int timeout = kDefaultTimeout / qMax(int(servers.size()), 1);
    for (const auto & server : servers)
        serversMap.unite( server->statsSafe(timeout).toMap() );
    for (const auto & server : adminServers)
        serversMap.unite( server->statsSafe(timeout).toMap() );
    m["Servers"] = serversMap;
    m["PeerMgr"] = peermgr ? peermgr->statsSafe(kDefaultTimeout/2) : QVariant();
    m["transactions sent"] = qulonglong(numTxBroadcasts.load());
    m["transactions sent (bytes)"] = qulonglong(txBroadcastBytesTotal.load());
    m["number of clients"] = qulonglong(Client::numClients.load());
    m["number of clients (max lifetime)"] = qulonglong(Client::numClientsMax.load());
    m["number of clients (total lifetime connections)"] = qulonglong(Client::numClientsCtr.load());
    return m;
}

QVariantList SrvMgr::adminRPC_getClients_blocking(int timeout_ms) const
{
    return Util::LambdaOnObject<QVariantList>(this, [this, timeout_ms] {
        const bool infiniteTimeout = timeout_ms <= 0;
        const auto nClients = std::max(int(servers.size()), 1);
        const int timeoutPerServer = infiniteTimeout ? -1 : std::min(timeout_ms / nClients, 1);
        QVariantList ret;
        for (const auto & server : servers)
            ret.push_back( server->statsSafe(timeoutPerServer) );
        return ret;
    }, timeout_ms);
}
