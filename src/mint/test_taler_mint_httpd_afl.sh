#!/bin/bash
#
# This file is part of TALER
# Copyright (C) 2015 GNUnet e.V.
#
#  TALER is free software; you can redistribute it and/or modify it under the
#  terms of the GNU Affero General Public License as published by the Free Software
#  Foundation; either version 3, or (at your option) any later version.
#
#  TALER is distributed in the hope that it will be useful, but WITHOUT ANY
#  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
#  A PARTICULAR PURPOSE.  See the GNU Affero General Public License for more details.
#
#  You should have received a copy of the GNU Affero General Public License along with
#  TALER; see the file COPYING.  If not, If not, see <http://www.gnu.org/licenses/>
#
#
# This script uses 'curl' to POST various ill-formed requests to the
# taler-mint-httpd.  Basically, the goal is to make sure that the
# HTTP server survives (and produces the 'correct' error code).
#
# We read the JSON snippets from afl-tests/
#
# Setup keys.
taler-mint-keyup -d test-mint-home -m test-mint-home/master.priv
# Run test...
for n in afl-tests/*.req
do
  echo -n "Test $n"
  taler-mint-httpd -d test-mint-home/ -t 1 -f $n -C > /dev/null || { echo "FAIL!"; exit 1; }
  echo "OK"
done
exit 0
