/**
 * Copyright (c) 2011-2015 libbitcoin developers (see AUTHORS)
 *
 * This file is part of libbitcoin.
 *
 * libbitcoin is free software: you can redistribute it and/or modify
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
#include <bitcoin/blockchain/implementation/blockchain_impl.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <unordered_map>
#include <boost/filesystem.hpp>
#include <boost/interprocess/sync/file_lock.hpp>
#include <bitcoin/bitcoin.hpp>
#include <bitcoin/blockchain/checkpoint.hpp>
#include <bitcoin/blockchain/implementation/organizer_impl.hpp>
#include <bitcoin/blockchain/implementation/simple_chain_impl.hpp>

#define BC_CHAIN_DATABASE_LOCK_FILE "db-lock"

namespace libbitcoin {
namespace chain {

using namespace boost::interprocess;
using path = boost::filesystem::path;

// This is a protocol limit that we incorporate into the query.
static constexpr size_t maximum_get_blocks = 500;

static file_lock init_lock(const std::string& prefix)
{
    // Touch the lock file (open/close).
    const auto lockfile = path(prefix) / BC_CHAIN_DATABASE_LOCK_FILE;
    bc::ofstream file(lockfile.string(), std::ios::app);
    file.close();
    return file_lock(lockfile.string().c_str());
}

blockchain_impl::blockchain_impl(threadpool& pool, const std::string& prefix,
    const db_active_heights &active_heights, size_t orphan_capacity,
    const config::checkpoint::list& checks)
  : read_strand_(pool),
    write_strand_(pool),
    flock_(init_lock(prefix)),
    seqlock_(0),
    stopped_(true),
    db_paths_(prefix),
    interface_(db_paths_, active_heights),
    orphans_(orphan_capacity),
    chain_(interface_),
    organizer_(pool, interface_, orphans_, chain_, checks)
{
}

bool blockchain_impl::start()
{
    if (!flock_.try_lock())
        return false;

    // TODO: can we actually restart?
    stopped_ = false;
    interface_.start();
    return true;
}

bool blockchain_impl::stop()
{
    // TODO: close all file descriptors, called once threadpool has stopped.
    stopped_ = true;
    organizer_.stop();
    return true;
}

bool blockchain_impl::stopped()
{
    return stopped_;
}

void blockchain_impl::subscribe_reorganize(
    reorganize_handler handle_reorganize)
{
    // Pass this through to the organizer, which issues the notifications.
    organizer_.subscribe_reorganize(handle_reorganize);
}

void blockchain_impl::start_write()
{
    DEBUG_ONLY(const auto lock =) ++seqlock_;

    // seqlock was now odd.
    BITCOIN_ASSERT(lock % 2 == 1);
}

void blockchain_impl::store(std::shared_ptr<block_type> block,
    store_block_handler handle_store)
{
    write_strand_.queue(
        std::bind(&blockchain_impl::do_store,
            this, block, handle_store));
}
void blockchain_impl::do_store(std::shared_ptr<block_type> block,
    store_block_handler handle_store)
{
    if (stopped())
        return;

    start_write();
    const auto& hash = hash_block_header(block->header);
    const auto height = chain_.find_height(hash);
    if (height != simple_chain::null_height)
    {
        const auto info = block_info{ block_status::confirmed, height };
        stop_write(handle_store, error::duplicate, info);
        return;
    }

    const auto detail = std::make_shared<block_detail>(block);
    if (!orphans_.add(detail))
    {
        static constexpr size_t height = 0;
        const auto info = block_info{ block_status::orphan, height };
        stop_write(handle_store, error::duplicate, info);
        return;
    }

    organizer_.start();
    stop_write(handle_store, detail->error(), detail->info());
}

void blockchain_impl::import(std::shared_ptr<block_type> block,
    import_block_handler handle_import)
{
    const auto do_import = [this, block, handle_import]()
    {
        start_write();
        interface_.push(*block);
        stop_write(handle_import, error::success);
    };
    write_strand_.queue(do_import);
}

void blockchain_impl::fetch_parallel(perform_read_functor perform_read)
{
    const auto try_read = [this, perform_read]() -> bool
    {
        // Implements the seqlock counter logic.
        const size_t slock = seqlock_.load();
        return ((slock % 2 != 1) && perform_read(slock));
    };

    const auto do_read = [this, try_read]()
    {
        // Sleeping while waiting for write to complete.
        while (!try_read())
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    };

    // Initiate async read operation.
    read_strand_.async(do_read);
}

void blockchain_impl::fetch_ordered(perform_read_functor perform_read)
{
    const auto try_read = [this, perform_read]() -> bool
    {
        // Implements the seqlock counter logic.
        const size_t slock = seqlock_.load();
        return ((slock % 2 != 1) && perform_read(slock));
    };

    const auto do_read = [this, try_read]()
    {
        // Sleeping while waiting for write to complete.
        while (!try_read())
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    };

    // Initiate async read operation.
    read_strand_.queue(do_read);
}

// This may generally execute 29+ queries.
// TODO: Collect asynchronous calls in a function invoked directly by caller.
void blockchain_impl::fetch_block_locator(
    fetch_handler_block_locator handle_fetch)
{
    const auto do_fetch = [this, handle_fetch](size_t slock)
    {
        block_locator_type locator;
        const auto last_height = interface_.blocks.last_height();
        const auto indexes = block_locator_indexes(last_height);

        for (auto index = indexes.begin(); index != indexes.end(); ++index)
        {
            const auto result = interface_.blocks.get(*index);
            if (!result)
                return finish_fetch(slock, handle_fetch,
                    error::not_found, locator);

            locator.push_back(hash_block_header(result.header()));
        }

        return finish_fetch(slock, handle_fetch,
            error::success, locator);
    };

    fetch_ordered(do_fetch);
}

// This may generally execute 502 but as many as 531+ queries.
// TODO: Collect asynchronous calls in a function invoked directly by caller.
void blockchain_impl::fetch_locator_blocks(const get_blocks_type& locator,
    const hash_digest& threshold, fetch_handler_locator_blocks handle_fetch)
{
    // This is based on the idea that looking up by block hash to get heights
    // will be much faster than hashing each retrieved block to test for stop.
    const auto do_fetch = [this, locator, threshold, handle_fetch](
        size_t slock)
    {
        // Find the first block height.
        // If no start block is on our chain we start with block 0.
        size_t start = 0;
        for (const auto& hash: locator.start_hashes)
        {
            const auto result = interface_.blocks.get(hash);
            if (result)
            {
                start = result.height();
                break;
            }
        }

        // Find the stop block height.
        // The maximum stop block is 501 blocks after start (to return 500).
        size_t stop = start + maximum_get_blocks + 1;
        if (locator.hash_stop != null_hash)
        {
            // If the stop block is not on chain we treat it as a null stop.
            const auto stop_result = interface_.blocks.get(locator.hash_stop);
            if (stop_result)
                stop = std::min(stop_result.height(), stop);
        }

        // Find the threshold block height.
        // If the threshold is above the start it becomes the new start.
        if (threshold != null_hash)
        {
            const auto start_result = interface_.blocks.get(threshold);
            if (start_result)
                start = std::max(start_result.height(), start);
        }

        // This largest portion can be parallelized.
        // Build the hash list until we hit last or the blockchain top.
        hash_list hashes;
        for (size_t index = start + 1; index < stop; ++index)
        {
            const auto result = interface_.blocks.get(index);
            if (!result)
            {
                hashes.push_back(hash_block_header(result.header()));
                break;
            }
        }

        return finish_fetch(slock, handle_fetch,
            error::success, hashes);
    };

    fetch_ordered(do_fetch);
}

// This may generally execute up to 500 queries.
// TODO: Collect asynchronous calls in a function invoked directly by caller.
void blockchain_impl::fetch_missing_block_hashes(const hash_list& hashes,
    fetch_handler_missing_block_hashes handle_fetch)
{
    const auto do_fetch = [this, hashes, handle_fetch](size_t slock)
    {
        hash_list missing;
        for (const auto& hash : hashes)
        {
            const auto result = interface_.blocks.get(hash);
            if (!result)
                missing.push_back(hash);
        }

        return finish_fetch(slock, handle_fetch,
            error::success, missing);
    };
    fetch_ordered(do_fetch);
}

void blockchain_impl::fetch_block_header(uint64_t height,
    fetch_handler_block_header handle_fetch)
{
    const auto do_fetch = [this, height, handle_fetch](size_t slock)
    {
        const auto result = interface_.blocks.get(height);
        if (!result)
            return finish_fetch(slock, handle_fetch,
                error::not_found, block_header_type());

        return finish_fetch(slock, handle_fetch,
            error::success, result.header());
    };
    fetch_parallel(do_fetch);
}

void blockchain_impl::fetch_block_header(const hash_digest& hash,
    fetch_handler_block_header handle_fetch)
{
    const auto do_fetch = [this, hash, handle_fetch](size_t slock)
    {
        const auto result = interface_.blocks.get(hash);
        if (!result)
            return finish_fetch(slock, handle_fetch,
                error::not_found, block_header_type());

        return finish_fetch(slock, handle_fetch,
            error::success, result.header());
    };
    fetch_parallel(do_fetch);
}

void blockchain_impl::fetch_block_transaction_hashes(
    const hash_digest& hash,
    fetch_handler_block_transaction_hashes handle_fetch)
{
    const auto do_fetch = [this, hash, handle_fetch](size_t slock)
    {
        const auto result = interface_.blocks.get(hash);
        if (!result)
            return finish_fetch(slock, handle_fetch,
                error::not_found, hash_list());

        hash_list hashes;
        for (size_t index = 0; index < result.transactions_size(); ++index)
            hashes.push_back(result.transaction_hash(index));

        return finish_fetch(slock, handle_fetch,
            error::success, hashes);
    };
    fetch_parallel(do_fetch);
}

void blockchain_impl::fetch_block_height(const hash_digest& hash,
    fetch_handler_block_height handle_fetch)
{
    const auto do_fetch = [this, hash, handle_fetch](size_t slock)
    {
        const auto result = interface_.blocks.get(hash);
        if (!result)
            return finish_fetch(slock, handle_fetch,
                error::not_found, 0);

        return finish_fetch(slock, handle_fetch,
            error::success, result.height());
    };
    fetch_parallel(do_fetch);
}

void blockchain_impl::fetch_last_height(
    fetch_handler_last_height handle_fetch)
{
    const auto do_fetch = [this, handle_fetch](size_t slock)
    {
        const auto last_height = interface_.blocks.last_height();
        if (last_height == block_database::null_height)
            return finish_fetch(slock, handle_fetch, error::not_found, 0);

        return finish_fetch(slock, handle_fetch,
            error::success, last_height);
    };
    fetch_parallel(do_fetch);
}

void blockchain_impl::fetch_transaction(
    const hash_digest& hash,
    fetch_handler_transaction handle_fetch)
{
    const auto do_fetch = [this, hash, handle_fetch](size_t slock)
    {
        const auto result = interface_.transactions.get(hash);
        if (!result)
            return finish_fetch(slock, handle_fetch,
                error::not_found, transaction_type());

        return finish_fetch(slock, handle_fetch,
            error::success, result.transaction());
    };
    fetch_parallel(do_fetch);
}

void blockchain_impl::fetch_transaction_index(
    const hash_digest& hash,
    fetch_handler_transaction_index handle_fetch)
{
    const auto do_fetch = [this, hash, handle_fetch](size_t slock)
    {
        const auto result = interface_.transactions.get(hash);
        if (!result)
            return finish_fetch(slock, handle_fetch,
                error::not_found, 0, 0);

        return finish_fetch(slock, handle_fetch,
            error::success, result.height(), result.index());
    };
    fetch_parallel(do_fetch);
}

void blockchain_impl::fetch_spend(const output_point& outpoint,
    fetch_handler_spend handle_fetch)
{
    const auto do_fetch = [this, outpoint, handle_fetch](size_t slock)
    {
        const auto result = interface_.spends.get(outpoint);        //TODO: Fer: [UTXO] reemplazar con UTXO
        if (!result)
            return finish_fetch(slock, handle_fetch,
                error::unspent_output, input_point());

        return finish_fetch(slock, handle_fetch,
            error::success, input_point{result.hash(), result.index()});
    };
    fetch_parallel(do_fetch);
}

void blockchain_impl::fetch_history(const payment_address& address,
    fetch_handler_history handle_fetch, const uint64_t limit, 
    const uint64_t from_height)
{
    const auto do_fetch = [this, address, handle_fetch, limit, from_height](
        size_t slock)
    {
        const auto history = interface_.history.get(address.hash(), limit, 
            from_height);
        return finish_fetch(slock, handle_fetch, error::success, history);
    };
    fetch_parallel(do_fetch);
}

void blockchain_impl::fetch_stealth(const binary_type& prefix,
    fetch_handler_stealth handle_fetch, uint64_t from_height)
{
    const auto do_fetch = [this, prefix, handle_fetch, from_height](
        size_t slock)
    {
        const auto stealth = interface_.stealth.scan(prefix, from_height);
        return finish_fetch(slock, handle_fetch, error::success, stealth);
    };
    fetch_parallel(do_fetch);
}

} // namespace chain
} // namespace libbitcoin
