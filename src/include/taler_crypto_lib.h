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
  TALER; see the file COPYING.  If not, If not, see <http://www.gnu.org/licenses/>
*/
/**
 * @file include/taler_crypto_lib.h
 * @brief taler-specific crypto functions
 * @author Sree Harsha Totakura <sreeharsha@totakura.in>
 */
#ifndef TALER_CRYPTO_LIB_H
#define TALER_CRYPTO_LIB_H

#include <gnunet/gnunet_util_lib.h>
#include <gcrypt.h>


/* ****************** Coin crypto primitives ************* */

/**
 * Type of public keys for Taler reserves.
 */
struct TALER_ReservePublicKey
{
  /**
   * Taler uses EdDSA for reserves.
   */
  struct GNUNET_CRYPTO_EddsaPublicKey eddsa_pub;
};


/**
 * Type of private keys for Taler reserves.
 */
struct TALER_ReservePrivateKey
{
  /**
   * Taler uses EdDSA for reserves.
   */
  struct GNUNET_CRYPTO_EddsaPrivateKey eddsa_priv;
};


/**
 * Type of signatures used with Taler reserves.
 */
struct TALER_ReserveSignature
{
  /**
   * Taler uses EdDSA for reserves.
   */
  struct GNUNET_CRYPTO_EddsaSignature eddsa_signature;
};


/**
 * Type of public keys to for merchant authorizations.
 * Merchants can issue refunds using the corresponding
 * private key.
 */
struct TALER_MerchantPublicKey
{
  /**
   * Taler uses EdDSA for merchants.
   */
  struct GNUNET_CRYPTO_EddsaPublicKey eddsa_pub;
};


/**
 * Type of private keys for merchant authorizations.
 * Merchants can issue refunds using the corresponding
 * private key.
 */
struct TALER_MerchantPrivateKey
{
  /**
   * Taler uses EdDSA for merchants.
   */
  struct GNUNET_CRYPTO_EddsaPrivateKey eddsa_priv;
};


/**
 * Type of public keys used by clients to sign
 * messages during a melting session.
 */
struct TALER_SessionPublicKey
{
  /**
   * Taler uses EdDSA for melting session keys.
   */
  struct GNUNET_CRYPTO_EddsaPublicKey eddsa_pub;
};


/**
 * Type of public keys used by clients to sign
 * messages during a melting session.
 */
struct TALER_SessionPrivateKey
{
  /**
   * Taler uses EdDSA for melting session keys.
   */
  struct GNUNET_CRYPTO_EddsaPrivateKey eddsa_priv;
};


/**
 * Type of transfer public keys used during refresh
 * operations.
 */
struct TALER_TransferPublicKey
{
  /**
   * Taler uses ECDSA for transfer keys.
   * FIXME: should this not be ECDHE?
   */
  struct GNUNET_CRYPTO_EcdsaPublicKey ecdsa_pub;
};


/**
 * Type of transfer public keys used during refresh
 * operations.
 */
struct TALER_TransferPrivateKey
{
  /**
   * Taler uses ECDSA for melting session keys.
   * FIXME: should this not be ECDHE?
   */
  struct GNUNET_CRYPTO_EcdsaPrivateKey ecdsa_priv;
};


/**
 * Type of signatures used by clients to sign
 * messages during a melting session.
 */
struct TALER_SessionSignature
{
  /**
   * Taler uses EdDSA for melting session keys.
   */
  struct GNUNET_CRYPTO_EddsaSignature eddsa_signature;
};


/**
 * Type of online public keys used by the mint to sign
 * messages.
 */
struct TALER_MintPublicKey
{
  /**
   * Taler uses EdDSA for online mint message signing.
   */
  struct GNUNET_CRYPTO_EddsaPublicKey eddsa_pub;
};


/**
 * Type of online public keys used by the mint to
 * sign messages.
 */
struct TALER_MintPrivateKey
{
  /**
   * Taler uses EdDSA for online signatures sessions.
   */
  struct GNUNET_CRYPTO_EddsaPrivateKey eddsa_priv;
};


/**
 * Type of signatures used by the mint to sign messages online.
 */
struct TALER_MintSignature
{
  /**
   * Taler uses EdDSA for online signatures sessions.
   */
  struct GNUNET_CRYPTO_EddsaSignature eddsa_signature;
};


/**
 * Type of the offline master public key used by the mint.
 */
struct TALER_MasterPublicKey
{
  /**
   * Taler uses EdDSA for the long-term offline master key.
   */
  struct GNUNET_CRYPTO_EddsaPublicKey eddsa_pub;
};


/**
 * Type of the offline master public keys used by the mint.
 */
struct TALER_MasterPrivateKey
{
  /**
   * Taler uses EdDSA for the long-term offline master key.
   */
  struct GNUNET_CRYPTO_EddsaPrivateKey eddsa_priv;
};


/**
 * Type of signatures by the offline master public key used by the mint.
 */
struct TALER_MasterSignature
{
  /**
   * Taler uses EdDSA for the long-term offline master key.
   */
  struct GNUNET_CRYPTO_EddsaSignature eddsa_signature;
};



/**
 * Type of public keys for Taler coins.
 */
struct TALER_CoinSpendPublicKey
{
  /**
   * Taler uses ECDSA for coins.
   */
  struct GNUNET_CRYPTO_EcdsaPublicKey ecdsa_pub;
};


/**
 * Type of private keys for Taler coins.
 */
struct TALER_CoinSpendPrivateKey
{
  /**
   * Taler uses ECDSA for coins.
   */
  struct GNUNET_CRYPTO_EcdsaPrivateKey ecdsa_priv;
};


/**
 * Type of signatures made with Taler coins.
 */
struct TALER_CoinSpendSignature
{
  /**
   * Taler uses ECDSA for coins.
   */
  struct GNUNET_CRYPTO_EcdsaSignature ecdsa_signature;
};


/**
 * Type of blinding keys for Taler.
 */
struct TALER_DenominationBlindingKey
{
  /**
   * Taler uses RSA for blinding.
   */
  struct GNUNET_CRYPTO_rsa_BlindingKey *rsa_blinding_key;
};


/**
 * Type of (unblinded) coin signatures for Taler.
 */
struct TALER_DenominationSignature
{
  /**
   * Taler uses RSA for blinding.
   */
  struct GNUNET_CRYPTO_rsa_Signature *rsa_signature;
};


/**
 * Type of public signing keys for verifying blindly signed coins.
 */
struct TALER_DenominationPublicKey
{
  /**
   * Taler uses RSA for signing coins.
   */
  struct GNUNET_CRYPTO_rsa_PublicKey *rsa_public_key;
};


/**
 * Type of private signing keys for blind signing of coins.
 */
struct TALER_DenominationPrivateKey
{
  /**
   * Taler uses RSA for signing coins.
   */
  struct GNUNET_CRYPTO_rsa_PrivateKey *rsa_private_key;
};


/**
 * Public information about a coin (including the public key
 * of the coin, the denomination key and the signature with
 * the denomination key).
 */
struct TALER_CoinPublicInfo
{
  /**
   * The coin's public key.
   */
  struct TALER_CoinSpendPublicKey coin_pub;

  /**
   * Public key representing the denomination of the coin
   * that is being deposited.
   */
  struct TALER_DenominationPublicKey denom_pub;

  /**
   * (Unblinded) signature over @e coin_pub with @e denom_pub,
   * which demonstrates that the coin is valid.
   */
  struct TALER_DenominationSignature denom_sig;
};


/**
 * Check if a coin is valid; that is, whether the denomination key exists,
 * is not expired, and the signature is correct.
 *
 * @param coin_public_info the coin public info to check for validity
 * @return #GNUNET_YES if the coin is valid,
 *         #GNUNET_NO if it is invalid
 *         #GNUNET_SYSERROR if an internal error occured
 */
int
TALER_test_coin_valid (const struct TALER_CoinPublicInfo *coin_public_info);


/* ****************** Refresh crypto primitives ************* */

/**
 * Secret used to decrypt the key to decrypt link secrets.
 */
struct TALER_TransferSecret
{
  /**
   * Secret used to encrypt/decrypt the `struct TALER_LinkSecret`.
   * Must be (currently) a hash as this is what
   * #GNUNET_CRYPTO_ecc_ecdh() returns to us.
   */
  struct GNUNET_HashCode key;
};


/**
 * Secret used to decrypt refresh links.
 */
struct TALER_LinkSecret
{
  /**
   * Secret used to decrypt the refresh link data.
   */
  char key[sizeof (struct GNUNET_HashCode)];
};


/**
 * Encrypted secret used to decrypt refresh links.
 */
struct TALER_EncryptedLinkSecret
{
  /**
   * Encrypted secret, must be the given size!
   */
  char enc[sizeof (struct TALER_LinkSecret)];
};


/**
 * Representation of an encrypted refresh link.
 */
struct TALER_RefreshLinkEncrypted
{

  /**
   * Encrypted blinding key with @e blinding_key_enc_size bytes,
   * must be allocated at the end of this struct.
   */
  const char *blinding_key_enc;

  /**
   * Number of bytes in @e blinding_key_enc.
   */
  size_t blinding_key_enc_size;

  /**
   * Encrypted private key of the coin.
   */
  char coin_priv_enc[sizeof (struct TALER_CoinSpendPrivateKey)];

};


/**
 * Representation of an refresh link in cleartext.
 */
struct TALER_RefreshLinkDecrypted
{

  /**
   * Private key of the coin.
   */
  struct TALER_CoinSpendPrivateKey coin_priv;

  /**
   * Blinding key.
   */
  struct TALER_DenominationBlindingKey blinding_key;

};


/**
 * Use the @a trans_sec (from ECDHE) to decrypt the @a secret_enc
 * to obtain the @a secret to decrypt the linkage data.
 *
 * @param secret_enc encrypted secret
 * @param trans_sec transfer secret
 * @param secret shared secret for refresh link decryption
 * @return #GNUNET_OK on success
 */
int
TALER_transfer_decrypt (const struct TALER_EncryptedLinkSecret *secret_enc,
                        const struct TALER_TransferSecret *trans_sec,
                        struct TALER_LinkSecret *secret);


/**
 * Use the @a trans_sec (from ECDHE) to encrypt the @a secret
 * to obtain the @a secret_enc.
 *
 * @param secret shared secret for refresh link decryption
 * @param trans_sec transfer secret
 * @param secret_enc[out] encrypted secret
 * @return #GNUNET_OK on success
 */
int
TALER_transfer_encrypt (const struct TALER_LinkSecret *secret,
                        const struct TALER_TransferSecret *trans_sec,
                        struct TALER_EncryptedLinkSecret *secret_enc);


/**
 * Decrypt refresh link information.
 *
 * @param input encrypted refresh link data
 * @param secret shared secret to use for decryption
 * @return NULL on error
 */
struct TALER_RefreshLinkDecrypted *
TALER_refresh_decrypt (const struct TALER_RefreshLinkEncrypted *input,
                       const struct TALER_LinkSecret *secret);


/**
 * Encrypt refresh link information.
 *
 * @param input plaintext refresh link data
 * @param secret shared secret to use for encryption
 * @return NULL on error (should never happen)
 */
struct TALER_RefreshLinkEncrypted *
TALER_refresh_encrypt (const struct TALER_RefreshLinkDecrypted *input,
                       const struct TALER_LinkSecret *secret);


/**
 * Decode encrypted refresh link information from buffer.
 *
 * @param buf buffer with refresh link data
 * @param buf_len number of bytes in @a buf
 * @return NULL on error (@a buf_len too small)
 */
struct TALER_RefreshLinkEncrypted *
TALER_refresh_link_encrypted_decode (const char *buf,
                                     size_t buf_len);



#endif
