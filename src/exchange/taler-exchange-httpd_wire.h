/*
  This file is part of TALER
  Copyright (C) 2014--2020 Taler Systems SA

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
 * @file taler-exchange-httpd_wire.h
 * @brief Handle /wire requests
 * @author Christian Grothoff
 */
#ifndef TALER_EXCHANGE_HTTPD_WIRE_H
#define TALER_EXCHANGE_HTTPD_WIRE_H

#include <gnunet/gnunet_util_lib.h>
#include <microhttpd.h>
#include "taler-exchange-httpd.h"


/**
 * Initialize wire subsystem.
 *
 * @param cfg configuration to use
 * @return #GNUNET_OK on success, #GNUNET_SYSERR if we found no valid
 *         wire methods
 */
int
TEH_WIRE_init (const struct GNUNET_CONFIGURATION_Handle *cfg);


/**
 * Clean up wire subsystem.
 */
void
TEH_WIRE_done (void);


/**
 * Handle a "/wire" request.
 *
 * @param rh context of the handler
 * @param connection the MHD connection to handle
 * @param args array of additional options (must be empty for this function)
 * @return MHD result code
 */
MHD_RESULT
TEH_handler_wire (const struct TEH_RequestHandler *rh,
                  struct MHD_Connection *connection,
                  const char *const args[]);


#endif
