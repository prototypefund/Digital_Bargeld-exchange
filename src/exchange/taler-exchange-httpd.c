/*
  This file is part of TALER
  Copyright (C) 2014, 2015, 2016 Inria and GNUnet e.V.

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
 * @file taler-exchange-httpd.c
 * @brief Serve the HTTP interface of the exchange
 * @author Florian Dold
 * @author Benedikt Mueller
 * @author Christian Grothoff
 */
#include "platform.h"
#include <gnunet/gnunet_util_lib.h>
#include <jansson.h>
#include <microhttpd.h>
#include <pthread.h>
#include "taler-exchange-httpd_parsing.h"
#include "taler-exchange-httpd_mhd.h"
#include "taler-exchange-httpd_admin.h"
#include "taler-exchange-httpd_deposit.h"
#include "taler-exchange-httpd_refund.h"
#include "taler-exchange-httpd_reserve.h"
#include "taler-exchange-httpd_wire.h"
#include "taler-exchange-httpd_refresh.h"
#include "taler-exchange-httpd_tracking.h"
#include "taler-exchange-httpd_keystate.h"
#if HAVE_DEVELOPER
#include "taler-exchange-httpd_test.h"
#endif
#include "taler_exchangedb_plugin.h"
#include "taler-exchange-httpd_validation.h"


/**
 * Backlog for listen operation on unix
 * domain sockets.
 */
#define UNIX_BACKLOG 500

/**
 * Which currency is used by this exchange?
 */
char *TMH_exchange_currency_string;

/**
 * Should we return "Connection: close" in each response?
 */
int TMH_exchange_connection_close;

/**
 * Base directory of the exchange (global)
 */
char *TMH_exchange_directory;

/**
 * The exchange's configuration (global)
 */
struct GNUNET_CONFIGURATION_Handle *cfg;

/**
 * Master public key (according to the
 * configuration in the exchange directory).
 */
struct TALER_MasterPublicKeyP TMH_master_public_key;

/**
 * Our DB plugin.
 */
struct TALER_EXCHANGEDB_Plugin *TMH_plugin;

/**
 * Default timeout in seconds for HTTP requests.
 */
static unsigned int connection_timeout = 30;

/**
 * The HTTP Daemon.
 */
static struct MHD_Daemon *mhd;

/**
 * The HTTP Daemon for /admin-requests.
 */
static struct MHD_Daemon *mhd_admin;

/**
 * Do not offer /admin API.
 */
static int no_admin;

/**
 * Port to run the daemon on.
 */
static uint16_t serve_port;

/**
 * Port to run the admin daemon on.
 */
static uint16_t serve_admin_port;

/**
 * Path for the unix domain-socket
 * to run the daemon on.
 */
static char *serve_unixpath;

/**
 * Path for the unix domain-socket
 * to run the admin daemon on.
 */
static char *serve_admin_unixpath;

/**
 * File mode for unix-domain socket.
 */
static mode_t unixpath_mode;

/**
 * File mode for unix-domain socket.
 */
static mode_t unixpath_admin_mode;


/**
 * Function called whenever MHD is done with a request.  If the
 * request was a POST, we may have stored a `struct Buffer *` in the
 * @a con_cls that might still need to be cleaned up.  Call the
 * respective function to free the memory.
 *
 * @param cls client-defined closure
 * @param connection connection handle
 * @param con_cls value as set by the last call to
 *        the #MHD_AccessHandlerCallback
 * @param toe reason for request termination
 * @see #MHD_OPTION_NOTIFY_COMPLETED
 * @ingroup request
 */
static void
handle_mhd_completion_callback (void *cls,
                                struct MHD_Connection *connection,
                                void **con_cls,
                                enum MHD_RequestTerminationCode toe)
{
  if (NULL == *con_cls)
    return;
  TMH_PARSE_post_cleanup_callback (*con_cls);
  *con_cls = NULL;
}


/**
 * Handle incoming HTTP request.
 *
 * @param cls closure for MHD daemon (unused)
 * @param connection the connection
 * @param url the requested url
 * @param method the method (POST, GET, ...)
 * @param version HTTP version (ignored)
 * @param upload_data request data
 * @param upload_data_size size of @a upload_data in bytes
 * @param con_cls closure for request (a `struct Buffer *`)
 * @return MHD result code
 */
static int
handle_mhd_request (void *cls,
                    struct MHD_Connection *connection,
                    const char *url,
                    const char *method,
                    const char *version,
                    const char *upload_data,
                    size_t *upload_data_size,
                    void **con_cls)
{
  static struct TMH_RequestHandler handlers[] =
    {
      /* Landing page, tell humans to go away. */
      { "/", MHD_HTTP_METHOD_GET, "text/plain",
        "Hello, I'm the Taler exchange. This HTTP server is not for humans.\n", 0,
        &TMH_MHD_handler_static_response, MHD_HTTP_OK },
      /* /robots.txt: disallow everything */
      { "/robots.txt", MHD_HTTP_METHOD_GET, "text/plain",
        "User-agent: *\nDisallow: /\n", 0,
        &TMH_MHD_handler_static_response, MHD_HTTP_OK },
      /* AGPL licensing page, redirect to source. As per the AGPL-license,
         every deployment is required to offer the user a download of the
         source. We make this easy by including a redirect to the source
         here. */
      { "/agpl", MHD_HTTP_METHOD_GET, "text/plain",
        NULL, 0,
        &TMH_MHD_handler_agpl_redirect, MHD_HTTP_FOUND },

      /* Return key material and fundamental properties for this exchange */
      { "/keys", MHD_HTTP_METHOD_GET, "application/json",
        NULL, 0,
        &TMH_KS_handler_keys, MHD_HTTP_OK },
      { "/keys", NULL, "text/plain",
        "Only GET is allowed", 0,
        &TMH_MHD_handler_send_json_pack_error, MHD_HTTP_METHOD_NOT_ALLOWED },

      /* Requests for wiring information */
      { "/wire", MHD_HTTP_METHOD_GET, "application/json",
        NULL, 0,
        &TMH_WIRE_handler_wire, MHD_HTTP_OK },
      { "/wire", NULL, "text/plain",
        "Only GET is allowed", 0,
        &TMH_MHD_handler_send_json_pack_error, MHD_HTTP_METHOD_NOT_ALLOWED },

      /* Withdrawing coins / interaction with reserves */
      { "/reserve/status", MHD_HTTP_METHOD_GET, "application/json",
        NULL, 0,
        &TMH_RESERVE_handler_reserve_status, MHD_HTTP_OK },
      { "/reserve/status", NULL, "text/plain",
        "Only GET is allowed", 0,
        &TMH_MHD_handler_send_json_pack_error, MHD_HTTP_METHOD_NOT_ALLOWED },

      { "/reserve/withdraw", MHD_HTTP_METHOD_POST, "application/json",
        NULL, 0,
        &TMH_RESERVE_handler_reserve_withdraw, MHD_HTTP_OK },
      { "/reserve/withdraw", NULL, "text/plain",
        "Only POST is allowed", 0,
        &TMH_MHD_handler_send_json_pack_error, MHD_HTTP_METHOD_NOT_ALLOWED },

      /* Depositing coins */
      { "/deposit", MHD_HTTP_METHOD_POST, "application/json",
        NULL, 0,
        &TMH_DEPOSIT_handler_deposit, MHD_HTTP_OK },
      { "/deposit", NULL, "text/plain",
        "Only POST is allowed", 0,
        &TMH_MHD_handler_send_json_pack_error, MHD_HTTP_METHOD_NOT_ALLOWED },

      /* Refunding coins */
      { "/refund", MHD_HTTP_METHOD_POST, "application/json",
        NULL, 0,
        &TMH_REFUND_handler_refund, MHD_HTTP_OK },
      { "/refund", NULL, "text/plain",
        "Only POST is allowed", 0,
        &TMH_MHD_handler_send_json_pack_error, MHD_HTTP_METHOD_NOT_ALLOWED },

      /* Dealing with change */
      { "/refresh/melt", MHD_HTTP_METHOD_POST, "application/json",
        NULL, 0,
        &TMH_REFRESH_handler_refresh_melt, MHD_HTTP_OK },
      { "/refresh/melt", NULL, "text/plain",
        "Only POST is allowed", 0,
        &TMH_MHD_handler_send_json_pack_error, MHD_HTTP_METHOD_NOT_ALLOWED },

      { "/refresh/reveal", MHD_HTTP_METHOD_POST, "application/json",
        NULL, 0,
        &TMH_REFRESH_handler_refresh_reveal, MHD_HTTP_OK },
      { "/refresh/reveal", NULL, "text/plain",
        "Only POST is allowed", 0,
        &TMH_MHD_handler_send_json_pack_error, MHD_HTTP_METHOD_NOT_ALLOWED },

      { "/refresh/reveal", MHD_HTTP_METHOD_POST, "application/json",
        NULL, 0,
        &TMH_REFRESH_handler_refresh_reveal, MHD_HTTP_OK },
      { "/refresh/reveal", NULL, "text/plain",
        "Only POST is allowed", 0,
        &TMH_MHD_handler_send_json_pack_error, MHD_HTTP_METHOD_NOT_ALLOWED },

      { "/refresh/link", MHD_HTTP_METHOD_GET, "application/json",
        NULL, 0,
        &TMH_REFRESH_handler_refresh_link, MHD_HTTP_OK },
      { "/refresh/link", NULL, "text/plain",
        "Only GET is allowed", 0,
        &TMH_MHD_handler_send_json_pack_error, MHD_HTTP_METHOD_NOT_ALLOWED },

      { "/track/transfer", MHD_HTTP_METHOD_GET, "application/json",
        NULL, 0,
        &TMH_TRACKING_handler_track_transfer, MHD_HTTP_OK },
      { "/track/transfer", NULL, "text/plain",
        "Only GET is allowed", 0,
        &TMH_MHD_handler_send_json_pack_error, MHD_HTTP_METHOD_NOT_ALLOWED },
      { "/track/transaction", MHD_HTTP_METHOD_POST, "application/json",
        NULL, 0,
        &TMH_TRACKING_handler_track_transaction, MHD_HTTP_OK },
      { "/track/transaction", NULL, "text/plain",
        "Only POST is allowed", 0,
        &TMH_MHD_handler_send_json_pack_error, MHD_HTTP_METHOD_NOT_ALLOWED },

      { NULL, NULL, NULL, NULL, 0, 0 }
    };
  static struct TMH_RequestHandler h404 =
    {
      "", NULL, "text/html",
      "<html><title>404: not found</title></html>", 0,
      &TMH_MHD_handler_static_response, MHD_HTTP_NOT_FOUND
    };
  struct TMH_RequestHandler *rh;
  unsigned int i;

  GNUNET_log (GNUNET_ERROR_TYPE_INFO,
              "Handling request for URL '%s'\n",
              url);
  for (i=0;NULL != handlers[i].url;i++)
  {
    rh = &handlers[i];
    if ( (0 == strcasecmp (url,
                           rh->url)) &&
         ( (NULL == rh->method) ||
           (0 == strcasecmp (method,
                             rh->method)) ) )
      return rh->handler (rh,
                          connection,
                          con_cls,
                          upload_data,
                          upload_data_size);
  }
  return TMH_MHD_handler_static_response (&h404,
                                          connection,
                                          con_cls,
                                          upload_data,
                                          upload_data_size);
}


/**
 * Handle incoming administrative HTTP request.
 *
 * @param cls closure for MHD daemon (unused)
 * @param connection the connection
 * @param url the requested url
 * @param method the method (POST, GET, ...)
 * @param version HTTP version (ignored)
 * @param upload_data request data
 * @param upload_data_size size of @a upload_data in bytes
 * @param con_cls closure for request (a `struct Buffer *`)
 * @return MHD result code
 */
static int
handle_mhd_admin_request (void *cls,
                          struct MHD_Connection *connection,
                          const char *url,
                          const char *method,
                          const char *version,
                          const char *upload_data,
                          size_t *upload_data_size,
                          void **con_cls)
{
  static struct TMH_RequestHandler handlers[] =
    {
      { "/admin/add/incoming", MHD_HTTP_METHOD_POST, "application/json",
        NULL, 0,
        &TMH_ADMIN_handler_admin_add_incoming, MHD_HTTP_OK },
      { "/admin/add/incoming", NULL, "text/plain",
        "Only POST is allowed", 0,
        &TMH_MHD_handler_send_json_pack_error, MHD_HTTP_METHOD_NOT_ALLOWED },

#if HAVE_DEVELOPER
      /* Client crypto-interoperability test functions */
      { "/test", MHD_HTTP_METHOD_POST, "application/json",
        NULL, 0,
        &TMH_TEST_handler_test, MHD_HTTP_OK },
      { "/test", NULL, "text/plain",
        "Only POST is allowed", 0,
        &TMH_MHD_handler_send_json_pack_error, MHD_HTTP_METHOD_NOT_ALLOWED },

      { "/test/base32", MHD_HTTP_METHOD_POST, "application/json",
	NULL, 0,
	&TMH_TEST_handler_test_base32, MHD_HTTP_OK },
      { "/test/base32", NULL, "text/plain",
        "Only POST is allowed", 0,
        &TMH_MHD_handler_send_json_pack_error, MHD_HTTP_METHOD_NOT_ALLOWED },

      { "/test/encrypt", MHD_HTTP_METHOD_POST, "application/json",
        NULL, 0,
	&TMH_TEST_handler_test_encrypt, MHD_HTTP_OK },
      { "/test/encrypt", NULL, "text/plain",
        "Only POST is allowed", 0,
        &TMH_MHD_handler_send_json_pack_error, MHD_HTTP_METHOD_NOT_ALLOWED },

      { "/test/hkdf", MHD_HTTP_METHOD_POST, "application/json",
	NULL, 0,
	&TMH_TEST_handler_test_hkdf, MHD_HTTP_OK },
      { "/test/hkdf", NULL, "text/plain",
        "Only POST is allowed", 0,
        &TMH_MHD_handler_send_json_pack_error, MHD_HTTP_METHOD_NOT_ALLOWED },

      { "/test/ecdhe", MHD_HTTP_METHOD_POST, "application/json",
	NULL, 0,
	&TMH_TEST_handler_test_ecdhe, MHD_HTTP_OK },
      { "/test/ecdhe", NULL, "text/plain",
        "Only POST is allowed", 0,
        &TMH_MHD_handler_send_json_pack_error, MHD_HTTP_METHOD_NOT_ALLOWED },

      { "/test/eddsa", MHD_HTTP_METHOD_POST, "application/json",
	NULL, 0,
	&TMH_TEST_handler_test_eddsa, MHD_HTTP_OK },
      { "/test/eddsa", NULL, "text/plain",
        "Only POST is allowed", 0,
        &TMH_MHD_handler_send_json_pack_error, MHD_HTTP_METHOD_NOT_ALLOWED },

      { "/test/rsa/get", MHD_HTTP_METHOD_GET, "application/json",
	NULL, 0,
	&TMH_TEST_handler_test_rsa_get, MHD_HTTP_OK },
      { "/test/rsa/get", NULL, "text/plain",
        "Only GET is allowed", 0,
        &TMH_MHD_handler_send_json_pack_error, MHD_HTTP_METHOD_NOT_ALLOWED },

      { "/test/rsa/sign", MHD_HTTP_METHOD_POST, "application/json",
	NULL, 0,
	&TMH_TEST_handler_test_rsa_sign, MHD_HTTP_OK },
      { "/test/rsa/sign", NULL, "text/plain",
        "Only POST is allowed", 0,
        &TMH_MHD_handler_send_json_pack_error, MHD_HTTP_METHOD_NOT_ALLOWED },

      { "/test/transfer", MHD_HTTP_METHOD_POST, "application/json",
	NULL, 0,
	&TMH_TEST_handler_test_transfer, MHD_HTTP_OK },
      { "/test/transfer", NULL, "text/plain",
        "Only POST is allowed", 0,
        &TMH_MHD_handler_send_json_pack_error, MHD_HTTP_METHOD_NOT_ALLOWED },
#endif

      { NULL, NULL, NULL, NULL, 0, 0 }
    };
  static struct TMH_RequestHandler h404 =
    {
      "", NULL, "text/html",
      "<html><title>404: not found</title></html>", 0,
      &TMH_MHD_handler_static_response, MHD_HTTP_NOT_FOUND
    };
  struct TMH_RequestHandler *rh;
  unsigned int i;

  GNUNET_log (GNUNET_ERROR_TYPE_INFO,
              "Handling request for URL '%s'\n",
              url);
  for (i=0;NULL != handlers[i].url;i++)
  {
    rh = &handlers[i];
    if ( (0 == strcasecmp (url,
                           rh->url)) &&
         ( (NULL == rh->method) ||
           (0 == strcasecmp (method,
                             rh->method)) ) )
      return rh->handler (rh,
                          connection,
                          con_cls,
                          upload_data,
                          upload_data_size);
  }
  return TMH_MHD_handler_static_response (&h404,
                                          connection,
                                          con_cls,
                                          upload_data,
                                          upload_data_size);
}


/**
 * Parse the configuration to determine on which port
 * or UNIX domain path we should run an HTTP service.
 *
 * @param type section of the configuration to parse ("exchange" or "exchange-admin")
 * @param[out] port set to the port number, or 0 for none
 * @param[out] unix_path set to the UNIX path, or NULL for none
 * @param[out] unix_mode set to the mode to be used for @a unix_path
 * @return #GNUNET_OK on success
 */
static int
parse_port_config (const char *section,
                   uint16_t *rport,
                   char **unix_path,
                   mode_t *unix_mode)
{
  const char *choices[] = {"tcp", "unix"};
  const char *serve_type;
  unsigned long long port;

  if (GNUNET_OK !=
      GNUNET_CONFIGURATION_get_value_choice (cfg,
                                             section,
                                             "serve",
                                             choices,
                                             &serve_type))
  {
    GNUNET_log_config_invalid (GNUNET_ERROR_TYPE_ERROR,
                               section,
                               "serve",
                               "serve type required");
    return GNUNET_SYSERR;
  }

  if (0 == strcmp (serve_type, "tcp"))
  {
    if (GNUNET_OK !=
        GNUNET_CONFIGURATION_get_value_number (cfg,
                                               section,
                                               "port",
                                               &port))
    {
      GNUNET_log_config_invalid (GNUNET_ERROR_TYPE_ERROR,
                                 section,
                                 "port",
                                 "port number required");
      return GNUNET_SYSERR;
    }

    if ( (0 == port) ||
         (port > UINT16_MAX) )
    {
      fprintf (stderr,
               "Invalid configuration (value out of range): %llu is not a valid port\n",
               port);
      return GNUNET_SYSERR;
    }
    *rport = (uint16_t) port;
    *unix_path = NULL;
    return GNUNET_OK;
  }
  if (0 == strcmp (serve_type, "unix"))
  {
    struct sockaddr_un s_un;
    char *modestring;

    if (GNUNET_OK !=
        GNUNET_CONFIGURATION_get_value_filename (cfg,
                                                 section,
                                                 "unixpath",
                                                 unix_path))
    {
      GNUNET_log_config_invalid (GNUNET_ERROR_TYPE_ERROR,
                                 section,
                                 "unixpath",
                                 "unixpath required");
      return GNUNET_SYSERR;
    }
    if (strlen (*unix_path) >= sizeof (s_un.sun_path))
    {
      fprintf (stderr,
               "Invalid configuration: unix path too long\n");
      return GNUNET_SYSERR;
    }

    if (GNUNET_OK !=
        GNUNET_CONFIGURATION_get_value_string (cfg,
                                               section,
                                               "unixpath_mode",
                                               &modestring))
    {
      GNUNET_log_config_invalid (GNUNET_ERROR_TYPE_ERROR,
                                 section,
                                 "unixpath_mode",
                                 "unixpath_mode required");
      return GNUNET_SYSERR;
    }
    errno = 0;
    *unix_mode = (mode_t) strtoul (modestring, NULL, 8);
    if (0 != errno)
    {
      GNUNET_log_config_invalid (GNUNET_ERROR_TYPE_ERROR,
                                 section,
                                 "unixpath_mode",
                                 "unixpath_mode required");
      GNUNET_free (modestring);
      return GNUNET_SYSERR;
    }
    GNUNET_free (modestring);
    return GNUNET_OK;
  }
  /* not reached */
  GNUNET_assert (0);
  return GNUNET_SYSERR;
}


/**
 * Load configuration parameters for the exchange
 * server into the corresponding global variables.
 *
 * @param exchange_directory the exchange's directory
 * @return #GNUNET_OK on success
 */
static int
exchange_serve_process_config ()
{
  char *TMH_master_public_key_str;

  if (GNUNET_OK !=
      GNUNET_CONFIGURATION_get_value_filename (cfg,
                                               "exchange",
                                               "KEYDIR",
                                               &TMH_exchange_directory))
  {
    GNUNET_log_config_missing (GNUNET_ERROR_TYPE_ERROR,
                               "exchange",
                               "KEYDIR");
    return GNUNET_SYSERR;
  }
  if (GNUNET_OK !=
      GNUNET_CONFIGURATION_get_value_string (cfg,
                                             "taler",
                                             "currency",
                                             &TMH_exchange_currency_string))
  {
    GNUNET_log_config_missing (GNUNET_ERROR_TYPE_ERROR,
                               "taler",
                               "currency");
    return GNUNET_SYSERR;
  }
  if (strlen (TMH_exchange_currency_string) >= TALER_CURRENCY_LEN)
  {
    fprintf (stderr,
             "Currency `%s' longer than the allowed limit of %u characters.",
             TMH_exchange_currency_string,
             (unsigned int) TALER_CURRENCY_LEN);
    return GNUNET_SYSERR;
  }
  if (GNUNET_OK !=
      TMH_VALIDATION_init (cfg))
    return GNUNET_SYSERR;

  if (GNUNET_OK !=
      GNUNET_CONFIGURATION_get_value_string (cfg,
                                             "exchange",
                                             "master_public_key",
                                             &TMH_master_public_key_str))
  {
    GNUNET_log_config_missing (GNUNET_ERROR_TYPE_ERROR,
                               "exchange",
                               "master_public_key");
    TMH_VALIDATION_done ();
    return GNUNET_SYSERR;
  }
  if (GNUNET_OK !=
      GNUNET_CRYPTO_eddsa_public_key_from_string (TMH_master_public_key_str,
                                                  strlen (TMH_master_public_key_str),
                                                  &TMH_master_public_key.eddsa_pub))
  {
    fprintf (stderr,
             "Invalid master public key given in exchange configuration.");
    GNUNET_free (TMH_master_public_key_str);
    TMH_VALIDATION_done ();
    return GNUNET_SYSERR;
  }
  GNUNET_free (TMH_master_public_key_str);

  if (NULL ==
      (TMH_plugin = TALER_EXCHANGEDB_plugin_load (cfg)))
  {
    fprintf (stderr,
             "Failed to initialize DB subsystem\n");
    TMH_VALIDATION_done ();
    return GNUNET_SYSERR;
  }
  if (GNUNET_YES ==
      GNUNET_CONFIGURATION_get_value_yesno (cfg,
                                            "exchange",
                                            "TESTRUN"))
  {
    GNUNET_log (GNUNET_ERROR_TYPE_INFO,
                "Running in TEST mode! Database contents will not persist!\n");
    TMH_plugin->create_tables (TMH_plugin->cls);
  }

  if (GNUNET_OK !=
      parse_port_config ("exchange",
                         &serve_port,
                         &serve_unixpath,
                         &unixpath_mode))
  {
    TMH_VALIDATION_done ();
    return GNUNET_SYSERR;
  }
  if (GNUNET_OK !=
      parse_port_config ("exchange-admin",
                         &serve_admin_port,
                         &serve_admin_unixpath,
                         &unixpath_admin_mode))
  {
    TMH_VALIDATION_done ();
    return GNUNET_SYSERR;
  }
  return GNUNET_OK;
}


/* Developer logic for supporting the `-f' option. */
#if HAVE_DEVELOPER

/**
 * Option `-f' (specifies an input file to give to the HTTP server).
 */
static char *input_filename;


/**
 * Run 'nc' or 'ncat' as a fake HTTP client using #input_filename
 * as the input for the request.  If launching the client worked,
 * run the #TMH_KS_loop() event loop as usual.
 *
 * @return #GNUNET_OK
 */
static int
run_fake_client ()
{
  pid_t cld;
  char ports[6];
  int fd;
  int ret;
  int status;

  fd = open (input_filename, O_RDONLY);
  if (-1 == fd)
  {
    fprintf (stderr,
             "Failed to open `%s': %s\n",
             input_filename,
             strerror (errno));
    return GNUNET_SYSERR;
  }
  /* Fake HTTP client request with #input_filename as input.
     We do this using the nc tool. */
  GNUNET_snprintf (ports,
                   sizeof (ports),
                   "%u",
                   serve_port);
  if (0 == (cld = fork()))
  {
    GNUNET_break (0 == close (0));
    GNUNET_break (0 == dup2 (fd, 0));
    GNUNET_break (0 == close (fd));
    if ( (0 != execlp ("nc",
                       "nc",
                       "localhost",
                       ports,
                       "-w", "30",
                       NULL)) &&
         (0 != execlp ("ncat",
                       "ncat",
                       "localhost",
                       ports,
                       "-i", "30",
                       NULL)) )
    {
      fprintf (stderr,
               "Failed to run both `nc' and `ncat': %s\n",
               strerror (errno));
    }
    _exit (1);
  }
  /* parent process */
  GNUNET_break (0 == close (fd));
  ret = TMH_KS_loop ();
  if (cld != waitpid (cld, &status, 0))
    fprintf (stderr,
             "Waiting for `nc' child failed: %s\n",
             strerror (errno));
  return ret;
}


/**
 * Signature of the callback used by MHD to notify the application
 * about completed connections.  If we are running in test-mode with
 * an #input_filename, this function is used to terminate the HTTPD
 * after the first request has been processed.
 *
 * @param cls client-defined closure, NULL
 * @param connection connection handle (ignored)
 * @param socket_context socket-specific pointer (ignored)
 * @param toe reason for connection notification
 */
static void
connection_done (void *cls,
                 struct MHD_Connection *connection,
                 void **socket_context,
                 enum MHD_ConnectionNotificationCode toe)
{
  /* We only act if the connection is closed. */
  if (MHD_CONNECTION_NOTIFY_CLOSED != toe)
    return;
  /* This callback is also present if the option wasn't, so
     make sure the option was actually set. */
  if (NULL == input_filename)
    return;
  /* We signal ourselves to terminate. */
  if (0 != kill (getpid(),
                 SIGTERM))
    GNUNET_log_strerror (GNUNET_ERROR_TYPE_ERROR,
                         "kill");
}

/* end of HAVE_DEVELOPER */
#endif


/**
 * Function called for logging by MHD.
 *
 * @param cls closure, NULL
 * @param fm format string (`printf()`-style)
 * @param ap arguments to @a fm
 */
static void
handle_mhd_logs (void *cls,
                 const char *fm,
                 va_list ap)
{
  char buf[2048];

  vsnprintf (buf,
             sizeof (buf),
             fm,
             ap);
  GNUNET_log_from (GNUNET_ERROR_TYPE_WARNING,
                   "libmicrohttpd",
                   "%s",
                   buf);
}


/**
 * Open UNIX domain socket for listining at @a unix_path with
 * permissions @a unix_mode.
 *
 * @param unix_path where to listen
 * @param unix_mode access permissions to set
 * @return -1 on error, otherwise the listen socket
 */
static int
open_unix_path (const char *unix_path,
                mode_t unix_mode)
{
  struct GNUNET_NETWORK_Handle *nh;
  struct sockaddr_un *un;
  int fd;

  if (sizeof (un->sun_path) <= strlen (unix_path))
  {
    fprintf (stderr,
             "unixpath `%s' too long\n",
             unix_path);
    return -1;
  }
  GNUNET_log (GNUNET_ERROR_TYPE_INFO,
              "Creating listen socket '%s' with mode %o\n",
              unix_path,
              unix_mode);

  if (GNUNET_OK !=
      GNUNET_DISK_directory_create_for_file (unix_path))
  {
    GNUNET_log_strerror_file (GNUNET_ERROR_TYPE_ERROR,
                              "mkdir",
                              unix_path);
  }

  un = GNUNET_new (struct sockaddr_un);
  un->sun_family = AF_UNIX;
  strncpy (un->sun_path,
           unix_path,
           sizeof (un->sun_path) - 1);
  GNUNET_NETWORK_unix_precheck (un);

  if (NULL == (nh = GNUNET_NETWORK_socket_create (AF_UNIX,
                                                  SOCK_STREAM,
                                                  0)))
  {
    fprintf (stderr,
             "create failed for AF_UNIX\n");
    GNUNET_free (un);
    return -1;
  }
  if (GNUNET_OK !=
      GNUNET_NETWORK_socket_bind (nh,
                                  (void *) un,
                                  sizeof (struct sockaddr_un)))
  {
    fprintf (stderr,
             "bind failed for AF_UNIX\n");
    GNUNET_free (un);
    GNUNET_NETWORK_socket_close (nh);
    return -1;
  }
  GNUNET_free (un);
  if (GNUNET_OK !=
      GNUNET_NETWORK_socket_listen (nh,
                                    UNIX_BACKLOG))
  {
    fprintf (stderr,
             "listen failed for AF_UNIX\n");
    GNUNET_NETWORK_socket_close (nh);
    return -1;
  }

  if (0 != chmod (serve_unixpath,
                  unix_mode))
  {
    fprintf (stderr,
             "chmod failed: %s\n",
             strerror (errno));
    GNUNET_NETWORK_socket_close (nh);
    return -1;
  }
  GNUNET_log (GNUNET_ERROR_TYPE_INFO,
              "set socket '%s' to mode %o\n",
              unix_path,
              unix_mode);
  fd = GNUNET_NETWORK_get_fd (nh);
  GNUNET_NETWORK_socket_free_memory_only_ (nh);
  return fd;
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
  char *cfgfile = NULL;
  const struct GNUNET_GETOPT_CommandLineOption options[] = {
    {'C', "connection-close", NULL,
     "force HTTP connections to be closed after each request", 0,
     &GNUNET_GETOPT_set_one, &TMH_exchange_connection_close},
    GNUNET_GETOPT_OPTION_CFG_FILE (&cfgfile),
    {'D', "disable-admin", NULL,
     "do not run the /admin-HTTP server", 0,
     &GNUNET_GETOPT_set_one, &no_admin},
    {'t', "timeout", "SECONDS",
     "after how long do connections timeout by default (in seconds)", 1,
     &GNUNET_GETOPT_set_uint, &connection_timeout},
#if HAVE_DEVELOPER
    {'f', "file-input", "FILENAME",
     "run in test-mode using FILENAME as the HTTP request to process", 1,
     &GNUNET_GETOPT_set_filename, &input_filename},
#endif
    GNUNET_GETOPT_OPTION_HELP ("HTTP server providing a RESTful API to access a Taler exchange"),
    GNUNET_GETOPT_OPTION_VERSION (VERSION "-" VCS_VERSION),
    GNUNET_GETOPT_OPTION_END
  };
  int ret;
  const char *listen_pid;
  const char *listen_fds;
  int fh = -1;
  int fh_admin = -1;

  GNUNET_assert (GNUNET_OK ==
                 GNUNET_log_setup ("taler-exchange-httpd",
                                   "INFO",
                                   NULL));
  if (0 >=
      GNUNET_GETOPT_run ("taler-exchange-httpd",
                         options,
                         argc, argv))
    return 1;
  if (NULL == cfgfile)
    cfgfile = GNUNET_strdup (GNUNET_OS_project_data_get ()->user_config_file);
  cfg = GNUNET_CONFIGURATION_create ();
  if (GNUNET_SYSERR ==
      GNUNET_CONFIGURATION_load (cfg, cfgfile))
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                _("Malformed configuration file `%s', exit ...\n"),
                cfgfile);
    GNUNET_free_non_null (cfgfile);
    return 1;
  }
  GNUNET_free_non_null (cfgfile);
  if (GNUNET_OK !=
      exchange_serve_process_config ())
    return 1;

  /* check for systemd-style FD passing */
  listen_pid = getenv ("LISTEN_PID");
  listen_fds = getenv ("LISTEN_FDS");
  if ( (NULL != listen_pid) &&
       (NULL != listen_fds) &&
       (getpid() == strtol (listen_pid, NULL, 10)) &&
       ( (1 == strtoul (listen_fds, NULL, 10)) ||
         (2 == strtoul (listen_fds, NULL, 10)) ) )
  {
    int flags;

    fh = 3;
    flags = fcntl (fh, F_GETFD);
    if ( (-1 == flags) && (EBADF == errno) )
    {
      fprintf (stderr,
               "Bad listen socket passed, ignored\n");
      fh = -1;
    }
    flags |= FD_CLOEXEC;
    if (0 != fcntl (fh, F_SETFD, flags))
      GNUNET_log_strerror (GNUNET_ERROR_TYPE_ERROR,
                           "fcntl");

    if (2 == strtoul (listen_fds, NULL, 10))
    {
      fh_admin = 4;
      flags = fcntl (fh_admin, F_GETFD);
      if ( (-1 == flags) && (EBADF == errno) )
      {
        fprintf (stderr,
                 "Bad listen socket passed, ignored\n");
        fh_admin = -1;
      }
      flags |= FD_CLOEXEC;
      if (0 != fcntl (fh_admin, F_SETFD, flags))
        GNUNET_log_strerror (GNUNET_ERROR_TYPE_ERROR,
                             "fcntl");
    }
  }

  /* consider unix path */
  if ( (-1 == fh) &&
       (NULL != serve_unixpath) )
  {
    fh = open_unix_path (serve_unixpath,
                         unixpath_mode);
    if (-1 == fh)
      return 1;
  }
  if ( (-1 == fh_admin) &&
       (0 == no_admin) &&
       (NULL != serve_admin_unixpath) )
  {
    fh_admin = open_unix_path (serve_admin_unixpath,
                               unixpath_admin_mode);
    if (-1 == fh_admin)
    {
      GNUNET_break (0 == close (fh));
      return 1;
    }
  }

  mhd
    = MHD_start_daemon (MHD_USE_SELECT_INTERNALLY | MHD_USE_PIPE_FOR_SHUTDOWN | MHD_USE_DEBUG,
                        (-1 == fh) ? serve_port : 0,
                        NULL, NULL,
                        &handle_mhd_request, NULL,
                        MHD_OPTION_LISTEN_SOCKET, fh,
                        MHD_OPTION_EXTERNAL_LOGGER, &handle_mhd_logs, NULL,
                        MHD_OPTION_NOTIFY_COMPLETED, &handle_mhd_completion_callback, NULL,
                        MHD_OPTION_CONNECTION_TIMEOUT, connection_timeout,
#if HAVE_DEVELOPER
                        MHD_OPTION_NOTIFY_CONNECTION, &connection_done, NULL,
#endif
                        MHD_OPTION_END);
  if (NULL == mhd)
  {
    fprintf (stderr,
             "Failed to start HTTP server.\n");
    return 1;
  }

  if (0 == no_admin)
  {
    mhd_admin
      = MHD_start_daemon (MHD_USE_SELECT_INTERNALLY | MHD_USE_PIPE_FOR_SHUTDOWN | MHD_USE_DEBUG,
                          (-1 == fh) ? serve_admin_port : 0,
                          NULL, NULL,
                          &handle_mhd_admin_request, NULL,
                          MHD_OPTION_LISTEN_SOCKET, fh_admin,
                          MHD_OPTION_EXTERNAL_LOGGER, &handle_mhd_logs, NULL,
                          MHD_OPTION_NOTIFY_COMPLETED, &handle_mhd_completion_callback, NULL,
                          MHD_OPTION_CONNECTION_TIMEOUT, connection_timeout,
#if HAVE_DEVELOPER
                          MHD_OPTION_NOTIFY_CONNECTION, &connection_done, NULL,
#endif
                          MHD_OPTION_END);
    if (NULL == mhd_admin)
    {
      fprintf (stderr,
               "Failed to start administrative HTTP server.\n");
      MHD_stop_daemon (mhd);
      return 1;
    }
  }

#if HAVE_DEVELOPER
  if (NULL != input_filename)
  {
    /* run only the testfile input, then terminate */
    ret = run_fake_client ();
  }
  else
  {
    /* normal behavior */
    ret = TMH_KS_loop ();
  }
#else
  /* normal behavior */
  ret = TMH_KS_loop ();
#endif

  switch (ret)
  {
  case GNUNET_OK:
  case GNUNET_SYSERR:
    MHD_stop_daemon (mhd);
    if (NULL != mhd_admin)
      MHD_stop_daemon (mhd_admin);
    break;
  case GNUNET_NO:
    {
      MHD_socket sock = MHD_quiesce_daemon (mhd);
      MHD_socket admin_sock;
      pid_t chld;
      int flags;

      /* Set flags to make 'sock' inherited by child */
      if (NULL != mhd_admin)
        admin_sock = MHD_quiesce_daemon (mhd_admin);
      flags = fcntl (sock, F_GETFD);
      GNUNET_assert (-1 != flags);
      flags &= ~FD_CLOEXEC;
      GNUNET_assert (-1 != fcntl (sock, F_SETFD, flags));
      chld = fork ();
      if (-1 == chld)
      {
        /* fork() failed, continue clean up, unhappily */
        GNUNET_log_strerror (GNUNET_ERROR_TYPE_ERROR,
                             "fork");
      }
      if (0 == chld)
      {
        char pids[12];

        /* exec another taler-exchange-httpd, passing on the listen socket;
           as in systemd it is expected to be on FD #3 */
        if (3 != dup2 (sock, 3))
        {
          GNUNET_log_strerror (GNUNET_ERROR_TYPE_ERROR,
                               "dup2");
          _exit (1);
        }
        if ( (NULL != mhd_admin) &&
             (4 != dup2 (admin_sock, 4)) )
        {
          GNUNET_log_strerror (GNUNET_ERROR_TYPE_ERROR,
                               "dup2");
          _exit (1);
        }
        /* Tell the child that it is the desired recipient for FD #3 */
        GNUNET_snprintf (pids,
                         sizeof (pids),
                         "%u",
                         getpid ());
        setenv ("LISTEN_PID", pids, 1);
        setenv ("LISTEN_FDS", (NULL != mhd_admin) ? "2" : "1", 1);
        /* Finally, exec the (presumably) more recent exchange binary */
        execvp ("taler-exchange-httpd",
                argv);
        GNUNET_log_strerror (GNUNET_ERROR_TYPE_ERROR,
                             "execvp");
        _exit (1);
      }
      /* we're the original process, handle remaining contextions
         before exiting; as the listen socket is no longer used,
         close it here */
      GNUNET_break (0 == close (sock));
      if (NULL != mhd_admin)
        GNUNET_break (0 == close (admin_sock));
      while ( (0 != MHD_get_daemon_info (mhd,
                                         MHD_DAEMON_INFO_CURRENT_CONNECTIONS)->num_connections) ||
              ( (NULL != mhd_admin) &&
                (0 != MHD_get_daemon_info (mhd_admin,
                                           MHD_DAEMON_INFO_CURRENT_CONNECTIONS)->num_connections) ) )
        sleep (1);
      /* Now we're really done, practice clean shutdown */
      MHD_stop_daemon (mhd);
      if (NULL != mhd_admin)
        MHD_stop_daemon (mhd_admin);
    }
    break;
  default:
    GNUNET_break (0);
    MHD_stop_daemon (mhd);
    if (NULL != mhd_admin)
      MHD_stop_daemon (mhd_admin);
    break;
  }
  TALER_EXCHANGEDB_plugin_unload (TMH_plugin);
  TMH_VALIDATION_done ();
  return (GNUNET_SYSERR == ret) ? 1 : 0;
}

/* end of taler-exchange-httpd.c */
