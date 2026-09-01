#!/usr/bin/env python3
# Copyright (c) 2021 The Bitcoin PoS Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Stake from an encrypted wallet without ever supplying the passphrase.

Staking needs a key that can sign, so an encrypted wallet cannot stake while it is
locked. Cold staking splits the key that signs coinstakes from the key that spends,
and consensus (OP_CHECKCOLDSTAKEVERIFY) restricts the former to producing coinstakes
that pay back to the owner. That makes it safe to keep the staking key unencrypted.

This test sets that up the cheap way - two wallets in one node:

  owner  : encrypted, holds the spending keys, stays LOCKED for the whole test
  staking: unencrypted, holds only a staking key, cannot move funds

and asserts the node stakes the delegation while the owner wallet is locked, with the
staker fee going to the owner (-coldstakefeetoowner) so the unencrypted wallet never
accumulates anything spendable.
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
FUND_BLOCKS = 300
DELEGATE_AT = 690
PASSPHRASE = "correct horse battery staple"


class ColdStakingPasswordlessTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.base_args = [
            "-lastpowblock=%d" % LAST_POW_BLOCK,
            "-coldstakeheight=%d" % COLDSTAKE_HEIGHT,
            "-coldstakefeetoowner=1",
            "-fallbackfee=0.0001",
        ]
        self.extra_args = [self.base_args + ["-staking=0"]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        node = self.nodes[0]

        self.log.info("Set up an encrypted owner wallet and an unencrypted staking wallet")
        node.createwallet(wallet_name="owner", descriptors=False)
        node.createwallet(wallet_name="staking", descriptors=False)
        # Somewhere to dump coins that must not belong to either wallet under test.
        node.createwallet(wallet_name="sink", descriptors=False)
        owner_w = node.get_wallet_rpc("owner")
        staking_w = node.get_wallet_rpc("staking")
        sink_w = node.get_wallet_rpc("sink")

        # The staking wallet holds exactly one thing: a staking key.
        staker_addr = staking_w.getnewaddress("staker", "legacy")
        owner_addr = owner_w.getnewaddress("owner", "legacy")
        fund_addr = owner_w.getnewaddress("fund", "legacy")
        sink_addr = sink_w.getnewaddress("sink", "legacy")

        self.log.info("Fund the owner wallet and delegate to the staking wallet's key")
        owner_w.generatetoaddress(FUND_BLOCKS, fund_addr)
        owner_w.generatetoaddress(DELEGATE_AT - FUND_BLOCKS, sink_addr)
        # staker_addr is not in the owner wallet, so this is an ordinary delegation.
        owner_w.delegatestake(staker_addr, owner_addr, 1000)

        self.log.info("Mine until the delegation matures, then sweep the owner's plain coins")
        owner_w.generatetoaddress(LAST_POW_BLOCK - 1 - node.getblockcount(), sink_addr)
        owner_w.sendtoaddress(sink_addr, owner_w.getbalance(), "", "", True)
        owner_w.generatetoaddress(1, sink_addr)
        assert_equal(node.getblockcount(), LAST_POW_BLOCK)

        self.log.info("Encrypt the owner wallet")
        # Encrypting stops the node's wallet handle, so re-acquire it afterwards.
        owner_w.encryptwallet(PASSPHRASE)
        owner_w = node.get_wallet_rpc("owner")

        self.log.info("The staking wallet holds nothing spendable")
        assert_equal(staking_w.getbalance(), Decimal("0E-8"))

        self.log.info("The owner wallet is locked and refuses to spend")
        assert_equal(owner_w.getwalletinfo()["unlocked_until"], 0)
        assert_raises_rpc_error(
            -13, "Please enter the wallet passphrase with walletpassphrase first",
            owner_w.sendtoaddress, sink_addr, 1)

        self.log.info("Restart with staking on - no passphrase is ever supplied")
        self.restart_node(0, self.base_args + [
            "-staking=1", "-wallet=owner", "-wallet=staking"])
        owner_w = node.get_wallet_rpc("owner")
        staking_w = node.get_wallet_rpc("staking")

        # The owner wallet must still be locked; nothing in this test unlocks it.
        assert_equal(owner_w.getwalletinfo()["unlocked_until"], 0)

        self.log.info("Wait for a proof-of-stake block")
        target = LAST_POW_BLOCK + 1
        self.wait_until(lambda: node.getblockcount() >= target, timeout=600)

        # Still locked after staking - staking never needed it.
        assert_equal(owner_w.getwalletinfo()["unlocked_until"], 0)

        self.log.info("Check the coinstake")
        block = node.getblock(node.getblockhash(target), 2)
        coinstake = block["tx"][1]
        vout = coinstake["vout"]
        assert_equal(vout[1]["scriptPubKey"]["type"], "coldstake")

        p2cs = vout[1]["scriptPubKey"]["hex"]
        fee_outputs = [o for o in vout[1:] if o["scriptPubKey"]["hex"] != p2cs]
        assert_equal(len(fee_outputs), 1)
        fee_out = fee_outputs[0]

        addresses = fee_out["scriptPubKey"].get("addresses")
        if addresses is None:
            addr = fee_out["scriptPubKey"].get("address")
            addresses = [addr] if addr else []
        assert_equal(addresses, [owner_addr])
        self.log.info("...staked while locked; fee of %s paid to the owner", fee_out["value"])

        # The point of the exercise: the unencrypted wallet still controls nothing.
        assert_equal(staking_w.getbalance(), Decimal("0E-8"))
        assert_greater_than(owner_w.getbalances()["mine"]["immature"] +
                            owner_w.getbalances()["mine"]["trusted"], 0)
        self.log.info("...the unencrypted staking wallet still holds no spendable coins")


if __name__ == '__main__':
    ColdStakingPasswordlessTest().main()
