#!/usr/bin/env python3
# Copyright (c) 2024 The Bitcoin PoS developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test auto-delegation: staking that survives ordinary sends and receives.

A delegation address gets coins staking the moment they arrive, but a wallet that
holds them still has to spend them, and spending is where the delegation used to be
lost: change went to a plain address, so paying anyone left the whole remainder
sitting undelegated. A wallet with a staking address set puts its change back into a
delegation of its own, so sending to and receiving from ordinary addresses does not
take the wallet out of staking.
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


class AutoDelegationTest(BitcoinTestFramework):
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

    def wallet(self, name):
        self.nodes[0].createwallet(wallet_name=name, descriptors=False)
        return self.nodes[0].get_wallet_rpc(name)

    def book(self, w):
        """Receiving addresses the wallet has recorded, as the GUI's Receive tab lists them."""
        try:
            return set(w.getaddressesbylabel(""))
        except Exception:
            return set()

    def keyhash(self, addr):
        """The key hash behind an ordinary address, without minting anything."""
        # P2PKH: OP_DUP OP_HASH160 <20 bytes> OP_EQUALVERIFY OP_CHECKSIG
        return self.nodes[0].validateaddress(addr)["scriptPubKey"][6:46]

    def outs(self, w, txid, kind):
        tx = self.nodes[0].decoderawtransaction(w.gettransaction(txid)["hex"])
        return [o for o in tx["vout"] if o["scriptPubKey"]["type"] == kind]

    def run_test(self):
        node = self.nodes[0]
        miner = self.wallet("miner")
        plain = miner.getnewaddress()
        miner.generatetoaddress(700, plain)

        staker = self.wallet("staker")
        staker_addr = staker.getnewaddress()
        user = self.wallet("user")

        self.log.info("Setting a staking address turns auto-delegation on and is remembered")
        assert_equal(user.getwalletinfo().get("staking_addresses"), None)
        res = user.setstakingaddress(staker_addr)
        assert_equal(res["staking_addresses"], [staker_addr])
        assert_equal(res["auto_delegating"], True)
        assert_equal(user.getwalletinfo()["staking_addresses"], [staker_addr])

        self.log.info("...and survives a wallet reload, so a restart keeps delegating")
        node.unloadwallet("user")
        node.loadwallet("user")
        user = node.get_wallet_rpc("user")
        assert_equal(user.getwalletinfo()["staking_addresses"], [staker_addr])

        self.log.info("getdelegationaddress then needs no argument")
        deleg = user.getdelegationaddress()
        assert_equal(deleg["staker_address"], staker_addr)
        staker_hash = node.validateaddress(deleg["address"])["staker_pubkeyhash"]
        # It is a receiving address like any other, so the GUI can list it.
        assert deleg["address"] in self.book(user)
        # ...and a payment to it is a receive, not change. The wallet used to look
        # the script up by its owner key alone, which is never recorded, so every
        # payment to a delegation address was classed as change.
        assert_equal(user.getaddressinfo(deleg["address"])["ischange"], False)

        self.log.info("Receiving from an ordinary address delegates on arrival")
        # The sender is a stock wallet paying a normal sendtoaddress; nothing about
        # its side is special.
        funding = miner.sendtoaddress(deleg["address"], 1000)
        miner.generatetoaddress(1, plain)
        assert_equal(len(self.outs(miner, funding, "coldstake")), 1)
        assert_equal(user.getwalletinfo()["delegated"], Decimal("1000.00000000"))

        self.log.info("...and the history RPCs show it, under the address that was paid")
        # Delegated coins used to be left out of every history RPC, so this payment was
        # invisible to listtransactions and getreceivedbyaddress reported nothing.
        received = [e for e in user.listtransactions() if e["txid"] == funding]
        assert_equal([(e["category"], e["address"], e["amount"]) for e in received],
                     [("receive", deleg["address"], Decimal("1000"))])
        assert_equal(user.gettransaction(funding)["amount"], Decimal("1000"))
        assert_equal(user.getreceivedbyaddress(deleg["address"]), Decimal("1000"))
        assert_equal({e["address"]: e["amount"] for e in user.listreceivedbyaddress()},
                     {deleg["address"]: Decimal("1000")})
        # The payer sees the address it was given, not the owner key inside it.
        sent = [e for e in miner.listtransactions() if e["txid"] == funding and e["category"] == "send"]
        assert_equal([e["address"] for e in sent], [deleg["address"]])

        self.log.info("Sending to an ordinary address spends the delegated coin without an opt-in")
        # Every coin this wallet holds is delegated. Before auto-delegation those were
        # excluded from coin selection unless asked for, which would have made the
        # wallet look empty and refused the send outright.
        spend = user.sendtoaddress(plain, 10)
        miner.generatetoaddress(1, plain)
        paid = [o for o in self.outs(user, spend, "pubkeyhash")
                if o["value"] == Decimal("10.00000000")]
        assert_equal(len(paid), 1)
        # The spend is in the history with its fee, and the delegated change is not.
        # Every input was delegated, so this used to show as nothing at all.
        sent = [e for e in user.listtransactions() if e["txid"] == spend]
        assert_equal([(e["category"], e["address"], e["amount"]) for e in sent],
                     [("send", plain, Decimal("-10"))])
        tx = user.gettransaction(spend)
        assert_equal(tx["amount"], Decimal("-10"))
        assert_greater_than(Decimal("0"), tx["fee"])
        assert_equal(sent[0]["fee"], tx["fee"])

        self.log.info("...and the change comes back delegated to the same staking key")
        change = self.outs(user, spend, "coldstake")
        assert_equal(len(change), 1)
        assert staker_hash in change[0]["scriptPubKey"]["asm"]
        # Change is not an address anybody asked for, so it stays out of the address
        # book -- otherwise every spend would add a line to the Receive tab.
        assert_equal(self.book(user), {deleg["address"]})
        # Nothing dropped out of staking: the balance moved, the delegation did not.
        info = user.getwalletinfo()
        assert_greater_than(info["delegated"], Decimal("989"))
        assert_equal(info["balance"], Decimal("0E-8"))

        self.log.info("The staking wallet still covers the whole remainder")
        cold = staker.getwalletinfo()
        assert_equal(cold["cold_stake"] + cold["immature_cold_stake"], info["delegated"])
        # It holds none of it: only the owner can spend.
        assert_equal(staker.getbalances()["mine"]["trusted"], Decimal("0E-8"))

        self.log.info("A second staking address spreads new delegations over both")
        second_staker = self.wallet("staker2").getnewaddress()
        added = user.addstakingaddress(second_staker)
        assert_equal(added["staking_addresses"], [staker_addr, second_staker])
        # Adding one already there is refused rather than silently duplicating it, which
        # would weight that key twice in the choice below.
        assert_equal(user.addstakingaddress(staker_addr)["staking_addresses"],
                     [staker_addr, second_staker])

        # The first staker starts two ahead: one address handed out, plus the delegated
        # change from the spend above. So the second takes the next two, the third is a
        # tie that goes to the first by list order, and the fourth swings back.
        first_hash = self.keyhash(staker_addr)
        second_hash = self.keyhash(second_staker)
        assert first_hash != second_hash
        used = [node.validateaddress(user.getdelegationaddress()["address"])["staker_pubkeyhash"]
                for _ in range(4)]
        assert_equal(used, [second_hash, second_hash, first_hash, second_hash])

        self.log.info("...and removing one leaves the other, without moving any coins")
        before = user.getwalletinfo()["delegated"]
        removed = user.removestakingaddress(staker_addr)
        assert_equal(removed["staking_addresses"], [second_staker])
        assert_equal(user.getwalletinfo()["delegated"], before)
        # Every new delegation now names the one that is left, even though it is the one
        # already looking after more.
        assert_equal(node.validateaddress(user.getdelegationaddress()["address"])["staker_pubkeyhash"],
                     second_hash)
        assert_raises_rpc_error(-8, "not one of this wallet", user.removestakingaddress, staker_addr)

        self.log.info("Switching it off stops the wallet reaching for delegated coins")
        assert_equal(user.setstakingaddress("")["auto_delegating"], False)
        assert_equal(user.getwalletinfo().get("staking_addresses"), None)
        assert_raises_rpc_error(-8, "setstakingaddress", user.getdelegationaddress)
        # This is what the wallet does with delegated coins by default, and the reason
        # auto-delegation has to override it: a wallet whose coins are all delegated
        # would otherwise report an empty balance and refuse to send.
        assert_raises_rpc_error(-6, "Insufficient funds", user.sendtoaddress, plain, 10)
        # Turning it back on makes them spendable again; nothing was stranded.
        user.setstakingaddress(staker_addr)
        assert_equal(user.getwalletinfo()["staking_addresses"], [staker_addr])
        again = user.sendtoaddress(plain, 10)
        miner.generatetoaddress(1, plain)
        assert_equal(len(self.outs(user, again, "coldstake")), 1)


if __name__ == '__main__':
    AutoDelegationTest().main()
