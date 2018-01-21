/*
  This file is part of TALER
  Copyright (C) 2014-2018 Taler Systems SA

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
 * @file exchange/test_exchange_api_new.c
 * @brief testcase to test exchange's HTTP API interface
 * @author Sree Harsha Totakura <sreeharsha@totakura.in>
 * @author Christian Grothoff
 */
#include "platform.h"
#include "taler_util.h"
#include "taler_signatures.h"
#include "taler_exchange_service.h"
#include "taler_json_lib.h"
#include <gnunet/gnunet_util_lib.h>
#include <microhttpd.h>
#include "taler_bank_service.h"
#include "taler_fakebank_lib.h"
#include "taler_testing_lib.h"


/**
 * Is the configuration file is set to include wire format 'test'?
 */
#define WIRE_TEST 1

/**
 * Is the configuration file is set to include wire format 'sepa'?
 */
#define WIRE_SEPA 1

/**
 * Account number of the exchange at the bank.
 */
#define EXCHANGE_ACCOUNT_NO 2

/**
 * Account number of some user.
 */
#define USER_ACCOUNT_NO 42

/**
 *
 */
#define USER_LOGIN_NAME "user42"

/**
 *
 */
#define USER_LOGIN_PASS "pass42"

/**
 *
 */
#define FAKEBANK_URL "http://localhost:8082/"

/**
 *
 */
#define CONFIG_FILE "test_exchange_api.conf"

/**
 *
 */
#define CMD_EXEC_WIREWATCH(label) \
   TALER_TESTING_cmd_exec_wirewatch (label, CONFIG_FILE)

/**
 *
 */
#define CMD_TRANSFER_TO_EXCHANGE(label,amount) \
   TALER_TESTING_cmd_fakebank_transfer (label, amount, \
     FAKEBANK_URL, USER_ACCOUNT_NO, EXCHANGE_ACCOUNT_NO, \
     USER_LOGIN_NAME, USER_LOGIN_PASS)



/**
 * Main function that will tell the interpreter what commands to run.
 *
 * @param cls closure
 */
static void
run (void *cls,
     struct TALER_TESTING_Interpreter *is)
{
  struct TALER_TESTING_Command commands[] = {
    CMD_TRANSFER_TO_EXCHANGE ("create-reserve-1",
                              "EUR:5.01"),
    CMD_EXEC_WIREWATCH ("exec-wirewatch-1"),
    TALER_TESTING_cmd_end ()
  };

  TALER_TESTING_run_with_fakebank (is,
                                   commands,
                                   FAKEBANK_URL);
}


int
main (int argc,
      char * const *argv)
{
  /* These environment variables get in the way... */
  unsetenv ("XDG_DATA_HOME");
  unsetenv ("XDG_CONFIG_HOME");
  GNUNET_log_setup ("test-exchange-api-new",
                    "INFO",
                    NULL);
  if (GNUNET_OK !=
      TALER_TESTING_url_port_free (FAKEBANK_URL))
    return 77;
  TALER_TESTING_cleanup_files (CONFIG_FILE);
  switch (TALER_TESTING_prepare_exchange (CONFIG_FILE))
  {
  case GNUNET_SYSERR:
    GNUNET_break (0);
    return 1;
  case GNUNET_NO:
    return 77;
  case GNUNET_OK:
    if (GNUNET_OK !=
        TALER_TESTING_setup_with_exchange (&run,
                                           NULL,
                                           CONFIG_FILE))
      return 1;
  default:
    GNUNET_break (0);
    return 1;
  }
  return 0;
}

/* end of test_exchange_api_new.c */
