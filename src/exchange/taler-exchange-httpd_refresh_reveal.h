/*
  This file is part of TALER
  Copyright (C) 2014-2017 Taler Systems SA

  TALER is free software; you can redistribute it and/or modify it under the
  terms of the GNU Affero General Public License as published by the Free Software
  Foundation; either version 3, or (at your option) any later version.

  TALER is distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
  A PARTICULAR PURPOSE.  See the GNU Affero General Public License for more details.

  You should have received a copy of the GNU Affero General Public License along with
  TALER; see the file COPYING.  If not, see <http://www.gnu.org/licenses/>
*/
/**
 * @file taler-exchange-httpd_refresh_reveal.h
 * @brief Handle /refresh/reveal requests
 * @author Florian Dold
 * @author Benedikt Mueller
 * @author Christian Grothoff
 */
#ifndef TALER_EXCHANGE_HTTPD_REFRESH_REVEAL_H
#define TALER_EXCHANGE_HTTPD_REFRESH_REVEAL_H

#include <gnunet/gnunet_util_lib.h>
#include <microhttpd.h>
#include "taler-exchange-httpd.h"


/**
 * Handle a "/refreshes/$RCH/reveal" request. This time, the client reveals the
 * private transfer keys except for the cut-and-choose value returned from
 * "/coins/$COIN_PUB/melt".  This function parses the revealed keys and secrets and
 * ultimately passes everything to #resolve_refresh_reveal_denominations()
 * which will verify that the revealed information is valid then runs the
 * transaction in #refresh_reveal_transaction() and finally returns the signed
 * refreshed coins.
 *
 * @param rh context of the handler
 * @param coin_pub public key of the coin
 * @param root uploaded JSON data
 * @param args array of additional options (length: 2, session hash and the string "reveal")
 * @return MHD result code
 */
int
TEH_REFRESH_handler_reveal (const struct TEH_RequestHandler *rh,
                            struct MHD_Connection *connection,
                            const json_t *root,
                            const char *const args[2]);


#endif
