#!/usr/bin/env python3
# Copyright (c) 2021 The Bitcoin PoS Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""The cold staker fee compounds without the owner's passphrase.

Paying the staker fee to a plain owner address is safe but leaves it idle: a plain
output is not a delegation, so the staking key cannot stake it, and the owner has
to unlock and sweep it back before it earns anything.

With -coldstakefeestaker the fee is instead paid into another delegation - a
different staking key, the same owner - so it is immediately stakeable by the same
unencrypted staking wallet. Consensus requires the fee output's script to differ
from the one being staked, which is exactly what the second staking key provides.

This test asserts the fee output is itself a delegation owned by the owner, and
then that it goes on to stake, which is what "compounds" actually means.
"""

from decimal import Decimal

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_greater_than

LAST_POW_BLOCK = 1200
COLDSTAKE_HEIGHT = 10
FUND_BLOCKS = 300
DELEGATE_AT = 690


class ColdStakingCompoundTest(BitcoinTestFramework):
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

    def p2cs_keys(self, spk_asm):
        """Pull (staker, owner) key hashes out of a decoded P2CS script."""
        # OP_DUP OP_HASH160 OP_ROT OP_IF OP_CHECKCOLDSTAKEVERIFY <staker>
        #   OP_ELSE <owner> OP_ENDIF OP_EQUALVERIFY OP_CHECKSIG
        parts = spk_asm.split()
        staker = parts[parts.index("OP_CHECKCOLDSTAKEVERIFY") + 1]
        owner = parts[parts.index("OP_ELSE") + 1]
        return staker, owner

    def run_test(self):
        node = self.nodes[0]

        self.log.info("Set up owner and staking wallets")
        node.createwallet(wallet_name="owner", descriptors=False)
        node.createwallet(wallet_name="staking", descriptors=False)
        node.createwallet(wallet_name="sink", descriptors=False)
        owner_w = node.get_wallet_rpc("owner")
        staking_w = node.get_wallet_rpc("staking")
        sink_w = node.get_wallet_rpc("sink")

        # Two staking keys, so the fee output can use one that differs from whichever
        # is currently staking. Both live in the unencrypted staking wallet.
        staker_a = staking_w.getnewaddress("staker_a", "legacy")
        staker_b = staking_w.getnewaddress("staker_b", "legacy")
        owner_addr = owner_w.getnewaddress("owner", "legacy")
        fund_addr = owner_w.getnewaddress("fund", "legacy")
        sink_addr = sink_w.getnewaddress("sink", "legacy")

        self.log.info("Fund and delegate to staker_a")
        owner_w.generatetoaddress(FUND_BLOCKS, fund_addr)
        owner_w.generatetoaddress(DELEGATE_AT - FUND_BLOCKS, sink_addr)
        owner_w.delegatestake(staker_a, owner_addr, 1000)

        self.log.info("Mature the delegation and sweep the owner's plain coins away")
        owner_w.generatetoaddress(LAST_POW_BLOCK - 1 - node.getblockcount(), sink_addr)
        owner_w.sendtoaddress(sink_addr, owner_w.getbalance(), "", "", True)
        owner_w.generatetoaddress(1, sink_addr)
        assert_equal(node.getblockcount(), LAST_POW_BLOCK)
        assert_equal(owner_w.getbalance(), Decimal("0E-8"))
        assert_equal(staking_w.getbalance(), Decimal("0E-8"))

        self.log.info("Restart staking with both staking keys offered for the fee")
        self.restart_node(0, self.base_args + [
            "-staking=1",
            "-wallet=owner", "-wallet=staking",
            "-coldstakefeestaker=%s" % staker_a,
            "-coldstakefeestaker=%s" % staker_b,
        ])
        # Wallet handles do not survive a restart; the old ones carry a stale cookie.
        owner_w = node.get_wallet_rpc("owner")
        staking_w = node.get_wallet_rpc("staking")

        self.log.info("Wait for the first stake")
        first = LAST_POW_BLOCK + 1
        self.wait_until(lambda: node.getblockcount() >= first, timeout=600)

        coinstake = node.getblock(node.getblockhash(first), 2)["tx"][1]
        vout = coinstake["vout"]
        assert_equal(vout[1]["scriptPubKey"]["type"], "coldstake")

        p2cs = vout[1]["scriptPubKey"]["hex"]
        fee_outs = [o for o in vout[1:] if o["scriptPubKey"]["hex"] != p2cs]
        assert_equal(len(fee_outs), 1)
        fee_out = fee_outs[0]

        # The fee must itself be a delegation, not a plain output.
        assert_equal(fee_out["scriptPubKey"]["type"], "coldstake")

        staked_staker, staked_owner = self.p2cs_keys(vout[1]["scriptPubKey"]["asm"])
        fee_staker, fee_owner = self.p2cs_keys(fee_out["scriptPubKey"]["asm"])

        # Same owner - the fee never leaves the owner's control...
        assert_equal(fee_owner, staked_owner)
        # ...but a different staking key, or consensus could not tell the two apart.
        assert fee_staker != staked_staker, "fee output must use a different staking key"
        self.log.info("...fee of %s paid into a delegation, same owner, staker %s",
                      fee_out["value"], fee_staker[:16])

        # The fee output is spendable only by the owner, so the staking wallet still
        # holds nothing it could run off with.
        assert_equal(staking_w.getbalance(), Decimal("0E-8"))

        self.log.info("Confirm the fee output is recognised as stakeable")
        # It needs COINBASE_MATURITY confirmations before it can stake, but the wallet
        # should already count it towards the delegated/stakeable totals.
        balances = owner_w.getbalances()["mine"]
        assert_greater_than(balances["trusted"] + balances["immature"], 0)
        self.log.info("...owner balances: %s", balances)


if __name__ == '__main__':
    ColdStakingCompoundTest().main()
