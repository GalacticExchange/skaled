/*
    Copyright (C) 2019-present, SKALE Labs

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
 * @file SnapshotHashAgent.cpp
 * @author Oleh Nikolaiev
 * @date 2019
 */

#include "SnapshotHashAgent.h"
#include "SkaleClient.h"

#include <libconsensus/libBLS/bls/bls.h>
#include <libethcore/CommonJS.h>
#include <libweb3jsonrpc/Skale.h>
#include <skutils/rest_call.h>

#include <jsonrpccpp/client/connectors/httpclient.h>
#include <libff/common/profiling.hpp>

void SnapshotHashAgent::verifyAllData() {
    for ( size_t i = 0; i < this->n_; ++i ) {
        if ( this->chain_params_.nodeInfo.id == this->chain_params_.sChain.nodes[i].id ) {
            continue;
        }

        try {
            libff::inhibit_profiling_info = true;
            if ( !this->bls_->Verification(
                     std::make_shared< std::array< uint8_t, 32 > >( this->hashes_[i].asArray() ),
                     this->signatures_[i], this->public_keys_[i] ) ) {
                throw std::logic_error( " Signature from " + std::to_string( i ) +
                                        "-th node was not verified during "
                                        "getNodesToDownloadSnapshotFrom " );
            }
        } catch ( std::exception& ex ) {
            std::throw_with_nested( std::runtime_error(
                cc::fatal( "FATAL:" ) + " " +
                cc::error( "Exception while verifying common signature from other skaleds: " ) +
                " " + cc::warn( ex.what() ) ) );
        }
    }
}

bool SnapshotHashAgent::voteForHash( std::pair< dev::h256, libff::alt_bn128_G1 >& to_vote ) {
    std::map< dev::h256, size_t > map_hash;

    this->verifyAllData();

    const std::lock_guard< std::mutex > lock( this->hashes_mutex );

    for ( size_t i = 0; i < this->n_; ++i ) {
        if ( this->chain_params_.nodeInfo.id == this->chain_params_.sChain.nodes[i].id ) {
            continue;
        }

        map_hash[this->hashes_[i]] += 1;
    }

    auto it = std::find_if( map_hash.begin(), map_hash.end(),
        [this]( const std::pair< dev::h256, size_t > p ) { return 3 * p.second > 2 * this->n_; } );

    if ( it == map_hash.end() ) {
        throw std::logic_error( "note enough votes to choose hash" );
    } else {
        std::vector< size_t > idx;
        std::vector< libff::alt_bn128_G1 > signatures;
        for ( size_t i = 0; i < this->n_; ++i ) {
            if ( this->chain_params_.nodeInfo.id == this->chain_params_.sChain.nodes[i].id ) {
                continue;
            }

            if ( this->hashes_[i] == ( *it ).first ) {
                this->nodes_to_download_snapshot_from_.push_back( i );
                idx.push_back( i + 1 );
                signatures.push_back( this->signatures_[i] );
            }
        }

        std::vector< libff::alt_bn128_Fr > lagrange_coeffs;
        libff::alt_bn128_G1 common_signature;
        try {
            lagrange_coeffs = this->bls_->LagrangeCoeffs( idx );
            common_signature = this->bls_->SignatureRecover( signatures, lagrange_coeffs );
        } catch ( std::exception& ex ) {
            std::cerr << cc::error(
                             "Exception while recovering common signature from other skaleds: " )
                      << cc::warn( ex.what() ) << "\n";
        }

        libff::alt_bn128_G2 common_public;

        common_public.X.c0 =
            libff::alt_bn128_Fq( chain_params_.nodeInfo.insecureCommonBLSPublicKeys[0].c_str() );
        common_public.X.c1 =
            libff::alt_bn128_Fq( chain_params_.nodeInfo.insecureCommonBLSPublicKeys[1].c_str() );
        common_public.Y.c0 =
            libff::alt_bn128_Fq( chain_params_.nodeInfo.insecureCommonBLSPublicKeys[2].c_str() );
        common_public.Y.c1 =
            libff::alt_bn128_Fq( chain_params_.nodeInfo.insecureCommonBLSPublicKeys[3].c_str() );
        common_public.Z = libff::alt_bn128_Fq2::one();

        try {
            libff::inhibit_profiling_info = true;
            if ( !this->bls_->Verification(
                     std::make_shared< std::array< uint8_t, 32 > >( ( *it ).first.asArray() ),
                     common_signature, common_public ) ) {
                return false;
            }
        } catch ( std::exception& ex ) {
            std::cerr << cc::error(
                             "Exception while verifying common signature from other skaleds: " )
                      << cc::warn( ex.what() ) << "\n";
            return false;
        }

        to_vote.first = ( *it ).first;
        to_vote.second = common_signature;

        return true;
    }
}

std::vector< std::string > SnapshotHashAgent::getNodesToDownloadSnapshotFrom(
    unsigned block_number ) {
    this->bls_.reset( new signatures::Bls( ( 2 * this->n_ + 2 ) / 3, this->n_ ) );
    std::vector< std::thread > threads;
    for ( size_t i = 0; i < this->n_; ++i ) {
        if ( this->chain_params_.nodeInfo.id == this->chain_params_.sChain.nodes[i].id ) {
            continue;
        }

        threads.push_back( std::thread( [this, i, block_number]() {
            try {
                jsonrpc::HttpClient* jsonRpcClient = new jsonrpc::HttpClient(
                    "http://" + this->chain_params_.sChain.nodes[i].ip + ':' +
                    ( this->chain_params_.sChain.nodes[i].port + 3 ).convert_to< std::string >() );
                SkaleClient skaleClient( *jsonRpcClient );

                Json::Value joSignatureResponse =
                    skaleClient.skale_getSnapshotSignature( block_number );

                std::string str_hash = joSignatureResponse["hash"].asString();

                libff::alt_bn128_G1 signature = libff::alt_bn128_G1(
                    libff::alt_bn128_Fq( joSignatureResponse["X"].asCString() ),
                    libff::alt_bn128_Fq( joSignatureResponse["Y"].asCString() ),
                    libff::alt_bn128_Fq::one() );

                Json::Value joPublicKeyResponse = skaleClient.skale_imaInfo();

                libff::alt_bn128_G2 public_key;
                public_key.X.c0 =
                    libff::alt_bn128_Fq( joPublicKeyResponse["insecureBLSPublicKey0"].asCString() );
                public_key.X.c1 =
                    libff::alt_bn128_Fq( joPublicKeyResponse["insecureBLSPublicKey1"].asCString() );
                public_key.Y.c0 =
                    libff::alt_bn128_Fq( joPublicKeyResponse["insecureBLSPublicKey2"].asCString() );
                public_key.Y.c1 =
                    libff::alt_bn128_Fq( joPublicKeyResponse["insecureBLSPublicKey3"].asCString() );
                public_key.Z = libff::alt_bn128_Fq2::one();

                const std::lock_guard< std::mutex > lock( this->hashes_mutex );

                this->hashes_[i] = dev::h256( str_hash );
                this->signatures_[i] = signature;
                this->public_keys_[i] = public_key;

                delete jsonRpcClient;
            } catch ( std::exception& ex ) {
                std::cerr
                    << cc::error(
                           "Exception while collecting snapshot signatures from other skaleds: " )
                    << cc::warn( ex.what() ) << "\n";
            }
        } ) );
    }

    for ( auto& thr : threads ) {
        thr.join();
    }

    bool result = this->voteForHash( this->voted_hash_ );
    if ( !result ) {
        return {};
    }

    std::vector< std::string > ret;
    for ( const size_t idx : this->nodes_to_download_snapshot_from_ ) {
        std::string ret_value =
            std::string( "http://" ) + std::string( this->chain_params_.sChain.nodes[idx].ip ) +
            std::string( ":" ) +
            ( this->chain_params_.sChain.nodes[idx].port + 3 ).convert_to< std::string >();
        ret.push_back( ret_value );
    }

    return ret;
}

std::pair< dev::h256, libff::alt_bn128_G1 > SnapshotHashAgent::getVotedHash() const {
    assert( this->voted_hash_.first != dev::h256() &&
            this->voted_hash_.second != libff::alt_bn128_G1::zero() &&
            this->voted_hash_.second.is_well_formed() );
    return this->voted_hash_;
}
