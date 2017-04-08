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
 * @file exchange-lib/exchange_api_payback.c
 * @brief Implementation of the /payback request of the exchange's HTTP API
 * @author Christian Grothoff
 */
#include "platform.h"
#include <curl/curl.h>
#include <jansson.h>
#include <microhttpd.h> /* just for HTTP status codes */
#include <gnunet/gnunet_util_lib.h>
#include <gnunet/gnunet_json_lib.h>
#include <gnunet/gnunet_curl_lib.h>
#include "taler_json_lib.h"
#include "taler_exchange_service.h"
#include "exchange_api_handle.h"
#include "taler_signatures.h"


/**
 * @brief A Payback Handle
 */
struct TALER_EXCHANGE_PaybackHandle
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
   * JSON encoding of the request to POST.
   */
  char *json_enc;

  /**
   * Handle for the request.
   */
  struct GNUNET_CURL_Job *job;

  /**
   * Function to call with the result.
   */
  TALER_EXCHANGE_PaybackResultCallback cb;

  /**
   * Closure for @a cb.
   */
  void *cb_cls;

  /**
   * Public key of the coin we are trying to get paid back.
   */
  struct TALER_CoinSpendPublicKeyP coin_pub;

};


/**
 * Verify that the signature on the "200 OK" response
 * from the exchange is valid. If it is, call the
 * callback.
 *
 * @param ph payback handle
 * @param json json reply with the signature
 * @return #GNUNET_OK if the signature is valid and we called the callback;
 *         #GNUNET_SYSERR if not (callback must still be called)
 */
static int
verify_payback_signature_ok (const struct TALER_EXCHANGE_PaybackHandle *ph,
                             const json_t *json)
{
  struct TALER_PaybackConfirmationPS pc;
  struct TALER_ExchangePublicKeyP exchange_pub;
  struct TALER_ExchangeSignatureP exchange_sig;
  struct TALER_Amount amount;
  struct GNUNET_TIME_Absolute timestamp;
  const struct TALER_EXCHANGE_Keys *key_state;
  struct GNUNET_JSON_Specification spec[] = {
    GNUNET_JSON_spec_fixed_auto ("exchange_sig", &exchange_sig),
    GNUNET_JSON_spec_fixed_auto ("exchange_pub", &exchange_pub),
    TALER_JSON_spec_amount ("amount", &amount),
    GNUNET_JSON_spec_absolute_time ("timestamp", &timestamp),
    GNUNET_JSON_spec_fixed_auto ("reserve_pub", &pc.reserve_pub),
    GNUNET_JSON_spec_end()
  };

  if (GNUNET_OK !=
      GNUNET_JSON_parse (json,
                         spec,
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
  pc.purpose.purpose = htonl (TALER_SIGNATURE_EXCHANGE_CONFIRM_PAYBACK);
  pc.purpose.size = htonl (sizeof (pc));
  pc.timestamp = GNUNET_TIME_absolute_hton (timestamp);
  TALER_amount_hton (&pc.payback_amount,
                     &amount);
  pc.coin_pub = ph->coin_pub;
  if (GNUNET_OK !=
      GNUNET_CRYPTO_eddsa_verify (TALER_SIGNATURE_EXCHANGE_CONFIRM_PAYBACK,
                                  &pc.purpose,
                                  &exchange_sig.eddsa_signature,
                                  &exchange_pub.eddsa_pub))
  {
    GNUNET_break_op (0);
    return GNUNET_SYSERR;
  }
  ph->cb (ph->cb_cls,
          MHD_HTTP_OK,
	  TALER_EC_NONE,
          &amount,
          timestamp,
          &pc.reserve_pub,
          json);
  return GNUNET_OK;
}


/**
 * Function called when we're done processing the
 * HTTP /payback request.
 *
 * @param cls the `struct TALER_EXCHANGE_PaybackHandle`
 * @param response_code HTTP response code, 0 on error
 * @param json parsed JSON result, NULL on error
 */
static void
handle_payback_finished (void *cls,
                         long response_code,
                         const json_t *json)
{
  struct TALER_EXCHANGE_PaybackHandle *ph = cls;

  ph->job = NULL;
  switch (response_code)
  {
  case 0:
    break;
  case MHD_HTTP_OK:
    if (GNUNET_OK !=
        verify_payback_signature_ok (ph,
                                     json))
    {
      GNUNET_break_op (0);
      response_code = 0;
    }
    TALER_EXCHANGE_payback_cancel (ph);
    return;
  case MHD_HTTP_BAD_REQUEST:
    /* This should never happen, either us or the exchange is buggy
       (or API version conflict); just pass JSON reply to the application */
    break;
  case MHD_HTTP_UNAUTHORIZED:
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
	  TALER_JSON_get_error_code (json),
          NULL,
          GNUNET_TIME_UNIT_FOREVER_ABS,
          NULL,
          json);
  TALER_EXCHANGE_payback_cancel (ph);
}


/**
 * Ask the exchange to pay back a coin due to the exchange triggering
 * the emergency payback protocol for a given denomination.  The value
 * of the coin will be refunded to the original customer (without fees).
 *
 * @param exchange the exchange handle; the exchange must be ready to operate
 * @param pk kind of coin to pay back
 * @param denom_sig signature over the coin by the exchange using @a pk
 * @param coin_priv the coin's private key,
 * @param blinding_key where to fetch the coin's blinding key
 * @param payback_cb the callback to call when the final result for this request is available
 * @param payback_cb_cls closure for @a payback_cb
 * @return NULL
 *         if the inputs are invalid (i.e. denomination key not with this exchange).
 *         In this case, the callback is not called.
 */
struct TALER_EXCHANGE_PaybackHandle *
TALER_EXCHANGE_payback (struct TALER_EXCHANGE_Handle *exchange,
                        const struct TALER_EXCHANGE_DenomPublicKey *pk,
                        const struct TALER_DenominationSignature *denom_sig,
                        const struct TALER_CoinSpendPrivateKeyP *coin_priv,
                        const struct TALER_DenominationBlindingKeyP *blinding_key,
                        TALER_EXCHANGE_PaybackResultCallback payback_cb,
                        void *payback_cb_cls)
{
  struct TALER_EXCHANGE_PaybackHandle *ph;
  struct GNUNET_CURL_Context *ctx;
  struct TALER_PaybackRequestPS pr;
  struct TALER_CoinSpendSignatureP coin_sig;
  json_t *payback_obj;
  CURL *eh;

  GNUNET_assert (GNUNET_YES ==
		 MAH_handle_is_ready (exchange));
  pr.purpose.purpose = htonl (TALER_SIGNATURE_WALLET_COIN_PAYBACK);
  pr.purpose.size = htonl (sizeof (struct TALER_PaybackRequestPS));
  GNUNET_CRYPTO_eddsa_key_get_public (&coin_priv->eddsa_priv,
                                      &pr.coin_pub.eddsa_pub);
  pr.h_denom_pub = pk->h_key;
  pr.coin_blind = *blinding_key;
  GNUNET_assert (GNUNET_OK ==
                 GNUNET_CRYPTO_eddsa_sign (&coin_priv->eddsa_priv,
                                           &pr.purpose,
                                           &coin_sig.eddsa_signature));

  payback_obj = json_pack ("{s:o, s:o," /* denom pub/sig */
                           " s:o, s:o," /* coin pub/sig */
                           " s:o}", /* coin_bks */
                           "denom_pub", GNUNET_JSON_from_rsa_public_key (pk->key.rsa_public_key),
                           "denom_sig", GNUNET_JSON_from_rsa_signature (denom_sig->rsa_signature),
                           "coin_pub", GNUNET_JSON_from_data_auto (&pr.coin_pub),
                           "coin_sig", GNUNET_JSON_from_data_auto (&coin_sig),
                           "coin_blind_key_secret", GNUNET_JSON_from_data_auto (blinding_key)
			  );
  GNUNET_assert (NULL != payback_obj);

  ph = GNUNET_new (struct TALER_EXCHANGE_PaybackHandle);
  ph->coin_pub = pr.coin_pub;
  ph->exchange = exchange;
  ph->cb = payback_cb;
  ph->cb_cls = payback_cb_cls;
  ph->url = MAH_path_to_url (exchange, "/payback");

  eh = curl_easy_init ();
  GNUNET_assert (NULL != (ph->json_enc =
                          json_dumps (payback_obj,
                                      JSON_COMPACT)));
  json_decref (payback_obj);
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "URL for payback: `%s'\n",
              ph->url);
  GNUNET_assert (CURLE_OK ==
                 curl_easy_setopt (eh,
                                   CURLOPT_URL,
                                   ph->url));
  GNUNET_assert (CURLE_OK ==
                 curl_easy_setopt (eh,
                                   CURLOPT_POSTFIELDS,
                                   ph->json_enc));
  GNUNET_assert (CURLE_OK ==
                 curl_easy_setopt (eh,
                                   CURLOPT_POSTFIELDSIZE,
                                   strlen (ph->json_enc)));
  ctx = MAH_handle_to_context (exchange);
  ph->job = GNUNET_CURL_job_add (ctx,
				 eh,
				 GNUNET_YES,
				 &handle_payback_finished,
				 ph);
  return ph;
}


/**
 * Cancel a payback request.  This function cannot be used on a
 * request handle if the callback was already invoked.
 *
 * @param ph the payback handle
 */
void
TALER_EXCHANGE_payback_cancel (struct TALER_EXCHANGE_PaybackHandle *ph)
{
  if (NULL != ph->job)
  {
    GNUNET_CURL_job_cancel (ph->job);
    ph->job = NULL;
  }
  GNUNET_free (ph->url);
  GNUNET_free (ph->json_enc);
  GNUNET_free (ph);
}


/* end of exchange_api_payback.c */