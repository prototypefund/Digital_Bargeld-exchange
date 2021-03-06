/*
  This file is part of TALER
  Copyright (C) 2014, 2015 Taler Systems SA

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
 * @file exchange/taler-exchange-httpd_keystate.h
 * @brief management of our private signing keys (denomination keys)
 * @author Florian Dold
 * @author Benedikt Mueller
 * @author Christian Grothoff
 */
#ifndef TALER_EXCHANGE_HTTPD_KEYSTATE_H
#define TALER_EXCHANGE_HTTPD_KEYSTATE_H

#include <gnunet/gnunet_util_lib.h>
#include <microhttpd.h>
#include "taler-exchange-httpd.h"
#include "taler_error_codes.h"
#include "taler_exchangedb_lib.h"


/**
 * Snapshot of the (coin and signing)
 * keys (including private keys) of the exchange.
 */
struct TEH_KS_StateHandle;


/**
 * Acquire the key state of the exchange.  Updates keys if necessary.
 * For every call to #TEH_KS_acquire(), a matching call
 * to #TEH_KS_release() must be made.
 *
 * @param now for what timestamp should we acquire the key state
 * @param location name of the function in which the lock is acquired
 * @return the key state, NULL on error (usually pretty fatal)
 */
struct TEH_KS_StateHandle *
TEH_KS_acquire_ (struct GNUNET_TIME_Absolute now,
                 const char *location);


/**
 * Release key state, free if necessary (if reference count gets to zero).
 *
 * @param location name of the function in which the lock is acquired
 * @param key_state the key state to release
 */
void
TEH_KS_release_ (const char *location,
                 struct TEH_KS_StateHandle *key_state);


/**
 * Acquire the key state of the exchange.  Updates keys if necessary.
 * For every call to #TEH_KS_acquire(), a matching call
 * to #TEH_KS_release() must be made.
 *
 * @param now current time snapshot; either true, or given by the
 *        client via the "now" URL parameter of "/keys".
 * @return the key state
 */
#define TEH_KS_acquire(now) TEH_KS_acquire_ (now, __FUNCTION__)


/**
 * Release key state, free if necessary (if reference count gets to zero).
 *
 * @param key_state the key state to release
 */
#define TEH_KS_release(key_state) TEH_KS_release_ (__FUNCTION__, key_state)


/**
 * Setup initial #internal_key_state and our signal handlers.
 *
 * @return #GNUNET_OK on success
 */
int
TEH_KS_init (void);


/**
 * Finally, release #internal_key_state and our signal handlers.
 */
void
TEH_KS_free (void);


/**
 * Denomination key lookups can be for signing of fresh coins
 * or to validate signatures on existing coins.  As the validity
 * periods for a key differ, the caller must specify which
 * use is relevant for the current operation.
 */
enum TEH_KS_DenominationKeyUse
{

  /**
   * The denomination key is to be used for a withdraw or reveal operation.
   */
  TEH_KS_DKU_WITHDRAW,

  /**
   * The denomination key is to be used for a deposit or melt operation.
   */
  TEH_KS_DKU_DEPOSIT,

  /**
   * The denomination key is to be used for a recoup operation, or to
   * melt a coin that was deposited (or melted) before the revocation.
   */
  TEH_KS_DKU_RECOUP,

  /**
   * The key is to be used for a refresh + recoup operation,
   * i.e. it is an old coin that regained value from a
   * recoup on a new coin derived from the old coin.
   */
  TEH_KS_DKU_ZOMBIE

};


/**
 * Look up the issue for a denom public key.  Note that the result
 * is only valid while the @a key_state is not released!
 *
 * @param key_state state to look in
 * @param denom_pub_hash hash of denomination public key
 * @param use purpose for which the key is being located
 * @param[out] ec set to the error code, in case the operation failed
 * @param[out] hc set to the HTTP status code to use
 * @return the denomination key issue,
 *         or NULL if denom_pub could not be found (or is not valid at this time for the given @a use)
 */
struct TALER_EXCHANGEDB_DenominationKey *
TEH_KS_denomination_key_lookup_by_hash (
  const struct TEH_KS_StateHandle *key_state,
  const struct GNUNET_HashCode *denom_pub_hash,
  enum TEH_KS_DenominationKeyUse use,
  enum TALER_ErrorCode *ec,
  unsigned int *hc);


/**
 * Read signals from a pipe in a loop, and reload keys from disk if
 * SIGUSR1 is received, terminate if SIGTERM/SIGINT is received, and
 * restart if SIGHUP is received.
 *
 * @return #GNUNET_SYSERR on errors,
 *         #GNUNET_OK to terminate normally
 *         #GNUNET_NO to restart an update version of the binary
 */
int
TEH_KS_loop (void);


/**
 * Sign the message in @a purpose with the exchange's signing
 * key.
 *
 * The @a purpose data is the beginning of the data of which the signature is
 * to be created. The `size` field in @a purpose must correctly indicate the
 * number of bytes of the data structure, including its header.  Use
 * #TEH_KS_sign() instead of calling this function directly!
 *
 * @param purpose the message to sign
 * @param[out] pub set to the current public signing key of the exchange
 * @param[out] sig signature over purpose using current signing key
 * @return #GNUNET_OK on success, #GNUNET_SYSERR if we lack key material
 */
int
TEH_KS_sign_ (const struct GNUNET_CRYPTO_EccSignaturePurpose *purpose,
              struct TALER_ExchangePublicKeyP *pub,
              struct TALER_ExchangeSignatureP *sig);

/**
 * @ingroup crypto
 * @brief EdDSA sign a given block.
 *
 * The @a ps data must be a fixed-size struct for which the signature is to be
 * created. The `size` field in @a ps->purpose must correctly indicate the
 * number of bytes of the data structure, including its header.
 *
 * @param ps packed struct with what to sign, MUST begin with a purpose
 * @param[out] pub where to store the public key to use for the signing
 * @param[out] sig where to write the signature
 */
#define TEH_KS_sign(ps,pub,sig) \
  ({                                                  \
    /* check size is set correctly */                 \
    GNUNET_assert (htonl ((ps)->purpose.size) ==      \
                   sizeof (*ps));                     \
    /* check 'ps' begins with the purpose */          \
    GNUNET_static_assert (((void*) (ps)) ==           \
                          ((void*) &(ps)->purpose));  \
    TEH_KS_sign_ (&(ps)->purpose,                     \
                  pub,                                \
                  sig);                               \
  })


/**
 * Handle a "/keys" request
 *
 * @param rh context of the handler
 * @param connection the MHD connection to handle
 * @param args array of additional options (must be empty for this function)
 * @return MHD result code
 */
MHD_RESULT
TEH_handler_keys (const struct TEH_RequestHandler *rh,
                  struct MHD_Connection *connection,
                  const char *const args[]);


#endif
