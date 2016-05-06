/*
  This file is part of TALER
  Copyright (C) 2014, 2015, 2016 GNUnet e.V.

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
 * @file taler-exchange-httpd_responses.c
 * @brief API for generating the various replies of the exchange; these
 *        functions are called TMH_RESPONSE_reply_ and they generate
 *        and queue MHD response objects for a given connection.
 * @author Florian Dold
 * @author Benedikt Mueller
 * @author Christian Grothoff
 */
#include "platform.h"
#include "taler-exchange-httpd_responses.h"
#include "taler_util.h"
#include "taler_json_lib.h"
#include "taler-exchange-httpd_keystate.h"


/**
 * Add headers we want to return in every response.
 * Useful for testing, like if we want to always close
 * connections.
 *
 * @param response response to modify
 */
void
TMH_RESPONSE_add_global_headers (struct MHD_Response *response)
{
  if (TMH_exchange_connection_close)
    (void) MHD_add_response_header (response,
                                    MHD_HTTP_HEADER_CONNECTION,
                                    "close");
}


/**
 * Send JSON object as response.
 *
 * @param connection the MHD connection
 * @param json the json object
 * @param response_code the http response code
 * @return MHD result code
 */
int
TMH_RESPONSE_reply_json (struct MHD_Connection *connection,
                         const json_t *json,
                         unsigned int response_code)
{
  struct MHD_Response *resp;
  char *json_str;
  int ret;

  json_str = json_dumps (json, JSON_INDENT(2));
  GNUNET_assert (NULL != json_str);
  resp = MHD_create_response_from_buffer (strlen (json_str), json_str,
                                          MHD_RESPMEM_MUST_FREE);
  if (NULL == resp)
  {
    free (json_str);
    GNUNET_break (0);
    return MHD_NO;
  }
  TMH_RESPONSE_add_global_headers (resp);
  (void) MHD_add_response_header (resp,
                                  MHD_HTTP_HEADER_CONTENT_TYPE,
                                  "application/json");
  ret = MHD_queue_response (connection,
                            response_code,
                            resp);
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
TMH_RESPONSE_reply_json_pack (struct MHD_Connection *connection,
                              unsigned int response_code,
                              const char *fmt,
                              ...)
{
  json_t *json;
  va_list argp;
  int ret;
  json_error_t jerror;

  va_start (argp, fmt);
  json = json_vpack_ex (&jerror, 0, fmt, argp);
  va_end (argp);
  if (NULL == json)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Failed to pack JSON with format `%s': %s\n",
                fmt,
                jerror.text);
    GNUNET_break (0);
    return MHD_NO;
  }
  ret = TMH_RESPONSE_reply_json (connection,
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
TMH_RESPONSE_reply_arg_invalid (struct MHD_Connection *connection,
                                const char *param_name)
{
  return TMH_RESPONSE_reply_json_pack (connection,
                                       MHD_HTTP_BAD_REQUEST,
                                       "{s:s, s:s}",
                                       "error", "invalid parameter",
                                       "parameter", param_name);
}


/**
 * Send a response indicating an argument refering to a
 * resource unknown to the exchange (i.e. unknown reserve or
 * denomination key).
 *
 * @param connection the MHD connection to use
 * @param param_name the parameter that is invalid
 * @return a MHD result code
 */
int
TMH_RESPONSE_reply_arg_unknown (struct MHD_Connection *connection,
                                const char *param_name)
{
  return TMH_RESPONSE_reply_json_pack (connection,
                                       MHD_HTTP_NOT_FOUND,
                                       "{s:s, s:s}",
                                       "error", "unknown entity referenced",
                                       "parameter", param_name);
}


/**
 * Send a response indicating an invalid signature.
 *
 * @param connection the MHD connection to use
 * @param param_name the parameter that is invalid
 * @return a MHD result code
 */
int
TMH_RESPONSE_reply_signature_invalid (struct MHD_Connection *connection,
                                      const char *param_name)
{
  return TMH_RESPONSE_reply_json_pack (connection,
                                       MHD_HTTP_UNAUTHORIZED,
                                       "{s:s, s:s}",
                                       "error", "invalid signature",
                                       "parameter", param_name);
}


/**
 * Send a response indicating a missing argument.
 *
 * @param connection the MHD connection to use
 * @param param_name the parameter that is missing
 * @return a MHD result code
 */
int
TMH_RESPONSE_reply_arg_missing (struct MHD_Connection *connection,
                                const char *param_name)
{
  return TMH_RESPONSE_reply_json_pack (connection,
                                       MHD_HTTP_BAD_REQUEST,
                                       "{ s:s, s:s}",
                                       "error", "missing parameter",
                                       "parameter", param_name);
}


/**
 * Send a response indicating permission denied.
 *
 * @param connection the MHD connection to use
 * @param hint hint about why access was denied
 * @return a MHD result code
 */
int
TMH_RESPONSE_reply_permission_denied (struct MHD_Connection *connection,
                                      const char *hint)
{
  return TMH_RESPONSE_reply_json_pack (connection,
                                       MHD_HTTP_FORBIDDEN,
                                       "{s:s, s:s}",
                                       "error", "permission denied",
                                       "hint", hint);
}


/**
 * Send a response indicating an internal error.
 *
 * @param connection the MHD connection to use
 * @param hint hint about the internal error's nature
 * @return a MHD result code
 */
int
TMH_RESPONSE_reply_internal_error (struct MHD_Connection *connection,
                                   const char *hint)
{
  return TMH_RESPONSE_reply_json_pack (connection,
                                       MHD_HTTP_INTERNAL_SERVER_ERROR,
                                       "{s:s, s:s}",
                                       "error", "internal error",
                                       "hint", hint);
}


/**
 * Send a response indicating an external error.
 *
 * @param connection the MHD connection to use
 * @param hint hint about the error's nature
 * @return a MHD result code
 */
int
TMH_RESPONSE_reply_external_error (struct MHD_Connection *connection,
                                   const char *hint)
{
  return TMH_RESPONSE_reply_json_pack (connection,
                                       MHD_HTTP_BAD_REQUEST,
                                       "{s:s, s:s}",
                                       "error", "client error",
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
TMH_RESPONSE_reply_commit_error (struct MHD_Connection *connection)
{
  return TMH_RESPONSE_reply_json_pack (connection,
                                       MHD_HTTP_BAD_REQUEST,
                                       "{s:s}",
                                       "error", "commit failure");
}


/**
 * Send a response indicating a failure to talk to the Exchange's
 * database.
 *
 * @param connection the MHD connection to use
 * @return a MHD result code
 */
int
TMH_RESPONSE_reply_internal_db_error (struct MHD_Connection *connection)
{
  return TMH_RESPONSE_reply_internal_error (connection,
                                            "Failed to connect to database");
}


/**
 * Send a response indicating that the request was too big.
 *
 * @param connection the MHD connection to use
 * @return a MHD result code
 */
int
TMH_RESPONSE_reply_request_too_large (struct MHD_Connection *connection)
{
  struct MHD_Response *resp;
  int ret;

  resp = MHD_create_response_from_buffer (0,
                                          NULL,
                                          MHD_RESPMEM_PERSISTENT);
  if (NULL == resp)
    return MHD_NO;
  TMH_RESPONSE_add_global_headers (resp);
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
TMH_RESPONSE_reply_invalid_json (struct MHD_Connection *connection)
{
  return TMH_RESPONSE_reply_json_pack (connection,
                                       MHD_HTTP_BAD_REQUEST,
                                       "{s:s}",
                                       "error",
                                       "invalid json");
}


/**
 * Send confirmation of deposit success to client.  This function
 * will create a signed message affirming the given information
 * and return it to the client.  By this, the exchange affirms that
 * the coin had sufficient (residual) value for the specified
 * transaction and that it will execute the requested deposit
 * operation with the given wiring details.
 *
 * @param connection connection to the client
 * @param coin_pub public key of the coin
 * @param h_wire hash of wire details
 * @param h_contract hash of contract details
 * @param transaction_id transaction ID
 * @param timestamp client's timestamp
 * @param refund_deadline until when this deposit be refunded
 * @param merchant merchant public key
 * @param amount_without_fee fraction of coin value to deposit, without the fee
 * @return MHD result code
 */
int
TMH_RESPONSE_reply_deposit_success (struct MHD_Connection *connection,
                                    const struct TALER_CoinSpendPublicKeyP *coin_pub,
                                    const struct GNUNET_HashCode *h_wire,
                                    const struct GNUNET_HashCode *h_contract,
                                    uint64_t transaction_id,
                                    struct GNUNET_TIME_Absolute timestamp,
                                    struct GNUNET_TIME_Absolute refund_deadline,
                                    const struct TALER_MerchantPublicKeyP *merchant,
                                    const struct TALER_Amount *amount_without_fee)
{
  struct TALER_DepositConfirmationPS dc;
  struct TALER_ExchangePublicKeyP pub;
  struct TALER_ExchangeSignatureP sig;

  dc.purpose.purpose = htonl (TALER_SIGNATURE_EXCHANGE_CONFIRM_DEPOSIT);
  dc.purpose.size = htonl (sizeof (struct TALER_DepositConfirmationPS));
  dc.h_contract = *h_contract;
  dc.h_wire = *h_wire;
  dc.transaction_id = GNUNET_htonll (transaction_id);
  dc.timestamp = GNUNET_TIME_absolute_hton (timestamp);
  dc.refund_deadline = GNUNET_TIME_absolute_hton (refund_deadline);
  TALER_amount_hton (&dc.amount_without_fee,
                     amount_without_fee);
  dc.coin_pub = *coin_pub;
  dc.merchant = *merchant;
  TMH_KS_sign (&dc.purpose,
               &pub,
               &sig);
  return TMH_RESPONSE_reply_json_pack (connection,
                                       MHD_HTTP_OK,
                                       "{s:s, s:o, s:o}",
                                       "status", "DEPOSIT_OK",
                                       "sig", GNUNET_JSON_from_data (&sig,
                                                                    sizeof (sig)),
                                       "pub", GNUNET_JSON_from_data (&pub,
                                                                    sizeof (pub)));
}


/**
 * Compile the transaction history of a coin into a JSON object.
 *
 * @param tl transaction history to JSON-ify
 * @return json representation of the @a rh
 */
static json_t *
compile_transaction_history (const struct TALER_EXCHANGEDB_TransactionList *tl)
{
  json_t *details;
  const char *type;
  struct TALER_Amount value;
  json_t *history;
  const struct GNUNET_CRYPTO_EddsaSignature *sig;
  const struct TALER_EXCHANGEDB_TransactionList *pos;

  history = json_array ();
  for (pos = tl; NULL != pos; pos = pos->next)
  {
    switch (pos->type)
    {
    case TALER_EXCHANGEDB_TT_DEPOSIT:
      {
        struct TALER_DepositRequestPS dr;
        const struct TALER_EXCHANGEDB_Deposit *deposit = pos->details.deposit;

        type = "DEPOSIT";
        value = deposit->amount_with_fee;
        dr.purpose.purpose = htonl (TALER_SIGNATURE_WALLET_COIN_DEPOSIT);
        dr.purpose.size = htonl (sizeof (struct TALER_DepositRequestPS));
        dr.h_contract = deposit->h_contract;
        dr.h_wire = deposit->h_wire;
        dr.timestamp = GNUNET_TIME_absolute_hton (deposit->timestamp);
        dr.refund_deadline = GNUNET_TIME_absolute_hton (deposit->refund_deadline);
        dr.transaction_id = GNUNET_htonll (deposit->transaction_id);
        TALER_amount_hton (&dr.amount_with_fee,
                           &deposit->amount_with_fee);
        TALER_amount_hton (&dr.deposit_fee,
                           &deposit->deposit_fee);
        dr.merchant = deposit->merchant_pub;
        dr.coin_pub = deposit->coin.coin_pub;
        sig = &deposit->csig.eddsa_signature;
	/* internal sanity check before we hand out a bogus sig... */
        if (GNUNET_OK !=
            GNUNET_CRYPTO_eddsa_verify (TALER_SIGNATURE_WALLET_COIN_DEPOSIT,
                                        &dr.purpose,
                                        sig,
                                        &deposit->coin.coin_pub.eddsa_pub))
	{
	  GNUNET_break (0);
	  json_decref (history);
	  return NULL;
	}

        details = GNUNET_JSON_from_data (&dr.purpose,
                                        sizeof (struct TALER_DepositRequestPS));
        break;
      }
    case TALER_EXCHANGEDB_TT_REFRESH_MELT:
      {
        struct TALER_RefreshMeltCoinAffirmationPS ms;
        const struct TALER_EXCHANGEDB_RefreshMelt *melt = pos->details.melt;

        type = "MELT";
        value = melt->amount_with_fee;
        ms.purpose.purpose = htonl (TALER_SIGNATURE_WALLET_COIN_MELT);
        ms.purpose.size = htonl (sizeof (struct TALER_RefreshMeltCoinAffirmationPS));
        ms.session_hash = melt->session_hash;
        TALER_amount_hton (&ms.amount_with_fee,
                           &melt->amount_with_fee);
        TALER_amount_hton (&ms.melt_fee,
                           &melt->melt_fee);
        ms.coin_pub = melt->coin.coin_pub;
        sig = &melt->coin_sig.eddsa_signature;
	/* internal sanity check before we hand out a bogus sig... */
        if (GNUNET_OK !=
            GNUNET_CRYPTO_eddsa_verify (TALER_SIGNATURE_WALLET_COIN_MELT,
                                        &ms.purpose,
                                        sig,
                                        &melt->coin.coin_pub.eddsa_pub))
	{
	  GNUNET_break (0);
	  json_decref (history);
	  return NULL;
	}

        details = GNUNET_JSON_from_data (&ms.purpose,
                                        sizeof (struct TALER_RefreshMeltCoinAffirmationPS));
      }
      break;
    case TALER_EXCHANGEDB_TT_REFUND:
      {
        struct TALER_RefundRequestPS rr;
        const struct TALER_EXCHANGEDB_Refund *refund = pos->details.refund;

        type = "REFUND";
        if (GNUNET_OK !=
            TALER_amount_subtract (&value,
                                   &refund->refund_amount,
                                   &refund->refund_fee))
        {
	  GNUNET_break (0);
	  json_decref (history);
	  return NULL;
        }
        rr.purpose.purpose = htonl (TALER_SIGNATURE_MERCHANT_REFUND);
        rr.purpose.size = htonl (sizeof (struct TALER_RefundRequestPS));
        rr.h_contract = refund->h_contract;
        rr.transaction_id = GNUNET_htonll (refund->transaction_id);
        rr.coin_pub = refund->coin.coin_pub;
        rr.merchant = refund->merchant_pub;
        rr.rtransaction_id = GNUNET_htonll (refund->rtransaction_id);
        TALER_amount_hton (&rr.refund_amount,
                           &refund->refund_amount);
        TALER_amount_hton (&rr.refund_fee,
                           &refund->refund_fee);
	/* internal sanity check before we hand out a bogus sig... */
        if (GNUNET_OK !=
            GNUNET_CRYPTO_eddsa_verify (TALER_SIGNATURE_MERCHANT_REFUND,
                                        &rr.purpose,
                                        sig,
                                        &refund->merchant_pub.eddsa_pub))
	{
	  GNUNET_break (0);
	  json_decref (history);
	  return NULL;
	}
        sig = &refund->merchant_sig.eddsa_sig;
        details = GNUNET_JSON_from_data (&rr.purpose,
                                         sizeof (struct TALER_RefundRequestPS));
      }
      break;
    default:
      GNUNET_assert (0);
    }
    json_array_append_new (history,
                           json_pack ("{s:s, s:o, s:o, s:o}",
                                      "type", type,
                                      "amount", TALER_JSON_from_amount (&value),
                                      "signature", GNUNET_JSON_from_data (sig,
                                                                          sizeof (struct GNUNET_CRYPTO_EddsaSignature)),
                                      "details", details));
  }
  return history;
}


/**
 * Send proof that a /deposit request is invalid to client.  This
 * function will create a message with all of the operations affecting
 * the coin that demonstrate that the coin has insufficient value.
 *
 * @param connection connection to the client
 * @param tl transaction list to use to build reply
 * @return MHD result code
 */
int
TMH_RESPONSE_reply_deposit_insufficient_funds (struct MHD_Connection *connection,
                                               const struct TALER_EXCHANGEDB_TransactionList *tl)
{
  json_t *history;

  history = compile_transaction_history (tl);
  if (NULL == history)
    return TMH_RESPONSE_reply_internal_db_error (connection);
  return TMH_RESPONSE_reply_json_pack (connection,
                                       MHD_HTTP_FORBIDDEN,
                                       "{s:s, s:o}",
                                       "error", "insufficient funds",
                                       "history", history);
}


/**
 * Compile the history of a reserve into a JSON object
 * and calculate the total balance.
 *
 * @param rh reserve history to JSON-ify
 * @param[out] balance set to current reserve balance
 * @return json representation of the @a rh, NULL on error
 */
static json_t *
compile_reserve_history (const struct TALER_EXCHANGEDB_ReserveHistory *rh,
                         struct TALER_Amount *balance)
{
  struct TALER_Amount deposit_total;
  struct TALER_Amount withdraw_total;
  struct TALER_Amount value;
  json_t *json_history;
  int ret;
  const struct TALER_EXCHANGEDB_ReserveHistory *pos;
  struct TALER_WithdrawRequestPS wr;

  json_history = json_array ();
  ret = 0;
  for (pos = rh; NULL != pos; pos = pos->next)
  {
    switch (pos->type)
    {
    case TALER_EXCHANGEDB_RO_BANK_TO_EXCHANGE:
      if (0 == ret)
        deposit_total = pos->details.bank->amount;
      else
        if (GNUNET_OK !=
            TALER_amount_add (&deposit_total,
                              &deposit_total,
                              &pos->details.bank->amount))
        {
          json_decref (json_history);
          return NULL;
        }
      ret = 1;
      json_array_append_new (json_history,
                             json_pack ("{s:s, s:O, s:o}",
                                        "type", "DEPOSIT",
                                        "wire", pos->details.bank->wire,
                                        "amount", TALER_JSON_from_amount (&pos->details.bank->amount)));
      break;
    case TALER_EXCHANGEDB_RO_WITHDRAW_COIN:
      break;
    }
  }

  ret = 0;
  for (pos = rh; NULL != pos; pos = pos->next)
  {
    switch (pos->type)
    {
    case TALER_EXCHANGEDB_RO_BANK_TO_EXCHANGE:
      break;
    case TALER_EXCHANGEDB_RO_WITHDRAW_COIN:
      value = pos->details.withdraw->amount_with_fee;
      if (0 == ret)
      {
        withdraw_total = value;
      }
      else
      {
        if (GNUNET_OK !=
            TALER_amount_add (&withdraw_total,
                              &withdraw_total,
                              &value))
        {
          json_decref (json_history);
          return NULL;
        }
      }
      ret = 1;
      wr.purpose.purpose = htonl (TALER_SIGNATURE_WALLET_RESERVE_WITHDRAW);
      wr.purpose.size = htonl (sizeof (struct TALER_WithdrawRequestPS));
      wr.reserve_pub = pos->details.withdraw->reserve_pub;
      TALER_amount_hton (&wr.amount_with_fee,
                         &value);
      TALER_amount_hton (&wr.withdraw_fee,
                         &pos->details.withdraw->withdraw_fee);
      GNUNET_CRYPTO_rsa_public_key_hash (pos->details.withdraw->denom_pub.rsa_public_key,
                                         &wr.h_denomination_pub);
      wr.h_coin_envelope = pos->details.withdraw->h_coin_envelope;
      json_array_append_new (json_history,
                             json_pack ("{s:s, s:o, s:o, s:o}",
                                        "type", "WITHDRAW",
                                        "signature", GNUNET_JSON_from_data (&pos->details.withdraw->reserve_sig,
                                                                           sizeof (struct TALER_ReserveSignatureP)),
                                        "details", GNUNET_JSON_from_data (&wr,
                                                                         sizeof (wr)),
                                        "amount", TALER_JSON_from_amount (&value)));
      break;
    }
  }
  if (0 == ret)
  {
    /* did not encounter any withdraw operations, set to zero */
    TALER_amount_get_zero (deposit_total.currency,
                           &withdraw_total);
  }
  if (GNUNET_SYSERR ==
      TALER_amount_subtract (balance,
                             &deposit_total,
                             &withdraw_total))
  {
    GNUNET_break (0);
    json_decref (json_history);
    return NULL;
  }

  return json_history;
}


/**
 * Generate refund conflict failure message. Returns the
 * transaction list @a tl with the details about the conflict.
 *
 * @param connection connection to the client
 * @param tl transaction list showing the conflict
 * @return MHD result code
 */
int
TMH_RESPONSE_reply_refund_conflict (struct MHD_Connection *connection,
                                    const struct TALER_EXCHANGEDB_TransactionList *tl)
{
  return TMH_RESPONSE_reply_json_pack (connection,
                                       MHD_HTTP_CONFLICT,
                                       "{s:s, s:o}",
                                       "status", "conflicting refund",
                                       "history", compile_transaction_history (tl));
}


/**
 * Generate generic refund failure message. All the details
 * are in the @a response_code.  The body can be empty.
 *
 * @param connection connection to the client
 * @param response_code response code to generate
 * @return MHD result code
 */
int
TMH_RESPONSE_reply_refund_failure (struct MHD_Connection *connection,
                                   unsigned int response_code)
{
  return TMH_RESPONSE_reply_json_pack (connection,
                                       response_code,
                                       "{s:s}",
                                       "error",
                                       "no details");
}


/**
 * Generate successful refund confirmation message.
 *
 * @param connection connection to the client
 * @param refund details about the successful refund
 * @return MHD result code
 */
int
TMH_RESPONSE_reply_refund_success (struct MHD_Connection *connection,
                                   const struct TALER_EXCHANGEDB_Refund *refund)
{
  struct TALER_RefundConfirmationPS rc;
  struct TALER_ExchangePublicKeyP pub;
  struct TALER_ExchangeSignatureP sig;

  rc.purpose.purpose = htonl (TALER_SIGNATURE_EXCHANGE_CONFIRM_REFUND);
  rc.purpose.size = htonl (sizeof (struct TALER_RefundConfirmationPS));
  rc.h_contract = refund->h_contract;
  rc.transaction_id = GNUNET_htonll (refund->transaction_id);
  rc.coin_pub = refund->coin.coin_pub;
  rc.merchant = refund->merchant_pub;
  rc.rtransaction_id = GNUNET_htonll (refund->rtransaction_id);
  TALER_amount_hton (&rc.refund_amount,
                     &refund->refund_amount);
  TALER_amount_hton (&rc.refund_fee,
                     &refund->refund_fee);
  TMH_KS_sign (&rc.purpose,
               &pub,
               &sig);
  return TMH_RESPONSE_reply_json_pack (connection,
                                       MHD_HTTP_OK,
                                       "{s:s, s:o, s:o}",
                                       "status", "REFUND_OK",
                                       "sig", GNUNET_JSON_from_data (&sig,
                                                                     sizeof (sig)),
                                       "pub", GNUNET_JSON_from_data (&pub,
                                                                     sizeof (pub)));
}


/**
 * Send reserve status information to client.
 *
 * @param connection connection to the client
 * @param rh reserve history to return
 * @return MHD result code
 */
int
TMH_RESPONSE_reply_reserve_status_success (struct MHD_Connection *connection,
                                           const struct TALER_EXCHANGEDB_ReserveHistory *rh)
{
  json_t *json_balance;
  json_t *json_history;
  struct TALER_Amount balance;

  json_history = compile_reserve_history (rh,
                                          &balance);
  if (NULL == json_history)
    return TMH_RESPONSE_reply_internal_error (connection,
                                              "balance calculation failure");
  json_balance = TALER_JSON_from_amount (&balance);
  return TMH_RESPONSE_reply_json_pack (connection,
                                       MHD_HTTP_OK,
                                       "{s:o, s:o}",
                                       "balance", json_balance,
                                       "history", json_history);
}


/**
 * Send reserve status information to client with the
 * message that we have insufficient funds for the
 * requested /reserve/withdraw operation.
 *
 * @param connection connection to the client
 * @param rh reserve history to return
 * @return MHD result code
 */
int
TMH_RESPONSE_reply_reserve_withdraw_insufficient_funds (struct MHD_Connection *connection,
                                                        const struct TALER_EXCHANGEDB_ReserveHistory *rh)
{
  json_t *json_balance;
  json_t *json_history;
  struct TALER_Amount balance;

  json_history = compile_reserve_history (rh,
                                          &balance);
  if (NULL == json_history)
    return TMH_RESPONSE_reply_internal_error (connection,
                                              "balance calculation failure");
  json_balance = TALER_JSON_from_amount (&balance);
  return TMH_RESPONSE_reply_json_pack (connection,
                                       MHD_HTTP_PAYMENT_REQUIRED,
                                       "{s:s, s:o, s:o}",
                                       "error", "Insufficient funds",
                                       "balance", json_balance,
                                       "history", json_history);
}


/**
 * Send blinded coin information to client.
 *
 * @param connection connection to the client
 * @param collectable blinded coin to return
 * @return MHD result code
 */
int
TMH_RESPONSE_reply_reserve_withdraw_success (struct MHD_Connection *connection,
                                          const struct TALER_EXCHANGEDB_CollectableBlindcoin *collectable)
{
  json_t *sig_json;

  sig_json = GNUNET_JSON_from_rsa_signature (collectable->sig.rsa_signature);
  return TMH_RESPONSE_reply_json_pack (connection,
                                       MHD_HTTP_OK,
                                       "{s:o}",
                                       "ev_sig", sig_json);
}


/**
 * Send a response for a failed "/refresh/melt" request.  The
 * transaction history of the given coin demonstrates that the
 * @a residual value of the coin is below the @a requested
 * contribution of the coin for the melt.  Thus, the exchange
 * refuses the melt operation.
 *
 * @param connection the connection to send the response to
 * @param coin_pub public key of the coin
 * @param coin_value original value of the coin
 * @param tl transaction history for the coin
 * @param requested how much this coin was supposed to contribute, including fee
 * @param residual remaining value of the coin (after subtracting @a tl)
 * @return a MHD result code
 */
int
TMH_RESPONSE_reply_refresh_melt_insufficient_funds (struct MHD_Connection *connection,
                                                    const struct TALER_CoinSpendPublicKeyP *coin_pub,
                                                    struct TALER_Amount coin_value,
                                                    struct TALER_EXCHANGEDB_TransactionList *tl,
                                                    struct TALER_Amount requested,
                                                    struct TALER_Amount residual)
{
  json_t *history;

  history = compile_transaction_history (tl);
  if (NULL == history)
    return TMH_RESPONSE_reply_internal_db_error (connection);
  return TMH_RESPONSE_reply_json_pack (connection,
                                       MHD_HTTP_FORBIDDEN,
                                       "{s:s, s:o, s:o, s:o, s:o, s:o}",
                                       "error",
                                       "insufficient funds",
                                       "coin_pub",
                                       GNUNET_JSON_from_data (coin_pub,
                                                             sizeof (struct TALER_CoinSpendPublicKeyP)),
                                       "original_value",
                                       TALER_JSON_from_amount (&coin_value),
                                       "residual_value",
                                       TALER_JSON_from_amount (&residual),
                                       "requested_value",
                                       TALER_JSON_from_amount (&requested),
                                       "history",
                                       history);
}


/**
 * Send a response to a "/refresh/melt" request.
 *
 * @param connection the connection to send the response to
 * @param session_hash hash of the refresh session
 * @param noreveal_index which index will the client not have to reveal
 * @return a MHD status code
 */
int
TMH_RESPONSE_reply_refresh_melt_success (struct MHD_Connection *connection,
                                         const struct GNUNET_HashCode *session_hash,
                                         uint16_t noreveal_index)
{
  struct TALER_RefreshMeltConfirmationPS body;
  struct TALER_ExchangePublicKeyP pub;
  struct TALER_ExchangeSignatureP sig;
  json_t *sig_json;

  body.purpose.size = htonl (sizeof (struct TALER_RefreshMeltConfirmationPS));
  body.purpose.purpose = htonl (TALER_SIGNATURE_EXCHANGE_CONFIRM_MELT);
  body.session_hash = *session_hash;
  body.noreveal_index = htons (noreveal_index);
  body.reserved = htons (0);
  TMH_KS_sign (&body.purpose,
               &pub,
               &sig);
  sig_json = GNUNET_JSON_from_data (&sig,
                                   sizeof (sig));
  GNUNET_assert (NULL != sig_json);
  return TMH_RESPONSE_reply_json_pack (connection,
                                       MHD_HTTP_OK,
                                       "{s:i, s:o, s:o}",
                                       "noreveal_index", (int) noreveal_index,
                                       "exchange_sig", sig_json,
                                       "exchange_pub", GNUNET_JSON_from_data (&pub,
                                                                         sizeof (pub)));
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
TMH_RESPONSE_reply_refresh_reveal_success (struct MHD_Connection *connection,
                                           unsigned int num_newcoins,
                                           const struct TALER_DenominationSignature *sigs)
{
  int newcoin_index;
  json_t *root;
  json_t *obj;
  json_t *list;
  int ret;

  list = json_array ();
  for (newcoin_index = 0; newcoin_index < num_newcoins; newcoin_index++)
  {
    obj = json_object ();
    json_object_set_new (obj,
			 "ev_sig",
			 GNUNET_JSON_from_rsa_signature (sigs[newcoin_index].rsa_signature));
    json_array_append_new (list,
                           obj);
  }
  root = json_object ();
  json_object_set_new (root,
                       "ev_sigs",
                       list);
  ret = TMH_RESPONSE_reply_json (connection,
                                 root,
                                 MHD_HTTP_OK);
  json_decref (root);
  return ret;
}


/**
 * Send a response for a failed "/refresh/reveal", where the
 * revealed value(s) do not match the original commitment.
 *
 * @param connection the connection to send the response to
 * @param mc all information about the original commitment
 * @param off offset in the array of kappa-commitments where
 *            the missmatch was detected
 * @param j index of the coin for which the missmatch was
 *            detected
 * @param missmatch_object name of the object that was
 *            bogus (i.e. "transfer key").
 * @return a MHD result code
 */
int
TMH_RESPONSE_reply_refresh_reveal_missmatch (struct MHD_Connection *connection,
                                             const struct TALER_EXCHANGEDB_MeltCommitment *mc,
                                             unsigned int off,
                                             unsigned int j,
                                             const char *missmatch_object)
{
  json_t *info_old;
  json_t *info_new;
  json_t *info_commit;
  json_t *info_links;
  unsigned int i;
  unsigned int k;

  info_old = json_array ();
  for (i=0;i<mc->num_oldcoins;i++)
  {
    const struct TALER_EXCHANGEDB_RefreshMelt *rm;
    json_t *rm_json;

    rm = &mc->melts[i];
    rm_json = json_object ();
    json_object_set_new (rm_json,
                         "coin_sig",
                         GNUNET_JSON_from_data (&rm->coin_sig,
                                               sizeof (struct TALER_CoinSpendSignatureP)));
    json_object_set_new (rm_json,
                         "coin_pub",
                         GNUNET_JSON_from_data (&rm->coin.coin_pub,
                                               sizeof (struct TALER_CoinSpendPublicKeyP)));
    json_object_set_new (rm_json,
                         "melt_amount_with_fee",
                         TALER_JSON_from_amount (&rm->amount_with_fee));
    json_object_set_new (rm_json,
                         "melt_fee",
                         TALER_JSON_from_amount (&rm->melt_fee));
    json_array_append_new (info_old,
                           rm_json);
  }
  info_new = json_array ();
  for (i=0;i<mc->num_newcoins;i++)
  {
    const struct TALER_DenominationPublicKey *pk;

    pk = &mc->denom_pubs[i];
    json_array_append_new (info_new,
                           GNUNET_JSON_from_rsa_public_key (pk->rsa_public_key));

  }
  info_commit = json_array ();
  info_links = json_array ();
  for (k=0;k<TALER_CNC_KAPPA;k++)
  {
    json_t *info_commit_k;
    json_t *info_link_k;

    info_commit_k = json_array ();
    for (i=0;i<mc->num_newcoins;i++)
    {
      const struct TALER_EXCHANGEDB_RefreshCommitCoin *cc;
      json_t *cc_json;

      cc = &mc->commit_coins[k][i];
      cc_json = json_object ();
      json_object_set_new (cc_json,
                           "coin_ev",
                           GNUNET_JSON_from_data (cc->coin_ev,
                                                 cc->coin_ev_size));
      json_object_set_new (cc_json,
                           "coin_priv_enc",
                           GNUNET_JSON_from_data (cc->refresh_link->coin_priv_enc,
                                                 sizeof (struct TALER_CoinSpendPrivateKeyP)));
      json_object_set_new (cc_json,
                           "blinding_key_enc",
                           GNUNET_JSON_from_data (cc->refresh_link->blinding_key_enc,
                                                 cc->refresh_link->blinding_key_enc_size));

      json_array_append_new (info_commit_k,
                             cc_json);
    }
    json_array_append_new (info_commit,
                           info_commit_k);
    info_link_k = json_array ();
    for (i=0;i<mc->num_oldcoins;i++)
    {
      const struct TALER_RefreshCommitLinkP *cl;
      json_t *cl_json;

      cl = &mc->commit_links[k][i];
      cl_json = json_object ();
      json_object_set_new (cl_json,
                           "transfer_pub",
                           GNUNET_JSON_from_data (&cl->transfer_pub,
                                                 sizeof (struct TALER_TransferPublicKeyP)));
      json_object_set_new (cl_json,
                           "shared_secret_enc",
                           GNUNET_JSON_from_data (&cl->shared_secret_enc,
                                                 sizeof (struct TALER_EncryptedLinkSecretP)));
      json_array_append_new (info_link_k,
                             cl_json);
    }
    json_array_append_new (info_links,
                           info_link_k);
  }
  return TMH_RESPONSE_reply_json_pack (connection,
                                       MHD_HTTP_CONFLICT,
                                       "{s:s, s:i, s:i, s:o, s:o, s:o, s:o, s:s}",
                                       "error", "commitment violation",
                                       "offset", (int) off,
                                       "index", (int) j,
                                       "oldcoin_infos", info_old,
                                       "newcoin_infos", info_new,
                                       "commit_infos", info_commit,
                                       "link_infos", info_links,
                                       "object", missmatch_object);
}


/**
 * Send a response for "/refresh/link".
 *
 * @param connection the connection to send the response to
 * @param num_sessions number of sessions the coin was used in
 * @param sessions array of @a num_session entries with
 *                  information for each session
 * @return a MHD result code
 */
int
TMH_RESPONSE_reply_refresh_link_success (struct MHD_Connection *connection,
                                         unsigned int num_sessions,
                                         const struct TMH_RESPONSE_LinkSessionInfo *sessions)
{
  json_t *root;
  json_t *mlist;
  int res;
  unsigned int i;

  mlist = json_array ();
  for (i=0;i<num_sessions;i++)
  {
    const struct TALER_EXCHANGEDB_LinkDataList *pos;
    json_t *list = json_array ();

    for (pos = sessions[i].ldl; NULL != pos; pos = pos->next)
    {
      json_t *obj;

      obj = json_object ();
      json_object_set_new (obj,
                           "link_enc",
                           GNUNET_JSON_from_data (pos->link_data_enc->coin_priv_enc,
                                                 sizeof (struct TALER_CoinSpendPrivateKeyP) +
                                                 pos->link_data_enc->blinding_key_enc_size));
      json_object_set_new (obj,
                           "denom_pub",
                           GNUNET_JSON_from_rsa_public_key (pos->denom_pub.rsa_public_key));
      json_object_set_new (obj,
                           "ev_sig",
                           GNUNET_JSON_from_rsa_signature (pos->ev_sig.rsa_signature));
      json_array_append_new (list,
                             obj);
    }
    root = json_object ();
    json_object_set_new (root,
                         "new_coins",
                         list);
    json_object_set_new (root,
                         "transfer_pub",
                         GNUNET_JSON_from_data (&sessions[i].transfer_pub,
                                               sizeof (struct TALER_TransferPublicKeyP)));
    json_object_set_new (root,
                         "secret_enc",
                         GNUNET_JSON_from_data (&sessions[i].shared_secret_enc,
                                               sizeof (struct TALER_EncryptedLinkSecretP)));
    json_array_append_new (mlist,
                           root);
  }
  res = TMH_RESPONSE_reply_json (connection,
                                 mlist,
                                 MHD_HTTP_OK);
  json_decref (mlist);
  return res;
}


/**
 * A merchant asked for details about a deposit, but
 * we do not know anything about the deposit. Generate the
 * 404 reply.
 *
 * @param connection connection to the client
 * @return MHD result code
 */
int
TMH_RESPONSE_reply_deposit_unknown (struct MHD_Connection *connection)
{
  return TMH_RESPONSE_reply_json_pack (connection,
                                       MHD_HTTP_NOT_FOUND,
                                       "{s:s}",
                                       "error", "Deposit unknown");
}


/**
 * A merchant asked for details about a deposit, but
 * we did not execute the deposit yet. Generate a 202 reply.
 *
 * @param connection connection to the client
 * @param planned_exec_time planned execution time
 * @return MHD result code
 */
int
TMH_RESPONSE_reply_deposit_pending (struct MHD_Connection *connection,
				    struct GNUNET_TIME_Absolute planned_exec_time)
{
  return TMH_RESPONSE_reply_json_pack (connection,
                                       MHD_HTTP_ACCEPTED,
                                       "{s:o}",
                                       "execution_time", GNUNET_JSON_from_time_abs (planned_exec_time));
}


/**
 * A merchant asked for details about a deposit.  Provide
 * them. Generates the 200 reply.
 *
 * @param connection connection to the client
 * @param h_contract hash of the contract
 * @param h_wire hash of wire account details
 * @param coin_pub public key of the coin
 * @param coin_contribution how much did the coin we asked about
 *        contribute to the total transfer value? (deposit value minus fee)
 * @param transaction_id merchant transaction identifier
 * @param wtid raw wire transfer identifier
 * @param exec_time execution time of the wire transfer
 * @return MHD result code
 */
int
TMH_RESPONSE_reply_deposit_wtid (struct MHD_Connection *connection,
                                 const struct GNUNET_HashCode *h_contract,
                                 const struct GNUNET_HashCode *h_wire,
                                 const struct TALER_CoinSpendPublicKeyP *coin_pub,
                                 const struct TALER_Amount *coin_contribution,
                                 uint64_t transaction_id,
				 const struct TALER_WireTransferIdentifierRawP *wtid,
                                 struct GNUNET_TIME_Absolute exec_time)
{
  struct TALER_ConfirmWirePS cw;
  struct TALER_ExchangePublicKeyP pub;
  struct TALER_ExchangeSignatureP sig;

  cw.purpose.purpose = htonl (TALER_SIGNATURE_EXCHANGE_CONFIRM_WIRE);
  cw.purpose.size = htonl (sizeof (struct TALER_ConfirmWirePS));
  cw.h_wire = *h_wire;
  cw.h_contract = *h_contract;
  cw.wtid = *wtid;
  cw.coin_pub = *coin_pub;
  cw.transaction_id = GNUNET_htonll (transaction_id);
  cw.execution_time = GNUNET_TIME_absolute_hton (exec_time);
  TALER_amount_hton (&cw.coin_contribution,
                     coin_contribution);
  TMH_KS_sign (&cw.purpose,
               &pub,
               &sig);
  return TMH_RESPONSE_reply_json_pack (connection,
                                       MHD_HTTP_OK,
                                       "{s:o, s:o, s:o, s:o, s:o}",
                                       "wtid", GNUNET_JSON_from_data (wtid,
                                                                      sizeof (*wtid)),
                                       "execution_time", GNUNET_JSON_from_time_abs (exec_time),
                                       "coin_contribution", TALER_JSON_from_amount (coin_contribution),
                                       "exchange_sig", GNUNET_JSON_from_data (&sig,
                                                                              sizeof (sig)),
                                       "exchange_pub", GNUNET_JSON_from_data (&pub,
                                                                              sizeof (pub)));
}


/**
 * A merchant asked for transaction details about a wire transfer.
 * Provide them. Generates the 200 reply.
 *
 * @param connection connection to the client
 * @param total total amount that was transferred
 * @param merchant_pub public key of the merchant
 * @param h_wire destination account
 * @param wdd_head linked list with details about the combined deposits
 * @return MHD result code
 */
int
TMH_RESPONSE_reply_wire_deposit_details (struct MHD_Connection *connection,
                                         const struct TALER_Amount *total,
                                         const struct TALER_MerchantPublicKeyP *merchant_pub,
                                         const struct GNUNET_HashCode *h_wire,
                                         const struct TMH_WireDepositDetail *wdd_head)
{
  const struct TMH_WireDepositDetail *wdd_pos;
  json_t *deposits;
  struct TALER_WireDepositDetailP dd;
  struct GNUNET_HashContext *hash_context;
  struct TALER_WireDepositDataPS wdp;
  struct TALER_ExchangePublicKeyP pub;
  struct TALER_ExchangeSignatureP sig;

  deposits = json_array ();
  hash_context = GNUNET_CRYPTO_hash_context_start ();
  for (wdd_pos = wdd_head; NULL != wdd_pos; wdd_pos = wdd_pos->next)
  {
    dd.h_contract = wdd_pos->h_contract;
    dd.transaction_id = GNUNET_htonll (wdd_pos->transaction_id);
    dd.coin_pub = wdd_pos->coin_pub;
    TALER_amount_hton (&dd.deposit_value,
                       &wdd_pos->deposit_value);
    TALER_amount_hton (&dd.deposit_fee,
                       &wdd_pos->deposit_fee);
    GNUNET_CRYPTO_hash_context_read (hash_context,
                                     &dd,
                                     sizeof (struct TALER_WireDepositDetailP));
    json_array_append (deposits,
                       json_pack ("{s:o, s:I, s:o, s:o, s:o}",
                                  "H_contract", GNUNET_JSON_from_data (&wdd_pos->h_contract,
                                                                       sizeof (struct GNUNET_HashCode)),
                                  "transaction_id", (json_int_t) wdd_pos->transaction_id,
                                  "coin_pub", GNUNET_JSON_from_data (&wdd_pos->coin_pub,
                                                                     sizeof (struct TALER_CoinSpendPublicKeyP)),
                                  "deposit_value", TALER_JSON_from_amount (&wdd_pos->deposit_value),
                                  "deposit_fee", TALER_JSON_from_amount (&wdd_pos->deposit_fee)));
  }
  wdp.purpose.purpose = htonl (TALER_SIGNATURE_EXCHANGE_CONFIRM_WIRE_DEPOSIT);
  wdp.purpose.size = htonl (sizeof (struct TALER_WireDepositDataPS));
  TALER_amount_hton (&wdp.total,
                     total);
  wdp.merchant_pub = *merchant_pub;
  wdp.h_wire = *h_wire;
  GNUNET_CRYPTO_hash_context_finish (hash_context,
                                     &wdp.h_details);
  TMH_KS_sign (&wdp.purpose,
               &pub,
               &sig);
  return TMH_RESPONSE_reply_json_pack (connection,
                                       MHD_HTTP_OK,
                                       "{s:o, s:o, s:o, s:o, s:o, s:o}",
                                       "total", TALER_JSON_from_amount (total),
                                       "merchant_pub", GNUNET_JSON_from_data (merchant_pub,
                                                                             sizeof (struct TALER_MerchantPublicKeyP)),
                                       "H_wire", GNUNET_JSON_from_data (h_wire,
                                                                       sizeof (struct GNUNET_HashCode)),
                                       "deposits", deposits,
                                       "exchange_sig", GNUNET_JSON_from_data (&sig,
                                                                              sizeof (sig)),
                                       "exchange_pub", GNUNET_JSON_from_data (&pub,
                                                                              sizeof (pub)));
}


/* end of taler-exchange-httpd_responses.c */
