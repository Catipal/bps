#!/usr/bin/env python3
# Copyright (c) 2021 The Bitcoin PoS Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""A descriptor wallet can stake.

CreateCoinStake used to open with GetLegacyScriptPubKeyMan() and give up if there
wasn't one, so a descriptor wallet could not stake at all - it refused before ever
looking at the coins. Since legacy wallets are deprecated upstream, that made the
staking path a dead end.

It now resolves a SigningProvider per script, which any key manager can supply.

Note what this does *not* cover: cold staking on a descriptor wallet.
DescriptorScriptPubKeyMan::IsMine only matches scripts its own descriptor generated,
and there is no descriptor for a pay-to-cold-stake script, so P2CS outputs are
invisible to descriptor wallets. That needs a new descriptor type.
"""

from decimal import Decimal

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_greater_than

# Must match the height we mine to: the staker will not search below nLastPOWBlock.
LAST_POW_BLOCK = 1200
# Blocks mined to the sink before funding. The subsidy is 50 and maturity is 500, so
# at this height blocks 1..100 are spendable - comfortably more than we send.
FUND_BLOCKS = 600


class StakingDescriptorsTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.base_args = [
            "-lastpowblock=%d" % LAST_POW_BLOCK,
            "-coldstakeheight=10",
            "-fallbackfee=0.0001",
            "-debug=coinstake",
        ]
        self.extra_args = [self.base_args + ["-staking=0"]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()
        self.skip_if_no_sqlite()

    def run_test(self):
        node = self.nodes[0]

        self.log.info("Create a descriptor wallet and a legacy sink to fund it")
        node.createwallet(wallet_name="desc", descriptors=True)
        node.createwallet(wallet_name="sink", descriptors=False)
        desc = node.get_wallet_rpc("desc")
        sink = node.get_wallet_rpc("sink")
        assert_equal(desc.getwalletinfo()["format"], "sqlite")

        # Staking only accepts PUBKEY, PUBKEYHASH and COLDSTAKE outputs, so ask the
        # descriptor wallet for a legacy address rather than its default bech32.
        stake_addr = desc.getnewaddress("stake", "legacy")
        sink_addr = sink.getnewaddress("sink", "legacy")

        self.log.info("Fund the descriptor wallet with one mature coin")
        sink.generatetoaddress(FUND_BLOCKS, sink_addr)
        sink.sendtoaddress(stake_addr, 1000)
        sink.generatetoaddress(LAST_POW_BLOCK - node.getblockcount(), sink_addr)
        assert_equal(node.getblockcount(), LAST_POW_BLOCK)
        # Confirmed at ~FUND_BLOCKS+1, so it has ~600 confirmations by LAST_POW_BLOCK,
        # comfortably past the 500 needed to stake.
        assert_equal(desc.getbalance(), Decimal("1000.00000000"))

        self.log.info("The descriptor wallet reports stakeable coins")
        status = desc.getstakingstatus()
        assert_equal(status["stakeablecoins"], True)

        self.log.info("Restart with staking enabled, only the descriptor wallet loaded")
        self.restart_node(0, self.base_args + ["-staking=1", "-wallet=desc"])
        desc = node.get_wallet_rpc("desc")

        self.log.info("Wait for it to stake a block")
        target = LAST_POW_BLOCK + 1
        self.wait_until(lambda: node.getblockcount() >= target, timeout=600)

        block = node.getblock(node.getblockhash(target), 2)
        coinstake = block["tx"][1]
        # vout[0] is the coinstake marker; vout[1] pays the staker back.
        assert_equal(coinstake["vout"][0]["value"], Decimal("0E-8"))
        spk = coinstake["vout"][1]["scriptPubKey"]

        # The payout is always P2PK, and that is mandatory rather than stylistic:
        # GetBlockPublicKey() recovers the block-signing key from this script, so the
        # pubkey has to be in it. Paying to P2PKH instead gets the block rejected with
        # bad-blk-signature.
        assert_equal(spk["type"], "pubkey")
        self.log.info("...staked; coinstake pays a %s output", spk["type"])

        # ...which a pkh() descriptor does not track, so the wallet cannot see what it
        # earned. This is the remaining gap for descriptor staking: the wallet needs a
        # pk() descriptor covering the same key for the reward to show up.
        balances = desc.getbalances()["mine"]
        staked_reward = balances["trusted"] + balances["immature"]
        self.log.info("...reward visible to the pkh() descriptor wallet: %s", staked_reward)
        assert_equal(staked_reward, Decimal("0E-8"))
        self.log.info("...as expected: the P2PK payout is outside the wallet's descriptors")


if __name__ == '__main__':
    StakingDescriptorsTest().main()
