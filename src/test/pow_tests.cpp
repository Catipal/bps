// Copyright (c) 2015-2019 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <arith_uint256.h>
#include <chain.h>
#include <chainparams.h>
#include <pow.h>
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(pow_tests, BasicTestingSetup)

// This chain does not use Bitcoin's retarget, so upstream's expected nBits -- read off
// Bitcoin mainnet blocks 30240, 32255, 66528 and 46367 -- describe an algorithm that
// does not run here. CalculateNextWorkRequired is the exponential adjustment
// (mul_exp): it nudges the target by 2*(actual_spacing - target_spacing)/16 over a
// window of (interval+1)*target_spacing/16, against a 3 minute target spacing rather
// than 10 minutes. There is no 4x clamp either; the observed spacing is capped at 20x
// the target instead.
//
// What follows are the properties that algorithm does have, which hold whatever the
// parameters are set to and so are worth asserting.

namespace {
//! A previous-block index that retargets against a parent nFirstBlockTime seconds back.
CBlockIndex IndexAt(int height, int64_t time, unsigned int bits)
{
    CBlockIndex index;
    index.nHeight = height;
    index.nTime = time;
    index.nBits = bits;
    return index;
}
//! Above BPSRewardMatchHeight, so nPowTargetSpacing applies rather than
//! STAKE_V1_TARGET_SPACING, which the code uses for early blocks.
const int LATE_HEIGHT = 200000;
} // namespace

/* Blocks arriving exactly on schedule leave the difficulty alone. */
BOOST_AUTO_TEST_CASE(retarget_on_target_is_a_no_op)
{
    const auto chainParams = CreateChainParams(*m_node.args, CBaseChainParams::MAIN);
    const int64_t spacing = chainParams->GetConsensus().nPowTargetSpacing;
    const int64_t now = 1600000000;
    CBlockIndex pindexLast = IndexAt(LATE_HEIGHT, now, 0x1d00ffff);
    BOOST_CHECK_EQUAL(CalculateNextWorkRequired(&pindexLast, now - spacing, chainParams->GetConsensus()),
                      0x1d00ffffU);
}

/* Blocks arriving faster than the target make the next one harder, slower makes it
   easier. A smaller compact target is harder. */
BOOST_AUTO_TEST_CASE(retarget_follows_block_rate)
{
    const auto chainParams = CreateChainParams(*m_node.args, CBaseChainParams::MAIN);
    const int64_t spacing = chainParams->GetConsensus().nPowTargetSpacing;
    const int64_t now = 1600000000;
    const unsigned int start_bits = 0x1c05a3f4;   // well away from the limit either way
    CBlockIndex pindexLast = IndexAt(LATE_HEIGHT, now, start_bits);

    const unsigned int fast = CalculateNextWorkRequired(&pindexLast, now - spacing / 4, chainParams->GetConsensus());
    const unsigned int slow = CalculateNextWorkRequired(&pindexLast, now - spacing * 4, chainParams->GetConsensus());

    arith_uint256 t_start, t_fast, t_slow;
    t_start.SetCompact(start_bits);
    t_fast.SetCompact(fast);
    t_slow.SetCompact(slow);

    BOOST_CHECK(t_fast < t_start);   // came in early -> harder
    BOOST_CHECK(t_slow > t_start);   // came in late  -> easier
}

/* A long gap cannot slacken the difficulty without bound: the observed spacing is
   capped at 20x the target, so a week-long gap counts the same as a 20x one. */
BOOST_AUTO_TEST_CASE(retarget_caps_a_long_gap)
{
    const auto chainParams = CreateChainParams(*m_node.args, CBaseChainParams::MAIN);
    const int64_t spacing = chainParams->GetConsensus().nPowTargetSpacing;
    const int64_t now = 1600000000;
    CBlockIndex pindexLast = IndexAt(LATE_HEIGHT, now, 0x1c05a3f4);

    const unsigned int at_cap = CalculateNextWorkRequired(&pindexLast, now - spacing * 20, chainParams->GetConsensus());
    const unsigned int past_cap = CalculateNextWorkRequired(&pindexLast, now - spacing * 20000, chainParams->GetConsensus());
    BOOST_CHECK_EQUAL(at_cap, past_cap);
}

/* Time running backwards is treated as an on-target spacing rather than making the
   difficulty jump. */
BOOST_AUTO_TEST_CASE(retarget_ignores_negative_spacing)
{
    const auto chainParams = CreateChainParams(*m_node.args, CBaseChainParams::MAIN);
    const int64_t spacing = chainParams->GetConsensus().nPowTargetSpacing;
    const int64_t now = 1600000000;
    CBlockIndex pindexLast = IndexAt(LATE_HEIGHT, now, 0x1c05a3f4);

    const unsigned int backwards = CalculateNextWorkRequired(&pindexLast, now + 5000, chainParams->GetConsensus());
    const unsigned int on_target = CalculateNextWorkRequired(&pindexLast, now - spacing, chainParams->GetConsensus());
    BOOST_CHECK_EQUAL(backwards, on_target);
}

/* However slowly blocks arrive, the result is never easier than the limit. */
BOOST_AUTO_TEST_CASE(retarget_never_exceeds_the_limit)
{
    const auto chainParams = CreateChainParams(*m_node.args, CBaseChainParams::MAIN);
    const auto& consensus = chainParams->GetConsensus();
    const int64_t now = 1600000000;
    arith_uint256 limit;
    limit.SetCompact(UintToArith256(consensus.powLimit).GetCompact());

    CBlockIndex pindexLast = IndexAt(LATE_HEIGHT, now, limit.GetCompact());
    const unsigned int next = CalculateNextWorkRequired(&pindexLast, now - consensus.nPowTargetSpacing * 20000, consensus);
    arith_uint256 t_next;
    t_next.SetCompact(next);
    BOOST_CHECK(t_next <= limit);
}

BOOST_AUTO_TEST_CASE(GetBlockProofEquivalentTime_test)
{
    const auto chainParams = CreateChainParams(*m_node.args, CBaseChainParams::MAIN);
    std::vector<CBlockIndex> blocks(10000);
    for (int i = 0; i < 10000; i++) {
        blocks[i].pprev = i ? &blocks[i - 1] : nullptr;
        blocks[i].nHeight = i;
        blocks[i].nTime = 1269211443 + i * chainParams->GetConsensus().nPowTargetSpacing;
        blocks[i].nBits = 0x207fffff; /* target 0x7fffff000... */
        blocks[i].nChainWork = i ? blocks[i - 1].nChainWork + GetBlockProof(blocks[i - 1]) : arith_uint256(0);
    }

    for (int j = 0; j < 1000; j++) {
        CBlockIndex *p1 = &blocks[InsecureRandRange(10000)];
        CBlockIndex *p2 = &blocks[InsecureRandRange(10000)];
        CBlockIndex *p3 = &blocks[InsecureRandRange(10000)];

        int64_t tdiff = GetBlockProofEquivalentTime(*p1, *p2, *p3, chainParams->GetConsensus());
        BOOST_CHECK_EQUAL(tdiff, p1->GetBlockTime() - p2->GetBlockTime());
    }
}

void sanity_check_chainparams(const ArgsManager& args, std::string chainName)
{
    const auto chainParams = CreateChainParams(args, chainName);
    const auto consensus = chainParams->GetConsensus();

    // hash genesis is correct
    BOOST_CHECK_EQUAL(consensus.hashGenesisBlock, chainParams->GenesisBlock().GetHash());

    // target timespan is an even multiple of spacing
    BOOST_CHECK_EQUAL(consensus.nPowTargetTimespan % consensus.nPowTargetSpacing, 0);

    // genesis nBits is positive, doesn't overflow and is lower than powLimit
    arith_uint256 pow_compact;
    bool neg, over;
    pow_compact.SetCompact(chainParams->GenesisBlock().nBits, &neg, &over);
    BOOST_CHECK(!neg && pow_compact != 0);
    BOOST_CHECK(!over);
    BOOST_CHECK(UintToArith256(consensus.powLimit) >= pow_compact);

    // check max target * 4*nPowTargetTimespan doesn't overflow -- see pow.cpp:CalculateNextWorkRequired()
    if (!consensus.fPowNoRetargeting) {
        arith_uint256 targ_max("0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF");
        targ_max /= consensus.nPowTargetTimespan*4;
        BOOST_CHECK(UintToArith256(consensus.powLimit) < targ_max);
    }
}

BOOST_AUTO_TEST_CASE(ChainParams_MAIN_sanity)
{
    sanity_check_chainparams(*m_node.args, CBaseChainParams::MAIN);
}

BOOST_AUTO_TEST_CASE(ChainParams_REGTEST_sanity)
{
    sanity_check_chainparams(*m_node.args, CBaseChainParams::REGTEST);
}

BOOST_AUTO_TEST_CASE(ChainParams_TESTNET_sanity)
{
    sanity_check_chainparams(*m_node.args, CBaseChainParams::TESTNET);
}

BOOST_AUTO_TEST_CASE(ChainParams_SIGNET_sanity)
{
    sanity_check_chainparams(*m_node.args, CBaseChainParams::SIGNET);
}

BOOST_AUTO_TEST_SUITE_END()
