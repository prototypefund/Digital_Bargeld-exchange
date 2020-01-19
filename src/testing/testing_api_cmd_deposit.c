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
 * @file testing/testing_api_cmd_deposit.c
 * @brief command for testing /deposit.
 * @author Marcello Stanisci
 */
#include "platform.h"
#include "taler_json_lib.h"
#include <gnunet/gnunet_curl_lib.h>
#include "taler_testing_lib.h"
#include "taler_signatures.h"
#include "backoff.h"


/**
 * State for a "deposit" CMD.
 */
struct DepositState
{

  /**
   * Amount to deposit.
   */
  struct TALER_Amount amount;

  /**
   * Reference to any command that is able to provide a coin.
   */
  const char *coin_reference;

  /**
   * If @e coin_reference refers to an operation that generated
   * an array of coins, this value determines which coin to pick.
   */
  unsigned int coin_index;

  /**
   * Wire details of who is depositing -- this would be merchant
   * wire details in a normal scenario.
   */
  json_t *wire_details;

  /**
   * JSON string describing what a proposal is about.
   */
  json_t *contract_terms;

  /**
   * Refund deadline. Zero for no refunds.
   */
  struct GNUNET_TIME_Absolute refund_deadline;

  /**
   * Set (by the interpreter) to a fresh private key.  This
   * key will be used to sign the deposit request.
   */
  struct TALER_MerchantPrivateKeyP merchant_priv;

  /**
   * Deposit handle while operation is running.
   */
  struct TALER_EXCHANGE_DepositHandle *dh;

  /**
   * Timestamp of the /deposit operation.
   */
  struct GNUNET_TIME_Absolute timestamp;

  /**
   * Interpreter state.
   */
  struct TALER_TESTING_Interpreter *is;

  /**
   * Task scheduled to try later.
   */
  struct GNUNET_SCHEDULER_Task *retry_task;

  /**
   * How long do we wait until we retry?
   */
  struct GNUNET_TIME_Relative backoff;

  /**
   * Expected HTTP response code.
   */
  unsigned int expected_response_code;

  /**
   * Should we retry on (transient) failures?
   */
  int do_retry;

  /**
   * Set to #GNUNET_YES if the /deposit succeeded
   * and we now can provide the resulting traits.
   */
  int traits_ready;

  /**
   * Signing key used by the exchange to sign the
   * deposit confirmation.
   */
  struct TALER_ExchangePublicKeyP exchange_pub;

  /**
   * Signature from the exchange on the
   * deposit confirmation.
   */
  struct TALER_ExchangeSignatureP exchange_sig;
};


/**
 * Run the command.
 *
 * @param cls closure.
 * @param cmd the command to execute.
 * @param is the interpreter state.
 */
static void
deposit_run (void *cls,
             const struct TALER_TESTING_Command *cmd,
             struct TALER_TESTING_Interpreter *is);


/**
 * Task scheduled to re-try #deposit_run.
 *
 * @param cls a `struct DepositState`
 */
static void
do_retry (void *cls)
{
  struct DepositState *ds = cls;

  ds->retry_task = NULL;
  deposit_run (ds,
               NULL,
               ds->is);
}


/**
 * Callback to analyze the /deposit response, just used to
 * check if the response code is acceptable.
 *
 * @param cls closure.
 * @param http_status HTTP response code.
 * @param ec taler-specific error code.
 * @param exchange_sig signature provided by the exchange
 *        (NULL on errors)
 * @param exchange_pub public key of the exchange,
 *        used for signing the response.
 * @param obj raw response from the exchange.
 */
static void
deposit_cb (void *cls,
            unsigned int http_status,
            enum TALER_ErrorCode ec,
            const struct TALER_ExchangeSignatureP *exchange_sig,
            const struct TALER_ExchangePublicKeyP *exchange_pub,
            const json_t *obj)
{
  struct DepositState *ds = cls;

  ds->dh = NULL;
  if (ds->expected_response_code != http_status)
  {
    if (GNUNET_YES == ds->do_retry)
    {
      if ( (0 == http_status) ||
           (TALER_EC_DB_COMMIT_FAILED_ON_RETRY == ec) ||
           (MHD_HTTP_INTERNAL_SERVER_ERROR == http_status) )
      {
        GNUNET_log (GNUNET_ERROR_TYPE_INFO,
                    "Retrying deposit failed with %u/%d\n",
                    http_status,
                    (int) ec);
        /* on DB conflicts, do not use backoff */
        if (TALER_EC_DB_COMMIT_FAILED_ON_RETRY == ec)
          ds->backoff = GNUNET_TIME_UNIT_ZERO;
        else
          ds->backoff = EXCHANGE_LIB_BACKOFF (ds->backoff);
        ds->retry_task
          = GNUNET_SCHEDULER_add_delayed (ds->backoff,
                                          &do_retry,
                                          ds);
        return;
      }
    }
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Unexpected response code %u to command %s in %s:%u\n",
                http_status,
                ds->is->commands[ds->is->ip].label,
                __FILE__,
                __LINE__);
    json_dumpf (obj, stderr, 0);
    TALER_TESTING_interpreter_fail (ds->is);
    return;
  }
  if (MHD_HTTP_OK == http_status)
  {
    ds->traits_ready = GNUNET_YES;
    ds->exchange_pub = *exchange_pub;
    ds->exchange_sig = *exchange_sig;
  }
  TALER_TESTING_interpreter_next (ds->is);
}


/**
 * Run the command.
 *
 * @param cls closure.
 * @param cmd the command to execute.
 * @param is the interpreter state.
 */
static void
deposit_run (void *cls,
             const struct TALER_TESTING_Command *cmd,
             struct TALER_TESTING_Interpreter *is)
{
  struct DepositState *ds = cls;
  const struct TALER_TESTING_Command *coin_cmd;
  const struct TALER_CoinSpendPrivateKeyP *coin_priv;
  struct TALER_CoinSpendPublicKeyP coin_pub;
  const struct TALER_EXCHANGE_DenomPublicKey *denom_pub;
  const struct TALER_DenominationSignature *denom_pub_sig;
  struct TALER_CoinSpendSignatureP coin_sig;
  struct GNUNET_TIME_Absolute wire_deadline;
  struct GNUNET_CRYPTO_EddsaPrivateKey *merchant_priv;
  struct TALER_MerchantPublicKeyP merchant_pub;
  struct GNUNET_HashCode h_contract_terms;

  ds->is = is;
  GNUNET_assert (ds->coin_reference);
  coin_cmd = TALER_TESTING_interpreter_lookup_command
               (is,
               ds->coin_reference);
  if (NULL == coin_cmd)
  {
    GNUNET_break (0);
    TALER_TESTING_interpreter_fail (is);
    return;
  }

  if ( (GNUNET_OK !=
        TALER_TESTING_get_trait_coin_priv (coin_cmd,
                                           ds->coin_index,
                                           &coin_priv)) ||
       (GNUNET_OK !=
        TALER_TESTING_get_trait_denom_pub (coin_cmd,
                                           ds->coin_index,
                                           &denom_pub)) ||
       (GNUNET_OK !=
        TALER_TESTING_get_trait_denom_sig (coin_cmd,
                                           ds->coin_index,
                                           &denom_pub_sig)) ||
       (GNUNET_OK !=
        TALER_JSON_hash (ds->contract_terms,
                         &h_contract_terms)) )
  {
    GNUNET_break (0);
    TALER_TESTING_interpreter_fail (is);
    return;
  }
  GNUNET_CRYPTO_eddsa_key_get_public (&coin_priv->eddsa_priv,
                                      &coin_pub.eddsa_pub);

  merchant_priv = GNUNET_CRYPTO_eddsa_key_create ();
  ds->merchant_priv.eddsa_priv = *merchant_priv;
  GNUNET_free (merchant_priv);

  if (0 != ds->refund_deadline.abs_value_us)
  {
    struct GNUNET_TIME_Relative refund_deadline;

    refund_deadline = GNUNET_TIME_absolute_get_remaining (ds->refund_deadline);
    wire_deadline = GNUNET_TIME_relative_to_absolute
                      (GNUNET_TIME_relative_multiply (refund_deadline, 2));
  }
  else
  {
    ds->refund_deadline = ds->timestamp;
    wire_deadline = GNUNET_TIME_relative_to_absolute
                      (GNUNET_TIME_UNIT_ZERO);
  }
  GNUNET_CRYPTO_eddsa_key_get_public (&ds->merchant_priv.eddsa_priv,
                                      &merchant_pub.eddsa_pub);

  (void) GNUNET_TIME_round_abs (&wire_deadline);

  {
    struct TALER_DepositRequestPS dr;

    memset (&dr, 0, sizeof (dr));
    dr.purpose.size = htonl
                        (sizeof (struct TALER_DepositRequestPS));
    dr.purpose.purpose = htonl
                           (TALER_SIGNATURE_WALLET_COIN_DEPOSIT);
    dr.h_contract_terms = h_contract_terms;
    GNUNET_assert (GNUNET_OK ==
                   TALER_JSON_merchant_wire_signature_hash (ds->wire_details,
                                                            &dr.h_wire));
    dr.timestamp = GNUNET_TIME_absolute_hton (ds->timestamp);
    dr.refund_deadline = GNUNET_TIME_absolute_hton
                           (ds->refund_deadline);
    TALER_amount_hton (&dr.amount_with_fee,
                       &ds->amount);
    TALER_amount_hton (&dr.deposit_fee,
                       &denom_pub->fee_deposit);
    dr.merchant = merchant_pub;
    dr.coin_pub = coin_pub;
    GNUNET_assert (GNUNET_OK ==
                   GNUNET_CRYPTO_eddsa_sign (&coin_priv->eddsa_priv,
                                             &dr.purpose,
                                             &coin_sig.eddsa_signature));
  }
  ds->dh = TALER_EXCHANGE_deposit (is->exchange,
                                   &ds->amount,
                                   wire_deadline,
                                   ds->wire_details,
                                   &h_contract_terms,
                                   &coin_pub,
                                   denom_pub_sig,
                                   &denom_pub->key,
                                   ds->timestamp,
                                   &merchant_pub,
                                   ds->refund_deadline,
                                   &coin_sig,
                                   &deposit_cb,
                                   ds);

  if (NULL == ds->dh)
  {
    GNUNET_break (0);
    TALER_TESTING_interpreter_fail (is);
    return;
  }
}


/**
 * Free the state of a "deposit" CMD, and possibly cancel a
 * pending operation thereof.
 *
 * @param cls closure, must be a `struct DepositState`.
 * @param cmd the command which is being cleaned up.
 */
static void
deposit_cleanup (void *cls,
                 const struct TALER_TESTING_Command *cmd)
{
  struct DepositState *ds = cls;

  if (NULL != ds->dh)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                "Command %u (%s) did not complete\n",
                ds->is->ip,
                cmd->label);
    TALER_EXCHANGE_deposit_cancel (ds->dh);
    ds->dh = NULL;
  }
  if (NULL != ds->retry_task)
  {
    GNUNET_SCHEDULER_cancel (ds->retry_task);
    ds->retry_task = NULL;
  }
  json_decref (ds->wire_details);
  json_decref (ds->contract_terms);
  GNUNET_free (ds);
}


/**
 * Offer internal data from a "deposit" CMD, to other commands.
 *
 * @param cls closure.
 * @param[out] ret result.
 * @param trait name of the trait.
 * @param index index number of the object to offer.
 *
 * @return #GNUNET_OK on success.
 */
static int
deposit_traits (void *cls,
                const void **ret,
                const char *trait,
                unsigned int index)
{
  struct DepositState *ds = cls;
  const struct TALER_TESTING_Command *coin_cmd;
  /* Will point to coin cmd internals. */
  const struct TALER_CoinSpendPrivateKeyP *coin_spent_priv;

  coin_cmd
    = TALER_TESTING_interpreter_lookup_command (ds->is,
                                                ds->coin_reference);
  if (NULL == coin_cmd)
  {
    GNUNET_break (0);
    TALER_TESTING_interpreter_fail (ds->is);
    return GNUNET_NO;
  }
  if (GNUNET_OK !=
      TALER_TESTING_get_trait_coin_priv (coin_cmd,
                                         ds->coin_index,
                                         &coin_spent_priv))
  {
    GNUNET_break (0);
    TALER_TESTING_interpreter_fail (ds->is);
    return GNUNET_NO;
  }
  {
    struct TALER_TESTING_Trait traits[] = {
      /* First two traits are only available if
         ds->traits is #GNUNET_YES */
      TALER_TESTING_make_trait_exchange_pub (0,
                                             &ds->exchange_pub),
      TALER_TESTING_make_trait_exchange_sig (0,
                                             &ds->exchange_sig),
      /* These traits are always available */
      TALER_TESTING_make_trait_coin_priv (0,
                                          coin_spent_priv),
      TALER_TESTING_make_trait_wire_details (0,
                                             ds->wire_details),
      TALER_TESTING_make_trait_contract_terms (0,
                                               ds->contract_terms),
      TALER_TESTING_make_trait_merchant_priv (0,
                                              &ds->merchant_priv),
      TALER_TESTING_make_trait_amount_obj (0,
                                           &ds->amount),
      TALER_TESTING_trait_end ()
    };

    return TALER_TESTING_get_trait ((ds->traits_ready)
                                    ? traits
                                    : &traits[2],
                                    ret,
                                    trait,
                                    index);
  }
}


/**
 * Create a "deposit" command.
 *
 * @param label command label.
 * @param coin_reference reference to any operation that can
 *        provide a coin.
 * @param coin_index if @a withdraw_reference offers an array of
 *        coins, this parameter selects which one in that array.
 *        This value is currently ignored, as only one-coin
 *        withdrawals are implemented.
 * @param target_account_payto target account for the "deposit"
 *        request.
 * @param contract_terms contract terms to be signed over by the
 *        coin.
 * @param refund_deadline refund deadline, zero means 'no refunds'.
 *        Note, if time were absolute, then it would have come
 *        one day and disrupt tests meaning.
 * @param amount how much is going to be deposited.
 * @param expected_response_code expected HTTP response code.
 *
 * @return the command.
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_deposit (const char *label,
                           const char *coin_reference,
                           unsigned int coin_index,
                           const char *target_account_payto,
                           const char *contract_terms,
                           struct GNUNET_TIME_Relative refund_deadline,
                           const char *amount,
                           unsigned int expected_response_code)
{
  struct DepositState *ds;
  json_t *wire_details;

  wire_details = TALER_TESTING_make_wire_details (target_account_payto);
  ds = GNUNET_new (struct DepositState);
  ds->coin_reference = coin_reference;
  ds->coin_index = coin_index;
  ds->wire_details = wire_details;
  ds->contract_terms = json_loads (contract_terms,
                                   JSON_REJECT_DUPLICATES,
                                   NULL);
  if (NULL == ds->contract_terms)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Failed to parse contract terms `%s' for CMD `%s'\n",
                contract_terms,
                label);
    GNUNET_assert (0);
  }
  ds->timestamp = GNUNET_TIME_absolute_get ();
  (void) GNUNET_TIME_round_abs (&ds->timestamp);

  json_object_set_new (ds->contract_terms,
                       "timestamp",
                       GNUNET_JSON_from_time_abs (ds->timestamp));
  if (0 != refund_deadline.rel_value_us)
  {
    ds->refund_deadline = GNUNET_TIME_relative_to_absolute (refund_deadline);
    (void) GNUNET_TIME_round_abs (&ds->refund_deadline);
    json_object_set_new (ds->contract_terms,
                         "refund_deadline",
                         GNUNET_JSON_from_time_abs (ds->refund_deadline));
  }
  GNUNET_assert (GNUNET_OK ==
                 TALER_string_to_amount (amount,
                                         &ds->amount));
  ds->expected_response_code = expected_response_code;
  {
    struct TALER_TESTING_Command cmd = {
      .cls = ds,
      .label = label,
      .run = &deposit_run,
      .cleanup = &deposit_cleanup,
      .traits = &deposit_traits
    };

    return cmd;
  }
}


/**
 * Modify a deposit command to enable retries when we get transient
 * errors from the exchange.
 *
 * @param cmd a deposit command
 * @return the command with retries enabled
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_deposit_with_retry (struct TALER_TESTING_Command cmd)
{
  struct DepositState *ds;

  GNUNET_assert (&deposit_run == cmd.run);
  ds = cmd.cls;
  ds->do_retry = GNUNET_YES;
  return cmd;
}


/* end of testing_api_cmd_deposit.c */