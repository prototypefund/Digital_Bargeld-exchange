/*
  This file is part of TALER
  Copyright (C) 2015-2018 Taler Systems SA

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
 * @file taler-exchange-wire.c
 * @brief Create signed response for /wire requests.
 * @author Christian Grothoff
 */
#include <platform.h>
#include <jansson.h>
#include <gnunet/gnunet_json_lib.h>
#include "taler_crypto_lib.h"
#include "taler_util.h"
#include "taler_json_lib.h"
#include "taler_exchangedb_lib.h"
#include "taler_signatures.h"


/**
 * Filename of the master private key.
 */
static char *masterkeyfile;

/**
 * Private key for signing.
 */
static struct TALER_MasterPrivateKeyP master_priv;

/**
 * Return value from main().
 */
static int global_ret;


#include "key-helper.c"


/**
 * Function called with information about a wire account.  Signs
 * the account's wire details and writes out the JSON file to disk.
 *
 * @param cls closure
 * @param ai account information
 */
static void
sign_account_data (void *cls,
                   const struct TALER_EXCHANGEDB_AccountInfo *ai)
{
  char *json_out;
  FILE *out;
  int ret;

  (void) cls;
  if (GNUNET_NO == ai->credit_enabled)
    return;
  if (NULL == ai->wire_response_filename)
  {
    GNUNET_log_config_missing (GNUNET_ERROR_TYPE_ERROR,
                               ai->section_name,
                               "WIRE_RESPONSE");
    global_ret = 1;
    return;
  }

  {
    json_t *wire;

    wire = TALER_JSON_exchange_wire_signature_make (ai->payto_uri,
                                                    &master_priv);
    if (NULL == wire)
    {
      GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                  "Could not sign wire account `%s'. Is the URI well-formed?\n",
                  ai->payto_uri);
      global_ret = 1;
      return;
    }
    GNUNET_assert (NULL != wire);
    json_out = json_dumps (wire,
                           JSON_INDENT (2));
    json_decref (wire);
  }
  GNUNET_assert (NULL != json_out);
  if (GNUNET_OK !=
      GNUNET_DISK_directory_create_for_file (ai->wire_response_filename))
  {
    GNUNET_log_strerror_file (GNUNET_ERROR_TYPE_ERROR,
                              "mkdir",
                              ai->wire_response_filename);
    global_ret = 1;
    free (json_out);
    return;
  }

  out = fopen (ai->wire_response_filename,
               "w+"); /* create, if exists, truncate */
  if (NULL == out)
  {
    GNUNET_log_strerror_file (GNUNET_ERROR_TYPE_ERROR,
                              "fopen(w+)",
                              ai->wire_response_filename);
    global_ret = 1;
    free (json_out);
    return;
  }
  ret = fprintf (out,
                 "%s",
                 json_out);
  if ( (0 != fclose (out)) ||
       (-1 == ret) )
  {
    fprintf (stderr,
             "Failure creating wire account file `%s': %s\n",
             ai->wire_response_filename,
             strerror (errno));
    /* attempt to remove malformed file */
    if (0 != unlink (ai->wire_response_filename))
      GNUNET_log_strerror_file (GNUNET_ERROR_TYPE_WARNING,
                                "unlink",
                                ai->wire_response_filename);
  }
  else
  {
    GNUNET_log (GNUNET_ERROR_TYPE_INFO,
                "Created wire account file `%s'\n",
                ai->wire_response_filename);
  }
  free (json_out);
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
  (void) cls;
  (void) args;
  (void) cfgfile;

  if (GNUNET_OK !=
      get_and_check_master_key (cfg,
                                masterkeyfile,
                                &master_priv))
  {
    global_ret = 1;
    return;
  }
  GNUNET_log (GNUNET_ERROR_TYPE_INFO,
              "Signing /wire responses\n");
  TALER_EXCHANGEDB_find_accounts (cfg,
                                  &sign_account_data,
                                  NULL);
}


/**
 * The main function of the taler-exchange-wire tool.  This tool is
 * used to sign the bank account details using the master key.
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
    GNUNET_GETOPT_option_timetravel ('T',
                                     "timetravel"),
    GNUNET_GETOPT_option_filename ('m',
                                   "master-key",
                                   "FILENAME",
                                   "master key file (private key)",
                                   &masterkeyfile),
    GNUNET_GETOPT_OPTION_END
  };

  /* force linker to link against libtalerutil; if we do
     not do this, the linker may "optimize" libtalerutil
     away and skip #TALER_OS_init(), which we do need */
  (void) TALER_project_data_default ();
  GNUNET_assert (GNUNET_OK ==
                 GNUNET_log_setup ("taler-exchange-wire",
                                   "WARNING",
                                   NULL));
  if (GNUNET_OK !=
      GNUNET_PROGRAM_run (argc, argv,
                          "taler-exchange-wire",
                          "Setup /wire response",
                          options,
                          &run, NULL))
    return 1;
  return global_ret;
}


/* end of taler-exchange-wire.c */
