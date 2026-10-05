#!/usr/bin/env python3
# Copyright (c) 2026 The Bitcoin PoS developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Join the hard-wrapped lines of release notes for a GitHub release page.

doc/release-notes.md is wrapped for reading in a terminal. A release page turns
every line break into a visible one, so each paragraph and list item is joined
back into a single line. Headings, their underlines and blank lines are kept.
"""

import re
import sys

RULE = re.compile(r'[=-]{3,}$')
ITEM = re.compile(r'\s*([-*]|\d+\.)\s')


def unwrap(text):
    out = []
    for line in text.split('\n'):
        starts_block = not line.strip() or ITEM.match(line) or RULE.match(line.strip())
        if out and out[-1].strip() and not RULE.match(out[-1].strip()) and not starts_block:
            out[-1] = out[-1].rstrip() + ' ' + line.strip()
        else:
            out.append(line)
    return '\n'.join(out)


if __name__ == '__main__':
    with open(sys.argv[1], encoding='utf8') as f:
        sys.stdout.write(unwrap(f.read()))
