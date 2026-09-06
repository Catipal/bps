#!/usr/bin/env python3
# Copyright (c) 2024 The Bitcoin PoS developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test delegation addresses: coins that are cold-staked the moment they arrive.

Paying an ordinary address produces an output the wallet must afterwards delegate,
which costs a transaction and needs the owner's passphrase. That makes staking
hands-off only until the next deposit.

A delegation address encodes both keys, so the sender's own wallet builds a P2CS
output directly. Nothing has to happen afterwards: the staking wallet can stake it,
and only the owner can spend it.
"""

from decimal import Decimal

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_greater_than,
    assert_raises_rpc_error,
)

LAST_POW_BLOCK = 1200
COLDSTAKE_HEIGHT = 10


class DelegationAddressTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.extra_args = [[
            "-lastpowblock=%d" % LAST_POW_BLOCK,
            "-coldstakeheight=%d" % COLDSTAKE_HEIGHT,
            "-fallbackfee=0.0001",
            "-staking=0",
            "-coldstaking=1",
        ]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def decode(self, w, txid):
        return self.nodes[0].decoderawtransaction(w.gettransaction(txid)["hex"])

    def run_test(self):
        node = self.nodes[0]
        node.createwallet(wallet_name="owner", descriptors=False)
        w = node.get_wallet_rpc("owner")
        addr = w.getnewaddress()
        w.generatetoaddress(700, addr)

        # The staking key lives apart from the coins; that separation is what lets it
        # be held unencrypted.
        node.createwallet(wallet_name="staker", descriptors=False)
        staker_w = node.get_wallet_rpc("staker")
        staker_addr = staker_w.getnewaddress()

        self.log.info("A delegation address encodes both keys and round-trips")
        result = w.getdelegationaddress(staker_addr)
        deleg = result["address"]
        assert_equal(result["staker_address"], staker_addr)
        # It must not be mistakable for an ordinary address.
        assert deleg != staker_addr and deleg != result["owner_address"]
        info = node.validateaddress(deleg)
        assert_equal(info["isvalid"], True)
        assert_equal(info["isdelegation"], True)

        self.log.info("Two calls give different addresses, so receiving does not reuse one")
        second = w.getdelegationaddress(staker_addr)["address"]
        assert second != deleg
        # ...but both delegate to the same staking key, so one staking wallet covers both.
        assert_equal(node.validateaddress(second)["staker_pubkeyhash"],
                     info["staker_pubkeyhash"])

        self.log.info("Paying it produces a cold-stake output, with no delegation step")
        txid = w.sendtoaddress(deleg, 1000)
        w.generatetoaddress(1, addr)
        outs = [o for o in self.decode(w, txid)["vout"]
                if o["scriptPubKey"]["type"] == "coldstake"]
        assert_equal(len(outs), 1)
        assert_equal(outs[0]["value"], Decimal("1000.00000000"))

        self.log.info("The staking wallet sees it as stakeable without holding the coins")
        # ISMINE_COLD: it can stake this output and do nothing else with it. The coins
        # are immature for COINBASE_MATURITY blocks, so they show under the immature
        # figure first -- which is the point being checked, that the staking wallet sees
        # them at all without any delegation step having happened.
        cold = staker_w.getwalletinfo()
        assert_greater_than(cold["cold_stake"] + cold["immature_cold_stake"], 0)
        # And it cannot spend them: the owner key is not there.
        assert_equal(staker_w.getbalances()["mine"]["trusted"], Decimal("0E-8"))

        self.log.info("The owner still owns it and can still spend it")
        assert_greater_than(w.getwalletinfo()["delegated"] + w.getwalletinfo()["immature_delegated"], 0)

        self.log.info("An older wallet would reject the address rather than misread it")
        # The payload is two hashes where every other base58 address here carries one,
        # so a wallet without the prefix cannot decode it as something else.
        assert_equal(node.validateaddress(deleg[:-1] + ("a" if deleg[-1] != "a" else "b"))["isvalid"], False)

        self.log.info("A delegation address is refused where a plain address is required")
        assert_raises_rpc_error(-5, "", w.getdelegationaddress, deleg)


if __name__ == '__main__':
    DelegationAddressTest().main()
