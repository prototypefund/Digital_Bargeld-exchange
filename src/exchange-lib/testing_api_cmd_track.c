/*
  This file is part of TALER
  Copyright (C) 2014-2018 Taler Systems SA

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
 * @file exchange/testing_api_cmd_status.c
 * @brief Implement the /reserve/status test command.
 * @author Marcello Stanisci
 */

#include "platform.h"
#include "taler_json_lib.h"
#include <gnunet/gnunet_curl_lib.h>
#include "exchange_api_handle.h"
#include "taler_testing_lib.h"

struct TrackTransactionState
{

  /**
   * Which #OC_CHECK_BANK_TRANSFER wtid should this match? NULL
   * for none.
   */
  const char *bank_transfer_reference;

  /**
   * Wire transfer identifier, set if #MHD_HTTP_OK was the
   * response code.
   */
  struct TALER_WireTransferIdentifierRawP wtid;

  /**
   * Expected HTTP response code.
   */
  unsigned int expected_response_code;

  /**
   * Reference to any operation that can provide a transaction.
   * Tipically a /deposit operation.
   */
  const char *transaction_reference;

  /**
   * Index of the coin involved in the transaction.
   */
  unsigned int coin_index;

  /**
   * Handle to the deposit wtid request.
   */
  struct TALER_EXCHANGE_TrackTransactionHandle *tth;

  /**
   * Handle to the exchange.
   */
  struct TALER_EXCHANGE_Handle *exchange;

  /**
   * Interpreter state.
   */
  struct TALER_TESTING_Interpreter *is;
};

struct TrackTransferState
{

  /**
   * Expected amount for this WTID.
   */
  const char *expected_total_amount;

  /**
   * Expected fee for this WTID.
   */
  const char *expected_wire_fee;

  /**
   * Expected HTTP response code.
   */
  unsigned int expected_response_code;

  /**
   * Reference to any operation that can provide a WTID.
   */
  const char *wtid_reference;

  /**
   * Reference to any operation that can provide wire details.
   */
  const char *wire_details_reference;

  /**
   * Reference to any operation that can provide an amount.
   */
  const char *total_amount_reference;

  /**
   * Index to the WTID to pick.
   */
  unsigned int index;

  /**
   * Handle to the deposit wtid request.
   */
  struct TALER_EXCHANGE_TrackTransferHandle *tth;

  /**
   * Handle to the exchange.
   */
  struct TALER_EXCHANGE_Handle *exchange;

  /**
   * Interpreter state.
   */
  struct TALER_TESTING_Interpreter *is;
};


/**
 * Function called with detailed wire transfer data.
 *
 * @param cls closure
 * @param http_status HTTP status code we got, 0 on exchange
 *        protocol violation
 * @param ec taler-specific error code, #TALER_EC_NONE on success
 * @param exchange_pub public key the exchange used for signing
 * @param json original json reply (may include signatures, those
 *        have then been validated already)
 * @param wtid wire transfer identifier used by the exchange, NULL
 *        if exchange did not yet execute the transaction
 * @param execution_time actual or planned execution time for the
 *        wire transfer
 * @param coin_contribution contribution to the @a total_amount of
 *        the deposited coin (may be NULL)
 * @param total_amount total amount of the wire transfer, or NULL
 *        if the exchange could not provide any @a wtid (set only
 *        if @a http_status is #MHD_HTTP_OK)
 */
static void
deposit_wtid_cb
  (void *cls,
   unsigned int http_status,
   enum TALER_ErrorCode ec,
   const struct TALER_ExchangePublicKeyP *exchange_pub,
   const json_t *json,
   const struct TALER_WireTransferIdentifierRawP *wtid,
   struct GNUNET_TIME_Absolute execution_time,
   const struct TALER_Amount *coin_contribution)
{
  struct TrackTransactionState *tts = cls;
  struct TALER_TESTING_Interpreter *is = tts->is;
  struct TALER_TESTING_Command *cmd = &is->commands[is->ip];

  tts->tth = NULL;
  if (tts->expected_response_code != http_status)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Unexpected response code %u to command %s\n",
                http_status,
                cmd->label);
    json_dumpf (json, stderr, 0);
    TALER_TESTING_interpreter_fail (is);
    return;
  }
  switch (http_status)
  {
  case MHD_HTTP_OK:
    tts->wtid = *wtid;
    if (NULL != tts->bank_transfer_reference)
    {
      const struct TALER_TESTING_Command *bank_transfer_cmd; 
      char *ws;

      ws = GNUNET_STRINGS_data_to_string_alloc (wtid,
                                                sizeof (*wtid));
      bank_transfer_cmd = TALER_TESTING_interpreter_lookup_command
        (is, tts->bank_transfer_reference);

      if (NULL == bank_transfer_cmd)
      {
        GNUNET_break (0);
        TALER_TESTING_interpreter_fail (is);
        return;
      }

      const char *transfer_subject;

      if (GNUNET_OK != TALER_TESTING_get_trait_transfer_subject
        (bank_transfer_cmd, 0, &transfer_subject))
      {
        GNUNET_break (0);
        TALER_TESTING_interpreter_fail (is);
        return;       
      }

      if (0 != strcmp (ws, transfer_subject))
      {
        GNUNET_break (0);
        GNUNET_free (ws);
        TALER_TESTING_interpreter_fail (tts->is);
        return;
      }

      GNUNET_free (ws);
    }
    break;
  case MHD_HTTP_ACCEPTED:
    /* allowed, nothing to check here */
    break;
  case MHD_HTTP_NOT_FOUND:
    /* allowed, nothing to check here */
    break;
  default:
    GNUNET_break (0);
    break;
  }
  TALER_TESTING_interpreter_next (tts->is);
}

/**
 * Run the command.
 *
 * @param cls closure, typically a #struct WireState.
 * @param cmd the command to execute, a /track/transaction one.
 * @param is the interpreter state.
 */
void
track_transaction_run (void *cls,
                       const struct TALER_TESTING_Command *cmd,
                       struct TALER_TESTING_Interpreter *is)
{
  struct TrackTransactionState *tts = cls;
  const struct TALER_TESTING_Command *transaction_cmd;
  struct TALER_CoinSpendPrivateKeyP *coin_priv;
  struct TALER_CoinSpendPublicKeyP coin_pub;
  const char *wire_details;
  const char *contract_terms;
  json_t *j_wire_details;
  json_t *j_contract_terms;
  struct GNUNET_HashCode h_wire_details;
  struct GNUNET_HashCode h_contract_terms;
  const struct GNUNET_CRYPTO_EddsaPrivateKey *merchant_priv;
  
  tts->is = is;
  transaction_cmd = TALER_TESTING_interpreter_lookup_command
    (tts->is, tts->transaction_reference);

  if (NULL == transaction_cmd)
  {
    GNUNET_break (0);
    TALER_TESTING_interpreter_fail (tts->is);
    return;  
  }

  if (GNUNET_OK != TALER_TESTING_get_trait_coin_priv
    (transaction_cmd, tts->coin_index, &coin_priv))
  {
    GNUNET_break (0);
    TALER_TESTING_interpreter_fail (tts->is);
    return;  
  }

  GNUNET_CRYPTO_eddsa_key_get_public (&coin_priv->eddsa_priv,
                                      &coin_pub.eddsa_pub);

  /* Get the strings.. */
  if (GNUNET_OK != TALER_TESTING_get_trait_wire_details
    (transaction_cmd, 0, &wire_details))
  {
    GNUNET_break (0);
    TALER_TESTING_interpreter_fail (tts->is);
    return;
  }

  if (GNUNET_OK != TALER_TESTING_get_trait_contract_terms
    (transaction_cmd, 0, &contract_terms))
  {
    GNUNET_break (0);
    TALER_TESTING_interpreter_fail (tts->is);
    return;
  }

  /* Parse them.. */
  j_wire_details = json_loads
    (wire_details, JSON_REJECT_DUPLICATES, NULL);
  j_contract_terms = json_loads
    (contract_terms, JSON_REJECT_DUPLICATES, NULL);
  
  if ((NULL == j_wire_details) || (NULL == j_contract_terms))
  {
    GNUNET_break (0);
    TALER_TESTING_interpreter_fail (tts->is);
    return;  
  }

  /* Should not fail here, json has been parsed already */
  GNUNET_assert
    ( (GNUNET_OK == TALER_JSON_hash (j_wire_details,
                                      &h_wire_details)) &&
      (GNUNET_OK == TALER_JSON_hash (j_contract_terms,
                                      &h_contract_terms)) );

  if (GNUNET_OK != TALER_TESTING_get_trait_peer_key
    (transaction_cmd, 0, &merchant_priv))
  {
    GNUNET_break (0);
    TALER_TESTING_interpreter_fail (tts->is);
    return; 
  }

  tts->tth = TALER_EXCHANGE_track_transaction
    (tts->exchange,
     (struct TALER_MerchantPrivateKeyP *) merchant_priv,
     &h_wire_details,
     &h_contract_terms,
     &coin_pub,
     &deposit_wtid_cb,
     tts);

  GNUNET_assert (NULL != tts->tth);
}

/**
 * Cleanup the state.
 *
 * @param cls closure, typically a #struct WireState.
 * @param cmd the command which is being cleaned up.
 */
void
track_transaction_cleanup
  (void *cls,
   const struct TALER_TESTING_Command *cmd)
{
  struct TrackTransactionState *tts = cls;

  if (NULL != tts->tth)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                "Command %u (%s) did not complete\n",
                tts->is->ip,
                cmd->label);
    TALER_EXCHANGE_track_transaction_cancel (tts->tth);
    tts->tth = NULL;
  }
  GNUNET_free (tts);
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
track_transaction_traits (void *cls,
                          void **ret,
                          const char *trait,
                          unsigned int index)
{
  struct TrackTransactionState *tts = cls;

  struct TALER_TESTING_Trait traits[] = {
    TALER_TESTING_make_trait_wtid (0, &tts->wtid),
    TALER_TESTING_trait_end ()
  };

  return TALER_TESTING_get_trait (traits,
                                  ret,
                                  trait,
                                  index);
}

/**
 * Create a /track/transaction command.
 *
 * @param label the command label.
 * @param exchange the exchange to connect to.
 * @param transaction_reference reference to a deposit operation.
 * @param coin_index index of the coin involved in the transaction
 * @param expected_response_code expected HTTP response code.
 * @param bank_transfer_reference which #OC_CHECK_BANK_TRANSFER
 *        wtid should this match? NULL
   * for none
 *
 * @return the command to be executed by the interpreter.
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_track_transaction
  (const char *label,
   struct TALER_EXCHANGE_Handle *exchange,
   const char *transaction_reference,
   unsigned int coin_index,
   unsigned int expected_response_code,
   const char *bank_transfer_reference)
{
  struct TALER_TESTING_Command cmd;
  struct TrackTransactionState *tts;

  tts = GNUNET_new (struct TrackTransactionState);
  tts->exchange = exchange;
  tts->transaction_reference = transaction_reference;
  tts->expected_response_code = expected_response_code;
  tts->bank_transfer_reference = bank_transfer_reference;
  tts->coin_index = coin_index;

  cmd.cls = tts;
  cmd.label = label;
  cmd.run = &track_transaction_run;
  cmd.cleanup = &track_transaction_cleanup;
  cmd.traits = &track_transaction_traits;

  return cmd;

}

/**
 * Cleanup the state.
 *
 * @param cls closure.
 * @param cmd the command which is being cleaned up.
 */
void
track_transfer_cleanup (void *cls,
                        const struct TALER_TESTING_Command *cmd)
{

  struct TrackTransferState *tts = cls;

  if (NULL != tts->tth)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                "Command %u (%s) did not complete\n",
                tts->is->ip,
                cmd->label);
    TALER_EXCHANGE_track_transfer_cancel (tts->tth);
    tts->tth = NULL;
  }
  GNUNET_free (tts);

}

/**
 * Function called with detailed wire transfer data, including all
 * of the coin transactions that were combined into the wire
 * transfer.
 *
 * @param cls closure
 * @param http_status HTTP status code we got, 0 on exchange
 *        protocol violation
 * @param ec taler-specific error code, #TALER_EC_NONE on success
 * @param exchange_pub public key the exchange used for signing
 * @param json original json reply (may include signatures, those
 *        have then been validated already)
 * @param h_wire hash of the wire transfer address the transfer
 *        went to, or NULL on error
 * @param execution_time time when the exchange claims to have
 *        performed the wire transfer
 * @param total_amount total amount of the wire transfer, or NULL
 *        if the exchange could not provide any @a wtid (set only
 *        if @a http_status is #MHD_HTTP_OK)
 * @param wire_fee wire fee that was charged by the exchange
 * @param details_length length of the @a details array
 * @param details array with details about the combined
 *        transactions
 */
static void
track_transfer_cb
  (void *cls,
   unsigned int http_status,
   enum TALER_ErrorCode ec,
   const struct TALER_ExchangePublicKeyP *exchange_pub,
   const json_t *json,
   const struct GNUNET_HashCode *h_wire,
   struct GNUNET_TIME_Absolute execution_time,
   const struct TALER_Amount *total_amount,
   const struct TALER_Amount *wire_fee,
   unsigned int details_length,
   const struct TALER_TrackTransferDetails *details)
{
  struct TrackTransferState *tts = cls;
  struct TALER_TESTING_Interpreter *is = tts->is;
  struct TALER_TESTING_Command *cmd = &is->commands[is->ip];

  const struct TALER_TESTING_Command *wtid_cmd;
  struct TALER_Amount expected_amount;

  tts->tth = NULL;

  if (tts->expected_response_code != http_status)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Unexpected response code %u to command %s\n",
                http_status,
                cmd->label);
    json_dumpf (json, stderr, 0);
    TALER_TESTING_interpreter_fail (is);
    return;
  }

  switch (http_status)
  {

  if (
    (NULL == tts->expected_total_amount) ||
      (NULL == tts->expected_wire_fee))
    GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                "Expected amount and fee not specified, "
                "likely to segfault...\n");

  case MHD_HTTP_OK:
    if (GNUNET_OK !=
        TALER_string_to_amount (tts->expected_total_amount,
                                &expected_amount))
    {
      GNUNET_break (0);
      TALER_TESTING_interpreter_fail (is);
      return;
    }
    if (0 != TALER_amount_cmp (total_amount,
                               &expected_amount))
    {
      GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                  "Total amount missmatch to command %s - "
                  "%s vs %s\n",
                  cmd->label,
                  TALER_amount_to_string (total_amount),
                  TALER_amount_to_string (&expected_amount));
      json_dumpf (json, stderr, 0);
      fprintf (stderr, "\n");
      TALER_TESTING_interpreter_fail (is);
      return;
    }

    if (GNUNET_OK !=
        TALER_string_to_amount (tts->expected_wire_fee,
                                &expected_amount))
    {
      GNUNET_break (0);
      TALER_TESTING_interpreter_fail (is);
      return;
    }

    if (0 != TALER_amount_cmp (wire_fee,
                               &expected_amount))
    {
      GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                  "Wire fee missmatch to command %s\n",
                  cmd->label);
      json_dumpf (json, stderr, 0);
      TALER_TESTING_interpreter_fail (is);
      return;
    }

    wtid_cmd = TALER_TESTING_interpreter_lookup_command
      (is, tts->wtid_reference);

    if (NULL == wtid_cmd)
    {
      GNUNET_break (0);
      TALER_TESTING_interpreter_fail (is);
      return;    
    }

    /**
     * Optionally checking: (1) wire-details for this transfer
     * match the ones from a referenced "deposit" operation -
     * or any operation that could provide wire-details.  (2)
     * Total amount for this transfer matches the one from any
     * referenced command that could provide one.
     */

    if (NULL != tts->wire_details_reference)
    {
      const struct TALER_TESTING_Command *wire_details_cmd;
      const char *wire_details;
      json_t *j_wire_details;
      struct GNUNET_HashCode h_wire_details;

      if (NULL == (wire_details_cmd
        = TALER_TESTING_interpreter_lookup_command
          (is, tts->wire_details_reference)))
      {
        GNUNET_break (0);
        TALER_TESTING_interpreter_fail (is);
        return;
      }

      if (GNUNET_OK != TALER_TESTING_get_trait_wire_details
        (wire_details_cmd, 0, &wire_details))
      {
        GNUNET_break (0);
        TALER_TESTING_interpreter_fail (is);
        return;
      }

      j_wire_details = json_loads
        (wire_details, JSON_REJECT_DUPLICATES, NULL);

      GNUNET_assert (NULL != j_wire_details);

      GNUNET_assert (GNUNET_OK == TALER_JSON_hash
        (j_wire_details, &h_wire_details));

      if (0 != memcmp (&h_wire_details,
                       h_wire,
                       sizeof (struct GNUNET_HashCode)))
      {
        GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                    "Wire hash missmath to command %s\n",
                    cmd->label);
        json_dumpf (json, stderr, 0);
        TALER_TESTING_interpreter_fail (is);
        return;
      }
    }
    if (NULL != tts->total_amount_reference)
    {
      const struct TALER_TESTING_Command *total_amount_cmd;
      const char *total_amount_from_reference_str;
      struct TALER_Amount total_amount_from_reference;

      if (NULL == (total_amount_cmd
        = TALER_TESTING_interpreter_lookup_command
          (is, tts->total_amount_reference)))
      {
        GNUNET_break (0);
        TALER_TESTING_interpreter_fail (is);
        return;      
      }

      if (GNUNET_OK != TALER_TESTING_get_trait_amount
        (total_amount_cmd, 0, &total_amount_from_reference_str))
      {
        GNUNET_break (0);
        TALER_TESTING_interpreter_fail (is);
        return;
      }
      
      GNUNET_assert (GNUNET_OK == TALER_string_to_amount
        (total_amount_from_reference_str,
         &total_amount_from_reference));

      if (0 != TALER_amount_cmp (total_amount,
                                 &total_amount_from_reference))
      {
        GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                    "Amount missmath to command %s\n",
                    cmd->label);
        json_dumpf (json, stderr, 0);
        TALER_TESTING_interpreter_fail (is);
        return;
      }
    }
  }
  TALER_TESTING_interpreter_next (is);
}

/**
 * Run the command.
 *
 * @param cls closure.
 * @param cmd the command to execute, a /track/transfer one.
 * @param is the interpreter state.
 */
void
track_transfer_run (void *cls,
                    const struct TALER_TESTING_Command *cmd,
                    struct TALER_TESTING_Interpreter *is)
{
  /* looking for a wtid to track .. */

  struct TrackTransferState *tts = cls;
  struct TALER_WireTransferIdentifierRawP wtid;
  struct TALER_WireTransferIdentifierRawP *wtid_ptr;

  /* If no reference is given, we'll use a all-zeros
   * WTID */
  memset (&wtid, 0, sizeof (wtid));
  wtid_ptr = &wtid;
  
  tts->is = is;
  if (NULL != tts->wtid_reference)
  {
    const struct TALER_TESTING_Command *wtid_cmd;

    wtid_cmd = TALER_TESTING_interpreter_lookup_command
      (tts->is, tts->wtid_reference);

    if (NULL == wtid_cmd)
    {
      GNUNET_break (0);
      TALER_TESTING_interpreter_fail (tts->is);
      return;
    }

    if (GNUNET_OK != TALER_TESTING_get_trait_wtid
      (wtid_cmd, tts->index, &wtid_ptr))
    {
      GNUNET_break (0);
      TALER_TESTING_interpreter_fail (tts->is);
      return;     
    }
    GNUNET_assert (NULL != wtid_ptr);
  }
  tts->tth = TALER_EXCHANGE_track_transfer (tts->exchange,
                                            wtid_ptr,
                                            &track_transfer_cb,
                                            tts);

  GNUNET_assert (NULL != tts->tth);
}

/**
 * Make a /track/transfer command, expecting the transfer
 * not being done (yet).
 *
 * @param label the command label
 * @param exchange connection to the exchange
 * @param wtid_reference reference to any command which can provide
 *        a wtid
 * @param index in case there are multiple wtid offered, this
 *        parameter selects a particular one
 * @param expected_response_code expected HTTP response code
 *
 * @return the command
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_track_transfer_empty
  (const char *label,
   struct TALER_EXCHANGE_Handle *exchange,
   const char *wtid_reference,
   unsigned int index,
   unsigned int expected_response_code)
{
  struct TrackTransferState *tts;
  struct TALER_TESTING_Command cmd;
  
  tts = GNUNET_new (struct TrackTransferState);

  tts->wtid_reference = wtid_reference;
  tts->index = index;
  tts->expected_response_code = expected_response_code;
  tts->exchange = exchange;

  cmd.cls = tts;
  cmd.label = label;
  cmd.run = &track_transfer_run;
  cmd.cleanup = &track_transfer_cleanup;

  return cmd; 
}

/**
 * Make a /track/transfer command, specifying which amount and
 * wire fee are expected.
 *
 * @param label the command label
 * @param exchange connection to the exchange
 * @param wtid_reference reference to any command which can provide
 *        a wtid
 * @param index in case there are multiple wtid offered, this
 *        parameter selects a particular one
 * @param expected_response_code expected HTTP response code
 * @param expected_amount how much money we expect being
 *        moved with this wire-transfer.
 * @param expected_wire_fee expected wire fee.
 *
 * @return the command
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_track_transfer
  (const char *label,
   struct TALER_EXCHANGE_Handle *exchange,
   const char *wtid_reference,
   unsigned int index,
   unsigned int expected_response_code,
   const char *expected_total_amount,
   const char *expected_wire_fee)
{
  struct TrackTransferState *tts;
  struct TALER_TESTING_Command cmd;
  
  tts = GNUNET_new (struct TrackTransferState);

  tts->wtid_reference = wtid_reference;
  tts->index = index;
  tts->expected_response_code = expected_response_code;
  tts->exchange = exchange;
  tts->expected_total_amount = expected_total_amount;
  tts->expected_wire_fee = expected_wire_fee;

  cmd.cls = tts;
  cmd.label = label;
  cmd.run = &track_transfer_run;
  cmd.cleanup = &track_transfer_cleanup;

  return cmd; 
}