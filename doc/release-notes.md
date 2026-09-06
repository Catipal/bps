Bitcoin PoS Core version 0.22 Release Notes
===========================================

Bitcoin PoS Core version 0.22 is now available.

This release adds passwordless cold staking, and repairs a number of defects in
the proof-of-stake code that had gone unnoticed since the 0.21 fork, including
seven lock-order bugs that could deadlock a node.

How to upgrade
==============

Shut the node down, replace the binaries, and start it again. The wallet format is
unchanged, so no reindex or rescan is needed, and you can downgrade to 0.21 by
reversing the process. One record is added to the wallet file, recording whether
staking was switched on; a 0.21 node ignores it.

Passwordless cold staking
=========================

An encrypted wallet cannot stake, because its keys are encrypted. Until now the
only options were to leave the wallet unencrypted, or to unlock it by hand after
every restart.

This release makes the intended arrangement work: the staking key is held in a
second, unencrypted wallet, while the coins stay in the encrypted one. Consensus
already restricts a staking key to producing coinstakes that pay back to the owner
(OP_CHECKCOLDSTAKEVERIFY), so a staking wallet readable by anyone cannot spend the
coins it stakes. A node restarts and resumes staking with no passphrase.

New RPCs:

- decryptwallet "passphrase" removes the passphrase from a wallet, leaving the
  private keys in plaintext. This is what puts a staking wallet into the state
  above. Anyone who can read the wallet file afterwards can spend from it, so it is
  meant for wallets holding only a staking key. Legacy wallets only. The GUI has
  offered a Decrypt option for years, but the implementation behind it always
  failed; it works now, and appears as "Decrypt Wallet..." in Settings.

- rotatestakingkey "new_staker_address" ( "owner_address" ) moves every cold-staked
  output whose owner key this wallet holds to a new staking key, in one
  transaction. This is the recovery path if a staking key is exposed: such a key
  cannot steal coins, but it can keep using the stake weight to produce blocks and
  take the staker fee. Only the owner key can authorise the move, so a wallet
  holding just the staking key cannot rotate away from its owner.

- setstaking true|false turns staking on or off at runtime. Previously staking
  could only be set with -staking at startup, or the GUI switch.

The 7% cold-staker fee can now compound: with -coldstakefeetoowner the fee is paid
into a delegation to the same owner rather than to a bare key, so a self-delegating
wallet accumulates its own fees instead of stranding them.

Staking is remembered per wallet across restarts. -staking remains the gate, so
-staking=0 still wins, and the per-wallet record then decides which wallets stake.
A wallet with no record keeps staking, so upgrading does not silently stop anyone.
Staking no longer starts on a locked wallet, which previously left a node reporting
that it was staking while unable to sign anything.

Descriptor wallets can now stake. Cold staking still requires a legacy wallet, as
no descriptor can express a P2CS script.

Fixes
=====

Lock ordering
-------------

Seven cs_main/cs_wallet ordering inversions were fixed. Five predate this release,
in CreateColdStakeDelegation, a wallet listing RPC, getmininginfo, getstakinginfo
and getstakingstatus; two were introduced and removed during this work.
getstakinginfo is polled on a timer by staking nodes, so this was reachable in
ordinary use rather than a corner case.

The correct order on this chain is cs_wallet then cs_main: since 0.21 the wallet
reaches cs_main only through interfaces::Chain, and CWallet::Create records that
order on every startup.

None of this is visible in a normal build. -Wthread-safety is silently inert under
GCC, and DEBUG_LOCKORDER is off unless --enable-debug is passed.

Staking
-------

- The staker read the UTXO set with no cs_main held at all, racing every reorg and
  cache flush.
- The kernel cache was filled inside CreateCoinStake, holding cs_wallet across
  roughly two database reads per coin, once per candidate timestamp. On a wallet
  with many outputs this made getstakinginfo time out. It now runs once per tip
  change, in batches, and never holds cs_wallet across a read.
- The staking thread could not be stopped. interrupt_all() was called against a
  thread whose sleeps were uninterruptible, and the thread group destructor
  detached it, leaving a live thread holding a pointer to a wallet that was about
  to be destroyed.
- CheckColdStake looped over the outputs but bounded the loop with the number of
  inputs, so the script-level cold-stake rule did nothing for coinstakes with three
  or fewer inputs.
- The global scriptsMap was unguarded.

Other
-----

- CURRENCY_UNIT was still "BTC". The GUI had been rebranded but the daemon had not,
  so every RPC help string, fee message and -paytxfee warning quoted the wrong
  currency, across 76 user-visible strings.
- generateblock built and validated a block, never submitted it, and returned an
  uninitialised all-zero hash as if it had succeeded. generatetodescriptor was a
  stub returning an empty array. Both work now.
- The GUI staking switch reported intent rather than state: it enabled staking
  before asking for the passphrase, so cancelling left a locked wallet with staking
  switched on, and its attempt to revert animated the widget without changing
  anything. The switch is now driven from the wallet.
- Choosing "Unlock Wallet" from the menu silently re-applied staking-only mode if
  the wallet had previously been unlocked for staking, leaving the user unable to
  spend despite entering the correct passphrase.

Tests
=====

The functional test framework did not run against this fork at all. It does now,
and ships tests for cold staking, passwordless staking, fee compounding, the
staking toggle, descriptor staking, wallet decryption and staking-key rotation.

The unit suite did not finish. mempool_locks_reorg hung forever, taking everything
after it with it: it reorgs deeper than the coinbase maturity, and this chain sets
nCheckpointSpan = COINBASE_MATURITY, so a reorg deep enough to un-mature a coinbase
is exactly the reorg that is refused. It is replaced by a test of that guarantee,
plus one covering mempool atomicity with a shallow reorg.

Inherited tests asserting Bitcoin values, such as base58 prefixes, block filters
and signed-message vectors, were removed rather than regenerated, since a
regenerated vector only asserts that the code agrees with itself. Tests measuring
real properties with the wrong numbers were corrected, including the emission
schedule, now pinned at 20,999,999.601 BPS against a 21,000,000 cap.

One unit test still fails: miner_tests/CreateNewBlock_validity. Its 110 hardcoded
nonces were mined for Bitcoin parameters, and this chain retargets every 10 blocks
rather than every 2016, so difficulty climbs as fast as blocks are found.

Known issues
============

Three consensus defects are documented and deliberately left unfixed, as changing
any of them requires a hard fork with an activation height:

- bnTarget *= bnWeight can overflow silently, unfairly penalising a large staker
  past a threshold. This is not an inflation bug.
- The early coinstake signature check runs with SCRIPT_VERIFY_NONE. Full script
  verification still happens in ConnectBlock.
- CheckColdStakeOutputs is gated on the active chain height rather than the height
  of the block being validated.

SigNetParams sets neither BPSRewardMatchStep nor BPSRewardMatchHeight, so
GetBlockSubsidy divides by an uninitialised value on signet.

This release has been tested on regtest only. It has not been run on testnet or
mainnet, and has had no external review.
