/*
  This file is part of TALER
  Copyright (C) 2014, 2015 GNUnet e.V.

  TALER is free software; you can redistribute it and/or modify it under the
  terms of the GNU Affero General Public License as published by the Free Software
  Foundation; either version 3, or (at your option) any later version.

  TALER is distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
  A PARTICULAR PURPOSE.  See the GNU Affero General Public License for more details.

  You should have received a copy of the GNU Affero General Public License along with
  TALER; see the file COPYING.  If not, If not, see <http://www.gnu.org/licenses/>
*/
/**
 * @file taler-mint-httpd_refresh.c
 * @brief Handle /refresh/ requests
 * @author Florian Dold
 * @author Benedikt Mueller
 * @author Christian Grothoff
 */
#include "platform.h"
#include <gnunet/gnunet_util_lib.h>
#include <jansson.h>
#include <microhttpd.h>
#include "taler_mintdb_plugin.h"
#include "taler_signatures.h"
#include "taler_util.h"
#include "taler-mint-httpd_parsing.h"
#include "taler-mint-httpd_mhd.h"
#include "taler-mint-httpd_refresh.h"
#include "taler-mint-httpd_responses.h"
#include "taler-mint-httpd_keystate.h"


/**
 * Handle a "/refresh/melt" request after the main JSON parsing has happened.
 * We now need to validate the coins being melted and the session signature
 * and then hand things of to execute the melt operation.
 *
 * @param connection the MHD connection to handle
 * @param num_new_denoms number of coins to be created, size of y-dimension of @commit_link array
 * @param denom_pubs array of @a num_new_denoms keys
 * @param coin_count number of coins to be melted, size of y-dimension of @commit_coin array
 * @param coin_public_infos array with @a coin_count entries about the coins
 * @param coin_melt_details array with @a coin_count entries with melting details
 * @param session_hash hash over the data that the client commits to
 * @param commit_client_sig signature of the client over this commitment
 * @param kappa size of x-dimension of @commit_coin and @commit_link arrays
 * @param commit_coin 2d array of coin commitments (what the mint is to sign
 *                    once the "/refres/reveal" of cut and choose is done)
 * @param commit_link 2d array of coin link commitments (what the mint is
 *                    to return via "/refresh/link" to enable linkage in the
 *                    future)
 * @return MHD result code
 */
static int
handle_refresh_melt_binary (struct MHD_Connection *connection,
                            unsigned int num_new_denoms,
                            const struct TALER_DenominationPublicKey *denom_pubs,
                            unsigned int coin_count,
                            struct TALER_CoinPublicInfo *coin_public_infos,
                            const struct MeltDetails *coin_melt_details,
                            const struct GNUNET_HashCode *session_hash,
                            unsigned int kappa,
                            struct RefreshCommitCoin *const* commit_coin,
                            struct RefreshCommitLink *const* commit_link)

{
  unsigned int i;
  struct MintKeyState *key_state;
  struct TALER_MINT_DenomKeyIssue *dki;
  struct TALER_Amount cost;
  struct TALER_Amount total_cost;
  struct TALER_Amount melt;
  struct TALER_Amount value;
  struct TALER_Amount fee_withdraw;
  struct TALER_Amount fee_melt;
  struct TALER_Amount total_melt;

  GNUNET_assert (GNUNET_OK ==
                 TALER_amount_get_zero (MINT_CURRENCY,
                                        &total_cost));
  key_state = TALER_MINT_key_state_acquire ();
  for (i=0;i<num_new_denoms;i++)
  {
    dki = &TALER_MINT_get_denom_key (key_state,
                                     &denom_pubs[i])->issue;
    TALER_amount_ntoh (&value,
                       &dki->value);
    TALER_amount_ntoh (&fee_withdraw,
                       &dki->fee_withdraw);
    if ( (GNUNET_OK !=
          TALER_amount_add (&cost,
                            &value,
                            &fee_withdraw)) ||
         (GNUNET_OK !=
          TALER_amount_add (&total_cost,
                            &cost,
                            &total_cost)) )
    {
      TALER_MINT_key_state_release (key_state);
      return TALER_MINT_reply_internal_error (connection,
                                              "cost calculation failure");
    }
  }

  GNUNET_assert (GNUNET_OK ==
                 TALER_amount_get_zero (MINT_CURRENCY,
                                        &total_melt));
  for (i=0;i<coin_count;i++)
  {
    /* calculate contribution of the i-th melt by subtracting
       the fee; add the rest to the total_melt value */
    dki = &TALER_MINT_get_denom_key (key_state,
                                     &coin_public_infos[i].denom_pub)->issue;
    TALER_amount_ntoh (&fee_melt,
                       &dki->fee_refresh);
    if (GNUNET_OK !=
        TALER_amount_subtract (&melt,
                               &coin_melt_details->melt_amount_with_fee,
                               &fee_melt))
    {
      TALER_MINT_key_state_release (key_state);
      return TALER_MINT_reply_external_error (connection,
                                              "Melt contribution below melting fee");
    }
    if (GNUNET_OK !=
        TALER_amount_add (&total_melt,
                          &melt,
                          &total_melt))
    {
      TALER_MINT_key_state_release (key_state);
      return TALER_MINT_reply_internal_error (connection,
                                              "balance calculation failure");
    }
  }
  TALER_MINT_key_state_release (key_state);
  if (0 !=
      TALER_amount_cmp (&total_cost,
                        &total_melt))
  {
    /* We require total value of coins being melted and
       total value of coins being generated to match! */
    return TALER_MINT_reply_json_pack (connection,
                                       MHD_HTTP_BAD_REQUEST,
                                       "{s:s}",
                                       "error", "value mismatch");
  }
  return TALER_MINT_db_execute_refresh_melt (connection,
                                             session_hash,
                                             num_new_denoms,
                                             denom_pubs,
                                             coin_count,
                                             coin_public_infos,
                                             coin_melt_details,
                                             kappa,
                                             commit_coin,
                                             commit_link);
}


/**
 * Extract public coin information from a JSON object.
 *
 * @param connection the connection to send error responses to
 * @param coin_info the JSON object to extract the coin info from
 * @param r_public_info[OUT] set to the coin's public information
 * @param r_melt_detail[OUT] set to details about the coin's melting permission (if valid)
 * @return #GNUNET_YES if coin public info in JSON was valid
 *         #GNUNET_NO JSON was invalid, response was generated
 *         #GNUNET_SYSERR on internal error
 */
static int
get_coin_public_info (struct MHD_Connection *connection,
                      json_t *coin_info,
                      struct TALER_CoinPublicInfo *r_public_info,
                      struct MeltDetails *r_melt_detail)
{
  int ret;
  struct TALER_CoinSpendSignature melt_sig;
  struct TALER_DenominationSignature sig;
  struct TALER_DenominationPublicKey pk;
  struct TALER_Amount amount;
  struct GNUNET_MINT_ParseFieldSpec spec[] = {
    TALER_MINT_PARSE_FIXED ("coin_pub", &r_public_info->coin_pub),
    TALER_MINT_PARSE_RSA_SIGNATURE ("denom_sig", &sig.rsa_signature),
    TALER_MINT_PARSE_RSA_PUBLIC_KEY ("denom_pub", &pk.rsa_public_key),
    TALER_MINT_PARSE_FIXED ("confirm_sig", &melt_sig),
    TALER_MINT_PARSE_AMOUNT ("value_with_fee", &amount),
    TALER_MINT_PARSE_END
  };

  ret = TALER_MINT_parse_json_data (connection,
                                    coin_info,
                                    spec);
  if (GNUNET_OK != ret)
    return ret;
  /* check mint signature on the coin */
  r_public_info->denom_sig = sig;
  r_public_info->denom_pub = pk;
  if (GNUNET_OK !=
      TALER_test_coin_valid (r_public_info))
  {
    TALER_MINT_release_parsed_data (spec);
    r_public_info->denom_sig.rsa_signature = NULL;
    r_public_info->denom_pub.rsa_public_key = NULL;
    return (MHD_YES ==
            TALER_MINT_reply_json_pack (connection,
                                        MHD_HTTP_NOT_FOUND,
                                        "{s:s}",
                                        "error", "coin invalid"))
      ? GNUNET_NO : GNUNET_SYSERR;
  }
  r_melt_detail->melt_sig = melt_sig;
  r_melt_detail->melt_amount_with_fee = amount;
  TALER_MINT_release_parsed_data (spec);
  return GNUNET_OK;
}


/**
 * Verify that the signature shows that this coin is to be melted into
 * the given @a session_pub melting session, and that this is a valid
 * coin (we know the denomination key and the signature on it is
 * valid).  Essentially, this does all of the per-coin checks that can
 * be done before the transaction starts.
 *
 * @param connection the connection to send error responses to
 * @param session_hash hash over refresh session the coin is melted into
 * @param r_public_info the coin's public information
 * @param r_melt_detail details about the coin's melting permission (if valid)
 * @return #GNUNET_YES if coin public info in JSON was valid
 *         #GNUNET_NO JSON was invalid, response was generated
 *         #GNUNET_SYSERR on internal error
 */
static int
verify_coin_public_info (struct MHD_Connection *connection,
                         const struct GNUNET_HashCode *session_hash,
                         const struct TALER_CoinPublicInfo *r_public_info,
                         const struct MeltDetails *r_melt_detail)
{
  struct RefreshMeltCoinSignature body;
  struct MintKeyState *key_state;
  struct TALER_MINT_DenomKeyIssuePriv *dki;
  struct TALER_Amount fee_refresh;

  body.purpose.size = htonl (sizeof (struct RefreshMeltCoinSignature));
  body.purpose.purpose = htonl (TALER_SIGNATURE_REFRESH_MELT_COIN);
  body.session_hash = *session_hash;
  TALER_amount_hton (&body.amount_with_fee,
                     &r_melt_detail->melt_amount_with_fee);
  body.coin_pub = r_public_info->coin_pub;
  if (GNUNET_OK !=
      GNUNET_CRYPTO_ecdsa_verify (TALER_SIGNATURE_REFRESH_MELT_COIN,
                                  &body.purpose,
                                  &r_melt_detail->melt_sig.ecdsa_signature,
                                  &r_public_info->coin_pub.ecdsa_pub))
  {
    if (MHD_YES !=
        TALER_MINT_reply_json_pack (connection,
                                    MHD_HTTP_UNAUTHORIZED,
                                    "{s:s}",
                                    "error", "signature invalid"))
      return GNUNET_SYSERR;
    return GNUNET_NO;
  }
  key_state = TALER_MINT_key_state_acquire ();
  dki = TALER_MINT_get_denom_key (key_state,
                                  &r_public_info->denom_pub);
  if (NULL == dki)
  {
    TALER_MINT_key_state_release (key_state);
    LOG_WARNING ("Unknown denomination key in /refresh/melt request\n");
    return TALER_MINT_reply_arg_invalid (connection,
                                         "denom_pub");
  }
  /* FIXME: need to check if denomination key is still
     valid for issuing! (#3634) */
  TALER_amount_ntoh (&fee_refresh,
                     &dki->issue.fee_refresh);
  if (TALER_amount_cmp (&fee_refresh,
                        &r_melt_detail->melt_amount_with_fee) < 0)
  {
    TALER_MINT_key_state_release (key_state);
    return (MHD_YES ==
            TALER_MINT_reply_external_error (connection,
                                             "melt amount smaller than melting fee"))
      ? GNUNET_NO : GNUNET_SYSERR;
  }

  TALER_MINT_key_state_release (key_state);
  return GNUNET_OK;
}


/**
 * Release memory from the @a commit_coin array.
 *
 * @param commit_coin array to release
 * @param kappa size of 1st dimension
 * @param num_new_coins size of 2nd dimension
 */
static void
free_commit_coins (struct RefreshCommitCoin **commit_coin,
                   unsigned int kappa,
                   unsigned int num_new_coins)
{
  unsigned int i;
  unsigned int j;

  for (i=0;i<kappa;i++)
  {
    if (NULL == commit_coin[i])
      break;
    for (j=0;j<num_new_coins;j++)
    {
      GNUNET_free_non_null (commit_coin[i][j].coin_ev);
      GNUNET_free_non_null (commit_coin[i][j].refresh_link);
    }
    GNUNET_free (commit_coin[i]);
  }
}


/**
 * Release memory from the @a commit_link array.
 *
 * @param commit_coin array to release
 * @param kappa size of 1st dimension
 * @param num_old_coins size of 2nd dimension
 */
static void
free_commit_links (struct RefreshCommitLink **commit_link,
                   unsigned int kappa,
                   unsigned int num_old_coins)
{
  unsigned int i;

  for (i=0;i<kappa;i++)
  {
    if (NULL == commit_link[i])
      break;
    GNUNET_free (commit_link[i]);
  }
}


/**
 * Handle a "/refresh/melt" request after the first parsing has happened.
 * We now need to validate the coins being melted and the session signature
 * and then hand things of to execute the melt operation.  This function
 * parses the JSON arrays and then passes processing on to
 * #handle_refresh_melt_binary().
 *
 * @param connection the MHD connection to handle
 * @param new_denoms array of denomination keys
 * @param melt_coins array of coins to melt
 * @param melt_sig_json signature affirming the melt operation
 * @param commit_signature signature over the commit
 * @param kappa security parameter for cut and choose
 * @param num_oldcoins number of coins that are being melted
 * @param transfer_pubs @a kappa-dimensional array of @a num_oldcoins transfer keys
 * @param secret_encs @a kappa-dimensional array of @a num_oldcoins secrets
 * @param num_newcoins number of coins that the refresh will generate
 * @param coin_envs @a kappa-dimensional array of @a num_newcoins envelopes to sign
 * @param link_encs @a kappa-dimensional array of @a num_newcoins encrypted links
 * @return MHD result code
 */
static int
handle_refresh_melt_json (struct MHD_Connection *connection,
                          const json_t *new_denoms,
                          const json_t *melt_coins,
                          const json_t *melt_sig_json,
                          const json_t *commit_signature,
                          unsigned int kappa,
                          unsigned int num_oldcoins,
                          const json_t *transfer_pubs,
                          const json_t *secret_encs,
                          unsigned int num_newcoins,
                          const json_t *coin_evs,
                          const json_t *link_encs)

{
  int res;
  unsigned int i;
  unsigned int j;
  struct TALER_DenominationPublicKey *denom_pubs;
  unsigned int num_new_denoms;
  struct TALER_CoinPublicInfo *coin_public_infos;
  struct MeltDetails *coin_melt_details;
  unsigned int coin_count;
  struct GNUNET_HashCode session_hash;
  struct GNUNET_HashContext *hash_context;
  struct RefreshCommitCoin *commit_coin[kappa];
  struct RefreshCommitLink *commit_link[kappa];

  /* For the signature check, we hash most of the inputs together
     (except for the signatures on the coins). */
  hash_context = GNUNET_CRYPTO_hash_context_start ();
  num_new_denoms = json_array_size (new_denoms);
  denom_pubs = GNUNET_malloc (num_new_denoms *
                              sizeof (struct TALER_DenominationPublicKey));
  for (i=0;i<num_new_denoms;i++)
  {
    char *buf;
    size_t buf_size;

    res = GNUNET_MINT_parse_navigate_json (connection,
                                           new_denoms,
                                           JNAV_INDEX, (int) i,
                                           JNAV_RET_RSA_PUBLIC_KEY,
                                           &denom_pubs[i].rsa_public_key);
    if (GNUNET_OK != res)
    {
      for (j=0;j<i;j++)
        GNUNET_CRYPTO_rsa_public_key_free (denom_pubs[j].rsa_public_key);
      GNUNET_free (denom_pubs);
      return res;
    }
    buf_size = GNUNET_CRYPTO_rsa_public_key_encode (denom_pubs[i].rsa_public_key,
                                                    &buf);
    GNUNET_CRYPTO_hash_context_read (hash_context,
                                     buf,
                                     buf_size);
    GNUNET_free (buf);
 }

  coin_count = json_array_size (melt_coins);
  /* FIXME: make 'struct TALER_CoinPublicInfo' part of `struct MeltDetails`
     and combine these two arrays/arguments! (#3726) */
  coin_public_infos = GNUNET_malloc (coin_count *
                                     sizeof (struct TALER_CoinPublicInfo));
  coin_melt_details = GNUNET_malloc (coin_count *
                                     sizeof (struct MeltDetails));
  for (i=0;i<coin_count;i++)
  {
    /* decode JSON data on coin to melt */
    struct TALER_AmountNBO melt_amount;

    res = get_coin_public_info (connection,
                                json_array_get (melt_coins, i),
                                &coin_public_infos[i],
                                &coin_melt_details[i]);
    if (GNUNET_OK != res)
    {
      for (j=0;j<i;j++)
      {
        GNUNET_CRYPTO_rsa_public_key_free (coin_public_infos[j].denom_pub.rsa_public_key);
        GNUNET_CRYPTO_rsa_signature_free (coin_public_infos[j].denom_sig.rsa_signature);
      }
      GNUNET_free (coin_public_infos);
      for (j=0;j<num_new_denoms;j++)
        GNUNET_CRYPTO_rsa_public_key_free (denom_pubs[j].rsa_public_key);
      GNUNET_free (coin_melt_details);
      GNUNET_free (denom_pubs);
      return (GNUNET_NO == res) ? MHD_YES : MHD_NO;
    }
    /* Check that the client does not try to melt the same coin twice
       into the same session! */
    for (j=0;j<i;j++)
    {
      if (0 == memcmp (&coin_public_infos[i].coin_pub,
                       &coin_public_infos[j].coin_pub,
                       sizeof (struct TALER_CoinSpendPublicKey)))
      {
        for (j=0;j<i;j++)
        {
          GNUNET_CRYPTO_rsa_public_key_free (coin_public_infos[j].denom_pub.rsa_public_key);
          GNUNET_CRYPTO_rsa_signature_free (coin_public_infos[j].denom_sig.rsa_signature);
        }
        GNUNET_free (coin_public_infos);
        for (j=0;j<num_new_denoms;j++)
          GNUNET_CRYPTO_rsa_public_key_free (denom_pubs[j].rsa_public_key);
        GNUNET_free (coin_melt_details);
        GNUNET_free (denom_pubs);
        return TALER_MINT_reply_external_error (connection,
                                                "melting same coin twice in same session is not allowed");
      }
    }
    TALER_amount_hton (&melt_amount,
                       &coin_melt_details[i].melt_amount_with_fee);
    GNUNET_CRYPTO_hash_context_read (hash_context,
                                     &coin_public_infos[i].coin_pub,
                                     sizeof (struct TALER_CoinSpendPublicKey));
    GNUNET_CRYPTO_hash_context_read (hash_context,
                                     &melt_amount,
                                     sizeof (struct TALER_AmountNBO));

  }

  /* parse JSON arrays into 2d binary arrays and hash everything
     together for the signature check */
  memset (commit_coin, 0, sizeof (commit_coin));
  memset (commit_link, 0, sizeof (commit_link));
  for (i = 0; i < kappa; i++)
  {
    commit_coin[i] = GNUNET_malloc (num_newcoins *
                                    sizeof (struct RefreshCommitCoin));
    for (j = 0; j < num_newcoins; j++)
    {
      char *link_enc;
      size_t link_enc_size;
      struct RefreshCommitCoin *rcc = &commit_coin[i][j];

      res = GNUNET_MINT_parse_navigate_json (connection,
                                             coin_evs,
                                             JNAV_INDEX, (int) i,
                                             JNAV_INDEX, (int) j,
                                             JNAV_RET_DATA_VAR,
                                             &rcc->coin_ev,
                                             &rcc->coin_ev_size);

      if (GNUNET_OK != res)
      {
        GNUNET_CRYPTO_hash_context_abort (hash_context);
        free_commit_coins (commit_coin, kappa, num_newcoins);
        return (GNUNET_SYSERR == res) ? MHD_NO : MHD_YES;
      }
      GNUNET_CRYPTO_hash_context_read (hash_context,
                                       rcc->coin_ev,
                                       rcc->coin_ev_size);
      res = GNUNET_MINT_parse_navigate_json (connection,
                                             link_encs,
                                             JNAV_INDEX, (int) i,
                                             JNAV_INDEX, (int) j,
                                             JNAV_RET_DATA_VAR,
                                             &link_enc,
                                             &link_enc_size);
      if (GNUNET_OK != res)
      {
        GNUNET_CRYPTO_hash_context_abort (hash_context);
        free_commit_coins (commit_coin, kappa, num_newcoins);
        return (GNUNET_SYSERR == res) ? MHD_NO : MHD_YES;
      }
      rcc->refresh_link
        = TALER_refresh_link_encrypted_decode (link_enc,
                                               link_enc_size);
      GNUNET_CRYPTO_hash_context_read (hash_context,
                                       link_enc,
                                       link_enc_size);
    }
  }

  for (i = 0; i < kappa; i++)
  {
    commit_link[i] = GNUNET_malloc (num_oldcoins *
                                    sizeof (struct RefreshCommitLink));
    for (j = 0; j < num_oldcoins; j++)
    {
      struct RefreshCommitLink *rcl = &commit_link[i][j];

      res = GNUNET_MINT_parse_navigate_json (connection,
                                             transfer_pubs,
                                             JNAV_INDEX, (int) i,
                                             JNAV_INDEX, (int) j,
                                             JNAV_RET_DATA,
                                             &rcl->transfer_pub,
                                             sizeof (struct TALER_TransferPublicKey));

      if (GNUNET_OK != res)
      {
        GNUNET_break (GNUNET_SYSERR != res);
        GNUNET_CRYPTO_hash_context_abort (hash_context);
        free_commit_coins (commit_coin, kappa, num_newcoins);
        free_commit_links (commit_link, kappa, num_oldcoins);
        return (GNUNET_SYSERR == res) ? MHD_NO : MHD_YES;
      }
      res = GNUNET_MINT_parse_navigate_json (connection,
                                             secret_encs,
                                             JNAV_INDEX, (int) i,
                                             JNAV_INDEX, (int) j,
                                             JNAV_RET_DATA,
                                             &rcl->shared_secret_enc,
                                             sizeof (struct GNUNET_HashCode));

      if (GNUNET_OK != res)
      {
        GNUNET_break (GNUNET_SYSERR != res);
        GNUNET_CRYPTO_hash_context_abort (hash_context);
        free_commit_coins (commit_coin, kappa, num_newcoins);
        free_commit_links (commit_link, kappa, num_oldcoins);
        return (GNUNET_SYSERR == res) ? MHD_NO : MHD_YES;
      }

      GNUNET_CRYPTO_hash_context_read (hash_context,
                                       rcl,
                                       sizeof (struct RefreshCommitLink));
    }

  }
  GNUNET_CRYPTO_hash_context_finish (hash_context,
                                     &session_hash);

  for (i=0;i<coin_count;i++)
  {
    /* verify signatures on coins to melt */
    res = verify_coin_public_info (connection,
                                   &session_hash,
                                   &coin_public_infos[i],
                                   &coin_melt_details[i]);
    if (GNUNET_OK != res)
    {
      res = (GNUNET_NO == res) ? MHD_YES : MHD_NO;
      goto cleanup;
    }
  }

  /* execute commit */
  res = handle_refresh_melt_binary (connection,
                                    num_new_denoms,
                                    denom_pubs,
                                    coin_count,
                                    coin_public_infos,
                                    coin_melt_details,
                                    &session_hash,
                                    kappa,
                                    commit_coin,
                                    commit_link);
 cleanup:
  free_commit_coins (commit_coin, kappa, num_newcoins);
  free_commit_links (commit_link, kappa, num_oldcoins);
  for (j=0;j<coin_count;j++)
  {
    GNUNET_CRYPTO_rsa_public_key_free (coin_public_infos[j].denom_pub.rsa_public_key);
    GNUNET_CRYPTO_rsa_signature_free (coin_public_infos[j].denom_sig.rsa_signature);
  }
  GNUNET_free (coin_public_infos);
  for (j=0;j<num_new_denoms;j++)
    GNUNET_CRYPTO_rsa_public_key_free (denom_pubs[j].rsa_public_key);
  GNUNET_free (coin_melt_details);
  GNUNET_free (denom_pubs);
  return res;
}


/**
 * Handle a "/refresh/melt" request.  Parses the request into the JSON
 * components and then hands things of to #handle_referesh_melt_json()
 * to validate the melted coins, the signature and execute the melt
 * using TALER_MINT_db_execute_refresh_melt().
 *
 * @param rh context of the handler
 * @param connection the MHD connection to handle
 * @param[IN|OUT] connection_cls the connection's closure (can be updated)
 * @param upload_data upload data
 * @param[IN|OUT] upload_data_size number of bytes (left) in @a upload_data
 * @return MHD result code
 */
int
TALER_MINT_handler_refresh_melt (struct RequestHandler *rh,
                                 struct MHD_Connection *connection,
                                 void **connection_cls,
                                 const char *upload_data,
                                 size_t *upload_data_size)
{
  json_t *root;
  json_t *new_denoms;
  json_t *melt_coins;
  json_t *melt_sig_json;
  json_t *coin_evs;
  json_t *link_encs;
  json_t *transfer_pubs;
  json_t *secret_encs;
  json_t *commit_sig_json;
  unsigned int kappa;
  unsigned int num_oldcoins;
  unsigned int num_newcoins;
  json_t *coin_detail;
  int res;
  struct GNUNET_MINT_ParseFieldSpec spec[] = {
    TALER_MINT_PARSE_ARRAY ("new_denoms", &new_denoms),
    TALER_MINT_PARSE_ARRAY ("melt_coins", &melt_coins),
    TALER_MINT_PARSE_ARRAY ("melt_signature", &melt_sig_json),
    TALER_MINT_PARSE_ARRAY ("coin_evs", &coin_evs),
    TALER_MINT_PARSE_ARRAY ("link_encs", &link_encs),
    TALER_MINT_PARSE_ARRAY ("transfer_pubs", &transfer_pubs),
    TALER_MINT_PARSE_ARRAY ("secret_encs", &secret_encs),
    TALER_MINT_PARSE_OBJECT ("commit_signature", &commit_sig_json),
    TALER_MINT_PARSE_END
  };

  res = TALER_MINT_parse_post_json (connection,
                                    connection_cls,
                                    upload_data,
                                    upload_data_size,
                                    &root);
  if (GNUNET_SYSERR == res)
    return MHD_NO;
  if ( (GNUNET_NO == res) || (NULL == root) )
    return MHD_YES;

  res = TALER_MINT_parse_json_data (connection,
                                    root,
                                    spec);
  json_decref (root);
  if (GNUNET_OK != res)
    return (GNUNET_SYSERR == res) ? MHD_NO : MHD_YES;

  /* Determine dimensionality of the request (kappa, #old and #new coins) */
  kappa = json_array_size (coin_evs);
  if ( (3 > kappa) || (kappa > 32) )
  {
    GNUNET_break_op (0);
    TALER_MINT_release_parsed_data (spec);
    return TALER_MINT_reply_arg_invalid (connection,
                                         "coin_evs");
  }
  if (json_array_size (transfer_pubs) != kappa)
  {
    GNUNET_break_op (0);
    TALER_MINT_release_parsed_data (spec);
    return TALER_MINT_reply_arg_invalid (connection,
                                         "transfer_pubs");
  }
  res = GNUNET_MINT_parse_navigate_json (connection, coin_evs,
                                         JNAV_INDEX, (int) 0,
                                         JNAV_RET_DATA,
                                         JSON_ARRAY, &coin_detail);
  if (GNUNET_OK != res)
  {
    TALER_MINT_release_parsed_data (spec);
    return (GNUNET_SYSERR == res) ? MHD_NO : MHD_YES;
  }
  num_newcoins = json_array_size (coin_detail);
  res = GNUNET_MINT_parse_navigate_json (connection,
                                         transfer_pubs,
                                         JNAV_INDEX, (int) 0,
                                         JNAV_RET_DATA,
                                         JSON_ARRAY, &coin_detail);
  if (GNUNET_OK != res)
  {
    TALER_MINT_release_parsed_data (spec);
    return (GNUNET_SYSERR == res) ? MHD_NO : MHD_YES;
  }
  num_oldcoins = json_array_size (coin_detail);

  res = handle_refresh_melt_json (connection,
                                  new_denoms,
                                  melt_coins,
                                  melt_sig_json,
                                  commit_sig_json,
                                  kappa,
                                  num_oldcoins,
                                  transfer_pubs,
                                  secret_encs,
                                  num_newcoins,
                                  coin_evs,
                                  link_encs);

  TALER_MINT_release_parsed_data (spec);
  return res;
}


/**
 * Handle a "/refresh/reveal" request.   Parses the given JSON
 * transfer private keys and if successful, passes everything to
 * #TALER_MINT_db_execute_refresh_reveal() which will verify that the
 * revealed information is valid then returns the signed refreshed
 * coins.
 *
 * @param connection the MHD connection to handle
 * @param session_hash hash identifying the melting session
 * @param kappa length of the 1st dimension of @a transfer_privs array PLUS ONE
 * @param num_oldcoins length of the 2nd dimension of @a transfer_privs array
 * @param tp_json private transfer keys in JSON format
 * @return MHD result code
  */
static int
handle_refresh_reveal_json (struct MHD_Connection *connection,
                            const struct GNUNET_HashCode *session_hash,
                            unsigned int kappa,
                            unsigned int num_oldcoins,
                            const json_t *tp_json)
{
  struct TALER_TransferPrivateKey *transfer_privs[kappa - 1];
  unsigned int i;
  unsigned int j;
  int res;

  for (i = 0; i < kappa - 1; i++)
    transfer_privs[i] = GNUNET_malloc (num_oldcoins *
                                       sizeof (struct TALER_TransferPrivateKey));
  res = GNUNET_OK;
  for (i = 0; i < kappa - 1; i++)
  {
    if (GNUNET_OK != res)
      break;
    for (j = 0; j < num_oldcoins; j++)
    {
      if (GNUNET_OK != res)
        break;
      res = GNUNET_MINT_parse_navigate_json (connection,
                                             tp_json,
                                             JNAV_INDEX, (int) i,
                                             JNAV_INDEX, (int) j,
                                             JNAV_RET_DATA,
                                             &transfer_privs[i][j],
                                             sizeof (struct TALER_TransferPrivateKey));
    }
  }
  if (GNUNET_OK != res)
    res = (GNUNET_SYSERR == res) ? MHD_NO : MHD_YES;
  else
    res = TALER_MINT_db_execute_refresh_reveal (connection,
                                                session_hash,
                                                kappa,
                                                num_oldcoins,
                                                transfer_privs);
  for (i = 0; i < kappa - 1; i++)
    GNUNET_free (transfer_privs[i]);
  return res;
}


/**
 * Handle a "/refresh/reveal" request. This time, the client reveals
 * the private transfer keys except for the cut-and-choose value
 * returned from "/refresh/melt".  This function parses the revealed
 * keys and secrets and ultimately passes everything to
 * #TALER_MINT_db_execute_refresh_reveal() which will verify that the
 * revealed information is valid then returns the signed refreshed
 * coins.
 *
 * @param rh context of the handler
 * @param connection the MHD connection to handle
 * @param[IN|OUT] connection_cls the connection's closure (can be updated)
 * @param upload_data upload data
 * @param[IN|OUT] upload_data_size number of bytes (left) in @a upload_data
 * @return MHD result code
  */
int
TALER_MINT_handler_refresh_reveal (struct RequestHandler *rh,
                                   struct MHD_Connection *connection,
                                   void **connection_cls,
                                   const char *upload_data,
                                   size_t *upload_data_size)
{
  struct GNUNET_HashCode session_hash;
  int res;
  unsigned int kappa;
  unsigned int num_oldcoins;
  json_t *reveal_detail;
  json_t *root;
  json_t *transfer_privs;
  struct GNUNET_MINT_ParseFieldSpec spec[] = {
    TALER_MINT_PARSE_FIXED ("session_hash", &session_hash),
    TALER_MINT_PARSE_ARRAY ("transfer_privs", &transfer_privs),
    TALER_MINT_PARSE_END
  };

  res = TALER_MINT_parse_post_json (connection,
                                    connection_cls,
                                    upload_data,
                                    upload_data_size,
                                    &root);
  if (GNUNET_SYSERR == res)
    return MHD_NO;
  if ( (GNUNET_NO == res) || (NULL == root) )
    return MHD_YES;

  res = TALER_MINT_parse_json_data (connection,
                                    root,
                                    spec);
  json_decref (root);
  if (GNUNET_OK != res)
    return (GNUNET_SYSERR == res) ? MHD_NO : MHD_YES;

  /* Determine dimensionality of the request (kappa and #old coins) */
  kappa = json_array_size (transfer_privs) + 1;
  if ( (2 > kappa) || (kappa > 31) )
  {
    TALER_MINT_release_parsed_data (spec);
    return TALER_MINT_reply_arg_invalid (connection,
                                         "transfer_privs");
  }
  /* Note we do +1 as 1 row (cut-and-choose!) is missing! */
  kappa++;
  res = GNUNET_MINT_parse_navigate_json (connection,
                                         transfer_privs,
                                         JNAV_INDEX, 0,
                                         JNAV_RET_TYPED_JSON,
                                         JSON_ARRAY,
                                         &reveal_detail);
  if (GNUNET_OK != res)
  {
    TALER_MINT_release_parsed_data (spec);
    return (GNUNET_SYSERR == res) ? MHD_NO : MHD_YES;
  }
  num_oldcoins = json_array_size (reveal_detail);
  res = handle_refresh_reveal_json (connection,
                                    &session_hash,
                                    kappa,
                                    num_oldcoins,
                                    transfer_privs);
  TALER_MINT_release_parsed_data (spec);
  return res;
}


/**
 * Handle a "/refresh/link" request
 *
 * @param rh context of the handler
 * @param connection the MHD connection to handle
 * @param[IN|OUT] connection_cls the connection's closure (can be updated)
 * @param upload_data upload data
 * @param[IN|OUT] upload_data_size number of bytes (left) in @a upload_data
 * @return MHD result code
  */
int
TALER_MINT_handler_refresh_link (struct RequestHandler *rh,
                                 struct MHD_Connection *connection,
                                 void **connection_cls,
                                 const char *upload_data,
                                 size_t *upload_data_size)
{
  struct TALER_CoinSpendPublicKey coin_pub;
  int res;

  res = TALER_MINT_mhd_request_arg_data (connection,
                                         "coin_pub",
                                         &coin_pub,
                                         sizeof (struct TALER_CoinSpendPublicKey));
  if (GNUNET_SYSERR == res)
    return MHD_NO;
  if (GNUNET_OK != res)
    return MHD_YES;

  return TALER_MINT_db_execute_refresh_link (connection,
                                             &coin_pub);
}


/* end of taler-mint-httpd_refresh.c */
