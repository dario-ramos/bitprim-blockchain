/**
 * Copyright (c) 2011-2015 Bitprim developers (see AUTHORS)
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
#include <bitcoin/blockchain/database/utxo_database.hpp>

#include <boost/filesystem.hpp>

namespace bitprim {
namespace chain {

constexpr size_t number_buckets = 228110589;
BC_CONSTEXPR size_t header_size = htdb_record_header_fsize(number_buckets);
BC_CONSTEXPR size_t initial_map_file_size = header_size + min_records_fsize;

BC_CONSTEXPR position_type allocator_offset = header_size;
BC_CONSTEXPR size_t value_size = hash_size + 4;
BC_CONSTEXPR size_t record_size = record_fsize_htdb<hash_digest>(value_size);

// Create a new hash from a hash + index (a point)
// deterministically suitable for use in a hashtable.
// This technique could be replaced by simply using the output.hash.
static hash_digest output_to_hash(const output_point& output)
{
    data_chunk point(sizeof(output.index) + sizeof(output.hash));
    const auto index = to_little_endian(output.index);
    std::copy(output.hash.begin(), output.hash.end(), point.begin());
    std::copy(index.begin(), index.end(), point.begin() + sizeof(output.hash));

    // The index has a *very* low level of bit distribution evenness, 
    // almost none, and we must preserve the presumed random bit distribution,
    // so we need to re-hash here.
    return sha256_hash(point);
}

utxo_result::utxo_result(const record_type record)
  : record_(record)
{
}

utxo_result::operator bool() const
{
    return record_ != nullptr;
}

hash_digest utxo_result::hash() const
{
    BITCOIN_ASSERT(record_ != nullptr);
    hash_digest result;
    std::copy(record_, record_ + hash_size, result.begin());
    return result;
}

uint32_t utxo_result::index() const
{
    BITCOIN_ASSERT(record_ != nullptr);
    return from_little_endian_unsafe<uint32_t>(record_ + hash_size);
}

utxo_database::utxo_database(const boost::filesystem::path& filename)
  : file_(filename), 
    header_(file_, 0),
    allocator_(file_, allocator_offset, record_size),
    map_(header_, allocator_, filename.string())
{
    BITCOIN_ASSERT(file_.data() != nullptr);
}

void utxo_database::create()
{
    file_.resize(initial_map_file_size);
    header_.create(number_buckets);
    allocator_.create();
}

void utxo_database::start()
{
    header_.start();
    allocator_.start();
}

utxo_result utxo_database::get(const output_point& outpoint) const
{
    const auto key = output_to_hash(outpoint);
    const auto record = map_.get(key);
    return utxo_result(record);
}

void utxo_database::store(const output_point& outpoint,
    const input_point& utxo)
{
    const auto write = [&utxo](uint8_t* data)
    {
        auto serial = make_serializer(data);
        serial.write_data(utxo.hash);
        serial.write_4_bytes(utxo.index);
    };

    const auto key = output_to_hash(outpoint);
    map_.store(key, write);
}

void utxo_database::remove(const output_point& outpoint)
{
    const auto key = output_to_hash(outpoint);
    DEBUG_ONLY(bool success =) map_.unlink(key);
    BITCOIN_ASSERT(success);
}

void utxo_database::sync()
{
    allocator_.sync();
}

utxo_statinfo utxo_database::statinfo() const
{
    return
    {
        header_.size(),
        allocator_.count()
    };
}

} // namespace chain
} // namespace bitprim

