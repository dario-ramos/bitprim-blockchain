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
#ifndef LIBBITCOIN_BLOCKCHAIN_VALIDATE_TRANSACTION_HPP
#define LIBBITCOIN_BLOCKCHAIN_VALIDATE_TRANSACTION_HPP

#include <cstdint>
#include <cstddef>
#include <functional>
#include <memory>
#include <bitcoin/bitcoin.hpp>
#include <bitcoin/blockchain/define.hpp>
#include <bitcoin/blockchain/transaction_pool.hpp>

namespace libbitcoin {
namespace blockchain {

/// This class is not thread safe.
/// This is a utility for transaction_pool::validate and validate_block.
class BCB_API validate_transaction
  : public enable_shared_from_base<validate_transaction>
{
public:
    typedef std::shared_ptr<validate_transaction> ptr;
    typedef message::transaction_message::ptr transaction_ptr;
    typedef std::function<void(const code&, transaction_ptr,
        chain::point::indexes)> validate_handler;

    // Used for tx and block validation (stateless).
    //-------------------------------------------------------------------------

    static code check_transaction(const chain::transaction& tx);

    static code check_input(const chain::transaction& tx,
        size_t input_index, const chain::transaction& previous_tx,
        size_t parent_height, size_t previous_height, uint64_t& value,
        uint32_t flags);

    // Used for memory pool transaction validation (stateful).
    //-------------------------------------------------------------------------

    validate_transaction(block_chain& chain, transaction_ptr tx,
        const transaction_pool& pool, dispatcher& dispatch);

    validate_transaction(block_chain& chain, const chain::transaction& tx,
        const transaction_pool& pool, dispatcher& dispatch);

    void start(validate_handler handler);

private:
    static code check_consensus(const chain::script& prevout_script,
        const chain::transaction& tx, size_t input_index, uint32_t flags);

    code check_for_mempool() const;
    void handle_duplicate_check(const code& ec);
    void set_last_height(const code& ec, size_t last_height);
    void next_previous_transaction();
    void previous_tx_index(const code& ec, size_t parent_height);
    void search_pool_previous_tx();
    void handle_previous_tx(const code& ec,
        const chain::transaction& previous_tx, size_t parent_height);
    void handle_spend(const code& ec, const chain::input_point& point);

    block_chain& blockchain_;
    const transaction_ptr tx_;
    const transaction_pool& pool_;
    dispatcher& dispatch_;

    const hash_digest tx_hash_;
    size_t last_block_height_;
    uint64_t value_in_;
    uint32_t current_input_;
    chain::point::indexes unconfirmed_;
    validate_handler handle_validate_;
};

} // namespace blockchain
} // namespace libbitcoin

#endif
