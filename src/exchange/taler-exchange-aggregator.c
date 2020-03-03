/*
  This file is part of TALER
  Copyright (C) 2016-2020 Taler Systems SA

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
 * @file taler-exchange-aggregator.c
 * @brief Process that aggregates outgoing transactions and executes them
 * @author Christian Grothoff
 */
#include "platform.h"
#include <gnunet/gnunet_util_lib.h>
#include <jansson.h>
#include <pthread.h>
#include "taler_exchangedb_lib.h"
#include "taler_exchangedb_plugin.h"
#include "taler_json_lib.h"
#include "taler_bank_service.h"


/**
 * Information we keep for each supported account.
 */
struct WireAccount
{
  /**
   * Accounts are kept in a DLL.
   */
  struct WireAccount *next;

  /**
   * Plugins are kept in a DLL.
   */
  struct WireAccount *prev;

  /**
   * Authentication data.
   */
  struct TALER_BANK_AuthenticationData auth;

  /**
   * Wire transfer fee structure.
   */
  struct TALER_EXCHANGEDB_AggregateFees *af;

  /**
   * Name of the section that configures this account.
   */
  char *section_name;

  /**
   * Name of the wire method underlying the account.
   */
  char *method;

};


/**
 * Data we keep to #run_transfers().  There is at most
 * one of these around at any given point in time.
 */
struct WirePrepareData
{

  /**
   * Database session for all of our transactions.
   */
  struct TALER_EXCHANGEDB_Session *session;

  /**
   * Wire execution handle.
   */
  struct TALER_BANK_TransferHandle *eh;

  /**
   * Wire account used for this preparation.
   */
  struct WireAccount *wa;

  /**
   * Row ID of the transfer.
   */
  unsigned long long row_id;

};


/**
 * Information about one aggregation process to be executed.  There is
 * at most one of these around at any given point in time.
 */
struct AggregationUnit
{
  /**
   * Public key of the merchant.
   */
  struct TALER_MerchantPublicKeyP merchant_pub;

  /**
   * Total amount to be transferred, before subtraction of @e wire_fee and rounding down.
   */
  struct TALER_Amount total_amount;

  /**
   * Final amount to be transferred (after fee and rounding down).
   */
  struct TALER_Amount final_amount;

  /**
   * Wire fee we charge for @e wp at @e execution_time.
   */
  struct TALER_Amount wire_fee;

  /**
   * Hash of @e wire.
   */
  struct GNUNET_HashCode h_wire;

  /**
   * Wire transfer identifier we use.
   */
  struct TALER_WireTransferIdentifierRawP wtid;

  /**
   * Row ID of the transaction that started it all.
   */
  unsigned long long row_id;

  /**
   * The current time.
   */
  struct GNUNET_TIME_Absolute execution_time;

  /**
   * Wire details of the merchant.
   */
  json_t *wire;

  /**
   * Wire account to be used for the preparation.
   */
  struct WireAccount *wa;

  /**
   * Database session for all of our transactions.
   */
  struct TALER_EXCHANGEDB_Session *session;

  /**
   * Wire preparation handle.
   */
  struct TALER_BANK_PrepareHandle *ph;

  /**
   * Array of #aggregation_limit row_ids from the
   * aggregation.
   */
  unsigned long long *additional_rows;

  /**
   * Offset specifying how many #additional_rows are in use.
   */
  unsigned int rows_offset;

  /**
   * Set to #GNUNET_YES if we have to abort due to failure.
   */
  int failed;

  /**
   * Set to #GNUNET_YES if we encountered a refund during #refund_by_coin_cb.
   * Used to wave the deposit fee.
   */
  int have_refund;
};


/**
 * Context we use while closing a reserve.
 */
struct CloseTransferContext
{

  /**
   * Our database session.
   */
  struct TALER_EXCHANGEDB_Session *session;

  /**
   * Wire transfer method.
   */
  char *method;

  /**
   * Wire account used for closing the reserve.
   */
  struct WireAccount *wa;
};


/**
 * Active context while processing reserve closing,
 * or NULL.
 */
static struct CloseTransferContext *ctc;

/**
 * Which currency is used by this exchange?
 */
static char *exchange_currency_string;

/**
 * How many fractional digits does the currency use?
 */
static struct TALER_Amount currency_round_unit;

/**
 * What is the base URL of this exchange?
 */
static char *exchange_base_url;

/**
 * The exchange's configuration (global)
 */
static struct GNUNET_CONFIGURATION_Handle *cfg;

/**
 * Our DB plugin.
 */
static struct TALER_EXCHANGEDB_Plugin *db_plugin;

/**
 * Head of list wire accounts of the exchange.
 */
static struct WireAccount *wa_head;

/**
 * Head of list wire accounts of the exchange.
 */
static struct WireAccount *wa_tail;

/**
 * Next task to run, if any.
 */
static struct GNUNET_SCHEDULER_Task *task;

/**
 * If we are currently executing a transfer, information about
 * the active transfer is here. Otherwise, this variable is NULL.
 */
static struct WirePrepareData *wpd;

/**
 * If we are currently aggregating transactions, information about the
 * active aggregation is here. Otherwise, this variable is NULL.
 */
static struct AggregationUnit *au;

/**
 * Handle to the context for interacting with the bank.
 */
static struct GNUNET_CURL_Context *ctx;

/**
 * Scheduler context for running the @e ctx.
 */
static struct GNUNET_CURL_RescheduleContext *rc;

/**
 * How long should we sleep when idle before trying to find more work?
 */
static struct GNUNET_TIME_Relative aggregator_idle_sleep_interval;

/**
 * Value to return from main(). #GNUNET_OK on success, #GNUNET_SYSERR
 * on serious errors.
 */
static int global_ret;

/**
 * #GNUNET_YES if we are in test mode and should exit when idle.
 */
static int test_mode;

/**
 * Did #run_reserve_closures() have any work during its last run?
 */
static int reserves_idle;

/**
 * Limit on the number of transactions we aggregate at once.  Note
 * that the limit must be big enough to ensure that when transactions
 * of the smallest possible unit are aggregated, they do surpass the
 * "tiny" threshold beyond which we never trigger a wire transaction!
 *
 * Note: do not change here, Postgres requires us to hard-code the
 * LIMIT in the prepared statement.
 */
static unsigned int aggregation_limit =
  TALER_EXCHANGEDB_MATCHING_DEPOSITS_LIMIT;


/**
 * Main work function that finds and triggers transfers for reserves
 * closures.
 *
 * @param cls closure
 */
static void
run_reserve_closures (void *cls);


/**
 * Main work function that queries the DB and aggregates transactions
 * into larger wire transfers.
 *
 * @param cls NULL
 */
static void
run_aggregation (void *cls);


/**
 * Execute the wire transfers that we have committed to
 * do.
 *
 * @param cls pointer to an `int` which we will return from main()
 */
static void
run_transfers (void *cls);


/**
 * Find the record valid at time @a now in the fee
 * structure.
 *
 * @param wa wire transfer fee data structure to update
 * @param now timestamp to update fees to
 * @return fee valid at @a now, or NULL if unknown
 */
static struct TALER_EXCHANGEDB_AggregateFees *
advance_fees (struct WireAccount *wa,
              struct GNUNET_TIME_Absolute now)
{
  struct TALER_EXCHANGEDB_AggregateFees *af;

  /* First, try to see if we have current fee information in memory */
  af = wa->af;
  while ( (NULL != af) &&
          (af->end_date.abs_value_us < now.abs_value_us) )
    af = af->next;
  return af;
}


/**
 * Update wire transfer fee data structure in @a wa.
 *
 * @param wa wire account data structure to update
 * @param now timestamp to update fees to
 * @param session DB session to use
 * @return fee valid at @a now, or NULL if unknown
 */
static struct TALER_EXCHANGEDB_AggregateFees *
update_fees (struct WireAccount *wa,
             struct GNUNET_TIME_Absolute now,
             struct TALER_EXCHANGEDB_Session *session)
{
  enum GNUNET_DB_QueryStatus qs;
  struct TALER_EXCHANGEDB_AggregateFees *af;

  af = advance_fees (wa,
                     now);
  if (NULL != af)
    return af;
  /* Let's try to load it from disk... */
  wa->af = TALER_EXCHANGEDB_fees_read (cfg,
                                       wa->method);
  for (struct TALER_EXCHANGEDB_AggregateFees *p = wa->af;
       NULL != p;
       p = p->next)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_INFO,
                "Persisting fees starting at %s in database\n",
                GNUNET_STRINGS_absolute_time_to_string (p->start_date));
    qs = db_plugin->insert_wire_fee (db_plugin->cls,
                                     session,
                                     wa->method,
                                     p->start_date,
                                     p->end_date,
                                     &p->wire_fee,
                                     &p->closing_fee,
                                     &p->master_sig);
    if (qs < 0)
    {
      GNUNET_break (GNUNET_DB_STATUS_SOFT_ERROR == qs);
      TALER_EXCHANGEDB_fees_free (wa->af);
      wa->af = NULL;
      return NULL;
    }
  }
  af = advance_fees (wa,
                     now);
  if (NULL != af)
    return af;
  GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
              "Failed to find current wire transfer fees for `%s'\n",
              wa->method);
  return NULL;
}


/**
 * Find the wire plugin for the given payto:// URL
 *
 * @param method wire method we need an account for
 * @return NULL on error
 */
static struct WireAccount *
find_account_by_method (const char *method)
{
  for (struct WireAccount *wa = wa_head; NULL != wa; wa = wa->next)
    if (0 == strcmp (method,
                     wa->method))
      return wa;
  return NULL;
}


/**
 * Find the wire plugin for the given payto:// URL
 *
 * @param url wire address we need an account for
 * @return NULL on error
 */
static struct WireAccount *
find_account_by_payto_uri (const char *url)
{
  char *method;
  struct WireAccount *wa;

  method = TALER_payto_get_method (url);
  if (NULL == method)
  {
    fprintf (stderr,
             "Invalid payto:// URL `%s'\n",
             url);
    return NULL;
  }
  wa = find_account_by_method (method);
  GNUNET_free (method);
  return wa;
}


/**
 * Function called with information about a wire account.  Adds
 * the account to our list.
 *
 * @param cls closure, NULL
 * @param ai account information
 */
static void
add_account_cb (void *cls,
                const struct TALER_EXCHANGEDB_AccountInfo *ai)
{
  struct WireAccount *wa;
  char *payto_uri;

  (void) cls;
  if (GNUNET_YES != ai->debit_enabled)
    return; /* not enabled for us, skip */
  wa = GNUNET_new (struct WireAccount);
  if (GNUNET_OK !=
      GNUNET_CONFIGURATION_get_value_string (cfg,
                                             ai->section_name,
                                             "PAYTO_URI",
                                             &payto_uri))
  {
    GNUNET_log_config_missing (GNUNET_ERROR_TYPE_ERROR,
                               ai->section_name,
                               "PAYTO_URI");
    GNUNET_free (wa);
    return;
  }
  wa->method = TALER_payto_get_method (payto_uri);
  GNUNET_free (payto_uri);
  if (GNUNET_OK !=
      TALER_BANK_auth_parse_cfg (cfg,
                                 ai->section_name,
                                 &wa->auth))
  {
    GNUNET_log (GNUNET_ERROR_TYPE_MESSAGE,
                "Failed to load exchange account `%s'\n",
                ai->section_name);
    GNUNET_free (wa->method);
    GNUNET_free (wa);
    return;
  }
  wa->section_name = GNUNET_strdup (ai->section_name);
  GNUNET_CONTAINER_DLL_insert (wa_head,
                               wa_tail,
                               wa);
}


/**
 * Free data stored in #au.
 */
static void
cleanup_au (void)
{
  if (NULL == au)
    return;
  GNUNET_free_non_null (au->additional_rows);
  if (NULL != au->wire)
  {
    json_decref (au->wire);
    au->wire = NULL;
  }
  GNUNET_free (au);
  au = NULL;
}


/**
 * We're being aborted with CTRL-C (or SIGTERM). Shut down.
 *
 * @param cls closure
 */
static void
shutdown_task (void *cls)
{
  (void) cls;
  if (NULL != ctx)
  {
    GNUNET_CURL_fini (ctx);
    ctx = NULL;
  }
  if (NULL != rc)
  {
    GNUNET_CURL_gnunet_rc_destroy (rc);
    rc = NULL;
  }
  GNUNET_log (GNUNET_ERROR_TYPE_INFO,
              "Running shutdown\n");
  if (NULL != task)
  {
    GNUNET_SCHEDULER_cancel (task);
    task = NULL;
  }
  if (NULL != wpd)
  {
    if (NULL != wpd->eh)
    {
      TALER_BANK_transfer_cancel (wpd->eh);
      wpd->eh = NULL;
    }
    db_plugin->rollback (db_plugin->cls,
                         wpd->session);
    GNUNET_free (wpd);
    wpd = NULL;
  }
  if (NULL != au)
  {
    db_plugin->rollback (db_plugin->cls,
                         au->session);
    cleanup_au ();
  }
  if (NULL != ctc)
  {
    db_plugin->rollback (db_plugin->cls,
                         ctc->session);
    GNUNET_free (ctc->method);
    GNUNET_free (ctc);
    ctc = NULL;
  }
  TALER_EXCHANGEDB_plugin_unload (db_plugin);
  db_plugin = NULL;

  {
    struct WireAccount *wa;

    while (NULL != (wa = wa_head))
    {
      GNUNET_CONTAINER_DLL_remove (wa_head,
                                   wa_tail,
                                   wa);
      TALER_BANK_auth_free (&wa->auth);
      TALER_EXCHANGEDB_fees_free (wa->af);
      GNUNET_free (wa->section_name);
      GNUNET_free (wa->method);
      GNUNET_free (wa);
    }
  }
  GNUNET_CONFIGURATION_destroy (cfg);
  cfg = NULL;
}


/**
 * Parse the configuration for wirewatch.
 *
 * @return #GNUNET_OK on success
 */
static int
parse_wirewatch_config ()
{
  if (GNUNET_OK !=
      GNUNET_CONFIGURATION_get_value_string (cfg,
                                             "exchange",
                                             "BASE_URL",
                                             &exchange_base_url))
  {
    GNUNET_log_config_missing (GNUNET_ERROR_TYPE_ERROR,
                               "exchange",
                               "BASE_URL");
    return GNUNET_SYSERR;
  }
  if (GNUNET_OK !=
      GNUNET_CONFIGURATION_get_value_time (cfg,
                                           "exchange",
                                           "AGGREGATOR_IDLE_SLEEP_INTERVAL",
                                           &aggregator_idle_sleep_interval))
  {
    GNUNET_log_config_missing (GNUNET_ERROR_TYPE_ERROR,
                               "exchange",
                               "AGGREGATOR_IDLE_SLEEP_INTERVAL");
    return GNUNET_SYSERR;
  }
  if (GNUNET_OK !=
      GNUNET_CONFIGURATION_get_value_string (cfg,
                                             "taler",
                                             "currency",
                                             &exchange_currency_string))
  {
    GNUNET_log_config_missing (GNUNET_ERROR_TYPE_ERROR,
                               "taler",
                               "currency");
    return GNUNET_SYSERR;
  }
  if (strlen (exchange_currency_string) >= TALER_CURRENCY_LEN)
  {
    fprintf (stderr,
             "Currency `%s' longer than the allowed limit of %u characters.",
             exchange_currency_string,
             (unsigned int) TALER_CURRENCY_LEN);
    return GNUNET_SYSERR;
  }

  if ( (GNUNET_OK !=
        TALER_config_get_amount (cfg,
                                 "taler",
                                 "CURRENCY_ROUND_UNIT",
                                 &currency_round_unit)) ||
       (0 != strcasecmp (exchange_currency_string,
                         currency_round_unit.currency)) ||
       ( (0 != currency_round_unit.fraction) &&
         (0 != currency_round_unit.value) ) )
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Invalid value specified in section `TALER' under `CURRENCY_ROUND_UNIT'\n");
    return GNUNET_SYSERR;
  }

  if (NULL ==
      (db_plugin = TALER_EXCHANGEDB_plugin_load (cfg)))
  {
    fprintf (stderr,
             "Failed to initialize DB subsystem\n");
    return GNUNET_SYSERR;
  }
  if (GNUNET_OK !=
      db_plugin->create_tables (db_plugin->cls))
  {
    fprintf (stderr,
             "Failed to initialize DB tables\n");
    TALER_EXCHANGEDB_plugin_unload (db_plugin);
    db_plugin = NULL;
    return GNUNET_SYSERR;
  }
  TALER_EXCHANGEDB_find_accounts (cfg,
                                  &add_account_cb,
                                  NULL);
  if (NULL == wa_head)
  {
    fprintf (stderr,
             "No wire accounts configured for debit!\n");
    TALER_EXCHANGEDB_plugin_unload (db_plugin);
    db_plugin = NULL;
    return GNUNET_SYSERR;
  }
  return GNUNET_OK;
}


/**
 * Callback invoked with information about refunds applicable
 * to a particular coin.  Subtract refunded amount(s) from
 * the aggregation unit's total amount.
 *
 * @param cls closure with a `struct AggregationUnit *`
 * @param amount_with_fee what was the refunded amount with the fee
 * @return #GNUNET_OK to continue to iterate, #GNUNET_SYSERR to stop
 */
static int
refund_by_coin_cb (void *cls,
                   const struct TALER_Amount *amount_with_fee)
{
  struct AggregationUnit *aux = cls;

  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Aggregator subtracts applicable refund of amount %s\n",
              TALER_amount2s (amount_with_fee));
  aux->have_refund = GNUNET_YES;
  if (GNUNET_OK !=
      TALER_amount_subtract (&aux->total_amount,
                             &aux->total_amount,
                             amount_with_fee))
  {
    GNUNET_break (0);
    return GNUNET_SYSERR;
  }
  return GNUNET_OK;
}


/**
 * Function called with details about deposits that have been made,
 * with the goal of executing the corresponding wire transaction.
 *
 * @param cls NULL
 * @param row_id identifies database entry
 * @param merchant_pub public key of the merchant
 * @param coin_pub public key of the coin
 * @param amount_with_fee amount that was deposited including fee
 * @param deposit_fee amount the exchange gets to keep as transaction fees
 * @param h_contract_terms hash of the proposal data known to merchant and customer
 * @param wire_deadline by which the merchant adviced that he would like the
 *        wire transfer to be executed
 * @param wire wire details for the merchant
 * @return transaction status code,  #GNUNET_DB_STATUS_SUCCESS_ONE_RESULT to continue to iterate
 */
static enum GNUNET_DB_QueryStatus
deposit_cb (void *cls,
            uint64_t row_id,
            const struct TALER_MerchantPublicKeyP *merchant_pub,
            const struct TALER_CoinSpendPublicKeyP *coin_pub,
            const struct TALER_Amount *amount_with_fee,
            const struct TALER_Amount *deposit_fee,
            const struct GNUNET_HashCode *h_contract_terms,
            struct GNUNET_TIME_Absolute wire_deadline,
            const json_t *wire)
{
  enum GNUNET_DB_QueryStatus qs;

  (void) cls;
  /* NOTE: potential optimization: use custom SQL API to not
     fetch this one: */
  (void) wire_deadline; /* already checked by SQL query */
  au->merchant_pub = *merchant_pub;
  GNUNET_log (GNUNET_ERROR_TYPE_INFO,
              "Aggregator processing payment %s with amount %s\n",
              TALER_B2S (coin_pub),
              TALER_amount2s (amount_with_fee));
  au->row_id = row_id;
  au->total_amount = *amount_with_fee;
  au->have_refund = GNUNET_NO;
  qs = db_plugin->select_refunds_by_coin (db_plugin->cls,
                                          au->session,
                                          coin_pub,
                                          &au->merchant_pub,
                                          h_contract_terms,
                                          &refund_by_coin_cb,
                                          au);
  if (0 > qs)
  {
    GNUNET_break (GNUNET_DB_STATUS_SOFT_ERROR == qs);
    return qs;
  }
  if (GNUNET_NO == au->have_refund)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_INFO,
                "Non-refunded transaction, subtracting deposit fee %s\n",
                TALER_amount2s (deposit_fee));
    if (GNUNET_SYSERR ==
        TALER_amount_subtract (&au->total_amount,
                               amount_with_fee,
                               deposit_fee))
    {
      GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                  "Fatally malformed record at row %llu over %s\n",
                  (unsigned long long) row_id,
                  TALER_amount2s (amount_with_fee));
      return GNUNET_DB_STATUS_HARD_ERROR;
    }
  }

  GNUNET_assert (NULL == au->wire);
  if (NULL == (au->wire = json_incref ((json_t *) wire)))
  {
    GNUNET_break (0);
    return GNUNET_DB_STATUS_HARD_ERROR;
  }
  if (GNUNET_OK !=
      TALER_JSON_merchant_wire_signature_hash (wire,
                                               &au->h_wire))
  {
    GNUNET_break (0);
    json_decref (au->wire);
    au->wire = NULL;
    return GNUNET_DB_STATUS_HARD_ERROR;
  }
  GNUNET_CRYPTO_random_block (GNUNET_CRYPTO_QUALITY_NONCE,
                              &au->wtid,
                              sizeof (au->wtid));
  GNUNET_log (GNUNET_ERROR_TYPE_INFO,
              "Starting aggregation under H(WTID)=%s, starting amount %s at %llu\n",
              TALER_B2S (&au->wtid),
              TALER_amount2s (amount_with_fee),
              (unsigned long long) row_id);
  {
    char *url;

    url = TALER_JSON_wire_to_payto (au->wire);
    au->wa = find_account_by_payto_uri (url);
    GNUNET_free (url);
  }
  if (NULL == au->wa)
  {
    GNUNET_break (0);
    return GNUNET_DB_STATUS_HARD_ERROR;
  }

  /* make sure we have current fees */
  au->execution_time = GNUNET_TIME_absolute_get ();
  (void) GNUNET_TIME_round_abs (&au->execution_time);
  {
    struct TALER_EXCHANGEDB_AggregateFees *af;

    af = update_fees (au->wa,
                      au->execution_time,
                      au->session);
    if (NULL == af)
    {
      GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                  "Could not get or persist wire fees. Aborting run.\n");
      return GNUNET_DB_STATUS_HARD_ERROR;
    }
    au->wire_fee = af->wire_fee;
  }

  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Aggregator starts aggregation for deposit %llu to %s with wire fee %s\n",
              (unsigned long long) row_id,
              TALER_B2S (&au->wtid),
              TALER_amount2s (&au->wire_fee));
  qs = db_plugin->insert_aggregation_tracking (db_plugin->cls,
                                               au->session,
                                               &au->wtid,
                                               row_id);
  if (qs <= 0)
  {
    GNUNET_break (GNUNET_DB_STATUS_SOFT_ERROR == qs);
    return qs;
  }
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Aggregator marks deposit %llu as done\n",
              (unsigned long long) row_id);
  qs = db_plugin->mark_deposit_done (db_plugin->cls,
                                     au->session,
                                     row_id);
  if (GNUNET_DB_STATUS_SUCCESS_ONE_RESULT != qs)
  {
    GNUNET_break (GNUNET_DB_STATUS_SOFT_ERROR == qs);
    return qs;
  }
  return qs;
}


/**
 * Function called with details about another deposit we
 * can aggregate into an existing aggregation unit.
 *
 * @param cls NULL
 * @param row_id identifies database entry
 * @param merchant_pub public key of the merchant
 * @param coin_pub public key of the coin
 * @param amount_with_fee amount that was deposited including fee
 * @param deposit_fee amount the exchange gets to keep as transaction fees
 * @param h_contract_terms hash of the proposal data known to merchant and customer
 * @param wire_deadline by which the merchant adviced that he would like the
 *        wire transfer to be executed
 * @param wire wire details for the merchant
 * @return transaction status code
 */
static enum GNUNET_DB_QueryStatus
aggregate_cb (void *cls,
              uint64_t row_id,
              const struct TALER_MerchantPublicKeyP *merchant_pub,
              const struct TALER_CoinSpendPublicKeyP *coin_pub,
              const struct TALER_Amount *amount_with_fee,
              const struct TALER_Amount *deposit_fee,
              const struct GNUNET_HashCode *h_contract_terms,
              struct GNUNET_TIME_Absolute wire_deadline,
              const json_t *wire)
{
  struct TALER_Amount delta;
  enum GNUNET_DB_QueryStatus qs;

  (void) cls;
  /* NOTE: potential optimization: use custom SQL API to not
     fetch these: */
  (void) wire_deadline; /* checked by SQL */
  (void) wire; /* must match */
  GNUNET_break (0 == GNUNET_memcmp (&au->merchant_pub,
                                    merchant_pub));
  /* compute contribution of this coin after fees */
  /* add to total */
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Adding transaction amount %s from row %llu to aggregation\n",
              TALER_amount2s (amount_with_fee),
              (unsigned long long) row_id);
  if (GNUNET_OK !=
      TALER_amount_add (&au->total_amount,
                        &au->total_amount,
                        amount_with_fee))
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Overflow or currency incompatibility during aggregation at %llu\n",
                (unsigned long long) row_id);
    /* Skip this one, but keep going! */
    return GNUNET_DB_STATUS_SUCCESS_ONE_RESULT;
  }
  au->have_refund = GNUNET_NO;
  qs = db_plugin->select_refunds_by_coin (db_plugin->cls,
                                          au->session,
                                          coin_pub,
                                          &au->merchant_pub,
                                          h_contract_terms,
                                          &refund_by_coin_cb,
                                          au);
  if (0 > qs)
  {
    GNUNET_break (GNUNET_DB_STATUS_SOFT_ERROR == qs);
    return qs;
  }
  if (GNUNET_NO == au->have_refund)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                "Subtracting deposit fee %s for non-refunded coin\n",
                TALER_amount2s (deposit_fee));
    if (GNUNET_SYSERR ==
        TALER_amount_subtract (&delta,
                               &au->total_amount,
                               deposit_fee))
    {
      GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                  "Fatally malformed record at %llu over amount %s\n",
                  (unsigned long long) row_id,
                  TALER_amount2s (&au->total_amount));
      return GNUNET_DB_STATUS_HARD_ERROR;
    }
    au->total_amount = delta;
  }

  if (au->rows_offset >= aggregation_limit)
  {
    /* Bug: we asked for at most #aggregation_limit results! */
    GNUNET_break (0);
    /* Skip this one, but keep going. */
    return GNUNET_DB_STATUS_SUCCESS_ONE_RESULT;
  }
  if (NULL == au->additional_rows)
    au->additional_rows = GNUNET_new_array (aggregation_limit,
                                            unsigned long long);
  /* "append" to our list of rows */
  au->additional_rows[au->rows_offset++] = row_id;
  /* insert into aggregation tracking table */
  qs = db_plugin->insert_aggregation_tracking (db_plugin->cls,
                                               au->session,
                                               &au->wtid,
                                               row_id);
  if (GNUNET_DB_STATUS_SUCCESS_ONE_RESULT != qs)
  {
    GNUNET_break (GNUNET_DB_STATUS_SOFT_ERROR == qs);
    return qs;
  }
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Aggregator marks aggregated deposit %llu as DONE\n",
              (unsigned long long) row_id);
  qs = db_plugin->mark_deposit_done (db_plugin->cls,
                                     au->session,
                                     row_id);
  if (GNUNET_DB_STATUS_SUCCESS_ONE_RESULT != qs)
  {
    GNUNET_break (GNUNET_DB_STATUS_SOFT_ERROR == qs);
    return qs;
  }
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Added row %llu with %s to aggregation\n",
              (unsigned long long) row_id,
              TALER_amount2s (&delta));
  return GNUNET_DB_STATUS_SUCCESS_ONE_RESULT;
}


/**
 * Perform a database commit. If it fails, print a warning.
 *
 * @param session session to perform the commit for.
 * @return status of commit
 */
static enum GNUNET_DB_QueryStatus
commit_or_warn (struct TALER_EXCHANGEDB_Session *session)
{
  enum GNUNET_DB_QueryStatus qs;

  qs = db_plugin->commit (db_plugin->cls,
                          session);
  if (GNUNET_DB_STATUS_SUCCESS_NO_RESULTS == qs)
    return qs;
  GNUNET_log ((GNUNET_DB_STATUS_SOFT_ERROR == qs)
              ? GNUNET_ERROR_TYPE_INFO
              : GNUNET_ERROR_TYPE_ERROR,
              "Failed to commit database transaction!\n");
  return qs;
}


/**
 * Closure for #expired_reserve_cb().
 */
struct ExpiredReserveContext
{

  /**
   * Database session we are using.
   */
  struct TALER_EXCHANGEDB_Session *session;

  /**
   * Set to #GNUNET_YES if the transaction continues
   * asynchronously.
   */
  int async_cont;
};


/**
 * Function called with details about expired reserves.
 * We trigger the reserve closure by inserting the respective
 * closing record and prewire instructions into the respective
 * tables.
 *
 * @param cls a `struct ExpiredReserveContext *`
 * @param reserve_pub public key of the reserve
 * @param left amount left in the reserve
 * @param account_payto_uri information about the bank account that initially
 *        caused the reserve to be created
 * @param expiration_date when did the reserve expire
 * @return transaction status code
 */
static enum GNUNET_DB_QueryStatus
expired_reserve_cb (void *cls,
                    const struct TALER_ReservePublicKeyP *reserve_pub,
                    const struct TALER_Amount *left,
                    const char *account_payto_uri,
                    struct GNUNET_TIME_Absolute expiration_date)
{
  struct ExpiredReserveContext *erc = cls;
  struct TALER_EXCHANGEDB_Session *session = erc->session;
  struct GNUNET_TIME_Absolute now;
  struct TALER_WireTransferIdentifierRawP wtid;
  struct TALER_Amount amount_without_fee;
  const struct TALER_Amount *closing_fee;
  int ret;
  enum GNUNET_DB_QueryStatus qs;
  struct WireAccount *wa;
  void *buf;
  size_t buf_size;

  /* NOTE: potential optimization: use custom SQL API to not
     fetch this: */
  GNUNET_assert (NULL == ctc);
  GNUNET_log (GNUNET_ERROR_TYPE_INFO,
              "Processing reserve closure at %s\n",
              GNUNET_STRINGS_absolute_time_to_string (expiration_date));
  now = GNUNET_TIME_absolute_get ();
  (void) GNUNET_TIME_round_abs (&now);

  /* lookup account we should use */
  wa = find_account_by_payto_uri (account_payto_uri);
  if (NULL == wa)
  {
    GNUNET_break (0);
    global_ret = GNUNET_SYSERR;
    GNUNET_SCHEDULER_shutdown ();
    return GNUNET_DB_STATUS_HARD_ERROR;
  }

  /* lookup `closing_fee` from time of actual reserve expiration
     (we may be lagging behind!) */
  {
    struct TALER_EXCHANGEDB_AggregateFees *af;

    af = update_fees (wa,
                      expiration_date,
                      session);
    if (NULL == af)
    {
      global_ret = GNUNET_SYSERR;
      GNUNET_SCHEDULER_shutdown ();
      return GNUNET_DB_STATUS_HARD_ERROR;
    }
    closing_fee = &af->closing_fee;
  }

  /* calculate transfer amount */
  ret = TALER_amount_subtract (&amount_without_fee,
                               left,
                               closing_fee);
  if ( (GNUNET_SYSERR == ret) ||
       (GNUNET_NO == ret) )
  {
    /* Closing fee higher than remaining balance, close
       without wire transfer. */
    closing_fee = left;
    GNUNET_assert (GNUNET_OK ==
                   TALER_amount_get_zero (left->currency,
                                          &amount_without_fee));
  }
  /* round down to enable transfer */
  if (GNUNET_SYSERR ==
      TALER_amount_round_down (&amount_without_fee,
                               &currency_round_unit))
  {
    GNUNET_break (0);
    global_ret = GNUNET_SYSERR;
    GNUNET_SCHEDULER_shutdown ();
    return GNUNET_DB_STATUS_HARD_ERROR;
  }
  if ( (0 == amount_without_fee.value) &&
       (0 == amount_without_fee.fraction) )
    ret = GNUNET_NO;

  /* NOTE: sizeof (*reserve_pub) == sizeof (wtid) right now, but to
     be future-compatible, we use the memset + min construction */
  memset (&wtid,
          0,
          sizeof (wtid));
  memcpy (&wtid,
          reserve_pub,
          GNUNET_MIN (sizeof (wtid),
                      sizeof (*reserve_pub)));
  if (GNUNET_SYSERR != ret)
    qs = db_plugin->insert_reserve_closed (db_plugin->cls,
                                           session,
                                           reserve_pub,
                                           now,
                                           account_payto_uri,
                                           &wtid,
                                           left,
                                           closing_fee);
  else
    qs = GNUNET_DB_STATUS_HARD_ERROR;
  GNUNET_log (GNUNET_ERROR_TYPE_INFO,
              "Closing reserve %s over %s (%d, %d)\n",
              TALER_B2S (reserve_pub),
              TALER_amount2s (left),
              ret,
              qs);
  /* Check for hard failure */
  if ( (GNUNET_SYSERR == ret) ||
       (GNUNET_DB_STATUS_HARD_ERROR == qs) )
  {
    GNUNET_break (0);
    global_ret = GNUNET_SYSERR;
    GNUNET_SCHEDULER_shutdown ();
    return GNUNET_DB_STATUS_HARD_ERROR;
  }
  if ( (GNUNET_OK != ret) ||
       (GNUNET_DB_STATUS_SUCCESS_ONE_RESULT != qs) )
  {
    /* Reserve balance was almost zero OR soft error */
    GNUNET_log (GNUNET_ERROR_TYPE_INFO,
                "Reserve was virtually empty, moving on\n");
    (void) commit_or_warn (session);
    erc->async_cont = GNUNET_YES;
    GNUNET_assert (NULL == task);
    task = GNUNET_SCHEDULER_add_now (&run_transfers,
                                     NULL);
    return qs;
  }

  /* success, perform wire transfer */
  ctc = GNUNET_new (struct CloseTransferContext);
  ctc->wa = wa;
  ctc->session = session;
  ctc->method = TALER_payto_get_method (account_payto_uri);
  TALER_BANK_prepare_transfer (account_payto_uri,
                               &amount_without_fee,
                               exchange_base_url,
                               &wtid,
                               &buf,
                               &buf_size);
  /* Commit our intention to execute the wire transfer! */
  qs = db_plugin->wire_prepare_data_insert (db_plugin->cls,
                                            ctc->session,
                                            ctc->method,
                                            buf,
                                            buf_size);
  GNUNET_free (buf);
  if (GNUNET_DB_STATUS_HARD_ERROR == qs)
  {
    GNUNET_break (0);
    GNUNET_free (ctc->method);
    GNUNET_free (ctc);
    ctc = NULL;
    return GNUNET_DB_STATUS_HARD_ERROR;
  }
  if (GNUNET_DB_STATUS_SOFT_ERROR == qs)
  {
    /* start again */
    GNUNET_free (ctc->method);
    GNUNET_free (ctc);
    ctc = NULL;
    return GNUNET_DB_STATUS_SUCCESS_NO_RESULTS;
  }
  erc->async_cont = GNUNET_YES;
  GNUNET_assert (NULL == task);
  task = GNUNET_SCHEDULER_add_now (&run_transfers,
                                   NULL);
  GNUNET_free (ctc->method);
  GNUNET_free (ctc);
  ctc = NULL;

  return GNUNET_DB_STATUS_SUCCESS_ONE_RESULT;
}


/**
 * Main work function that finds and triggers transfers for reserves
 * closures.
 *
 * @param cls closure
 */
static void
run_reserve_closures (void *cls)
{
  struct TALER_EXCHANGEDB_Session *session;
  enum GNUNET_DB_QueryStatus qs;
  const struct GNUNET_SCHEDULER_TaskContext *tc;
  struct ExpiredReserveContext erc;
  struct GNUNET_TIME_Absolute now;

  (void) cls;
  task = NULL;
  reserves_idle = GNUNET_NO;
  tc = GNUNET_SCHEDULER_get_task_context ();
  if (0 != (tc->reason & GNUNET_SCHEDULER_REASON_SHUTDOWN))
    return;
  if (NULL == (session = db_plugin->get_session (db_plugin->cls)))
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Failed to obtain database session!\n");
    global_ret = GNUNET_SYSERR;
    GNUNET_SCHEDULER_shutdown ();
    return;
  }

  if (GNUNET_OK !=
      db_plugin->start (db_plugin->cls,
                        session,
                        "aggregator reserve closures"))
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Failed to start database transaction!\n");
    global_ret = GNUNET_SYSERR;
    GNUNET_SCHEDULER_shutdown ();
    return;
  }
  erc.session = session;
  erc.async_cont = GNUNET_NO;
  now = GNUNET_TIME_absolute_get ();
  (void) GNUNET_TIME_round_abs (&now);
  GNUNET_log (GNUNET_ERROR_TYPE_INFO,
              "Checking for reserves to close by date %s\n",
              GNUNET_STRINGS_absolute_time_to_string (now));
  qs = db_plugin->get_expired_reserves (db_plugin->cls,
                                        session,
                                        now,
                                        &expired_reserve_cb,
                                        &erc);
  GNUNET_assert (1 >= qs);
  switch (qs)
  {
  case GNUNET_DB_STATUS_HARD_ERROR:
    GNUNET_break (0);
    db_plugin->rollback (db_plugin->cls,
                         session);
    global_ret = GNUNET_SYSERR;
    GNUNET_SCHEDULER_shutdown ();
    return;
  case GNUNET_DB_STATUS_SOFT_ERROR:
    db_plugin->rollback (db_plugin->cls,
                         session);
    GNUNET_assert (NULL == task);
    task = GNUNET_SCHEDULER_add_now (&run_reserve_closures,
                                     NULL);
    return;
  case GNUNET_DB_STATUS_SUCCESS_NO_RESULTS:
    GNUNET_log (GNUNET_ERROR_TYPE_INFO,
                "No more idle reserves, going back to aggregation\n");
    reserves_idle = GNUNET_YES;
    db_plugin->rollback (db_plugin->cls,
                         session);
    GNUNET_assert (NULL == task);
    task = GNUNET_SCHEDULER_add_now (&run_aggregation,
                                     NULL);
    return;
  case GNUNET_DB_STATUS_SUCCESS_ONE_RESULT:
    (void) commit_or_warn (session);
    if (GNUNET_YES == erc.async_cont)
      break;
    GNUNET_assert (NULL == task);
    task = GNUNET_SCHEDULER_add_now (&run_reserve_closures,
                                     NULL);
    return;
  }
}


/**
 * Main work function that queries the DB and aggregates transactions
 * into larger wire transfers.
 *
 * @param cls NULL
 */
static void
run_aggregation (void *cls)
{
  static unsigned int swap;
  struct TALER_EXCHANGEDB_Session *session;
  enum GNUNET_DB_QueryStatus qs;
  const struct GNUNET_SCHEDULER_TaskContext *tc;
  void *buf;
  size_t buf_size;

  (void) cls;
  task = NULL;
  tc = GNUNET_SCHEDULER_get_task_context ();
  if (0 != (tc->reason & GNUNET_SCHEDULER_REASON_SHUTDOWN))
    return;
  if (0 == (++swap % 2))
  {
    GNUNET_assert (NULL == task);
    task = GNUNET_SCHEDULER_add_now (&run_reserve_closures,
                                     NULL);
    return;
  }
  GNUNET_log (GNUNET_ERROR_TYPE_INFO,
              "Checking for ready deposits to aggregate\n");
  if (NULL == (session = db_plugin->get_session (db_plugin->cls)))
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Failed to obtain database session!\n");
    global_ret = GNUNET_SYSERR;
    GNUNET_SCHEDULER_shutdown ();
    return;
  }
  if (GNUNET_OK !=
      db_plugin->start_deferred_wire_out (db_plugin->cls,
                                          session))
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Failed to start database transaction!\n");
    global_ret = GNUNET_SYSERR;
    GNUNET_SCHEDULER_shutdown ();
    return;
  }
  au = GNUNET_new (struct AggregationUnit);
  au->session = session;
  qs = db_plugin->get_ready_deposit (db_plugin->cls,
                                     session,
                                     &deposit_cb,
                                     au);
  if (0 >= qs)
  {
    cleanup_au ();
    db_plugin->rollback (db_plugin->cls,
                         session);
    if (GNUNET_DB_STATUS_HARD_ERROR == qs)
    {
      GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                  "Failed to execute deposit iteration!\n");
      global_ret = GNUNET_SYSERR;
      GNUNET_SCHEDULER_shutdown ();
      return;
    }
    if (GNUNET_DB_STATUS_SOFT_ERROR == qs)
    {
      /* should re-try immediately */
      swap--; /* do not count failed attempts */
      GNUNET_assert (NULL == task);
      task = GNUNET_SCHEDULER_add_now (&run_aggregation,
                                       NULL);
      return;
    }
    GNUNET_log (GNUNET_ERROR_TYPE_INFO,
                "No more ready deposits, going to sleep\n");
    if ( (GNUNET_YES == test_mode) &&
         (swap >= 2) )
    {
      /* in test mode, shutdown if we end up being idle */
      GNUNET_SCHEDULER_shutdown ();
    }
    else
    {
      if ( (GNUNET_NO == reserves_idle) ||
           (GNUNET_YES == test_mode) )
      {
        /* Possibly more to on reserves, go for it immediately */
        GNUNET_assert (NULL == task);
        task = GNUNET_SCHEDULER_add_now (&run_reserve_closures,
                                         NULL);
      }
      else
      {
        /* nothing to do, sleep for a minute and try again */
        GNUNET_assert (NULL == task);
        task = GNUNET_SCHEDULER_add_delayed (aggregator_idle_sleep_interval,
                                             &run_aggregation,
                                             NULL);
      }
    }
    return;
  }

  /* Now try to find other deposits to aggregate */
  GNUNET_log (GNUNET_ERROR_TYPE_INFO,
              "Found ready deposit for %s, aggregating\n",
              TALER_B2S (&au->merchant_pub));
  qs = db_plugin->iterate_matching_deposits (db_plugin->cls,
                                             session,
                                             &au->h_wire,
                                             &au->merchant_pub,
                                             &aggregate_cb,
                                             au,
                                             aggregation_limit);
  if ( (GNUNET_DB_STATUS_HARD_ERROR == qs) ||
       (GNUNET_YES == au->failed) )
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Failed to execute deposit iteration!\n");
    cleanup_au ();
    db_plugin->rollback (db_plugin->cls,
                         session);
    global_ret = GNUNET_SYSERR;
    GNUNET_SCHEDULER_shutdown ();
    return;
  }
  if (GNUNET_DB_STATUS_SOFT_ERROR == qs)
  {
    /* serializiability issue, try again */
    GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                "Serialization issue, trying again later!\n");
    db_plugin->rollback (db_plugin->cls,
                         session);
    GNUNET_assert (NULL == task);
    task = GNUNET_SCHEDULER_add_now (&run_aggregation,
                                     NULL);
    return;
  }

  /* Subtract wire transfer fee and round to the unit supported by the
     wire transfer method; Check if after rounding down, we still have
     an amount to transfer, and if not mark as 'tiny'. */
  if ( (GNUNET_OK !=
        TALER_amount_subtract (&au->final_amount,
                               &au->total_amount,
                               &au->wire_fee)) ||
       (GNUNET_SYSERR ==
        TALER_amount_round_down (&au->final_amount,
                                 &currency_round_unit)) ||
       ( (0 == au->final_amount.value) &&
         (0 == au->final_amount.fraction) ) )
  {
    GNUNET_log (GNUNET_ERROR_TYPE_INFO,
                "Aggregate value too low for transfer (%d/%s)\n",
                qs,
                TALER_amount2s (&au->final_amount));
    /* Rollback ongoing transaction, as we will not use the respective
       WTID and thus need to remove the tracking data */
    db_plugin->rollback (db_plugin->cls,
                         session);

    /* There were results, just the value was too low.  Start another
       transaction to mark all* of the selected deposits as minor! */
    if (GNUNET_OK !=
        db_plugin->start (db_plugin->cls,
                          session,
                          "aggregator mark tiny transactions"))
    {
      GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                  "Failed to start database transaction!\n");
      global_ret = GNUNET_SYSERR;
      cleanup_au ();
      GNUNET_SCHEDULER_shutdown ();
      return;
    }
    /* Mark transactions by row_id as minor */
    qs = db_plugin->mark_deposit_tiny (db_plugin->cls,
                                       session,
                                       au->row_id);
    if (0 <= qs)
    {
      for (unsigned int i = 0; i<au->rows_offset; i++)
      {
        qs = db_plugin->mark_deposit_tiny (db_plugin->cls,
                                           session,
                                           au->additional_rows[i]);
        if (0 > qs)
          break;
      }
    }
    if (GNUNET_DB_STATUS_SOFT_ERROR == qs)
    {
      GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                  "Serialization issue, trying again later!\n");
      db_plugin->rollback (db_plugin->cls,
                           session);
      cleanup_au ();
      /* start again */
      GNUNET_assert (NULL == task);
      task = GNUNET_SCHEDULER_add_now (&run_aggregation,
                                       NULL);
      return;
    }
    if (GNUNET_DB_STATUS_HARD_ERROR == qs)
    {
      db_plugin->rollback (db_plugin->cls,
                           session);
      cleanup_au ();
      GNUNET_SCHEDULER_shutdown ();
      return;
    }
    /* commit */
    (void) commit_or_warn (session);
    cleanup_au ();
    /* start again */
    GNUNET_assert (NULL == task);
    task = GNUNET_SCHEDULER_add_now (&run_aggregation,
                                     NULL);
    return;
  }
  {
    char *amount_s;

    amount_s = TALER_amount_to_string (&au->final_amount);
    GNUNET_log (GNUNET_ERROR_TYPE_INFO,
                "Preparing wire transfer of %s to %s\n",
                amount_s,
                TALER_B2S (&au->merchant_pub));
    GNUNET_free (amount_s);
  }
  {
    char *url;

    url = TALER_JSON_wire_to_payto (au->wire);
    TALER_BANK_prepare_transfer (url,
                                 &au->final_amount,
                                 exchange_base_url,
                                 &au->wtid,
                                 &buf,
                                 &buf_size);
    GNUNET_free (url);
  }
  GNUNET_free_non_null (au->additional_rows);
  au->additional_rows = NULL;
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Storing %u bytes of wire prepare data\n",
              (unsigned int) buf_size);
  /* Commit our intention to execute the wire transfer! */
  qs = db_plugin->wire_prepare_data_insert (db_plugin->cls,
                                            session,
                                            au->wa->method,
                                            buf,
                                            buf_size);
  GNUNET_free (buf);
  /* Commit the WTID data to 'wire_out' to finally satisfy aggregation
     table constraints */
  if (qs >= 0)
    qs = db_plugin->store_wire_transfer_out (db_plugin->cls,
                                             session,
                                             au->execution_time,
                                             &au->wtid,
                                             au->wire,
                                             au->wa->section_name,
                                             &au->final_amount);
  cleanup_au ();
  if (GNUNET_DB_STATUS_SOFT_ERROR == qs)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                "Serialization issue for prepared wire data; trying again later!\n");
    db_plugin->rollback (db_plugin->cls,
                         session);
    /* start again */
    GNUNET_assert (NULL == task);
    task = GNUNET_SCHEDULER_add_now (&run_aggregation,
                                     NULL);
    return;
  }
  if (GNUNET_DB_STATUS_HARD_ERROR == qs)
  {
    GNUNET_break (0);
    db_plugin->rollback (db_plugin->cls,
                         session);
    /* die hard */
    global_ret = GNUNET_SYSERR;
    GNUNET_SCHEDULER_shutdown ();
    return;
  }

  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Stored wire transfer out instructions\n");

  /* Now we can finally commit the overall transaction, as we are
     again consistent if all of this passes. */
  switch (commit_or_warn (session))
  {
  case GNUNET_DB_STATUS_SOFT_ERROR:
    /* try again */
    GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                "Commit issue for prepared wire data; trying again later!\n");
    GNUNET_assert (NULL == task);
    task = GNUNET_SCHEDULER_add_now (&run_aggregation,
                                     NULL);
    return;
  case GNUNET_DB_STATUS_HARD_ERROR:
    GNUNET_break (0);
    global_ret = GNUNET_SYSERR;
    GNUNET_SCHEDULER_shutdown ();
    return;
  case GNUNET_DB_STATUS_SUCCESS_NO_RESULTS:
    GNUNET_log (GNUNET_ERROR_TYPE_INFO,
                "Preparation complete, switching to transfer mode\n");
    /* run alternative task: actually do wire transfer! */
    GNUNET_assert (NULL == task);
    task = GNUNET_SCHEDULER_add_now (&run_transfers,
                                     NULL);
    return;
  default:
    GNUNET_break (0);
    global_ret = GNUNET_SYSERR;
    GNUNET_SCHEDULER_shutdown ();
    return;
  }
}


/**
 * Function called with the result from the execute step.
 *
 * @param cls NULL
 * @param http_status_code #MHD_HTTP_OK on success
 * @param ec taler error code
 * @param row_id unique ID of the wire transfer in the bank's records
 * @param wire_timestamp when did the transfer happen
 */
static void
wire_confirm_cb (void *cls,
                 unsigned int http_status_code,
                 enum TALER_ErrorCode ec,
                 uint64_t row_id,
                 struct GNUNET_TIME_Absolute wire_timestamp)
{
  struct TALER_EXCHANGEDB_Session *session = wpd->session;
  enum GNUNET_DB_QueryStatus qs;

  (void) cls;
  (void) row_id;
  (void) wire_timestamp;
  wpd->eh = NULL;
  if (MHD_HTTP_OK != http_status_code)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Wire transaction failed: %u/%d\n",
                http_status_code,
                ec);
    db_plugin->rollback (db_plugin->cls,
                         session);
    global_ret = GNUNET_SYSERR;
    GNUNET_SCHEDULER_shutdown ();
    GNUNET_free (wpd);
    wpd = NULL;
    return;
  }
  qs = db_plugin->wire_prepare_data_mark_finished (db_plugin->cls,
                                                   session,
                                                   wpd->row_id);
  if (0 >= qs)
  {
    GNUNET_break (GNUNET_DB_STATUS_SOFT_ERROR == qs);
    db_plugin->rollback (db_plugin->cls,
                         session);
    if (GNUNET_DB_STATUS_SOFT_ERROR == qs)
    {
      /* try again */
      GNUNET_assert (NULL == task);
      task = GNUNET_SCHEDULER_add_now (&run_aggregation,
                                       NULL);
    }
    else
    {
      global_ret = GNUNET_SYSERR;
      GNUNET_SCHEDULER_shutdown ();
    }
    GNUNET_free (wpd);
    wpd = NULL;
    return;
  }
  GNUNET_free (wpd);
  wpd = NULL;
  switch (commit_or_warn (session))
  {
  case GNUNET_DB_STATUS_SOFT_ERROR:
    /* try again */
    GNUNET_assert (NULL == task);
    task = GNUNET_SCHEDULER_add_now (&run_aggregation,
                                     NULL);
    return;
  case GNUNET_DB_STATUS_HARD_ERROR:
    GNUNET_break (0);
    global_ret = GNUNET_SYSERR;
    GNUNET_SCHEDULER_shutdown ();
    return;
  case GNUNET_DB_STATUS_SUCCESS_NO_RESULTS:
    GNUNET_log (GNUNET_ERROR_TYPE_INFO,
                "Wire transfer complete\n");
    /* continue with #run_transfers(), just to guard
       against the unlikely case that there are more. */
    GNUNET_assert (NULL == task);
    task = GNUNET_SCHEDULER_add_now (&run_transfers,
                                     NULL);
    return;
  default:
    GNUNET_break (0);
    global_ret = GNUNET_SYSERR;
    GNUNET_SCHEDULER_shutdown ();
    return;
  }
}


/**
 * Callback with data about a prepared transaction.
 *
 * @param cls NULL
 * @param rowid row identifier used to mark prepared transaction as done
 * @param wire_method wire method the preparation was done for
 * @param buf transaction data that was persisted, NULL on error
 * @param buf_size number of bytes in @a buf, 0 on error
 */
static void
wire_prepare_cb (void *cls,
                 uint64_t rowid,
                 const char *wire_method,
                 const char *buf,
                 size_t buf_size)
{
  struct WireAccount *wa;

  (void) cls;
  wpd->row_id = rowid;
  GNUNET_log (GNUNET_ERROR_TYPE_INFO,
              "Starting wire transfer %llu\n",
              (unsigned long long) rowid);
  wpd->wa = find_account_by_method (wire_method);
  if (NULL == wpd->wa)
  {
    /* Should really never happen here, as when we get
       here the wire account should be in the cache. */
    GNUNET_break (0);
    db_plugin->rollback (db_plugin->cls,
                         wpd->session);
    global_ret = GNUNET_SYSERR;
    GNUNET_SCHEDULER_shutdown ();
    GNUNET_free (wpd);
    wpd = NULL;
    return;
  }
  wa = wpd->wa;
  wpd->eh = TALER_BANK_transfer (ctx,
                                 &wa->auth,
                                 buf,
                                 buf_size,
                                 &wire_confirm_cb,
                                 NULL);
  if (NULL == wpd->eh)
  {
    GNUNET_break (0); /* Irrecoverable */
    db_plugin->rollback (db_plugin->cls,
                         wpd->session);
    global_ret = GNUNET_SYSERR;
    GNUNET_SCHEDULER_shutdown ();
    GNUNET_free (wpd);
    wpd = NULL;
    return;
  }
}


/**
 * Execute the wire transfers that we have committed to
 * do.
 *
 * @param cls NULL
 */
static void
run_transfers (void *cls)
{
  enum GNUNET_DB_QueryStatus qs;
  struct TALER_EXCHANGEDB_Session *session;
  const struct GNUNET_SCHEDULER_TaskContext *tc;

  (void) cls;
  task = NULL;
  GNUNET_log (GNUNET_ERROR_TYPE_INFO,
              "Checking for pending wire transfers\n");
  tc = GNUNET_SCHEDULER_get_task_context ();
  if (0 != (tc->reason & GNUNET_SCHEDULER_REASON_SHUTDOWN))
    return;
  if (NULL == (session = db_plugin->get_session (db_plugin->cls)))
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Failed to obtain database session!\n");
    global_ret = GNUNET_SYSERR;
    GNUNET_SCHEDULER_shutdown ();
    return;
  }
  if (GNUNET_OK !=
      db_plugin->start (db_plugin->cls,
                        session,
                        "aggregator run transfer"))
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Failed to start database transaction!\n");
    global_ret = GNUNET_SYSERR;
    GNUNET_SCHEDULER_shutdown ();
    return;
  }
  wpd = GNUNET_new (struct WirePrepareData);
  wpd->session = session;
  qs = db_plugin->wire_prepare_data_get (db_plugin->cls,
                                         session,
                                         &wire_prepare_cb,
                                         NULL);
  if (GNUNET_DB_STATUS_SUCCESS_ONE_RESULT == qs)
    return;  /* continued via continuation set in #wire_prepare_cb() */
  db_plugin->rollback (db_plugin->cls,
                       session);
  GNUNET_free (wpd);
  wpd = NULL;
  switch (qs)
  {
  case GNUNET_DB_STATUS_HARD_ERROR:
    GNUNET_break (0);
    global_ret = GNUNET_SYSERR;
    GNUNET_SCHEDULER_shutdown ();
    return;
  case GNUNET_DB_STATUS_SOFT_ERROR:
    /* try again */
    GNUNET_assert (NULL == task);
    task = GNUNET_SCHEDULER_add_now (&run_transfers,
                                     NULL);
    return;
  case GNUNET_DB_STATUS_SUCCESS_NO_RESULTS:
    /* no more prepared wire transfers, go back to aggregation! */
    GNUNET_log (GNUNET_ERROR_TYPE_INFO,
                "No more pending wire transfers, starting aggregation\n");
    GNUNET_assert (NULL == task);
    task = GNUNET_SCHEDULER_add_now (&run_aggregation,
                                     NULL);
    return;
  case GNUNET_DB_STATUS_SUCCESS_ONE_RESULT:
    /* should be impossible */
    GNUNET_assert (0);
  }
}


/**
 * First task.
 *
 * @param cls closure, NULL
 * @param args remaining command-line arguments
 * @param cfgfile name of the configuration file used (for saving, can be NULL!)
 * @param c configuration
 */
static void
run (void *cls,
     char *const *args,
     const char *cfgfile,
     const struct GNUNET_CONFIGURATION_Handle *c)
{
  (void) cls;
  (void) args;
  (void) cfgfile;

  cfg = GNUNET_CONFIGURATION_dup (c);
  if (GNUNET_OK != parse_wirewatch_config ())
  {
    GNUNET_CONFIGURATION_destroy (cfg);
    cfg = NULL;
    global_ret = 1;
    return;
  }
  ctx = GNUNET_CURL_init (&GNUNET_CURL_gnunet_scheduler_reschedule,
                          &rc);
  rc = GNUNET_CURL_gnunet_rc_create (ctx);
  if (NULL == ctx)
  {
    GNUNET_break (0);
    return;
  }

  GNUNET_assert (NULL == task);
  task = GNUNET_SCHEDULER_add_now (&run_transfers,
                                   NULL);
  GNUNET_SCHEDULER_add_shutdown (&shutdown_task,
                                 cls);
}


/**
 * The main function of the taler-exchange-aggregator.
 *
 * @param argc number of arguments from the command line
 * @param argv command line arguments
 * @return 0 ok, 1 on error
 */
int
main (int argc,
      char *const *argv)
{
  struct GNUNET_GETOPT_CommandLineOption options[] = {
    GNUNET_GETOPT_option_timetravel ('T',
                                     "timetravel"),
    GNUNET_GETOPT_option_flag ('t',
                               "test",
                               "run in test mode and exit when idle",
                               &test_mode),
    GNUNET_GETOPT_option_version (VERSION "-" VCS_VERSION),
    GNUNET_GETOPT_OPTION_END
  };

  if (GNUNET_OK != GNUNET_STRINGS_get_utf8_args (argc, argv,
                                                 &argc, &argv))
    return 2;
  if (GNUNET_OK !=
      GNUNET_PROGRAM_run (argc, argv,
                          "taler-exchange-aggregator",
                          gettext_noop (
                            "background process that aggregates and executes wire transfers to merchants"),
                          options,
                          &run, NULL))
  {
    GNUNET_free ((void *) argv);
    return 1;
  }
  GNUNET_free ((void *) argv);
  return global_ret;
}


/* end of taler-exchange-aggregator.c */
