/*
  This file is part of TALER
  Copyright (C) 2016, 2017 GNUnet e.V.

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
 * @file taler-exchange-wirewatch.c
 * @brief Process that watches for wire transfers to the exchange's bank account
 * @author Christian Grothoff
 */
#include "platform.h"
#include <gnunet/gnunet_util_lib.h>
#include <jansson.h>
#include <pthread.h>
#include <microhttpd.h>
#include "taler_exchangedb_lib.h"
#include "taler_exchangedb_plugin.h"
#include "taler_json_lib.h"
#include "taler_wire_lib.h"

/**
 * How long do we sleep before trying again if there
 * are no transactions returned by the wire plugin?
 */
#define DELAY GNUNET_TIME_UNIT_SECONDS


/**
 * Closure for #reject_cb().
 */
struct RejectContext
{
  /**
   * Wire transfer subject that was illformed.
   */
  char *wtid_s;

  /**
   * Database session that encountered the problem.
   */
  struct TALER_EXCHANGEDB_Session *session;
};


/**
 * Handle to the plugin.
 */
static struct TALER_WIRE_Plugin *wire_plugin;

/**
 * Which currency is used by this exchange?
 */
static char *exchange_currency_string;

/**
 * The exchange's configuration (global)
 */
static const struct GNUNET_CONFIGURATION_Handle *cfg;

/**
 * Our DB plugin.
 */
static struct TALER_EXCHANGEDB_Plugin *db_plugin;

/**
 * Value to return from main(). #GNUNET_OK on success, #GNUNET_SYSERR
 * on serious errors.
 */
static int global_ret;

/**
 * Encoded offset in the wire transfer list from where
 * to start the next query with the bank.
 */
static void *last_row_off;

/**
 * Number of bytes in #last_row_off.
 */
static size_t last_row_off_size;

/**
 * Which wire plugin are we watching?
 */
static char *type;

/**
 * Should we delay the next request to the wire plugin a bit?
 */
static int delay;

/**
 * Are we run in testing mode and should only do one pass?
 */
static int test_mode;

/**
 * Are we running from scratch and should re-process all transactions?
 */
static int reset_mode;

/**
 * Next task to run, if any.
 */
static struct GNUNET_SCHEDULER_Task *task;

/**
 * Active request for history.
 */
static struct TALER_WIRE_HistoryHandle *hh;

/**
 * Active request to reject a wire transfer.
 */
static struct TALER_WIRE_RejectHandle *rt;


/**
 * We're being aborted with CTRL-C (or SIGTERM). Shut down.
 *
 * @param cls closure
 */
static void
shutdown_task (void *cls)
{
  if (NULL != task)
  {
    GNUNET_SCHEDULER_cancel (task);
    task = NULL;
  }
  if (NULL != hh)
  {
    wire_plugin->get_history_cancel (wire_plugin->cls,
				     hh);
    hh = NULL;
  }
  if (NULL != rt)
  {
    char *wtid_s;

    wtid_s = wire_plugin->reject_transfer_cancel (wire_plugin->cls,
                                                  rt);
    rt = NULL;
    GNUNET_free (wtid_s);
  }
  TALER_EXCHANGEDB_plugin_unload (db_plugin);
  db_plugin = NULL;
  TALER_WIRE_plugin_unload (wire_plugin);
  wire_plugin = NULL;
  GNUNET_free_non_null (last_row_off);
  last_row_off = NULL;
}


/**
 * Parse configuration parameters for the exchange server into the
 * corresponding global variables.
 *
 * @return #GNUNET_OK on success
 */
static int
exchange_serve_process_config ()
{
  if (NULL == type)
  {
    fprintf (stderr,
             "Option `-t' to specify wire plugin is mandatory.\n");
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

  if (NULL ==
      (db_plugin = TALER_EXCHANGEDB_plugin_load (cfg)))
  {
    fprintf (stderr,
             "Failed to initialize DB subsystem\n");
    return GNUNET_SYSERR;
  }
  if (NULL ==
      (wire_plugin = TALER_WIRE_plugin_load (cfg,
					     type)))
  {
    fprintf (stderr,
             "Failed to load wire plugin for `%s'\n",
             type);
    TALER_EXCHANGEDB_plugin_unload (db_plugin);
    return GNUNET_SYSERR;
  }

  return GNUNET_OK;
}


/**
 * Query for incoming wire transfers.
 *
 * @param cls NULL
 */
static void
find_transfers (void *cls);


/**
 * Function called upon completion of the rejection of a wire transfer.
 *
 * @param cls closure with the `struct RejectContext`
 * @param ec error code for the operation
 */
static void
reject_cb (void *cls,
           enum TALER_ErrorCode ec)
{
  struct RejectContext *rtc = cls;
  enum GNUNET_DB_QueryStatus qs;

  rt = NULL;
  if (TALER_EC_NONE != ec)
  {
    fprintf (stderr,
             "Failed to wire back transfer `%s': %d\n",
             rtc->wtid_s,
             ec);
    GNUNET_free (rtc->wtid_s);
    db_plugin->rollback (db_plugin->cls,
			 rtc->session);
    GNUNET_free (rtc);
    GNUNET_SCHEDULER_shutdown ();
    return;
  }
  GNUNET_free (rtc->wtid_s);
  qs = db_plugin->commit (db_plugin->cls,
                          rtc->session);
  GNUNET_break (0 <= qs);
  GNUNET_free (rtc);
  task = GNUNET_SCHEDULER_add_now (&find_transfers,
                                   NULL);
}


/**
 * Callbacks of this type are used to serve the result of asking
 * the bank for the transaction history.
 *
 * @param cls closure with the `struct TALER_EXCHANGEDB_Session *`
 * @param ec taler error code
 * @param dir direction of the transfer
 * @param row_off identification of the position at which we are querying
 * @param row_off_size number of bytes in @a row_off
 * @param details details about the wire transfer
 * @return #GNUNET_OK to continue, #GNUNET_SYSERR to abort iteration
 */
static int
history_cb (void *cls,
            enum TALER_ErrorCode ec,
	    enum TALER_BANK_Direction dir,
	    const void *row_off,
	    size_t row_off_size,
	    const struct TALER_WIRE_TransferDetails *details)
{
  struct TALER_EXCHANGEDB_Session *session = cls;
  enum GNUNET_DB_QueryStatus qs;
  struct TALER_ReservePublicKeyP reserve_pub;

  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Got history callback, direction %u!\n", (unsigned int) dir);

  if (TALER_BANK_DIRECTION_NONE == dir)
  {
    hh = NULL;

    if (TALER_EC_NONE != ec)
    {
      GNUNET_log (GNUNET_ERROR_TYPE_INFO,
                  "Error fetching history: %u!\n",
                  (unsigned int) ec);
    }
    GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                "End of list. Committing progress!\n");
    qs = db_plugin->commit (db_plugin->cls,
			    session);
    if ( (GNUNET_YES == delay) &&
         (test_mode) )
    {
      GNUNET_SCHEDULER_shutdown ();
      return GNUNET_OK;
    }
    if (GNUNET_YES == delay)
      task = GNUNET_SCHEDULER_add_delayed (DELAY,
					   &find_transfers,
					   NULL);
    else
      task = GNUNET_SCHEDULER_add_now (&find_transfers,
				       NULL);
    return GNUNET_OK; /* will be ignored anyway */
  }
  if (NULL != details->wtid_s)
  {
    struct RejectContext *rtc;

    GNUNET_log (GNUNET_ERROR_TYPE_INFO,
                "Wire transfer over %s has invalid subject `%s', sending it back!\n",
                TALER_amount2s (&details->amount),
                details->wtid_s);
    GNUNET_break (0 != row_off_size);
    if (last_row_off_size != row_off_size)
    {
      GNUNET_free_non_null (last_row_off);
      last_row_off = GNUNET_malloc (row_off_size);
      last_row_off_size = row_off_size;
    }
    memcpy (last_row_off,
            row_off,
            row_off_size);
    rtc = GNUNET_new (struct RejectContext);
    rtc->session = session;
    rtc->wtid_s = GNUNET_strdup (details->wtid_s);
    rt = wire_plugin->reject_transfer (wire_plugin->cls,
                                       row_off,
                                       row_off_size,
                                       &reject_cb,
                                       rtc);
    return GNUNET_SYSERR; /* will continue later... */
  }

  GNUNET_log (GNUNET_ERROR_TYPE_INFO,
              "Adding wire transfer over %s with subject `%s'\n",
              TALER_amount2s (&details->amount),
              TALER_B2S (&details->wtid));
  /* Wire transfer identifier == reserve public key */
  GNUNET_assert (sizeof (reserve_pub) == sizeof (details->wtid));
  memcpy (&reserve_pub,
          &details->wtid,
          sizeof (reserve_pub));
  qs = db_plugin->reserves_in_insert (db_plugin->cls,
				      session,
				      &reserve_pub,
				      &details->amount,
				      details->execution_date,
				      details->account_details,
				      row_off,
				      row_off_size);
  if (GNUNET_DB_STATUS_HARD_ERROR == qs)
  {
    GNUNET_break (0);
    GNUNET_SCHEDULER_shutdown ();
    return GNUNET_SYSERR;
  }
  if (GNUNET_DB_STATUS_SOFT_ERROR == qs)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                "Got DB soft error for reserve_in_insert\n");
    /* try again */
    task = GNUNET_SCHEDULER_add_now (&find_transfers,
				     NULL);
    return GNUNET_SYSERR;
  }

  GNUNET_break (0 != row_off_size);

  if (last_row_off_size != row_off_size)
  {
    GNUNET_free_non_null (last_row_off);
    last_row_off = GNUNET_malloc (row_off_size);
    last_row_off_size = row_off_size;
  }
  memcpy (last_row_off,
	  row_off,
	  row_off_size);
  return GNUNET_OK;
}


/**
 * Query for incoming wire transfers.
 *
 * @param cls NULL
 */
static void
find_transfers (void *cls)
{
  struct TALER_EXCHANGEDB_Session *session;
  enum GNUNET_DB_QueryStatus qs;

  task = NULL;
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Checking for incoming wire transfers\n");

  if (NULL == (session = db_plugin->get_session (db_plugin->cls)))
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Failed to obtain database session!\n");
    global_ret = GNUNET_SYSERR;
    GNUNET_SCHEDULER_shutdown ();
    return;
  }
  db_plugin->preflight (db_plugin->cls,
                        session);
  if (GNUNET_OK !=
      db_plugin->start (db_plugin->cls,
                        session,
                        "wirewatch check for incoming wire transfers"))
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Failed to start database transaction!\n");
    global_ret = GNUNET_SYSERR;
    GNUNET_SCHEDULER_shutdown ();
    return;
  }
  if (! reset_mode)
  {
    qs = db_plugin->get_latest_reserve_in_reference (db_plugin->cls,
                                                     session,
                                                     &last_row_off,
                                                     &last_row_off_size);
    if (GNUNET_DB_STATUS_HARD_ERROR == qs)
    {
      GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                  "Failed to obtain starting point for montoring from database!\n");
      global_ret = GNUNET_SYSERR;
      GNUNET_SCHEDULER_shutdown ();
      return;
    }
    if (GNUNET_DB_STATUS_SOFT_ERROR == qs)
    {
      /* try again */
      task = GNUNET_SCHEDULER_add_now (&find_transfers,
                                       NULL);
      return;
    }
  }
  GNUNET_assert ((NULL == last_row_off) || ((NULL != last_row_off) && (last_row_off_size != 0)));
  delay = GNUNET_YES;
  hh = wire_plugin->get_history (wire_plugin->cls,
				 TALER_BANK_DIRECTION_CREDIT,
				 last_row_off,
				 last_row_off_size,
				 1024,
				 &history_cb,
				 session);
  if (NULL == hh)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Failed to start request for account history!\n");
    db_plugin->rollback (db_plugin->cls,
                         session);
    global_ret = GNUNET_SYSERR;
    GNUNET_SCHEDULER_shutdown ();
    return;
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
  cfg = c;
  if (GNUNET_OK !=
      exchange_serve_process_config ())
  {
    global_ret = 1;
    return;
  }

  task = GNUNET_SCHEDULER_add_now (&find_transfers,
                                   NULL);
  GNUNET_SCHEDULER_add_shutdown (&shutdown_task,
                                 cls);
}


/**
 * The main function of taler-exchange-wirewatch
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
    GNUNET_GETOPT_option_string ('t',
				 "type",
				 "PLUGINNAME",
				 "which wire plugin to use",
				 &type),
    GNUNET_GETOPT_option_flag ('T',
			       "test",
			       "run in test mode and exit when idle",
			       &test_mode),
    GNUNET_GETOPT_option_flag ('r',
			       "reset",
			       "start fresh with all transactions in the history",
			       &reset_mode),
    GNUNET_GETOPT_OPTION_END
  };

  if (GNUNET_OK !=
      GNUNET_STRINGS_get_utf8_args (argc, argv,
				    &argc, &argv))
    return 2;
  if (GNUNET_OK !=
      GNUNET_PROGRAM_run (argc, argv,
                          "taler-exchange-wirewatch",
                          gettext_noop ("background process that watches for incomming wire transfers from customers"),
                          options,
                          &run, NULL))
  {
    GNUNET_free ((void*) argv);
    return 1;
  }
  GNUNET_free ((void*) argv);
  return global_ret;
}

/* end of taler-exchange-wirewatch.c */
