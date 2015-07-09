/*
  This file is part of TALER
  Copyright (C) 2014, 2015 Christian Grothoff (and other contributing authors)

  TALER is free software; you can redistribute it and/or modify it under the
  terms of the GNU General Public License as published by the Free Software
  Foundation; either version 3, or (at your option) any later version.

  TALER is distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
  A PARTICULAR PURPOSE.  See the GNU General Public License for more details.

  You should have received a copy of the GNU General Public License along with
  TALER; see the file COPYING.  If not, If not, see <http://www.gnu.org/licenses/>
*/

/**
 * @file mint/test_mint_api.c
 * @brief testcase to test mint's HTTP API interface
 * @author Sree Harsha Totakura <sreeharsha@totakura.in>
 * @author Christian Grothoff
 *
 * TODO:
 * - enhance interpreter to allow for testing of failure conditions
 *   (i.e. double-spending, insufficient funds on withdraw)
 * - add checks for /withdraw/status
 */
#include "platform.h"
#include "taler_util.h"
#include "taler_signatures.h"
#include "taler_mint_service.h"
#include <gnunet/gnunet_util_lib.h>
#include <microhttpd.h>


/**
 * Main execution context for the main loop.
 */
static struct TALER_MINT_Context *ctx;

/**
 * Handle to access the mint.
 */
static struct TALER_MINT_Handle *mint;

/**
 * Task run on shutdown.
 */
static struct GNUNET_SCHEDULER_Task *shutdown_task;

/**
 * Task that runs the main event loop.
 */
static struct GNUNET_SCHEDULER_Task *ctx_task;

/**
 * Result of the testcases, #GNUNET_OK on success
 */
static int result;


/**
 * Opcodes for the interpreter.
 */
enum OpCode
{
  /**
   * Termination code, stops the interpreter loop (with success).
   */
  OC_END = 0,

  /**
   * Add funds to a reserve by (faking) incoming wire transfer.
   */
  OC_ADMIN_ADD_INCOMING,

  /**
   * Check status of a reserve.
   */
  OC_WITHDRAW_STATUS,

  /**
   * Withdraw a coin from a reserve.
   */
  OC_WITHDRAW_SIGN,

  /**
   * Deposit a coin (pay with it).
   */
  OC_DEPOSIT

};


/**
 * Details for a mint operation to execute.
 */
struct Command
{
  /**
   * Opcode of the command.
   */
  enum OpCode oc;

  /**
   * Label for the command, can be NULL.
   */
  const char *label;

  /**
   * Details about the command.
   */
  union
  {

    struct
    {

      /**
       * Label to another admin_add_incoming command if we
       * should deposit into an existing reserve, NULL if
       * a fresh reserve should be created.
       */
      const char *reserve_reference;

      /**
       * String describing the amount to add to the reserve.
       */
      const char *amount;

      /**
       * Wire details (JSON).
       */
      const char *wire;

      /**
       * Set (by the interpreter) to the reserve's private key
       * we used to fill the reserve.
       */
      struct TALER_ReservePrivateKeyP reserve_priv;

      /**
       * Set to the API's handle during the operation.
       */
      struct TALER_MINT_AdminAddIncomingHandle *aih;

    } admin_add_incoming;

    struct
    {

      /**
       * Label to the #OC_ADMIN_ADD_INCOMING command which
       * created the reserve.
       */
      const char *reserve_reference;

      /**
       * Set to the API's handle during the operation.
       */
      struct TALER_MINT_WithdrawStatusHandle *wsh;

    } withdraw_status;

    struct
    {
      /**
       * Which reserve should we withdraw from?
       */
      const char *reserve_reference;

      /**
       * String describing the denomination value we should withdraw.
       * A corresponding denomination key must exist in the mint's
       * offerings.  Can be NULL if @e pk is set instead.
       */
      const char *amount;

      /**
       * If @e amount is NULL, this specifies the denomination key to
       * use.  Otherwise, this will be set (by the interpreter) to the
       * denomination PK matching @e amount.
       */
      const struct TALER_MINT_DenomPublicKey *pk;

      /**
       * Set (by the interpreter) to the mint's signature over the
       * coin's public key.
       */
      struct TALER_DenominationSignature sig;

      /**
       * Set (by the interpreter) to the coin's private key.
       */
      struct TALER_CoinSpendPrivateKeyP coin_priv;

      /**
       * Blinding key used for the operation.
       */
      struct TALER_DenominationBlindingKey blinding_key;

      /**
       * Withdraw handle (while operation is running).
       */
      struct TALER_MINT_WithdrawSignHandle *wsh;

    } withdraw_sign;

    struct
    {

      /**
       * Amount to deposit.
       */
      const char *amount;

      /**
       * Reference to a withdraw_sign operation for a coin to
       * be used for the /deposit operation.
       */
      const char *coin_ref;

      /**
       * JSON string describing the merchant's "wire details".
       */
      const char *wire_details;

      /**
       * JSON string describing the contract between the two parties.
       */
      const char *contract;

      /**
       * Transaction ID to use.
       */
      uint64_t transaction_id;

      /**
       * Relative time (to add to 'now') to compute the refund deadline.
       * Zero for no refunds.
       */
      struct GNUNET_TIME_Relative refund_deadline;

      /**
       * Set (by the interpreter) to a fresh private key of the merchant,
       * if @e refund_deadline is non-zero.
       */
      struct TALER_MerchantPrivateKeyP merchant_priv;

      /**
       * Deposit handle while operation is running.
       */
      struct TALER_MINT_DepositHandle *dh;

    } deposit;

  } details;

};


/**
 * State of the interpreter loop.
 */
struct InterpreterState
{
  /**
   * Keys from the mint.
   */
  const struct TALER_MINT_Keys *keys;

  /**
   * Commands the interpreter will run.
   */
  struct Command *commands;

  /**
   * Interpreter task (if one is scheduled).
   */
  struct GNUNET_SCHEDULER_Task *task;

  /**
   * Instruction pointer.  Tells #interpreter_run() which
   * instruction to run next.
   */
  unsigned int ip;

};


/**
 * Task that runs the context's event loop with the GNUnet scheduler.
 *
 * @param cls unused
 * @param tc scheduler context (unused)
 */
static void
context_task (void *cls,
              const struct GNUNET_SCHEDULER_TaskContext *tc);


/**
 * Run the context task, the working set has changed.
 */
static void
trigger_context_task ()
{
  GNUNET_SCHEDULER_cancel (ctx_task);
  ctx_task = GNUNET_SCHEDULER_add_now (&context_task,
                                       NULL);
}


/**
 * The testcase failed, return with an error code.
 *
 * @param is interpreter state to clean up
 */
static void
fail (struct InterpreterState *is)
{
  result = GNUNET_SYSERR;
  GNUNET_SCHEDULER_shutdown ();
}


/**
 * Find a command by label.
 *
 * @param is interpreter state to search
 * @param label label to look for
 * @return NULL if command was not found
 */
static const struct Command *
find_command (const struct InterpreterState *is,
              const char *label)
{
  unsigned int i;
  const struct Command *cmd;

  if (NULL == label)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                "Attempt to lookup command for empty label\n");
    return NULL;
  }
  for (i=0;OC_END != (cmd = &is->commands[i])->oc;i++)
    if ( (NULL != cmd->label) &&
         (0 == strcmp (cmd->label,
                       label)) )
      return cmd;
  GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
              "Command not found: %s\n",
              label);
  return NULL;
}


/**
 * Run the main interpreter loop that performs mint operations.
 *
 * @param cls contains the `struct InterpreterState`
 * @param tc scheduler context
 */
static void
interpreter_run (void *cls,
                 const struct GNUNET_SCHEDULER_TaskContext *tc);


/**
 * Function called upon completion of our /admin/add/incoming request.
 *
 * @param cls closure with the interpreter state
 * @param http_status HTTP response code, #MHD_HTTP_OK (200) for successful status request
 *                    0 if the mint's reply is bogus (fails to follow the protocol)
 * @param full_response full response from the mint (for logging, in case of errors)
 */
static void
add_incoming_cb (void *cls,
                 unsigned int http_status,
                 json_t *full_response)
{
  struct InterpreterState *is = cls;
  struct Command *cmd = &is->commands[is->ip];

  cmd->details.admin_add_incoming.aih = NULL;
  if (MHD_HTTP_OK != http_status)
  {
    GNUNET_break (0);
    fail (is);
    return;
  }
  is->ip++;
  is->task = GNUNET_SCHEDULER_add_now (&interpreter_run,
                                       is);
}


/**
 * Check if the given historic event @a h corresponds to the given
 * command @a cmd.
 *
 * @param h event in history
 * @param cmd an #OC_ADMIN_ADD_INCOMING command
 * @return #GNUNET_OK if they match, #GNUNET_SYSERR if not
 */
static int
compare_admin_add_incoming_history (const struct TALER_MINT_ReserveHistory *h,
                                    const struct Command *cmd)
{
  struct TALER_Amount amount;

  if (TALER_MINT_RTT_DEPOSIT != h->type)
  {
    GNUNET_break_op (0);
    return GNUNET_SYSERR;
  }
  GNUNET_assert (GNUNET_OK ==
                 TALER_string_to_amount (cmd->details.admin_add_incoming.amount,
                                         &amount));
  if (0 != TALER_amount_cmp (&amount,
                             &h->amount))
  {
    GNUNET_break_op (0);
    return GNUNET_SYSERR;
  }
  return GNUNET_OK;
}


/**
 * Check if the given historic event @a h corresponds to the given
 * command @a cmd.
 *
 * @param h event in history
 * @param cmd an #OC_WITHDRAW_SIGN command
 * @return #GNUNET_OK if they match, #GNUNET_SYSERR if not
 */
static int
compare_withdraw_sign_history (const struct TALER_MINT_ReserveHistory *h,
                               const struct Command *cmd)
{
  struct TALER_Amount amount;

  if (TALER_MINT_RTT_WITHDRAWAL != h->type)
  {
    GNUNET_break_op (0);
    return GNUNET_SYSERR;
  }
  GNUNET_assert (GNUNET_OK ==
                 TALER_string_to_amount (cmd->details.withdraw_sign.amount,
                                         &amount));
  // FIXME: should fail (amount with vs. amount without fee!)
  if (0 != TALER_amount_cmp (&amount,
                             &h->amount))
  {
    GNUNET_break_op (0);
    return GNUNET_OK; /* FIXME: returning OK for now, as the above
                         fails due to fee/no-fee mismatch */
  }
  return GNUNET_OK;
}


/**
 * Function called with the result of a /withdraw/status request.
 *
 * @param cls closure with the interpreter state
 * @param http_status HTTP response code, #MHD_HTTP_OK (200) for successful status request
 *                    0 if the mint's reply is bogus (fails to follow the protocol)
 * @param[in] json original response in JSON format (useful only for diagnostics)
 * @param balance current balance in the reserve, NULL on error
 * @param history_length number of entries in the transaction history, 0 on error
 * @param history detailed transaction history, NULL on error
 */
static void
withdraw_status_cb (void *cls,
                    unsigned int http_status,
                    json_t *json,
                    const struct TALER_Amount *balance,
                    unsigned int history_length,
                    const struct TALER_MINT_ReserveHistory *history)
{
  struct InterpreterState *is = cls;
  struct Command *cmd = &is->commands[is->ip];
  struct Command *rel;
  unsigned int i;
  unsigned int j;

  cmd->details.withdraw_status.wsh = NULL;
  if (MHD_HTTP_OK != http_status)
  {
    GNUNET_break (0);
    fail (is);
    return;
  }
  /* FIXME: note that history events may come in a different
     order than the commands right now... */
  j = 0;
  for (i=0;i<is->ip;i++)
  {
    switch ((rel = &is->commands[i])->oc)
    {
    case OC_ADMIN_ADD_INCOMING:
      if ( ( (NULL != rel->label) &&
             (0 == strcmp (cmd->details.withdraw_status.reserve_reference,
                           rel->label) ) ) ||
           ( (NULL != rel->details.admin_add_incoming.reserve_reference) &&
             (0 == strcmp (cmd->details.withdraw_status.reserve_reference,
                           rel->details.admin_add_incoming.reserve_reference) ) ) )
      {
        if (GNUNET_OK !=
            compare_admin_add_incoming_history (&history[j],
                                                rel))
        {
          GNUNET_break (0);
          fail (is);
          return;
        }
        j++;
      }
      break;
    case OC_WITHDRAW_SIGN:
      if (0 == strcmp (cmd->details.withdraw_status.reserve_reference,
                       rel->details.withdraw_sign.reserve_reference))
      {
        if (GNUNET_OK !=
            compare_withdraw_sign_history (&history[j],
                                           rel))
        {
          GNUNET_break (0);
          fail (is);
          return;
        }
        j++;
      }
      break;
    default:
      /* unreleated, just skip */
      break;
    }
  }
  if (j != history_length)
  {
    GNUNET_break (0);
    fail (is);
    return;
  }
  /* FIXME: check the amount... */

  is->ip++;
  is->task = GNUNET_SCHEDULER_add_now (&interpreter_run,
                                       is);
}


/**
 * Function called upon completion of our /withdraw/sign request.
 *
 * @param cls closure with the interpreter state
 * @param http_status HTTP response code, #MHD_HTTP_OK (200) for successful status request
 *                    0 if the mint's reply is bogus (fails to follow the protocol)
 * @param sig signature over the coin, NULL on error
 * @param full_response full response from the mint (for logging, in case of errors)
 */
static void
withdraw_sign_cb (void *cls,
                  unsigned int http_status,
                  const struct TALER_DenominationSignature *sig,
                  json_t *full_response)
{
  struct InterpreterState *is = cls;
  struct Command *cmd = &is->commands[is->ip];

  cmd->details.withdraw_sign.wsh = NULL;
  if (NULL == sig)
  {
    GNUNET_break (0);
    fail (is);
    return;
  }
  cmd->details.withdraw_sign.sig.rsa_signature
    = GNUNET_CRYPTO_rsa_signature_dup (sig->rsa_signature);
  is->ip++;
  is->task = GNUNET_SCHEDULER_add_now (&interpreter_run,
                                       is);
}


/**
 * Function called with the result of a /deposit operation.
 *
 * @param cls closure with the interpreter state
 * @param http_status HTTP response code, #MHD_HTTP_OK (200) for successful deposit;
 *                    0 if the mint's reply is bogus (fails to follow the protocol)
 * @param obj the received JSON reply, should be kept as proof (and, in case of errors,
 *            be forwarded to the customer)
 */
static void
deposit_cb (void *cls,
            unsigned int http_status,
            json_t *obj)
{
  struct InterpreterState *is = cls;
  struct Command *cmd = &is->commands[is->ip];

  cmd->details.deposit.dh = NULL;
  if (MHD_HTTP_OK != http_status)
  {
    GNUNET_break (0);
    fail (is);
    return;
  }
  is->ip++;
  is->task = GNUNET_SCHEDULER_add_now (&interpreter_run,
                                       is);

}


/**
 * Find denomination key matching the given amount.
 *
 * @param keys array of keys to search
 * @param amount coin value to look for
 * @return NULL if no matching key was found
 */
static const struct TALER_MINT_DenomPublicKey *
find_pk (const struct TALER_MINT_Keys *keys,
         const struct TALER_Amount *amount)
{
  unsigned int i;
  struct GNUNET_TIME_Absolute now;
  struct TALER_MINT_DenomPublicKey *pk;
  char *str;

  now = GNUNET_TIME_absolute_get ();
  for (i=0;i<keys->num_denom_keys;i++)
  {
    pk = &keys->denom_keys[i];
    if ( (0 == TALER_amount_cmp (amount,
                                 &pk->value)) &&
         (now.abs_value_us >= pk->valid_from.abs_value_us) &&
         (now.abs_value_us < pk->withdraw_valid_until.abs_value_us) )
      return pk;
  }
  /* do 2nd pass to check if expiration times are to blame for failure */
  str = TALER_amount_to_string (amount);
  for (i=0;i<keys->num_denom_keys;i++)
  {
    if ( (0 == TALER_amount_cmp (amount,
                                 &pk->value)) &&
         ( (now.abs_value_us < pk->valid_from.abs_value_us) ||
           (now.abs_value_us > pk->withdraw_valid_until.abs_value_us) ) )
    {
      GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                  "Have denomination key for `%s', but with wrong expiration range %llu vs [%llu,%llu)\n",
                  str,
                  now.abs_value_us,
                  pk->valid_from.abs_value_us,
                  pk->withdraw_valid_until.abs_value_us);
      GNUNET_free (str);
      return NULL;
    }
  }
  GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
              "No denomination key for amount %s found\n",
              str);
  GNUNET_free (str);
  return NULL;
}


/**
 * Run the main interpreter loop that performs mint operations.
 *
 * @param cls contains the `struct InterpreterState`
 * @param tc scheduler context
 */
static void
interpreter_run (void *cls,
                 const struct GNUNET_SCHEDULER_TaskContext *tc)
{
  struct InterpreterState *is = cls;
  struct Command *cmd = &is->commands[is->ip];
  const struct Command *ref;
  struct TALER_ReservePublicKeyP reserve_pub;
  struct TALER_CoinSpendPublicKeyP coin_pub;
  struct TALER_Amount amount;
  struct GNUNET_TIME_Absolute execution_date;
  json_t *wire;

  is->task = NULL;
  if (0 != (tc->reason & GNUNET_SCHEDULER_REASON_SHUTDOWN))
  {
    fprintf (stderr,
             "Test aborted by shutdown request\n");
    fail (is);
    return;
  }
  switch (cmd->oc)
  {
  case OC_END:
    result = GNUNET_OK;
    GNUNET_SCHEDULER_shutdown ();
    return;
  case OC_ADMIN_ADD_INCOMING:
    if (NULL !=
        cmd->details.admin_add_incoming.reserve_reference)
    {
      ref = find_command (is,
                          cmd->details.admin_add_incoming.reserve_reference);
      GNUNET_assert (NULL != ref);
      GNUNET_assert (OC_ADMIN_ADD_INCOMING == ref->oc);
      cmd->details.admin_add_incoming.reserve_priv
        = ref->details.admin_add_incoming.reserve_priv;
    }
    else
    {
      struct GNUNET_CRYPTO_EddsaPrivateKey *priv;

      priv = GNUNET_CRYPTO_eddsa_key_create ();
      cmd->details.admin_add_incoming.reserve_priv.eddsa_priv = *priv;
      GNUNET_free (priv);
    }
    GNUNET_CRYPTO_eddsa_key_get_public (&cmd->details.admin_add_incoming.reserve_priv.eddsa_priv,
                                        &reserve_pub.eddsa_pub);
    if (GNUNET_OK !=
        TALER_string_to_amount (cmd->details.admin_add_incoming.amount,
                                &amount))
    {
      GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                  "Failed to parse amount `%s' at %u\n",
                  cmd->details.admin_add_incoming.amount,
                  is->ip);
      fail (is);
      return;
    }
    wire = json_loads (cmd->details.admin_add_incoming.wire,
                       JSON_REJECT_DUPLICATES,
                       NULL);
    if (NULL == wire)
    {
      GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                  "Failed to parse wire details `%s' at %u\n",
                  cmd->details.admin_add_incoming.wire,
                  is->ip);
      fail (is);
      return;
    }
    execution_date = GNUNET_TIME_absolute_get ();
    TALER_round_abs_time (&execution_date);
    cmd->details.admin_add_incoming.aih
      = TALER_MINT_admin_add_incoming (mint,
                                       &reserve_pub,
                                       &amount,
                                       execution_date,
                                       wire,
                                       &add_incoming_cb,
                                       is);
    if (NULL == cmd->details.admin_add_incoming.aih)
    {
      GNUNET_break (0);
      fail (is);
      return;
    }
    trigger_context_task ();
    return;
  case OC_WITHDRAW_STATUS:
    GNUNET_assert (NULL !=
                   cmd->details.withdraw_status.reserve_reference);
    ref = find_command (is,
                        cmd->details.withdraw_status.reserve_reference);
    GNUNET_assert (NULL != ref);
    GNUNET_assert (OC_ADMIN_ADD_INCOMING == ref->oc);
    GNUNET_CRYPTO_eddsa_key_get_public (&ref->details.admin_add_incoming.reserve_priv.eddsa_priv,
                                        &reserve_pub.eddsa_pub);
    cmd->details.withdraw_status.wsh
      = TALER_MINT_withdraw_status (mint,
                                    &reserve_pub,
                                    &withdraw_status_cb,
                                    is);
    trigger_context_task ();
    return;
  case OC_WITHDRAW_SIGN:
    GNUNET_assert (NULL !=
                   cmd->details.withdraw_sign.reserve_reference);
    ref = find_command (is,
                        cmd->details.withdraw_sign.reserve_reference);
    GNUNET_assert (NULL != ref);
    GNUNET_assert (OC_ADMIN_ADD_INCOMING == ref->oc);
    if (NULL != cmd->details.withdraw_sign.amount)
    {
      if (GNUNET_OK !=
          TALER_string_to_amount (cmd->details.withdraw_sign.amount,
                                  &amount))
      {
        GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                    "Failed to parse amount `%s' at %u\n",
                    cmd->details.withdraw_sign.amount,
                    is->ip);
        fail (is);
        return;
      }
      cmd->details.withdraw_sign.pk = find_pk (is->keys,
                                               &amount);
    }
    if (NULL == cmd->details.withdraw_sign.pk)
    {
      GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                  "Failed to determine denomination key at %u\n",
                  is->ip);
      fail (is);
      return;
    }

    /* create coin's private key */
    {
      struct GNUNET_CRYPTO_EddsaPrivateKey *priv;

      priv = GNUNET_CRYPTO_eddsa_key_create ();
      cmd->details.withdraw_sign.coin_priv.eddsa_priv = *priv;
      GNUNET_free (priv);
    }
    GNUNET_CRYPTO_eddsa_key_get_public (&cmd->details.withdraw_sign.coin_priv.eddsa_priv,
                                        &coin_pub.eddsa_pub);
    cmd->details.withdraw_sign.blinding_key.rsa_blinding_key
      = GNUNET_CRYPTO_rsa_blinding_key_create (GNUNET_CRYPTO_rsa_public_key_len (cmd->details.withdraw_sign.pk->key.rsa_public_key));

    cmd->details.withdraw_sign.wsh
      = TALER_MINT_withdraw_sign (mint,
                                  cmd->details.withdraw_sign.pk,
                                  &ref->details.admin_add_incoming.reserve_priv,
                                  &cmd->details.withdraw_sign.coin_priv,
                                  &cmd->details.withdraw_sign.blinding_key,
                                  &withdraw_sign_cb,
                                  is);
    if (NULL == cmd->details.withdraw_sign.wsh)
    {
      GNUNET_break (0);
      fail (is);
      return;
    }
    trigger_context_task ();
    return;
  case OC_DEPOSIT:
    {
      struct GNUNET_HashCode h_contract;
      struct TALER_CoinSpendPublicKeyP coin_pub;
      struct TALER_CoinSpendSignatureP coin_sig;
      struct GNUNET_TIME_Absolute refund_deadline;
      struct GNUNET_TIME_Absolute timestamp;
      struct TALER_MerchantPublicKeyP merchant_pub;
      json_t *wire;

      GNUNET_assert (NULL !=
                     cmd->details.deposit.coin_ref);
      ref = find_command (is,
                          cmd->details.deposit.coin_ref);
      GNUNET_assert (NULL != ref);
      GNUNET_assert (OC_WITHDRAW_SIGN == ref->oc);
      if (GNUNET_OK !=
          TALER_string_to_amount (cmd->details.deposit.amount,
                                  &amount))
      {
        GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                    "Failed to parse amount `%s' at %u\n",
                    cmd->details.deposit.amount,
                    is->ip);
        fail (is);
        return;
      }
      GNUNET_CRYPTO_hash (cmd->details.deposit.contract,
                          strlen (cmd->details.deposit.contract),
                          &h_contract);
      wire = json_loads (cmd->details.deposit.wire_details,
                         JSON_REJECT_DUPLICATES,
                         NULL);
      if (NULL == wire)
      {
        GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                    "Failed to parse wire details `%s' at %u\n",
                    cmd->details.deposit.wire_details,
                    is->ip);
        fail (is);
        return;
      }
      GNUNET_CRYPTO_eddsa_key_get_public (&ref->details.withdraw_sign.coin_priv.eddsa_priv,
                                          &coin_pub.eddsa_pub);

      if (0 != cmd->details.deposit.refund_deadline.rel_value_us)
      {
        struct GNUNET_CRYPTO_EddsaPrivateKey *priv;

        priv = GNUNET_CRYPTO_eddsa_key_create ();
        cmd->details.deposit.merchant_priv.eddsa_priv = *priv;
        GNUNET_free (priv);
        refund_deadline = GNUNET_TIME_relative_to_absolute (cmd->details.deposit.refund_deadline);
      }
      else
      {
        refund_deadline = GNUNET_TIME_UNIT_ZERO_ABS;
      }
      timestamp = GNUNET_TIME_absolute_get ();
      TALER_round_abs_time (&timestamp);
      {
        struct TALER_DepositRequestPS dr;

        dr.purpose.size = htonl (sizeof (struct TALER_DepositRequestPS));
        dr.purpose.purpose = htonl (TALER_SIGNATURE_WALLET_COIN_DEPOSIT);
        dr.h_contract = h_contract;
        TALER_hash_json (wire,
                         &dr.h_wire);
        dr.timestamp = GNUNET_TIME_absolute_hton (timestamp);
        dr.refund_deadline = GNUNET_TIME_absolute_hton (refund_deadline);
        dr.transaction_id = GNUNET_htonll (cmd->details.deposit.transaction_id);
        TALER_amount_hton (&dr.amount_with_fee,
                           &amount);
        TALER_amount_hton (&dr.deposit_fee,
                           &ref->details.withdraw_sign.pk->fee_deposit);
        dr.merchant = merchant_pub;
        dr.coin_pub = coin_pub;
        GNUNET_assert (GNUNET_OK ==
                       GNUNET_CRYPTO_eddsa_sign (&ref->details.withdraw_sign.coin_priv.eddsa_priv,
                                                 &dr.purpose,
                                                 &coin_sig.eddsa_signature));

      }
      cmd->details.deposit.dh
        = TALER_MINT_deposit (mint,
                              &amount,
                              wire,
                              &h_contract,
                              &coin_pub,
                              &ref->details.withdraw_sign.sig,
                              &ref->details.withdraw_sign.pk->key,
                              timestamp,
                              cmd->details.deposit.transaction_id,
                              &merchant_pub,
                              refund_deadline,
                              &coin_sig,
                              &deposit_cb,
                              is);
      if (NULL == cmd->details.deposit.dh)
      {
        GNUNET_break (0);
        json_decref (wire);
        fail (is);
        return;
      }
      trigger_context_task ();
      return;
    }
  default:
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Unknown instruction %d at %u (%s)\n",
                cmd->oc,
                is->ip,
                cmd->label);
    fail (is);
    return;
  }
  is->task = GNUNET_SCHEDULER_add_now (&interpreter_run,
                                       is);
}


/**
 * Function run when the test terminates (good or bad).
 * Cleans up our state.
 *
 * @param cls the interpreter state.
 * @param tc unused
 */
static void
do_shutdown (void *cls,
             const struct GNUNET_SCHEDULER_TaskContext *tc)
{
  struct InterpreterState *is = cls;
  struct Command *cmd;
  unsigned int i;

  shutdown_task = NULL;
  for (i=0;OC_END != (cmd = &is->commands[i])->oc;i++)
  {
    switch (cmd->oc)
    {
    case OC_END:
      GNUNET_assert (0);
      break;
    case OC_ADMIN_ADD_INCOMING:
      if (NULL != cmd->details.admin_add_incoming.aih)
      {
        GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                    "Command %u (%s) did not complete\n",
                    i,
                    cmd->label);
        TALER_MINT_admin_add_incoming_cancel (cmd->details.admin_add_incoming.aih);
        cmd->details.admin_add_incoming.aih = NULL;
      }
      break;
    case OC_WITHDRAW_STATUS:
      if (NULL != cmd->details.withdraw_status.wsh)
      {
        GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                    "Command %u (%s) did not complete\n",
                    i,
                    cmd->label);
        TALER_MINT_withdraw_status_cancel (cmd->details.withdraw_status.wsh);
        cmd->details.withdraw_status.wsh = NULL;
      }
      break;
    case OC_WITHDRAW_SIGN:
      if (NULL != cmd->details.withdraw_sign.wsh)
      {
        GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                    "Command %u (%s) did not complete\n",
                    i,
                    cmd->label);
        TALER_MINT_withdraw_sign_cancel (cmd->details.withdraw_sign.wsh);
        cmd->details.withdraw_sign.wsh = NULL;
      }
      if (NULL != cmd->details.withdraw_sign.sig.rsa_signature)
      {
        GNUNET_CRYPTO_rsa_signature_free (cmd->details.withdraw_sign.sig.rsa_signature);
        cmd->details.withdraw_sign.sig.rsa_signature = NULL;
      }
      if (NULL != cmd->details.withdraw_sign.blinding_key.rsa_blinding_key)
      {
        GNUNET_CRYPTO_rsa_blinding_key_free (cmd->details.withdraw_sign.blinding_key.rsa_blinding_key);
        cmd->details.withdraw_sign.blinding_key.rsa_blinding_key = NULL;
      }
      break;
    case OC_DEPOSIT:
      if (NULL != cmd->details.deposit.dh)
      {
        GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                    "Command %u (%s) did not complete\n",
                    i,
                    cmd->label);
        TALER_MINT_deposit_cancel (cmd->details.deposit.dh);
        cmd->details.deposit.dh = NULL;
      }
      break;
    default:
      GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                  "Unknown instruction %d at %u (%s)\n",
                  cmd->oc,
                  i,
                  cmd->label);
      break;
    }
  }
  if (NULL != is->task)
  {
    GNUNET_SCHEDULER_cancel (is->task);
    is->task = NULL;
  }
  GNUNET_free (is);
  if (NULL != ctx_task)
  {
    GNUNET_SCHEDULER_cancel (ctx_task);
    ctx_task = NULL;
  }
  if (NULL != mint)
  {
    TALER_MINT_disconnect (mint);
    mint = NULL;
  }
  if (NULL != ctx)
  {
    TALER_MINT_fini (ctx);
    ctx = NULL;
  }
}


/**
 * Functions of this type are called to provide the retrieved signing and
 * denomination keys of the mint.  No TALER_MINT_*() functions should be called
 * in this callback.
 *
 * @param cls closure
 * @param keys information about keys of the mint
 */
static void
cert_cb (void *cls,
         const struct TALER_MINT_Keys *keys)
{
  struct InterpreterState *is = cls;

  /* check that keys is OK */
#define ERR(cond) do { if(!(cond)) break; GNUNET_break (0); GNUNET_SCHEDULER_shutdown(); return; } while (0)
  ERR (NULL == keys);
  ERR (0 == keys->num_sign_keys);
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Read %u signing keys\n",
              keys->num_sign_keys);
  ERR (0 == keys->num_denom_keys);
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Read %u denomination keys\n",
              keys->num_denom_keys);
#undef ERR

  /* run actual tests via interpreter-loop */
  is->keys = keys;
  is->task = GNUNET_SCHEDULER_add_now (&interpreter_run,
                                       is);
}


/**
 * Task that runs the context's event loop with the GNUnet scheduler.
 *
 * @param cls unused
 * @param tc scheduler context (unused)
 */
static void
context_task (void *cls,
              const struct GNUNET_SCHEDULER_TaskContext *tc)
{
  long timeout;
  int max_fd;
  fd_set read_fd_set;
  fd_set write_fd_set;
  fd_set except_fd_set;
  struct GNUNET_NETWORK_FDSet *rs;
  struct GNUNET_NETWORK_FDSet *ws;
  struct GNUNET_TIME_Relative delay;

  ctx_task = NULL;
  TALER_MINT_perform (ctx);
  max_fd = -1;
  timeout = -1;
  FD_ZERO (&read_fd_set);
  FD_ZERO (&write_fd_set);
  FD_ZERO (&except_fd_set);
  TALER_MINT_get_select_info (ctx,
                              &read_fd_set,
                              &write_fd_set,
                              &except_fd_set,
                              &max_fd,
                              &timeout);
  if (timeout >= 0)
    delay = GNUNET_TIME_relative_multiply (GNUNET_TIME_UNIT_MILLISECONDS,
                                           timeout);
  else
    delay = GNUNET_TIME_UNIT_FOREVER_REL;
  rs = GNUNET_NETWORK_fdset_create ();
  GNUNET_NETWORK_fdset_copy_native (rs,
                                    &read_fd_set,
                                    max_fd + 1);
  ws = GNUNET_NETWORK_fdset_create ();
  GNUNET_NETWORK_fdset_copy_native (ws,
                                    &write_fd_set,
                                    max_fd + 1);
  ctx_task = GNUNET_SCHEDULER_add_select (GNUNET_SCHEDULER_PRIORITY_DEFAULT,
                                          delay,
                                          rs,
                                          ws,
                                          &context_task,
                                          cls);
  GNUNET_NETWORK_fdset_destroy (rs);
  GNUNET_NETWORK_fdset_destroy (ws);
}


/**
 * Main function that will be run by the scheduler.
 *
 * @param cls closure
 * @param args remaining command-line arguments
 * @param cfgfile name of the configuration file used (for saving, can be NULL!)
 * @param config configuration
 */
static void
run (void *cls,
     const struct GNUNET_SCHEDULER_TaskContext *tc)
{
  struct InterpreterState *is;
  static struct Command commands[] =
  {
    /* Fill reserve with EUR:5.01, as withdraw fee is 1 ct per config */
    { .oc = OC_ADMIN_ADD_INCOMING,
      .label = "create-reserve-1",
      .details.admin_add_incoming.wire = "{ \"type\":\"TEST\", \"bank\":\"source bank\", \"account\":42 }",
      .details.admin_add_incoming.amount = "EUR:5.01" },
    { .oc = OC_WITHDRAW_SIGN,
      .label = "withdraw-coin-1",
      .details.withdraw_sign.reserve_reference = "create-reserve-1",
      .details.withdraw_sign.amount = "EUR:5" },
    { .oc = OC_WITHDRAW_STATUS,
      .label = "withdraw-status-1",
      .details.withdraw_status.reserve_reference = "create-reserve-1" },
    { .oc = OC_DEPOSIT,
      .label = "deposit-simple",
      .details.deposit.amount = "EUR:5",
      .details.deposit.coin_ref = "withdraw-coin-1",
      .details.deposit.wire_details = "{ \"type\":\"TEST\", \"bank\":\"dest bank\", \"account\":42 }",
      .details.deposit.contract = "{ \"items\"={ \"name\":\"ice cream\", \"value\":1 } }",
      .details.deposit.transaction_id = 1 },
    { .oc = OC_END }
  };

  is = GNUNET_new (struct InterpreterState);
  is->commands = commands;

  ctx = TALER_MINT_init ();
  GNUNET_assert (NULL != ctx);
  ctx_task = GNUNET_SCHEDULER_add_now (&context_task,
                                       ctx);
  mint = TALER_MINT_connect (ctx,
                             "http://localhost:8081",
                             &cert_cb, is,
                             TALER_MINT_OPTION_END);
  GNUNET_assert (NULL != mint);
  shutdown_task
    = GNUNET_SCHEDULER_add_delayed (GNUNET_TIME_relative_multiply
                                    (GNUNET_TIME_UNIT_SECONDS, 5),
                                    &do_shutdown, is);
}


/**
 * Main function for the testcase for the mint API.
 *
 * @param argc expected to be 1
 * @param argv expected to only contain the program name
 */
int
main (int argc,
      char * const *argv)
{
  struct GNUNET_OS_Process *proc;
  struct GNUNET_OS_Process *mintd;

  GNUNET_log_setup ("test-mint-api",
                    "WARNING",
                    NULL);
  proc = GNUNET_OS_start_process (GNUNET_NO,
                                  GNUNET_OS_INHERIT_STD_ALL,
                                  NULL, NULL, NULL,
                                  "taler-mint-keyup",
                                  "taler-mint-keyup",
                                  "-d", "test-mint-home",
                                  "-m", "test-mint-home/master.priv",
                                  NULL);
  GNUNET_OS_process_wait (proc);
  GNUNET_OS_process_destroy (proc);
  mintd = GNUNET_OS_start_process (GNUNET_NO,
                                   GNUNET_OS_INHERIT_STD_ALL,
                                   NULL, NULL, NULL,
                                   "taler-mint-httpd",
                                   "taler-mint-httpd",
                                   "-d", "test-mint-home",
                                   NULL);
  /* give child time to start and bind against the socket */
  sleep (2);
  result = GNUNET_SYSERR;
  GNUNET_SCHEDULER_run (&run, NULL);
  GNUNET_OS_process_kill (mintd,
                          SIGTERM);
  GNUNET_OS_process_wait (mintd);
  GNUNET_OS_process_destroy (mintd);
  return (GNUNET_OK == result) ? 0 : 1;
}

/* end of test_mint_api.c */
