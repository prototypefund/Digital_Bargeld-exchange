/*
  This file is part of TALER
  (C) 2014 GNUnet e.V.

  TALER is free software; you can redistribute it and/or modify it under the
  terms of the GNU Affero General Public License as published by the Free Software
  Foundation; either version 3, or (at your option) any later version.

  TALER is distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
  A PARTICULAR PURPOSE.  See the GNU Affero General Public License for more details.

  You should have received a copy of the GNU Affero General Public License along with
  TALER; see the file COPYING.  If not, If not, see <http://www.gnu.org/licenses/>
*/

/**
 * @file taler-mint-httpd_responses.h
 * @brief API for generating the various replies of the mint; these
 *        functions are called TALER_MINT_reply_ and they generate
 *        and queue MHD response objects for a given connection.
 * @author Florian Dold
 * @author Benedikt Mueller
 * @author Christian Grothoff
 */
#ifndef TALER_MINT_HTTPD_RESPONSES_H
#define TALER_MINT_HTTPD_RESPONSES_H
#include <gnunet/gnunet_util_lib.h>
#include <jansson.h>
#include <microhttpd.h>
#include <libpq-fe.h>
#include <pthread.h>
#include "taler-mint-httpd.h"
#include "taler-mint-httpd_mhd.h"


/**
 * Send JSON object as response.  Decreases the reference count of the
 * JSON object.
 *
 * @param connection the MHD connection
 * @param json the json object
 * @param response_code the http response code
 * @return MHD result code
 */
int
TALER_MINT_reply_json (struct MHD_Connection *connection,
                       json_t *json,
                       unsigned int response_code);


/**
 * Function to call to handle the request by building a JSON
 * reply from a format string and varargs.
 *
 * @param connection the MHD connection to handle
 * @param response_code HTTP response code to use
 * @param fmt format string for pack
 * @param ... varargs
 * @return MHD result code
 */
int
TALER_MINT_reply_json_pack (struct MHD_Connection *connection,
                            unsigned int response_code,
                            const char *fmt,
                            ...);


/**
 * Send a response indicating an invalid argument.
 *
 * @param connection the MHD connection to use
 * @param param_name the parameter that is invalid
 * @return MHD result code
 */
int
TALER_MINT_reply_arg_invalid (struct MHD_Connection *connection,
                              const char *param_name);


/**
 * Send a response indicating a missing argument.
 *
 * @param connection the MHD connection to use
 * @param param_name the parameter that is missing
 * @return a MHD result code
 */
int
TALER_MINT_reply_arg_missing (struct MHD_Connection *connection,
                              const char *param_name);




#endif
