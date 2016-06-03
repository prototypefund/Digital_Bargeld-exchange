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
 * Information regarding a coin; for simplicity, every
 * withdrawn coin is EUR 1
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

};

/**
 * Context for running the #ctx's event loop.
 */
static struct GNUNET_CURL_RescheduleContext *rc;

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
 * URI under which the exchange is reachable during the benchmark.
 */
#define EXCHANGE_URI "http://localhost:8081"

/**
 * How many coins (AKA withdraw operations) per reserve should be withdrawn
 */
#define COINS_PER_RESERVE 12

static void
do_shutdown(void *cls);

/**
 * Function called upon completion of our /admin/add/incoming request.
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
  /**
   * FIXME pick a way to get the "current" reserve index. It's also possible to
   * NOT use a traditional 'for' loop in the reserve creation function, but rather
   * an iterator which makes use of a global "state" of the operations, as happens
   * in test_merchant_api with 'struct InterpreterState' (look at how its 'ip' field
   * is used).
   * For now, just operate on the first reserve in order to get the coins' scaffold
   * defined and compiled
   */

  /**
   * 0. set NULL the reserve handler for this call (otherwise do_shutdown() segfaults
   * when attempting to cancel this operation, which cannot since has been served)
   * 1. Check if reserve got correctly created
   * 2. Define per-coin stuff
   */
  unsigned int reserve_index = 0; // TEMPORARY
  reserves[reserve_index].aih = NULL;

  GNUNET_log (GNUNET_ERROR_TYPE_INFO, "/admin/add/incoming callback called\n");
  return;
}

/**
 * Run the main interpreter loop that performs exchange operations.
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
  
  GNUNET_CRYPTO_random_block (GNUNET_CRYPTO_QUALITY_WEAK,
                              &blinding_key,
                              sizeof (blinding_key));
  TALER_string_to_amount ("EUR:24", &reserve_amount);
  /* FIXME bank_uri to be tuned to exchange's tastes */
  sender_details = json_loads ("{ \"type\":\"test\", \"bank_uri\":\"http://localhost/\", \"account_number\":62}",
                               JSON_REJECT_DUPLICATES,
                               NULL);
  execution_date = GNUNET_TIME_absolute_get ();
  GNUNET_TIME_round_abs (&execution_date);

  GNUNET_log (GNUNET_ERROR_TYPE_INFO, "benchmark_run() invoked\n");
  GNUNET_log (GNUNET_ERROR_TYPE_INFO, "gotten pool_size of %d\n", pool_size);
  nreserves = pool_size / COINS_PER_RESERVE;
  GNUNET_log (GNUNET_ERROR_TYPE_INFO, "creating %d reserves\n", nreserves);

  reserves = GNUNET_malloc (nreserves * sizeof (struct Reserve));
  coins = GNUNET_malloc (COINS_PER_RESERVE * nreserves * sizeof (struct Coin));

  /* reserves */
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
                                                         add_incoming_cb,
                                                         NULL);
    GNUNET_assert (NULL != reserves[i].aih);                                                         
    printf (".\n");
    json_decref (transfer_details);
  }
  json_decref (sender_details);

  /* coins */

  GNUNET_log (GNUNET_ERROR_TYPE_INFO, "benchmark_run() returns\n");
  GNUNET_SCHEDULER_shutdown ();
  return;
}

/**
 * Functions of this type are called to provide the retrieved signing and
 * denomination keys of the exchange.  No TALER_EXCHANGE_*() functions should be called
 * in this callback.
 *
 * @param cls closure
 * @param keys information about keys of the exchange
 */
static void
cert_cb (void *cls,
         const struct TALER_EXCHANGE_Keys *keys)
{
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
  GNUNET_log (GNUNET_ERROR_TYPE_INFO,
	      "Certificate callback invoked, invoking benchmark_run()\n");
  benchmark_task = GNUNET_SCHEDULER_add_now (&benchmark_run,
                                             NULL);
}

/**
 * Function run when the test terminates (good or bad).
 * Cleans up our state.
 *
 * @param cls the interpreter state.
 */
static void
do_shutdown (void *cls)
{
  unsigned int i;

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
  }

  GNUNET_free_non_null (reserves);
  GNUNET_free_non_null (coins);
}

/**
 * Main function that will be run by the scheduler.
 *
 * @param cls closure
 */
static void
run (void *cls)
{
  GNUNET_log (GNUNET_ERROR_TYPE_INFO, "running run()\n");
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
