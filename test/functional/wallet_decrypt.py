#!/usr/bin/env python3
# Copyright (c) 2024 The Bitcoin PoS developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test decryptwallet, and that the staking switch survives a restart.

decryptwallet removes the passphrase from a wallet so its keys can be used without
one -- the state a dedicated staking wallet needs to be in. The parts worth proving
are that the keys still work afterwards, that they are genuinely no longer encrypted
(rather than merely unlocked), and that a wrong passphrase changes nothing.
"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error


class WalletDecryptTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        node = self.nodes[0]

        node.createwallet(wallet_name="w", descriptors=False)
        w = node.get_wallet_rpc("w")
        addr = w.getnewaddress()
        privkey_before = w.dumpprivkey(addr)

        self.log.info("Encrypting the wallet hides the key behind a passphrase")
        w.encryptwallet("passphrase")
        assert_raises_rpc_error(-13, "Error: Please enter the wallet passphrase with walletpassphrase first",
                                w.dumpprivkey, addr)

        self.log.info("A wrong passphrase must leave the wallet exactly as it was")
        assert_raises_rpc_error(-14, "The wallet passphrase entered was incorrect.",
                                w.decryptwallet, "not the passphrase")
        assert_equal(w.getwalletinfo()["unlocked_until"], 0)
        assert_raises_rpc_error(-13, "Error: Please enter the wallet passphrase with walletpassphrase first",
                                w.dumpprivkey, addr)

        self.log.info("decryptwallet removes the passphrase and the key still works")
        w.decryptwallet("passphrase")
        assert_equal(w.dumpprivkey(addr), privkey_before)

        self.log.info("...and the wallet is really unencrypted, not merely unlocked")
        # unlocked_until is absent entirely on an unencrypted wallet; an unlocked-but-
        # encrypted one would report a timestamp here. This is the assertion that
        # distinguishes a real decryption from a wallet left unlocked in memory.
        assert "unlocked_until" not in w.getwalletinfo()
        assert_raises_rpc_error(-15, "Error: running with an unencrypted wallet, but walletpassphrase was called.",
                                w.walletpassphrase, "passphrase", 10)
        # Nothing to lock any more.
        assert_raises_rpc_error(-15, "Error: running with an unencrypted wallet, but walletlock was called.",
                                w.walletlock)

        self.log.info("The decryption survives a restart -- it was written to disk")
        w.unloadwallet()
        self.restart_node(0)
        node.loadwallet("w")
        w = node.get_wallet_rpc("w")
        assert "unlocked_until" not in w.getwalletinfo()
        assert_equal(w.dumpprivkey(addr), privkey_before)

        self.log.info("Calling it again on an already-plaintext wallet is refused")
        assert_raises_rpc_error(-15, "Error: running with an unencrypted wallet, but decryptwallet was called.",
                                w.decryptwallet, "passphrase")

        self.log.info("It can be encrypted again afterwards")
        w.encryptwallet("second passphrase")
        assert_raises_rpc_error(-13, "Error: Please enter the wallet passphrase with walletpassphrase first",
                                w.dumpprivkey, addr)
        w.decryptwallet("second passphrase")
        assert_equal(w.dumpprivkey(addr), privkey_before)

        self.log.info("Descriptor wallets are refused rather than silently doing nothing")
        node.createwallet(wallet_name="d", descriptors=True, passphrase="passphrase")
        d = node.get_wallet_rpc("d")
        assert_raises_rpc_error(-16, "Error: decryptwallet is not supported for descriptor wallets.",
                                d.decryptwallet, "passphrase")

        self.log.info("The staking switch is remembered across a restart")
        assert_equal(w.setstaking(False), False)
        w.unloadwallet()
        self.restart_node(0)
        node.loadwallet("w")
        w = node.get_wallet_rpc("w")
        assert_equal(w.getstakinginfo()["enabled"], True)  # -staking, the global gate
        assert_equal(w.getstakingstatus()["walletstakingenabled"], False)

        self.log.info("...and turning it back on is remembered too")
        assert_equal(w.setstaking(True), True)
        w.unloadwallet()
        self.restart_node(0)
        node.loadwallet("w")
        w = node.get_wallet_rpc("w")
        assert_equal(w.getstakingstatus()["walletstakingenabled"], True)


if __name__ == '__main__':
    WalletDecryptTest().main()
