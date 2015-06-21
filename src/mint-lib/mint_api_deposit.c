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
  TALER; see the file COPYING.  If not, If not, see
  <http://www.gnu.org/licenses/>
*/
/**
 * @file mint-lib/mint_api_deposit.c
 * @brief Implementation of the /deposit request of the mint's HTTP API
 * @author Sree Harsha Totakura <sreeharsha@totakura.in>
 * @author Christian Grothoff
 */
#include "platform.h"
#include <curl/curl.h>
#include <jansson.h>
#include <gnunet/gnunet_util_lib.h>
#include "taler_mint_service.h"
#include "mint_api_context.h"
#include "mint_api_handle.h"
#include "taler_signatures.h"


/**
 * Print JSON parsing related error information
 */
#define JSON_WARN(error)                                                \
    GNUNET_log (GNUNET_ERROR_TYPE_WARNING,                              \
                "JSON parsing failed at %s:%u: %s (%s)",                \
                __FILE__, __LINE__, error.text, error.source)


/**
 * @brief A Deposit Handle
 */
struct TALER_MINT_DepositHandle
{

  /**
   * The connection to mint this request handle will use
   */
  struct TALER_MINT_Handle *mint;

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
  struct MAC_Job *job;

  /**
   * HTTP headers for the request.
   */
  struct curl_slist *headers;

  /**
   * Function to call with the result.
   */
  TALER_MINT_DepositResultCallback cb;

  /**
   * Closure for @a cb.
   */
  void *cb_cls;

  /**
   * Download buffer
   */
  void *buf;

  /**
   * The size of the download buffer
   */
  size_t buf_size;

  /**
   * Error code (based on libc errno) if we failed to download
   * (i.e. response too large).
   */
  int eno;

};


/**
 * Function called when we're done processing the
 * HTTP /deposit request.
 *
 * @param cls the `struct TALER_MINT_DepositHandle`
 */
static void
handle_deposit_finished (void *cls,
                         CURL *eh)
{
  struct TALER_MINT_DepositHandle *dh = cls;
  unsigned int response_code;
  json_error_t error;
  json_t *json;

  json = NULL;
  if (0 == dh->eno)
  {
    json = json_loadb (dh->buf,
                       dh->buf_size,
                       JSON_REJECT_DUPLICATES | JSON_DISABLE_EOF_CHECK,
                       &error);
    if (NULL == json)
    {
      JSON_WARN (error);
      response_code = 0;
    }
  }
  if (NULL != json)
  {
    GNUNET_break (0); // FIXME: obtain response code from eh!
    response_code = 42;
  }
  switch (response_code)
  {
  /* FIXME: verify json response signatures
     (and that format matches response_code) */
  default:
    /* unexpected response code */
    GNUNET_break (0);
    response_code = 0;
    break;
  }
  dh->cb (dh->cb_cls,
          response_code,
          json);
  json_decref (json);
  TALER_MINT_deposit_cancel (dh);
}


/**
 * Verify signature information about the deposit.
 *
 * @param deposit information about the deposit
 * @return #GNUNET_OK if signatures are OK, #GNUNET_SYSERR if not
 */
static int
verify_signatures (struct TALER_MINT_Handle *mint,
                   const struct TALER_Amount *amount,
                   const struct GNUNET_HashCode *h_wire,
                   const struct GNUNET_HashCode *h_contract,
                   const struct TALER_CoinSpendPublicKeyP *coin_pub,
                   const struct TALER_DenominationSignature *denom_sig,
                   const struct TALER_DenominationPublicKey *denom_pub,
                   struct GNUNET_TIME_Absolute timestamp,
                   uint64_t transaction_id,
                   const struct TALER_MerchantPublicKeyP *merchant_pub,
                   struct GNUNET_TIME_Absolute refund_deadline,
                   const struct TALER_CoinSpendSignatureP *coin_sig)
{
  const struct TALER_MINT_Keys *key_state;
  struct TALER_DepositRequestPS dr;
  const struct TALER_MINT_DenomPublicKey *dki;
  struct TALER_CoinPublicInfo coin_info;

  key_state = TALER_MINT_get_keys (mint);
  dki = TALER_MINT_get_denomination_key (key_state,
                                         denom_pub);
  if (NULL == dki)
  {
    TALER_LOG_WARNING ("Denomination key unknown to mint\n");
    return GNUNET_SYSERR;
  }
  dr.purpose.purpose = htonl (TALER_SIGNATURE_WALLET_COIN_DEPOSIT);
  dr.purpose.size = htonl (sizeof (struct TALER_DepositRequestPS));
  dr.h_contract = *h_contract;
  dr.h_wire = *h_wire;
  dr.timestamp = GNUNET_TIME_absolute_hton (timestamp);
  dr.refund_deadline = GNUNET_TIME_absolute_hton (refund_deadline);
  dr.transaction_id = GNUNET_htonll (transaction_id);
  TALER_amount_hton (&dr.amount_with_fee,
                     amount);
  TALER_amount_hton (&dr.deposit_fee,
                     &dki->fee_deposit);
  dr.merchant = *merchant_pub;
  dr.coin_pub = *coin_pub;
  if (GNUNET_OK !=
      GNUNET_CRYPTO_eddsa_verify (TALER_SIGNATURE_WALLET_COIN_DEPOSIT,
                                  &dr.purpose,
                                  &coin_sig->eddsa_signature,
                                  &coin_pub->eddsa_pub))
  {
    TALER_LOG_WARNING ("Invalid coin signature on /deposit request\n");
    return GNUNET_SYSERR;
  }

  /* check coin signature */
  coin_info.coin_pub = *coin_pub;
  coin_info.denom_pub = *denom_pub;
  coin_info.denom_sig = *denom_sig;
  if (GNUNET_YES !=
      TALER_test_coin_valid (&coin_info))
  {
    TALER_LOG_WARNING ("Invalid coin passed for /deposit\n");
    return GNUNET_SYSERR;
  }
  if (TALER_amount_cmp (&dki->fee_deposit,
                        amount) < 0)
  {
    TALER_LOG_WARNING ("Deposit amount smaller than fee\n");
    return GNUNET_SYSERR;
  }
  return GNUNET_OK;
}


/**
 * Callback used when downloading the reply to a /deposit request.
 * Just appends all of the data to the `buf` in the
 * `struct TALER_MINT_DepositHandle` for further processing. The size of
 * the download is limited to #GNUNET_MAX_MALLOC_CHECKED, if
 * the download exceeds this size, we abort with an error.
 *
 * @param bufptr data downloaded via HTTP
 * @param size size of an item in @a bufptr
 * @param nitems number of items in @a bufptr
 * @param cls the `struct TALER_MINT_DepositHandle`
 * @return number of bytes processed from @a bufptr
 */
static int
deposit_download_cb (char *bufptr,
                     size_t size,
                     size_t nitems,
                     void *cls)
{
  struct TALER_MINT_DepositHandle *dh = cls;
  size_t msize;
  void *buf;

  if (0 == size * nitems)
  {
    /* Nothing (left) to do */
    return 0;
  }
  msize = size * nitems;
  if ( (msize + dh->buf_size) >= GNUNET_MAX_MALLOC_CHECKED)
  {
    dh->eno = ENOMEM;
    return 0; /* signals an error to curl */
  }
  dh->buf = GNUNET_realloc (dh->buf,
                            dh->buf_size + msize);
  buf = dh->buf + dh->buf_size;
  memcpy (buf, bufptr, msize);
  dh->buf_size += msize;
  return msize;
}


/**
 * Submit a deposit permission to the mint and get the mint's response.
 * Note that while we return the response verbatim to the caller for
 * further processing, we do already verify that the response is
 * well-formed (i.e. that signatures included in the response are all
 * valid).  If the mint's reply is not well-formed, we return an
 * HTTP status code of zero to @a cb.
 *
 * We also verify that the @a coin_sig is valid for this deposit
 * request, and that the @a ub_sig is a valid signature for @a
 * coin_pub.  Also, the @a mint must be ready to operate (i.e.  have
 * finished processing the /keys reply).  If either check fails, we do
 * NOT initiate the transaction with the mint and instead return NULL.
 *
 * @param mint the mint handle; the mint must be ready to operate
 * @param amount the amount to be deposited
 * @param wire the merchant’s account details, in a format supported by the mint
 * @param h_contract hash of the contact of the merchant with the customer (further details are never disclosed to the mint)
 * @param coin_pub coin’s public key
 * @param denom_pub denomination key with which the coin is signed
 * @param ub_sig mint’s unblinded signature of the coin
 * @param timestamp timestamp when the contract was finalized, must match approximately the current time of the mint
 * @param transaction_id transaction id for the transaction between merchant and customer
 * @param merchant_pub the public key of the merchant (used to identify the merchant for refund requests)
 * @param refund_deadline date until which the merchant can issue a refund to the customer via the mint (can be zero if refunds are not allowed)
 * @param coin_sig the signature made with purpose #TALER_SIGNATURE_WALLET_COIN_DEPOSIT made by the customer with the coin’s private key.
 * @param cb the callback to call when a reply for this request is available
 * @param cb_cls closure for the above callback
 * @return a handle for this request; NULL if the inputs are invalid (i.e.
 *         signatures fail to verify).  In this case, the callback is not called.
 */
struct TALER_MINT_DepositHandle *
TALER_MINT_deposit (struct TALER_MINT_Handle *mint,
                    const struct TALER_Amount *amount,
                    json_t *wire_details,
                    const struct GNUNET_HashCode *h_contract,
                    const struct TALER_CoinSpendPublicKeyP *coin_pub,
                    const struct TALER_DenominationSignature *denom_sig,
                    const struct TALER_DenominationPublicKey *denom_pub,
                    struct GNUNET_TIME_Absolute timestamp,
                    uint64_t transaction_id,
                    const struct TALER_MerchantPublicKeyP *merchant_pub,
                    struct GNUNET_TIME_Absolute refund_deadline,
                    const struct TALER_CoinSpendSignatureP *coin_sig,
                    TALER_MINT_DepositResultCallback cb,
                    void *cb_cls)
{
  struct TALER_MINT_DepositHandle *dh;
  struct TALER_MINT_Context *ctx;
  json_t *deposit_obj;
  CURL *eh;
  struct GNUNET_HashCode h_wire;

  if (GNUNET_YES !=
      MAH_handle_is_ready (mint))
  {
    GNUNET_break (0);
    return NULL;
  }
  /* initialize h_wire */
  if (GNUNET_OK !=
      TALER_hash_json (wire_details,
                       &h_wire))
  {
    GNUNET_break (0);
    return NULL;
  }

  if (GNUNET_OK !=
      verify_signatures (mint,
                         amount,
                         &h_wire,
                         h_contract,
                         coin_pub,
                         denom_sig,
                         denom_pub,
                         timestamp,
                         transaction_id,
                         merchant_pub,
                         refund_deadline,
                         coin_sig))
  {
    GNUNET_break_op (0);
    return NULL;
  }

  deposit_obj = json_pack ("{s:o, s:o," /* f/wire */
                           " s:s, s:s," /* H_wire, H_contract */
                           " s:s, s:s," /* coin_pub, denom_pub */
                           " s:s, s:s," /* ub_sig, timestamp */
                           " s:I, s:s," /* transaction id, merchant_pub */
                           " s:s, s:s}", /* refund_deadline, coin_sig */
                           "f", TALER_json_from_amount (amount),
                           "wire", wire_details,
                           "H_wire", TALER_json_from_data (&h_wire,
                                                           sizeof (h_wire)),
                           "H_contract", TALER_json_from_data (&h_contract,
                                                               sizeof (h_contract)),
                           "coin_pub", TALER_json_from_data (coin_pub,
                                                             sizeof (*coin_pub)),
                           "denom_pub", TALER_json_from_rsa_public_key (denom_pub->rsa_public_key),
                           "ub_sig", TALER_json_from_rsa_signature (denom_sig->rsa_signature),
                           "timestamp", TALER_json_from_abs (timestamp),
                           "transaction_id", (json_int_t) transaction_id,
                           "merchant_pub", TALER_json_from_data (merchant_pub,
                                                                 sizeof (*merchant_pub)),
                           "refund_deadline", TALER_json_from_abs (refund_deadline),
                           "coin_sig", TALER_json_from_data (coin_sig,
                                                             sizeof (*coin_sig))
                           );

  dh = GNUNET_new (struct TALER_MINT_DepositHandle);
  dh->mint = mint;
  dh->cb = cb;
  dh->cb_cls = cb_cls;
  dh->url = MAH_path_to_url (mint, "/deposit");
  eh = curl_easy_init ();
  GNUNET_assert (NULL != (dh->json_enc =
                          json_dumps (deposit_obj,
                                      JSON_COMPACT)));
  json_decref (deposit_obj);
  GNUNET_assert (CURLE_OK ==
                 curl_easy_setopt (eh,
                                   CURLOPT_URL,
                                   dh->url));
  GNUNET_assert (CURLE_OK ==
                 curl_easy_setopt (eh,
                                   CURLOPT_POSTFIELDS,
                                   dh->json_enc));
  GNUNET_assert (CURLE_OK ==
                 curl_easy_setopt (eh,
                                   CURLOPT_POSTFIELDSIZE,
                                   strlen (dh->json_enc)));
  GNUNET_assert (CURLE_OK ==
                 curl_easy_setopt (eh,
                                   CURLOPT_WRITEFUNCTION,
                                   &deposit_download_cb));
  GNUNET_assert (CURLE_OK ==
                 curl_easy_setopt (eh,
                                   CURLOPT_WRITEDATA,
                                   dh));
  GNUNET_assert (NULL != (dh->headers =
                          curl_slist_append (dh->headers,
                                             "Content-Type: application/json")));
  GNUNET_assert (CURLE_OK ==
                 curl_easy_setopt (eh,
                                   CURLOPT_HTTPHEADER,
                                   dh->headers));
  ctx = MAH_handle_to_context (mint);
  dh->job = MAC_job_add (ctx,
                         eh,
                         &handle_deposit_finished,
                         dh);
  return dh;
}


/**
 * Cancel a deposit permission request.  This function cannot be used
 * on a request handle if a response is already served for it.
 *
 * @param deposit the deposit permission request handle
 */
void
TALER_MINT_deposit_cancel (struct TALER_MINT_DepositHandle *deposit)
{
  MAC_job_cancel (deposit->job);
  curl_slist_free_all (deposit->headers);
  GNUNET_free (deposit->url);
  GNUNET_free (deposit->json_enc);
  GNUNET_free (deposit);
}



/* end of mint_api_deposit.c */
