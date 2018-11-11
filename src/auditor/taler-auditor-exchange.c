/*
  This file is part of TALER
  Copyright (C) 2014, 2015, 2018 GNUnet e.V.

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
 * @file taler-auditor-exchange.c
 * @brief Tool used by the auditor to add or remove the exchange's master key
 *        to its database.
 * @author Christian Grothoff
 */
#include <platform.h>
#include "taler_exchangedb_lib.h"
#include "taler_auditordb_lib.h"


/**
 * URL of the exchange.
 */
static char *exchange_url;

/**
 * Master public key of the exchange.
 */
static struct TALER_MasterPublicKeyP master_public_key;

/**
 * Our configuration.
 */
static struct GNUNET_CONFIGURATION_Handle *cfg;

/**
 * Handle to access the auditor's database.
 */
static struct TALER_AUDITORDB_Plugin *adb;

/**
 * -r option given.
 */
static int remove_flag;


/**
 * The main function of the taler-auditor-exchange tool.  This tool is used
 * to add (or remove) an exchange's master key and base URL to the auditor's
 * database.
 *
 * @param argc number of arguments from the command line
 * @param argv command line arguments
 * @return 0 ok, 1 on error
 */
int
main (int argc,
      char *const *argv)
{
  char *cfgfile = NULL;
  const struct GNUNET_GETOPT_CommandLineOption options[] = {
    GNUNET_GETOPT_option_cfgfile (&cfgfile),
    GNUNET_GETOPT_option_help ("Add or remove exchange to list of audited exchanges"),
    GNUNET_GETOPT_option_mandatory
    (GNUNET_GETOPT_option_base32_auto ('m',
                                       "exchange-key",
                                       "KEY",
                                       "public key of the exchange (Crockford base32 encoded)",
                                       &master_public_key)),
    GNUNET_GETOPT_option_string ('u',
				 "exchange-url",
				 "URL",
				 "base URL of the exchange",
				 &exchange_url),
    GNUNET_GETOPT_option_flag ('r',
                               "remove",
                               "remove the exchange's key (default is to add)",
                               &remove_flag),
    GNUNET_GETOPT_option_version (VERSION "-" VCS_VERSION),
    GNUNET_GETOPT_OPTION_END
  };

  GNUNET_assert (GNUNET_OK ==
                 GNUNET_log_setup ("taler-auditor-exchange",
                                   "WARNING",
                                   NULL));
  if (GNUNET_GETOPT_run ("taler-auditor-exchange",
                         options,
                         argc, argv) <= 0)
    return 1;
  cfg = GNUNET_CONFIGURATION_create ();
  if (GNUNET_SYSERR ==
      GNUNET_CONFIGURATION_load (cfg,
                                 cfgfile))
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                _("Malformed configuration file `%s', exit ...\n"),
                cfgfile);
    GNUNET_free_non_null (cfgfile);
    return 1;
  }
  GNUNET_free_non_null (cfgfile);

  if (! remove_flag)
  {
    if (NULL == exchange_url)
    {
      FPRINTF (stderr,
	       _("Missing either `%s' or `%s'.\n"),
	       "-u URL",
	       "--remove");
      return 1;
    }
    if ( (0 == strlen (exchange_url)) ||
	 ( (0 != strncasecmp ("http://",
			      exchange_url,
			      strlen ("http://"))) &&
	   (0 != strncasecmp ("https://",
			      exchange_url,
			      strlen ("https://"))) )  ||
	 ('/' != exchange_url[strlen(exchange_url)-1]) )
    {
      fprintf (stderr,
	       "Exchange URL must begin with `http://` or `https://` and end with `/'\n");
      return 3;
    }
  }
	  
  
  if (NULL ==
      (adb = TALER_AUDITORDB_plugin_load (cfg)))
  {
    fprintf (stderr,
             "Failed to initialize auditor database plugin.\n");
    return 3;
  }

  /* Create required tables */
  if (GNUNET_OK !=
      adb->create_tables (adb->cls))
  {
    fprintf (stderr,
             "Failed to create tables in auditor's database\n");
    TALER_AUDITORDB_plugin_unload (adb);
    return 3;
  }

  /* Update DB */
  {
    enum GNUNET_DB_QueryStatus qs;
    struct TALER_AUDITORDB_Session *session;

    session = adb->get_session (adb->cls);
    if (NULL == session)
    {
      fprintf (stderr,
	       "Failed to initialize database session\n");
      TALER_AUDITORDB_plugin_unload (adb);
      return 3;
    }

    if (remove_flag)
    {
      qs = adb->delete_exchange (adb->cls,
                                 session,
                                 &master_public_key);
    }
    else
    {
      qs = adb->insert_exchange (adb->cls,
                                 session,
                                 &master_public_key,
                                 exchange_url);
    }
    if (0 > qs)
    {
      fprintf (stderr,
               "Failed to update auditor DB (%d)\n",
               qs);
      TALER_AUDITORDB_plugin_unload (adb);
      return 3;
    }
  }
  TALER_AUDITORDB_plugin_unload (adb);
  return 0;
}

/* end of taler-auditor-exchange.c */