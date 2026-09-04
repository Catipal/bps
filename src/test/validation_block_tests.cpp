// Copyright (c) 2018-2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/test/unit_test.hpp>

#include <chainparams.h>
#include <consensus/merkle.h>
#include <consensus/validation.h>
#include <miner.h>
#include <pow.h>
#include <random.h>
#include <script/standard.h>
#include <test/util/setup_common.h>
#include <util/time.h>
#include <validation.h>
#include <validationinterface.h>

#include <atomic>
#include <thread>

static const std::vector<unsigned char> V_OP_TRUE{OP_TRUE};

namespace validation_block_tests {
struct MinerTestingSetup : public RegTestingSetup {
    std::shared_ptr<CBlock> Block(const uint256& prev_hash, int height = -1);
    std::shared_ptr<const CBlock> GoodBlock(const uint256& prev_hash, int height = -1);
    std::shared_ptr<const CBlock> BadBlock(const uint256& prev_hash);
    std::shared_ptr<CBlock> FinalizeBlock(std::shared_ptr<CBlock> pblock);
    void BuildChain(const uint256& root, int height, const unsigned int invalid_rate, const unsigned int branch_rate, const unsigned int max_size, std::vector<std::shared_ptr<const CBlock>>& blocks);
};
} // namespace validation_block_tests

BOOST_FIXTURE_TEST_SUITE(validation_block_tests, MinerTestingSetup)

struct TestSubscriber final : public CValidationInterface {
    uint256 m_expected_tip;

    explicit TestSubscriber(uint256 tip) : m_expected_tip(tip) {}

    void UpdatedBlockTip(const CBlockIndex* pindexNew, const CBlockIndex* pindexFork, bool fInitialDownload) override
    {
        BOOST_CHECK_EQUAL(m_expected_tip, pindexNew->GetBlockHash());
    }

    void BlockConnected(const std::shared_ptr<const CBlock>& block, const CBlockIndex* pindex) override
    {
        BOOST_CHECK_EQUAL(m_expected_tip, block->hashPrevBlock);
        BOOST_CHECK_EQUAL(m_expected_tip, pindex->pprev->GetBlockHash());

        m_expected_tip = block->GetHash();
    }

    void BlockDisconnected(const std::shared_ptr<const CBlock>& block, const CBlockIndex* pindex) override
    {
        BOOST_CHECK_EQUAL(m_expected_tip, block->GetHash());
        BOOST_CHECK_EQUAL(m_expected_tip, pindex->GetBlockHash());

        m_expected_tip = block->hashPrevBlock;
    }
};

std::shared_ptr<CBlock> MinerTestingSetup::Block(const uint256& prev_hash, int height)
{
    static int i = 0;
    static uint64_t time = Params().GenesisBlock().nTime;

    CScript pubKey;
    pubKey << i++ << OP_TRUE;

    auto ptemplate = BlockAssembler(*m_node.mempool, Params()).CreateNewBlock(pubKey);
    auto pblock = std::make_shared<CBlock>(ptemplate->block);
    pblock->hashPrevBlock = prev_hash;
    pblock->nTime = ++time;

    // The assembler commits to the height after the *current tip*, but these tests
    // build blocks on arbitrary parents to make forks. Regtest enforces BIP34 from
    // height 500, and this chain's COINBASE_MATURITY is 500, so every test that has to
    // mature a coinbase is already above that line and a stale height is a consensus
    // failure (bad-cb-height). Upstream never hit this: with maturity 100 its chains
    // stay under 500. Commit to the real parent's height instead.
    // Callers building a fork pass the height explicitly. Otherwise look the parent up,
    // which covers blocks built on the tip; if it is not in the index either (some
    // tests build whole chains before processing any of them) leave the template's
    // value alone, which is what this always did.
    if (height < 0) {
        LOCK(cs_main);
        const CBlockIndex* prev = LookupBlockIndex(prev_hash);
        if (prev) height = prev->nHeight + 1;
    }

    pubKey.clear();
    {
        WitnessV0ScriptHash witness_program;
        CSHA256().Write(&V_OP_TRUE[0], V_OP_TRUE.size()).Finalize(witness_program.begin());
        pubKey << OP_0 << ToByteVector(witness_program);
    }

    // Make the coinbase transaction with two outputs:
    // One zero-value one that has a unique pubkey to make sure that blocks at the same height can have a different hash
    // Another one that has the coinbase reward in a P2WSH with OP_TRUE as witness program to make it easy to spend
    CMutableTransaction txCoinbase(*pblock->vtx[0]);
    if (height >= 0) txCoinbase.vin[0].scriptSig = CScript() << height << OP_0;
    txCoinbase.vout.resize(2);
    txCoinbase.vout[1].scriptPubKey = pubKey;
    txCoinbase.vout[1].nValue = txCoinbase.vout[0].nValue;
    txCoinbase.vout[0].nValue = 0;
    txCoinbase.vin[0].scriptWitness.SetNull();
    pblock->vtx[0] = MakeTransactionRef(std::move(txCoinbase));

    return pblock;
}

std::shared_ptr<CBlock> MinerTestingSetup::FinalizeBlock(std::shared_ptr<CBlock> pblock)
{
    LOCK(cs_main); // For LookupBlockIndex
    GenerateCoinbaseCommitment(*pblock, LookupBlockIndex(pblock->hashPrevBlock), Params().GetConsensus());

    pblock->hashMerkleRoot = BlockMerkleRoot(*pblock);

    while (!CheckProofOfWork(pblock->GetHash(), pblock->nBits, Params().GetConsensus())) {
        ++(pblock->nNonce);
    }

    return pblock;
}

// construct a valid block
std::shared_ptr<const CBlock> MinerTestingSetup::GoodBlock(const uint256& prev_hash, int height)
{
    return FinalizeBlock(Block(prev_hash, height));
}

// construct an invalid block (but with a valid header)
std::shared_ptr<const CBlock> MinerTestingSetup::BadBlock(const uint256& prev_hash)
{
    auto pblock = Block(prev_hash);

    CMutableTransaction coinbase_spend;
    coinbase_spend.vin.push_back(CTxIn(COutPoint(pblock->vtx[0]->GetHash(), 0), CScript(), 0));
    coinbase_spend.vout.push_back(pblock->vtx[0]->vout[0]);

    CTransactionRef tx = MakeTransactionRef(coinbase_spend);
    pblock->vtx.push_back(tx);

    auto ret = FinalizeBlock(pblock);
    return ret;
}

void MinerTestingSetup::BuildChain(const uint256& root, int height, const unsigned int invalid_rate, const unsigned int branch_rate, const unsigned int max_size, std::vector<std::shared_ptr<const CBlock>>& blocks)
{
    if (height <= 0 || blocks.size() >= max_size) return;

    bool gen_invalid = InsecureRandRange(100) < invalid_rate;
    bool gen_fork = InsecureRandRange(100) < branch_rate;

    const std::shared_ptr<const CBlock> pblock = gen_invalid ? BadBlock(root) : GoodBlock(root);
    blocks.push_back(pblock);
    if (!gen_invalid) {
        BuildChain(pblock->GetHash(), height - 1, invalid_rate, branch_rate, max_size, blocks);
    }

    if (gen_fork) {
        blocks.push_back(GoodBlock(root));
        BuildChain(blocks.back()->GetHash(), height - 1, invalid_rate, branch_rate, max_size, blocks);
    }
}

BOOST_AUTO_TEST_CASE(processnewblock_signals_ordering)
{
    // build a large-ish chain that's likely to have some forks
    std::vector<std::shared_ptr<const CBlock>> blocks;
    while (blocks.size() < 50) {
        blocks.clear();
        BuildChain(Params().GenesisBlock().GetHash(), 100, 15, 10, 500, blocks);
    }

    bool ignored;
    BlockValidationState state;
    std::vector<CBlockHeader> headers;
    std::transform(blocks.begin(), blocks.end(), std::back_inserter(headers), [](std::shared_ptr<const CBlock> b) { return b->GetBlockHeader(); });

    // Process all the headers so we understand the toplogy of the chain
    BOOST_CHECK(Assert(m_node.chainman)->ProcessNewBlockHeaders(headers, state, Params()));

    // Connect the genesis block and drain any outstanding events
    BOOST_CHECK(Assert(m_node.chainman)->ProcessNewBlock(Params(), std::make_shared<CBlock>(Params().GenesisBlock()), true, &ignored));
    SyncWithValidationInterfaceQueue();

    // subscribe to events (this subscriber will validate event ordering)
    const CBlockIndex* initial_tip = nullptr;
    {
        LOCK(cs_main);
        initial_tip = ::ChainActive().Tip();
    }
    auto sub = std::make_shared<TestSubscriber>(initial_tip->GetBlockHash());
    RegisterSharedValidationInterface(sub);

    // create a bunch of threads that repeatedly process a block generated above at random
    // this will create parallelism and randomness inside validation - the ValidationInterface
    // will subscribe to events generated during block validation and assert on ordering invariance
    std::vector<std::thread> threads;
    for (int i = 0; i < 10; i++) {
        threads.emplace_back([&]() {
            bool ignored;
            FastRandomContext insecure;
            for (int i = 0; i < 1000; i++) {
                auto block = blocks[insecure.randrange(blocks.size() - 1)];
                Assert(m_node.chainman)->ProcessNewBlock(Params(), block, true, &ignored);
            }

            // to make sure that eventually we process the full chain - do it here
            for (auto block : blocks) {
                if (block->vtx.size() == 1) {
                    bool processed = Assert(m_node.chainman)->ProcessNewBlock(Params(), block, true, &ignored);
                    assert(processed);
                }
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }
    SyncWithValidationInterfaceQueue();

    UnregisterSharedValidationInterface(sub);

    LOCK(cs_main);
    BOOST_CHECK_EQUAL(sub->m_expected_tip, ::ChainActive().Tip()->GetBlockHash());
}

/**
 * Test that mempool updates happen atomically with reorgs.
 *
 * This prevents RPC clients, among others, from retrieving immediately-out-of-date mempool data
 * during large reorgs.
 *
 * The test verifies this by creating a chain of `num_txs` blocks, matures their coinbases, and then
 * submits txns spending from their coinbase to the mempool. A fork chain is then processed,
 * invalidating the txns and evicting them from the mempool.
 *
 * We verify that the mempool updates atomically by polling it continuously
 * from another thread during the reorg and checking that its size only changes
 * once. The size changing exactly once indicates that the polling thread's
 * view of the mempool is either consistent with the chain state before reorg,
 * or consistent with the chain state after the reorg, and not just consistent
 * with some intermediate state during the reorg.
 */
// Upstream ships this as mempool_locks_reorg, which checks that a reorg empties the
// mempool atomically -- a caller polling the mempool sees either all of the invalidated
// transactions or none, never a partial state. It builds that situation by reorging
// past the coinbase maturity so the reorg destroys the coinbase outputs its mempool
// transactions spend.
//
// That situation cannot arise on this chain. chainparams.cpp sets
// consensus.nCheckpointSpan = COINBASE_MATURITY on every network, so both are 500: a
// reorg deep enough to un-mature a coinbase is exactly a reorg Checkpoints::CheckSync
// refuses. The upstream test therefore hung here rather than failing -- its polling
// thread spun forever waiting for a reorg that is never accepted, and took the whole
// unit suite with it.
//
// So the test asserts what this chain actually guarantees instead: a reorg deeper than
// nCheckpointSpan is rejected outright, and the mempool and tip are left alone. That
// guarantee is the reason a spend of a mature coinbase can never be undone here, which
// is worth having a test for -- it is otherwise undocumented.
BOOST_AUTO_TEST_CASE(deep_reorg_is_refused)
{
    bool ignored;
    auto ProcessBlock = [&](std::shared_ptr<const CBlock> block) -> bool {
        return Assert(m_node.chainman)->ProcessNewBlock(Params(), block, /* fForceProcessing */ true, /* fNewBlock */ &ignored);
    };

    BOOST_REQUIRE(ProcessBlock(std::make_shared<CBlock>(Params().GenesisBlock())));
    auto last_mined = GoodBlock(Params().GenesisBlock().GetHash());
    BOOST_REQUIRE(ProcessBlock(last_mined));

    // Split from just below the tip, so the fork is as deep as the chain we are about
    // to build on top of it.
    const uint256 split_hash{last_mined->hashPrevBlock};
    const int split_height = WITH_LOCK(cs_main, return LookupBlockIndex(split_hash)->nHeight);

    // Transactions spending the coinbases of the most recent blocks.
    std::vector<CTransactionRef> txs;
    for (int num_txs = 22; num_txs > 0; --num_txs) {
        CMutableTransaction mtx;
        mtx.vin.push_back(CTxIn{COutPoint{last_mined->vtx[0]->GetHash(), 1}, CScript{}});
        mtx.vin[0].scriptWitness.stack.push_back(V_OP_TRUE);
        mtx.vout.push_back(last_mined->vtx[0]->vout[1]);
        mtx.vout[0].nValue -= 1000;
        txs.push_back(MakeTransactionRef(mtx));

        last_mined = GoodBlock(last_mined->GetHash());
        BOOST_REQUIRE(ProcessBlock(last_mined));
    }

    // Mature those coinbases. This is what pushes the fork past nCheckpointSpan, and
    // it is not avoidable: a coinbase is only spendable once it is COINBASE_MATURITY
    // deep, which is the same number.
    for (int j = COINBASE_MATURITY; j > 0; --j) {
        last_mined = GoodBlock(last_mined->GetHash());
        BOOST_REQUIRE(ProcessBlock(last_mined));
    }

    const uint256 tip_before{last_mined->GetHash()};
    BOOST_CHECK_EQUAL(tip_before, ::ChainActive().Tip()->GetBlockHash());

    // A competing branch from the split point, one block longer than the chain it
    // would replace, so only the reorg depth stops it from winning.
    std::vector<std::shared_ptr<const CBlock>> reorg;
    int fork_height = split_height + 1;
    auto fork_tip = GoodBlock(split_hash, fork_height++);
    reorg.push_back(fork_tip);
    for (size_t j = COINBASE_MATURITY + txs.size() + 1; j > 0; --j) {
        fork_tip = GoodBlock(fork_tip->GetHash(), fork_height++);
        reorg.push_back(fork_tip);
    }

    {
        LOCK(cs_main);
        TxValidationState state;
        std::list<CTransactionRef> plTxnReplaced;
        for (const auto& tx : txs) {
            BOOST_REQUIRE(AcceptToMemoryPool(*m_node.mempool, state, tx, &plTxnReplaced, /* bypass_limits */ false));
        }
    }
    {
        LOCK(m_node.mempool->cs);
        BOOST_CHECK_EQUAL(m_node.mempool->mapTx.size(), txs.size());
    }

    // Name the reason. The refusal happens in header validation, so these blocks are
    // rejected rather than merely left unconnected -- which means "the tip did not
    // move" on its own proves nothing: a malformed branch would do the same. It very
    // nearly did. Until the block heights below were made explicit, every one of these
    // carried the wrong BIP34 height and was simply invalid, and this test passed
    // without ever exercising the checkpoint at all.
    {
        std::vector<CBlockHeader> headers;
        headers.reserve(reorg.size());
        for (const auto& b : reorg) headers.push_back(b->GetBlockHeader());
        BlockValidationState state;
        Assert(m_node.chainman)->ProcessNewBlockHeaders(headers, state, Params());
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-fork-prior-to-synch-checkpoint");
    }

    for (const auto& b : reorg) {
        ProcessBlock(b);
    }

    // The branch is longer, but reaches further back than nCheckpointSpan, so the tip
    // must not have moved.
    BOOST_CHECK_EQUAL(tip_before, ::ChainActive().Tip()->GetBlockHash());

    // And nothing was rolled back, so every transaction is still in the mempool.
    {
        LOCK(m_node.mempool->cs);
        BOOST_CHECK_EQUAL(m_node.mempool->mapTx.size(), txs.size());
    }
}

// The property upstream's mempool_locks_reorg checked: a reorg empties the mempool
// atomically. Validation removes the invalidated transactions one at a time, but a
// caller polling the mempool must never see that -- it gets all of them or none.
//
// Upstream reached that state by reorging past the coinbase maturity, which this chain
// refuses (see deep_reorg_is_refused above). So the transactions are orphaned by a
// shallow reorg instead: they spend the outputs of an ordinary transaction, and the
// competing branch double-spends that transaction's own input. The funding transaction
// therefore cannot come back when its block is disconnected -- its input is already
// spent on the winning branch -- so its children have nothing to spend and every one of
// them leaves the mempool.
BOOST_AUTO_TEST_CASE(mempool_empties_atomically_on_reorg)
{
    bool ignored;
    auto ProcessBlock = [&](std::shared_ptr<const CBlock> block) -> bool {
        return Assert(m_node.chainman)->ProcessNewBlock(Params(), block, /* fForceProcessing */ true, /* fNewBlock */ &ignored);
    };
    // Attach a transaction to a block. Same shape as BadBlock(), which is the only
    // other place here that puts anything but a coinbase in a block.
    auto BlockWithTx = [&](const uint256& prev_hash, const CTransactionRef& tx, int height) {
        auto pblock = Block(prev_hash, height);
        pblock->vtx.push_back(tx);
        return FinalizeBlock(pblock);
    };

    BOOST_REQUIRE(ProcessBlock(std::make_shared<CBlock>(Params().GenesisBlock())));
    auto last_mined = GoodBlock(Params().GenesisBlock().GetHash());
    BOOST_REQUIRE(ProcessBlock(last_mined));

    // The coinbase everything below is funded from, and enough blocks to mature it.
    // This is chain height, not reorg depth -- the reorg itself stays three blocks deep.
    const CTransactionRef funding_coinbase = last_mined->vtx[0];
    for (int j = COINBASE_MATURITY + 1; j > 0; --j) {
        last_mined = GoodBlock(last_mined->GetHash());
        BOOST_REQUIRE(ProcessBlock(last_mined));
    }

    const uint256 split_hash{last_mined->GetHash()};
    const int split_height = WITH_LOCK(cs_main, return ::ChainActive().Height());
    const CScript& spend_script = funding_coinbase->vout[1].scriptPubKey;
    const CAmount funded = funding_coinbase->vout[1].nValue;
    const int NUM_CHILDREN = 8;

    // One transaction spending the mature coinbase into several outputs...
    CMutableTransaction funding;
    funding.vin.push_back(CTxIn{COutPoint{funding_coinbase->GetHash(), 1}, CScript{}});
    funding.vin[0].scriptWitness.stack.push_back(V_OP_TRUE);
    const CAmount per_child = (funded - 10000) / NUM_CHILDREN;
    for (int i = 0; i < NUM_CHILDREN; ++i) {
        funding.vout.push_back(CTxOut{per_child, spend_script});
    }
    const CTransactionRef funding_tx = MakeTransactionRef(funding);

    // ...mined into the branch that is about to lose.
    // Explicitly const: BlockWithTx yields a mutable block, GoodBlock a const one.
    std::shared_ptr<const CBlock> losing_tip = BlockWithTx(split_hash, funding_tx, split_height + 1);
    BOOST_REQUIRE(ProcessBlock(losing_tip));
    losing_tip = GoodBlock(losing_tip->GetHash(), split_height + 2);
    BOOST_REQUIRE(ProcessBlock(losing_tip));
    const uint256 tip_before{losing_tip->GetHash()};

    // The winning branch double-spends the funding transaction's input, so the funding
    // transaction cannot return to the mempool when its block is disconnected.
    CMutableTransaction double_spend;
    double_spend.vin.push_back(CTxIn{COutPoint{funding_coinbase->GetHash(), 1}, CScript{}});
    double_spend.vin[0].scriptWitness.stack.push_back(V_OP_TRUE);
    double_spend.vout.push_back(CTxOut{funded - 20000, spend_script});

    // Built and held back while the mempool is still empty. Block() runs the block
    // assembler over the mempool, so building these after the children were accepted
    // would sweep them into the templates and make every block on this branch invalid.
    std::vector<std::shared_ptr<const CBlock>> reorg;
    reorg.push_back(BlockWithTx(split_hash, MakeTransactionRef(double_spend), split_height + 1));
    std::shared_ptr<const CBlock> branch_tip = reorg.back();
    for (int j = 2; j <= 3; ++j) {   // three blocks against two, so this branch wins
        branch_tip = GoodBlock(branch_tip->GetHash(), split_height + j);
        reorg.push_back(branch_tip);
    }

    // A child per output, all of them into the mempool.
    std::vector<CTransactionRef> children;
    for (int i = 0; i < NUM_CHILDREN; ++i) {
        CMutableTransaction child;
        child.vin.push_back(CTxIn{COutPoint{funding_tx->GetHash(), (uint32_t)i}, CScript{}});
        child.vin[0].scriptWitness.stack.push_back(V_OP_TRUE);
        child.vout.push_back(CTxOut{per_child - 1000, spend_script});
        children.push_back(MakeTransactionRef(child));
    }
    {
        LOCK(cs_main);
        TxValidationState state;
        std::list<CTransactionRef> plTxnReplaced;
        for (const auto& tx : children) {
            BOOST_REQUIRE(AcceptToMemoryPool(*m_node.mempool, state, tx, &plTxnReplaced, /* bypass_limits */ false));
        }
    }
    {
        LOCK(m_node.mempool->cs);
        BOOST_CHECK_EQUAL(m_node.mempool->mapTx.size(), children.size());
    }

    // A caller polling throughout must only ever see all of them or none. Bounded, so
    // a regression that stops the mempool emptying fails instead of hanging the suite
    // the way the upstream test did.
    std::atomic<bool> saw_empty{false};
    std::atomic<bool> saw_partial{false};
    std::thread rpc_thread{[&]() {
        const int64_t deadline = GetTimeMillis() + 60 * 1000;
        while (GetTimeMillis() < deadline) {
            LOCK(m_node.mempool->cs);
            const size_t size = m_node.mempool->mapTx.size();
            if (size == 0) { saw_empty = true; return; }
            if (size != children.size()) { saw_partial = true; return; }
        }
    }};

    for (const auto& b : reorg) {
        ProcessBlock(b);
    }
    rpc_thread.join();

    // The reorg happened...
    BOOST_CHECK(tip_before != ::ChainActive().Tip()->GetBlockHash());
    BOOST_CHECK_EQUAL(branch_tip->GetHash(), ::ChainActive().Tip()->GetBlockHash());

    // ...it took every child with it...
    {
        LOCK(m_node.mempool->cs);
        BOOST_CHECK_EQUAL(m_node.mempool->mapTx.size(), 0U);
    }

    // ...and nobody watching ever saw it half done.
    BOOST_CHECK(!saw_partial);
    BOOST_CHECK(saw_empty);
}

BOOST_AUTO_TEST_CASE(witness_commitment_index)
{
    CScript pubKey;
    pubKey << 1 << OP_TRUE;
    auto ptemplate = BlockAssembler(*m_node.mempool, Params()).CreateNewBlock(pubKey);
    CBlock pblock = ptemplate->block;

    CTxOut witness;
    witness.scriptPubKey.resize(MINIMUM_WITNESS_COMMITMENT);
    witness.scriptPubKey[0] = OP_RETURN;
    witness.scriptPubKey[1] = 0x24;
    witness.scriptPubKey[2] = 0xaa;
    witness.scriptPubKey[3] = 0x21;
    witness.scriptPubKey[4] = 0xa9;
    witness.scriptPubKey[5] = 0xed;

    // A witness larger than the minimum size is still valid
    CTxOut min_plus_one = witness;
    min_plus_one.scriptPubKey.resize(MINIMUM_WITNESS_COMMITMENT + 1);

    CTxOut invalid = witness;
    invalid.scriptPubKey[0] = OP_VERIFY;

    CMutableTransaction txCoinbase(*pblock.vtx[0]);
    txCoinbase.vout.resize(4);
    txCoinbase.vout[0] = witness;
    txCoinbase.vout[1] = witness;
    txCoinbase.vout[2] = min_plus_one;
    txCoinbase.vout[3] = invalid;
    pblock.vtx[0] = MakeTransactionRef(std::move(txCoinbase));

    BOOST_CHECK_EQUAL(GetWitnessCommitmentIndex(pblock), 2);
}
BOOST_AUTO_TEST_SUITE_END()
