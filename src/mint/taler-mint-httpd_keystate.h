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
  TALER; see the file COPYING.  If not, If not, see <http://www.gnu.org/licenses/>
*/
/**
 * @file taler-mint-httpd_keystate.h
 * @brief management of our private signing keys (denomination keys)
 * @author Florian Dold
 * @author Benedikt Mueller
 * @author Christian Grothoff
 */
#ifndef TALER_MINT_HTTPD_KEYSTATE_H
#define TALER_MINT_HTTPD_KEYSTATE_H


#include <gnunet/gnunet_util_lib.h>
#include <microhttpd.h>
#include <jansson.h>
#include "taler-mint-httpd.h"
#include "key_io.h"


/**
 * Snapshot of the (coin and signing)
 * keys (including private keys) of the mint.
 */
struct MintKeyState;


/**
 * Acquire the key state of the mint.  Updates keys if necessary.
 * For every call to #TALER_MINT_key_state_acquire(), a matching call
 * to #TALER_MINT_key_state_release() must be made.
 *
 * @return the key state
 */
struct MintKeyState *
TALER_MINT_key_state_acquire (void);


/**
 * Release key state, free if necessary (if reference count gets to zero).
 *
 * @param key_state the key state to release
 */
void
TALER_MINT_key_state_release (struct MintKeyState *key_state);


/**
 * Look up the issue for a denom public key.
 *
 * @param key state to look in
 * @param denom_pub denomination public key
 * @return the denomination key issue,
 *         or NULL if denom_pub could not be found
 */
struct TALER_MINT_DenomKeyIssuePriv *
TALER_MINT_get_denom_key (const struct MintKeyState *key_state,
                          const struct GNUNET_CRYPTO_rsa_PublicKey *denom_pub);


/**
 * Read signals from a pipe in a loop, and reload keys from disk if
 * SIGUSR1 is read from the pipe.
 *
 * @return #GNUNET_OK if we terminated normally, #GNUNET_SYSERR on error
 */
int
TALER_MINT_key_reload_loop (void);


/**
 * Sign the message in @a purpose with the mint's signing
 * key.
 *
 * @param purpose the message to sign
 * @param[OUT] sig signature over purpose using current signing key
 */
void
TALER_MINT_keys_sign (const struct GNUNET_CRYPTO_EccSignaturePurpose *purpose,
                      struct GNUNET_CRYPTO_EddsaSignature *sig);


/**
 * Handle a "/keys" request
 *
 * @param rh context of the handler
 * @param connection the MHD connection to handle
 * @param[IN|OUT] connection_cls the connection's closure (can be updated)
 * @param upload_data upload data
 * @param[IN|OUT] upload_data_size number of bytes (left) in @a upload_data
 * @return MHD result code
  */
int
TALER_MINT_handler_keys (struct RequestHandler *rh,
                         struct MHD_Connection *connection,
                         void **connection_cls,
                         const char *upload_data,
                         size_t *upload_data_size);


#endif
