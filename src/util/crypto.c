/*
  This file is part of TALER
  Copyright (C) 2014, 2015 GNUnet e.V.

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

#if HAVE_GNUNET_GNUNET_UTIL_TALER_WALLET_LIB_H
#include "taler_util_wallet.h"
#endif
#if HAVE_GNUNET_GNUNET_UTIL_LIB_H
#include "taler_util.h"
#endif
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
  fprintf (stderr,
           "Fatal error in libgcrypt: %s\n",
           msg);
  abort();
}


/**
 * Initialize libgcrypt.
 */
void  __attribute__ ((constructor))
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
  /* Disable secure memory.  */
  gcry_control (GCRYCTL_DISABLE_SECMEM, 0);
  gcry_control (GCRYCTL_INITIALIZATION_FINISHED, 0);
}


/**
 * Check if a coin is valid; that is, whether the denomination key exists,
 * is not expired, and the signature is correct.
 *
 * @param coin_public_info the coin public info to check for validity
 * @return #GNUNET_YES if the coin is valid,
 *         #GNUNET_NO if it is invalid
 *         #GNUNET_SYSERR if an internal error occured
 */
int
TALER_test_coin_valid (const struct TALER_CoinPublicInfo *coin_public_info)
{
  struct GNUNET_HashCode c_hash;

  GNUNET_CRYPTO_hash (&coin_public_info->coin_pub,
                      sizeof (struct GNUNET_CRYPTO_EcdsaPublicKey),
                      &c_hash);
  if (GNUNET_OK !=
      GNUNET_CRYPTO_rsa_verify (&c_hash,
                                coin_public_info->denom_sig.rsa_signature,
                                coin_public_info->denom_pub.rsa_public_key))
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
TALER_link_derive_transfer_secret (const struct TALER_CoinSpendPrivateKeyP *coin_priv,
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
TALER_link_reveal_transfer_secret (const struct TALER_TransferPrivateKeyP *trans_priv,
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
TALER_link_recover_transfer_secret (const struct TALER_TransferPublicKeyP *trans_pub,
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
 * @param[out] fc value to initialize
 */
void
TALER_setup_planchet (const struct TALER_TransferSecretP *secret_seed,
                        unsigned int coin_num_salt,
                        struct TALER_PlanchetSecretsP *fc)
{
  uint32_t be_salt = htonl (coin_num_salt);
  uint8_t *p;

  GNUNET_assert (GNUNET_OK ==
                 GNUNET_CRYPTO_kdf (fc,
                                    sizeof (*fc),
                                    &be_salt,
                                    sizeof (be_salt),
                                    secret_seed,
                                    sizeof (*secret_seed),
                                    "taler-coin-derivation",
                                    strlen ("taler-coin-derivation"),
                                    NULL, 0));

  /* Taken from like 170-172 of libgcrypt/cipher/ecc.c
   * We note that libgcrypt stores the private key in the reverse order
   * from many Ed25519 implementatons. */
  p = (uint8_t *) &(fc->coin_priv);
  p[0] &= 0x7f;  /* Clear bit 255. */
  p[0] |= 0x40;  /* Set bit 254.   */
  p[31] &= 0xf8; /* Clear bits 2..0 so that d mod 8 == 0  */

  /* FIXME: Run GNUNET_CRYPTO_ecdhe_key_create several times and inspect
   * the output to verify that the same bits are set and cleared.
   * Is it worth also adding a test case that runs gcry_pk_testkey on
   * this key after first parsing it into libgcrypt's s-expression mess
   * ala decode_private_eddsa_key from gnunet/src/util/crypto_ecc.c?
   * It'd run check_secret_key but not test_keys from libgcrypt/cipher/ecc.c */
}




/* end of crypto.c */
