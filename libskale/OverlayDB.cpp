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
 * @file OverlayDB.cpp
 * @author Dmytro Stebaiev
 * @date 2018
 */

#include "OverlayDB.h"

#include <thread>

using std::string;
using std::unordered_map;
using std::vector;

#include <libdevcore/Common.h>
#include <libdevcore/db.h>

//#include "SHA3.h"

using dev::bytes;
using dev::bytesConstRef;
using dev::h160;
using dev::h256;
using dev::u256;
using dev::db::Slice;

#ifndef DEV_GUARDED_DB
#define DEV_GUARDED_DB 0
#endif

namespace skale {
namespace {
inline Slice toSlice( h256 const& _h ) {
    return Slice( reinterpret_cast< char const* >( _h.data() ), _h.size );
}

// inline Slice toSlice( std::string const& _str ) { // l_sergiy: clang did detected this as unused
//    return Slice( _str.data(), _str.size() );
//}

inline Slice toSlice( bytes const& _b ) {
    return Slice( reinterpret_cast< char const* >( &_b[0] ), _b.size() );
}

inline Slice toSlice( h160 const& _h ) {
    return Slice( reinterpret_cast< char const* >( _h.data() ), _h.size );
}

inline Slice toSlice( std::string const& _s ) {
    return Slice( reinterpret_cast< char const* >( &_s[0] ), _s.size() );
}

// inline Slice toSlice( bytesConstRef _h ) { // l_sergiy: clang did detected this as unused
//    return Slice( reinterpret_cast< char const* >( _h.data() ), _h.size() );
//}

}  // namespace

OverlayDB::OverlayDB( std::unique_ptr< dev::db::DatabaseFace > _db )
    : m_db( _db.release(), []( dev::db::DatabaseFace* db ) {
          // clog(dev::VerbosityDebug, "overlaydb") << "Closing state DB";
          //        std::cerr << "!!! Closing state DB !!!" << std::endl;
          //        std::cerr.flush();
          delete db;
      } ) {}

void OverlayDB::commit() {
    if ( m_db ) {
        for ( unsigned commitTry = 0; commitTry < 10; ++commitTry ) {
            auto writeBatch = m_db->createWriteBatch();
//      cnote << "Committing nodes to disk DB:";
#if DEV_GUARDED_DB
            DEV_READ_GUARDED( x_this )
#endif
            {
                for ( auto const& addressValuePair : m_cache ) {
                    h160 const& address = addressValuePair.first;
                    bytes const& value = addressValuePair.second;
                    writeBatch->insert( toSlice( address ), toSlice( value ) );
                }
                for ( auto const& addressSpacePair : m_auxiliaryCache ) {
                    h160 const& address = addressSpacePair.first;
                    unordered_map< _byte_, bytes > const& spaces = addressSpacePair.second;
                    for ( auto const& spaceValuePair : spaces ) {
                        _byte_ space = spaceValuePair.first;
                        bytes const& value = spaceValuePair.second;

                        writeBatch->insert(
                            toSlice( getAuxiliaryKey( address, space ) ), toSlice( value ) );
                    }
                }
                for ( auto const& addressStoragePair : m_storageCache ) {
                    h160 const& address = addressStoragePair.first;
                    unordered_map< h256, h256 > const& storage = addressStoragePair.second;
                    for ( auto const& stateAddressValuePair : storage ) {
                        h256 const& storageAddress = stateAddressValuePair.first;
                        h256 const& value = stateAddressValuePair.second;

                        writeBatch->insert(
                            toSlice( getStorageKey( address, storageAddress ) ), toSlice( value ) );
                    }
                }
                writeBatch->insert( toSlice( "storageUsed" ), toSlice( storageUsed_.str() ) );
                writeBatch->insert(
                    toSlice( "lastTransactionHash" ), toSlice( lastTransactionHash_ ) );
            }
            try {
                m_db->commit( std::move( writeBatch ) );
                break;
            } catch ( boost::exception const& ex ) {
                if ( commitTry == 9 ) {
                    cwarn << "Fail writing to state database. Bombing out.";
                    exit( -1 );
                }
                std::cerr << "Error writing to state database: "
                          << boost::diagnostic_information( ex ) << std::endl;
                cwarn << "Error writing to state database: " << boost::diagnostic_information( ex );
                cwarn << "Sleeping for" << ( commitTry + 1 ) << "seconds, then retrying.";
                std::this_thread::sleep_for( std::chrono::seconds( commitTry + 1 ) );
            }
        }
#if DEV_GUARDED_DB
        DEV_WRITE_GUARDED( x_this )
#endif
        {
            m_cache.clear();
            m_auxiliaryCache.clear();
            m_storageCache.clear();
        }
    }
}

string OverlayDB::lookupAuxiliary( h160 const& _address, _byte_ _space ) const {
    string value;
    auto addressSpacePairPtr = m_auxiliaryCache.find( _address );
    if ( addressSpacePairPtr != m_auxiliaryCache.end() ) {
        auto spaceValuePtr = addressSpacePairPtr->second.find( _space );
        if ( spaceValuePtr != addressSpacePairPtr->second.end() ) {
            value = string( spaceValuePtr->second.begin(), spaceValuePtr->second.end() );
        }
    }
    if ( !value.empty() || !m_db )
        return value;

    std::string const loadedValue = m_db->lookup( toSlice( getAuxiliaryKey( _address, _space ) ) );
    if ( loadedValue.empty() )
        cwarn << "Aux not found: " << _address;

    return loadedValue;
}

void OverlayDB::killAuxiliary( const dev::h160& _address, _byte_ _space ) {
    bool cache_hit = false;
    auto spaces_ptr = m_auxiliaryCache.find( _address );
    if ( spaces_ptr != m_auxiliaryCache.end() ) {
        auto value_ptr = spaces_ptr->second.find( _space );
        if ( value_ptr != spaces_ptr->second.end() ) {
            cache_hit = true;
            spaces_ptr->second.erase( value_ptr );
            if ( spaces_ptr->second.empty() ) {
                m_auxiliaryCache.erase( spaces_ptr );
            }
        }
    }
    if ( !cache_hit ) {
        if ( m_db ) {
            Slice key = toSlice( getAuxiliaryKey( _address, _space ) );
            if ( m_db->exists( key ) ) {
                m_db->kill( key );
            } else {
                cnote << "Try to delete non existing key " << _address << "(" << _space << ")";
            }
        }
    }
}

void OverlayDB::insertAuxiliary(
    const dev::h160& _address, dev::bytesConstRef _value, _byte_ _space ) {
    auto address_ptr = m_auxiliaryCache.find( _address );
    if ( address_ptr != m_auxiliaryCache.end() ) {
        auto space_ptr = address_ptr->second.find( _space );
        if ( space_ptr != address_ptr->second.end() ) {
            space_ptr->second = _value.toBytes();
        } else {
            address_ptr->second[_space] = _value.toBytes();
        }
    } else {
        m_auxiliaryCache[_address][_space] = _value.toBytes();
    }
}

std::unordered_map< h160, string > OverlayDB::accounts() const {
    unordered_map< h160, string > accounts;
    if ( m_db ) {
        m_db->forEach( [&accounts]( Slice key, Slice value ) {
            if ( key.size() == h160::size ) {
                // key is account address
                string keyString( key.begin(), key.end() );
                h160 address = h160( keyString, h160::ConstructFromStringType::FromBinary );
                accounts[address] = string( value.begin(), value.end() );
            }
            return true;
        } );
    } else {
        cerror << "Try to load account but connection to database is not established";
    }
    return accounts;
}

std::unordered_map< u256, u256 > OverlayDB::storage( const dev::h160& _address ) const {
    unordered_map< u256, u256 > storage;
    if ( m_db ) {
        m_db->forEach( [&storage, &_address]( Slice key, Slice value ) {
            if ( key.size() == h160::size + h256::size ) {
                // key is storage address
                string keyString( key.begin(), key.end() );
                h160 address = h160(
                    keyString.substr( 0, h160::size ), h160::ConstructFromStringType::FromBinary );
                if ( address == _address ) {
                    h256 memoryAddress = h256(
                        keyString.substr( h160::size ), h256::ConstructFromStringType::FromBinary );
                    u256 memoryValue = h256( string( value.begin(), value.end() ),
                        h256::ConstructFromStringType::FromBinary );
                    storage[memoryAddress] = memoryValue;
                }
            }
            return true;
        } );
    } else {
        cerror << "Try to load account's storage but connection to database is not established";
    }
    return storage;
}

void OverlayDB::rollback() {
#if DEV_GUARDED_DB
    WriteGuard l( x_this );
#endif
    m_cache.clear();
    m_auxiliaryCache.clear();
    m_storageCache.clear();
}

void OverlayDB::clearDB() {
    if ( m_db ) {
        vector< Slice > keys;
        m_db->forEach( [&keys]( Slice key, Slice ) {
            keys.push_back( key );
            return true;
        } );
        for ( const auto& key : keys ) {
            m_db->kill( key );
        }
    }
}

bool OverlayDB::connected() const {
    return m_db != nullptr;
}

bool OverlayDB::empty() const {
    if ( m_db ) {
        bool empty = true;
        m_db->forEach( [&empty]( Slice, Slice ) {
            empty = false;
            return false;
        } );
        return empty;
    } else {
        return true;
    }
}

dev::bytes OverlayDB::getAuxiliaryKey( dev::h160 const& _address, _byte_ space ) const {
    bytes key = _address.asBytes();
    key.push_back( space );  // for aux
    return key;
}

dev::bytes OverlayDB::getStorageKey(
    dev::h160 const& _address, dev::h256 const& _storageAddress ) const {
    bytes key = _address.asBytes();
    bytes storageAddress = _storageAddress.asBytes();
    key.insert( key.end(), storageAddress.begin(), storageAddress.end() );
    return key;
}

string OverlayDB::lookup( h160 const& _h ) const {
    string ret;
    auto p = m_cache.find( _h );
    if ( p != m_cache.end() ) {
        ret = string( p->second.begin(), p->second.end() );
    }
    if ( !ret.empty() || !m_db )
        return ret;

    return m_db->lookup( toSlice( _h ) );
}

bool OverlayDB::exists( h160 const& _h ) const {
    if ( m_cache.find( _h ) != m_cache.end() )
        return true;
    return m_db && m_db->exists( toSlice( _h ) );
}

void OverlayDB::kill( h160 const& _h ) {
    auto p = m_cache.find( _h );
    if ( p != m_cache.end() ) {
        m_cache.erase( p );
    } else {
        if ( m_db ) {
            Slice key = toSlice( _h );
            if ( m_db->exists( key ) ) {
                m_db->kill( key );
            } else {
                cnote << "Try to delete non existing key " << _h;
            }
        }
    }
}

void OverlayDB::insert( const dev::h160& _address, dev::bytesConstRef _value ) {
    auto it = m_cache.find( _address );
    if ( it != m_cache.end() ) {
        it->second = _value.toBytes();
    } else
        m_cache[_address] = _value.toBytes();
}

h256 OverlayDB::lookup( const dev::h160& _address, const dev::h256& _storageAddress ) const {
    auto address_ptr = m_storageCache.find( _address );
    if ( address_ptr != m_storageCache.end() ) {
        auto storage_ptr = address_ptr->second.find( _storageAddress );
        if ( storage_ptr != address_ptr->second.end() ) {
            return storage_ptr->second;
        }
    }

    if ( m_db ) {
        string value = m_db->lookup( toSlice( getStorageKey( _address, _storageAddress ) ) );
        return h256( value, h256::ConstructFromStringType::FromBinary );
    } else {
        return h256( 0 );
    }
}

void OverlayDB::insert(
    const dev::h160& _address, const dev::h256& _storageAddress, dev::h256 const& _value ) {
    auto address_ptr = m_storageCache.find( _address );
    if ( address_ptr != m_storageCache.end() ) {
        auto storage_ptr = address_ptr->second.find( _storageAddress );
        if ( storage_ptr != address_ptr->second.end() ) {
            storage_ptr->second = _value;
        } else {
            address_ptr->second[_storageAddress] = _value;
        }
    } else {
        m_storageCache[_address][_storageAddress] = _value;
    }
}

// TODO maybe set and return same thing?!
dev::s256 OverlayDB::storageUsed() const {
    if ( m_db ) {
        return dev::s256( m_db->lookup( toSlice( "storageUsed" ) ) );
    }
    return 0;
}

void OverlayDB::updateStorageUsage( dev::s256 const& _storageUsed ) {
    storageUsed_ = _storageUsed;
}

dev::h256 OverlayDB::lastTransactionHash() const {
    if ( m_db ) {
        std::string s = m_db->lookup( toSlice( "lastTransactionHash" ) );
        return h256( s, h256::ConstructFromStringType::FromBinary );
    }
    return dev::h256();
}
void OverlayDB::setLastTransactionHash( dev::h256 const& _h ) {
    lastTransactionHash_ = _h;
}


}  // namespace skale
