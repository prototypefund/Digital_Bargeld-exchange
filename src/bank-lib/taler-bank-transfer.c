/*
  This file is part of TALER
  Copyright (C) 2017-2020 Taler Systems SA

  TALER is free software; you can redistribute it and/or modify it under the
  terms of the GNU General Public License as published by the Free Software
  Foundation; either version 3, or (at your option) any later version.

  TALER is distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
  A PARTICULAR PURPOSE.  See the GNU General Public License for more details.

  You should have received a copy of the GNU General Public License along with
  TALER; see the file COPYING.  If not, see <http://www.gnu.org/licenses/>
*/
/**
 * @file taler-bank-transfer.c
 * @brief Execute wire transfer.
 * @author Christian Grothoff
 */
#include "platform.h"
#include <gnunet/gnunet_util_lib.h>
#include <gnunet/gnunet_json_lib.h>
#include <jansson.h>
#include "taler_bank_service.h"

/**
 * Amount to transfer.
 */
static struct TALER_Amount amount;

/**
 * Credit account payto://-URI.
 */
static char *credit_account;

/**
 * Wire transfer subject.
 */
static char *subject;

/**
 * Authentication data.
 */
static struct TALER_BANK_AuthenticationData auth = {
  .method = TALER_BANK_AUTH_BASIC
};

/**
 * Return value from main().
 */
static int global_ret;

/**
 * Main execution context for the main loop.
 */
static struct GNUNET_CURL_Context *ctx;

/**
 * Handle to access the exchange.
 */
static struct TALER_BANK_AdminAddIncomingHandle *op;

/**
 * Context for running the CURL event loop.
 */
static struct GNUNET_CURL_RescheduleContext *rc;


/**
 * Function run when the test terminates (good or bad).
 * Cleans up our state.
 *
 * @param cls NULL
 */
static void
do_shutdown (void *cls)
{
  (void) cls;
  if (NULL != op)
  {
    TALER_BANK_admin_add_incoming_cancel (op);
    op = NULL;
  }
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
}


/**
 * Function called with the result of the operation.
 *
 * @param cls closure
 * @param http_status HTTP response code, #MHD_HTTP_OK (200) for successful status request
 *                    0 if the bank's reply is bogus (fails to follow the protocol)
 * @param ec detailed error code
 * @param serial_id unique ID of the wire transfer in the bank's records; UINT64_MAX on error
 * @param timestamp timestamp when the transaction got settled at the bank.
 * @param json detailed response from the HTTPD, or NULL if reply was not in JSON
 */
static void
res_cb (void *cls,
        unsigned int http_status,
        enum TALER_ErrorCode ec,
        uint64_t serial_id,
        struct GNUNET_TIME_Absolute timestamp,
        const json_t *json)
{
  (void) cls;
  (void) timestamp;
  op = NULL;
  switch (ec)
  {
  case TALER_EC_NONE:
    global_ret = 0;
    fprintf (stdout,
             "%llu\n",
             (unsigned long long) serial_id);
    break;
  default:
    fprintf (stderr,
             "Operation failed with staus code %u/%u\n",
             (unsigned int) ec,
             http_status);
    if (NULL != json)
      json_dumpf (json,
                  stderr,
                  JSON_INDENT (2));
    break;
  }
  GNUNET_SCHEDULER_shutdown ();
}


/**
 * Main function that will be run.
 *
 * @param cls closure
 * @param args remaining command-line arguments
 * @param cfgfile name of the configuration file used (for saving, can be NULL!)
 * @param cfg configuration
 */
static void
run (void *cls,
     char *const *args,
     const char *cfgfile,
     const struct GNUNET_CONFIGURATION_Handle *cfg)
{
  struct TALER_ReservePublicKeyP reserve_pub;

  (void) cls;
  (void) args;
  (void) cfgfile;
  (void) cfg;
  if (GNUNET_OK !=
      GNUNET_STRINGS_string_to_data (subject,
                                     strlen (subject),
                                     &reserve_pub,
                                     sizeof (reserve_pub)))
  {
    fprintf (stderr,
             "Error: wire transfer subject must be a reserve public key\n");
    return;
  }

  ctx = GNUNET_CURL_init (&GNUNET_CURL_gnunet_scheduler_reschedule,
                          &rc);
  GNUNET_assert (NULL != ctx);
  rc = GNUNET_CURL_gnunet_rc_create (ctx);

  op = TALER_BANK_admin_add_incoming (ctx,
                                      &auth,
                                      &reserve_pub,
                                      &amount,
                                      credit_account,
                                      &res_cb,
                                      NULL);
  GNUNET_SCHEDULER_add_shutdown (&do_shutdown,
                                 NULL);
  if (NULL == op)
    GNUNET_SCHEDULER_shutdown ();
}


/**
 * The main function of the taler-bank-transfer tool
 *
 * @param argc number of arguments from the command line
 * @param argv command line arguments
 * @return 0 ok, 1 on error
 */
int
main (int argc, char *const *argv)
{
  const struct GNUNET_GETOPT_CommandLineOption options[] = {
    GNUNET_GETOPT_option_mandatory
      (TALER_getopt_get_amount ('a',
                                "amount",
                                "VALUE",
                                "value to transfer",
                                &amount)),
    GNUNET_GETOPT_option_mandatory
      (GNUNET_GETOPT_option_string ('b',
                                    "bank",
                                    "URL",
                                    "Wire gateway URL to use to talk to the bank",
                                    &auth.wire_gateway_url)),
    GNUNET_GETOPT_option_help ("Deposit funds into a Taler reserve"),
    GNUNET_GETOPT_option_mandatory
      (GNUNET_GETOPT_option_string ('C',
                                    "credit",
                                    "ACCOUNT",
                                    "payto URI of the bank account to credit",
                                    &credit_account)),
    GNUNET_GETOPT_option_mandatory
      (GNUNET_GETOPT_option_string ('s',
                                    "subject",
                                    "STRING",
                                    "specifies the wire transfer subject (must be a reserve public key)",
                                    &subject)),
    GNUNET_GETOPT_option_mandatory
      (GNUNET_GETOPT_option_string ('u',
                                    "user",
                                    "USERNAME",
                                    "username to use for authentication",
                                    &auth.details.basic.username)),
    GNUNET_GETOPT_option_mandatory
      (GNUNET_GETOPT_option_string ('p',
                                    "pass",
                                    "PASSPHRASE",
                                    "passphrase to use for authentication",
                                    &auth.details.basic.password)),
    GNUNET_GETOPT_OPTION_END
  };

  GNUNET_assert (GNUNET_OK ==
                 GNUNET_log_setup ("taler-bank-transfer",
                                   "WARNING",
                                   NULL));
  global_ret = 1;
  if (GNUNET_OK !=
      GNUNET_PROGRAM_run (argc, argv,
                          "taler-bank-transfer",
                          "Execute bank transfer to the exchange",
                          options,
                          &run, NULL))
    return 1;
  return global_ret;
}


/* end taler-bank-transfer.c */
