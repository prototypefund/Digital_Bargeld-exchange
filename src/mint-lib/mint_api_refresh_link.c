/*
  This file is part of TALER
  Copyright (C) 2015, 2016 GNUnet e.V.

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
 * @file mint-lib/mint_api_refresh_link.c
 * @brief Implementation of the /refresh/link request of the mint's HTTP API
 * @author Christian Grothoff
 */
#include "platform.h"
#include <curl/curl.h>
#include <jansson.h>
#include <microhttpd.h> /* just for HTTP status codes */
#include <gnunet/gnunet_util_lib.h>
#include "taler_mint_service.h"
#include "mint_api_json.h"
#include "mint_api_context.h"
#include "mint_api_handle.h"
#include "taler_signatures.h"


/**
 * @brief A /refresh/link Handle
 */
struct TALER_MINT_RefreshLinkHandle
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
   * Handle for the request.
   */
  struct MAC_Job *job;

  /**
   * Function to call with the result.
   */
  TALER_MINT_RefreshLinkCallback link_cb;

  /**
   * Closure for @e cb.
   */
  void *link_cb_cls;

  /**
   * Download buffer
   */
  struct MAC_DownloadBuffer db;

  /**
   * Private key of the coin, required to decode link information.
   */
  struct TALER_CoinSpendPrivateKeyP coin_priv;

};


/**
 * Parse the provided linkage data from the "200 OK" response
 * for one of the coins.
 *
 * @param rlh refresh link handle
 * @param json json reply with the data for one coin
 * @param trans_pub our transfer public key
 * @param secret_enc encrypted key to decrypt link data
 * @param[out] coin_priv where to return private coin key
 * @param[out] sig where to return private coin signature
 * @param[out] pub where to return the public key for the coin
 * @return #GNUNET_OK on success, #GNUNET_SYSERR on error
 */
static int
parse_refresh_link_coin (const struct TALER_MINT_RefreshLinkHandle *rlh,
                         json_t *json,
                         const struct TALER_TransferPublicKeyP *trans_pub,
                         const struct TALER_EncryptedLinkSecretP *secret_enc,
                         struct TALER_CoinSpendPrivateKeyP *coin_priv,
                         struct TALER_DenominationSignature *sig,
                         struct TALER_DenominationPublicKey *pub)
{
  void *link_enc;
  size_t link_enc_size;
  struct GNUNET_CRYPTO_rsa_Signature *bsig;
  struct GNUNET_CRYPTO_rsa_PublicKey *rpub;
  struct MAJ_Specification spec[] = {
    MAJ_spec_varsize ("link_enc", &link_enc, &link_enc_size),
    MAJ_spec_rsa_public_key ("denom_pub", &rpub),
    MAJ_spec_rsa_signature ("ev_sig", &bsig),
    MAJ_spec_end
  };
  struct TALER_RefreshLinkEncrypted *rle;
  struct TALER_RefreshLinkDecrypted *rld;
  struct TALER_LinkSecretP secret;

  /* parse reply */
  if (GNUNET_OK !=
      MAJ_parse_json (json,
                      spec))
  {
    GNUNET_break_op (0);
    return GNUNET_SYSERR;
  }

  /* decode and decrypt link data */
  rle = TALER_refresh_link_encrypted_decode (link_enc,
                                             link_enc_size);
  if (NULL == rle)
  {
    GNUNET_break_op (0);
    MAJ_parse_free (spec);
    return GNUNET_SYSERR;
  }
  if (GNUNET_OK !=
      TALER_link_decrypt_secret2 (secret_enc,
                                  trans_pub,
                                  &rlh->coin_priv,
                                  &secret))
  {
    GNUNET_break_op (0);
    MAJ_parse_free (spec);
    return GNUNET_SYSERR;
  }
  rld = TALER_refresh_decrypt (rle,
                               &secret);
  if (NULL == rld)
  {
    GNUNET_break_op (0);
    MAJ_parse_free (spec);
    return GNUNET_SYSERR;
  }

  /* extract coin and signature */
  *coin_priv = rld->coin_priv;
  sig->rsa_signature
    = GNUNET_CRYPTO_rsa_unblind (bsig,
                                 rld->blinding_key.rsa_blinding_key,
                                 rpub);

  /* clean up */
  GNUNET_free (rld);
  pub->rsa_public_key = GNUNET_CRYPTO_rsa_public_key_dup (rpub);
  MAJ_parse_free (spec);
  return GNUNET_OK;
}


/**
 * Parse the provided linkage data from the "200 OK" response
 * for one of the coins.
 *
 * @param[in,out] rlh refresh link handle (callback may be zero'ed out)
 * @param json json reply with the data for one coin
 * @return #GNUNET_OK on success, #GNUNET_SYSERR on error
 */
static int
parse_refresh_link_ok (struct TALER_MINT_RefreshLinkHandle *rlh,
                       json_t *json)
{
  unsigned int session;
  unsigned int num_coins;
  int ret;

  if (! json_is_array (json))
  {
    GNUNET_break_op (0);
    return GNUNET_SYSERR;
  }
  num_coins = 0;
  /* Theoretically, a coin may have been melted repeatedly
     into different sessions; so the response is an array
     which contains information by melting session.  That
     array contains another array.  However, our API returns
     a single 1d array, so we flatten the 2d array that is
     returned into a single array. Note that usually a coin
     is melted at most once, and so we'll only run this
     loop once for 'session=0' in most cases.

     num_coins tracks the size of the 1d array we return,
     whilst 'i' and 'session' track the 2d array. */
  for (session=0;session<json_array_size (json); session++)
  {
    json_t *jsona;
    struct MAJ_Specification spec[] = {
      MAJ_spec_json ("new_coins", &jsona),
      MAJ_spec_end
    };

    if (GNUNET_OK !=
	MAJ_parse_json (json_array_get (json,
					session),
			spec))
    {
      GNUNET_break_op (0);
      return GNUNET_SYSERR;
    }
    if (! json_is_array (jsona))
    {
      GNUNET_break_op (0);
      MAJ_parse_free (spec);
      return GNUNET_SYSERR;
    }

    /* count all coins over all sessions */
    num_coins += json_array_size (jsona);
    MAJ_parse_free (spec);
  }
  /* Now that we know how big the 1d array is, allocate
     and fill it. */
  {
    unsigned int off_coin; /* index into 1d array */
    unsigned int i;
    struct TALER_CoinSpendPrivateKeyP coin_privs[num_coins];
    struct TALER_DenominationSignature sigs[num_coins];
    struct TALER_DenominationPublicKey pubs[num_coins];

    memset (sigs, 0, sizeof (sigs));
    memset (pubs, 0, sizeof (pubs));
    off_coin = 0;
    for (session=0;session<json_array_size (json); session++)
    {
      json_t *jsona;
      struct TALER_TransferPublicKeyP trans_pub;
      struct TALER_EncryptedLinkSecretP secret_enc;
      struct MAJ_Specification spec[] = {
	MAJ_spec_json ("new_coins", &jsona),
	MAJ_spec_fixed_auto ("transfer_pub", &trans_pub),
	MAJ_spec_fixed_auto ("secret_enc", &secret_enc),
	MAJ_spec_end
      };

      if (GNUNET_OK !=
	  MAJ_parse_json (json_array_get (json,
					  session),
			  spec))
      {
	GNUNET_break_op (0);
	return GNUNET_SYSERR;
      }
      if (! json_is_array (jsona))
      {
	GNUNET_break_op (0);
	MAJ_parse_free (spec);
	return GNUNET_SYSERR;
      }

      /* decode all coins */
      for (i=0;i<json_array_size (jsona);i++)
      {
	if (GNUNET_OK !=
	    parse_refresh_link_coin (rlh,
				     json_array_get (jsona,
						     i),
				     &trans_pub,
				     &secret_enc,
				     &coin_privs[i+off_coin],
				     &sigs[i+off_coin],
				     &pubs[i+off_coin]))
	{
	  GNUNET_break_op (0);
	  break;
	}
      }
      /* check if we really got all, then invoke callback */
      off_coin += i;
      if (i != json_array_size (jsona))
      {
	GNUNET_break_op (0);
	ret = GNUNET_SYSERR;
	MAJ_parse_free (spec);
	break;
      }
      MAJ_parse_free (spec);
    } /* end of for (session) */

    if (off_coin == num_coins)
    {
      rlh->link_cb (rlh->link_cb_cls,
		    MHD_HTTP_OK,
		    num_coins,
		    coin_privs,
		    sigs,
		    pubs,
		    json);
      rlh->link_cb = NULL;
      ret = GNUNET_OK;
    }
    else
    {
      GNUNET_break_op (0);
      ret = GNUNET_SYSERR;
    }

    /* clean up */
    for (i=0;i<off_coin;i++)
    {
      if (NULL != sigs[i].rsa_signature)
        GNUNET_CRYPTO_rsa_signature_free (sigs[i].rsa_signature);
      if (NULL != pubs[i].rsa_public_key)
        GNUNET_CRYPTO_rsa_public_key_free (pubs[i].rsa_public_key);
    }
  }
  return ret;
}


/**
 * Function called when we're done processing the
 * HTTP /refresh/link request.
 *
 * @param cls the `struct TALER_MINT_RefreshLinkHandle`
 * @param eh the curl request handle
 */
static void
handle_refresh_link_finished (void *cls,
                              CURL *eh)
{
  struct TALER_MINT_RefreshLinkHandle *rlh = cls;
  long response_code;
  json_t *json;

  rlh->job = NULL;
  json = MAC_download_get_result (&rlh->db,
                                  eh,
                                  &response_code);
  switch (response_code)
  {
  case 0:
    break;
  case MHD_HTTP_OK:
    if (GNUNET_OK !=
        parse_refresh_link_ok (rlh,
                               json))
    {
      GNUNET_break_op (0);
      response_code = 0;
    }
    break;
  case MHD_HTTP_BAD_REQUEST:
    /* This should never happen, either us or the mint is buggy
       (or API version conflict); just pass JSON reply to the application */
    break;
  case MHD_HTTP_NOT_FOUND:
    /* Nothing really to verify, mint says this coin was not melted; we
       should pass the JSON reply to the application */
    break;
  case MHD_HTTP_INTERNAL_SERVER_ERROR:
    /* Server had an internal issue; we should retry, but this API
       leaves this to the application */
    break;
  default:
    /* unexpected response code */
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Unexpected response code %u\n",
                response_code);
    GNUNET_break (0);
    response_code = 0;
    break;
  }
  if (NULL != rlh->link_cb)
    rlh->link_cb (rlh->link_cb_cls,
                  response_code,
                  0, NULL, NULL, NULL,
                  json);
  json_decref (json);
  TALER_MINT_refresh_link_cancel (rlh);
}


/**
 * Submit a link request to the mint and get the mint's response.
 *
 * This API is typically not used by anyone, it is more a threat
 * against those trying to receive a funds transfer by abusing the
 * /refresh protocol.
 *
 * @param mint the mint handle; the mint must be ready to operate
 * @param coin_priv private key to request link data for
 * @param link_cb the callback to call with the useful result of the
 *        refresh operation the @a coin_priv was involved in (if any)
 * @param link_cb_cls closure for @a link_cb
 * @return a handle for this request
 */
struct TALER_MINT_RefreshLinkHandle *
TALER_MINT_refresh_link (struct TALER_MINT_Handle *mint,
                         const struct TALER_CoinSpendPrivateKeyP *coin_priv,
                         TALER_MINT_RefreshLinkCallback link_cb,
                         void *link_cb_cls)
{
  struct TALER_MINT_RefreshLinkHandle *rlh;
  CURL *eh;
  struct TALER_MINT_Context *ctx;
  struct TALER_CoinSpendPublicKeyP coin_pub;
  char *pub_str;
  char *arg_str;

  if (GNUNET_YES !=
      MAH_handle_is_ready (mint))
  {
    GNUNET_break (0);
    return NULL;
  }

  GNUNET_CRYPTO_eddsa_key_get_public (&coin_priv->eddsa_priv,
                                      &coin_pub.eddsa_pub);
  pub_str = GNUNET_STRINGS_data_to_string_alloc (&coin_pub,
                                                 sizeof (struct TALER_CoinSpendPublicKeyP));
  GNUNET_asprintf (&arg_str,
                   "/refresh/link?coin_pub=%s",
                   pub_str);
  GNUNET_free (pub_str);

  rlh = GNUNET_new (struct TALER_MINT_RefreshLinkHandle);
  rlh->mint = mint;
  rlh->link_cb = link_cb;
  rlh->link_cb_cls = link_cb_cls;
  rlh->coin_priv = *coin_priv;
  rlh->url = MAH_path_to_url (mint, arg_str);
  GNUNET_free (arg_str);

  eh = curl_easy_init ();
  GNUNET_assert (CURLE_OK ==
                 curl_easy_setopt (eh,
                                   CURLOPT_URL,
                                   rlh->url));
  GNUNET_assert (CURLE_OK ==
                 curl_easy_setopt (eh,
                                   CURLOPT_WRITEFUNCTION,
                                   &MAC_download_cb));
  GNUNET_assert (CURLE_OK ==
                 curl_easy_setopt (eh,
                                   CURLOPT_WRITEDATA,
                                   &rlh->db));
  ctx = MAH_handle_to_context (mint);
  rlh->job = MAC_job_add (ctx,
                          eh,
                          GNUNET_YES,
                          &handle_refresh_link_finished,
                          rlh);
  return rlh;
}


/**
 * Cancel a refresh link request.  This function cannot be used
 * on a request handle if the callback was already invoked.
 *
 * @param rlh the refresh link handle
 */
void
TALER_MINT_refresh_link_cancel (struct TALER_MINT_RefreshLinkHandle *rlh)
{
  if (NULL != rlh->job)
  {
    MAC_job_cancel (rlh->job);
    rlh->job = NULL;
  }
  GNUNET_free_non_null (rlh->db.buf);
  GNUNET_free (rlh->url);
  GNUNET_free (rlh);
}


/* end of mint_api_refresh_link.c */
