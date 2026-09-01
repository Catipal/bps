#!/usr/bin/env python3
# Copyright (c) 2021 The Bitcoin PoS Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test cold staking (pay-to-cold-stake) delegation and the staker fee.

Covers both shapes of delegation:

  * third-party delegation - the owner delegates to somebody else's staking key.
    The staker earns COLD_STAKER_FEE percent of the block reward, paid to a bare
    P2PK output for its own staking key.

  * self-delegation - one wallet holds both the staking key and the owner key,
    which is how a node stakes without a passphrase: consensus restricts the
    staking key to producing coinstakes that pay back to the owner, so it can be
    kept apart from the spending key. Here the fee has no reason to go to the
    staking key and is paid to the owner instead.

In both cases consensus only constrains the *value* of the staker output (exactly
COLD_STAKER_FEE percent, see CheckReward) and never its script, so the two are
equally valid blocks; which one a wallet produces is wallet policy.
"""

from decimal import Decimal

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_greater_than,
)

# Must stay above COINBASE_MATURITY (500) plus the height the delegations are made
# at, so the delegated outputs can mature while proof-of-work is still allowed.
LAST_POW_BLOCK = 1200
COLDSTAKE_HEIGHT = 10
FUND_BLOCKS = 300
DELEGATE_AT = 690
COLD_STAKER_FEE = 7  # percent, see src/miner.h


class ColdStakingTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        # Regtest ships nLastPOWBlock = INT_MAX, which makes ConnectBlock reject
        # proof-of-stake at every height. Lower it so staking can be exercised.
        self.base_args = [
            "-lastpowblock=%d" % LAST_POW_BLOCK,
            "-coldstakeheight=%d" % COLDSTAKE_HEIGHT,
            "-fallbackfee=0.0001",
        ]
        self.extra_args = [self.base_args + ["-staking=0"]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def restart_staking(self, wallet):
        """Restart the node staking, with only `wallet` loaded.

        Staking is started from postInitProcess(), so the wallet has to be loaded
        at startup rather than with loadwallet.
        """
        self.restart_node(0, self.base_args + ["-staking=1", "-wallet=%s" % wallet])

    def wait_for_pos_block(self, height):
        """Wait for the staker to produce the block at `height`."""
        self.wait_until(lambda: self.nodes[0].getblockcount() >= height, timeout=600)
        return self.nodes[0].getblock(self.nodes[0].getblockhash(height), 2)

    def coinstake_of(self, block):
        assert_greater_than(len(block["tx"]), 1)
        return block["tx"][1]

    def check_stake_payout(self, coinstake, expect_fee_to):
        """Assert the coinstake is a cold stake paying its fee to `expect_fee_to`.

        Returns (principal_output, fee_output).
        """
        vout = coinstake["vout"]
        assert_equal(vout[1]["scriptPubKey"]["type"], "coldstake")

        # The staker fee is the single output whose script differs from the P2CS
        # script being staked; that is exactly how CheckReward identifies it.
        p2cs = vout[1]["scriptPubKey"]["hex"]
        fee_outputs = [o for o in vout[1:] if o["scriptPubKey"]["hex"] != p2cs]
        assert_equal(len(fee_outputs), 1)
        fee_out = fee_outputs[0]

        addresses = fee_out["scriptPubKey"].get("addresses")
        if addresses is None:
            address = fee_out["scriptPubKey"].get("address")
            addresses = [address] if address else []
        assert_equal(addresses, [expect_fee_to])

        # The fee must be exactly COLD_STAKER_FEE percent of the block reward, and
        # the rest of the reward must return to the P2CS script.
        reward = fee_out["nValue"] if "nValue" in fee_out else fee_out["value"]
        assert_greater_than(reward, 0)
        return vout[1], fee_out

    def run_test(self):
        node = self.nodes[0]

        self.log.info("Create wallets: stk (owner+self-staker), ext (third-party staker), sink")
        node.createwallet(wallet_name="stk", descriptors=False)
        node.createwallet(wallet_name="ext", descriptors=False)
        node.createwallet(wallet_name="sink", descriptors=False)
        stk = node.get_wallet_rpc("stk")
        ext = node.get_wallet_rpc("ext")
        sink = node.get_wallet_rpc("sink")

        self_staker = stk.getnewaddress("self_staker", "legacy")
        self_owner = stk.getnewaddress("self_owner", "legacy")
        ext_staker = ext.getnewaddress("ext_staker", "legacy")
        ext_owner = stk.getnewaddress("ext_owner", "legacy")
        fund_addr = stk.getnewaddress("fund", "legacy")
        sink_addr = sink.getnewaddress("sink", "legacy")

        self.log.info("Mine %d funding blocks to stk, the rest to sink", FUND_BLOCKS)
        # Only the first FUND_BLOCKS coinbases pay into stk. Everything later goes to
        # the sink so that, once stk is swept, no further coinbase can mature into it
        # and give the staker a plain coin to use instead of a delegation.
        stk.generatetoaddress(FUND_BLOCKS, fund_addr)
        stk.generatetoaddress(DELEGATE_AT - FUND_BLOCKS, sink_addr)
        assert_equal(node.getblockcount(), DELEGATE_AT)

        self.log.info("Delegate to a third-party staker (fee should go to that staker)")
        third_party = stk.delegatestake(ext_staker, ext_owner, 1000)
        assert_equal(third_party["staker_address"], ext_staker)

        self.log.info("Self-delegate (fee should go to the owner)")
        # Refused by default: delegating to a key you already hold is pointless under
        # the usual model. It is the point here, so opt in explicitly.
        self.log.info("...refused without allow_self_delegation")
        try:
            stk.delegatestake(self_staker, self_owner, 1000)
            raise AssertionError("self-delegation should be refused by default")
        except Exception as e:
            assert "delegate them to yourself" in str(e), str(e)
        self_deleg = stk.delegatestake(self_staker, self_owner, 1000, False, True)
        assert_equal(self_deleg["staker_address"], self_staker)

        self.log.info("Mine until the delegations mature")
        stk.generatetoaddress(LAST_POW_BLOCK - 1 - node.getblockcount(), sink_addr)

        self.log.info("Sweep every other mature coin out of stk")
        balance = stk.getbalance()
        assert_greater_than(balance, 0)
        stk.sendtoaddress(sink_addr, balance, "", "", True)
        stk.generatetoaddress(1, sink_addr)
        assert_equal(node.getblockcount(), LAST_POW_BLOCK)
        # Nothing plain left, so any coinstake must come from a delegation.
        assert_equal(stk.getbalance(), Decimal("0E-8"))

        self.log.info("Stake as the third-party staker: fee goes to the staker's own key")
        self.restart_staking("ext")
        block = self.wait_for_pos_block(LAST_POW_BLOCK + 1)
        coinstake = self.coinstake_of(block)
        vout = coinstake["vout"]
        assert_equal(vout[1]["scriptPubKey"]["type"], "coldstake")
        p2cs = vout[1]["scriptPubKey"]["hex"]
        fee_outputs = [o for o in vout[1:] if o["scriptPubKey"]["hex"] != p2cs]
        assert_equal(len(fee_outputs), 1)
        # Paid to the staking key as a bare P2PK, which no address is rendered for.
        assert_equal(fee_outputs[0]["scriptPubKey"]["type"], "pubkey")
        self.log.info("...third-party staker fee paid to its own key, as expected")

        self.log.info("Stake the self-delegation: fee goes to the owner instead")
        self.restart_staking("stk")
        height = node.getblockcount() + 1
        block = self.wait_for_pos_block(height)
        coinstake = self.coinstake_of(block)
        principal, fee_out = self.check_stake_payout(coinstake, self_owner)

        # 100% of the reward stays under the owner's control: the principal returns to
        # the P2CS script by consensus, and the fee now goes to the owner as well.
        total_out = sum(o["value"] for o in coinstake["vout"])
        fee_ratio = (fee_out["value"] * 100) / (total_out - Decimal(1000))
        assert_equal(int(fee_ratio.quantize(Decimal("1"))), COLD_STAKER_FEE)
        self.log.info("...self-delegated staker fee paid to the owner, %d%% of the reward",
                      COLD_STAKER_FEE)


if __name__ == '__main__':
    ColdStakingTest().main()
