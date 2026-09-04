// Copyright (c) 2014-2019 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <net.h>
#include <signet.h>
#include <validation.h>

#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(validation_tests, TestingSetup)

// This chain does not halve, so upstream's "every step is half the one before" no
// longer describes GetBlockSubsidy at all. It has two phases:
//
//   heights 1 .. 4*BPSRewardMatchStep   50 COIN, halved once per BPSRewardMatchStep
//   above that                          6.25 COIN, reduced by 25% per nSubsidyHalvingInterval,
//                                       and zero once that has happened 64 times
//
// Only the first phase has exact, parameter-independent values (they are integer
// shifts). The second multiplies by pow(0.75, n) in floating point and truncates, and
// where the phases meet depends on how BPSRewardMatchStep and nSubsidyHalvingInterval
// relate -- on regtest the interval is shorter than the step, so the second phase does
// not begin at 6.25. So the second phase is checked by its properties rather than by
// restating the formula, which would only assert that the code equals itself.
static void CheckSubsidySchedule(const Consensus::Params& consensusParams)
{
    const CAmount nInitialSubsidy = 50 * COIN;

    // Phase one: four exact halvings, and each one holds for its whole step.
    for (int step = 0; step <= 3; ++step) {
        const CAmount expected = nInitialSubsidy >> step;
        BOOST_CHECK_EQUAL(GetBlockSubsidy(step * consensusParams.BPSRewardMatchStep + 1, consensusParams), expected);
        BOOST_CHECK_EQUAL(GetBlockSubsidy((step + 1) * consensusParams.BPSRewardMatchStep, consensusParams), expected);
    }

    // Phase two: never rises, never exceeds the initial subsidy, and ends at zero
    // rather than trailing off into dust.
    CAmount nPreviousSubsidy = nInitialSubsidy;
    for (int r = 0; r <= 64; ++r) {
        const int nHeight = consensusParams.BPSRewardMatchHeight + r * consensusParams.nSubsidyHalvingInterval + 1;
        const CAmount nSubsidy = GetBlockSubsidy(nHeight, consensusParams);
        BOOST_CHECK(nSubsidy <= nInitialSubsidy);
        BOOST_CHECK(nSubsidy <= nPreviousSubsidy);
        nPreviousSubsidy = nSubsidy;
    }
    BOOST_CHECK_EQUAL(GetBlockSubsidy(consensusParams.BPSRewardMatchHeight + 64 * consensusParams.nSubsidyHalvingInterval + 1, consensusParams), 0);

    // And it never goes back up anywhere in between.
    nPreviousSubsidy = GetBlockSubsidy(1, consensusParams);
    for (int nHeight = 1; nHeight < 4000000; nHeight += 997) {
        const CAmount nSubsidy = GetBlockSubsidy(nHeight, consensusParams);
        BOOST_CHECK(nSubsidy <= nPreviousSubsidy);
        BOOST_CHECK(nSubsidy <= nInitialSubsidy);
        nPreviousSubsidy = nSubsidy;
    }
}

BOOST_AUTO_TEST_CASE(block_subsidy_test)
{
    // Upstream also called this with a bare Consensus::Params carrying nothing but
    // nSubsidyHalvingInterval, to try other intervals. That is undefined behaviour
    // here: BPSRewardMatchStep is a plain int with no initialiser and GetBlockSubsidy
    // divides by it. Only real chain parameters are used now.
    CheckSubsidySchedule(CreateChainParams(*m_node.args, CBaseChainParams::MAIN)->GetConsensus());
    CheckSubsidySchedule(CreateChainParams(*m_node.args, CBaseChainParams::TESTNET)->GetConsensus());
    CheckSubsidySchedule(CreateChainParams(*m_node.args, CBaseChainParams::REGTEST)->GetConsensus());
}

BOOST_AUTO_TEST_CASE(subsidy_limit_test)
{
    const auto chainParams = CreateChainParams(*m_node.args, CBaseChainParams::MAIN);
    CAmount nSum = 0;
    // From height 1, not 0. This chain indexes its reward epochs as
    // (nHeight - 1) / BPSRewardMatchStep, so a sample at height 0 falls in the same
    // epoch as height 1 and the loop counts an extra 1000 blocks at the full 50 COIN.
    // That alone is ~50,000 BPS and pushes the total over MAX_MONEY, which made it look
    // like the schedule overpays. It does not: sampling from 1 lands exactly on every
    // epoch boundary and matches a block-by-block sum to the satoshi.
    for (int nHeight = 1; nHeight < 60000000; nHeight += 1000) {
        CAmount nSubsidy = GetBlockSubsidy(nHeight, chainParams->GetConsensus());
        BOOST_CHECK(nSubsidy <= 50 * COIN);
        nSum += nSubsidy * 1000;
        BOOST_CHECK(MoneyRange(nSum));
    }

    // Emission stops rather than continuing indefinitely.
    BOOST_CHECK_EQUAL(GetBlockSubsidy(60000000, chainParams->GetConsensus()), 0);

    // The total is a change detector, not an independently derived figure: it is what
    // the current schedule pays out, recorded so that an accidental change to
    // GetBlockSubsidy or to the parameters it reads shows up as a failing test rather
    // than as quiet inflation. Sampled every 1000 blocks, which is exact here because
    // BPSRewardMatchStep (40000) and nSubsidyHalvingInterval (700000) are both
    // multiples of 1000.
    // 20,999,999.601 BPS -- just under the 21,000,000 cap, which is plainly deliberate.
    BOOST_CHECK_EQUAL(nSum, CAmount{2099999960100000});
    BOOST_CHECK(MoneyRange(nSum));
}

BOOST_AUTO_TEST_CASE(signet_parse_tests)
{
    ArgsManager signet_argsman;
    signet_argsman.ForceSetArg("-signetchallenge", "51"); // set challenge to OP_TRUE
    const auto signet_params = CreateChainParams(signet_argsman, CBaseChainParams::SIGNET);
    CBlock block;
    BOOST_CHECK(signet_params->GetConsensus().signet_challenge == std::vector<uint8_t>{OP_TRUE});
    CScript challenge{OP_TRUE};

    // empty block is invalid
    BOOST_CHECK(!SignetTxs::Create(block, challenge));
    BOOST_CHECK(!CheckSignetBlockSolution(block, signet_params->GetConsensus()));

    // no witness commitment
    CMutableTransaction cb;
    cb.vout.emplace_back(0, CScript{});
    block.vtx.push_back(MakeTransactionRef(cb));
    block.vtx.push_back(MakeTransactionRef(cb)); // Add dummy tx to excercise merkle root code
    BOOST_CHECK(!SignetTxs::Create(block, challenge));
    BOOST_CHECK(!CheckSignetBlockSolution(block, signet_params->GetConsensus()));

    // no header is treated valid
    std::vector<uint8_t> witness_commitment_section_141{0xaa, 0x21, 0xa9, 0xed};
    for (int i = 0; i < 32; ++i) {
        witness_commitment_section_141.push_back(0xff);
    }
    cb.vout.at(0).scriptPubKey = CScript{} << OP_RETURN << witness_commitment_section_141;
    block.vtx.at(0) = MakeTransactionRef(cb);
    BOOST_CHECK(SignetTxs::Create(block, challenge));
    BOOST_CHECK(CheckSignetBlockSolution(block, signet_params->GetConsensus()));

    // no data after header, valid
    std::vector<uint8_t> witness_commitment_section_325{0xec, 0xc7, 0xda, 0xa2};
    cb.vout.at(0).scriptPubKey = CScript{} << OP_RETURN << witness_commitment_section_141 << witness_commitment_section_325;
    block.vtx.at(0) = MakeTransactionRef(cb);
    BOOST_CHECK(SignetTxs::Create(block, challenge));
    BOOST_CHECK(CheckSignetBlockSolution(block, signet_params->GetConsensus()));

    // Premature end of data, invalid
    witness_commitment_section_325.push_back(0x01);
    witness_commitment_section_325.push_back(0x51);
    cb.vout.at(0).scriptPubKey = CScript{} << OP_RETURN << witness_commitment_section_141 << witness_commitment_section_325;
    block.vtx.at(0) = MakeTransactionRef(cb);
    BOOST_CHECK(!SignetTxs::Create(block, challenge));
    BOOST_CHECK(!CheckSignetBlockSolution(block, signet_params->GetConsensus()));

    // has data, valid
    witness_commitment_section_325.push_back(0x00);
    cb.vout.at(0).scriptPubKey = CScript{} << OP_RETURN << witness_commitment_section_141 << witness_commitment_section_325;
    block.vtx.at(0) = MakeTransactionRef(cb);
    BOOST_CHECK(SignetTxs::Create(block, challenge));
    BOOST_CHECK(CheckSignetBlockSolution(block, signet_params->GetConsensus()));

    // Extraneous data, invalid
    witness_commitment_section_325.push_back(0x00);
    cb.vout.at(0).scriptPubKey = CScript{} << OP_RETURN << witness_commitment_section_141 << witness_commitment_section_325;
    block.vtx.at(0) = MakeTransactionRef(cb);
    BOOST_CHECK(!SignetTxs::Create(block, challenge));
    BOOST_CHECK(!CheckSignetBlockSolution(block, signet_params->GetConsensus()));
}

BOOST_AUTO_TEST_SUITE_END()
