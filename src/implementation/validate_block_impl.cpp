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
#include <bitcoin/blockchain/implementation/validate_block_impl.hpp>

#include <cstddef>
#include <bitcoin/bitcoin.hpp>
#include <bitcoin/blockchain/checkpoint.hpp>

namespace libbitcoin {
namespace chain {

validate_block_impl::validate_block_impl(db_interface& database,
    size_t fork_index, const block_detail_list& orphan_chain,
    size_t orphan_index, size_t height, const block_type& block,
    const config::checkpoint::list& checks, stopped_callback stopped)
  : validate_block(height, block, checks, stopped),
    interface_(database),
    height_(height),
    fork_index_(fork_index),
    orphan_index_(orphan_index),
    orphan_chain_(orphan_chain)
{
}

block_header_type validate_block_impl::fetch_block(size_t fetch_height) const
{
    if (fetch_height > fork_index_)
    {
        const auto fetch_index = fetch_height - fork_index_ - 1;
        BITCOIN_ASSERT(fetch_index <= orphan_index_);
        BITCOIN_ASSERT(orphan_index_ < orphan_chain_.size());
        return orphan_chain_[fetch_index]->actual().header;
    }

    auto result = interface_.blocks.get(fetch_height);
    BITCOIN_ASSERT(result);
    return result.header();
}

uint32_t validate_block_impl::previous_block_bits() const
{
    // Read block (d - 1) and return bits
    return fetch_block(height_ - 1).bits;
}

validate_block::versions validate_block_impl::preceding_block_versions(
    size_t maximum) const
{
    // 1000 previous versions maximum  sample.
    // 950 previous versions minimum required for enforcement.
    // 750 previous versions minimum required for activation.
    const auto size = std::min(maximum, height_);

    // Read block (d - 1) through (d - 1000) and return version vector.
    versions result;
    for (size_t index = 0; index < size; ++index)
    {
        const auto version = fetch_block(height_ - index - 1).version;

        // Some blocks have high versions, see block #390777.
        static const auto maximum = static_cast<uint32_t>(max_uint8);
        const auto normal = std::min(version, maximum);
        result.push_back(static_cast<uint8_t>(normal));
    }

    return result;
}

uint64_t validate_block_impl::actual_timespan(size_t interval) const
{
    BITCOIN_ASSERT(height_ > 0 && height_ >= interval);

    // height - interval and height - 1, return time difference
    return fetch_block(height_ - 1).timestamp -
        fetch_block(height_ - interval).timestamp;
}

uint64_t validate_block_impl::median_time_past() const
{
    // Read last 11 (or height if height < 11) block times into array.
    std::vector<uint64_t> times;
    const auto count = std::min(height_, (size_t)11);
    for (size_t i = 0; i < count; ++i)
        times.push_back(fetch_block(height_ - i - 1).timestamp);

    // Select median value from the array.
    std::sort(times.begin(), times.end());
    return times.empty() ? 0 : times[times.size() / 2];
}

bool tx_after_fork(size_t tx_height, size_t fork_index)
{
    return tx_height > fork_index;
}

bool validate_block_impl::transaction_exists(const hash_digest& tx_hash) const
{
    const auto result = interface_.transactions.get(tx_hash);
    if (!result)
        return false;

    return !tx_after_fork(result.height(), fork_index_);
}

// bool validate_block_impl::is_output_spent(
//     const output_point& outpoint) const
// {
//     log_info(LOG_BLOCKCHAIN) << "FER - validate_block_impl::is_output_spent(...1 parameters...) - 1";

//     const auto result = interface_.spends.get(outpoint);    //TODO: Fer: [UTXO] reemplazar con UTXO
    
//     log_info(LOG_BLOCKCHAIN) << "FER - validate_block_impl::is_output_spent(...1 parameters...) - 2";

//     if (!result)
//         return false;

//     // Lookup block height. Is the spend after the fork point?
//     auto hash = result.hash();

//     log_info(LOG_BLOCKCHAIN) << "FER - validate_block_impl::is_output_spent(...1 parameters...) - 3";

//     auto res = transaction_exists(hash);

//     log_info(LOG_BLOCKCHAIN) << "FER - validate_block_impl::is_output_spent(...1 parameters...) - 4";

//     return res;
// }

bool validate_block_impl::is_output_spent(
    const output_point& outpoint) const
{
    // log_info(LOG_BLOCKCHAIN) << "FER - validate_block_impl::is_output_spent(...1 parameters...) - 1";

    const auto result = interface_.spends.get(outpoint);        //TODO: Fer: [UTXO] reemplazar con UTXO
    
    // log_info(LOG_BLOCKCHAIN) << "FER - validate_block_impl::is_output_spent(...1 parameters...) - 2";

    if (!result)
        return false;

    // Lookup block height. Is the spend after the fork point?
    return transaction_exists(result.hash());
}

bool validate_block_impl::fetch_transaction(transaction_type& tx,
    size_t& tx_height, const hash_digest& tx_hash) const
{
    const auto result = interface_.transactions.get(tx_hash);
    if (!result || tx_after_fork(result.height(), fork_index_))
        return fetch_orphan_transaction(tx, tx_height, tx_hash);

    tx = result.transaction();
    tx_height = result.height();
    return true;
}

bool validate_block_impl::fetch_orphan_transaction(transaction_type& tx,
    size_t& tx_height, const hash_digest& tx_hash) const
{
    for (size_t orphan = 0; orphan <= orphan_index_; ++orphan)
    {
        const auto& orphan_block = orphan_chain_[orphan]->actual();
        for (const auto& orphan_tx: orphan_block.transactions)
        {
            if (hash_transaction(orphan_tx) == tx_hash)
            {
                tx = orphan_tx;
                tx_height = fork_index_ + orphan + 1;
                return true;
            }
        }
    }
    return false;
}

bool validate_block_impl::is_output_spent(const output_point& previous_output,
    size_t index_in_parent, size_t input_index) const
{

    // log_info(LOG_BLOCKCHAIN) << "FER - validate_block_impl::is_output_spent(...3 parameters...) - 1";

    // Search for double spends. This must be done in both chain AND orphan.
    // Searching chain when this tx is an orphan is redundant but it does not
    // happen enough to care.
    if (is_output_spent(previous_output)) {
        // log_info(LOG_BLOCKCHAIN) << "FER - validate_block_impl::is_output_spent(...3 parameters...) - 2";
        return true;
    }

    // log_info(LOG_BLOCKCHAIN) << "FER - validate_block_impl::is_output_spent(...3 parameters...) - 3";


    if (orphan_is_spent(previous_output, index_in_parent, input_index)) {
        // log_info(LOG_BLOCKCHAIN) << "FER - validate_block_impl::is_output_spent(...3 parameters...) - 4";
        return true;
    }

    // log_info(LOG_BLOCKCHAIN) << "FER - validate_block_impl::is_output_spent(...3 parameters...) - 5";

    return false;
}

bool validate_block_impl::orphan_is_spent(const output_point& previous_output,
    size_t skip_tx, size_t skip_input) const
{
    for (size_t orphan = 0; orphan <= orphan_index_; ++orphan)
    {
        const auto& orphan_block = orphan_chain_[orphan]->actual();
        const auto& transactions = orphan_block.transactions;

        BITCOIN_ASSERT(!transactions.empty());
        BITCOIN_ASSERT(is_coinbase(transactions.front()));

        for (size_t tx_index = 0; tx_index < transactions.size();
            ++tx_index)
        {
            // TODO: too deep, move this section to subfunction.
            const auto& orphan_tx = transactions[tx_index];
            for (size_t input_index = 0; input_index < orphan_tx.inputs.size();
                ++input_index)
            {
                const auto& orphan_input = orphan_tx.inputs[input_index];
                if (orphan == orphan_index_ && tx_index == skip_tx &&
                    input_index == skip_input)
                    continue;

                if (orphan_input.previous_output == previous_output)
                    return true;
            }
        }
    }

    return false;
}

} // namespace chain
} // namespace libbitcoin
