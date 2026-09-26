#!/usr/bin/env python3
# Copyright (c) 2026 The Bitcoin PoS developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Coins paid to a delegation address must actually stake.

wallet_delegation_address.py and wallet_auto_delegation.py check that the staking
wallet *sees* these outputs. Seeing is not staking. This test pays a delegation
address from an ordinary wallet, the way anybody would, lets the output mature, and
then waits for a block whose coinstake spends that exact output -- once with a
third-party staker and once with a wallet holding both keys.
"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_greater_than,
)

# Payments made at PAY_AT mature (COINBASE_MATURITY = 500) before proof-of-work
# ends at LAST_POW_BLOCK, so they are stakeable the moment staking can start.
LAST_POW_BLOCK = 1200
COLDSTAKE_HEIGHT = 10
FUND_BLOCKS = 300
PAY_AT = 690


class DelegationStakingTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.base_args = [
            "-lastpowblock=%d" % LAST_POW_BLOCK,
            "-coldstakeheight=%d" % COLDSTAKE_HEIGHT,
            "-fallbackfee=0.0001",
        ]
        self.extra_args = [self.base_args + ["-staking=0"]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def restart_staking(self, wallet):
        # Staking starts from postInitProcess, so the wallet has to be loaded at
        # startup -- and loading only this one means nothing else can stake.
        self.restart_node(0, self.base_args + ["-staking=1", "-wallet=%s" % wallet])

    def wait_for_pos_block(self, height):
        self.wait_until(lambda: self.nodes[0].getblockcount() >= height, timeout=600)
        return self.nodes[0].getblock(self.nodes[0].getblockhash(height), 2)

    def p2cs_outpoint(self, txid):
        tx = self.nodes[0].getrawtransaction(txid, True)
        outs = [o for o in tx["vout"] if o["scriptPubKey"]["type"] == "coldstake"]
        assert_equal(len(outs), 1)
        return (txid, outs[0]["n"])

    def kernel_of(self, block):
        assert_greater_than(len(block["tx"]), 1)
        coinstake = block["tx"][1]
        # The principal returns to the same cold-stake script by consensus.
        assert_equal(coinstake["vout"][1]["scriptPubKey"]["type"], "coldstake")
        vin = coinstake["vin"][0]
        return (vin["txid"], vin["vout"])

    def run_test(self):
        node = self.nodes[0]
        for name in ("payer", "owner", "ext", "self", "sink"):
            node.createwallet(wallet_name=name, descriptors=False)
        payer = node.get_wallet_rpc("payer")
        owner = node.get_wallet_rpc("owner")
        ext = node.get_wallet_rpc("ext")
        selfw = node.get_wallet_rpc("self")
        sink = node.get_wallet_rpc("sink")

        payer_addr = payer.getnewaddress("", "legacy")
        sink_addr = sink.getnewaddress("", "legacy")
        payer.generatetoaddress(FUND_BLOCKS, payer_addr)
        payer.generatetoaddress(PAY_AT - FUND_BLOCKS, sink_addr)

        self.log.info("Owner delegates to a staker in another wallet; one wallet delegates to itself")
        ext_staker = ext.getnewaddress("", "legacy")
        owner.setstakingaddress(ext_staker)
        third = owner.getdelegationaddress()["address"]

        self_staker = selfw.getnewaddress("", "legacy")
        selfw.setstakingaddress(self_staker)
        own = selfw.getdelegationaddress()["address"]

        self.log.info("A stock wallet pays both delegation addresses")
        op_third = self.p2cs_outpoint(payer.sendtoaddress(third, 1000))
        op_self = self.p2cs_outpoint(payer.sendtoaddress(own, 1000))
        payer.generatetoaddress(1, sink_addr)

        self.log.info("Let them mature while proof-of-work still runs")
        payer.generatetoaddress(LAST_POW_BLOCK - node.getblockcount(), sink_addr)
        assert_equal(node.getblockcount(), LAST_POW_BLOCK)

        self.log.info("The third-party staker produces a block from the payment it was delegated")
        # ext holds only the staking key and no coins at all, so a coinstake from it
        # can only have come from the delegation.
        self.restart_staking("ext")
        block = self.wait_for_pos_block(LAST_POW_BLOCK + 1)
        assert_equal(self.kernel_of(block), op_third)

        self.log.info("The self-delegating wallet produces a block from its own payment")
        # "self" never received a plain coin, so the same argument holds.
        self.restart_staking("self")
        height = node.getblockcount() + 1
        block = self.wait_for_pos_block(height)
        assert_equal(self.kernel_of(block), op_self)


if __name__ == '__main__':
    DelegationStakingTest().main()
