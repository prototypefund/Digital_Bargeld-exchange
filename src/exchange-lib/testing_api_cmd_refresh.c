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
 * @file exchange-lib/testing_api_cmd_refresh.c
 * @brief commands for testing all "refresh" features.
 * @author Marcello Stanisci
 */

#include "platform.h"
#include "taler_json_lib.h"
#include <gnunet/gnunet_curl_lib.h>
#include "exchange_api_handle.h"
#include "taler_testing_lib.h"
#include "taler_signatures.h"

/**
 * Data for a coin to be melted.
 */
struct MeltDetails
{

  /**
   * Amount to melt (including fee).
   */
  const char *amount;

  /**
   * Reference to reserve_withdraw operations for coin to
   * be used for the /refresh/melt operation.
   */
  const char *coin_reference;
};


/**
 * State for a "refresh melt" command.
 */
struct RefreshMeltState
{

  /**
   * if set to GNUNET_YES, then two /refresh/melt operations
   * will be performed.  This is needed to trigger the logic
   * that manages those already-made requests.  Note: it
   * is not possible to just copy-and-paste a test refresh melt
   * CMD to have the same effect, because every data preparation
   * generates new planchets that (in turn) make the whole "hash"
   * different from any previous one, therefore NOT allowing the
   * exchange to pick any previous /rerfesh/melt operation from
   * the database.
   */
  unsigned int double_melt;

  /**
   * Amount to be melted.  FIXME: this value is useless
   * here as the @a melted_coin field (below) has already it.
   */
  const char *amount;

  /**
   * Information about coins to be melted.
   */
  struct MeltDetails melted_coin;

  /**
   * "Crypto data" used in the refresh operation.
   */
  char *refresh_data;

  /**
   * Number of bytes in @e refresh_data.
   */
  size_t refresh_data_length;

  /**
   * Reference to a previous melt command.
   */
  const char *melt_reference;

  /**
   * Melt handle while operation is running.
   */
  struct TALER_EXCHANGE_RefreshMeltHandle *rmh;

  /**
   * Connection to the exchange.
   */
  struct TALER_EXCHANGE_Handle *exchange;

  /**
   * Interpreter state.
   */
  struct TALER_TESTING_Interpreter *is;

  /**
   * Expected HTTP response code.
   */
  unsigned int expected_response_code;

  /**
   * Array of the denomination public keys
   * corresponding to the @e fresh_amounts.
   */
  struct TALER_EXCHANGE_DenomPublicKey *fresh_pks;

  /**
   * Set by the melt callback as it comes from the exchange.
   */
  uint16_t noreveal_index;
};

/**
 * State for a "refresh reveal" CMD.
 */
struct RefreshRevealState
{
  /**
   * Link to a "refresh melt" command.
   */
  const char *melt_reference;

  /**
   * Reveal handle while operation is running.
   */
  struct TALER_EXCHANGE_RefreshRevealHandle *rrh;

  /**
   * Number of fresh coins withdrawn, set by the
   * reveal callback as it comes from the exchange,
   * it is the length of the @e fresh_coins array.
   */
  unsigned int num_fresh_coins;

  /**
   * Convenience struct to keep in one place all the
   * data related to one fresh coin, set by the reveal callback
   * as it comes from the exchange.
   */
  struct FreshCoin *fresh_coins;

  /**
   * Connection to the exchange.
   */
  struct TALER_EXCHANGE_Handle *exchange;

  /**
   * Interpreter state.
   */
  struct TALER_TESTING_Interpreter *is;

  /**
   * Expected HTTP response code.
   */
  unsigned int expected_response_code;
};

/**
 * State for a "refresh link" CMD.
 */
struct RefreshLinkState
{
  /**
   * Link to a "refresh reveal" command.
   */
  const char *reveal_reference;

  /**
   * Handle to the ongoing operation.
   */
  struct TALER_EXCHANGE_RefreshLinkHandle *rlh;

  /**
   * Connection to the exchange.
   */
  struct TALER_EXCHANGE_Handle *exchange;

  /**
   * Interpreter state.
   */
  struct TALER_TESTING_Interpreter *is;

  /**
   * Expected HTTP response code.
   */
  unsigned int expected_response_code;
};


/**
 * "refresh reveal" request callback; it checks that the response
 * code is expected and copies into its command's state the data
 * coming from the exchange, namely the fresh coins.
 *
 * @param cls closure.
 * @param http_status HTTP response code.
 * @param ec taler-specific error code.
 * @param num_coins number of fresh coins created, length of the
 *        @a sigs and @a coin_privs arrays, 0 if the operation
 *        failed.
 * @param coin_privs array of @a num_coins private keys for the
 *        coins that were created, NULL on error.
 * @param sigs array of signature over @a num_coins coins,
 *        NULL on error.
 * @param full_response raw exchange response.
 */
static void
reveal_cb (void *cls,
           unsigned int http_status,
	   enum TALER_ErrorCode ec,
           unsigned int num_coins,
           const struct TALER_CoinSpendPrivateKeyP *coin_privs,
           const struct TALER_DenominationSignature *sigs,
           const json_t *full_response)
{

  struct RefreshRevealState *rrs = cls;
  const struct TALER_TESTING_Command *melt_cmd;

  rrs->rrh = NULL;
  if (rrs->expected_response_code != http_status)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Unexpected response code %u to command %s\n",
                http_status,
                rrs->is->commands[rrs->is->ip].label);
    json_dumpf (full_response, stderr, 0);
    TALER_TESTING_interpreter_fail (rrs->is);
    return;
  }
  melt_cmd = TALER_TESTING_interpreter_lookup_command
    (rrs->is, rrs->melt_reference);
  if (NULL == melt_cmd)
  {
    GNUNET_break (0);
    TALER_TESTING_interpreter_fail (rrs->is);
    return;
  }
  rrs->num_fresh_coins = num_coins;
  switch (http_status)
  {
  case MHD_HTTP_OK:
    rrs->fresh_coins = GNUNET_new_array
      (num_coins, struct FreshCoin);

    const struct TALER_EXCHANGE_DenomPublicKey *fresh_pks;
    unsigned int i;
    if (GNUNET_OK != TALER_TESTING_get_trait_denom_pub
      (melt_cmd, 0, &fresh_pks))
    {
      GNUNET_break (0);
      TALER_TESTING_interpreter_fail (rrs->is);
      return;    
    }

    for (i=0; i<num_coins; i++)
    {
      struct FreshCoin *fc = &rrs->fresh_coins[i];

      fc->pk = &fresh_pks[i];
      fc->coin_priv = coin_privs[i];
      fc->sig.rsa_signature = GNUNET_CRYPTO_rsa_signature_dup
        (sigs[i].rsa_signature);
    }
    break;
  default:
    GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                "Unknown HTTP status %d\n",
                http_status);
  }
  TALER_TESTING_interpreter_next (rrs->is);
}

/**
 * Run the command.
 *
 * @param cls closure.
 * @param cmd the command to execute.
 * @param is the interpreter state.
 */
void
refresh_reveal_run (void *cls,
                    const struct TALER_TESTING_Command *cmd,
                    struct TALER_TESTING_Interpreter *is)
{
  struct RefreshRevealState *rrs = cls;
  struct RefreshMeltState *rms;
  const struct TALER_TESTING_Command *melt_cmd;
    
  rrs->is = is;
  melt_cmd = TALER_TESTING_interpreter_lookup_command
    (is, rrs->melt_reference);
  
  if (NULL == melt_cmd)
  {
    GNUNET_break (0);
    TALER_TESTING_interpreter_fail (rrs->is);
    return;
  }
  rms = melt_cmd->cls;
  rrs->rrh = TALER_EXCHANGE_refresh_reveal
    (rrs->exchange,
     rms->refresh_data_length,
     rms->refresh_data,
     rms->noreveal_index,
     &reveal_cb, rrs);

  if (NULL == rrs->rrh)
  {
    GNUNET_break (0);
    TALER_TESTING_interpreter_fail (is);
    return;
  }
}


/**
 * Free the state from a "refresh reveal" CMD, and possibly
 * cancel a pending operation thereof.
 *
 * @param cls closure.
 * @param cmd the command which is being cleaned up.
 */
void
refresh_reveal_cleanup (void *cls,
                        const struct TALER_TESTING_Command *cmd)
{
  struct RefreshRevealState *rrs = cls; 

  if (NULL != rrs->rrh)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                "Command %u (%s) did not complete\n",
                rrs->is->ip,
                cmd->label);

    TALER_EXCHANGE_refresh_reveal_cancel (rrs->rrh);
    rrs->rrh = NULL;
  }

  { /* FIXME: why block-ing this? */
    unsigned int j;

    for (j=0; j < rrs->num_fresh_coins; j++)
      GNUNET_CRYPTO_rsa_signature_free
        (rrs->fresh_coins[j].sig.rsa_signature);
  }

  GNUNET_free_non_null (rrs->fresh_coins);
  rrs->fresh_coins = NULL;
  rrs->num_fresh_coins = 0;
}


/**
 * "refresh link" operation callback, checks that HTTP response
 * code is expected _and_ that all the linked coins were actually
 * withdrawn by the "refresh reveal" CMD.
 *
 * @param cls closure. 
 * @param http_status HTTP response code.
 * @param ec taler-specific error code
 * @param num_coins number of fresh coins created, length of the
 *        @a sigs and @a coin_privs arrays, 0 if the operation
 *        failed.
 * @param coin_privs array of @a num_coins private keys for the
 *        coins that were created, NULL on error.
 * @param sigs array of signature over @a num_coins coins, NULL on
 *        error.
 * @param pubs array of public keys for the @a sigs,
 *        NULL on error.
 * @param full_response raw response from the exchange.
 */
static void
link_cb (void *cls,
         unsigned int http_status,
	 enum TALER_ErrorCode ec,
         unsigned int num_coins,
         const struct TALER_CoinSpendPrivateKeyP *coin_privs,
         const struct TALER_DenominationSignature *sigs,
         const struct TALER_DenominationPublicKey *pubs,
         const json_t *full_response)
{

  struct RefreshLinkState *rls = cls;
  const struct TALER_TESTING_Command *reveal_cmd;
  struct TALER_TESTING_Command *link_cmd
    = &rls->is->commands[rls->is->ip];
  unsigned int found;
  unsigned int *num_fresh_coins;

  rls->rlh = NULL;
  if (rls->expected_response_code != http_status)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Unexpected response code %u to command %s\n",
                http_status,
                link_cmd->label);
    json_dumpf (full_response, stderr, 0);
    TALER_TESTING_interpreter_fail (rls->is);
    return;
  }
  reveal_cmd = TALER_TESTING_interpreter_lookup_command
    (rls->is, rls->reveal_reference);

  if (NULL == reveal_cmd)
  {
    GNUNET_break (0);
    TALER_TESTING_interpreter_fail (rls->is);
    return;
  }

  switch (http_status)
  {
  case MHD_HTTP_OK:
    /* check that number of coins returned matches */
    if (GNUNET_OK != TALER_TESTING_get_trait_uint
      (reveal_cmd, 0, &num_fresh_coins))
    {
      GNUNET_break (0);
      TALER_TESTING_interpreter_fail (rls->is);
      return;
    }
    if (num_coins != *num_fresh_coins)
    {
      GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                  "Unexpected number of fresh coins: %d vs %d\n",
                  num_coins, *num_fresh_coins);
      TALER_TESTING_interpreter_fail (rls->is);
      return;
    }
    /* check that the coins match */
    for (unsigned int i=0;i<num_coins;i++)
      for (unsigned int j=i+1;j<num_coins;j++)
	if (0 == memcmp
          (&coin_privs[i], &coin_privs[j],
           sizeof (struct TALER_CoinSpendPrivateKeyP)))
	  GNUNET_break (0);
    /* Note: coins might be legitimately permutated in here... */
    found = 0;

    /* Will point to the pointer inside the cmd state. */
    struct FreshCoin *fc = NULL;

    if (GNUNET_OK != TALER_TESTING_get_trait_fresh_coins
      (reveal_cmd, 0, &fc))
    {
      GNUNET_break (0);
      TALER_TESTING_interpreter_fail (rls->is);
      return;
    }

    for (unsigned int i=0;i<num_coins;i++)
      for (unsigned int j=0;j<num_coins;j++)
      {
	if ( (0 == memcmp
                (&coin_privs[i], &fc[i].coin_priv,
                sizeof (struct TALER_CoinSpendPrivateKeyP))) &&
	     (0 == GNUNET_CRYPTO_rsa_signature_cmp
                (fc[i].sig.rsa_signature,
                 sigs[i].rsa_signature)) &&
	     (0 == GNUNET_CRYPTO_rsa_public_key_cmp
               (fc[i].pk->key.rsa_public_key,
                pubs[i].rsa_public_key)) )
	{
	  found++;
	  break;
	}
      }
    if (found != num_coins)
    {
      GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                  "Only %u/%u coins match expectations\n",
	          found, num_coins);
      GNUNET_break (0);
      TALER_TESTING_interpreter_fail (rls->is);
      return;
    }
    break;
  default:
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Unknown HTTP response code %u.\n",
                http_status);
  }
  TALER_TESTING_interpreter_next (rls->is);
}


/**
 * Run the command.
 *
 * @param cls closure.
 * @param cmd the command to execute.
 * @param is the interpreter state.
 */
void
refresh_link_run (void *cls,
                  const struct TALER_TESTING_Command *cmd,
                  struct TALER_TESTING_Interpreter *is)
{
  
  struct RefreshLinkState *rls = cls;
  struct RefreshRevealState *rrs;
  struct RefreshMeltState *rms;

  const struct TALER_TESTING_Command *reveal_cmd;
  const struct TALER_TESTING_Command *melt_cmd;
  const struct TALER_TESTING_Command *coin_cmd;
  rls->is = is;

  reveal_cmd = TALER_TESTING_interpreter_lookup_command
    (rls->is, rls->reveal_reference);

  if (NULL == reveal_cmd)
  {
    GNUNET_break (0);
    TALER_TESTING_interpreter_fail (rls->is);
    return; 
  }
  rrs = reveal_cmd->cls;
  melt_cmd = TALER_TESTING_interpreter_lookup_command
    (rls->is, rrs->melt_reference);

  if (NULL == melt_cmd)
  {
    GNUNET_break (0);
    TALER_TESTING_interpreter_fail (rls->is);
    return; 
  }

  /* find reserve_withdraw command */
  {
    const struct MeltDetails *md;
    
    rms = melt_cmd->cls;
    md = &rms->melted_coin;
    coin_cmd = TALER_TESTING_interpreter_lookup_command
      (rls->is, md->coin_reference);
    if (NULL == coin_cmd)
    {
      GNUNET_break (0);
      TALER_TESTING_interpreter_fail (rls->is);
      return;
    }
  }

  struct TALER_CoinSpendPrivateKeyP *coin_priv;
  if (GNUNET_OK != TALER_TESTING_get_trait_coin_priv
    (coin_cmd, 0, &coin_priv))
  {
    GNUNET_break (0);
    TALER_TESTING_interpreter_fail (rls->is);
    return;
  }

  /* finally, use private key from withdraw sign command */
  rls->rlh = TALER_EXCHANGE_refresh_link
    (rls->exchange, coin_priv, &link_cb, rls);

  if (NULL == rls->rlh)
  {
    GNUNET_break (0);
    TALER_TESTING_interpreter_fail (rls->is);
    return;
  }
}

/**
 * Free the state of the "refresh link" CMD, and possibly
 * cancel a operation thereof.
 *
 * @param cls closure
 * @param cmd the command which is being cleaned up.
 */
void
refresh_link_cleanup (void *cls,
                      const struct TALER_TESTING_Command *cmd)
{
  struct RefreshLinkState *rls = cls;
  
  if (NULL != rls->rlh)
  {

    GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                "Command %u (%s) did not complete\n",
                rls->is->ip,
                cmd->label);
    TALER_EXCHANGE_refresh_link_cancel (rls->rlh);
    rls->rlh = NULL;
  }
}


/**
 * Callback for a "refresh melt" operation; checks if the HTTP
 * response code is okay and re-run the melt operation if the
 * CMD was set to do so.
 *
 * @param cls closure.
 * @param http_status HTTP response code.
 * @param ec taler-specific error code.
 * @param noreveal_index choice by the exchange in the
 *        cut-and-choose protocol, UINT16_MAX on error.
 * @param exchange_pub public key the exchange used for signing.
 * @param full_response raw response body from the exchange.
 */
static void
melt_cb (void *cls,
         unsigned int http_status,
	 enum TALER_ErrorCode ec,
         uint32_t noreveal_index,
         const struct TALER_ExchangePublicKeyP *exchange_pub,
         const json_t *full_response)
{
  struct RefreshMeltState *rms = cls;

  rms->rmh = NULL;
  if (rms->expected_response_code != http_status)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Unexpected response code %u to command %s\n",
                http_status,
                rms->is->commands[rms->is->ip].label);
    json_dumpf (full_response, stderr, 0);
    TALER_TESTING_interpreter_fail (rms->is);
    return;
  }
  rms->noreveal_index = noreveal_index;

  if (GNUNET_YES == rms->double_melt)
  {
    TALER_LOG_DEBUG ("Doubling the melt (%s)\n",
                     rms->is->commands[rms->is->ip].label);
    rms->rmh = TALER_EXCHANGE_refresh_melt
      (rms->exchange, rms->refresh_data_length,
       rms->refresh_data, &melt_cb, rms);
    rms->double_melt = GNUNET_NO;
    return;
  }

  TALER_TESTING_interpreter_next (rms->is);
}

/**
 * Run the command.
 *
 * @param cls closure.
 * @param cmd the command to execute.
 * @param is the interpreter state.
 */
void
refresh_melt_run (void *cls,
                  const struct TALER_TESTING_Command *cmd,
                  struct TALER_TESTING_Interpreter *is)
{
  struct RefreshMeltState *rms = cls;
  unsigned int num_fresh_coins;
  const struct TALER_TESTING_Command *coin_command;
  const char *melt_fresh_amounts[] = {
    /* with 0.01 withdraw fees (except for 1ct coins),
       this totals up to exactly EUR:3.97, and with
       the 0.03 refresh fee, to EUR:4.0 */
    "EUR:1", "EUR:1", "EUR:1", "EUR:0.1", "EUR:0.1", "EUR:0.1",
    "EUR:0.1", "EUR:0.1", "EUR:0.1", "EUR:0.1", "EUR:0.1",
    "EUR:0.01", "EUR:0.01", "EUR:0.01", "EUR:0.01", "EUR:0.01",
    "EUR:0.01", NULL};
  const struct TALER_EXCHANGE_DenomPublicKey *fresh_pk;

  rms->is = is;
  rms->noreveal_index = UINT16_MAX;
  for (num_fresh_coins=0;
       NULL != melt_fresh_amounts[num_fresh_coins];
       num_fresh_coins++) ;

  rms->fresh_pks = GNUNET_new_array
    (num_fresh_coins,
     struct TALER_EXCHANGE_DenomPublicKey);
  {
    struct TALER_CoinSpendPrivateKeyP *melt_priv;
    struct TALER_Amount melt_amount;
    struct TALER_Amount fresh_amount;
    struct TALER_DenominationSignature *melt_sig;
    const struct TALER_EXCHANGE_DenomPublicKey *melt_denom_pub;
    unsigned int i;

    const struct MeltDetails *md = &rms->melted_coin;
    if (NULL == (coin_command
      = TALER_TESTING_interpreter_lookup_command
        (is, md->coin_reference)))
    {
      GNUNET_break (0);
      TALER_TESTING_interpreter_fail (rms->is);
      return; 
    }

    if (GNUNET_OK != TALER_TESTING_get_trait_coin_priv
      (coin_command, 0, &melt_priv))
    {
      GNUNET_break (0);
      TALER_TESTING_interpreter_fail (rms->is);
      return;    
    }

    if (GNUNET_OK !=
        TALER_string_to_amount (md->amount,
                                &melt_amount))
    {
      GNUNET_break (0);
      GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                  "Failed to parse amount `%s' at %u\n",
                  md->amount,
                  is->ip);
      TALER_TESTING_interpreter_fail (rms->is);
      return;
    }
    if (GNUNET_OK != TALER_TESTING_get_trait_denom_sig
      (coin_command, 0, &melt_sig))
    {
      GNUNET_break (0);
      TALER_TESTING_interpreter_fail (rms->is);
      return;
    }
    if (GNUNET_OK != TALER_TESTING_get_trait_denom_pub
      (coin_command, 0, &melt_denom_pub))
    {
      GNUNET_break (0);
      TALER_TESTING_interpreter_fail (rms->is);
      return;
    }

    for (i=0;i<num_fresh_coins;i++)
    {
      if (GNUNET_OK != TALER_string_to_amount
        (melt_fresh_amounts[i], &fresh_amount))
      {
        GNUNET_break (0);
        GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                    "Failed to parse amount `%s' at index %u\n",
                    melt_fresh_amounts[i], i);
        TALER_TESTING_interpreter_fail (rms->is);
        return;
      }
      fresh_pk = TALER_TESTING_find_pk
        (TALER_EXCHANGE_get_keys (rms->exchange), &fresh_amount);
      if (NULL == fresh_pk)
      {
        GNUNET_break (0);
        /* Subroutine logs specific error */
        TALER_TESTING_interpreter_fail (rms->is);
        return;
      }

      rms->fresh_pks[i] = *fresh_pk;
    }
    rms->refresh_data = TALER_EXCHANGE_refresh_prepare
      (melt_priv, &melt_amount, melt_sig, melt_denom_pub,
       GNUNET_YES, num_fresh_coins, rms->fresh_pks,
       &rms->refresh_data_length);

    if (NULL == rms->refresh_data)
    {
      GNUNET_break (0);
      TALER_TESTING_interpreter_fail (rms->is);
      return;
    }
    rms->rmh = TALER_EXCHANGE_refresh_melt
      (rms->exchange, rms->refresh_data_length,
       rms->refresh_data, &melt_cb, rms);

    if (NULL == rms->rmh)
    {
      GNUNET_break (0);
      TALER_TESTING_interpreter_fail (rms->is);
      return;
    }
  }
}

/**
 * Free the "refresh melt" CMD state, and possibly cancel a
 * pending operation thereof.
 *
 * @param cls closure, typically a #struct RefreshMeltState.
 * @param cmd the command which is being cleaned up.
 */
void
refresh_melt_cleanup (void *cls,
                      const struct TALER_TESTING_Command *cmd)
{
  struct RefreshMeltState *rms = cls;

  if (NULL != rms->rmh)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                "Command %u (%s) did not complete\n",
                rms->is->ip, rms->is->commands[rms->is->ip].label);
    TALER_EXCHANGE_refresh_melt_cancel (rms->rmh);
    rms->rmh = NULL;
  }
  GNUNET_free_non_null (rms->fresh_pks);
  rms->fresh_pks = NULL;
  GNUNET_free_non_null (rms->refresh_data);
  rms->refresh_data = NULL;
  rms->refresh_data_length = 0;
}

/**
 * Offer internal data to the "refresh melt" CMD.
 *
 * @param cls closure.
 * @param ret[out] result (could be anything).
 * @param trait name of the trait.
 * @param index index number of the object to offer.
 *
 * @return #GNUNET_OK on success.
 */
static int
refresh_melt_traits (void *cls,
                     void **ret,
                     const char *trait,
                     unsigned int index)
{
  struct RefreshMeltState *rms = cls;

  struct TALER_TESTING_Trait traits[] = {
    TALER_TESTING_make_trait_denom_pub (0, rms->fresh_pks),
    TALER_TESTING_trait_end ()
  };

  return TALER_TESTING_get_trait (traits,
                                  ret,
                                  trait,
                                  index);
}


/**
 * Create a "refresh melt" command.
 *
 * @param label command label.
 * @param exchange connection to the exchange.
 * @param amount amount to be melted.
 * @param coin_reference reference to a command
 *        that will provide a coin to refresh.
 * @param expected_response_code expected HTTP code.
 *
 * @return the command.
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_refresh_melt
  (const char *label,
   struct TALER_EXCHANGE_Handle *exchange,
   const char *amount,
   const char *coin_reference,
   unsigned int expected_response_code)
{
  struct RefreshMeltState *rms;
  struct MeltDetails md;
  struct TALER_TESTING_Command cmd;

  md.coin_reference = coin_reference;
  md.amount = amount;

  rms = GNUNET_new (struct RefreshMeltState);
  rms->amount = amount;
  rms->melted_coin = md;
  rms->expected_response_code = expected_response_code;
  rms->exchange = exchange;

  cmd.label = label;
  cmd.cls = rms;
  cmd.run = &refresh_melt_run;
  cmd.cleanup = &refresh_melt_cleanup;
  cmd.traits = &refresh_melt_traits;
  
  return cmd;
}

/**
 * Create a "refresh melt" CMD that does TWO /refresh/melt
 * requests.  This was needed to test the replay of a valid melt
 * request, see #5312.
 *
 * @param label command label
 * @param exchange connection to the exchange
 * @param amount FIXME not used.
 * @param coin_reference reference to a command that will provide
 *        a coin to refresh
 * @param expected_response_code expected HTTP code
 *
 * @return the command.
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_refresh_melt_double
  (const char *label,
   struct TALER_EXCHANGE_Handle *exchange,
   const char *amount,
   const char *coin_reference,
   unsigned int expected_response_code)
{
  struct RefreshMeltState *rms;
  struct MeltDetails md;
  struct TALER_TESTING_Command cmd;

  md.coin_reference = coin_reference;
  md.amount = amount;

  rms = GNUNET_new (struct RefreshMeltState);
  rms->amount = amount;
  rms->melted_coin = md;
  rms->expected_response_code = expected_response_code;
  rms->exchange = exchange;
  rms->double_melt = GNUNET_YES;

  cmd.label = label;
  cmd.cls = rms;
  cmd.run = &refresh_melt_run;
  cmd.cleanup = &refresh_melt_cleanup;
  cmd.traits = &refresh_melt_traits;
  
  return cmd;
}

/**
 * Offer internal data from a "refresh reveal" CMD.
 *
 * @param cls closure.
 * @param ret[out] result (could be anything).
 * @param trait name of the trait.
 * @param index index number of the object to offer.
 *
 * @return #GNUNET_OK on success.
 */
static int
refresh_reveal_traits (void *cls,
                       void **ret,
                       const char *trait,
                       unsigned int index)
{
  struct RefreshRevealState *rrs = cls;
  unsigned int num_coins = rrs->num_fresh_coins;
  #define NUM_TRAITS (num_coins * 3) + 3
  struct TALER_TESTING_Trait traits[NUM_TRAITS];
  unsigned int i;

  /* Making coin privs traits */
  for (i=0; i<num_coins; i++)
    traits[i] = TALER_TESTING_make_trait_coin_priv
      (i, &rrs->fresh_coins[i].coin_priv);  

  /* Making denom pubs traits */
  for (i=0; i<num_coins; i++)
    traits[num_coins + i]
      = TALER_TESTING_make_trait_denom_pub
        (i, rrs->fresh_coins[i].pk);

  /* Making denom sigs traits */
  for (i=0; i<num_coins; i++)
    traits[(num_coins * 2) + i]
      = TALER_TESTING_make_trait_denom_sig
        (i, &rrs->fresh_coins[i].sig);

  /* number of fresh coins */
  traits[(num_coins * 3)] = TALER_TESTING_make_trait_uint
    (0, &rrs->num_fresh_coins);

  /* whole array of fresh coins */
  traits[(num_coins * 3) + 1]
    = TALER_TESTING_make_trait_fresh_coins (0, rrs->fresh_coins),

  /* end of traits */
  traits[(num_coins * 3) + 2] = TALER_TESTING_trait_end ();

  return TALER_TESTING_get_trait (traits,
                                  ret,
                                  trait,
                                  index);
}

/**
 * Create a "refresh reveal" command.
 *
 * @param label command label.
 * @param exchange connection to the exchange.
 * @param melt_reference reference to a "refresh melt" command.
 * @param expected_response_code expected HTTP response code.
 *
 * @return the command.
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_refresh_reveal
  (const char *label,
   struct TALER_EXCHANGE_Handle *exchange,
   const char *melt_reference,
   unsigned int expected_response_code)
{
  struct RefreshRevealState *rrs;
  struct TALER_TESTING_Command cmd;
  
  rrs = GNUNET_new (struct RefreshRevealState);
  rrs->melt_reference = melt_reference;
  rrs->exchange = exchange;
  rrs->expected_response_code = expected_response_code;

  cmd.cls = rrs;
  cmd.label = label;
  cmd.run = &refresh_reveal_run;
  cmd.cleanup = &refresh_reveal_cleanup;
  cmd.traits = &refresh_reveal_traits;
  
  return cmd;
}


/**
 * Create a "refresh link" command.
 *
 * @param label command label.
 * @param exchange connection to the exchange.
 * @param reveal_reference reference to a "refresh reveal" CMD.
 * @param expected_response_code expected HTTP response code
 *
 * @return the "refresh link" command
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_refresh_link
  (const char *label,
   struct TALER_EXCHANGE_Handle *exchange,
   const char *reveal_reference,
   unsigned int expected_response_code)
{
  struct RefreshLinkState *rrs;
  struct TALER_TESTING_Command cmd;
  
  rrs = GNUNET_new (struct RefreshLinkState);
  rrs->reveal_reference = reveal_reference;
  rrs->exchange = exchange;
  rrs->expected_response_code = expected_response_code;

  cmd.cls = rrs;
  cmd.label = label;
  cmd.run = &refresh_link_run;
  cmd.cleanup = &refresh_link_cleanup;
  
  return cmd;
}
