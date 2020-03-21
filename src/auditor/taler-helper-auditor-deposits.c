/*
  This file is part of TALER
  Copyright (C) 2016-2020 Taler Systems SA

  TALER is free software; you can redistribute it and/or modify it under the
  terms of the GNU Affero Public License as published by the Free Software
  Foundation; either version 3, or (at your option) any later version.

  TALER is distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
  A PARTICULAR PURPOSE.  See the GNU Affero Public License for more details.

  You should have received a copy of the GNU Affero Public License along with
  TALER; see the file COPYING.  If not, see <http://www.gnu.org/licenses/>
*/
/**
 * @file auditor/taler-auditor-deposits.c
 * @brief audits an exchange database for deposit confirmation consistency
 * @author Christian Grothoff
 */
#include "platform.h"
#include <gnunet/gnunet_util_lib.h>
#include "taler_auditordb_plugin.h"
#include "taler_exchangedb_lib.h"
#include "taler_json_lib.h"
#include "taler_bank_service.h"
#include "taler_signatures.h"
#include "report-lib.h"


/**
 * Return value from main().
 */
static int global_ret;

/**
 * Array of TALER_ARL_reports about missing deposit confirmations.
 */
static json_t *report_deposit_confirmation_inconsistencies;

/**
 * Total number of deposit confirmations that we did not get.
 */
static json_int_t number_missed_deposit_confirmations;

/**
 * Total amount involved in deposit confirmations that we did not get.
 */
static struct TALER_Amount total_missed_deposit_confirmations;


/* *************************** Analysis of deposit-confirmations ********** */

/**
 * Closure for #test_dc.
 */
struct DepositConfirmationContext
{

  /**
   * How many deposit confirmations did we NOT find in the #TALER_ARL_edb?
   */
  unsigned long long missed_count;

  /**
   * What is the total amount missing?
   */
  struct TALER_Amount missed_amount;

  /**
   * Lowest SerialID of the first coin we missed? (This is where we
   * should resume next time).
   */
  uint64_t first_missed_coin_serial;

  /**
   * Lowest SerialID of the first coin we missed? (This is where we
   * should resume next time).
   */
  uint64_t last_seen_coin_serial;

  /**
   * Success or failure of (exchange) database operations within
   * #test_dc.
   */
  enum GNUNET_DB_QueryStatus qs;

};


/**
 * Given a deposit confirmation from #TALER_ARL_adb, check that it is also
 * in #TALER_ARL_edb.  Update the deposit confirmation context accordingly.
 *
 * @param cls our `struct DepositConfirmationContext`
 * @param serial_id row of the @a dc in the database
 * @param dc the deposit confirmation we know
 */
static void
test_dc (void *cls,
         uint64_t serial_id,
         const struct TALER_AUDITORDB_DepositConfirmation *dc)
{
  struct DepositConfirmationContext *dcc = cls;
  enum GNUNET_DB_QueryStatus qs;
  struct TALER_EXCHANGEDB_Deposit dep;

  dcc->last_seen_coin_serial = serial_id;
  memset (&dep,
          0,
          sizeof (dep));
  dep.coin.coin_pub = dc->coin_pub;
  dep.h_contract_terms = dc->h_contract_terms;
  dep.merchant_pub = dc->merchant;
  dep.h_wire = dc->h_wire;
  dep.refund_deadline = dc->refund_deadline;

  qs = TALER_ARL_edb->have_deposit (TALER_ARL_edb->cls,
                                    TALER_ARL_esession,
                                    &dep,
                                    GNUNET_NO /* do not check refund deadline */);
  if (qs > 0)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                "Found deposit %s in exchange database\n",
                GNUNET_h2s (&dc->h_contract_terms));
    return;   /* found, all good */
  }
  if (qs < 0)
  {
    GNUNET_break (0); /* DB error, complain */
    dcc->qs = qs;
    return;
  }
  /* deposit confirmation missing! TALER_ARL_report! */
  TALER_ARL_report (report_deposit_confirmation_inconsistencies,
                    json_pack ("{s:o, s:o, s:I, s:o}",
                               "timestamp",
                               TALER_ARL_json_from_time_abs (dc->timestamp),
                               "amount",
                               TALER_JSON_from_amount (&dc->amount_without_fee),
                               "rowid",
                               (json_int_t) serial_id,
                               "account",
                               GNUNET_JSON_from_data_auto (&dc->h_wire)));
  dcc->first_missed_coin_serial = GNUNET_MIN (dcc->first_missed_coin_serial,
                                              serial_id);
  dcc->missed_count++;
  GNUNET_assert (GNUNET_OK ==
                 TALER_amount_add (&dcc->missed_amount,
                                   &dcc->missed_amount,
                                   &dc->amount_without_fee));
}


/**
 * Check that the deposit-confirmations that were TALER_ARL_reported to
 * us by merchants are also in the exchange's database.
 *
 * @param cls closure
 * @return transaction status code
 */
static enum GNUNET_DB_QueryStatus
analyze_deposit_confirmations (void *cls)
{
  struct TALER_AUDITORDB_ProgressPointDepositConfirmation ppdc;
  struct DepositConfirmationContext dcc;
  enum GNUNET_DB_QueryStatus qs;
  enum GNUNET_DB_QueryStatus qsx;
  enum GNUNET_DB_QueryStatus qsp;

  (void) cls;
  GNUNET_log (GNUNET_ERROR_TYPE_INFO,
              "Analyzing deposit confirmations\n");
  ppdc.last_deposit_confirmation_serial_id = 0;
  qsp = TALER_ARL_adb->get_auditor_progress_deposit_confirmation (
    TALER_ARL_adb->cls,
    TALER_ARL_asession,
    &
    TALER_ARL_master_pub,
    &ppdc);
  if (0 > qsp)
  {
    GNUNET_break (GNUNET_DB_STATUS_SOFT_ERROR == qsp);
    return qsp;
  }
  if (GNUNET_DB_STATUS_SUCCESS_NO_RESULTS == qsp)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_MESSAGE,
                _ (
                  "First analysis using this auditor, starting audit from scratch\n"));
  }
  else
  {
    GNUNET_log (GNUNET_ERROR_TYPE_INFO,
                _ ("Resuming deposit confirmation audit at %llu\n"),
                (unsigned long long) ppdc.last_deposit_confirmation_serial_id);
  }

  /* setup 'cc' */
  GNUNET_assert (GNUNET_OK ==
                 TALER_amount_get_zero (TALER_ARL_currency,
                                        &dcc.missed_amount));
  dcc.qs = GNUNET_DB_STATUS_SUCCESS_ONE_RESULT;
  dcc.missed_count = 0LLU;
  dcc.first_missed_coin_serial = UINT64_MAX;
  qsx = TALER_ARL_adb->get_deposit_confirmations (TALER_ARL_adb->cls,
                                                  TALER_ARL_asession,
                                                  &TALER_ARL_master_pub,
                                                  ppdc.
                                                  last_deposit_confirmation_serial_id,
                                                  &test_dc,
                                                  &dcc);
  if (0 > qsx)
  {
    GNUNET_break (GNUNET_DB_STATUS_SOFT_ERROR == qsx);
    return qsx;
  }
  GNUNET_log (GNUNET_ERROR_TYPE_INFO,
              "Analyzed %d deposit confirmations (above serial ID %llu)\n",
              (int) qsx,
              (unsigned long long) ppdc.last_deposit_confirmation_serial_id);
  if (0 > dcc.qs)
  {
    GNUNET_break (GNUNET_DB_STATUS_SOFT_ERROR == dcc.qs);
    return dcc.qs;
  }
  if (UINT64_MAX == dcc.first_missed_coin_serial)
    ppdc.last_deposit_confirmation_serial_id = dcc.last_seen_coin_serial;
  else
    ppdc.last_deposit_confirmation_serial_id = dcc.first_missed_coin_serial - 1;

  /* sync 'cc' back to disk */
  if (GNUNET_DB_STATUS_SUCCESS_ONE_RESULT == qsp)
    qs = TALER_ARL_adb->update_auditor_progress_deposit_confirmation (
      TALER_ARL_adb->cls,
      TALER_ARL_asession,
      &
      TALER_ARL_master_pub,
      &ppdc);
  else
    qs = TALER_ARL_adb->insert_auditor_progress_deposit_confirmation (
      TALER_ARL_adb->cls,
      TALER_ARL_asession,
      &
      TALER_ARL_master_pub,
      &ppdc);
  if (0 >= qs)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_INFO,
                "Failed to update auditor DB, not recording progress\n");
    GNUNET_break (GNUNET_DB_STATUS_SOFT_ERROR == qs);
    return qs;
  }
  number_missed_deposit_confirmations = (json_int_t) dcc.missed_count;
  total_missed_deposit_confirmations = dcc.missed_amount;

  GNUNET_log (GNUNET_ERROR_TYPE_INFO,
              _ ("Concluded deposit confirmation audit step at %llu\n"),
              (unsigned long long) ppdc.last_deposit_confirmation_serial_id);
  return qs;
}


/**
 * Main function that will be run.
 *
 * @param cls closure
 * @param args remaining command-line arguments
 * @param TALER_ARL_cfgfile name of the configuration file used (for saving, can be NULL!)
 * @param c configuration
 */
static void
run (void *cls,
     char *const *args,
     const char *TALER_ARL_cfgfile,
     const struct GNUNET_CONFIGURATION_Handle *c)
{
  (void) cls;
  (void) args;
  (void) TALER_ARL_cfgfile;
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Launching auditor\n");
  if (GNUNET_OK !=
      TALER_ARL_init (c))
  {
    global_ret = 1;
    return;
  }
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Starting audit\n");
  GNUNET_assert (NULL !=
                 (report_deposit_confirmation_inconsistencies = json_array ()));
  if (GNUNET_OK !=
      TALER_ARL_setup_sessions_and_run (&analyze_deposit_confirmations,
                                        NULL))
  {
    global_ret = 1;
    return;
  }
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Audit complete\n");
  {
    json_t *report;

    report = json_pack ("{s:o, s:I, s:o, s:o, s:o}",
                        "deposit_confirmation_inconsistencies",
                        report_deposit_confirmation_inconsistencies,
                        "missing_deposit_confirmation_count",
                        (json_int_t) number_missed_deposit_confirmations,
                        "missing_deposit_confirmation_total",
                        TALER_JSON_from_amount (
                          &total_missed_deposit_confirmations),
                        "auditor_start_time",
                        TALER_ARL_json_from_time_abs (
                          start_time),
                        "auditor_end_time",
                        TALER_ARL_json_from_time_abs (
                          GNUNET_TIME_absolute_get ())
                        );
    GNUNET_break (NULL != report);
    TALER_ARL_done (report);
  }
}


/**
 * The main function of the database initialization tool.
 * Used to initialize the Taler Exchange's database.
 *
 * @param argc number of arguments from the command line
 * @param argv command line arguments
 * @return 0 ok, 1 on error
 */
int
main (int argc,
      char *const *argv)
{
  const struct GNUNET_GETOPT_CommandLineOption options[] = {
    GNUNET_GETOPT_option_base32_auto ('m',
                                      "exchange-key",
                                      "KEY",
                                      "public key of the exchange (Crockford base32 encoded)",
                                      &TALER_ARL_master_pub),
    GNUNET_GETOPT_option_flag ('r',
                               "TALER_ARL_restart",
                               "TALER_ARL_restart audit from the beginning (required on first run)",
                               &TALER_ARL_restart),
    GNUNET_GETOPT_option_timetravel ('T',
                                     "timetravel"),
    GNUNET_GETOPT_OPTION_END
  };

  /* force linker to link against libtalerutil; if we do
     not do this, the linker may "optimize" libtalerutil
     away and skip #TALER_OS_init(), which we do need */
  (void) TALER_project_data_default ();
  GNUNET_assert (GNUNET_OK ==
                 GNUNET_log_setup ("taler-auditor-deposits",
                                   "MESSAGE",
                                   NULL));
  if (GNUNET_OK !=
      GNUNET_PROGRAM_run (argc,
                          argv,
                          "taler-auditor-deposits",
                          "Audit Taler exchange database for deposit confirmation consistency",
                          options,
                          &run,
                          NULL))
    return 1;
  return global_ret;
}


/* end of taler-auditor-deposits.c */
