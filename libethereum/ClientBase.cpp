/*
    Modifications Copyright (C) 2018-2019 SKALE Labs

    This file is part of cpp-ethereum.

    cpp-ethereum is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    cpp-ethereum is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with cpp-ethereum.  If not, see <http://www.gnu.org/licenses/>.
 */
/** @file ClientBase.cpp
 * @author Gav Wood <i@gavwood.com>
 * @author Marek Kotewicz <marek@ethdev.com>
 * @date 2015
 */

#include "ClientBase.h"

#include <algorithm>
#include <utility>

#include "BlockChain.h"
#include "Executive.h"

using namespace std;
using namespace dev;
using namespace dev::eth;
using skale::Permanence;
using skale::State;

static const int64_t c_maxGasEstimate = 50000000;

ClientWatch::ClientWatch() : lastPoll( std::chrono::system_clock::now() ) {}

ClientWatch::ClientWatch(
    h256 _id, Reaping _r, fnClientWatchHandlerMulti_t fnOnNewChanges, unsigned iw )
    : id( _id ),
      iw_( iw ),
      fnOnNewChanges_( fnOnNewChanges ),
      lastPoll( ( _r == Reaping::Automatic ) ? std::chrono::system_clock::now() :
                                               std::chrono::system_clock::time_point::max() ) {}

LocalisedLogEntries ClientWatch::get_changes() const {
    return changes_;
}

void ClientWatch::swap_changes( LocalisedLogEntries& otherChanges ) {
    if ( ( ( void* ) &changes_ ) == ( ( void* ) &otherChanges ) )
        return;
    std::swap( changes_, otherChanges );
    if ( !changes_.empty() )
        fnOnNewChanges_( iw_ );
}

void ClientWatch::append_changes( const LocalisedLogEntries& otherChanges ) {
    if ( ( ( void* ) &changes_ ) == ( ( void* ) &otherChanges ) )
        return;
    changes_ += otherChanges;
    if ( !changes_.empty() )
        fnOnNewChanges_( iw_ );
}

void ClientWatch::append_changes( const LocalisedLogEntry& entry ) {
    changes_.push_back( entry );
    if ( !changes_.empty() )
        fnOnNewChanges_( iw_ );
}


std::pair< u256, ExecutionResult > ClientBase::estimateGas( Address const& _from, u256 _value,
    Address _dest, bytes const& _data, int64_t _maxGas, u256 _gasPrice,
    GasEstimationCallback const& _callback ) {
    try {
        int64_t upperBound = _maxGas;
        if ( upperBound == Invalid256 || upperBound > c_maxGasEstimate )
            upperBound = c_maxGasEstimate;
        int64_t lowerBound = Transaction::baseGasRequired( !_dest, &_data, EVMSchedule() );
        Block bk = latestBlock();
        u256 gasPrice = _gasPrice == Invalid256 ? gasBidPrice() : _gasPrice;
        ExecutionResult er;
        ExecutionResult lastGood;
        bool good = false;
        while ( upperBound != lowerBound ) {
            int64_t mid = ( lowerBound + upperBound ) / 2;
            u256 n = bk.transactionsFrom( _from );
            Transaction t;
            if ( _dest )
                t = Transaction( _value, gasPrice, mid, _dest, _data, n );
            else
                t = Transaction( _value, gasPrice, mid, _data, n );
            t.forceSender( _from );
            EnvInfo const env( bk.info(), bc().lastBlockHashes(), 0, mid );
            State& tempState = bk.mutableState();
            tempState.addBalance( _from, ( u256 )( t.gas() * t.gasPrice() + t.value() ) );
            er = tempState.execute( env, *bc().sealEngine(), t, Permanence::Reverted ).first;
            if ( er.excepted == TransactionException::OutOfGas ||
                 er.excepted == TransactionException::OutOfGasBase ||
                 er.excepted == TransactionException::OutOfGasIntrinsic ||
                 er.codeDeposit == CodeDeposit::Failed ||
                 er.excepted == TransactionException::BadJumpDestination ||
                 er.excepted == TransactionException::RevertInstruction )
                lowerBound = lowerBound == mid ? upperBound : mid;
            else {
                lastGood = er;
                upperBound = upperBound == mid ? lowerBound : mid;
                good = true;
            }

            if ( _callback )
                _callback( GasEstimationProgress{lowerBound, upperBound} );
        }
        if ( _callback )
            _callback( GasEstimationProgress{lowerBound, upperBound} );
        return make_pair( upperBound, good ? lastGood : er );
    } catch ( ... ) {
        // TODO: Some sort of notification of failure.
        return make_pair( u256(), ExecutionResult() );
    }
}

ImportResult ClientBase::injectBlock( bytes const& _block ) {
    return bc().attemptImport( _block, preSeal().mutableState() ).first;
}

u256 ClientBase::balanceAt( Address _a ) const {
    return latestBlock().balance( _a );
}

u256 ClientBase::countAt( Address _a ) const {
    return latestBlock().transactionsFrom( _a );
}

u256 ClientBase::stateAt( Address _a, u256 _l ) const {
    return latestBlock().storage( _a, _l );
}

bytes ClientBase::codeAt( Address _a ) const {
    return latestBlock().code( _a );
}

h256 ClientBase::codeHashAt( Address _a ) const {
    return latestBlock().codeHash( _a );
}

map< h256, pair< u256, u256 > > ClientBase::storageAt( Address _a ) const {
    return latestBlock().storage( _a );
}

// TODO: remove try/catch, allow exceptions
LocalisedLogEntries ClientBase::logs( unsigned _watchId ) const {
    LogFilter f;
    try {
        Guard l( x_filtersWatches );
        f = m_filters.at( m_watches.at( _watchId ).id ).filter;
    } catch ( ... ) {
        return LocalisedLogEntries();
    }
    return logs( f );
}

LocalisedLogEntries ClientBase::logs( LogFilter const& _f ) const {
    LocalisedLogEntries ret;
    unsigned begin = min( bc().number() + 1, ( unsigned ) numberFromHash( _f.latest() ) );
    unsigned end = min( bc().number(), min( begin, ( unsigned ) numberFromHash( _f.earliest() ) ) );

    // Handle pending transactions differently as they're not on the block chain.
    if ( begin > bc().number() ) {
        Block temp = postSeal();
        for ( unsigned i = 0; i < temp.pending().size(); ++i ) {
            // Might have a transaction that contains a matching log.
            TransactionReceipt const& tr = temp.receipt( i );
            LogEntries le = _f.matches( tr );
            for ( unsigned j = 0; j < le.size(); ++j )
                ret.insert( ret.begin(), LocalisedLogEntry( le[j] ) );
        }
        begin = bc().number();
    }

    // Handle reverted blocks
    // There are not so many, so let's iterate over them
    h256s blocks;
    h256 ancestor;
    unsigned ancestorIndex;
    tie( blocks, ancestor, ancestorIndex ) = bc().treeRoute( _f.earliest(), _f.latest(), false );

    for ( size_t i = 0; i < ancestorIndex; i++ )
        prependLogsFromBlock( _f, blocks[i], BlockPolarity::Dead, ret );

    // cause end is our earliest block, let's compare it with our ancestor
    // if ancestor is smaller let's move our end to it
    // example:
    //
    // 3b -> 2b -> 1b
    //                -> g
    // 3a -> 2a -> 1a
    //
    // if earliest is at 2a and latest is a 3b, coverting them to numbers
    // will give us pair (2, 3)
    // and we want to get all logs from 1 (ancestor + 1) to 3
    // so we have to move 2a to g + 1
    end = min( end, ( unsigned ) numberFromHash( ancestor ) + 1 );

    // Handle blocks from main chain
    set< unsigned > matchingBlocks;
    if ( !_f.isRangeFilter() )
        for ( auto const& i : _f.bloomPossibilities() )
            for ( auto u : bc().withBlockBloom( i, end, begin ) )
                matchingBlocks.insert( u );
    else
        // if it is a range filter, we want to get all logs from all blocks in given range
        for ( unsigned i = end; i <= begin; i++ )
            matchingBlocks.insert( i );

    for ( auto n : matchingBlocks )
        prependLogsFromBlock( _f, bc().numberHash( n ), BlockPolarity::Live, ret );

    reverse( ret.begin(), ret.end() );
    return ret;
}

void ClientBase::prependLogsFromBlock( LogFilter const& _f, h256 const& _blockHash,
    BlockPolarity _polarity, LocalisedLogEntries& io_logs ) const {
    auto receipts = bc().receipts( _blockHash ).receipts;
    for ( size_t i = 0; i < receipts.size(); i++ ) {
        TransactionReceipt receipt = receipts[i];
        auto th = transaction( _blockHash, i ).sha3();
        LogEntries le = _f.matches( receipt );
        for ( unsigned j = 0; j < le.size(); ++j )
            io_logs.insert( io_logs.begin(),
                LocalisedLogEntry( le[j], _blockHash, ( BlockNumber ) bc().number( _blockHash ), th,
                    i, 0, _polarity ) );
    }
}

unsigned ClientBase::installWatch(
    LogFilter const& _f, Reaping _r, fnClientWatchHandlerMulti_t fnOnNewChanges ) {
    h256 h = _f.sha3();
    {
        Guard l( x_filtersWatches );
        if ( !m_filters.count( h ) ) {
            LOG( m_loggerWatch ) << "FFF" << _f << h;
            m_filters.insert( make_pair( h, _f ) );
        }
    }
    return installWatch( h, _r, fnOnNewChanges );
}

unsigned ClientBase::installWatch(
    h256 _h, Reaping _r, fnClientWatchHandlerMulti_t fnOnNewChanges ) {
    unsigned ret;
    {
        Guard l( x_filtersWatches );
        ret = m_watches.size() ? m_watches.rbegin()->first + 1 : 0;
        m_watches[ret] = ClientWatch( _h, _r, fnOnNewChanges, ret );
        LOG( m_loggerWatch ) << "+++" << ret << _h;
    }
#if INITIAL_STATE_AS_CHANGES
    auto ch = logs( ret );
    if ( ch.empty() )
        ch.push_back( InitialChange );
    {
        Guard l( x_filtersWatches );
        m_watches[ret].swap_changes( ch );
    }
#endif
    return ret;
}

bool ClientBase::uninstallWatch( unsigned _i ) {
    LOG( m_loggerWatch ) << "XXX" << _i;

    Guard l( x_filtersWatches );

    auto it = m_watches.find( _i );
    if ( it == m_watches.end() )
        return false;
    auto id = it->second.id;
    m_watches.erase( it );

    auto fit = m_filters.find( id );
    if ( fit != m_filters.end() )
        if ( !--fit->second.refCount ) {
            LOG( m_loggerWatch ) << "*X*" << fit->first << ":" << fit->second.filter;
            m_filters.erase( fit );
        }
    return true;
}

LocalisedLogEntries ClientBase::peekWatch( unsigned _watchId ) const {
    Guard l( x_filtersWatches );

    //	LOG(m_loggerWatch) << "peekWatch" << _watchId;
    auto& w = m_watches.at( _watchId );
    //	LOG(m_loggerWatch) << "lastPoll updated to " <<
    // chrono::duration_cast<chrono::seconds>(chrono::system_clock::now().time_since_epoch()).count();
    if ( w.lastPoll != chrono::system_clock::time_point::max() )
        w.lastPoll = chrono::system_clock::now();
    return w.get_changes();
}

LocalisedLogEntries ClientBase::checkWatch( unsigned _watchId ) {
    Guard l( x_filtersWatches );
    LocalisedLogEntries ret;

    //	LOG(m_loggerWatch) << "checkWatch" << _watchId;
    auto& w = m_watches.at( _watchId );
    //	LOG(m_loggerWatch) << "lastPoll updated to " <<
    // chrono::duration_cast<chrono::seconds>(chrono::system_clock::now().time_since_epoch()).count();
    w.swap_changes( ret );
    if ( w.lastPoll != chrono::system_clock::time_point::max() )
        w.lastPoll = chrono::system_clock::now();

    return ret;
}

BlockHeader ClientBase::blockInfo( h256 _hash ) const {
    if ( _hash == PendingBlockHash )
        return preSeal().info();
    return BlockHeader( bc().block( _hash ) );
}

BlockDetails ClientBase::blockDetails( h256 _hash ) const {
    return bc().details( _hash );
}

Transaction ClientBase::transaction( h256 _transactionHash ) const {
    return Transaction( bc().transaction( _transactionHash ), CheckTransaction::Cheap );
}

LocalisedTransaction ClientBase::localisedTransaction( h256 const& _transactionHash ) const {
    std::pair< h256, unsigned > tl = bc().transactionLocation( _transactionHash );
    return localisedTransaction( tl.first, tl.second );
}

Transaction ClientBase::transaction( h256 _blockHash, unsigned _i ) const {
    auto bl = bc().block( _blockHash );
    RLP b( bl );
    if ( _i < b[1].itemCount() )
        return Transaction( b[1][_i].data(), CheckTransaction::Cheap );
    else
        return Transaction();
}

LocalisedTransaction ClientBase::localisedTransaction( h256 const& _blockHash, unsigned _i ) const {
    Transaction t = Transaction( bc().transaction( _blockHash, _i ), CheckTransaction::Cheap );
    return LocalisedTransaction( t, _blockHash, _i, numberFromHash( _blockHash ) );
}

TransactionReceipt ClientBase::transactionReceipt( h256 const& _transactionHash ) const {
    return bc().transactionReceipt( _transactionHash );
}

LocalisedTransactionReceipt ClientBase::localisedTransactionReceipt(
    h256 const& _transactionHash ) const {
    std::pair< h256, unsigned > tl = bc().transactionLocation( _transactionHash );
    Transaction t = Transaction( bc().transaction( tl.first, tl.second ), CheckTransaction::Cheap );
    TransactionReceipt tr = bc().transactionReceipt( tl.first, tl.second );
    u256 gasUsed = tr.cumulativeGasUsed();
    if ( tl.second > 0 )
        gasUsed -= bc().transactionReceipt( tl.first, tl.second - 1 ).cumulativeGasUsed();
    return LocalisedTransactionReceipt( tr, t.sha3(), tl.first, numberFromHash( tl.first ),
        tl.second, gasUsed, toAddress( t.from(), t.nonce() ) );
}

pair< h256, unsigned > ClientBase::transactionLocation( h256 const& _transactionHash ) const {
    return bc().transactionLocation( _transactionHash );
}

Transactions ClientBase::transactions( h256 _blockHash ) const {
    auto bl = bc().block( _blockHash );
    RLP b( bl );
    Transactions res;
    for ( unsigned i = 0; i < b[1].itemCount(); i++ )
        res.emplace_back( b[1][i].data(), CheckTransaction::Cheap, true );
    return res;
}

TransactionHashes ClientBase::transactionHashes( h256 _blockHash ) const {
    return bc().transactionHashes( _blockHash );
}

BlockHeader ClientBase::uncle( h256 _blockHash, unsigned _i ) const {
    auto bl = bc().block( _blockHash );
    RLP b( bl );
    if ( _i < b[2].itemCount() )
        return BlockHeader( b[2][_i].data(), HeaderData );
    else
        return BlockHeader();
}

UncleHashes ClientBase::uncleHashes( h256 _blockHash ) const {
    return bc().uncleHashes( _blockHash );
}

unsigned ClientBase::transactionCount( h256 _blockHash ) const {
    auto bl = bc().block( _blockHash );
    RLP b( bl );
    return b[1].itemCount();
}

unsigned ClientBase::uncleCount( h256 _blockHash ) const {
    auto bl = bc().block( _blockHash );
    RLP b( bl );
    return b[2].itemCount();
}

unsigned ClientBase::number() const {
    return bc().number();
}

h256s ClientBase::pendingHashes() const {
    return h256s() + postSeal().pendingHashes();
}

BlockHeader ClientBase::pendingInfo() const {
    return postSeal().info();
}

BlockDetails ClientBase::pendingDetails() const {
    auto pm = postSeal().info();
    auto li = Interface::blockDetails( LatestBlock );
    return BlockDetails(
        ( unsigned ) pm.number(), li.totalDifficulty + pm.difficulty(), pm.parentHash(), h256s{} );
}

u256 ClientBase::gasLimitRemaining() const {
    return postSeal().gasLimitRemaining();
}

Address ClientBase::author() const {
    return preSeal().author();
}

h256 ClientBase::hashFromNumber( BlockNumber _number ) const {
    if ( _number == PendingBlock )
        return h256();
    if ( _number == LatestBlock )
        return bc().currentHash();
    return bc().numberHash( _number );
}

BlockNumber ClientBase::numberFromHash( h256 _blockHash ) const {
    if ( _blockHash == PendingBlockHash )
        return bc().number() + 1;
    else if ( _blockHash == LatestBlockHash )
        return bc().number();
    else if ( _blockHash == EarliestBlockHash )
        return 0;
    return bc().number( _blockHash );
}

int ClientBase::compareBlockHashes( h256 _h1, h256 _h2 ) const {
    BlockNumber n1 = numberFromHash( _h1 );
    BlockNumber n2 = numberFromHash( _h2 );

    if ( n1 > n2 )
        return 1;
    else if ( n1 == n2 )
        return 0;
    return -1;
}

bool ClientBase::isKnown( h256 const& _hash ) const {
    return _hash == PendingBlockHash || _hash == LatestBlockHash || _hash == EarliestBlockHash ||
           bc().isKnown( _hash );
}

bool ClientBase::isKnown( BlockNumber _block ) const {
    if ( _block == PendingBlock )
        return true;
    if ( _block == LatestBlock )
        return true;
    auto a = bc().numberHash( _block );
    auto b = h256();
    if ( a != b )
        return true;
    return false;
}

bool ClientBase::isKnownTransaction( h256 const& _transactionHash ) const {
    return bc().isKnownTransaction( _transactionHash );
}

bool ClientBase::isKnownTransaction( h256 const& _blockHash, unsigned _i ) const {
    bytes block = bc().block( _blockHash );

    if ( block.empty() )
        return false;

    VerifiedBlockRef vb = bc().verifyBlock( &block, function< void( Exception& ) >() );

    return vb.transactions.size() > _i;
}

Block ClientBase::latestBlock() const {
    Block res = postSeal();
    res.startReadState();
    return res;
}

int ClientBase::chainId() const {
    return bc().chainParams().chainID;
}
