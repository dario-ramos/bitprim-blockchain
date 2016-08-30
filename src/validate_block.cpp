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
#include <bitcoin/blockchain/validate_block.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <system_error>
#include <vector>
#include <boost/date_time.hpp>
#include <bitcoin/bitcoin.hpp>
#include <bitcoin/blockchain.hpp>
#include <bitcoin/blockchain/checkpoint.hpp>
#include <bitcoin/blockchain/validate_transaction.hpp>

#ifdef WITH_CONSENSUS
#include <bitcoin/consensus.hpp>
#endif

namespace libbitcoin {
namespace chain {

using boost::posix_time::ptime;
using boost::posix_time::from_time_t;
using boost::posix_time::second_clock;
using boost::posix_time::hours;

// Consensus rule change activation and enforcement parameters.
constexpr uint8_t version_4 = 4;
constexpr uint8_t version_3 = 3;
constexpr uint8_t version_2 = 2;
constexpr uint8_t version_1 = 1;

#ifdef ENABLE_TESTNET
    // See bip34 specification section.
    constexpr size_t sample = 100;
    constexpr size_t enforced = 75;
    constexpr size_t activated = 51;

    // Block 514 is the first block after activation, which was date-based.
    constexpr size_t bip16_activation_height = 514;

    // No bip30 testnet exceptions (we don't validate genesis block anyway).
    constexpr size_t bip30_exception_height1 = 0;
    constexpr size_t bip30_exception_height2 = 0;
#else
    // See bip34 specification section.
    constexpr size_t sample = 1000;
    constexpr size_t enforced = 950;
    constexpr size_t activated = 750;

    // Block 173805 is the first block after activation, which was date-based.
    constexpr size_t bip16_activation_height = 173805;

    // bip30 is retro-active starting at block zero, excluding these two bocks.
    constexpr size_t bip30_exception_height1 = 91842;
    constexpr size_t bip30_exception_height2 = 91880;
#endif

// Max block size is 1,000,000.
constexpr uint32_t max_block_size = 1000000;

// Maximum signature operations per block is 20,000.
constexpr uint32_t max_block_script_sig_operations = max_block_size / 50;

// Target readjustment every 2 weeks (in seconds).
constexpr uint64_t target_timespan = 2 * 7 * 24 * 60 * 60;

// Aim for blocks every 10 mins (in seconds).
constexpr uint64_t target_spacing = 10 * 60;

// Two weeks worth of blocks (count of blocks).
constexpr uint64_t readjustment_interval = target_timespan / target_spacing;

// To improve readability.
#define RETURN_IF_STOPPED() \
if (stopped()) \
    return error::service_stopped

// The nullptr option is for backward compatibility only.
validate_block::validate_block(size_t height, const block_type& block,
    const config::checkpoint::list& checks, stopped_callback callback)
  : height_(height),
    activations_(script_context::none_enabled),
    minimum_version_(0),
    current_block_(block),
    checkpoints_(checks),
    stop_callback_(callback == nullptr ? [](){ return false; } : callback)
{
}

void validate_block::initialize_context()
{
    // Continue even if height < sample (simpler and faster overall).
    const auto versions = preceding_block_versions(sample);

    const auto ge_4 = [](uint8_t version) { return version >= version_4; };
    const auto ge_3 = [](uint8_t version) { return version >= version_3; };
    const auto ge_2 = [](uint8_t version) { return version >= version_2; };

    const auto count_4 = std::count_if(versions.begin(), versions.end(), ge_4);
    const auto count_3 = std::count_if(versions.begin(), versions.end(), ge_3);
    const auto count_2 = std::count_if(versions.begin(), versions.end(), ge_2);

    const auto enforce = [](size_t count) { return count >= enforced; };
    const auto activate = [](size_t count) { return count >= activated; };

    // version 4/3/2 required based on 95% of preceding 1000 mainnet blocks.
    if (enforce(count_4))
        minimum_version_ = version_4;
    else if (enforce(count_3))
        minimum_version_ = version_3;
    else if (enforce(count_2))
        minimum_version_ = version_2;
    else
        minimum_version_ = version_1;

    // bip65 is activated based on 75% of preceding 1000 mainnet blocks.
    if (activate(count_4))
        activations_ |= script_context::bip65_enabled;

    // bip66 is activated based on 75% of preceding 1000 mainnet blocks.
    if (activate(count_3))
        activations_ |= script_context::bip66_enabled;

    // bip34 is activated based on 75% of preceding 1000 mainnet blocks.
    if (activate(count_2))
        activations_ |= script_context::bip34_enabled;

    // bip30 applies to all but two historical blocks that violate the rule.
    if (height_ != bip30_exception_height1 &&
        height_ != bip30_exception_height2)
        activations_ |= script_context::bip30_enabled;

    // bip16 was activated with a one-time test (~55% rule).
    if (height_ >= bip16_activation_height)
        activations_ |= script_context::bip16_enabled;
}

// initialize_context must be called first (to set activations_).
bool validate_block::is_active(script_context flag) const
{
    if (!bc::is_active(activations_, flag))
        return false;

    const auto version = current_block_.header.version;
    return
        (flag == script_context::bip65_enabled && version >= version_4) ||
        (flag == script_context::bip66_enabled && version >= version_3) ||
        (flag == script_context::bip34_enabled && version >= version_2);
}

// validate_version must be called first (to set minimum_version_).
bool validate_block::is_valid_version() const
{
    return current_block_.header.version >= minimum_version_;
}

bool validate_block::stopped() const
{
    return stop_callback_();
}

std::error_code validate_block::check_block() const
{
    // These are checks that are independent of the blockchain
    // that can be validated before saving an orphan block.

    const auto& transactions = current_block_.transactions;
    if (transactions.empty() || transactions.size() > max_block_size ||
        satoshi_raw_size(current_block_) > max_block_size)
    {
        return error::size_limits;
    }

    const auto& header = current_block_.header;
    const auto hash = hash_block_header(header);
    if (!is_valid_proof_of_work(hash, header.bits))
        return error::proof_of_work;

    RETURN_IF_STOPPED();

    if (!is_valid_time_stamp(header.timestamp))
        return error::futuristic_timestamp;

    RETURN_IF_STOPPED();

    if (!is_coinbase(transactions.front()))
        return error::first_not_coinbase;

    for (auto it = ++transactions.begin(); it != transactions.end(); ++it)
    {
        RETURN_IF_STOPPED();

        if (is_coinbase(*it))
            return error::extra_coinbases;
    }

    for (const auto& tx: transactions)
    {
        RETURN_IF_STOPPED();

        const auto ec = validate_transaction::check_transaction(tx);
        if (ec)
            return ec;
    }

    RETURN_IF_STOPPED();

    if (!is_distinct_tx_set(transactions))
        return error::duplicate;

    RETURN_IF_STOPPED();

    const auto sigops = legacy_sigops_count(transactions);
    if (sigops > max_block_script_sig_operations)
        return error::too_many_sigs;

    RETURN_IF_STOPPED();

    // auto merkle_root = generate_merkle_root(transactions);
    // log_info(LOG_BLOCKCHAIN) << "FER - validate_block::check_block() - merkle_root: " << encode_hash(merkle_root);
    // if (header.merkle != merkle_root)
    //     return error::merkle_mismatch;

    if (header.merkle != generate_merkle_root(transactions))
        return error::merkle_mismatch;

    return error::success;
}

bool validate_block::is_distinct_tx_set(const transaction_list& txs)
{
    // We test distinctness by transaction hash.
    const auto hasher = [](const transaction_type& transaction)
    {
        return hash_transaction(transaction);
    };

    std::vector<hash_digest> hashes(txs.size());
    std::transform(txs.begin(), txs.end(), hashes.begin(), hasher);
    std::sort(hashes.begin(), hashes.end());
    auto distinct_end = std::unique(hashes.begin(), hashes.end());
    return distinct_end == hashes.end();
}

ptime validate_block::current_time() const
{
    return second_clock::universal_time();
}

bool validate_block::is_valid_time_stamp(uint32_t timestamp) const
{
    const auto two_hour_future = current_time() + hours(2);
    const auto block_time = from_time_t(timestamp);
    return block_time <= two_hour_future;
}

bool validate_block::is_valid_proof_of_work(hash_digest hash, uint32_t bits)
{
    hash_number target;
    if (!target.set_compact(bits))
        return false;

    if (target <= 0 || target > max_target())
        return false;

    hash_number our_value;
    our_value.set_hash(hash);
    return (our_value <= target);
}

inline bool within_op_n(opcode code)
{
    const auto raw_code = static_cast<uint8_t>(code);
    constexpr auto op_1 = static_cast<uint8_t>(opcode::op_1);
    constexpr auto op_16 = static_cast<uint8_t>(opcode::op_16);
    return op_1 <= raw_code && raw_code <= op_16;
}

inline uint8_t decode_op_n(opcode code)
{
    const auto raw_code = static_cast<uint8_t>(code);
    BITCOIN_ASSERT(within_op_n(code));

    // Add 1 because we minus opcode::op_1, not the value before.
    constexpr auto op_1 = static_cast<uint8_t>(opcode::op_1);
    return raw_code - op_1 + 1;
}

inline size_t count_script_sigops(const operation_stack& operations,
    bool accurate)
{
    size_t total_sigs = 0;
    opcode last_opcode = opcode::bad_operation;
    for (const auto& op: operations)
    {
        if (op.code == opcode::checksig ||
            op.code == opcode::checksigverify)
        {
            total_sigs++;
        }
        else if (op.code == opcode::checkmultisig ||
            op.code == opcode::checkmultisigverify)
        {
            if (accurate && within_op_n(last_opcode))
                total_sigs += decode_op_n(last_opcode);
            else
                total_sigs += 20;
        }

        last_opcode = op.code;
    }

    return total_sigs;
}

size_t validate_block::legacy_sigops_count(const transaction_type& tx)
{
    size_t total_sigs = 0;
    for (const auto& input: tx.inputs)
    {
        const auto& operations = input.script.operations();
        total_sigs += count_script_sigops(operations, false);
    }

    for (const auto& output: tx.outputs)
    {
        const auto& operations = output.script.operations();
        total_sigs += count_script_sigops(operations, false);
    }

    return total_sigs;
}

size_t validate_block::legacy_sigops_count(const transaction_list& txs)
{
    size_t total_sigs = 0;
    for (const auto& tx: txs)
        total_sigs += legacy_sigops_count(tx);

    return total_sigs;
}

std::error_code validate_block::accept_block() const
{
    const auto& header = current_block_.header;
    if (header.bits != work_required())
        return error::incorrect_proof_of_work;

    RETURN_IF_STOPPED();

    if (header.timestamp <= median_time_past())
        return error::timestamp_too_early;

    RETURN_IF_STOPPED();

    // Txs should be final when included in a block.
    for (const auto& tx: current_block_.transactions)
    {
        if (!is_final(tx, height_, header.timestamp))
            return error::non_final_transaction;

        RETURN_IF_STOPPED();
    }

    // Ensure that the block passes checkpoints.
    // This is both DOS protection and performance optimization for sync.
    const auto block_hash = hash_block_header(header);
    if (!checkpoint::validate(block_hash, height_, checkpoints_))
        return error::checkpoints_failed;

    RETURN_IF_STOPPED();

    // Reject blocks that are below the minimum version for the current height.
    if (!is_valid_version())
        return error::old_version_block;

    RETURN_IF_STOPPED();

    // Enforce rule that the coinbase starts with serialized height.
    if (is_active(script_context::bip34_enabled) &&
        !is_valid_coinbase_height(height_, current_block_))
        return error::coinbase_height_mismatch;

    return error::success;
}

uint32_t validate_block::work_required() const
{
#ifdef ENABLE_TESTNET
    const auto last_non_special_bits = [this]()
    {
        // Return the last non-special block
        block_header_type previous_block;
        auto previous_height = height_;

        // Loop backwards until we find a difficulty change point,
        // or we find a block which does not have max_bits (is not special).
        while (true)
        {
            if (previous_height % readjustment_interval == 0)
               break;

            --previous_height;
            previous_block = fetch_block(previous_height);
            if (previous_block.bits != max_work_bits)
               break;
        }

        return previous_block.bits;
    };
#endif

    if (height_ == 0)
        return max_work_bits;

    if (height_ % readjustment_interval != 0)
    {
#ifdef ENABLE_TESTNET
        uint32_t max_time_gap =
            fetch_block(height_ - 1).timestamp + 2 * target_spacing;
        if (current_block_.header.timestamp > max_time_gap)
            return max_work_bits;

        return last_non_special_bits();
#else
        return previous_block_bits();
#endif
    }

    // This is the total time it took for the last 2016 blocks.
    const auto actual = actual_timespan(readjustment_interval);

    // Now constrain the time between an upper and lower bound.
    const auto constrained = range_constrain(actual,
        target_timespan / 4, target_timespan * 4);

    hash_number retarget;
    retarget.set_compact(previous_block_bits());
    retarget *= constrained;
    retarget /= target_timespan;
    if (retarget > max_target())
        retarget = max_target();

    return retarget.compact();
}

bool validate_block::is_valid_coinbase_height(size_t height, 
    const block_type& block)
{
    // There must be a transaction with an input.
    if (block.transactions.empty() || 
        block.transactions.front().inputs.empty())
        return false;

    // Get the serialized coinbase input script as a series of bytes.
    const auto& coinbase_tx = block.transactions.front();
    const auto& coinbase_script = coinbase_tx.inputs.front().script;
    const auto raw_coinbase = save_script(coinbase_script);

    // Try to recreate the expected bytes.
    script_type expect_coinbase;
    script_number expect_number(height);
    expect_coinbase.push_operation({opcode::special, expect_number.data()});

    // Save the expected coinbase script.
    const auto expect = save_script(expect_coinbase);

    // Perform comparison of the first bytes with raw_coinbase.
    if (expect.size() > raw_coinbase.size())
        return false;

    return std::equal(expect.begin(), expect.end(), raw_coinbase.begin());
}

std::error_code validate_block::connect_block() const
{

    log_info(LOG_BLOCKCHAIN) << "FER - validate_block::connect_block(...) - 1";

    const auto& transactions = current_block_.transactions;

    // These coinbase transactions are spent and are not indexed.
    if (is_active(script_context::bip30_enabled))
    {
        ////////////// TODO: parallelize. //////////////
        for (const auto& tx: transactions)
        {
            if (is_spent_duplicate(tx))
                return error::duplicate_or_spent;

            RETURN_IF_STOPPED();
        }
    }

    log_info(LOG_BLOCKCHAIN) << "FER - validate_block::connect_block(...) - 2";


    uint64_t fees = 0;
    size_t total_sigops = 0;
    const auto count = transactions.size();

    ////////////// TODO: parallelize. //////////////
    for (size_t tx_index = 0; tx_index < count; ++tx_index)
    {
        uint64_t value_in = 0;
        const auto& tx = transactions[tx_index];

        // It appears that this is also checked in check_block().
        total_sigops += legacy_sigops_count(tx);
        if (total_sigops > max_block_script_sig_operations)
            return error::too_many_sigs;

        log_info(LOG_BLOCKCHAIN) << "FER - validate_block::connect_block(...) - 3";
        RETURN_IF_STOPPED();

        // Count sigops for tx 0, but we don't perform
        // the other checks on coinbase tx.
        if (is_coinbase(tx))
            continue;


        log_info(LOG_BLOCKCHAIN) << "FER - validate_block::connect_block(...) - 4";
        RETURN_IF_STOPPED();

        // Consensus checks here.
        if (!validate_inputs(tx, tx_index, value_in, total_sigops))
            return error::validate_inputs_failed;

        log_info(LOG_BLOCKCHAIN) << "FER - validate_block::connect_block(...) - 5";
        RETURN_IF_STOPPED();

        if (!validate_transaction::tally_fees(tx, value_in, fees))
            return error::fees_out_of_range;

        log_info(LOG_BLOCKCHAIN) << "FER - validate_block::connect_block(...) - 6";

    }

    RETURN_IF_STOPPED();

    const auto& coinbase = transactions.front();
    const auto coinbase_value = total_output_value(coinbase);

    log_info(LOG_BLOCKCHAIN) << "FER - validate_block::connect_block(...) - 7";


    if (coinbase_value > block_value(height_) + fees)
        return error::coinbase_too_large;

    log_info(LOG_BLOCKCHAIN) << "FER - validate_block::connect_block(...) - 8";

    return error::success;
}

bool validate_block::is_spent_duplicate(const transaction_type& tx) const
{
    const auto tx_hash = hash_transaction(tx);

    // Is there a matching previous tx?
    if (!transaction_exists(tx_hash))
        return false;

    // Are all outputs spent?
    ////////////// TODO: parallelize. //////////////
    for (uint32_t output_index = 0; output_index < tx.outputs.size();
        ++output_index)
    {
        if (!is_output_spent({ tx_hash, output_index }))
            return false;
    }

    return true;
}

bool validate_block::validate_inputs(const transaction_type& tx,
    size_t index_in_parent, uint64_t& value_in, size_t& total_sigops) const
{
    BITCOIN_ASSERT(!is_coinbase(tx));

    //log_info(LOG_BLOCKCHAIN) << "FER - validate_block::validate_inputs(...) - 1";
    //log_info(LOG_BLOCKCHAIN) << "FER - validate_block::validate_inputs(...) - tx.inputs.size(): " << tx.inputs.size();

    ////////////// TODO: parallelize. //////////////
    for (size_t input_index = 0; input_index < tx.inputs.size(); ++input_index) {

        //log_info(LOG_BLOCKCHAIN) << "FER - validate_block::validate_inputs(...) - input_index: " << input_index;

        if (!connect_input(index_in_parent, tx, input_index, value_in, total_sigops)) {
            log_warning(LOG_VALIDATE) << "Invalid input ["
                << encode_hash(hash_transaction(tx)) << ":"
                << input_index << "]";
            return false;
        }
    }

    //log_info(LOG_BLOCKCHAIN) << "FER - validate_block::validate_inputs(...) - 2";

    return true;
}

size_t script_hash_signature_operations_count(
    const script_type& output_script, const script_type& input_script)
{
    if (output_script.type() != payment_type::script_hash)
        return 0;

    if (input_script.operations().empty())
        return 0;

    const auto& last_data = input_script.operations().back().data;
    const auto eval_script = parse_script(last_data);
    return count_script_sigops(eval_script.operations(), true);
}

bool validate_block::connect_input(size_t index_in_parent,
    const transaction_type& current_tx, size_t input_index, uint64_t& value_in,
    size_t& total_sigops) const
{
    BITCOIN_ASSERT(input_index < current_tx.inputs.size());

    //log_info(LOG_BLOCKCHAIN) << "FER - validate_block::connect_input(...) - 1";

    // Lookup previous output
    size_t previous_height;
    transaction_type previous_tx;
    const auto& input = current_tx.inputs[input_index];
    const auto& previous_output = input.previous_output;

    //log_info(LOG_BLOCKCHAIN) << "FER - validate_block::connect_input(...) - 2";
    //TODO: Fer: Has to be improved. Prioriry 1
    if (!fetch_transaction(previous_tx, previous_height, previous_output.hash))
    {
        log_warning(LOG_VALIDATE)
            << "Failure fetching input transaction ["
            << encode_hash(previous_output.hash) << "]";
        return false;
    }

    //log_info(LOG_BLOCKCHAIN) << "FER - validate_block::connect_input(...) - 3";


    const auto& previous_tx_out = previous_tx.outputs[previous_output.index];

    //log_info(LOG_BLOCKCHAIN) << "FER - validate_block::connect_input(...) - 4";


    // Signature operations count if script_hash payment type.
    try
    {
        total_sigops += script_hash_signature_operations_count(
            previous_tx_out.script, input.script);
    }
    catch (end_of_stream)
    {
        log_warning(LOG_VALIDATE) << "Invalid eval script.";
        return false;
    }

    //log_info(LOG_BLOCKCHAIN) << "FER - validate_block::connect_input(...) - 5";


    if (total_sigops > max_block_script_sig_operations)
    {
        log_warning(LOG_VALIDATE) << "Total sigops exceeds block maximum.";
        return false;
    }

    //log_info(LOG_BLOCKCHAIN) << "FER - validate_block::connect_input(...) - 6";


    // Get output amount
    const auto output_value = previous_tx_out.value;
    if (output_value > max_money())
    {
        log_warning(LOG_VALIDATE) << "Output money exceeds 21 million.";
        return false;
    }

    //log_info(LOG_BLOCKCHAIN) << "FER - validate_block::connect_input(...) - 7";


    // Check coinbase maturity has been reached
    if (is_coinbase(previous_tx))
    {
        BITCOIN_ASSERT(previous_height <= height_);
        const auto height_difference = height_ - previous_height;
        if (height_difference < coinbase_maturity)
        {
            log_warning(LOG_VALIDATE) << "Immature coinbase spend attempt.";
            return false;
        }
    }

    //log_info(LOG_BLOCKCHAIN) << "FER - validate_block::connect_input(...) - 8";

    //TODO: Fer: Has to be improved. Prioriry 2
    if (!validate_transaction::check_consensus(previous_tx_out.script,
        current_tx, input_index, activations_))
    {
        log_warning(LOG_VALIDATE) << "Input script invalid consensus.";
        return false;
    }

    //log_info(LOG_BLOCKCHAIN) << "FER - validate_block::connect_input(...) - 9";


    //TODO: Fer: Has to be improved. Prioriry 0
    // Search for double spends.
    if (is_output_spent(previous_output, index_in_parent, input_index))
    {
        log_warning(LOG_VALIDATE) << "Double spend attempt.";
        return false;
    }

    //log_info(LOG_BLOCKCHAIN) << "FER - validate_block::connect_input(...) - 10";


    // Increase value_in by this output's value
    value_in += output_value;
    if (value_in > max_money())
    {
        log_warning(LOG_VALIDATE) << "Input money exceeds 21 million.";
        return false;
    }

    //log_info(LOG_BLOCKCHAIN) << "FER - validate_block::connect_input(...) - 11";


    return true;
}

#undef RETURN_IF_STOPPED

} // namespace chain
} // namespace libbitcoin
