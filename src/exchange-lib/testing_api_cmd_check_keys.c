/*
  This file is part of TALER
  (C) 2018 Taler Systems SA

  TALER is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as
  published by the Free Software Foundation; either version 3, or
  (at your option) any later version.

  TALER is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public
  License along with TALER; see the file COPYING.  If not, see
  <http://www.gnu.org/licenses/>
*/

/**
 * @file exchange-lib/testing_api_cmd_check_keys.c
 * @brief Implementation of "check keys" test command.
 * @author Marcello Stanisci
 */

#include "platform.h"
#include "taler_json_lib.h"
#include <gnunet/gnunet_curl_lib.h>
#include "exchange_api_handle.h"
#include "taler_testing_lib.h"


/**
 * State for a "check keys" CMD.
 */
struct CheckKeysState
{
  /**
   * How many times the /keys response was received by the
   * exchange under test.
   */
  unsigned int generation;

  /**
   * How many denomination keys the exchange is supposed to
   * have.
   */
  unsigned int num_denom_keys;
};


/**
 * Run the "check keys" command.
 *
 * @param cls closure.
 * @param cmd the command currently being executed.
 * @param is the interpreter state.
 */
static void
check_keys_run (void *cls,
                const struct TALER_TESTING_Command *cmd,
                struct TALER_TESTING_Interpreter *is)
{
  struct CheckKeysState *cks = cls;

  GNUNET_log (GNUNET_ERROR_TYPE_INFO,
              "cmd `%s', key generation: %d\n",
              cmd->label,
              is->key_generation);
  if (is->key_generation < cks->generation)
  {
    /* Go back to waiting for /keys signal! */
    is->working = GNUNET_NO;
    GNUNET_log (GNUNET_ERROR_TYPE_INFO,
                "Triggering /keys dl, cmd `%s'\n",
                cmd->label);

    /* Means re-download /keys.  */
    GNUNET_break (0 == TALER_EXCHANGE_check_keys_current
      (is->exchange, GNUNET_YES).abs_value_us);
    return;
  }
  if (is->key_generation > cks->generation)
  {
    /* We got /keys too often, strange. Fatal. May theoretically
       happen if somehow we were really unlucky and /keys expired
       "naturally", but obviously with a sane configuration this
       should also not be. */
    GNUNET_break (0);
    TALER_TESTING_interpreter_fail (is);
    return;
  }
  /* /keys was updated, let's check they were OK! */
  if (cks->num_denom_keys != is->keys->num_denom_keys)
  {
    /* Did not get the expected number of denomination keys! */
    GNUNET_break (0);
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Got %u keys in step %s, expected %u\n",
                is->keys->num_denom_keys,
                cmd->label,
                cks->num_denom_keys);
    TALER_TESTING_interpreter_fail (is);
    return;
  }
  TALER_TESTING_interpreter_next (is);
}


/**
 * Cleanup the state.
 *
 * @param cls closure.
 * @param cmd the command which is being cleaned up.
 */
static void
check_keys_cleanup (void *cls,
                    const struct TALER_TESTING_Command *cmd)
{
  struct CheckKeysState *cks = cls;

  GNUNET_free (cks);
}


/**
 * Make a "check keys" command.  This type of command
 * checks whether the number of denomination keys from
 * @a exchange matches @a num_denom_keys.
 *
 * @param label command label
 * @param generation how many /keys responses are expected to
 *        have been returned when this CMD will be run.
 * @param num_denom_keys expected number of denomination keys.
 * @param exchange connection handle to the exchange to test.
 *
 * @return the command.
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_check_keys
  (const char *label,
   unsigned int generation,
   unsigned int num_denom_keys)
{
  struct CheckKeysState *cks;
  struct TALER_TESTING_Command cmd;

  cks = GNUNET_new (struct CheckKeysState);
  cks->generation = generation;
  cks->num_denom_keys = num_denom_keys;
  cmd.cls = cks;
  cmd.label = label;
  cmd.run = &check_keys_run;
  cmd.cleanup = &check_keys_cleanup;
  return cmd;
}

/* end of testing_api_cmd_check_keys.c */
