/*
  This file is part of TALER
  Copyright (C) 2014-2020 Taler Systems SA

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
 * @file taler-exchange-httpd_deposit.c
 * @brief Handle /deposit requests; parses the POST and JSON and
 *        verifies the coin signature before handing things off
 *        to the database.
 * @author Florian Dold
 * @author Benedikt Mueller
 * @author Christian Grothoff
 */
#include "platform.h"
#include <gnunet/gnunet_util_lib.h>
#include <gnunet/gnunet_json_lib.h>
#include <jansson.h>
#include <microhttpd.h>
#include <pthread.h>
#include "taler_json_lib.h"
#include "taler_mhd_lib.h"
#include "taler-exchange-httpd_deposit.h"
#include "taler-exchange-httpd_responses.h"
#include "taler-exchange-httpd_keystate.h"


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
 * @param h_contract_terms hash of contract details
 * @param timestamp client's timestamp
 * @param refund_deadline until when this deposit be refunded
 * @param merchant merchant public key
 * @param amount_without_fee fraction of coin value to deposit, without the fee
 * @return MHD result code
 */
static MHD_RESULT
reply_deposit_success (struct MHD_Connection *connection,
                       const struct TALER_CoinSpendPublicKeyP *coin_pub,
                       const struct GNUNET_HashCode *h_wire,
                       const struct GNUNET_HashCode *h_contract_terms,
                       struct GNUNET_TIME_Absolute timestamp,
                       struct GNUNET_TIME_Absolute refund_deadline,
                       const struct TALER_MerchantPublicKeyP *merchant,
                       const struct TALER_Amount *amount_without_fee)
{
  struct TALER_ExchangePublicKeyP pub;
  struct TALER_ExchangeSignatureP sig;
  struct TALER_DepositConfirmationPS dc = {
    .purpose.purpose = htonl (TALER_SIGNATURE_EXCHANGE_CONFIRM_DEPOSIT),
    .purpose.size = htonl (sizeof (dc)),
    .h_contract_terms = *h_contract_terms,
    .h_wire = *h_wire,
    .timestamp = GNUNET_TIME_absolute_hton (timestamp),
    .refund_deadline = GNUNET_TIME_absolute_hton (refund_deadline),
    .coin_pub = *coin_pub,
    .merchant = *merchant
  };

  TALER_amount_hton (&dc.amount_without_fee,
                     amount_without_fee);
  if (GNUNET_OK !=
      TEH_KS_sign (&dc,
                   &pub,
                   &sig))
  {
    return TALER_MHD_reply_with_error (connection,
                                       MHD_HTTP_INTERNAL_SERVER_ERROR,
                                       TALER_EC_EXCHANGE_BAD_CONFIGURATION,
                                       "no keys");
  }
  return TALER_MHD_reply_json_pack (connection,
                                    MHD_HTTP_OK,
                                    "{s:s, s:o, s:o}",
                                    "status", "DEPOSIT_OK",
                                    "sig", GNUNET_JSON_from_data_auto (&sig),
                                    "pub", GNUNET_JSON_from_data_auto (&pub));
}


/**
 * Closure for #deposit_transaction.
 */
struct DepositContext
{
  /**
   * Information about the deposit request.
   */
  const struct TALER_EXCHANGEDB_Deposit *deposit;

  /**
   * Value of the coin.
   */
  struct TALER_Amount value;

};


/**
 * Execute database transaction for /deposit.  Runs the transaction
 * logic; IF it returns a non-error code, the transaction logic MUST
 * NOT queue a MHD response.  IF it returns an hard error, the
 * transaction logic MUST queue a MHD response and set @a mhd_ret.  IF
 * it returns the soft error code, the function MAY be called again to
 * retry and MUST not queue a MHD response.
 *
 * @param cls a `struct DepositContext`
 * @param connection MHD request context
 * @param session database session and transaction to use
 * @param[out] mhd_ret set to MHD status on error
 * @return transaction status
 */
static enum GNUNET_DB_QueryStatus
deposit_transaction (void *cls,
                     struct MHD_Connection *connection,
                     struct TALER_EXCHANGEDB_Session *session,
                     MHD_RESULT *mhd_ret)
{
  struct DepositContext *dc = cls;
  const struct TALER_EXCHANGEDB_Deposit *deposit = dc->deposit;
  struct TALER_Amount spent;
  enum GNUNET_DB_QueryStatus qs;

  qs = TEH_plugin->have_deposit (TEH_plugin->cls,
                                 session,
                                 deposit,
                                 GNUNET_YES /* check refund deadline */);
  if (qs < 0)
  {
    if (GNUNET_DB_STATUS_HARD_ERROR == qs)
    {
      *mhd_ret = TALER_MHD_reply_with_error (connection,
                                             MHD_HTTP_INTERNAL_SERVER_ERROR,
                                             TALER_EC_DEPOSIT_HISTORY_DB_ERROR,
                                             "Could not check for existing identical deposit");
      return GNUNET_DB_STATUS_HARD_ERROR;
    }
    return qs;
  }
  if (GNUNET_DB_STATUS_SUCCESS_ONE_RESULT == qs)
  {
    struct TALER_Amount amount_without_fee;

    GNUNET_log (GNUNET_ERROR_TYPE_INFO,
                "/deposit replay, accepting again!\n");
    GNUNET_assert (0 <=
                   TALER_amount_subtract (&amount_without_fee,
                                          &deposit->amount_with_fee,
                                          &deposit->deposit_fee));
    *mhd_ret = reply_deposit_success (connection,
                                      &deposit->coin.coin_pub,
                                      &deposit->h_wire,
                                      &deposit->h_contract_terms,
                                      deposit->timestamp,
                                      deposit->refund_deadline,
                                      &deposit->merchant_pub,
                                      &amount_without_fee);
    /* Treat as 'hard' DB error as we want to rollback and
       never try again. */
    return GNUNET_DB_STATUS_HARD_ERROR;
  }

  /* Start with fee for THIS transaction */
  spent = deposit->amount_with_fee;
  /* add cost of all previous transactions; skip RECOUP as revoked
     denominations are not eligible for deposit, and if we are the old coin
     pub of a revoked coin (aka a zombie), then ONLY refresh is allowed. */
  {
    struct TALER_EXCHANGEDB_TransactionList *tl;

    qs = TEH_plugin->get_coin_transactions (TEH_plugin->cls,
                                            session,
                                            &deposit->coin.coin_pub,
                                            GNUNET_NO,
                                            &tl);
    if (0 > qs)
    {
      if (GNUNET_DB_STATUS_HARD_ERROR == qs)
        *mhd_ret = TALER_MHD_reply_with_error (
          connection,
          MHD_HTTP_INTERNAL_SERVER_ERROR,
          TALER_EC_DEPOSIT_HISTORY_DB_ERROR,
          "could not fetch coin transaction history");
      return qs;
    }
    if (GNUNET_OK !=
        TALER_EXCHANGEDB_calculate_transaction_list_totals (tl,
                                                            &spent, /* starting offset */
                                                            &spent /* result */))
    {
      TEH_plugin->free_coin_transaction_list (TEH_plugin->cls,
                                              tl);
      *mhd_ret = TALER_MHD_reply_with_error (
        connection,
        MHD_HTTP_INTERNAL_SERVER_ERROR,
        TALER_EC_DEPOSIT_HISTORY_DB_ERROR,
        "could not calculate historic coin amount total");
      return GNUNET_DB_STATUS_HARD_ERROR;
    }
    /* Check that cost of all transactions (including the current one) is
       smaller (or equal) than the value of the coin. */
    if (0 < TALER_amount_cmp (&spent,
                              &dc->value))
    {
      GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                  "Deposited coin has insufficient funds left!\n");
      *mhd_ret = TEH_RESPONSE_reply_coin_insufficient_funds (connection,
                                                             TALER_EC_DEPOSIT_INSUFFICIENT_FUNDS,
                                                             &deposit->coin.
                                                             coin_pub,
                                                             tl);
      TEH_plugin->free_coin_transaction_list (TEH_plugin->cls,
                                              tl);
      return GNUNET_DB_STATUS_HARD_ERROR;
    }
    TEH_plugin->free_coin_transaction_list (TEH_plugin->cls,
                                            tl);
  }
  qs = TEH_plugin->insert_deposit (TEH_plugin->cls,
                                   session,
                                   deposit);
  if (GNUNET_DB_STATUS_HARD_ERROR == qs)
  {
    TALER_LOG_WARNING ("Failed to store /deposit information in database\n");
    *mhd_ret = TALER_MHD_reply_with_error (connection,
                                           MHD_HTTP_INTERNAL_SERVER_ERROR,
                                           TALER_EC_DEPOSIT_STORE_DB_ERROR,
                                           "Could not persist /deposit data");
  }
  return qs;
}


/**
 * Check that @a ts is reasonably close to our own RTC.
 *
 * @param ts timestamp to check
 * @return #GNUNET_OK if @a ts is reasonable
 */
static int
check_timestamp_current (struct GNUNET_TIME_Absolute ts)
{
  struct GNUNET_TIME_Relative r;
  struct GNUNET_TIME_Relative tolerance;

  /* Let's be VERY generous (after all, this is basically about
     which year the deposit counts for in terms of tax purposes) */
  tolerance = GNUNET_TIME_UNIT_MONTHS;
  r = GNUNET_TIME_absolute_get_duration (ts);
  if (r.rel_value_us > tolerance.rel_value_us)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                "Deposit timestamp too old: %llu vs %llu > %llu\n",
                (unsigned long long) ts.abs_value_us,
                (unsigned long long) GNUNET_TIME_absolute_get ().abs_value_us,
                (unsigned long long) tolerance.rel_value_us);
    return GNUNET_SYSERR;
  }
  r = GNUNET_TIME_absolute_get_remaining (ts);
  if (r.rel_value_us > tolerance.rel_value_us)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                "Deposit timestamp too new: %llu vs %llu < - %llu\n",
                (unsigned long long) ts.abs_value_us,
                (unsigned long long) GNUNET_TIME_absolute_get ().abs_value_us,
                (unsigned long long) tolerance.rel_value_us);
    return GNUNET_SYSERR;
  }
  return GNUNET_OK;
}


/**
 * Handle a "/coins/$COIN_PUB/deposit" request.  Parses the JSON, and, if
 * successful, passes the JSON data to #deposit_transaction() to
 * further check the details of the operation specified.  If everything checks
 * out, this will ultimately lead to the "/deposit" being executed, or
 * rejected.
 *
 * @param connection the MHD connection to handle
 * @param coin_pub public key of the coin
 * @param root uploaded JSON data
 * @return MHD result code
  */
MHD_RESULT
TEH_handler_deposit (struct MHD_Connection *connection,
                     const struct TALER_CoinSpendPublicKeyP *coin_pub,
                     const json_t *root)
{
  json_t *wire;
  struct DepositContext dc;
  struct TALER_EXCHANGEDB_Deposit deposit;
  struct GNUNET_HashCode my_h_wire;
  struct GNUNET_JSON_Specification spec[] = {
    GNUNET_JSON_spec_json ("wire", &wire),
    TALER_JSON_spec_amount ("contribution", &deposit.amount_with_fee),
    GNUNET_JSON_spec_fixed_auto ("denom_pub_hash",
                                 &deposit.coin.denom_pub_hash),
    TALER_JSON_spec_denomination_signature ("ub_sig", &deposit.coin.denom_sig),
    GNUNET_JSON_spec_fixed_auto ("merchant_pub", &deposit.merchant_pub),
    GNUNET_JSON_spec_fixed_auto ("h_contract_terms", &deposit.h_contract_terms),
    GNUNET_JSON_spec_fixed_auto ("h_wire", &deposit.h_wire),
    GNUNET_JSON_spec_fixed_auto ("coin_sig",  &deposit.csig),
    GNUNET_JSON_spec_absolute_time ("timestamp", &deposit.timestamp),
    GNUNET_JSON_spec_absolute_time ("refund_deadline",
                                    &deposit.refund_deadline),
    GNUNET_JSON_spec_absolute_time ("wire_transfer_deadline",
                                    &deposit.wire_deadline),
    GNUNET_JSON_spec_end ()
  };

  memset (&deposit,
          0,
          sizeof (deposit));
  deposit.coin.coin_pub = *coin_pub;
  {
    int res;

    res = TALER_MHD_parse_json_data (connection,
                                     root,
                                     spec);
    if (GNUNET_SYSERR == res)
    {
      GNUNET_break (0);
      return MHD_NO; /* hard failure */
    }
    if (GNUNET_NO == res)
    {
      GNUNET_break_op (0);
      return MHD_YES; /* failure */
    }
  }
  deposit.receiver_wire_account = wire;
  if (deposit.refund_deadline.abs_value_us > deposit.wire_deadline.abs_value_us)
  {
    GNUNET_break_op (0);
    GNUNET_JSON_parse_free (spec);
    return TALER_MHD_reply_with_error (connection,
                                       MHD_HTTP_BAD_REQUEST,
                                       TALER_EC_DEPOSIT_REFUND_DEADLINE_AFTER_WIRE_DEADLINE,
                                       "refund_deadline");
  }

  if (GNUNET_OK !=
      check_timestamp_current (deposit.timestamp))
  {
    GNUNET_break_op (0);
    GNUNET_JSON_parse_free (spec);
    return TALER_MHD_reply_with_error (connection,
                                       MHD_HTTP_BAD_REQUEST,
                                       TALER_EC_DEPOSIT_INVALID_TIMESTAMP,
                                       "timestamp");
  }
  if (GNUNET_OK !=
      TALER_JSON_merchant_wire_signature_hash (wire,
                                               &my_h_wire))
  {
    TALER_LOG_WARNING (
      "Failed to parse JSON wire format specification for /deposit request\n");
    GNUNET_JSON_parse_free (spec);
    return TALER_MHD_reply_with_error (connection,
                                       MHD_HTTP_BAD_REQUEST,
                                       TALER_EC_DEPOSIT_INVALID_WIRE_FORMAT_JSON,
                                       "wire");
  }
  if (0 != GNUNET_memcmp (&deposit.h_wire,
                          &my_h_wire))
  {
    /* Client hashed wire details differently than we did, reject */
    GNUNET_JSON_parse_free (spec);
    return TALER_MHD_reply_with_error (connection,
                                       MHD_HTTP_BAD_REQUEST,
                                       TALER_EC_DEPOSIT_INVALID_WIRE_FORMAT_CONTRACT_HASH_CONFLICT,
                                       "h_wire");
  }

  /* check denomination exists and is valid */
  {
    struct TEH_KS_StateHandle *key_state;
    struct TALER_EXCHANGEDB_DenominationKey *dki;
    enum TALER_ErrorCode ec;
    unsigned int hc;

    key_state = TEH_KS_acquire (GNUNET_TIME_absolute_get ());
    if (NULL == key_state)
    {
      TALER_LOG_ERROR ("Lacking keys to operate\n");
      GNUNET_JSON_parse_free (spec);
      return TALER_MHD_reply_with_error (connection,
                                         MHD_HTTP_INTERNAL_SERVER_ERROR,
                                         TALER_EC_EXCHANGE_BAD_CONFIGURATION,
                                         "no keys");
    }
    dki = TEH_KS_denomination_key_lookup_by_hash (key_state,
                                                  &deposit.coin.denom_pub_hash,
                                                  TEH_KS_DKU_DEPOSIT,
                                                  &ec,
                                                  &hc);
    if (NULL == dki)
    {
      TALER_LOG_DEBUG ("Unknown denomination key in /deposit request\n");
      TEH_KS_release (key_state);
      GNUNET_JSON_parse_free (spec);
      return TALER_MHD_reply_with_error (connection,
                                         hc,
                                         ec,
                                         "Could not find denomination key used in deposit");
    }
    TALER_amount_ntoh (&deposit.deposit_fee,
                       &dki->issue.properties.fee_deposit);
    if (GNUNET_YES !=
        TALER_amount_cmp_currency (&deposit.amount_with_fee,
                                   &deposit.deposit_fee) )
    {
      GNUNET_break_op (0);
      TEH_KS_release (key_state);
      GNUNET_JSON_parse_free (spec);
      return TALER_MHD_reply_with_error (connection,
                                         MHD_HTTP_BAD_REQUEST,
                                         TALER_EC_DEPOSIT_CURRENCY_MISMATCH,
                                         "contribution");
    }
    /* check coin signature */
    if (GNUNET_YES !=
        TALER_test_coin_valid (&deposit.coin,
                               &dki->denom_pub))
    {
      TALER_LOG_WARNING ("Invalid coin passed for /deposit\n");
      TEH_KS_release (key_state);
      GNUNET_JSON_parse_free (spec);
      return TALER_MHD_reply_with_error (connection,
                                         MHD_HTTP_UNAUTHORIZED,
                                         TALER_EC_DEPOSIT_DENOMINATION_SIGNATURE_INVALID,
                                         "ub_sig");
    }
    TALER_amount_ntoh (&dc.value,
                       &dki->issue.properties.value);
    TEH_KS_release (key_state);
  }
  if (0 < TALER_amount_cmp (&deposit.deposit_fee,
                            &deposit.amount_with_fee))
  {
    GNUNET_break_op (0);
    GNUNET_JSON_parse_free (spec);
    return TALER_MHD_reply_with_error (connection,
                                       MHD_HTTP_BAD_REQUEST,
                                       TALER_EC_DEPOSIT_NEGATIVE_VALUE_AFTER_FEE,
                                       "deposited amount smaller than depositing fee");
  }

  /* make sure coin is 'known' in database */
  {
    MHD_RESULT mhd_ret;
    struct TEH_DB_KnowCoinContext kcc = {
      .coin = &deposit.coin,
      .connection = connection
    };

    if (GNUNET_OK !=
        TEH_DB_run_transaction (connection,
                                "know coin for deposit",
                                &mhd_ret,
                                &TEH_DB_know_coin_transaction,
                                &kcc))
    {
      GNUNET_JSON_parse_free (spec);
      return mhd_ret;
    }
  }

  /* check deposit signature */
  {
    struct TALER_DepositRequestPS dr = {
      .purpose.purpose = htonl (TALER_SIGNATURE_WALLET_COIN_DEPOSIT),
      .purpose.size = htonl (sizeof (dr)),
      .h_contract_terms = deposit.h_contract_terms,
      .h_wire = deposit.h_wire,
      .timestamp = GNUNET_TIME_absolute_hton (deposit.timestamp),
      .refund_deadline = GNUNET_TIME_absolute_hton (deposit.refund_deadline),
      .merchant = deposit.merchant_pub,
      .coin_pub = deposit.coin.coin_pub
    };

    TALER_amount_hton (&dr.amount_with_fee,
                       &deposit.amount_with_fee);
    TALER_amount_hton (&dr.deposit_fee,
                       &deposit.deposit_fee);
    if (GNUNET_OK !=
        GNUNET_CRYPTO_eddsa_verify (TALER_SIGNATURE_WALLET_COIN_DEPOSIT,
                                    &dr,
                                    &deposit.csig.eddsa_signature,
                                    &deposit.coin.coin_pub.eddsa_pub))
    {
      TALER_LOG_WARNING ("Invalid signature on /deposit request\n");
      GNUNET_JSON_parse_free (spec);
      return TALER_MHD_reply_with_error (connection,
                                         MHD_HTTP_UNAUTHORIZED,
                                         TALER_EC_DEPOSIT_COIN_SIGNATURE_INVALID,
                                         "coin_sig");
    }
  }

  /* execute transaction */
  dc.deposit = &deposit;
  {
    MHD_RESULT mhd_ret;

    if (GNUNET_OK !=
        TEH_DB_run_transaction (connection,
                                "execute deposit",
                                &mhd_ret,
                                &deposit_transaction,
                                &dc))
    {
      GNUNET_JSON_parse_free (spec);
      return mhd_ret;
    }
  }

  /* generate regular response */
  {
    struct TALER_Amount amount_without_fee;
    MHD_RESULT res;

    GNUNET_assert (0 <=
                   TALER_amount_subtract (&amount_without_fee,
                                          &deposit.amount_with_fee,
                                          &deposit.deposit_fee));
    res = reply_deposit_success (connection,
                                 &deposit.coin.coin_pub,
                                 &deposit.h_wire,
                                 &deposit.h_contract_terms,
                                 deposit.timestamp,
                                 deposit.refund_deadline,
                                 &deposit.merchant_pub,
                                 &amount_without_fee);
    GNUNET_JSON_parse_free (spec);
    return res;
  }
}


/* end of taler-exchange-httpd_deposit.c */
