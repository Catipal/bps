// Copyright (c) 2021 The Bitcoin PoS Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

//
// Adversarial tests for the cold staking consensus rules.
//
// Cold staking lets a staking key sign coinstakes for coins it cannot spend. The
// whole safety argument - and therefore the case for keeping a staking key
// unencrypted so a node can stake without a passphrase - rests on three checks
// stopping that key from sending the coins anywhere else:
//
//   CheckColdStake()        script level, via OP_CHECKCOLDSTAKEVERIFY
//   CheckColdStakeOutputs() block level, in ConnectBlock
//   CheckReward()           block level, pins the staker fee to COLD_STAKER_FEE percent
//
// These tests take the attacker's side: each one builds a coinstake that tries to
// move value somewhere the owner did not authorise, and asserts it is rejected.
//

#include <chainparams.h>
#include <consensus/validation.h>
#include <key.h>
#include <miner.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <pubkey.h>
#include <script/standard.h>
#include <test/util/setup_common.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(coldstake_tests, TestingSetup)

namespace {

//! A distinct, valid-looking destination we can pretend belongs to an attacker.
CScript AttackerScript()
{
    CKey key;
    key.MakeNewKey(true);
    return GetScriptForDestination(PKHash(key.GetPubKey()));
}

//! The pay-to-cold-stake script the honest owner's coins live in.
CScript P2CSScript(CKeyID& stakerOut, CKeyID& ownerOut)
{
    CKey staker, owner;
    staker.MakeNewKey(true);
    owner.MakeNewKey(true);
    stakerOut = staker.GetPubKey().GetID();
    ownerOut = owner.GetPubKey().GetID();
    return GetScriptForStakeDelegation(stakerOut, ownerOut);
}

//! Build the coinstake a well-behaved staker would produce:
//!   vout[0] empty marker, vout[1] back to the P2CS script, vout[2] the staker fee.
CMutableTransaction HonestColdStake(const CScript& p2cs, const CScript& feeScript,
                                    CAmount principal, CAmount fee)
{
    CMutableTransaction tx;
    tx.vin.resize(1);
    tx.vin[0].prevout = COutPoint(InsecureRand256(), 0);
    tx.vin[0].scriptSig = CScript() << OP_1;
    tx.vout.resize(3);
    tx.vout[0].SetEmpty();
    tx.vout[1] = CTxOut(principal, p2cs);
    tx.vout[2] = CTxOut(fee, feeScript);
    return tx;
}

} // namespace

// The honest shape must pass, or the tests below prove nothing.
BOOST_AUTO_TEST_CASE(coldstake_honest_is_accepted)
{
    CKeyID staker, owner;
    const CScript p2cs = P2CSScript(staker, owner);
    const CScript fee_script = GetScriptForDestination(PKHash(owner));

    CMutableTransaction tx = HonestColdStake(p2cs, fee_script, 1000 * COIN, 5 * COIN);
    BOOST_CHECK(tx.CheckColdStake(p2cs));
    BOOST_CHECK(CTransaction(tx).CheckColdStake(p2cs));
}

// The core attack: sign a coinstake that sends the principal somewhere else.
BOOST_AUTO_TEST_CASE(coldstake_cannot_redirect_principal)
{
    CKeyID staker, owner;
    const CScript p2cs = P2CSScript(staker, owner);
    const CScript attacker = AttackerScript();

    CMutableTransaction tx = HonestColdStake(p2cs, attacker, 1000 * COIN, 5 * COIN);
    tx.vout[1] = CTxOut(1000 * COIN, attacker); // principal to the attacker

    BOOST_CHECK(!tx.CheckColdStake(p2cs));
    BOOST_CHECK(!CTransaction(tx).CheckColdStake(p2cs));
}

// Sneak a second non-P2CS output in alongside the legitimate fee. Before the
// vin/vout guard was fixed this was not checked at the script level at all: with a
// single input the loop was skipped entirely.
BOOST_AUTO_TEST_CASE(coldstake_cannot_add_a_second_foreign_output)
{
    CKeyID staker, owner;
    const CScript p2cs = P2CSScript(staker, owner);
    const CScript fee_script = GetScriptForDestination(PKHash(owner));
    const CScript attacker = AttackerScript();

    CMutableTransaction tx = HonestColdStake(p2cs, fee_script, 900 * COIN, 5 * COIN);
    tx.vout.push_back(CTxOut(100 * COIN, attacker)); // skimmed off the principal

    BOOST_CHECK_MESSAGE(!tx.CheckColdStake(p2cs),
                        "a coinstake with two foreign outputs must be rejected");
    BOOST_CHECK(!CTransaction(tx).CheckColdStake(p2cs));
}

// The specific shape the broken guard let through: one input, four outputs, with the
// diversion sitting in the middle rather than last.
BOOST_AUTO_TEST_CASE(coldstake_cannot_divert_a_middle_output)
{
    CKeyID staker, owner;
    const CScript p2cs = P2CSScript(staker, owner);
    const CScript attacker = AttackerScript();

    CMutableTransaction tx;
    tx.vin.resize(1); // <= 3 inputs: the old guard skipped the loop here
    tx.vin[0].prevout = COutPoint(InsecureRand256(), 0);
    tx.vin[0].scriptSig = CScript() << OP_1;
    tx.vout.resize(4);
    tx.vout[0].SetEmpty();
    tx.vout[1] = CTxOut(900 * COIN, p2cs);
    tx.vout[2] = CTxOut(100 * COIN, attacker); // diverted, and not the last output
    tx.vout[3] = CTxOut(5 * COIN, GetScriptForDestination(PKHash(owner)));

    BOOST_CHECK_MESSAGE(!tx.CheckColdStake(p2cs),
                        "a diverted middle output must be rejected");
    BOOST_CHECK(!CTransaction(tx).CheckColdStake(p2cs));
}

// Mixed scriptSigs would let an attacker combine their own input with the victim's.
BOOST_AUTO_TEST_CASE(coldstake_requires_uniform_input_scriptsigs)
{
    CKeyID staker, owner;
    const CScript p2cs = P2CSScript(staker, owner);

    CMutableTransaction tx = HonestColdStake(p2cs, GetScriptForDestination(PKHash(owner)),
                                             1000 * COIN, 5 * COIN);
    tx.vin.resize(2);
    tx.vin[1].prevout = COutPoint(InsecureRand256(), 0);
    tx.vin[1].scriptSig = CScript() << OP_2; // differs from vin[0]

    BOOST_CHECK(!tx.CheckColdStake(p2cs));
}

// A transaction that is not a coinstake must never satisfy the rule.
BOOST_AUTO_TEST_CASE(coldstake_rejects_non_coinstake)
{
    CKeyID staker, owner;
    const CScript p2cs = P2CSScript(staker, owner);

    CMutableTransaction tx = HonestColdStake(p2cs, GetScriptForDestination(PKHash(owner)),
                                             1000 * COIN, 5 * COIN);
    tx.vout[0] = CTxOut(1 * COIN, p2cs); // marker no longer empty

    BOOST_CHECK(!tx.CheckColdStake(p2cs));
}

// The block-level rule that backs up the script-level one.
BOOST_AUTO_TEST_CASE(coldstakeoutputs_rejects_a_second_foreign_output)
{
    CKeyID staker, owner;
    const CScript p2cs = P2CSScript(staker, owner);
    const CScript fee_script = GetScriptForDestination(PKHash(owner));
    const CScript attacker = AttackerScript();

    CBlock block;
    CMutableTransaction coinbase;
    coinbase.vin.resize(1);
    coinbase.vout.resize(1);
    coinbase.vout[0].SetEmpty();
    block.vtx.push_back(MakeTransactionRef(coinbase));

    // One foreign output (the fee) is allowed...
    CMutableTransaction ok = HonestColdStake(p2cs, fee_script, 1000 * COIN, 5 * COIN);
    block.vtx.push_back(MakeTransactionRef(ok));
    BOOST_CHECK(CheckColdStakeOutputs(block));

    // ...a second one is not.
    CMutableTransaction bad = HonestColdStake(p2cs, fee_script, 900 * COIN, 5 * COIN);
    bad.vout.push_back(CTxOut(100 * COIN, attacker));
    block.vtx[1] = MakeTransactionRef(bad);
    BOOST_CHECK_MESSAGE(!CheckColdStakeOutputs(block),
                        "CheckColdStakeOutputs must allow at most one non-P2CS output");
}

BOOST_AUTO_TEST_SUITE_END()
