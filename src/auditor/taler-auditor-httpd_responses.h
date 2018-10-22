/*
  This file is part of TALER
  Copyright (C) 2014 GNUnet e.V.

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
 * @file taler-auditor-httpd_responses.h
 * @brief API for generating generic replies of the auditor; these
 *        functions are called TAH_RESPONSE_reply_ and they generate
 *        and queue MHD response objects for a given connection.
 * @author Florian Dold
 * @author Benedikt Mueller
 * @author Christian Grothoff
 */
#ifndef TALER_AUDITOR_HTTPD_RESPONSES_H
#define TALER_AUDITOR_HTTPD_RESPONSES_H
#include <gnunet/gnunet_util_lib.h>
#include <jansson.h>
#include <microhttpd.h>
#include <pthread.h>
#include "taler_error_codes.h"
#include "taler-auditor-httpd.h"


/**
 * Add headers we want to return in every response.
 * Useful for testing, like if we want to always close
 * connections.
 *
 * @param response response to modify
 */
void
TAH_RESPONSE_add_global_headers (struct MHD_Response *response);


/**
 * Try to compress a response body.  Updates @a buf and @a buf_size.
 *
 * @param[in,out] buf pointer to body to compress
 * @param[in,out] buf_size pointer to initial size of @a buf
 * @return #MHD_YES if @a buf was compressed
 */
int
TAH_RESPONSE_body_compress (void **buf,
			    size_t *buf_size);


/**
 * Is HTTP body deflate compression supported by the client?
 *
 * @param connection connection to check
 * @return #MHD_YES if 'deflate' compression is allowed
 */
int
TAH_RESPONSE_can_compress (struct MHD_Connection *connection);


/**
 * Send JSON object as response.
 *
 * @param connection the MHD connection
 * @param json the json object
 * @param response_code the http response code
 * @return MHD result code
 */
int
TAH_RESPONSE_reply_json (struct MHD_Connection *connection,
                         const json_t *json,
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
TAH_RESPONSE_reply_json_pack (struct MHD_Connection *connection,
                              unsigned int response_code,
                              const char *fmt,
                              ...);


/**
 * Send a response indicating an invalid signature.
 *
 * @param connection the MHD connection to use
 * @param ec error code uniquely identifying the error
 * @param param_name the parameter that is invalid
 * @return a MHD result code
 */
int
TAH_RESPONSE_reply_signature_invalid (struct MHD_Connection *connection,
				      enum TALER_ErrorCode ec,
                                      const char *param_name);


/**
 * Send a response indicating an invalid argument.
 *
 * @param connection the MHD connection to use
 * @param ec error code uniquely identifying the error
 * @param param_name the parameter that is invalid
 * @return MHD result code
 */
int
TAH_RESPONSE_reply_arg_invalid (struct MHD_Connection *connection,
				enum TALER_ErrorCode ec,
                                const char *param_name);


/**
 * Send a response indicating an argument refering to a
 * resource unknown to the auditor (i.e. unknown reserve or
 * denomination key).
 *
 * @param connection the MHD connection to use
 * @param ec error code uniquely identifying the error
 * @param param_name the parameter that is invalid
 * @return a MHD result code
 */
int
TAH_RESPONSE_reply_arg_unknown (struct MHD_Connection *connection,
				enum TALER_ErrorCode ec,
                                const char *param_name);


/**
 * Send a response indicating a missing argument.
 *
 * @param connection the MHD connection to use
 * @param ec error code uniquely identifying the error
 * @param param_name the parameter that is missing
 * @return a MHD result code
 */
int
TAH_RESPONSE_reply_arg_missing (struct MHD_Connection *connection,
				enum TALER_ErrorCode ec,
                                const char *param_name);


/**
 * Send a response indicating permission denied.
 *
 * @param connection the MHD connection to use
 * @param ec error code uniquely identifying the error
 * @param hint hint about why access was denied
 * @return a MHD result code
 */
int
TAH_RESPONSE_reply_permission_denied (struct MHD_Connection *connection,
				      enum TALER_ErrorCode ec,
                                      const char *hint);


/**
 * Send a response indicating an internal error.
 *
 * @param connection the MHD connection to use
 * @param ec error code uniquely identifying the error
 * @param hint hint about the internal error's nature
 * @return a MHD result code
 */
int
TAH_RESPONSE_reply_internal_error (struct MHD_Connection *connection,
				   enum TALER_ErrorCode ec,
                                   const char *hint);


/**
 * Send a response indicating an external error.
 *
 * @param connection the MHD connection to use
 * @param ec error code uniquely identifying the error
 * @param hint hint about the error's nature
 * @return a MHD result code
 */
int
TAH_RESPONSE_reply_external_error (struct MHD_Connection *connection,
				   enum TALER_ErrorCode ec,
                                   const char *hint);


/**
 * Send a response indicating an error committing a
 * transaction (concurrent interference).
 *
 * @param connection the MHD connection to use
 * @param ec error code uniquely identifying the error
 * @return a MHD result code
 */
int
TAH_RESPONSE_reply_commit_error (struct MHD_Connection *connection,
				      enum TALER_ErrorCode ec);

/**
 * Send a response indicating a failure to talk to the Auditor's
 * database.
 *
 * @param connection the MHD connection to use
 * @param ec error code uniquely identifying the error
 * @return a MHD result code
 */
int
TAH_RESPONSE_reply_internal_db_error (struct MHD_Connection *connection,
				      enum TALER_ErrorCode ec);


/**
 * Send a response indicating that the request was too big.
 *
 * @param connection the MHD connection to use
 * @return a MHD result code
 */
int
TAH_RESPONSE_reply_request_too_large (struct MHD_Connection *connection);


/**
 * Send a response indicating that the JSON was malformed.
 *
 * @param connection the MHD connection to use
 * @return a MHD result code
 */
int
TAH_RESPONSE_reply_invalid_json (struct MHD_Connection *connectionx);


#endif