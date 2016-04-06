/*
  This file is part of TALER
  Copyright (C) 2016 GNUnet e.V.

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
 * @file taler-exchange-aggregator.c
 * @brief Process that aggregates outgoing transactions and executes them
 * @author Christian Grothoff
 *
 * TODO:
 * - simplify global_ret: make it a global!
 * - handle shutdown more nicely (call 'cancel' method on wire transfers)
 */
#include "platform.h"
#include <gnunet/gnunet_util_lib.h>
#include <jansson.h>
#include <pthread.h>
#include "taler_exchangedb_lib.h"
#include "taler_exchangedb_plugin.h"
#include "taler_json_lib.h"
#include "taler_wire_lib.h"

/**
 * Which currency is used by this exchange?
 */
static char *exchange_currency_string;

/**
 * Which wireformat should be supported by this aggregator?
 */
static char *exchange_wireformat;

/**
 * Base directory of the exchange (global)
 */
static char *exchange_directory;

/**
 * The exchange's configuration (global)
 */
static struct GNUNET_CONFIGURATION_Handle *cfg;

/**
 * Our DB plugin.
 */
static struct TALER_EXCHANGEDB_Plugin *db_plugin;

/**
 * Our wire plugin.
 */
static struct TALER_WIRE_Plugin *wire_plugin;

/**
 * Task for the main #run() function.
 */
static struct GNUNET_SCHEDULER_Task *task;

/**
 * #GNUNET_YES if we are in test mode and are using temporary tables.
 */
static int test_mode;

/**
 * Limit on the number of transactions we aggregate at once.  Note
 * that the limit must be big enough to ensure that when transactions
 * of the smallest possible unit are aggregated, they do surpass the
 * "tiny" threshold beyond which we never trigger a wire transaction!
 *
 * Note: do not change here, Postgres requires us to hard-code the
 * LIMIT in the prepared statement.
 */
static unsigned int aggregation_limit = TALER_EXCHANGEDB_MATCHING_DEPOSITS_LIMIT;


/**
 * Load configuration parameters for the exchange
 * server into the corresponding global variables.
 *
 * @param exchange_directory the exchange's directory
 * @return #GNUNET_OK on success
 */
static int
exchange_serve_process_config (const char *exchange_directory)
{
  cfg = TALER_config_load (exchange_directory);
  if (NULL == cfg)
  {
    fprintf (stderr,
             "Failed to load exchange configuration\n");
    return GNUNET_SYSERR;
  }
  if (GNUNET_OK !=
      GNUNET_CONFIGURATION_get_value_string (cfg,
                                             "exchange",
                                             "currency",
                                             &exchange_currency_string))
  {
    GNUNET_log_config_missing (GNUNET_ERROR_TYPE_ERROR,
                               "exchange",
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
  if (NULL != exchange_wireformat)
    GNUNET_CONFIGURATION_set_value_string (cfg,
                                           "exchange",
                                           "wireformat",
                                           exchange_wireformat);
  if (GNUNET_OK !=
      GNUNET_CONFIGURATION_get_value_string (cfg,
                                             "exchange",
                                             "wireformat",
                                             &exchange_wireformat))
  {
    GNUNET_log_config_missing (GNUNET_ERROR_TYPE_ERROR,
                               "exchange",
                               "wireformat");
    return GNUNET_SYSERR;
  }

  if (NULL ==
      (db_plugin = TALER_EXCHANGEDB_plugin_load (cfg)))
  {
    fprintf (stderr,
             "Failed to initialize DB subsystem\n");
    return GNUNET_SYSERR;
  }

  if (NULL ==
      (wire_plugin = TALER_WIRE_plugin_load (cfg,
                                             exchange_wireformat)))
  {
    fprintf (stderr,
             "Failed to load wire plugin for `%s'\n",
             exchange_wireformat);
    return GNUNET_SYSERR;
  }

  return GNUNET_OK;
}


/**
 * Information about one aggregation process to
 * be executed.
 */
struct AggregationUnit
{
  /**
   * Public key of the merchant.
   */
  struct TALER_MerchantPublicKeyP merchant_pub;

  /**
   * Total amount to be transferred.
   */
  struct TALER_Amount total_amount;

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
   * Database session for all of our transactions.
   */
  struct TALER_EXCHANGEDB_Session *session;

  /**
   * Wire preparation handle.
   */
  struct TALER_WIRE_PrepareHandle *ph;

  /**
   * Array of #aggregation_limit row_ids from the
   * aggregation.
   */
  unsigned long long *additional_rows;

  /**
   * Pointer to global return value. Closure for #run().
   */
  int *global_ret;

  /**
   * Offset specifying how many #additional_rows are in use.
   */
  unsigned int rows_offset;

  /**
   * Set to #GNUNET_YES if we have to abort due to failure.
   */
  int failed;

};


/**
 * Function called with details about deposits that have been made,
 * with the goal of executing the corresponding wire transaction.
 *
 * @param cls closure with the `struct AggregationUnit`
 * @param row_id identifies database entry
 * @param merchant_pub public key of the merchant
 * @param coin_pub public key of the coin
 * @param amount_with_fee amount that was deposited including fee
 * @param deposit_fee amount the exchange gets to keep as transaction fees
 * @param transaction_id unique transaction ID chosen by the merchant
 * @param h_contract hash of the contract between merchant and customer
 * @param wire_deadline by which the merchant adviced that he would like the
 *        wire transfer to be executed
 * @param wire wire details for the merchant
 * @return #GNUNET_OK to continue to iterate, #GNUNET_SYSERR to stop
 */
static int
deposit_cb (void *cls,
            unsigned long long row_id,
            const struct TALER_MerchantPublicKeyP *merchant_pub,
            const struct TALER_CoinSpendPublicKeyP *coin_pub,
            const struct TALER_Amount *amount_with_fee,
            const struct TALER_Amount *deposit_fee,
            uint64_t transaction_id,
            const struct GNUNET_HashCode *h_contract,
            struct GNUNET_TIME_Absolute wire_deadline,
            const json_t *wire)
{
  struct AggregationUnit *au = cls;

  au->merchant_pub = *merchant_pub;
  if (GNUNET_OK !=
      TALER_amount_subtract (&au->total_amount,
                             amount_with_fee,
                             deposit_fee))
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Fatally malformed record at %llu\n",
                row_id);
    return GNUNET_SYSERR;
  }
  au->row_id = row_id;
  au->wire = (json_t *) wire;
  au->execution_time = GNUNET_TIME_absolute_get ();
  TALER_JSON_hash (au->wire,
                   &au->h_wire);
  json_incref (au->wire);
  GNUNET_CRYPTO_random_block (GNUNET_CRYPTO_QUALITY_NONCE,
                              &au->wtid,
                              sizeof (au->wtid));
  if (GNUNET_OK !=
      db_plugin->insert_aggregation_tracking (db_plugin->cls,
                                              au->session,
                                              &au->wtid,
                                              merchant_pub,
                                              &au->h_wire,
                                              h_contract,
                                              transaction_id,
                                              au->execution_time,
                                              coin_pub,
                                              amount_with_fee,
                                              deposit_fee))
  {
    GNUNET_break (0);
    return GNUNET_SYSERR;
  }
  if (GNUNET_OK !=
      db_plugin->mark_deposit_done (db_plugin->cls,
                                    au->session,
                                    row_id))
  {
    GNUNET_break (0);
    au->failed = GNUNET_YES;
    return GNUNET_SYSERR;
  }
  return GNUNET_OK;
}



/**
 * Function called with details about another deposit we
 * can aggregate into an existing aggregation unit.
 *
 * @param cls closure with the `struct AggregationUnit`
 * @param row_id identifies database entry
 * @param merchant_pub public key of the merchant
 * @param coin_pub public key of the coin
 * @param amount_with_fee amount that was deposited including fee
 * @param deposit_fee amount the exchange gets to keep as transaction fees
 * @param transaction_id unique transaction ID chosen by the merchant
 * @param h_contract hash of the contract between merchant and customer
 * @param wire_deadline by which the merchant adviced that he would like the
 *        wire transfer to be executed
 * @param wire wire details for the merchant
 * @return #GNUNET_OK to continue to iterate, #GNUNET_SYSERR to stop
 */
static int
aggregate_cb (void *cls,
              unsigned long long row_id,
              const struct TALER_MerchantPublicKeyP *merchant_pub,
              const struct TALER_CoinSpendPublicKeyP *coin_pub,
              const struct TALER_Amount *amount_with_fee,
              const struct TALER_Amount *deposit_fee,
              uint64_t transaction_id,
              const struct GNUNET_HashCode *h_contract,
              struct GNUNET_TIME_Absolute wire_deadline,
              const json_t *wire)
{
  struct AggregationUnit *au = cls;
  struct TALER_Amount delta;

  GNUNET_break (0 ==
                memcmp (&au->merchant_pub,
                        merchant_pub,
                        sizeof (struct TALER_MerchantPublicKeyP)));
  /* compute contribution of this coin after fees */
  if (GNUNET_OK !=
      TALER_amount_subtract (&delta,
                             amount_with_fee,
                             deposit_fee))
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Fatally malformed record at %llu\n",
                row_id);
    return GNUNET_SYSERR;
  }
  /* add to total */
  if (GNUNET_OK !=
      TALER_amount_add (&au->total_amount,
                        &au->total_amount,
                        &delta))
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Overflow or currency incompatibility during aggregation at %llu\n",
                row_id);
    /* Skip this one, but keep going! */
    return GNUNET_OK;
  }
  if (au->rows_offset >= aggregation_limit)
  {
    /* Bug: we asked for at most #aggregation_limit results! */
    GNUNET_break (0);
    /* Skip this one, but keep going. */
    return GNUNET_OK;
  }
  if (NULL == au->additional_rows)
    au->additional_rows = GNUNET_new_array (aggregation_limit,
                                            unsigned long long);
  /* "append" to our list of rows */
  au->additional_rows[au->rows_offset++] = row_id;
  /* insert into aggregation tracking table */
  if (GNUNET_OK !=
      db_plugin->insert_aggregation_tracking (db_plugin->cls,
                                              au->session,
                                              &au->wtid,
                                              merchant_pub,
                                              &au->h_wire,
                                              h_contract,
                                              transaction_id,
                                              au->execution_time,
                                              coin_pub,
                                              amount_with_fee,
                                              deposit_fee))
  {
    GNUNET_break (0);
    return GNUNET_SYSERR;
  }
  if (GNUNET_OK !=
      db_plugin->mark_deposit_done (db_plugin->cls,
                                    au->session,
                                    row_id))
  {
    GNUNET_break (0);
    au->failed = GNUNET_YES;
    return GNUNET_SYSERR;
  }
  return GNUNET_OK;
}


/**
 * Function to be called with the prepared transfer data.
 *
 * @param cls closure with the `struct AggregationUnit`
 * @param buf transaction data to persist, NULL on error
 * @param buf_size number of bytes in @a buf, 0 on error
 */
static void
prepare_cb (void *cls,
            const char *buf,
            size_t buf_size);


/**
 * Main work function that queries the DB and aggregates transactions
 * into larger wire transfers.
 *
 * @param cls pointer to an `int` which we will return from main()
 * @param tc scheduler context
 */
static void
run_aggregation (void *cls,
                 const struct GNUNET_SCHEDULER_TaskContext *tc)
{
  int *global_ret = cls;
  struct TALER_EXCHANGEDB_Session *session;
  struct AggregationUnit *au;
  unsigned int i;
  int ret;

  if (0 != (tc->reason & GNUNET_SCHEDULER_REASON_SHUTDOWN))
    return;
  if (NULL == (session = db_plugin->get_session (db_plugin->cls,
                                                 test_mode)))
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Failed to obtain database session!\n");
    *global_ret = GNUNET_SYSERR;
    return;
  }
  if (GNUNET_OK !=
      db_plugin->start (db_plugin->cls,
                        session))
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Failed to start database transaction!\n");
    *global_ret = GNUNET_SYSERR;
    return;
  }
  au = GNUNET_new (struct AggregationUnit);
  au->session = session;
  ret = db_plugin->get_ready_deposit (db_plugin->cls,
                                      session,
                                      &deposit_cb,
                                      au);
  if (GNUNET_OK != ret)
  {
    if (NULL != au->wire)
      json_decref (au->wire);
    GNUNET_free (au);
    db_plugin->rollback (db_plugin->cls,
                         session);
    if (0 != ret)
    {
      GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                  "Failed to execute deposit iteration!\n");
      *global_ret = GNUNET_SYSERR;
      return;
    }
    if (GNUNET_YES == test_mode)
    {
      /* in test mode, shutdown if we end up being idle */
      GNUNET_SCHEDULER_shutdown ();
    }
    else
    {
      /* nothing to do, sleep for a minute and try again */
      task = GNUNET_SCHEDULER_add_delayed (GNUNET_TIME_UNIT_MINUTES,
                                           &run_aggregation,
                                           global_ret);
    }
    return;
  }
  /* Now try to find other deposits to aggregate */
  ret = db_plugin->iterate_matching_deposits (db_plugin->cls,
                                              session,
                                              &au->h_wire,
                                              &au->merchant_pub,
                                              &aggregate_cb,
                                              au,
                                              aggregation_limit);
  if ( (GNUNET_SYSERR == ret) ||
       (GNUNET_YES == au->failed) )
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Failed to execute deposit iteration!\n");
    GNUNET_free_non_null (au->additional_rows);
    if (NULL != au->wire)
      json_decref (au->wire);
    GNUNET_free (au);
    db_plugin->rollback (db_plugin->cls,
                         session);
    *global_ret = GNUNET_SYSERR;
    return;
  }
  /* Round to the unit supported by the wire transfer method */
  GNUNET_assert (GNUNET_SYSERR !=
                 wire_plugin->amount_round (wire_plugin->cls,
                                            &au->total_amount));
  /* Check if after rounding down, we still have an amount to transfer */
  if ( (0 == au->total_amount.value) &&
       (0 == au->total_amount.fraction) )
  {
    /* Rollback ongoing transaction, as we will not use the respective
       WTID and thus need to remove the tracking data */
    db_plugin->rollback (db_plugin->cls,
                         session);
    /* Start another transaction to mark all* of the selected deposits
       *as minor! */
    if (GNUNET_OK !=
        db_plugin->start (db_plugin->cls,
                          session))
    {
      GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                  "Failed to start database transaction!\n");
      *global_ret = GNUNET_SYSERR;
      GNUNET_free_non_null (au->additional_rows);
      if (NULL != au->wire)
        json_decref (au->wire);
      GNUNET_free (au);
      return;
    }
    /* Mark transactions by row_id as minor */
    ret = GNUNET_OK;
    if (GNUNET_OK !=
        db_plugin->mark_deposit_tiny (db_plugin->cls,
                                      session,
                                      au->row_id))
      ret = GNUNET_SYSERR;
    else
      for (i=0;i<au->rows_offset;i++)
        if (GNUNET_OK !=
            db_plugin->mark_deposit_tiny (db_plugin->cls,
                                          session,
                                          au->additional_rows[i]))
          ret = GNUNET_SYSERR;
    /* commit */
    if (GNUNET_OK !=
        db_plugin->commit (db_plugin->cls,
                           session))
    {
      GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                  "Failed to commit database transaction!\n");
    }
    GNUNET_free_non_null (au->additional_rows);
    if (NULL != au->wire)
      json_decref (au->wire);
    GNUNET_free (au);
    /* start again */
    task = GNUNET_SCHEDULER_add_now (&run_aggregation,
                                     global_ret);
    return;
  }
  au->global_ret = global_ret;
  au->ph = wire_plugin->prepare_wire_transfer (wire_plugin->cls,
                                               au->wire,
                                               &au->total_amount,
                                               &au->wtid,
                                               &prepare_cb,
                                               au);
  /* FIXME: currently we have no clean-up plan on
     shutdown to call prepare_wire_transfer_cancel!
     Maybe make 'au' global? */
  if (NULL == au->ph)
  {
    GNUNET_break (0); /* why? how to best recover? */
    db_plugin->rollback (db_plugin->cls,
                         session);
    GNUNET_free_non_null (au->additional_rows);
    if (NULL != au->wire)
      json_decref (au->wire);
    GNUNET_free (au);
    /* start again */
    task = GNUNET_SCHEDULER_add_now (&run_aggregation,
                                     global_ret);
    return;
  }
  /* otherwise we continue with #prepare_cb(), see below */
}


/**
 * Execute the wire transfers that we have committed to
 * do.
 *
 * @param cls pointer to an `int` which we will return from main()
 * @param tc scheduler context
 */
static void
run_transfers (void *cls,
               const struct GNUNET_SCHEDULER_TaskContext *tc);


/**
 * Function to be called with the prepared transfer data.
 *
 * @param cls closure with the `struct AggregationUnit`
 * @param buf transaction data to persist, NULL on error
 * @param buf_size number of bytes in @a buf, 0 on error
 */
static void
prepare_cb (void *cls,
            const char *buf,
            size_t buf_size)
{
  struct AggregationUnit *au = cls;
  int *global_ret = au->global_ret;
  struct TALER_EXCHANGEDB_Session *session = au->session;

  GNUNET_free_non_null (au->additional_rows);
  if (NULL != au->wire)
    json_decref (au->wire);
  GNUNET_free (au);
  if (NULL == buf)
  {
    GNUNET_break (0); /* why? how to best recover? */
    db_plugin->rollback (db_plugin->cls,
                         session);
    /* start again */
    task = GNUNET_SCHEDULER_add_now (&run_aggregation,
                                     global_ret);
    return;
  }

  /* Commit our intention to execute the wire transfer! */
  if (GNUNET_OK !=
      db_plugin->wire_prepare_data_insert (db_plugin->cls,
                                           session,
                                           exchange_wireformat,
                                           buf,
                                           buf_size))
  {
    GNUNET_break (0); /* why? how to best recover? */
    db_plugin->rollback (db_plugin->cls,
                         session);
    /* start again */
    task = GNUNET_SCHEDULER_add_now (&run_aggregation,
                                     global_ret);
    return;
  }

  /* Now we can finally commit the overall transaction, as we are
     again consistent if all of this passes. */
  if (GNUNET_OK !=
      db_plugin->commit (db_plugin->cls,
                         session))
  {
    GNUNET_log (GNUNET_ERROR_TYPE_INFO,
                "Failed to commit database transaction!\n");
    /* try again */
    task = GNUNET_SCHEDULER_add_now (&run_aggregation,
                                     global_ret);
    return;
  }

  /* run alternative task: actually do wire transfer! */
  task = GNUNET_SCHEDULER_add_now (&run_transfers,
                                   &global_ret);
}


/**
 * Data we keep to #run_transfers().
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
  struct TALER_WIRE_ExecuteHandle *eh;

  /**
   * Pointer to global return value. Closure for #run().
   */
  int *global_ret;


  /**
   * Row ID of the transfer.
   */
  unsigned long long row_id;

};


/**
 * Function called with the result from the execute step.
 *
 * @param cls closure with the `struct WirePrepareData`
 * @param success #GNUNET_OK on success, #GNUNET_SYSERR on failure
 * @param emsg NULL on success, otherwise an error message
 */
static void
wire_confirm_cb (void *cls,
                 int success,
                 const char *emsg)
{
  struct WirePrepareData *wpd = cls;
  int *global_ret = wpd->global_ret;
  struct TALER_EXCHANGEDB_Session *session = wpd->session;

  wpd->eh = NULL;
  if (GNUNET_SYSERR == success)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Wire transaction failed: %s\n",
                emsg);
    db_plugin->rollback (db_plugin->cls,
                         session);
    *global_ret = GNUNET_SYSERR;
    GNUNET_free (wpd);
    return;
  }
  if (GNUNET_OK !=
      db_plugin->wire_prepare_data_mark_finished (db_plugin->cls,
                                                  session,
                                                  wpd->row_id))
  {
    GNUNET_break (0); /* why!? */
    db_plugin->rollback (db_plugin->cls,
                         session);
    *global_ret = GNUNET_SYSERR;
    GNUNET_free (wpd);
    return;
  }
  GNUNET_free (wpd);
  if (GNUNET_OK !=
      db_plugin->commit (db_plugin->cls,
                         session))
  {
    GNUNET_log (GNUNET_ERROR_TYPE_INFO,
                "Failed to commit database transaction!\n");
    /* try again */
    task = GNUNET_SCHEDULER_add_now (&run_aggregation,
                                     global_ret);
    return;
  }
  /* continue with #run_transfers(), just to guard
     against the unlikely case that there are more. */
  task = GNUNET_SCHEDULER_add_now (&run_transfers,
                                   &global_ret);

}


/**
 * Callback with data about a prepared transaction.
 *
 * @param cls closure with the `struct WirePrepareData`
 * @param rowid row identifier used to mark prepared transaction as done
 * @param buf transaction data that was persisted, NULL on error
 * @param buf_size number of bytes in @a buf, 0 on error
 */
static void
wire_prepare_cb (void *cls,
                 unsigned long long rowid,
                 const char *buf,
                 size_t buf_size)
{
  struct WirePrepareData *wpd = cls;
  int *global_ret = wpd->global_ret;

  wpd->row_id = rowid;
  wpd->eh = wire_plugin->execute_wire_transfer (wire_plugin->cls,
                                                buf,
                                                buf_size,
                                                &wire_confirm_cb,
                                                wpd);
  /* FIXME: currently we have no clean-up plan on
     shutdown to call execute_wire_transfer_cancel!
     Maybe make 'wpd' global? */
  if (NULL == wpd->eh)
  {
    GNUNET_break (0); /* why? how to best recover? */
    db_plugin->rollback (db_plugin->cls,
                         wpd->session);
    *global_ret = GNUNET_SYSERR;
    GNUNET_free (wpd);
    return;
  }
}


/**
 * Execute the wire transfers that we have committed to
 * do.
 *
 * @param cls pointer to an `int` which we will return from main()
 * @param tc scheduler context
 */
static void
run_transfers (void *cls,
               const struct GNUNET_SCHEDULER_TaskContext *tc)
{
  int *global_ret = cls;
  int ret;
  struct WirePrepareData *wpd;
  struct TALER_EXCHANGEDB_Session *session;

  if (0 != (tc->reason & GNUNET_SCHEDULER_REASON_SHUTDOWN))
    return;
  if (NULL == (session = db_plugin->get_session (db_plugin->cls,
                                                 test_mode)))
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Failed to obtain database session!\n");
    *global_ret = GNUNET_SYSERR;
    return;
  }
  if (GNUNET_OK !=
      db_plugin->start (db_plugin->cls,
                        session))
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Failed to start database transaction!\n");
    *global_ret = GNUNET_SYSERR;
    return;
  }
  wpd = GNUNET_new (struct WirePrepareData);
  wpd->session = session;
  wpd->global_ret = global_ret;
  ret = db_plugin->wire_prepare_data_get (db_plugin->cls,
                                          session,
                                          exchange_wireformat,
                                          &wire_prepare_cb,
                                          wpd);
  if (GNUNET_SYSERR == ret)
  {
    GNUNET_break (0); /* why? how to best recover? */
    db_plugin->rollback (db_plugin->cls,
                         session);
    *global_ret = GNUNET_SYSERR;
    GNUNET_free (wpd);
    return;
  }
  if (GNUNET_NO == ret)
  {
    /* no more prepared wire transfers, go back to aggregation! */
    db_plugin->rollback (db_plugin->cls,
                         session);
    task = GNUNET_SCHEDULER_add_now (&run_aggregation,
                                     global_ret);
    GNUNET_free (wpd);
    return;
  }
  /* otherwise, continues in #wire_prepare_cb() */
}


/**
 * The main function of the taler-exchange-httpd server ("the exchange").
 *
 * @param argc number of arguments from the command line
 * @param argv command line arguments
 * @return 0 ok, 1 on error
 */
int
main (int argc,
      char *const *argv)
{
  static const struct GNUNET_GETOPT_CommandLineOption options[] = {
    {'d', "exchange-dir", "DIR",
     "exchange directory with configuration and keys for operating the exchange", 1,
     &GNUNET_GETOPT_set_filename, &exchange_directory},
    {'f', "format", "WIREFORMAT",
     "wireformat to use, overrides WIREFORMAT option in [exchange] section", 1,
     &GNUNET_GETOPT_set_filename, &exchange_wireformat},
    TALER_GETOPT_OPTION_HELP ("background process that aggregates and executes wire transfers to merchants"),
    {'t', "test", NULL,
     "run in test mode with temporary tables", 0,
     &GNUNET_GETOPT_set_one, &test_mode},
    TALER_GETOPT_OPTION_HELP ("background process that aggregates and executes wire transfers to merchants"),
    GNUNET_GETOPT_OPTION_VERSION (VERSION "-" VCS_VERSION),
    GNUNET_GETOPT_OPTION_END
  };
  int ret = GNUNET_OK;

  GNUNET_assert (GNUNET_OK ==
                 GNUNET_log_setup ("taler-exchange-aggregator",
                                   "INFO",
                                   NULL));
  if (0 >=
      GNUNET_GETOPT_run ("taler-exchange-aggregator",
                         options,
                         argc, argv))
    return 1;
  if (NULL == exchange_directory)
  {
    fprintf (stderr,
             "Exchange directory not specified\n");
    return 1;
  }
  if (GNUNET_OK !=
      exchange_serve_process_config (exchange_directory))
  {
    return 1;
  }

  GNUNET_SCHEDULER_run (&run_transfers, &ret);

  TALER_EXCHANGEDB_plugin_unload (db_plugin);
  TALER_WIRE_plugin_unload (wire_plugin);
  return (GNUNET_SYSERR == ret) ? 1 : 0;
}

/* end of taler-exchange-aggregator.c */
