/*
  This file is part of TALER
  Copyright (C) 2014-2017 Taler Systems SA

  TALER is free software; you can redistribute it and/or modify it under the
  terms of the GNU General Public License as published by the Free Software
  Foundation; either version 3, or (at your option) any later version.

  TALER is distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
  A PARTICULAR PURPOSE.  See the GNU General Public License for more details.

  You should have received a copy of the GNU General Public License along with
  TALER; see the file COPYING.  If not, see <http://www.gnu.org/licenses/>
*/
/**
 * @file util/crypto.c
 * @brief Cryptographic utility functions
 * @author Sree Harsha Totakura <sreeharsha@totakura.in>
 * @author Florian Dold
 * @author Benedikt Mueller
 * @author Christian Grothoff
 */
#include "platform.h"
#include "taler_util.h"
#include <gcrypt.h>


/**
 * Function called by libgcrypt on serious errors.
 * Prints an error message and aborts the process.
 *
 * @param cls NULL
 * @param wtf unknown
 * @param msg error message
 */
static void
fatal_error_handler (void *cls,
                     int wtf,
                     const char *msg)
{
  (void) cls;
  (void) wtf;
  fprintf (stderr,
           "Fatal error in libgcrypt: %s\n",
           msg);
  abort ();
}


/**
 * Initialize libgcrypt.
 */
void __attribute__ ((constructor))
TALER_gcrypt_init ()
{
  gcry_set_fatalerror_handler (&fatal_error_handler,
                               NULL);
  if (! gcry_check_version (NEED_LIBGCRYPT_VERSION))
  {
    fprintf (stderr,
             "libgcrypt version mismatch\n");
    abort ();
  }
  /* Disable secure memory (we should never run on a system that
     even uses swap space for memory). */
  gcry_control (GCRYCTL_DISABLE_SECMEM, 0);
  gcry_control (GCRYCTL_INITIALIZATION_FINISHED, 0);
}


/**
 * Check if a coin is valid; that is, whether the denomination key exists,
 * is not expired, and the signature is correct.
 *
 * @param coin_public_info the coin public info to check for validity
 * @param denom_pub denomination key, must match @a coin_public_info's `denom_pub_hash`
 * @return #GNUNET_YES if the coin is valid,
 *         #GNUNET_NO if it is invalid
 *         #GNUNET_SYSERR if an internal error occurred
 */
int
TALER_test_coin_valid (const struct TALER_CoinPublicInfo *coin_public_info,
                       const struct TALER_DenominationPublicKey *denom_pub)
{
  struct GNUNET_HashCode c_hash;
#if ENABLE_SANITY_CHECKS
  struct GNUNET_HashCode d_hash;

  GNUNET_CRYPTO_rsa_public_key_hash (denom_pub->rsa_public_key,
                                     &d_hash);
  GNUNET_assert (0 ==
                 GNUNET_memcmp (&d_hash,
                                &coin_public_info->denom_pub_hash));
#endif
  GNUNET_CRYPTO_hash (&coin_public_info->coin_pub,
                      sizeof (struct GNUNET_CRYPTO_EcdsaPublicKey),
                      &c_hash);
  if (GNUNET_OK !=
      GNUNET_CRYPTO_rsa_verify (&c_hash,
                                coin_public_info->denom_sig.rsa_signature,
                                denom_pub->rsa_public_key))
  {
    GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                "coin signature is invalid\n");
    return GNUNET_NO;
  }
  return GNUNET_YES;
}


/**
 * Given the coin and the transfer private keys, compute the
 * transfer secret.  (Technically, we only need one of the two
 * private keys, but the caller currently trivially only has
 * the two private keys, so we derive one of the public keys
 * internally to this function.)
 *
 * @param coin_priv coin key
 * @param trans_priv transfer private key
 * @param[out] ts computed transfer secret
 */
void
TALER_link_derive_transfer_secret (
  const struct TALER_CoinSpendPrivateKeyP *coin_priv,
  const struct TALER_TransferPrivateKeyP *trans_priv,
  struct TALER_TransferSecretP *ts)
{
  struct TALER_CoinSpendPublicKeyP coin_pub;

  GNUNET_CRYPTO_eddsa_key_get_public (&coin_priv->eddsa_priv,
                                      &coin_pub.eddsa_pub);
  GNUNET_assert (GNUNET_OK ==
                 GNUNET_CRYPTO_ecdh_eddsa (&trans_priv->ecdhe_priv,
                                           &coin_pub.eddsa_pub,
                                           &ts->key));

}


/**
 * Decrypt the shared @a secret from the information in the
 * @a trans_priv and @a coin_pub.
 *
 * @param trans_priv transfer private key
 * @param coin_pub coin public key
 * @param[out] transfer_secret set to the shared secret
 */
void
TALER_link_reveal_transfer_secret (
  const struct TALER_TransferPrivateKeyP *trans_priv,
  const struct TALER_CoinSpendPublicKeyP *coin_pub,
  struct TALER_TransferSecretP *transfer_secret)
{
  GNUNET_assert (GNUNET_OK ==
                 GNUNET_CRYPTO_ecdh_eddsa (&trans_priv->ecdhe_priv,
                                           &coin_pub->eddsa_pub,
                                           &transfer_secret->key));
}


/**
 * Decrypt the shared @a secret from the information in the
 * @a trans_priv and @a coin_pub.
 *
 * @param trans_pub transfer private key
 * @param coin_priv coin public key
 * @param[out] transfer_secret set to the shared secret
 */
void
TALER_link_recover_transfer_secret (
  const struct TALER_TransferPublicKeyP *trans_pub,
  const struct TALER_CoinSpendPrivateKeyP *coin_priv,
  struct TALER_TransferSecretP *transfer_secret)
{
  GNUNET_assert (GNUNET_OK ==
                 GNUNET_CRYPTO_eddsa_ecdh (&coin_priv->eddsa_priv,
                                           &trans_pub->ecdhe_pub,
                                           &transfer_secret->key));
}


/**
 * Setup information for a fresh coin.
 *
 * @param secret_seed seed to use for KDF to derive coin keys
 * @param coin_num_salt number of the coin to include in KDF
 * @param[out] ps value to initialize
 */
void
TALER_planchet_setup_refresh (const struct TALER_TransferSecretP *secret_seed,
                              uint32_t coin_num_salt,
                              struct TALER_PlanchetSecretsP *ps)
{
  uint32_t be_salt = htonl (coin_num_salt);

  GNUNET_assert (GNUNET_OK ==
                 GNUNET_CRYPTO_kdf (ps,
                                    sizeof (*ps),
                                    &be_salt,
                                    sizeof (be_salt),
                                    secret_seed,
                                    sizeof (*secret_seed),
                                    "taler-coin-derivation",
                                    strlen ("taler-coin-derivation"),
                                    NULL, 0));
}


/**
 * Setup information for a fresh coin.
 *
 * @param[out] ps value to initialize
 */
void
TALER_planchet_setup_random (struct TALER_PlanchetSecretsP *ps)
{
  GNUNET_CRYPTO_random_block (GNUNET_CRYPTO_QUALITY_STRONG,
                              ps,
                              sizeof (*ps));
}


/**
 * Prepare a planchet for tipping.  Creates and blinds a coin.
 *
 * @param dk denomination key for the coin to be created
 * @param ps secret planchet internals (for #TALER_planchet_to_coin)
 * @param[out] c_hash set to the hash of the public key of the coin (needed later)
 * @param[out] pd set to the planchet detail for TALER_MERCHANT_tip_pickup() and
 *               other withdraw operations
 * @return #GNUNET_OK on success
 */
int
TALER_planchet_prepare (const struct TALER_DenominationPublicKey *dk,
                        const struct TALER_PlanchetSecretsP *ps,
                        struct GNUNET_HashCode *c_hash,
                        struct TALER_PlanchetDetail *pd)
{
  struct TALER_CoinSpendPublicKeyP coin_pub;

  GNUNET_CRYPTO_eddsa_key_get_public (&ps->coin_priv.eddsa_priv,
                                      &coin_pub.eddsa_pub);
  GNUNET_CRYPTO_hash (&coin_pub.eddsa_pub,
                      sizeof (struct GNUNET_CRYPTO_EcdsaPublicKey),
                      c_hash);
  if (GNUNET_YES !=
      GNUNET_CRYPTO_rsa_blind (c_hash,
                               &ps->blinding_key.bks,
                               dk->rsa_public_key,
                               &pd->coin_ev,
                               &pd->coin_ev_size))
  {
    GNUNET_break_op (0);
    return GNUNET_SYSERR;
  }
  GNUNET_CRYPTO_rsa_public_key_hash (dk->rsa_public_key,
                                     &pd->denom_pub_hash);
  return GNUNET_OK;
}


/**
 * Obtain a coin from the planchet's secrets and the blind signature
 * of the exchange.
 *
 * @param dk denomination key, must match what was given to #TALER_planchet_prepare()
 * @param blind_sig blind signature from the exchange
 * @param ps secrets from #TALER_planchet_prepare()
 * @param c_hash hash of the coin's public key for verification of the signature
 * @param[out] coin set to the details of the fresh coin
 * @return #GNUNET_OK on success
 */
int
TALER_planchet_to_coin (const struct TALER_DenominationPublicKey *dk,
                        const struct GNUNET_CRYPTO_RsaSignature *blind_sig,
                        const struct TALER_PlanchetSecretsP *ps,
                        const struct GNUNET_HashCode *c_hash,
                        struct TALER_FreshCoin *coin)
{
  struct GNUNET_CRYPTO_RsaSignature *sig;

  sig = GNUNET_CRYPTO_rsa_unblind (blind_sig,
                                   &ps->blinding_key.bks,
                                   dk->rsa_public_key);
  if (GNUNET_OK !=
      GNUNET_CRYPTO_rsa_verify (c_hash,
                                sig,
                                dk->rsa_public_key))
  {
    GNUNET_break_op (0);
    GNUNET_CRYPTO_rsa_signature_free (sig);
    return GNUNET_SYSERR;
  }
  coin->sig.rsa_signature = sig;
  coin->coin_priv = ps->coin_priv;
  return GNUNET_OK;
}


/**
 * Compute the commitment for a /refresh/melt operation from
 * the respective public inputs.
 *
 * @param[out] rc set to the value the wallet must commit to
 * @param kappa number of transfer public keys involved (must be #TALER_CNC_KAPPA)
 * @param num_new_coins number of new coins to be created
 * @param rcs commitments array of @a kappa commitments
 * @param coin_pub public key of the coin to be melted
 * @param amount_with_fee amount to be melted, including fee
 */
void
TALER_refresh_get_commitment (struct TALER_RefreshCommitmentP *rc,
                              uint32_t kappa,
                              uint32_t num_new_coins,
                              const struct TALER_RefreshCommitmentEntry *rcs,
                              const struct TALER_CoinSpendPublicKeyP *coin_pub,
                              const struct TALER_Amount *amount_with_fee)
{
  struct GNUNET_HashContext *hash_context;

  hash_context = GNUNET_CRYPTO_hash_context_start ();
  /* first, iterate over transfer public keys for hash_context */
  for (unsigned int i = 0; i<kappa; i++)
  {
    GNUNET_CRYPTO_hash_context_read (hash_context,
                                     &rcs[i].transfer_pub,
                                     sizeof (struct TALER_TransferPublicKeyP));
  }
  /* next, add all of the hashes from the denomination keys to the
     hash_context */
  for (unsigned int i = 0; i<num_new_coins; i++)
  {
    void *buf;
    size_t buf_size;

    /* The denomination keys should / must all be identical regardless
       of what offset we use, so we use [0]. */
    GNUNET_assert (kappa > 0); /* sanity check */
    buf_size = GNUNET_CRYPTO_rsa_public_key_encode (
      rcs[0].new_coins[i].dk->rsa_public_key,
      &buf);
    GNUNET_CRYPTO_hash_context_read (hash_context,
                                     buf,
                                     buf_size);
    GNUNET_free (buf);
  }

  /* next, add public key of coin and amount being refreshed */
  {
    struct TALER_AmountNBO melt_amountn;

    GNUNET_CRYPTO_hash_context_read (hash_context,
                                     coin_pub,
                                     sizeof (struct TALER_CoinSpendPublicKeyP));
    TALER_amount_hton (&melt_amountn,
                       amount_with_fee);
    GNUNET_CRYPTO_hash_context_read (hash_context,
                                     &melt_amountn,
                                     sizeof (struct TALER_AmountNBO));
  }

  /* finally, add all the envelopes */
  for (unsigned int i = 0; i<kappa; i++)
  {
    const struct TALER_RefreshCommitmentEntry *rce = &rcs[i];

    for (unsigned int j = 0; j<num_new_coins; j++)
    {
      const struct TALER_RefreshCoinData *rcd = &rce->new_coins[j];

      GNUNET_CRYPTO_hash_context_read (hash_context,
                                       rcd->coin_ev,
                                       rcd->coin_ev_size);
    }
  }

  /* Conclude */
  GNUNET_CRYPTO_hash_context_finish (hash_context,
                                     &rc->session_hash);
}


/* end of crypto.c */
