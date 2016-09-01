/**
 * Copyright (c) 2016 Bitprim developers (see AUTHORS)
 *
 * This file is part of Bitprim.
 *
 * Bitprim is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License with
 * additional permissions to the one published by the Free Software
 * Foundation, either version 3 of the License, or (at your option)
 * any later version. For more information see LICENSE.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */
#ifndef BITPRIM_BLOCKCHAIN_UTXO_DATABASE_HPP
#define BITPRIM_BLOCKCHAIN_UTXO_DATABASE_HPP

#include <boost/filesystem.hpp>
#include <bitcoin/bitcoin.hpp>
#include <bitcoin/blockchain/define.hpp>
#include <bitcoin/blockchain/database/htdb_record.hpp>

namespace bitprim {
namespace chain {

class BCB_API utxo_result
{
public:
    utxo_result(const record_type record);

    /**
     * Test whether the result exists, return false otherwise.
     */
    operator bool() const;

    /**
     * Transaction hash for utxo.
     */
    hash_digest hash() const;

    /**
     * Index of input within transaction for utxo.
     */
    uint32_t index() const;

private:
    const record_type record_;
};

struct utxo_statinfo
{
    /// Number of buckets used in the hashtable.
    /// load factor = rows / buckets
    const size_t buckets;
    /// Total number of utxo rows.
    const size_t rows;
};

/**
 * utxo_database enables you to lookup the utxo of an output point,
 * returning the input point. It is a simple map.
 */
class BCB_API utxo_database
{
public:
    utxo_database(const boost::filesystem::path& filename);

    /**
     * Initialize a new utxo database.
     */
    void create();

    /**
     * You must call start() before using the database.
     */
    void start();

    /**
     * Get input utxo of an output point.
     */
    utxo_result get(const output_point& outpoint) const;

    /**
     * Store a utxo in the database.
     */
    void store(const output_point& outpoint, const input_point& spend);

    /**
     * Delete outpoint utxo item from database.
     */
    void remove(const output_point& outpoint);

    /**
     * Synchronise storage with disk so things are consistent.
     * Should be done at the end of every block write.
     */
    void sync();

    /**
     * Return statistical info about the database.
     */
    utxo_statinfo statinfo() const;

private:
    typedef htdb_record<hash_digest> map_type;

    /// The hashtable used for looking up inpoint utxos by outpoint.
    mmfile file_;
    htdb_record_header header_;
    record_allocator allocator_;
    map_type map_;
};

} // namespace chain
} // namespace bitprim

#endif /*BITPRIM_BLOCKCHAIN_UTXO_DATABASE_HPP*/

