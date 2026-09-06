#!/usr/bin/env python3
# Copyright (c) 2021 The Bitcoin PoS Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Staking can be turned on and off at runtime, and off really means off.

The staking thread used to be stopped with boost::thread_group::interrupt_all(),
which never worked: ThreadStakeMiner spends nearly all its time in
UninterruptibleSleep, which is not an interruption point. The thread_group was
then deleted anyway, leaving a detached thread running against a wallet the node
was about to destroy.

There was also no way to observe any of this, because staking could only be set
with the -staking startup flag or the GUI checkbox. setstaking makes it
scriptable, so the stop path can be tested.
"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
)

# The staker refuses to look for kernels while the tip is below nLastPOWBlock, so
# this has to match the height we actually mine to. COINBASE_MATURITY is 500, so 700
# blocks leaves w's coin mature and stakeable.
LAST_POW_BLOCK = 1010
COLDSTAKE_HEIGHT = 10


class StakingToggleTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.extra_args = [[
            "-lastpowblock=%d" % LAST_POW_BLOCK,
            "-coldstakeheight=%d" % COLDSTAKE_HEIGHT,
            "-fallbackfee=0.0001",
            "-staking=0",
        ]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        node = self.nodes[0]

        self.log.info("Fund a wallet with a single mature, stakeable coin")
        node.createwallet(wallet_name="w", descriptors=False)
        node.createwallet(wallet_name="sink", descriptors=False)
        w = node.get_wallet_rpc("w")
        sink = node.get_wallet_rpc("sink")
        addr = w.getnewaddress("stake", "legacy")
        sink_addr = sink.getnewaddress("sink", "legacy")

        # Mine to the sink, not to w. The staker holds cs_wallet while it caches a
        # kernel for every candidate coin (two disk reads each), and getstakinginfo
        # also wants cs_wallet - so a wallet with hundreds of UTXOs makes that RPC
        # block long enough to time out. One coin keeps this test about the toggle.
        sink.generatetoaddress(505, sink_addr)
        sink.sendtoaddress(addr, 100)
        # Bring the tip to nLastPOWBlock so the staker will search, and give w's coin
        # its COINBASE_MATURITY confirmations.
        sink.generatetoaddress(LAST_POW_BLOCK - node.getblockcount(), sink_addr)
        assert_equal(node.getblockcount(), LAST_POW_BLOCK)

        self.log.info("Staking starts off")
        assert_equal(w.getstakinginfo()["staking"], False)

        self.log.info("setstaking(true) starts it")
        assert_equal(w.setstaking(True), True)
        assert_equal(w.getstakingstatus()["walletstakingenabled"], True)
        # Deliberately not waiting on getstakinginfo()["staking"]. That is
        # search_interval && weight, and search_interval is i - m_last_coin_stake_search_time,
        # which is zero until the search loop advances - up to a whole nMinerSleep
        # period later. Waiting on it makes the test timing-dependent without telling
        # us anything about the toggle, which is what this test is for.

        self.log.info("setstaking(false) stops it, and blocks until the thread is gone")
        # setstaking(false) calls StopStake(), which joins. If the stop flag were
        # ignored - as interrupt_all() was - this call would hang rather than return,
        # so simply getting an answer here is the assertion that matters.
        assert_equal(w.setstaking(False), False)
        assert_equal(w.getstakingstatus()["walletstakingenabled"], False)

        self.log.info("It can be started again - the old thread was joined, not leaked")
        assert_equal(w.setstaking(True), True)
        assert_equal(w.getstakingstatus()["walletstakingenabled"], True)

        self.log.info("Toggle repeatedly to shake out a leaked or double-started thread")
        for _ in range(5):
            assert_equal(w.setstaking(False), False)
            assert_equal(w.setstaking(True), True)
        assert_equal(w.setstaking(False), False)

        self.log.info("An encrypted, locked wallet refuses to start staking")
        node.createwallet(wallet_name="enc", descriptors=False)
        enc = node.get_wallet_rpc("enc")
        enc.encryptwallet("passphrase")
        enc = node.get_wallet_rpc("enc")
        assert_raises_rpc_error(-13, "the wallet is locked", enc.setstaking, True)
        self.log.info("...refused, as it holds no keys able to sign a coinstake")

        # The three checks below are here because none of the above caught any of it.
        # ThreadStakeMiner waits while `IsLocked() || !m_enabled_staking`, so a locked
        # wallet is a pause, not a stop. Code that "helpfully" stopped the staker on
        # lock joined the thread and nothing ever restarted it, so a momentary lock --
        # exactly what the passphrase prompt behind the GUI's Send button causes --
        # ended staking for the rest of the session. Everything above still passed,
        # because none of it locks the wallet while staking is on.
        self.log.info("Staking survives a lock/unlock cycle")
        enc.walletpassphrase("passphrase", 1000)
        assert_equal(enc.setstaking(True), True)
        enc.walletlock()
        assert_equal(enc.getstakingstatus()["walletstakingenabled"], True)
        assert_equal(enc.getstakingstatus()["walletunlocked"], False)
        enc.walletpassphrase("passphrase", 1000)
        assert_equal(enc.getstakingstatus()["walletstakingenabled"], True)

        self.log.info("...and repeated locking does not wear it down")
        for _ in range(3):
            enc.walletlock()
            enc.walletpassphrase("passphrase", 1000)
        assert_equal(enc.getstakingstatus()["walletstakingenabled"], True)

        # The restarts below drop -staking=0. That flag is the global gate and it wins,
        # so with it in place staking correctly stays off after a restart no matter what
        # the wallet recorded -- which is what a node actually running staking looks
        # like, and what the GUI runs with.
        staking_args = [a for a in self.extra_args[0] if a != "-staking=0"] + ["-staking=1"]

        self.log.info("An encrypted wallet comes back from a restart still staking")
        # Locked at startup, because nothing has supplied the passphrase yet. Staking
        # must still be on: refusing to start it here left an encrypted wallet with
        # staking off after every restart, whatever the user had chosen, and unlocking
        # later did not bring it back.
        enc.unloadwallet()
        self.restart_node(0, extra_args=staking_args)
        node.loadwallet("enc")
        enc = node.get_wallet_rpc("enc")
        assert_equal(enc.getstakingstatus()["walletunlocked"], False)
        assert_equal(enc.getstakingstatus()["walletstakingenabled"], True)

        self.log.info("...and switching it off survives a restart too")
        enc.walletpassphrase("passphrase", 1000)
        assert_equal(enc.setstaking(False), False)
        enc.unloadwallet()
        self.restart_node(0, extra_args=staking_args)
        node.loadwallet("enc")
        enc = node.get_wallet_rpc("enc")
        assert_equal(enc.getstakingstatus()["walletstakingenabled"], False)


if __name__ == '__main__':
    StakingToggleTest().main()
