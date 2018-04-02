/*
  This file is part of TALER
  Copyright (C) 2018 Taler Systems SA

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
 * @file util/crypto_wire.c
 * @brief functions for making and verifying /wire account signatures
 * @author Christian Grothoff <christian@grothoff.org>
 */
#include "platform.h"
#include "taler_crypto_lib.h"
#include "taler_signatures.h"

/**
 * Compute the hash of the given wire details.   The resulting
 * hash is what is put into the contract.
 *
 * @param payto_url bank account
 * @param salt salt used to eliminate brute-force inversion
 * @param hc[out] set to the hash
 */
void
TALER_wire_signature_hash (const char *payto_url,
                           const char *salt,
                           struct GNUNET_HashCode *hc)
{
  GNUNET_assert (GNUNET_YES ==
                 GNUNET_CRYPTO_kdf (hc,
                                    sizeof (*hc),
                                    salt,
                                    strlen (salt) + 1,
                                    payto_url,
                                    strlen (payto_url) + 1,
                                    "wire-signature",
                                    strlen ("wire-signature"),
                                    NULL, 0));
}


/**
 * Check the signature in @a wire_s.
 *
 * @param payto_url URL that is signed
 * @param salt the salt used to salt the @a payto_url when hashing
 * @param master_pub master public key of the exchange
 * @param master_sig signature of the exchange
 * @return #GNUNET_OK if signature is valid
 */
int
TALER_wire_signature_check (const char *payto_url,
                            const char *salt,
                            const struct TALER_MasterPublicKeyP *master_pub,
                            const struct TALER_MasterSignatureP *master_sig)
{
  struct TALER_MasterWireDetailsPS wd;

  wd.purpose.purpose = htonl (TALER_SIGNATURE_MASTER_WIRE_DETAILS);
  wd.purpose.size = htonl (sizeof (wd));
  TALER_wire_signature_hash (payto_url,
                             salt,
                             &wd.h_wire_details);
  return GNUNET_CRYPTO_eddsa_verify (TALER_SIGNATURE_MASTER_WIRE_DETAILS,
                                     &wd.purpose,
                                     &master_sig->eddsa_signature,
                                     &master_pub->eddsa_pub);
}


/**
 * Create a signed wire statement for the given account.
 *
 * @param payto_url account specification
 * @param salt the salt used to salt the @a payto_url when hashing
 * @param master_priv private key to sign with
 * @param master_sig[out] where to write the signature
 */
void
TALER_wire_signature_make (const char *payto_url,
                           const char *salt,
                           const struct TALER_MasterPrivateKeyP *master_priv,
                           struct TALER_MasterSignatureP *master_sig)
{
  struct TALER_MasterWireDetailsPS wd;

  wd.purpose.purpose = htonl (TALER_SIGNATURE_MASTER_WIRE_DETAILS);
  wd.purpose.size = htonl (sizeof (wd));
  TALER_wire_signature_hash (payto_url,
                             salt,
                             &wd.h_wire_details);
  GNUNET_assert (GNUNET_OK ==
                 GNUNET_CRYPTO_eddsa_sign (&master_priv->eddsa_priv,
                                           &wd.purpose,
                                           &master_sig->eddsa_signature));
}


/* end of crypto_wire.c */
