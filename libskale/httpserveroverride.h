/*
    Copyright (C) 2018-present, SKALE Labs

    This file is part of skaled.

    skaled is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    skaled is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with skaled.  If not, see <http://www.gnu.org/licenses/>.
*/
/**
 * @file httpserveroverride.h
 * @author Dima Litvinov
 * @date 2018
 */

#ifndef HTTPSERVEROVERRIDE_H
#define HTTPSERVEROVERRIDE_H

#include <stdarg.h>
#include <stdint.h>
#include <sys/types.h>
#if defined( _WIN32 ) && !defined( __CYGWIN__ )
#include <ws2tcpip.h>
#if defined( _MSC_FULL_VER ) && !defined( _SSIZE_T_DEFINED )
#define _SSIZE_T_DEFINED
typedef intptr_t ssize_t;
#endif  // !_SSIZE_T_DEFINED */
#else
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#include <jsonrpccpp/server/abstractserverconnector.h>
#include <microhttpd.h>
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>

#include <skutils/console_colors.h>
#include <skutils/dispatch.h>
#include <skutils/http.h>
#include <skutils/stats.h>
#include <skutils/utils.h>
#include <skutils/ws.h>
#include <json.hpp>

#include <libdevcore/Log.h>
#include <libethereum/ChainParams.h>
#include <libethereum/Interface.h>
#include <libethereum/LogFilter.h>

#include <libweb3jsonrpc/SkaleStatsSite.h>

class SkaleStatsSubscriptionManager;
struct SkaleServerConnectionsTrackHelper;
class SkaleWsPeer;
class SkaleRelayWS;
class SkaleServerOverride;

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

class SkaleStatsSubscriptionManager {
public:
    typedef int64_t subscription_id_t;

protected:
    typedef skutils::multithreading::recursive_mutex_type mutex_type;
    typedef std::lock_guard< mutex_type > lock_type;
    mutex_type mtx_;

    std::atomic< subscription_id_t > next_subscription_;
    subscription_id_t nextSubscriptionID();

    struct subscription_data_t {
        subscription_id_t m_idSubscription = 0;
        skutils::retain_release_ptr< SkaleWsPeer > m_pPeer;
        size_t m_nIntervalMilliseconds = 0;
        skutils::dispatch::job_id_t m_idDispatchJob;
    };  /// struct subscription_data_t
    typedef std::map< subscription_id_t, subscription_data_t > map_subscriptions_t;
    map_subscriptions_t map_subscriptions_;

public:
    SkaleStatsSubscriptionManager();
    virtual ~SkaleStatsSubscriptionManager();
    bool subscribe(
        subscription_id_t& idSubscription, SkaleWsPeer* pPeer, size_t nIntervalMilliseconds );
    bool unsubscribe( const subscription_id_t& idSubscription );
    void unsubscribeAll();
    virtual SkaleServerOverride& getSSO() = 0;
};  // class SkaleStatsSubscriptionManager

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

struct SkaleServerConnectionsTrackHelper {
    SkaleServerOverride& m_sso;
    SkaleServerConnectionsTrackHelper( SkaleServerOverride& sso );
    ~SkaleServerConnectionsTrackHelper();
};  /// struct SkaleServerConnectionsTrackHelper

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

class SkaleWsPeer : public skutils::ws::peer {
public:
    std::atomic_size_t nTaskNumberInPeer_ = 0;
    const std::string m_strPeerQueueID;
    std::unique_ptr< SkaleServerConnectionsTrackHelper > m_pSSCTH;
    SkaleWsPeer( skutils::ws::server& srv, const skutils::ws::hdl_t& hdl );
    ~SkaleWsPeer() override;
    void onPeerRegister() override;
    void onPeerUnregister() override;  // peer will no longer receive onMessage after call to this
    void onMessage( const std::string& msg, skutils::ws::opcv eOpCode ) override;
    void onClose( const std::string& reason, int local_close_code,
        const std::string& local_close_code_as_str ) override;
    void onFail() override;
    void onLogMessage(
        skutils::ws::e_ws_log_message_type_t eWSLMT, const std::string& msg ) override;

    std::string desc( bool isColored = true ) const {
        return getShortPeerDescription( isColored, false, false );
    }
    SkaleRelayWS& getRelay();
    const SkaleRelayWS& getRelay() const { return const_cast< SkaleWsPeer* >( this )->getRelay(); }
    SkaleServerOverride* pso();
    const SkaleServerOverride* pso() const { return const_cast< SkaleWsPeer* >( this )->pso(); }
    dev::eth::Interface* ethereum() const;

protected:
    typedef std::set< unsigned > set_watche_ids_t;
    set_watche_ids_t setInstalledWatchesLogs_, setInstalledWatchesNewPendingTransactions_,
        setInstalledWatchesNewBlocks_;
    void uninstallAllWatches();

public:
    bool handleRequestWithBinaryAnswer( const nlohmann::json& joRequest );

    bool handleWebSocketSpecificRequest(
        const nlohmann::json& joRequest, std::string& strResponse );
    bool handleWebSocketSpecificRequest(
        const nlohmann::json& joRequest, nlohmann::json& joResponse );

protected:
    typedef void ( SkaleWsPeer::*rpc_method_t )(
        const nlohmann::json& joRequest, nlohmann::json& joResponse );
    typedef std::map< std::string, rpc_method_t > ws_rpc_map_t;
    static const ws_rpc_map_t g_ws_rpc_map;

    void eth_subscribe( const nlohmann::json& joRequest, nlohmann::json& joResponse );
    void eth_subscribe_logs( const nlohmann::json& joRequest, nlohmann::json& joResponse );
    void eth_subscribe_newPendingTransactions(
        const nlohmann::json& joRequest, nlohmann::json& joResponse );
    void eth_subscribe_newHeads(
        const nlohmann::json& joRequest, nlohmann::json& joResponse, bool bIncludeTransactions );
    void eth_subscribe_skaleStats( const nlohmann::json& joRequest, nlohmann::json& joResponse );
    void eth_unsubscribe( const nlohmann::json& joRequest, nlohmann::json& joResponse );

public:
    friend class SkaleRelayWS;
};  /// class SkaleWsPeer

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

class SkaleServerHelper {
protected:
    int m_nServerIndex;

public:
    SkaleServerHelper( int nServerIndex = -1 ) : m_nServerIndex( nServerIndex ) {}
    virtual ~SkaleServerHelper() {}
    int serverIndex() const { return m_nServerIndex; }
};  /// class SkaleServerHelper

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

class SkaleRelayWS : public skutils::ws::server, public SkaleServerHelper {
protected:
    std::atomic_bool m_isRunning = false;
    std::atomic_bool m_isInLoop = false;
    int ipVer_;
    std::string strBindAddr_, strInterfaceName_;
    std::string m_strScheme_;
    std::string m_strSchemeUC;
    int m_nPort = -1;
    SkaleServerOverride* m_pSO = nullptr;

public:
    typedef skutils::multithreading::recursive_mutex_type mutex_type;
    typedef std::lock_guard< mutex_type > lock_type;
    typedef skutils::retain_release_ptr< SkaleWsPeer > skale_peer_ptr_t;
    typedef std::map< std::string, skale_peer_ptr_t > map_skale_peers_t;  // maps m_strPeerQueueID
                                                                          // -> skale peer pointer

protected:
    mutable mutex_type m_mtxAllPeers;
    mutable map_skale_peers_t m_mapAllPeers;

public:
    SkaleRelayWS( int ipVer, const char* strBindAddr, const char* strScheme,  // "ws" or "wss"
        int nPort, int nServerIndex = -1 );
    ~SkaleRelayWS() override;
    void run( skutils::ws::fn_continue_status_flag_t fnContinueStatusFlag );
    bool isRunning() const { return m_isRunning; }
    bool isInLoop() const { return m_isInLoop; }
    void waitWhileInLoop();
    bool start( SkaleServerOverride* pSO );
    void stop();
    SkaleServerOverride* pso() { return m_pSO; }
    const SkaleServerOverride* pso() const { return m_pSO; }
    dev::eth::Interface* ethereum() const;
    mutex_type& mtxAllPeers() const { return m_mtxAllPeers; }

    std::string nfoGetScheme() const { return m_strScheme_; }
    std::string nfoGetSchemeUC() const { return m_strSchemeUC; }

    friend class SkaleWsPeer;
};  /// class SkaleRelayWS

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

class SkaleRelayHTTP : public SkaleServerHelper {
protected:
    SkaleServerOverride* m_pSO = nullptr;

public:
    int ipVer_;
    std::string strBindAddr_;
    int nPort_;
    const bool m_bHelperIsSSL : 1;
    std::shared_ptr< skutils::http::server > m_pServer;
    SkaleRelayHTTP( SkaleServerOverride* pSO, int ipVer, const char* strBindAddr, int nPort,
        const char* cert_path = nullptr, const char* private_key_path = nullptr,
        int nServerIndex = -1,
        size_t a_max_http_handler_queues = __SKUTILS_HTTP_DEFAULT_MAX_PARALLEL_QUEUES_COUNT__,
        bool is_async_http_transfer_mode = true );
    ~SkaleRelayHTTP() override;
    SkaleServerOverride* pso() { return m_pSO; }
    const SkaleServerOverride* pso() const { return m_pSO; }
    bool handleHttpSpecificRequest(
        const std::string& strOrigin, const std::string& strRequest, std::string& strResponse );
    bool handleHttpSpecificRequest(
        const std::string& strOrigin, const nlohmann::json& joRequest, nlohmann::json& joResponse );

protected:
    typedef void ( SkaleRelayHTTP::*rpc_method_t )(
        const std::string& strOrigin, const nlohmann::json& joRequest, nlohmann::json& joResponse );
    typedef std::map< std::string, rpc_method_t > http_rpc_map_t;
    static const http_rpc_map_t g_http_rpc_map;
};  /// class SkaleRelayHTTP

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

class SkaleServerOverride : public jsonrpc::AbstractServerConnector,
                            public SkaleStatsSubscriptionManager,
                            public dev::rpc::SkaleStatsProviderImpl {
    std::atomic_size_t nTaskNumberCall_ = 9;
    size_t m_cntServers;
    mutable dev::eth::Interface* pEth_;
    dev::eth::ChainParams& chainParams_;

public:
    typedef std::function< std::vector< uint8_t >( const nlohmann::json& joRequest ) >
        fn_binary_snapshot_download_t;

private:
    fn_binary_snapshot_download_t fn_binary_snapshot_download_;

public:
    const double lfExecutionDurationMaxForPerformanceWarning_;                 // in seconds
    static const double g_lfDefaultExecutionDurationMaxForPerformanceWarning;  // in seconds,
                                                                               // default 1 second

    SkaleServerOverride( dev::eth::ChainParams& chainParams,
        fn_binary_snapshot_download_t fn_binary_snapshot_download, size_t cntServers,
        dev::eth::Interface* pEth, const std::string& strAddrHTTP4, int nBasePortHTTP4,
        const std::string& strAddrHTTP6, int nBasePortHTTP6, const std::string& strAddrHTTPS4,
        int nBasePortHTTPS4, const std::string& strAddrHTTPS6, int nBasePortHTTPS6,
        const std::string& strAddrWS4, int nBasePortWS4, const std::string& strAddrWS6,
        int nBasePortWS6, const std::string& strAddrWSS4, int nBasePortWSS4,
        const std::string& strAddrWSS6, int nBasePortWSS6, const std::string& strPathSslKey,
        const std::string& strPathSslCert,
        double lfExecutionDurationMaxForPerformanceWarning  // in seconds
    );
    ~SkaleServerOverride() override;

    dev::eth::Interface* ethereum() const;
    dev::eth::ChainParams& chainParams();
    const dev::eth::ChainParams& chainParams() const;
    bool checkAdminOriginAllowed( const std::string& origin ) const;

private:
    bool implStartListening( std::shared_ptr< SkaleRelayHTTP >& pSrv, int ipVer,
        const std::string& strAddr, int nPort, const std::string& strPathSslKey,
        const std::string& strPathSslCert, int nServerIndex,
        size_t a_max_http_handler_queues = __SKUTILS_HTTP_DEFAULT_MAX_PARALLEL_QUEUES_COUNT__,
        bool is_async_http_transfer_mode = true );
    bool implStartListening( std::shared_ptr< SkaleRelayWS >& pSrv, int ipVer,
        const std::string& strAddr, int nPort, const std::string& strPathSslKey,
        const std::string& strPathSslCert, int nServerIndex );
    bool implStopListening( std::shared_ptr< SkaleRelayHTTP >& pSrv, int ipVer, bool bIsSSL );
    bool implStopListening( std::shared_ptr< SkaleRelayWS >& pSrv, int ipVer, bool bIsSSL );

public:
    size_t max_http_handler_queues_ = __SKUTILS_HTTP_DEFAULT_MAX_PARALLEL_QUEUES_COUNT__;
    bool is_async_http_transfer_mode_ = true;
    virtual bool StartListening() override;
    virtual bool StopListening() override;

    void SetUrlHandler( const std::string& url, jsonrpc::IClientConnectionHandler* handler );

    void logPerformanceWarning( double lfExecutionDuration, int ipVer, const char* strProtocol,
        int nServerIndex, const char* strOrigin, const char* strMethod, nlohmann::json joID );
    void logTraceServerEvent( bool isError, int ipVer, const char* strProtocol, int nServerIndex,
        const std::string& strMessage );
    void logTraceServerTraffic( bool isRX, bool isError, int ipVer, const char* strProtocol,
        int nServerIndex, const char* strOrigin, const std::string& strPayload );

private:
    const std::string m_strAddrHTTP4;
    const int m_nBasePortHTTP4;
    const std::string m_strAddrHTTP6;
    const int m_nBasePortHTTP6;
    const std::string m_strAddrHTTPS4;
    const int m_nBasePortHTTPS4;
    const std::string m_strAddrHTTPS6;
    const int m_nBasePortHTTPS6;
    const std::string m_strAddrWS4;
    const int m_nBasePortWS4;
    const std::string m_strAddrWS6;
    const int m_nBasePortWS6;
    const std::string m_strAddrWSS4;
    const int m_nBasePortWSS4;
    const std::string m_strAddrWSS6;
    const int m_nBasePortWSS6;

    std::map< std::string, jsonrpc::IClientConnectionHandler* > urlhandler;
    jsonrpc::IClientConnectionHandler* GetHandler( const std::string& url );

public:
    bool m_bTraceCalls;
    std::atomic_bool m_bShutdownMode = false;

private:
    std::list< std::shared_ptr< SkaleRelayHTTP > > m_serversHTTP4, m_serversHTTP6, m_serversHTTPS4,
        m_serversHTTPS6;
    std::string m_strPathSslKey, m_strPathSslCert;
    std::list< std::shared_ptr< SkaleRelayWS > > m_serversWS4, m_serversWS6, m_serversWSS4,
        m_serversWSS6;

    std::atomic_size_t m_cntConnections;
    std::atomic_size_t m_cntConnectionsMax;  // 0 is unlimited

public:
    // status API, returns running server port or -1 if server is not started
    int getServerPortStatusHTTP( int ipVer ) const;
    int getServerPortStatusHTTPS( int ipVer ) const;
    int getServerPortStatusWS( int ipVer ) const;
    int getServerPortStatusWSS( int ipVer ) const;

    bool is_connection_limit_overflow() const;
    void connection_counter_inc();
    void connection_counter_dec();
    size_t max_connection_get() const;
    void max_connection_set( size_t cntConnectionsMax );
    virtual void on_connection_overflow_peer_closed(
        int ipVer, const char* strProtocol, int nServerIndex, int nPort );

    SkaleServerOverride& getSSO() override;       // abstract in SkaleStatsSubscriptionManager
    nlohmann::json provideSkaleStats() override;  // abstract from dev::rpc::SkaleStatsProviderImpl

    bool handleRequestWithBinaryAnswer(
        const nlohmann::json& joRequest, std::vector< uint8_t >& buffer );
    bool handleAdminOriginFilter( const std::string& strMethod, const std::string& strOriginURL );

    bool isShutdownMode() const { return m_bShutdownMode; }

    bool handleProtocolSpecificRequest( SkaleServerHelper& sse, const std::string& strOrigin,
        const nlohmann::json& joRequest, nlohmann::json& joResponse );

protected:
    typedef void ( SkaleServerOverride::*rpc_method_t )( SkaleServerHelper& sse,
        const std::string& strOrigin, const nlohmann::json& joRequest, nlohmann::json& joResponse );
    typedef std::map< std::string, rpc_method_t > protocol_rpc_map_t;
    static const protocol_rpc_map_t g_protocol_rpc_map;

    void setSchainExitTime( SkaleServerHelper& sse, const std::string& strOrigin,
        const nlohmann::json& joRequest, nlohmann::json& joResponse );

    friend class SkaleRelayWS;
    friend class SkaleWsPeer;
};  /// class SkaleServerOverride

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#endif  ///(!defined __HTTP_SERVER_OVERRIDE_H)
