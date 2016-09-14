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
#include <bitcoin/blockchain/validate_block_impl.hpp>

#include <cstddef>
#include <bitcoin/bitcoin.hpp>
#include <bitcoin/blockchain/block_detail.hpp>
#include <bitcoin/blockchain/simple_chain.hpp>

namespace libbitcoin {
namespace blockchain {
    
// Value used to define median time past.
static constexpr size_t median_time_past_blocks = 11;

validate_block_impl::validate_block_impl(simple_chain& chain,
    size_t fork_index, const block_detail::list& orphan_chain,
    size_t orphan_index, size_t height, const chain::block& block,
    bool testnet, const config::checkpoint::list& checks,
    stopped_callback stopped)
  : validate_block(height, block, testnet, checks, stopped),
    chain_(chain),
    height_(height),
    fork_index_(fork_index),
    orphan_index_(orphan_index),
    orphan_chain_(orphan_chain)
{
}

uint32_t validate_block_impl::previous_block_bits() const
{
    // Read block header (top - 1) and return bits
    return fetch_block(height_ - 1).bits;
}

validate_block::versions validate_block_impl::preceding_block_versions(
    size_t maximum) const
{
    // 1000 previous versions maximum sample.
    // 950 previous versions minimum required for enforcement.
    // 750 previous versions minimum required for activation.
    const auto size = std::min(maximum, height_);

    // Read block (top - 1) through (top - 1000) and return version vector.
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

uint64_t validate_block_impl::actual_time_span(size_t interval) const
{
    BITCOIN_ASSERT(height_ > 0 && height_ >= interval);

    // height - interval and height - 1, return time difference
    return fetch_block(height_ - 1).timestamp -
        fetch_block(height_ - interval).timestamp;
}

uint64_t validate_block_impl::median_time_past() const
{
    // Read last 11 (or height if height < 11) block times into array.
    const auto count = std::min(height_, median_time_past_blocks);

    std::vector<uint64_t> times;
    for (size_t i = 0; i < count; ++i)
        times.push_back(fetch_block(height_ - i - 1).timestamp);

    // Sort and select middle (median) value from the array.
    std::sort(times.begin(), times.end());
    return times.empty() ? 0 : times[times.size() / 2];
}

chain::header validate_block_impl::fetch_block(size_t fetch_height) const
{
    if (fetch_height > fork_index_)
    {
        const auto fetch_index = fetch_height - fork_index_ - 1;
        BITCOIN_ASSERT(fetch_index <= orphan_index_);
        BITCOIN_ASSERT(orphan_index_ < orphan_chain_.size());
        return orphan_chain_[fetch_index]->actual()->header;
    }

    chain::header out;
    DEBUG_ONLY(const auto result =) chain_.get_header(out, fetch_height);
    BITCOIN_ASSERT(result);
    return out;
}

bool validate_block_impl::fetch_transaction(chain::transaction& tx,
    size_t& tx_height, const hash_digest& tx_hash) const
{
    uint64_t out_tx_height;
    const auto result = chain_.get_transaction(tx, out_tx_height, tx_hash);

    if (result && out_tx_height <= fork_index_)
    {
        BITCOIN_ASSERT(out_tx_height <= max_size_t);
        tx_height = static_cast<size_t>(out_tx_height);
        return true;
    }

    return fetch_orphan_transaction(tx, tx_height, tx_hash);
}

bool validate_block_impl::fetch_orphan_transaction(chain::transaction& tx,
    size_t& tx_height, const hash_digest& tx_hash) const
{
    for (size_t orphan = 0; orphan <= orphan_index_; ++orphan)
    {
        const auto& orphan_block = orphan_chain_[orphan]->actual();

        for (const auto& orphan_tx: orphan_block->transactions)
        {
            if (orphan_tx.hash() == tx_hash)
            {
                // TRANSACTION COPY
                tx = orphan_tx;
                tx_height = fork_index_ + orphan + 1;
                return true;
            }
        }
    }

    return false;
}

// bool validate_block_impl::is_output_spent(
//     const chain::output_point& outpoint) const
// {
//     return !chain_.contains_outpoint_in_utxo(outpoint);
    
//     // hash_digest out_hash;
//     // const auto result = chain_.get_outpoint_transaction(out_hash, outpoint);
//     // if (!result)
//     //     return false;

//     // // Lookup block height. Is the spend after the fork point?
//     // return transaction_exists(out_hash);
// }

bool validate_block_impl::is_output_spent(
    const chain::output_point& outpoint) const
{
    uint64_t tx_height;
    hash_digest tx_hash;

    auto libbtc_method_1 = false;
    auto libbtc_method_2 = false;
    auto libbtc_method_3 = false;

    libbtc_method_1 = chain_.get_outpoint_transaction(tx_hash, outpoint);
    
    if (libbtc_method_1)
        libbtc_method_2 = chain_.get_transaction_height(tx_height, tx_hash);

    if (libbtc_method_2)
        libbtc_method_3 = tx_height <= fork_index_;

    // return
    //     chain_.get_outpoint_transaction(tx_hash, outpoint) &&
    //     chain_.get_transaction_height(tx_height, tx_hash) &&
    //     tx_height <= fork_index_;


    auto fer_method = !chain_.contains_outpoint_in_utxo(outpoint);

    std::cout << "libbtc_method_1: " << libbtc_method_1 << '\n';
    std::cout << "libbtc_method_2: " << libbtc_method_2 << '\n';
    std::cout << "libbtc_method_3: " << libbtc_method_3 << '\n';
    std::cout << "fer_method:      " << fer_method << '\n';

    return fer_method;

    // return !chain_.contains_outpoint_in_utxo(outpoint);

    // uint64_t tx_height;
    // hash_digest tx_hash;
    // return
    //     chain_.get_outpoint_transaction(tx_hash, outpoint) &&
    //     chain_.get_transaction_height(tx_height, tx_hash) &&
    //     tx_height <= fork_index_;
}

bool validate_block_impl::is_output_spent(
    const chain::output_point& previous_output,
    size_t index_in_parent, size_t input_index) const
{
    // Search for double spends. This must be done in both chain AND orphan.
    if (is_output_spent(previous_output))
        return true;

    if (is_orphan_spent(previous_output, index_in_parent, input_index))
        return true;

    return false;
}

bool validate_block_impl::is_orphan_spent(
    const chain::output_point& previous_output,
    size_t skip_tx, size_t skip_input) const
{
    // This gets costly as the size of the orphan pool increases.
    for (size_t orphan = 0; orphan <= orphan_index_; ++orphan)
    {
        const auto& orphan_block = orphan_chain_[orphan]->actual();
        const auto& transactions = orphan_block->transactions;

        BITCOIN_ASSERT(!transactions.empty());
        BITCOIN_ASSERT(transactions.front().is_coinbase());

        for (size_t tx_index = 0; tx_index < transactions.size();
            ++tx_index)
        {
            // TODO: too visually deep, move this section to subfunction.
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

} // namespace blockchain
} // namespace libbitcoin
