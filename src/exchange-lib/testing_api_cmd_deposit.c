/*
  This file is part of TALER
  Copyright (C) 2018 Taler Systems SA

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
 * @file exchange-lib/testing_api_cmd_deposit.c
 * @brief command for testing /deposit.
 * @author Marcello Stanisci
 */

#include "platform.h"
#include "taler_json_lib.h"
#include <gnunet/gnunet_curl_lib.h>
#include "exchange_api_handle.h"
#include "taler_testing_lib.h"
#include "taler_signatures.h"

struct DepositState
{

  /**
   * Amount to deposit.
   */
  const char *amount;

  /**
   * Reference to any command that is able to provide a coin.
   */
  const char *coin_reference;

  /**
   * If this @e coin_ref refers to an operation that generated
   * an array of coins, this value determines which coin to pick.
   */
  unsigned int coin_index;

  /**
   * JSON string describing the merchant's "wire details".
   */
  char *wire_details;

  /**
   * JSON string describing what a proposal is about.
   */
  const char *contract_terms;

  /**
   * Relative time (to add to 'now') to compute the refund
   * deadline.  Zero for no refunds.
   */
  struct GNUNET_TIME_Relative refund_deadline;

  /**
   * Set (by the interpreter) to a fresh private key.
   */
  struct TALER_MerchantPrivateKeyP merchant_priv;

  /**
   * Deposit handle while operation is running.
   */
  struct TALER_EXCHANGE_DepositHandle *dh;

  /**
   * Expected HTTP response code.
   */
  unsigned int expected_response_code;

  /**
   * Interpreter state.
   */
  struct TALER_TESTING_Interpreter *is;

  /**
   * Exchange connection.
   */
  struct TALER_EXCHANGE_Handle *exchange;
};

/**
 * Function called with the result of a /deposit operation.
 *
 * @param cls closure with the interpreter state
 * @param http_status HTTP response code, #MHD_HTTP_OK (200) for
 *        successful deposit; 0 if the exchange's reply is bogus
 *        (fails to follow the protocol)
 * @param ec taler-specific error code, #TALER_EC_NONE on success
 * @param exchange_pub public key the exchange used for signing
 * @param obj the received JSON reply, should be kept as proof
 *        (and, in case of errors, be forwarded to the customer)
 */
static void
deposit_cb (void *cls,
            unsigned int http_status,
            enum TALER_ErrorCode ec,
            const struct TALER_ExchangePublicKeyP *exchange_pub,
            const json_t *obj)
{
  struct DepositState *ds = cls;

  ds->dh = NULL;
  if (ds->expected_response_code != http_status)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Unexpected response code %u to command %s\n",
                http_status,
                ds->is->commands[ds->is->ip].label);
    json_dumpf (obj, stderr, 0);
    TALER_TESTING_interpreter_fail (ds->is);
    return;
  }
  TALER_TESTING_interpreter_next (ds->is);
}

/**
 * Run the command.
 *
 * @param cls closure, typically a #struct WireState.
 * @param cmd the command to execute, a /wire one.
 * @param i the interpreter state.
 */
void
deposit_run (void *cls,
             const struct TALER_TESTING_Command *cmd,
             struct TALER_TESTING_Interpreter *is)
{
  struct DepositState *ds = cls;
  const struct TALER_TESTING_Command *coin_cmd;
  struct TALER_TESTING_Command *this_cmd;
  struct TALER_CoinSpendPrivateKeyP *coin_priv;
  struct TALER_CoinSpendPublicKeyP coin_pub;
  const struct TALER_EXCHANGE_DenomPublicKey *denom_pub;
  struct TALER_DenominationSignature *denom_pub_sig;
  struct TALER_CoinSpendSignatureP coin_sig;
  struct GNUNET_TIME_Absolute refund_deadline;
  struct GNUNET_TIME_Absolute wire_deadline;
  struct GNUNET_TIME_Absolute timestamp;
  struct GNUNET_CRYPTO_EddsaPrivateKey *merchant_priv;
  struct TALER_MerchantPublicKeyP merchant_pub;
  struct GNUNET_HashCode h_contract_terms;
  json_t *contract_terms;
  json_t *wire;
  struct TALER_Amount amount;

  ds->is = is;
  this_cmd = &is->commands[is->ip];

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

  /* Fixme: do prefer "interpreter fail" over assertions,
   * as the former takes care of shutting down processes  too */
  GNUNET_assert (NULL != coin_cmd);

  GNUNET_assert (GNUNET_OK
    == TALER_TESTING_get_trait_coin_priv (coin_cmd,
                                          ds->coin_index,
                                          &coin_priv));

  GNUNET_assert (GNUNET_OK
    == TALER_TESTING_get_trait_denom_pub (coin_cmd,
                                          ds->coin_index,
                                          &denom_pub));

  GNUNET_assert (GNUNET_OK
    == TALER_TESTING_get_trait_denom_sig (coin_cmd,
                                          ds->coin_index,
                                          &denom_pub_sig));
  if (GNUNET_OK !=
      TALER_string_to_amount (ds->amount,
                              &amount))
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Failed to parse amount `%s' at '%u/%s'\n",
                 ds->amount, is->ip, this_cmd->label);
    TALER_TESTING_interpreter_fail (is);
    return;
  }
  contract_terms = json_loads (ds->contract_terms,
                               JSON_REJECT_DUPLICATES,
                               NULL);
  if (NULL == contract_terms)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Failed to parse proposal data `%s' at %u/%s\n",
                ds->contract_terms, is->ip, this_cmd->label);
    TALER_TESTING_interpreter_fail (is);
    return;
  }
  GNUNET_assert (GNUNET_OK ==
                 TALER_JSON_hash (contract_terms,
                                  &h_contract_terms));
  json_decref (contract_terms);

  wire = json_loads (ds->wire_details,
                     JSON_REJECT_DUPLICATES,
                     NULL);
  if (NULL == wire)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Failed to parse wire details `%s' at %u/%s\n",
                ds->wire_details,
                is->ip,
                this_cmd->label);
    TALER_TESTING_interpreter_fail (is);
    return;
  }
  
  GNUNET_CRYPTO_eddsa_key_get_public (&coin_priv->eddsa_priv,
                                      &coin_pub.eddsa_pub);

  merchant_priv = GNUNET_CRYPTO_eddsa_key_create ();
  ds->merchant_priv.eddsa_priv = *merchant_priv;
  GNUNET_free (merchant_priv);

  if (0 != ds->refund_deadline.rel_value_us)
  {
    refund_deadline = GNUNET_TIME_relative_to_absolute
      (ds->refund_deadline);
    wire_deadline = GNUNET_TIME_relative_to_absolute
    (GNUNET_TIME_relative_multiply
      (ds->refund_deadline, 2));
  }
  else
  {
    refund_deadline = GNUNET_TIME_UNIT_ZERO_ABS;
    wire_deadline = GNUNET_TIME_relative_to_absolute
      (GNUNET_TIME_UNIT_ZERO);
  }
  GNUNET_CRYPTO_eddsa_key_get_public
    (&ds->merchant_priv.eddsa_priv,
     &merchant_pub.eddsa_pub);

  timestamp = GNUNET_TIME_absolute_get ();
  GNUNET_TIME_round_abs (&timestamp);
  GNUNET_TIME_round_abs (&refund_deadline);
  GNUNET_TIME_round_abs (&wire_deadline);

  {
    struct TALER_DepositRequestPS dr;

    memset (&dr, 0, sizeof (dr));
    dr.purpose.size = htonl
      (sizeof (struct TALER_DepositRequestPS));
    dr.purpose.purpose = htonl
      (TALER_SIGNATURE_WALLET_COIN_DEPOSIT);
    dr.h_contract_terms = h_contract_terms;
    GNUNET_assert (GNUNET_OK == TALER_JSON_hash
      (wire, &dr.h_wire));
    dr.timestamp = GNUNET_TIME_absolute_hton (timestamp);
    dr.refund_deadline = GNUNET_TIME_absolute_hton
      (refund_deadline);
    TALER_amount_hton (&dr.amount_with_fee, &amount);
    TALER_amount_hton
      (&dr.deposit_fee, &denom_pub->fee_deposit);
    dr.merchant = merchant_pub;
    dr.coin_pub = coin_pub;
    GNUNET_assert (GNUNET_OK == GNUNET_CRYPTO_eddsa_sign
      (&coin_priv->eddsa_priv,
       &dr.purpose,
       &coin_sig.eddsa_signature));
  }
  ds->dh = TALER_EXCHANGE_deposit
    (ds->exchange,
     &amount,
     wire_deadline,
     wire,
     &h_contract_terms,
     &coin_pub,
     denom_pub_sig,
     &denom_pub->key,
     timestamp,
     &merchant_pub,
     refund_deadline,
     &coin_sig,
     &deposit_cb,
     ds);

  if (NULL == ds->dh)
  {
    GNUNET_break (0);
    json_decref (wire);
    TALER_TESTING_interpreter_fail (is);
    return;
  }
  json_decref (wire);
  return;
}

/**
 * Cleanup the state.
 *
 * @param cls closure, typically a #struct WireState.
 * @param cmd the command which is being cleaned up.
 */
void
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

  GNUNET_free (ds->wire_details);
  GNUNET_free (ds);
}

/**
 * Extract information from a command that is useful for other
 * commands.
 *
 * @param cls closure
 * @param ret[out] result (could be anything)
 * @param trait name of the trait
 * @param selector more detailed information about which object
 *                 to return in case there were multiple generated
 *                 by the command
 * @return #GNUNET_OK on success
 */
static int
deposit_traits (void *cls,
                void **ret,
                const char *trait,
                unsigned int index)
{
  struct DepositState *ds = cls;
  const struct TALER_TESTING_Command *coin_cmd; 
  /* Will point to coin cmd internals. */
  struct TALER_CoinSpendPrivateKeyP *coin_spent_priv;

  coin_cmd = TALER_TESTING_interpreter_lookup_command
    (ds->is, ds->coin_reference);

  if (NULL == coin_cmd)
  {
    GNUNET_break (0);
    TALER_TESTING_interpreter_fail (ds->is);
    return GNUNET_NO;
  }

  if (GNUNET_OK != TALER_TESTING_get_trait_coin_priv
    (coin_cmd, ds->coin_index, &coin_spent_priv))
  {
    GNUNET_break (0);
    TALER_TESTING_interpreter_fail (ds->is);
    return GNUNET_NO;
  }

  struct TALER_TESTING_Trait traits[] = {
    TALER_TESTING_make_trait_coin_priv (0, coin_spent_priv),
    TALER_TESTING_make_trait_wire_details (0, ds->wire_details),
    TALER_TESTING_make_trait_contract_terms (0, ds->contract_terms),
    TALER_TESTING_make_trait_peer_key
      (0, &ds->merchant_priv.eddsa_priv),
    TALER_TESTING_trait_end ()  
  };

  return TALER_TESTING_get_trait (traits,
                                  ret,
                                  trait,
                                  index);
}

/**
 * Create a deposit command.
 *
 * @param label command label
 * @param exchange exchange connection
 * @param coin_reference reference to any operation that can
 *        provide a coin
 * @param coin_index if @a withdraw_reference offers an array of
 *        coins, this parameter selects which one in that array.
 *        This value is currently ignored, as only one-coin
 *        withdrawals are implemented.
 * @param wire_details bank details of the merchant performing the
 *        deposit
 * @param contract_terms contract terms to be signed over by the
 *        coin
 * @param refund_deadline refund deadline, zero means 'no refunds'
 * @param amount how much is going to be deposited
 * @param expected_response_code which HTTP status code we expect
 *        in the response
 *
 * @return the deposit command to be run by the interpreter
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_deposit
  (const char *label,
   struct TALER_EXCHANGE_Handle *exchange,
   const char *coin_reference,
   unsigned int coin_index,
   char *wire_details,
   const char *contract_terms,
   struct GNUNET_TIME_Relative refund_deadline,
   const char *amount,
   unsigned int expected_response_code)
{
  struct TALER_TESTING_Command cmd;
  struct DepositState *ds;
  
  ds = GNUNET_new (struct DepositState);
  ds->exchange = exchange;
  ds->coin_reference = coin_reference;
  ds->coin_index = coin_index;
  ds->wire_details = wire_details;
  ds->contract_terms = contract_terms;
  ds->refund_deadline = refund_deadline;
  ds->amount = amount;
  ds->expected_response_code = expected_response_code;

  cmd.cls = ds;
  cmd.label = label;
  cmd.run = &deposit_run;
  cmd.cleanup = &deposit_cleanup;
  cmd.traits = &deposit_traits;

  return cmd;
}