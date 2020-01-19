/*
  This file is part of TALER
  Copyright (C) 2017 GNUnet e.V. and Inria

  TALER is free software; you can redistribute it and/or modify it under the
  terms of the GNU General Public License as published by the Free Software
  Foundation; either version 3, or (at your option) any later version.

  TALER is distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
  A PARTICULAR PURPOSE.  See the GNU General Public License for more details.

  You should have received a copy of the GNU General Public License along with
  TALER; see the file COPYING.  If not, see
  <http://www.gnu.org/licenses/>
*/
/**
 * @file lib/exchange_api_recoup.c
 * @brief Implementation of the /recoup request of the exchange's HTTP API
 * @author Christian Grothoff
 */
#include "platform.h"
#include <jansson.h>
#include <microhttpd.h> /* just for HTTP status codes */
#include <gnunet/gnunet_util_lib.h>
#include <gnunet/gnunet_json_lib.h>
#include <gnunet/gnunet_curl_lib.h>
#include "taler_json_lib.h"
#include "taler_exchange_service.h"
#include "exchange_api_handle.h"
#include "taler_signatures.h"
#include "exchange_api_curl_defaults.h"


/**
 * @brief A Recoup Handle
 */
struct TALER_EXCHANGE_RecoupHandle
{

  /**
   * The connection to exchange this request handle will use
   */
  struct TALER_EXCHANGE_Handle *exchange;

  /**
   * The url for this request.
   */
  char *url;

  /**
   * Context for #TEH_curl_easy_post(). Keeps the data that must
   * persist for Curl to make the upload.
   */
  struct TALER_CURL_PostContext ctx;

  /**
   * Denomination key of the coin.
   */
  struct TALER_EXCHANGE_DenomPublicKey pk;

  /**
   * Handle for the request.
   */
  struct GNUNET_CURL_Job *job;

  /**
   * Function to call with the result.
   */
  TALER_EXCHANGE_RecoupResultCallback cb;

  /**
   * Closure for @a cb.
   */
  void *cb_cls;

  /**
   * Public key of the coin we are trying to get paid back.
   */
  struct TALER_CoinSpendPublicKeyP coin_pub;

  /**
   * #GNUNET_YES if the coin was refreshed
   */
  int was_refreshed;

};


/**
 * Verify that the signature on the "200 OK" response
 * from the exchange is valid. If it is, call the
 * callback.
 *
 * @param ph recoup handle
 * @param json json reply with the signature
 * @return #GNUNET_OK if the signature is valid and we called the callback;
 *         #GNUNET_SYSERR if not (callback must still be called)
 */
static int
verify_recoup_signature_ok (const struct TALER_EXCHANGE_RecoupHandle *ph,
                            const json_t *json)
{
  struct TALER_RecoupConfirmationPS pc;
  struct TALER_RecoupRefreshConfirmationPS pr;
  struct TALER_ExchangePublicKeyP exchange_pub;
  struct TALER_ExchangeSignatureP exchange_sig;
  struct TALER_Amount amount;
  struct GNUNET_TIME_Absolute timestamp;
  const struct TALER_EXCHANGE_Keys *key_state;
  struct GNUNET_JSON_Specification spec_withdraw[] = {
    GNUNET_JSON_spec_fixed_auto ("exchange_sig", &exchange_sig),
    GNUNET_JSON_spec_fixed_auto ("exchange_pub", &exchange_pub),
    TALER_JSON_spec_amount ("amount", &amount),
    GNUNET_JSON_spec_absolute_time ("timestamp", &timestamp),
    GNUNET_JSON_spec_fixed_auto ("reserve_pub", &pc.reserve_pub),
    GNUNET_JSON_spec_end ()
  };
  struct GNUNET_JSON_Specification spec_refresh[] = {
    GNUNET_JSON_spec_fixed_auto ("exchange_sig", &exchange_sig),
    GNUNET_JSON_spec_fixed_auto ("exchange_pub", &exchange_pub),
    TALER_JSON_spec_amount ("amount", &amount),
    GNUNET_JSON_spec_absolute_time ("timestamp", &timestamp),
    GNUNET_JSON_spec_fixed_auto ("old_coin_pub", &pr.old_coin_pub),
    GNUNET_JSON_spec_end ()
  };

  if (GNUNET_OK !=
      GNUNET_JSON_parse (json,
                         ph->was_refreshed ? spec_refresh : spec_withdraw,
                         NULL, NULL))
  {
    GNUNET_break_op (0);
    return GNUNET_SYSERR;
  }
  key_state = TALER_EXCHANGE_get_keys (ph->exchange);
  if (GNUNET_OK !=
      TALER_EXCHANGE_test_signing_key (key_state,
                                       &exchange_pub))
  {
    GNUNET_break_op (0);
    return GNUNET_SYSERR;
  }
  if (ph->was_refreshed)
  {
    pr.purpose.purpose = htonl (
      TALER_SIGNATURE_EXCHANGE_CONFIRM_RECOUP_REFRESH);
    pr.purpose.size = htonl (sizeof (pr));
    pr.timestamp = GNUNET_TIME_absolute_hton (timestamp);
    TALER_amount_hton (&pr.recoup_amount,
                       &amount);
    pr.coin_pub = ph->coin_pub;
    if (GNUNET_OK !=
        GNUNET_CRYPTO_eddsa_verify (
          TALER_SIGNATURE_EXCHANGE_CONFIRM_RECOUP_REFRESH,
          &pr.purpose,
          &exchange_sig.eddsa_signature,
          &exchange_pub.eddsa_pub))
    {
      GNUNET_break_op (0);
      return GNUNET_SYSERR;
    }
  }
  else
  {
    pc.purpose.purpose = htonl (TALER_SIGNATURE_EXCHANGE_CONFIRM_RECOUP);
    pc.purpose.size = htonl (sizeof (pc));
    pc.timestamp = GNUNET_TIME_absolute_hton (timestamp);
    TALER_amount_hton (&pc.recoup_amount,
                       &amount);
    pc.coin_pub = ph->coin_pub;
    if (GNUNET_OK !=
        GNUNET_CRYPTO_eddsa_verify (TALER_SIGNATURE_EXCHANGE_CONFIRM_RECOUP,
                                    &pc.purpose,
                                    &exchange_sig.eddsa_signature,
                                    &exchange_pub.eddsa_pub))
    {
      GNUNET_break_op (0);
      return GNUNET_SYSERR;
    }
  }
  ph->cb (ph->cb_cls,
          MHD_HTTP_OK,
          TALER_EC_NONE,
          &amount,
          timestamp,
          ph->was_refreshed ? NULL : &pc.reserve_pub,
          ph->was_refreshed ? &pr.old_coin_pub : NULL,
          json);
  return GNUNET_OK;
}


/**
 * Function called when we're done processing the
 * HTTP /recoup request.
 *
 * @param cls the `struct TALER_EXCHANGE_RecoupHandle`
 * @param response_code HTTP response code, 0 on error
 * @param response parsed JSON result, NULL on error
 */
static void
handle_recoup_finished (void *cls,
                        long response_code,
                        const void *response)
{
  struct TALER_EXCHANGE_RecoupHandle *ph = cls;
  const json_t *j = response;

  ph->job = NULL;
  switch (response_code)
  {
  case 0:
    break;
  case MHD_HTTP_OK:
    if (GNUNET_OK !=
        verify_recoup_signature_ok (ph,
                                    j))
    {
      GNUNET_break_op (0);
      response_code = 0;
      break;
    }
    TALER_EXCHANGE_recoup_cancel (ph);
    return;
  case MHD_HTTP_BAD_REQUEST:
    /* This should never happen, either us or the exchange is buggy
       (or API version conflict); just pass JSON reply to the application */
    break;
  case MHD_HTTP_CONFLICT:
    {
      /* Insufficient funds, proof attached */
      json_t *history;
      struct TALER_Amount total;
      const struct TALER_EXCHANGE_DenomPublicKey *dki;

      dki = &ph->pk;
      history = json_object_get (j,
                                 "history");
      if (GNUNET_OK !=
          TALER_EXCHANGE_verify_coin_history (dki,
                                              dki->fee_deposit.currency,
                                              &ph->coin_pub,
                                              history,
                                              &total))
      {
        GNUNET_break_op (0);
        response_code = 0;
      }
      ph->cb (ph->cb_cls,
              response_code,
              TALER_JSON_get_error_code (j),
              &total,
              GNUNET_TIME_UNIT_FOREVER_ABS,
              NULL,
              NULL,
              j);
      TALER_EXCHANGE_recoup_cancel (ph);
      return;
    }
  case MHD_HTTP_FORBIDDEN:
    /* Nothing really to verify, exchange says one of the signatures is
       invalid; as we checked them, this should never happen, we
       should pass the JSON reply to the application */
    break;
  case MHD_HTTP_NOT_FOUND:
    /* Nothing really to verify, this should never
       happen, we should pass the JSON reply to the application */
    break;
  case MHD_HTTP_GONE:
    /* Kind of normal: the money was already sent to the merchant
       (it was too late for the refund). */
    break;
  case MHD_HTTP_INTERNAL_SERVER_ERROR:
    /* Server had an internal issue; we should retry, but this API
       leaves this to the application */
    break;
  default:
    /* unexpected response code */
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Unexpected response code %u\n",
                (unsigned int) response_code);
    GNUNET_break (0);
    response_code = 0;
    break;
  }
  ph->cb (ph->cb_cls,
          response_code,
          TALER_JSON_get_error_code (j),
          NULL,
          GNUNET_TIME_UNIT_FOREVER_ABS,
          NULL,
          NULL,
          j);
  TALER_EXCHANGE_recoup_cancel (ph);
}


/**
 * Ask the exchange to pay back a coin due to the exchange triggering
 * the emergency recoup protocol for a given denomination.  The value
 * of the coin will be refunded to the original customer (without fees).
 *
 * @param exchange the exchange handle; the exchange must be ready to operate
 * @param pk kind of coin to pay back
 * @param denom_sig signature over the coin by the exchange using @a pk
 * @param ps secret internals of the original planchet
 * @param was_refreshed #GNUNET_YES if the coin in @a ps was refreshed
 * @param recoup_cb the callback to call when the final result for this request is available
 * @param recoup_cb_cls closure for @a recoup_cb
 * @return NULL
 *         if the inputs are invalid (i.e. denomination key not with this exchange).
 *         In this case, the callback is not called.
 */
struct TALER_EXCHANGE_RecoupHandle *
TALER_EXCHANGE_recoup (struct TALER_EXCHANGE_Handle *exchange,
                       const struct TALER_EXCHANGE_DenomPublicKey *pk,
                       const struct TALER_DenominationSignature *denom_sig,
                       const struct TALER_PlanchetSecretsP *ps,
                       int was_refreshed,
                       TALER_EXCHANGE_RecoupResultCallback recoup_cb,
                       void *recoup_cb_cls)
{
  struct TALER_EXCHANGE_RecoupHandle *ph;
  struct GNUNET_CURL_Context *ctx;
  struct TALER_RecoupRequestPS pr;
  struct TALER_CoinSpendSignatureP coin_sig;
  struct GNUNET_HashCode h_denom_pub;
  json_t *recoup_obj;
  CURL *eh;

  GNUNET_assert (GNUNET_YES ==
                 TEAH_handle_is_ready (exchange));
  pr.purpose.purpose = htonl (TALER_SIGNATURE_WALLET_COIN_RECOUP);
  pr.purpose.size = htonl (sizeof (struct TALER_RecoupRequestPS));
  GNUNET_CRYPTO_eddsa_key_get_public (&ps->coin_priv.eddsa_priv,
                                      &pr.coin_pub.eddsa_pub);
  GNUNET_CRYPTO_rsa_public_key_hash (pk->key.rsa_public_key,
                                     &h_denom_pub);
  pr.h_denom_pub = pk->h_key;
  pr.coin_blind = ps->blinding_key;
  GNUNET_assert (GNUNET_OK ==
                 GNUNET_CRYPTO_eddsa_sign (&ps->coin_priv.eddsa_priv,
                                           &pr.purpose,
                                           &coin_sig.eddsa_signature));

  recoup_obj = json_pack ("{s:o, s:o," /* denom pub/sig */
                          " s:o, s:o,"  /* coin pub/sig */
                          " s:o, s:o}",  /* coin_bks */
                          "denom_pub_hash", GNUNET_JSON_from_data_auto (
                            &h_denom_pub),
                          "denom_sig", GNUNET_JSON_from_rsa_signature (
                            denom_sig->rsa_signature),
                          "coin_pub", GNUNET_JSON_from_data_auto (
                            &pr.coin_pub),
                          "coin_sig", GNUNET_JSON_from_data_auto (&coin_sig),
                          "coin_blind_key_secret", GNUNET_JSON_from_data_auto (
                            &ps->blinding_key),
                          "refreshed", json_boolean (was_refreshed)
                          );
  if (NULL == recoup_obj)
  {
    GNUNET_break (0);
    return NULL;
  }

  ph = GNUNET_new (struct TALER_EXCHANGE_RecoupHandle);
  ph->coin_pub = pr.coin_pub;
  ph->exchange = exchange;
  ph->pk = *pk;
  ph->pk.key.rsa_public_key = NULL; /* zero out, as lifetime cannot be warranted */
  ph->cb = recoup_cb;
  ph->cb_cls = recoup_cb_cls;
  ph->url = TEAH_path_to_url (exchange, "/recoup");
  ph->was_refreshed = was_refreshed;
  eh = TEL_curl_easy_get (ph->url);
  if (GNUNET_OK !=
      TALER_curl_easy_post (&ph->ctx,
                            eh,
                            recoup_obj))
  {
    GNUNET_break (0);
    curl_easy_cleanup (eh);
    json_decref (recoup_obj);
    GNUNET_free (ph->url);
    GNUNET_free (ph);
    return NULL;
  }
  json_decref (recoup_obj);
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "URL for recoup: `%s'\n",
              ph->url);
  ctx = TEAH_handle_to_context (exchange);
  ph->job = GNUNET_CURL_job_add2 (ctx,
                                  eh,
                                  ph->ctx.headers,
                                  &handle_recoup_finished,
                                  ph);
  return ph;
}


/**
 * Cancel a recoup request.  This function cannot be used on a
 * request handle if the callback was already invoked.
 *
 * @param ph the recoup handle
 */
void
TALER_EXCHANGE_recoup_cancel (struct TALER_EXCHANGE_RecoupHandle *ph)
{
  if (NULL != ph->job)
  {
    GNUNET_CURL_job_cancel (ph->job);
    ph->job = NULL;
  }
  GNUNET_free (ph->url);
  TALER_curl_easy_post_finished (&ph->ctx);
  GNUNET_free (ph);
}


/* end of exchange_api_recoup.c */