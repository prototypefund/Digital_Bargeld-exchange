/*
  This file is part of TALER
  Copyright (C) 2018-2020 Taler Systems SA

  TALER is free software; you can redistribute it and/or modify it
  under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 3, or (at your
  option) any later version.

  TALER is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  General Public License for more details.

  You should have received a copy of the GNU General Public
  License along with TALER; see the file COPYING.  If not, see
  <http://www.gnu.org/licenses/>
*/
/**
 * @file testing/testing_api_cmd_withdraw.c
 * @brief main interpreter loop for testcases
 * @author Christian Grothoff
 * @author Marcello Stanisci
 */
#include "platform.h"
#include "taler_json_lib.h"
#include <microhttpd.h>
#include <gnunet/gnunet_curl_lib.h>
#include "taler_signatures.h"
#include "taler_testing_lib.h"
#include "backoff.h"


/**
 * How often do we retry before giving up?
 */
#define NUM_RETRIES 15

/**
 * How long do we wait AT LEAST if the exchange says the reserve is unknown?
 */
#define UNKNOWN_MIN_BACKOFF GNUNET_TIME_relative_multiply ( \
    GNUNET_TIME_UNIT_MILLISECONDS, 10)

/**
 * How long do we wait AT MOST if the exchange says the reserve is unknown?
 */
#define UNKNOWN_MAX_BACKOFF GNUNET_TIME_relative_multiply ( \
    GNUNET_TIME_UNIT_MILLISECONDS, 100)

/**
 * State for a "withdraw" CMD.
 */
struct WithdrawState
{

  /**
   * Which reserve should we withdraw from?
   */
  const char *reserve_reference;

  /**
   * String describing the denomination value we should withdraw.
   * A corresponding denomination key must exist in the exchange's
   * offerings.  Can be NULL if @e pk is set instead.
   */
  struct TALER_Amount amount;

  /**
   * If @e amount is NULL, this specifies the denomination key to
   * use.  Otherwise, this will be set (by the interpreter) to the
   * denomination PK matching @e amount.
   */
  struct TALER_EXCHANGE_DenomPublicKey *pk;

  /**
   * Exchange base URL.  Only used as offered trait.
   */
  char *exchange_url;

  /**
   * Interpreter state (during command).
   */
  struct TALER_TESTING_Interpreter *is;

  /**
   * Set (by the interpreter) to the exchange's signature over the
   * coin's public key.
   */
  struct TALER_DenominationSignature sig;

  /**
   * Private key material of the coin, set by the interpreter.
   */
  struct TALER_PlanchetSecretsP ps;

  /**
   * Withdraw handle (while operation is running).
   */
  struct TALER_EXCHANGE_WithdrawHandle *wsh;

  /**
   * Task scheduled to try later.
   */
  struct GNUNET_SCHEDULER_Task *retry_task;

  /**
   * How long do we wait until we retry?
   */
  struct GNUNET_TIME_Relative backoff;

  /**
   * Total withdraw backoff applied.
   */
  struct GNUNET_TIME_Relative total_backoff;

  /**
   * Expected HTTP response code to the request.
   */
  unsigned int expected_response_code;

  /**
   * Was this command modified via
   * #TALER_TESTING_cmd_withdraw_with_retry to
   * enable retries? How often should we still retry?
   */
  unsigned int do_retry;
};


/**
 * Run the command.
 *
 * @param cls closure.
 * @param cmd the commaind being run.
 * @param is interpreter state.
 */
static void
withdraw_run (void *cls,
              const struct TALER_TESTING_Command *cmd,
              struct TALER_TESTING_Interpreter *is);


/**
 * Task scheduled to re-try #withdraw_run.
 *
 * @param cls a `struct WithdrawState`
 */
static void
do_retry (void *cls)
{
  struct WithdrawState *ws = cls;

  ws->retry_task = NULL;
  ws->is->commands[ws->is->ip].last_req_time
    = GNUNET_TIME_absolute_get ();
  withdraw_run (ws,
                NULL,
                ws->is);
}


/**
 * "reserve withdraw" operation callback; checks that the
 * response code is expected and store the exchange signature
 * in the state.
 *
 * @param cls closure.
 * @param http_status HTTP response code.
 * @param ec taler-specific error code.
 * @param sig signature over the coin, NULL on error.
 * @param full_response raw response.
 */
static void
reserve_withdraw_cb (void *cls,
                     unsigned int http_status,
                     enum TALER_ErrorCode ec,
                     const struct TALER_DenominationSignature *sig,
                     const json_t *full_response)
{
  struct WithdrawState *ws = cls;
  struct TALER_TESTING_Interpreter *is = ws->is;

  ws->wsh = NULL;
  if (ws->expected_response_code != http_status)
  {
    if (0 != ws->do_retry)
    {
      if (TALER_EC_WITHDRAW_RESERVE_UNKNOWN != ec)
        ws->do_retry--; /* we don't count reserve unknown as failures here */
      if ( (0 == http_status) ||
           (TALER_EC_DB_COMMIT_FAILED_ON_RETRY == ec) ||
           (TALER_EC_WITHDRAW_INSUFFICIENT_FUNDS == ec) ||
           (TALER_EC_WITHDRAW_RESERVE_UNKNOWN == ec) ||
           (MHD_HTTP_INTERNAL_SERVER_ERROR == http_status) )
      {
        GNUNET_log (GNUNET_ERROR_TYPE_INFO,
                    "Retrying withdraw failed with %u/%d\n",
                    http_status,
                    (int) ec);
        /* on DB conflicts, do not use backoff */
        if (TALER_EC_DB_COMMIT_FAILED_ON_RETRY == ec)
          ws->backoff = GNUNET_TIME_UNIT_ZERO;
        else if (TALER_EC_WITHDRAW_RESERVE_UNKNOWN != ec)
          ws->backoff = EXCHANGE_LIB_BACKOFF (ws->backoff);
        else
          ws->backoff = GNUNET_TIME_relative_max (UNKNOWN_MIN_BACKOFF,
                                                  ws->backoff);
        ws->backoff = GNUNET_TIME_relative_min (ws->backoff,
                                                UNKNOWN_MAX_BACKOFF);
        ws->total_backoff = GNUNET_TIME_relative_add (ws->total_backoff,
                                                      ws->backoff);
        ws->is->commands[ws->is->ip].num_tries++;
        ws->retry_task = GNUNET_SCHEDULER_add_delayed (ws->backoff,
                                                       &do_retry,
                                                       ws);
        return;
      }
    }
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Unexpected response code %u/%d to command %s in %s:%u\n",
                http_status,
                (int) ec,
                TALER_TESTING_interpreter_get_current_label (is),
                __FILE__,
                __LINE__);
    json_dumpf (full_response,
                stderr,
                0);
    GNUNET_break (0);
    TALER_TESTING_interpreter_fail (is);
    return;
  }
  switch (http_status)
  {
  case MHD_HTTP_OK:
    if (NULL == sig)
    {
      GNUNET_break (0);
      TALER_TESTING_interpreter_fail (is);
      return;
    }
    ws->sig.rsa_signature
      = GNUNET_CRYPTO_rsa_signature_dup (sig->rsa_signature);
    if (0 != ws->total_backoff.rel_value_us)
    {
      GNUNET_log (GNUNET_ERROR_TYPE_INFO,
                  "Total withdraw backoff for %s was %s\n",
                  is->commands[is->ip].label,
                  GNUNET_STRINGS_relative_time_to_string (ws->total_backoff,
                                                          GNUNET_YES));
    }
    break;
  case MHD_HTTP_FORBIDDEN:
    /* nothing to check */
    break;
  case MHD_HTTP_CONFLICT:
    /* nothing to check */
    break;
  case MHD_HTTP_NOT_FOUND:
    /* nothing to check */
    break;
  default:
    /* Unsupported status code (by test harness) */
    GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                "Withdraw test command does not support status code %u\n",
                http_status);
    GNUNET_break (0);
    break;
  }
  TALER_TESTING_interpreter_next (is);
}


/**
 * Run the command.
 */
static void
withdraw_run (void *cls,
              const struct TALER_TESTING_Command *cmd,
              struct TALER_TESTING_Interpreter *is)
{
  struct WithdrawState *ws = cls;
  const struct TALER_ReservePrivateKeyP *rp;
  const struct TALER_TESTING_Command *create_reserve;
  const struct TALER_EXCHANGE_DenomPublicKey *dpk;

  (void) cmd;
  create_reserve = TALER_TESTING_interpreter_lookup_command
                     (is, ws->reserve_reference);
  if (NULL == create_reserve)
  {
    GNUNET_break (0);
    TALER_TESTING_interpreter_fail (is);
    return;
  }
  if (GNUNET_OK !=
      TALER_TESTING_get_trait_reserve_priv (create_reserve,
                                            0,
                                            &rp))
  {
    GNUNET_break (0);
    TALER_TESTING_interpreter_fail (is);
    return;
  }
  TALER_planchet_setup_random (&ws->ps);
  ws->is = is;
  if (NULL == ws->pk)
  {
    dpk = TALER_TESTING_find_pk (TALER_EXCHANGE_get_keys (is->exchange),
                                 &ws->amount);
    if (NULL == dpk)
    {
      GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                  "Failed to determine denomination key at %s\n",
                  (NULL != cmd) ? cmd->label : "<retried command>");
      GNUNET_break (0);
      TALER_TESTING_interpreter_fail (is);
      return;
    }
    /* We copy the denomination key, as re-querying /keys
     * would free the old one. */
    ws->pk = TALER_EXCHANGE_copy_denomination_key (dpk);
  }
  ws->wsh = TALER_EXCHANGE_withdraw (is->exchange,
                                     ws->pk,
                                     rp,
                                     &ws->ps,
                                     &reserve_withdraw_cb,
                                     ws);
  if (NULL == ws->wsh)
  {
    GNUNET_break (0);
    TALER_TESTING_interpreter_fail (is);
    return;
  }
}


/**
 * Free the state of a "withdraw" CMD, and possibly cancel
 * a pending operation thereof.
 *
 * @param cls closure.
 * @param cmd the command being freed.
 */
static void
withdraw_cleanup (void *cls,
                  const struct TALER_TESTING_Command *cmd)
{
  struct WithdrawState *ws = cls;

  if (NULL != ws->wsh)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                "Command %s did not complete\n",
                cmd->label);
    TALER_EXCHANGE_withdraw_cancel (ws->wsh);
    ws->wsh = NULL;
  }
  if (NULL != ws->retry_task)
  {
    GNUNET_SCHEDULER_cancel (ws->retry_task);
    ws->retry_task = NULL;
  }
  if (NULL != ws->sig.rsa_signature)
  {
    GNUNET_CRYPTO_rsa_signature_free (ws->sig.rsa_signature);
    ws->sig.rsa_signature = NULL;
  }
  if (NULL != ws->pk)
  {
    TALER_EXCHANGE_destroy_denomination_key (ws->pk);
    ws->pk = NULL;
  }
  GNUNET_free_non_null (ws->exchange_url);
  GNUNET_free (ws);
}


/**
 * Offer internal data to a "withdraw" CMD state to other
 * commands.
 *
 * @param cls closure
 * @param[out] ret result (could be anything)
 * @param trait name of the trait
 * @param index index number of the object to offer.
 * @return #GNUNET_OK on success
 */
static int
withdraw_traits (void *cls,
                 const void **ret,
                 const char *trait,
                 unsigned int index)
{
  struct WithdrawState *ws = cls;
  const struct TALER_TESTING_Command *reserve_cmd;
  const struct TALER_ReservePrivateKeyP *reserve_priv;
  const struct TALER_ReservePublicKeyP *reserve_pub;

  /* We offer the reserve key where these coins were withdrawn
   * from. */
  reserve_cmd = TALER_TESTING_interpreter_lookup_command
                  (ws->is, ws->reserve_reference);

  if (NULL == reserve_cmd)
  {
    GNUNET_break (0);
    TALER_TESTING_interpreter_fail (ws->is);
    return GNUNET_SYSERR;
  }

  if (GNUNET_OK !=
      TALER_TESTING_get_trait_reserve_priv (reserve_cmd,
                                            0,
                                            &reserve_priv))
  {
    GNUNET_break (0);
    TALER_TESTING_interpreter_fail (ws->is);
    return GNUNET_SYSERR;
  }
  if (GNUNET_OK !=
      TALER_TESTING_get_trait_reserve_pub (reserve_cmd,
                                           0,
                                           &reserve_pub))
  {
    GNUNET_break (0);
    TALER_TESTING_interpreter_fail (ws->is);
    return GNUNET_SYSERR;
  }
  if (NULL == ws->exchange_url)
    ws->exchange_url
      = GNUNET_strdup (TALER_EXCHANGE_get_base_url (ws->is->exchange));
  {
    struct TALER_TESTING_Trait traits[] = {
      TALER_TESTING_make_trait_coin_priv (0 /* only one coin */,
                                          &ws->ps.coin_priv),
      TALER_TESTING_make_trait_blinding_key (0 /* only one coin */,
                                             &ws->ps.blinding_key),
      TALER_TESTING_make_trait_denom_pub (0 /* only one coin */,
                                          ws->pk),
      TALER_TESTING_make_trait_denom_sig (0 /* only one coin */,
                                          &ws->sig),
      TALER_TESTING_make_trait_reserve_priv (0,
                                             reserve_priv),
      TALER_TESTING_make_trait_reserve_pub (0,
                                            reserve_pub),
      TALER_TESTING_make_trait_amount_obj (0,
                                           &ws->amount),
      TALER_TESTING_make_trait_url (TALER_TESTING_UT_EXCHANGE_BASE_URL,
                                    ws->exchange_url),
      TALER_TESTING_trait_end ()
    };

    return TALER_TESTING_get_trait (traits,
                                    ret,
                                    trait,
                                    index);
  }
}


/**
 * Create a withdraw command, letting the caller specify
 * the desired amount as string.
 *
 * @param label command label.
 * @param reserve_reference command providing us with a reserve to withdraw from
 * @param amount how much we withdraw.
 * @param expected_response_code which HTTP response code
 *        we expect from the exchange.
 * @return the withdraw command to be executed by the interpreter.
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_withdraw_amount (const char *label,
                                   const char *reserve_reference,
                                   const char *amount,
                                   unsigned int expected_response_code)
{
  struct WithdrawState *ws;

  ws = GNUNET_new (struct WithdrawState);
  ws->reserve_reference = reserve_reference;
  if (GNUNET_OK !=
      TALER_string_to_amount (amount,
                              &ws->amount))
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Failed to parse amount `%s' at %s\n",
                amount,
                label);
    GNUNET_assert (0);
  }
  ws->expected_response_code = expected_response_code;
  {
    struct TALER_TESTING_Command cmd = {
      .cls = ws,
      .label = label,
      .run = &withdraw_run,
      .cleanup = &withdraw_cleanup,
      .traits = &withdraw_traits
    };

    return cmd;
  }
}


/**
 * Create withdraw command, letting the caller specify the
 * amount by a denomination key.
 *
 * @param label command label.
 * @param reserve_reference reference to the reserve to withdraw
 *        from; will provide reserve priv to sign the request.
 * @param dk denomination public key.
 * @param expected_response_code expected HTTP response code.
 *
 * @return the command.
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_withdraw_denomination (const char *label,
                                         const char *reserve_reference,
                                         const struct
                                         TALER_EXCHANGE_DenomPublicKey *dk,
                                         unsigned int expected_response_code)
{
  struct WithdrawState *ws;

  if (NULL == dk)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Denomination key not specified at %s\n",
                label);
    GNUNET_assert (0);
  }
  ws = GNUNET_new (struct WithdrawState);
  ws->reserve_reference = reserve_reference;
  ws->pk = TALER_EXCHANGE_copy_denomination_key (dk);
  ws->expected_response_code = expected_response_code;
  {
    struct TALER_TESTING_Command cmd = {
      .cls = ws,
      .label = label,
      .run = &withdraw_run,
      .cleanup = &withdraw_cleanup,
      .traits = &withdraw_traits
    };

    return cmd;
  }
}


/**
 * Modify a withdraw command to enable retries when the
 * reserve is not yet full or we get other transient
 * errors from the exchange.
 *
 * @param cmd a withdraw command
 * @return the command with retries enabled
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_withdraw_with_retry (struct TALER_TESTING_Command cmd)
{
  struct WithdrawState *ws;

  GNUNET_assert (&withdraw_run == cmd.run);
  ws = cmd.cls;
  ws->do_retry = NUM_RETRIES;
  return cmd;
}


/* end of testing_api_cmd_withdraw.c */
