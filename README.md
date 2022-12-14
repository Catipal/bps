Bitcoin PoS Core integration/staging tree
=====================================

https://bitcoinpos.net

What is Bitcoin PoS?
----------------

Bitcoin PoS is an experimental digital currency that enables instant payments to
anyone, anywhere in the world. Bitcoin PoS uses peer-to-peer technology to operate
with no central authority: managing transactions and issuing money are carried
out collectively by the network. Bitcoin PoS Core is the name of open source
software which enables the use of this currency.

Bitcoin PoS Core is built on top of Bitcoin Core. The difference between the two
is the consensus algorithm: Bitcoin PoS Core uses Proof of Stake consensus, whilst
Bitcoin Core uses Proof of Work. Using Proof of Stake as a consensus algorithm is
allowing it not only to scale better and be orders of magnitude more efficient in
terms of power consumption, but it is also lowering the entry barrier for contributing
to the creation of new blocks.

For more information, as well as an immediately useable, binary version of
the Bitcoin PoS Core software, see https://www.bitcoinpos.net/#wallet-bps, or read the
[original whitepaper](https://www.bitcoinpos.net/WhitePaperBPS.pdf).

License
-------

Bitcoin PoS Core is released under the terms of the MIT license. See [COPYING](COPYING) for more
information or see https://opensource.org/licenses/MIT.

Development Process
-------------------

The `master` branch is regularly built and tested, but is not guaranteed to be
completely stable. [Tags](https://github.com/bitcoin-pos/bitcoin-pos/tags) are created
regularly to indicate new official, stable release versions of Bitcoin PoS Core.

The contribution workflow is described in [CONTRIBUTING.md](CONTRIBUTING.md)
and useful hints for developers can be found in [doc/developer-notes.md](doc/developer-notes.md).

Testing
-------

Testing and code review is the bottleneck for development; we get more pull
requests than we can review and test on short notice. Please be patient and help out by testing
other people's pull requests, and remember this is a security-critical project where any mistake might cost people
lots of money.

### Automated Testing

Developers are strongly encouraged to write [unit tests](src/test/README.md) for new code, and to
submit new unit tests for old code. Unit tests can be compiled and run
(assuming they weren't disabled in configure) with: `make check`. Further details on running
and extending unit tests can be found in [/src/test/README.md](/src/test/README.md).

There are also [regression and integration tests](/test), written
in Python, that are run automatically on the build server.
These tests can be run (if the [test dependencies](/test) are installed) with: `test/functional/test_runner.py`

The Travis CI system makes sure that every pull request is built for Windows, Linux, and macOS, and that unit/sanity tests are run automatically.

### Manual Quality Assurance (QA) Testing

Changes should be tested by somebody other than the developer who wrote the
code. This is especially important for large or high-risk changes. It is useful
to add a test plan to the pull request description if testing the changes is
not straightforward.
