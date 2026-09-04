#!/usr/bin/env python3
# Copyright (c) 2024 The Bitcoin PoS developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test rotatestakingkey, the recovery path for a leaked staking key.

A staking key cannot spend the coins it stakes, so a leak is not theft. What it can
do is keep using the owner's stake weight to produce blocks and take the staker fee.
Rotation re-delegates to a fresh staking key in one transaction, which is only
possible because the owner key -- held separately -- is what authorises the spend.
"""

from decimal import Decimal

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_greater_than, assert_raises_rpc_error


class WalletStakingRotationTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1
        # Regtest ships nLastPOWBlock = INT_MAX and enables cold staking at 2000; both
        # have to be lowered before any of this is reachable. Same values as
        # wallet_coldstaking.py.
        self.extra_args = [[
            "-lastpowblock=1200",
            "-coldstakeheight=10",
            "-fallbackfee=0.0001",
            "-staking=0",
            "-coldstaking=1",
        ]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def decode(self, w, txid):
        # gettransaction's "decoded" field needs a verbosity argument this fork does
        # not take, so decode the raw hex instead.
        return self.nodes[0].decoderawtransaction(w.gettransaction(txid)["hex"])

    def coldstake_outs(self, w, txid):
        """The cold-stake outputs of one transaction, as (value, scriptPubKey)."""
        return [(o["value"], o["scriptPubKey"]) for o in self.decode(w, txid)["vout"]
                if o["scriptPubKey"]["type"] == "coldstake"]

    def run_test(self):
        node = self.nodes[0]
        node.createwallet(wallet_name="owner", descriptors=False)
        w = node.get_wallet_rpc("owner")

        owner_addr = w.getnewaddress()
        w.generatetoaddress(600, owner_addr)
        self.sync_all()

        # The staking key lives in a second wallet: that separation is the whole point,
        # and it is what makes a leak survivable.
        node.createwallet(wallet_name="staker", descriptors=False)
        staker_w = node.get_wallet_rpc("staker")
        leaked_staker = staker_w.getnewaddress()
        fresh_staker = staker_w.getnewaddress()

        self.log.info("Delegate to the staking key that will later be treated as leaked")
        delegation_txid = w.delegatestake(leaked_staker, owner_addr, 1000)["txid"]
        w.generatetoaddress(1, owner_addr)

        before = self.coldstake_outs(w, delegation_txid)
        assert_equal(len(before), 1)
        delegated_value, before_spk = before[0]
        assert_equal(delegated_value, Decimal("1000.00000000"))
        assert leaked_staker in before_spk["addresses"]

        self.log.info("Rotating to a fresh staking key moves the coins in one transaction")
        result = w.rotatestakingkey(fresh_staker)
        assert_equal(result["new_staker_address"], fresh_staker)
        assert_equal(result["outputs_rotated"], 1)
        assert_equal(result["amount_rotated"], delegated_value)
        w.generatetoaddress(1, owner_addr)

        self.log.info("The value survived less the fee, now under the new staking key")
        after = self.coldstake_outs(w, result["txid"])
        assert_equal(len(after), 1)
        rotated_value, rotated_spk = after[0]
        assert_greater_than(rotated_value, delegated_value - Decimal("0.01"))
        assert fresh_staker in rotated_spk["addresses"]
        assert leaked_staker not in rotated_spk["addresses"]


        self.log.info("Rotating again to the same key is refused rather than paying a fee for nothing")
        assert_raises_rpc_error(-4, "already delegated to that staking address",
                                w.rotatestakingkey, fresh_staker)

        self.log.info("A wallet without the owner key cannot rotate, even holding the staking key")
        assert_raises_rpc_error(-4, "no cold-staked outputs whose owner key it has",
                                staker_w.rotatestakingkey, leaked_staker)

        self.log.info("An unrelated address with nothing delegated to it is refused too")
        stranger = staker_w.getnewaddress()
        assert_raises_rpc_error(-4, "No delegated outputs to rotate",
                                staker_w.rotatestakingkey, stranger)


if __name__ == '__main__':
    WalletStakingRotationTest().main()
