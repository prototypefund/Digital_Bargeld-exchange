/*
  This file is part of TALER
  Copyright (C) 2014, 2015, 2016 GNUnet e.V. and INRIA

  TALER is free software; you can redistribute it and/or modify it under the
  terms of the GNU Lesser General Public License as published by the Free Software
  Foundation; either version 2.1, or (at your option) any later version.

  TALER is distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
  A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License along with
  TALER; see the file COPYING.LGPL.  If not, If not, see <http://www.gnu.org/licenses/>
*/
/**
 * @file src/benchmark/taler-exchange-benchmark.c
 * @brief exchange's benchmark
 * @author Marcello Stanisci
 */
#include "platform.h"
#include "taler_util.h"
#include "taler_signatures.h"
#include "taler_exchange_service.h"
#include "taler_json_lib.h"
#include <gnunet/gnunet_util_lib.h>
#include <gnunet/gnunet_curl_lib.h>
#include <microhttpd.h>

/**
 * How many coins the benchmark should operate on
 */
static unsigned int pool_size = 100;

/**
 * How many reservers ought to be created given the pool size
 */
static unsigned int nreserves;

/**
 * Needed information for a reserve. Other values are the same for all reserves, therefore defined in global variables
 */
struct Reserve {
   /**
   * Set (by the interpreter) to the reserve's private key
   * we used to fill the reserve.
   */
  struct TALER_ReservePrivateKeyP reserve_priv;

  /**
   * Set to the API's handle during the operation.
   */
  struct TALER_EXCHANGE_AdminAddIncomingHandle *aih;

};


/**
 * Same blinding key for all coins
 */
struct TALER_DenominationBlindingKeyP blinding_key;

/**
 * Information regarding a coin
 */
struct Coin {
  /**
   * Index in the reserve's global array indicating which
   * reserve this coin is to be retrieved
   */
  unsigned int reserve_index;

  /**
   * If @e amount is NULL, this specifies the denomination key to
   * use.  Otherwise, this will be set (by the interpreter) to the
   * denomination PK matching @e amount.
   */
  const struct TALER_EXCHANGE_DenomPublicKey *pk;

  /**
   * Array of denomination keys needed in case this coin is to be
   * refreshed
   */
  const struct TALER_EXCHANGE_DenomPublicKey **refresh_pk;

  /**
   * Set (by the interpreter) to the exchange's signature over the
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
  struct TALER_DenominationBlindingKeyP blinding_key;

  /**
   * Withdraw handle (while operation is running).
   */
  struct TALER_EXCHANGE_ReserveWithdrawHandle *wsh;

  /**
   * Deposit handle (while operation is running).
   */
  struct TALER_EXCHANGE_DepositHandle *dh;

  /**
   * Refresh melt handle
   */
  struct TALER_EXCHANGE_RefreshMeltHandle *rmh;

  /**
   * Refresh reveal handle
   */
  struct TALER_EXCHANGE_RefreshRevealHandle *rrh;

};

/**
 * Context for running the #ctx's event loop.
 */
static struct GNUNET_CURL_RescheduleContext *rc;


/**
 * Exchange's keys
 */
static const struct TALER_EXCHANGE_Keys *keys;

/**
 * Benchmark's task
 */
struct GNUNET_SCHEDULER_Task *benchmark_task;

/**
 * Main execution context for the main loop of the exchange.
 */
static struct GNUNET_CURL_Context *ctx;

/**
 * Handle to access the exchange.
 */
static struct TALER_EXCHANGE_Handle *exchange;

/**
 * The array of all reserves
 */
static struct Reserve *reserves;

/**
 * The array of all coins
 */
static struct Coin *coins;

/**
 * Indices of spent coins (the first element always indicates
 * the total number of elements, including itself)
 */
static unsigned int *spent_coins;

/**
 * Current number of spent coins
 */
static unsigned int spent_coins_size = 0;

/**
 * Transaction id counter
 */
static unsigned int transaction_id = 0;

/**
 * This key (usually provided by merchants) is needed when depositing coins,
 * even though there is no merchant acting in the benchmark
 */
static struct TALER_MerchantPrivateKeyP merchant_priv;

/**
 * URI under which the exchange is reachable during the benchmark.
 */
#define EXCHANGE_URI "http://localhost:8081"

/**
 * How many coins (AKA withdraw operations) per reserve should be withdrawn
 */
#define COINS_PER_RESERVE 12

/**
 * Used currency (to be preferably gotten via config file, together
 * exchange URI and other needed values)
 */
#define CURRENCY "PUDOS"


/**
 * Large enough value to allow having 12 coins per reserve without parsing
 * /keys in the first place
 */
#define RESERVE_AMOUNT CURRENCY":1000"

/**
 * Probability a coin can be spent
 */
#define SPEND_PROBABILITY 0.1

/**
 * Probability a coin can be refreshed
 */
#define REFRESH_PROBABILITY 0.1

/**
 * Refreshed once. For each batch of deposits, only one
 * coin will be refreshed, according to #REFRESH_PROBABILITY
 */
static unsigned int refreshed_once = GNUNET_NO;

static unsigned int
eval_probability (float probability)
{
  unsigned int random;
  float random_01;

  random = GNUNET_CRYPTO_random_u32 (GNUNET_CRYPTO_QUALITY_WEAK, UINT32_MAX);
  random_01 = (float) random / UINT32_MAX;
  return random_01 <= probability ? GNUNET_OK : GNUNET_NO;
}


static void
do_shutdown (void *cls);


/**
 * Shutdown benchmark in case of errors
 *
 * @param msg error message to print in logs
 */
static void
fail (const char *msg)
{
  GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
              "%s\n",
              msg);
  GNUNET_SCHEDULER_shutdown ();
}


/**
 * Find denomination key matching the given amount.
 *
 * @param keys array of keys to search
 * @param amount coin value to look for
 * @return NULL if no matching key was found
 */
static const struct TALER_EXCHANGE_DenomPublicKey *
find_pk (const struct TALER_EXCHANGE_Keys *keys,
         const struct TALER_Amount *amount)
{
  unsigned int i;
  struct GNUNET_TIME_Absolute now;
  struct TALER_EXCHANGE_DenomPublicKey *pk;
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
    pk = &keys->denom_keys[i];
    if ( (0 == TALER_amount_cmp (amount,
                                 &pk->value)) &&
         ( (now.abs_value_us < pk->valid_from.abs_value_us) ||
           (now.abs_value_us > pk->withdraw_valid_until.abs_value_us) ) )
    {
      GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                  "Have denomination key for `%s', but with wrong expiration range %llu vs [%llu,%llu)\n",
                  str,
                  (unsigned long long) now.abs_value_us,
                  (unsigned long long) pk->valid_from.abs_value_us,
                  (unsigned long long) pk->withdraw_valid_until.abs_value_us);
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
 * Function called with the result of a /deposit operation.
 *
 * @param cls closure with the interpreter state
 * @param http_status HTTP response code, #MHD_HTTP_OK (200) for successful deposit;
 *                    0 if the exchange's reply is bogus (fails to follow the protocol)
 * @param exchange_pub public key used by the exchange for signing
 * @param obj the received JSON reply, should be kept as proof (and, in case of errors,
 *            be forwarded to the customer)
 */
static void
deposit_cb (void *cls,
            unsigned int http_status,
            const struct TALER_ExchangePublicKeyP *exchange_pub,
            const json_t *obj)
{
  unsigned int coin_index = (unsigned int) (long) cls;

  coins[coin_index].dh = NULL;
  if (MHD_HTTP_OK != http_status)
  {
    fail ("At least one coin has not been deposited, status: %d\n");
    return;
  }
  GNUNET_log (GNUNET_ERROR_TYPE_INFO, "Coin #%d correctly spent!\n", coin_index);
  GNUNET_array_append (spent_coins, spent_coins_size, coin_index);
  spent_coins_size++;
  if (GNUNET_YES == eval_probability (REFRESH_PROBABILITY)
      && GNUNET_NO == refreshed_once)
  {
    /* TODO: all the refresh logic here */
    refreshed_once = GNUNET_YES;
  
  }
}

/**
 * Function called upon completion of our /reserve/withdraw request.
 * This is merely the function which spends withdrawn coins
 *
 * @param cls closure with the interpreter state
 * @param http_status HTTP response code, #MHD_HTTP_OK (200) for successful status request
 *                    0 if the exchange's reply is bogus (fails to follow the protocol)
 * @param sig signature over the coin, NULL on error
 * @param full_response full response from the exchange (for logging, in case of errors)
 */
static void
reserve_withdraw_cb (void *cls,
                     unsigned int http_status,
                     const struct TALER_DenominationSignature *sig,
                     const json_t *full_response)
{

  unsigned int coin_index = (unsigned int) (long) cls;

  coins[coin_index].wsh = NULL;
  if (MHD_HTTP_OK != http_status)
    fail ("At least one coin has not correctly been withdrawn\n");
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "%d-th coin withdrawn\n",
              coin_index);
  coins[coin_index].sig.rsa_signature =
    GNUNET_CRYPTO_rsa_signature_dup (sig->rsa_signature);
  if (GNUNET_OK == eval_probability (SPEND_PROBABILITY))
  {
    struct TALER_Amount amount;
    struct GNUNET_TIME_Absolute wire_deadline;
    struct GNUNET_TIME_Absolute timestamp;
    struct GNUNET_TIME_Absolute refund_deadline;
    struct GNUNET_HashCode h_contract;
    json_t *merchant_details;
    struct TALER_CoinSpendPublicKeyP coin_pub;
    struct TALER_DepositRequestPS dr;
    struct TALER_MerchantPublicKeyP merchant_pub;
    struct TALER_CoinSpendSignatureP coin_sig;

    GNUNET_CRYPTO_eddsa_key_get_public (&coins[coin_index].coin_priv.eddsa_priv,
                                        &coin_pub.eddsa_pub);
    GNUNET_CRYPTO_random_block (GNUNET_CRYPTO_QUALITY_WEAK,
                                &h_contract,
                                sizeof (h_contract));
    timestamp = GNUNET_TIME_absolute_get ();
    wire_deadline = GNUNET_TIME_absolute_add (timestamp, GNUNET_TIME_UNIT_WEEKS);
    refund_deadline = GNUNET_TIME_absolute_add (timestamp, GNUNET_TIME_UNIT_DAYS);
    GNUNET_TIME_round_abs (&timestamp);
    GNUNET_TIME_round_abs (&wire_deadline);
    GNUNET_TIME_round_abs (&refund_deadline);
    GNUNET_log (GNUNET_ERROR_TYPE_DEBUG, "Spending %d-th coin\n", coin_index);
    TALER_amount_subtract (&amount,
                           &coins[coin_index].pk->value,
                           &coins[coin_index].pk->fee_deposit);
    merchant_details = json_loads ("{ \"type\":\"test\", \"bank_uri\":\"https://bank.test.taler.net/\", \"account_number\":63}",
                               JSON_REJECT_DUPLICATES,
                               NULL);

    memset (&dr, 0, sizeof (dr));
    dr.purpose.size = htonl (sizeof (struct TALER_DepositRequestPS));
    dr.purpose.purpose = htonl (TALER_SIGNATURE_WALLET_COIN_DEPOSIT);
    dr.h_contract = h_contract;
    TALER_JSON_hash (merchant_details,
                     &dr.h_wire);

    dr.timestamp = GNUNET_TIME_absolute_hton (timestamp);
    dr.refund_deadline = GNUNET_TIME_absolute_hton (refund_deadline);
    dr.transaction_id = GNUNET_htonll (transaction_id);

    TALER_amount_hton (&dr.amount_with_fee,
                       &amount);
    TALER_amount_hton (&dr.deposit_fee,
                       &coins[coin_index].pk->fee_deposit);

    GNUNET_CRYPTO_eddsa_key_get_public (&merchant_priv.eddsa_priv,
                                        &merchant_pub.eddsa_pub);
    dr.merchant = merchant_pub;
    dr.coin_pub = coin_pub;
    GNUNET_assert (GNUNET_OK ==
                   GNUNET_CRYPTO_eddsa_sign (&coins[coin_index].coin_priv.eddsa_priv,
                                             &dr.purpose,
                                             &coin_sig.eddsa_signature));

    coins[coin_index].dh = TALER_EXCHANGE_deposit (exchange,
                                                   &amount,
                                                   wire_deadline,
                                                   merchant_details,
                                                   &h_contract,
                                                   &coin_pub,
                                                   &coins[coin_index].sig,
                                                   &coins[coin_index].pk->key,
                                                   timestamp,
                                                   transaction_id,
                                                   &merchant_pub,
                                                   refund_deadline,
                                                   &coin_sig,
                                                   &deposit_cb,
                                                   (void *) (long) coin_index);
    if (NULL == coins[coin_index].dh)
    {
      json_decref (merchant_details);
      fail ("An error occurred while calling deposit API\n");
    }
    json_decref (merchant_details);
    transaction_id++;
  }
}


/**
 * Function called upon completion of our /admin/add/incoming request.
 * Its duty is withdrawing coins on the freshly created reserve.
 *
 * @param cls closure with the interpreter state
 * @param http_status HTTP response code, #MHD_HTTP_OK (200) for successful status request
 *                    0 if the exchange's reply is bogus (fails to follow the protocol)
 * @param full_response full response from the exchange (for logging, in case of errors)
 */
static void
add_incoming_cb (void *cls,
                 unsigned int http_status,
                 const json_t *full_response)
{
  unsigned int reserve_index = (unsigned int) (long) cls;
  struct GNUNET_CRYPTO_EddsaPrivateKey *coin_priv;
  unsigned int i;
  unsigned int coin_index;
  struct TALER_Amount amount;

  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "/admin/add/incoming callback called on %d-th reserve\n",
              reserve_index);
  reserves[reserve_index].aih = NULL;
  if (MHD_HTTP_OK != http_status)
    fail ("At least one reserve failed in being created\n");

  for (i=0; i < COINS_PER_RESERVE; i++)
  {
    coin_priv = GNUNET_CRYPTO_eddsa_key_create ();
    coin_index = reserve_index * COINS_PER_RESERVE + i;
    coins[coin_index].coin_priv.eddsa_priv = *coin_priv;
    coins[coin_index].reserve_index = reserve_index;
    TALER_string_to_amount (CURRENCY":5", &amount);
    GNUNET_assert (NULL != (coins[coin_index].pk = find_pk (keys, &amount)));
    GNUNET_free (coin_priv);
    coins[coin_index].wsh =
      TALER_EXCHANGE_reserve_withdraw (exchange,
                                       coins[coin_index].pk,
                                       &reserves[reserve_index].reserve_priv,
                                       &coins[coin_index].coin_priv,
                                       &blinding_key,
                                       reserve_withdraw_cb,
                                       (void *) (long) coin_index);
  }
}


/**
 * Benchmark runner.
 *
 * @param cls closure for benchmark_run()
 */
static void
benchmark_run (void *cls)
{
  unsigned int i;
  struct GNUNET_CRYPTO_EddsaPrivateKey *priv;
  json_t *transfer_details;
  json_t *sender_details;
  char *uuid;
  struct TALER_ReservePublicKeyP reserve_pub;
  struct GNUNET_TIME_Absolute execution_date;
  struct TALER_Amount reserve_amount;

  priv = GNUNET_CRYPTO_eddsa_key_create ();
  merchant_priv.eddsa_priv = *priv;
  GNUNET_free (priv);

  GNUNET_CRYPTO_random_block (GNUNET_CRYPTO_QUALITY_WEAK,
                              &blinding_key,
                              sizeof (blinding_key));
  TALER_string_to_amount (RESERVE_AMOUNT, &reserve_amount);
  sender_details = json_loads ("{ \"type\":\"test\", \"bank_uri\":\"https://bank.test.taler.net/\", \"account_number\":62}",
                               JSON_REJECT_DUPLICATES,
                               NULL);
  execution_date = GNUNET_TIME_absolute_get ();
  GNUNET_TIME_round_abs (&execution_date);

  GNUNET_log (GNUNET_ERROR_TYPE_INFO,
              "benchmark_run() invoked\n");
  GNUNET_log (GNUNET_ERROR_TYPE_INFO,
              "gotten pool_size of %d\n",
              pool_size);
  nreserves = pool_size / COINS_PER_RESERVE;
  GNUNET_log (GNUNET_ERROR_TYPE_INFO,
              "creating %d reserves\n",
              nreserves);

  reserves = GNUNET_new_array (nreserves,
                               struct Reserve);
  coins = GNUNET_new_array (COINS_PER_RESERVE * nreserves,
                            struct Coin);

  for (i=0;i < nreserves && 0 < nreserves;i++)
  {
    priv = GNUNET_CRYPTO_eddsa_key_create ();
    reserves[i].reserve_priv.eddsa_priv = *priv;
    GNUNET_free (priv);
    GNUNET_asprintf (&uuid, "{ \"uuid\":%d}", i);
    transfer_details = json_loads (uuid, JSON_REJECT_DUPLICATES, NULL);
    GNUNET_free (uuid);
    GNUNET_CRYPTO_eddsa_key_get_public (&reserves[i].reserve_priv.eddsa_priv,
                                        &reserve_pub.eddsa_pub);

    reserves[i].aih = TALER_EXCHANGE_admin_add_incoming (exchange,
                                                         &reserve_pub,
                                                         &reserve_amount,
                                                         execution_date,
                                                         sender_details,
                                                         transfer_details,
                                                         &add_incoming_cb,
                                                         (void *) (long) i);
    GNUNET_assert (NULL != reserves[i].aih);
    json_decref (transfer_details);
  }
  json_decref (sender_details);
  GNUNET_log (GNUNET_ERROR_TYPE_INFO,
              "benchmark_run() returns\n");
}


/**
 * Functions of this type are called to provide the retrieved signing and
 * denomination keys of the exchange.  No TALER_EXCHANGE_*() functions should be called
 * in this callback.
 *
 * @param cls closure
 * @param _keys information about keys of the exchange. The _ is there because
 * there is a global 'keys' variable, and this function has to set it.
 */
static void
cert_cb (void *cls,
         const struct TALER_EXCHANGE_Keys *_keys)
{
  /* check that keys is OK */
#define ERR(cond) do { if(!(cond)) break; GNUNET_break (0); GNUNET_SCHEDULER_shutdown(); return; } while (0)
  ERR (NULL == _keys);
  ERR (0 == _keys->num_sign_keys);
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Read %u signing keys\n",
              _keys->num_sign_keys);
  ERR (0 == _keys->num_denom_keys);
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Read %u denomination keys\n",
              _keys->num_denom_keys);
#undef ERR

  GNUNET_log (GNUNET_ERROR_TYPE_INFO,
	      "Certificate callback invoked, invoking benchmark_run()\n");
  keys = _keys;
  benchmark_task = GNUNET_SCHEDULER_add_now (&benchmark_run,
                                             NULL);
}


/**
 * Function run when the benchmark terminates (good or bad).
 * Cleans up our state.
 *
 * @param cls the interpreter state.
 */
static void
do_shutdown (void *cls)
{
  unsigned int i;

  GNUNET_log (GNUNET_ERROR_TYPE_INFO, "shutting down..\n");

  /**
   * WARNING: all the non NULL handles must correspond to non completed
   * calls (AKA calls for which the callback function has not been called).
   * If not, it segfaults
   */
  for (i=0; i<nreserves && 0<nreserves; i++)
  {
    if (NULL != reserves[i].aih)
    {
      GNUNET_log (GNUNET_ERROR_TYPE_INFO, "Cancelling %d-th reserve\n", i);
      TALER_EXCHANGE_admin_add_incoming_cancel(reserves[i].aih);
      reserves[i].aih = NULL;
    }
  }

  for (i=0; i<COINS_PER_RESERVE * nreserves && 0<nreserves; i++)
  {
    if (NULL != coins[i].wsh)
    {
      TALER_EXCHANGE_reserve_withdraw_cancel(coins[i].wsh);
      coins[i].wsh = NULL;
    }
    if (NULL != coins[i].dh)
    {
      TALER_EXCHANGE_deposit_cancel(coins[i].dh);
      coins[i].dh = NULL;
    }
    if (NULL != coins[i].rmh)
    {
      TALER_EXCHANGE_refresh_melt_cancel(coins[i].rmh);
      coins[i].rmh = NULL;    
    }
    if (NULL != coins[i].rrh)
    {
      TALER_EXCHANGE_refresh_reveal_cancel(coins[i].rrh);
      coins[i].rmh = NULL;    
    }
    if (NULL != coins[i].refresh_pk)
    {
      GNUNET_free (coins[i].refresh_pk);
    }

  }

  GNUNET_free_non_null (reserves);
  GNUNET_free_non_null (coins);
  GNUNET_free_non_null (spent_coins);

  if (NULL != exchange)
  {
    TALER_EXCHANGE_disconnect (exchange);
    exchange = NULL;
  }
  if (NULL != ctx)
  {
    GNUNET_CURL_fini (ctx);
    ctx = NULL;
  }
  if (NULL != rc)
  {
    GNUNET_CURL_gnunet_rc_destroy (rc);
    rc = NULL;
  }
}


/**
 * Main function that will be run by the scheduler.
 *
 * @param cls closure
 */
static void
run (void *cls)
{
  GNUNET_log (GNUNET_ERROR_TYPE_INFO,
              "running run()\n");
  GNUNET_array_append (spent_coins,
                       spent_coins_size,
                       1);
  spent_coins_size++;

  reserves = NULL;
  coins = NULL;
  ctx = GNUNET_CURL_init (&GNUNET_CURL_gnunet_scheduler_reschedule,
                          &rc);
  GNUNET_assert (NULL != ctx);
  rc = GNUNET_CURL_gnunet_rc_create (ctx);
  exchange = TALER_EXCHANGE_connect (ctx,
                                     EXCHANGE_URI,
                                     &cert_cb, NULL,
                                     TALER_EXCHANGE_OPTION_END);
  GNUNET_assert (NULL != exchange);
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG, "connected to exchange\n");
  GNUNET_SCHEDULER_add_shutdown (&do_shutdown, NULL);
}


int
main (int argc,
      char * const *argv)
{

  GNUNET_log_setup ("taler-exchange-benchmark",
                    "WARNING",
                    NULL);
  const struct GNUNET_GETOPT_CommandLineOption options[] = {
    {'s', "pool-size", NULL,
     "How many coins this benchmark should instantiate", GNUNET_YES,
     &GNUNET_GETOPT_set_uint, &pool_size}
    };

  GNUNET_assert (GNUNET_SYSERR !=
                   GNUNET_GETOPT_run ("taler-exchange-benchmark",
                                      options, argc, argv));
  GNUNET_SCHEDULER_run (&run, NULL);
  return GNUNET_OK;
}
