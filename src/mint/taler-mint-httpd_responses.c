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
 * @file taler-mint-httpd_responses.c
 * @brief API for generating the various replies of the mint; these
 *        functions are called TALER_MINT_reply_ and they generate
 *        and queue MHD response objects for a given connection.
 * @author Florian Dold
 * @author Benedikt Mueller
 * @author Christian Grothoff
 *
 * TODO:
 * - when generating /withdraw/status reply, which signature do
 *   we use there? Might want to instead return *all* signatures on the
 *   existig withdraw operations, instead of Mint's signature
 *   (check reply format, adjust `struct Reserve` if needed)
 */
#include "platform.h"
#include "taler-mint-httpd_responses.h"
#include "taler_util.h"
#include <gnunet/gnunet_util_lib.h>
#include "taler-mint-httpd_keystate.h"


/**
 * Send JSON object as response.
 *
 * @param connection the MHD connection
 * @param json the json object
 * @param response_code the http response code
 * @return MHD result code
 */
int
TALER_MINT_reply_json (struct MHD_Connection *connection,
                       const json_t *json,
                       unsigned int response_code)
{
  struct MHD_Response *resp;
  char *json_str;
  int ret;

  json_str = json_dumps (json, JSON_INDENT(2));
  resp = MHD_create_response_from_buffer (strlen (json_str), json_str,
                                          MHD_RESPMEM_MUST_FREE);
  if (NULL == resp)
    return MHD_NO;
  (void) MHD_add_response_header (resp,
                                  MHD_HTTP_HEADER_CONTENT_TYPE,
                                  "application/json");
  ret = MHD_queue_response (connection, response_code, resp);
  MHD_destroy_response (resp);
  return ret;
}


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
                            ...)
{
  json_t *json;
  va_list argp;
  int ret;

  va_start (argp, fmt);
  json = json_vpack_ex (NULL, 0, fmt, argp);
  va_end (argp);
  if (NULL == json)
    return MHD_NO;
  ret = TALER_MINT_reply_json (connection,
                               json,
                               response_code);
  json_decref (json);
  return ret;
}


/**
 * Send a response indicating an invalid argument.
 *
 * @param connection the MHD connection to use
 * @param param_name the parameter that is invalid
 * @return a MHD result code
 */
int
TALER_MINT_reply_arg_invalid (struct MHD_Connection *connection,
                              const char *param_name)
{
  return TALER_MINT_reply_json_pack (connection,
                                     MHD_HTTP_BAD_REQUEST,
                                     "{s:s, s:s}",
                                     "error", "invalid parameter",
                                     "parameter", param_name);
}


/**
 * Send a response indicating an invalid coin.  (I.e. the signature
 * over the public key of the coin does not match a valid signing key
 * of this mint).
 *
 * @param connection the MHD connection to use
 * @return MHD result code
 */
int
TALER_MINT_reply_coin_invalid (struct MHD_Connection *connection)
{
  /* TODO: may want to be more precise in the future and
     distinguish bogus signatures from bogus public keys. */
  return TALER_MINT_reply_json_pack (connection,
                                     MHD_HTTP_NOT_FOUND,
                                     "{s:s}",
                                     "error", "Coin is not valid");
}


/**
 * Send a response indicating a missing argument.
 *
 * @param connection the MHD connection to use
 * @param param_name the parameter that is missing
 * @return a MHD result code
 */
int
TALER_MINT_reply_arg_missing (struct MHD_Connection *connection,
                              const char *param_name)
{
  return TALER_MINT_reply_json_pack (connection,
                                     MHD_HTTP_BAD_REQUEST,
                                     "{ s:s, s:s}",
                                     "error", "missing parameter",
                                     "parameter", param_name);
}


/**
 * Send a response indicating an internal error.
 *
 * @param connection the MHD connection to use
 * @param hint hint about the internal error's nature
 * @return a MHD result code
 */
int
TALER_MINT_reply_internal_error (struct MHD_Connection *connection,
                                 const char *hint)
{
  return TALER_MINT_reply_json_pack (connection,
                                     MHD_HTTP_BAD_REQUEST,
                                     "{s:s, s:s}",
                                     "error", "internal error",
                                     "hint", hint);
}


/**
 * Send a response indicating an error committing a
 * transaction (concurrent interference).
 *
 * @param connection the MHD connection to use
 * @return a MHD result code
 */
int
TALER_MINT_reply_commit_error (struct MHD_Connection *connection)
{
  return TALER_MINT_reply_json_pack (connection,
                                     MHD_HTTP_BAD_REQUEST,
                                     "{s:s}",
                                     "error", "commit failure");
}


/**
 * Send a response indicating a failure to talk to the Mint's
 * database.
 *
 * @param connection the MHD connection to use
 * @return a MHD result code
 */
int
TALER_MINT_reply_internal_db_error (struct MHD_Connection *connection)
{
  return TALER_MINT_reply_internal_error (connection,
                                          "Failed to connect to database");
}


/**
 * Send a response indicating that the request was too big.
 *
 * @param connection the MHD connection to use
 * @return a MHD result code
 */
int
TALER_MINT_reply_request_too_large (struct MHD_Connection *connection)
{
  struct MHD_Response *resp;
  int ret;

  resp = MHD_create_response_from_buffer (0,
                                          NULL,
                                          MHD_RESPMEM_PERSISTENT);
  if (NULL == resp)
    return MHD_NO;
  ret = MHD_queue_response (connection,
                            MHD_HTTP_REQUEST_ENTITY_TOO_LARGE,
                            resp);
  MHD_destroy_response (resp);
  return ret;
}


/**
 * Send a response indicating that the JSON was malformed.
 *
 * @param connection the MHD connection to use
 * @return a MHD result code
 */
int
TALER_MINT_reply_invalid_json (struct MHD_Connection *connection)
{
  return TALER_MINT_reply_json_pack (connection,
                                     MHD_HTTP_BAD_REQUEST,
                                     "{s:s}",
                                     "error",
                                     "invalid json");
}


/**
 * Send confirmation of deposit success to client.  This function
 * will create a signed message affirming the given information
 * and return it to the client.  By this, the mint affirms that
 * the coin had sufficient (residual) value for the specified
 * transaction and that it will execute the requested deposit
 * operation with the given wiring details.
 *
 * @param connection connection to the client
 * @param coin_pub public key of the coin
 * @param h_wire hash of wire details
 * @param h_contract hash of contract details
 * @param transaction_id transaction ID
 * @param merchant merchant public key
 * @param amount fraction of coin value to deposit
 * @return MHD result code
 */
int
TALER_MINT_reply_deposit_success (struct MHD_Connection *connection,
                                  const struct GNUNET_CRYPTO_EcdsaPublicKey *coin_pub,
                                  const struct GNUNET_HashCode *h_wire,
                                  const struct GNUNET_HashCode *h_contract,
                                  uint64_t transaction_id,
                                  const struct GNUNET_CRYPTO_EddsaPublicKey *merchant,
                                  const struct TALER_Amount *amount)
{
  struct TALER_DepositConfirmation dc;
  struct GNUNET_CRYPTO_EddsaSignature sig;
  json_t *sig_json;
  int ret;

  dc.purpose.purpose = htonl (TALER_SIGNATURE_MINT_DEPOSIT);
  dc.purpose.size = htonl (sizeof (struct TALER_DepositConfirmation));
  dc.h_contract = *h_contract;
  dc.h_wire = *h_wire;
  dc.transaction_id = GNUNET_htonll (transaction_id);
  dc.amount = TALER_amount_hton (*amount);
  dc.coin_pub = *coin_pub;
  dc.merchant = *merchant;
  TALER_MINT_keys_sign (&dc.purpose,
                        &sig);
  {
    LOG_WARNING ("Failed to create EdDSA signature using my private key\n");
    return TALER_MINT_reply_internal_error (connection,
                                            "Failed to EdDSA-sign response\n");
  }
  sig_json = TALER_JSON_from_sig (&dc.purpose, &sig);
  ret = TALER_MINT_reply_json_pack (connection,
                                    MHD_HTTP_OK,
                                    "{s:s, s:o}",
                                    "status", "DEPOSIT_OK",
                                    "signature", sig_json);
  json_decref (sig_json);
  return ret;
}


/**
 * Send reserve status information to client.
 *
 * @param connection connection to the client
 * @param balance current reserve balance
 * @param expiration when will the reserve expire
 * @return MHD result code
 */
int
TALER_MINT_reply_withdraw_status_success (struct MHD_Connection *connection,
                                          const struct TALER_Amount balance,
                                          struct GNUNET_TIME_Absolute expiration)
{
  json_t *json_balance;
  json_t *json_expiration;
  int ret;

  json_balance = TALER_JSON_from_amount (balance);
  json_expiration = TALER_JSON_from_abs (expiration);
  ret = TALER_MINT_reply_json_pack (connection,
                                    MHD_HTTP_OK,
                                    "{s:o, s:o}",
                                    "balance", json_balance,
                                    "expiration", json_expiration);
  json_decref (json_balance);
  json_decref (json_expiration);
  return ret;
}


/**
 * Send blinded coin information to client.
 *
 * @param connection connection to the client
 * @param collectable blinded coin to return
 * @return MHD result code
 */
int
TALER_MINT_reply_withdraw_sign_success (struct MHD_Connection *connection,
                                        const struct CollectableBlindcoin *collectable)
{
  json_t *sig_json;
  size_t sig_buf_size;
  char *sig_buf;
  int ret;

  /* FIXME: use TALER_JSON_from_sig here instead! */
  sig_buf_size = GNUNET_CRYPTO_rsa_signature_encode (collectable->sig,
                                                     &sig_buf);
  sig_json = TALER_JSON_from_data (sig_buf,
                                   sig_buf_size);
  GNUNET_free (sig_buf);
  ret = TALER_MINT_reply_json_pack (connection,
                                    MHD_HTTP_OK,
                                    "{s:o}",
                                    "ev_sig", sig_json);
  json_decref (sig_json);
  return ret;
}


/**
 * Send a response for "/refresh/melt".
 *
 * @param connection the connection to send the response to
 * @param db_conn the database connection to fetch values from
 * @param session_pub the refresh session public key.
 * @return a MHD result code
 */
int
TALER_MINT_reply_refresh_melt_success (struct MHD_Connection *connection,
                                       const struct RefreshSession *session,
                                       const struct GNUNET_CRYPTO_EddsaPublicKey *session_pub)
{
  int ret;
  json_t *list;
  struct GNUNET_HashContext *hash_context;
  struct RefreshMeltResponseSignatureBody body;
  struct GNUNET_CRYPTO_EddsaSignature sig;
  json_t *sig_json;

  list = json_array ();
  hash_context = GNUNET_CRYPTO_hash_context_start ();
  body.purpose.size = htonl (sizeof (struct RefreshMeltResponseSignatureBody));
  body.purpose.purpose = htonl (TALER_SIGNATURE_REFRESH_MELT_RESPONSE);
  /* FIXME: should we not add something to the hash_context in the meantime? */
  GNUNET_CRYPTO_hash_context_finish (hash_context,
                                     &body.melt_response_hash);
  TALER_MINT_keys_sign (&body.purpose,
                        &sig);
  sig_json = TALER_JSON_from_sig (&body.purpose, &sig);
  GNUNET_assert (NULL != sig_json);
  ret = TALER_MINT_reply_json_pack (connection,
                                    MHD_HTTP_OK,
                                    "{s:o, s:o}",
                                    "signature", sig_json,
                                    "blind_session_pubs", list);
  json_decref (sig_json);
  json_decref (list);
  return ret;
}


/**
 * Send a response to a "/refresh/commit" request.
 *
 * FIXME: maybe not the ideal argument type for @a refresh_session here.
 *
 * @param connection the connection to send the response to
 * @param refresh_session the refresh session
 * @return a MHD status code
 */
int
TALER_MINT_reply_refresh_commit_success (struct MHD_Connection *connection,
                                         struct RefreshSession *refresh_session)
{
  struct RefreshCommitResponseSignatureBody body;
  struct GNUNET_CRYPTO_EddsaSignature sig;
  json_t *sig_json;
  int ret;

  body.purpose.size = htonl (sizeof (struct RefreshCommitResponseSignatureBody));
  body.purpose.purpose = htonl (TALER_SIGNATURE_REFRESH_COMMIT_RESPONSE);
  body.noreveal_index = htons (refresh_session->noreveal_index);
  TALER_MINT_keys_sign (&body.purpose,
                        &sig);
  sig_json = TALER_JSON_from_sig (&body.purpose, &sig);
  GNUNET_assert (NULL != sig_json);
  ret = TALER_MINT_reply_json_pack (connection,
                                     MHD_HTTP_OK,
                                     "{s:i, s:o}",
                                     "noreveal_index", (int) refresh_session->noreveal_index,
                                     "signature", sig_json);
  json_decref (sig_json);
  return ret;
}


/**
 * Send a response for "/refresh/reveal".
 *
 * @param connection the connection to send the response to
 * @param num_newcoins number of new coins for which we reveal data
 * @param sigs array of @a num_newcoins signatures revealed
 * @return a MHD result code
 */
int
TALER_MINT_reply_refresh_reveal_success (struct MHD_Connection *connection,
                                         unsigned int num_newcoins,
                                         struct GNUNET_CRYPTO_rsa_Signature **sigs)
{
  int newcoin_index;
  json_t *root;
  json_t *list;
  char *buf;
  size_t buf_size;
  int ret;

  root = json_object ();
  list = json_array ();
  json_object_set_new (root, "ev_sigs", list);
  for (newcoin_index = 0; newcoin_index < num_newcoins; newcoin_index++)
  {
    buf_size = GNUNET_CRYPTO_rsa_signature_encode (sigs[newcoin_index],
                                                   &buf);
    json_array_append_new (list,
                           TALER_JSON_from_data (buf,
                                                 buf_size));
    GNUNET_free (buf);
  }
  ret = TALER_MINT_reply_json (connection,
                               root,
                               MHD_HTTP_OK);
  json_decref (root);
  return ret;
}



/* end of taler-mint-httpd_responses.c */
