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
 * @file json/json_wire.c
 * @brief helper functions to generate or check /wire replies
 * @author Christian Grothoff
 */
#include "platform.h"
#include <gnunet/gnunet_util_lib.h>
#include "taler_util.h"
#include "taler_json_lib.h"
#include "taler_wire_lib.h"


/**
 * Compute the hash of the given wire details.   The resulting
 * hash is what is put into the contract.
 *
 * @param wire_s wire details to hash
 * @param hc[out] set to the hash
 * @return #GNUNET_OK on success, #GNUNET_SYSERR if @a wire_s is malformed
 */
int
TALER_JSON_exchange_wire_signature_hash (const json_t *wire_s,
                                         struct GNUNET_HashCode *hc)
{
  const char *payto_url;
  struct GNUNET_JSON_Specification spec[] = {
    GNUNET_JSON_spec_string ("url", &payto_url),
    GNUNET_JSON_spec_end ()
  };

  if (GNUNET_OK !=
      GNUNET_JSON_parse (wire_s,
                         spec,
                         NULL, NULL))
  {
    GNUNET_break_op (0);
    return GNUNET_SYSERR;
  }
  TALER_exchange_wire_signature_hash (payto_url,
                                      hc);
  return GNUNET_OK;
}


/**
 * Compute the hash of the given wire details.   The resulting
 * hash is what is put into the contract.
 *
 * @param wire_s wire details to hash
 * @param hc[out] set to the hash
 * @return #GNUNET_OK on success, #GNUNET_SYSERR if @a wire_s is malformed
 */
int
TALER_JSON_merchant_wire_signature_hash (const json_t *wire_s,
                                         struct GNUNET_HashCode *hc)
{
  const char *payto_url;
  const char *salt;
  struct GNUNET_JSON_Specification spec[] = {
    GNUNET_JSON_spec_string ("url", &payto_url),
    GNUNET_JSON_spec_string ("salt", &salt),
    GNUNET_JSON_spec_end ()
  };

  if (GNUNET_OK !=
      GNUNET_JSON_parse (wire_s,
                         spec,
                         NULL, NULL))
  {
    GNUNET_break_op (0);
    return GNUNET_SYSERR;
  }
  TALER_merchant_wire_signature_hash (payto_url,
                                      salt,
                                      hc);
  return GNUNET_OK;
}


/**
 * Check the signature in @a wire_s.
 *
 * @param wire_s signed wire information of an exchange
 * @param master_pub master public key of the exchange
 * @return #GNUNET_OK if signature is valid
 */
int
TALER_JSON_exchange_wire_signature_check (const json_t *wire_s,
                                          const struct TALER_MasterPublicKeyP *master_pub)
{
  const char *payto_url;
  struct TALER_MasterSignatureP master_sig;
  struct TALER_MasterPublicKeyP master_pub_from_wire;
  struct GNUNET_JSON_Specification spec[] = {
    GNUNET_JSON_spec_string ("url", &payto_url),
    GNUNET_JSON_spec_fixed_auto ("master_sig", &master_sig),
    GNUNET_JSON_spec_fixed_auto ("master_pub", &master_pub_from_wire),
    GNUNET_JSON_spec_end ()
  };

  if (0 != memcmp (&master_pub_from_wire, master_pub))
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "wire signature has an unexpected master public key\n");
    return GNUNET_SYSERR;
  }

  if (GNUNET_OK !=
      GNUNET_JSON_parse (wire_s,
                         spec,
                         NULL, NULL))
  {
    GNUNET_break_op (0);
    return GNUNET_SYSERR;
  }
  return TALER_exchange_wire_signature_check (payto_url,
                                              master_pub,
                                              &master_sig);
}


/**
 * Create a signed wire statement for the given account.
 *
 * @param payto_url account specification
 * @param master_priv private key to sign with, NULL to not sign
 */
json_t *
TALER_JSON_exchange_wire_signature_make (const char *payto_url,
                                         const struct TALER_MasterPrivateKeyP *master_priv)
{
  struct TALER_MasterSignatureP master_sig;
  struct TALER_MasterPublicKeyP master_pub;

  GNUNET_CRYPTO_eddsa_key_get_public (&master_priv->eddsa_priv,
                                      &master_pub.eddsa_pub);

  TALER_exchange_wire_signature_make (payto_url,
                                      master_priv,
                                      &master_sig);
  return json_pack ("{s:s, s:o, s:o}",
                    "url", payto_url,
                    "master_sig", GNUNET_JSON_from_data_auto (&master_sig),
                    "master_pub", GNUNET_JSON_from_data_auto (&master_pub));
}


/**
 * Obtain the wire method associated with the given
 * wire account details.  @a wire_s must contain a payto://-URL
 * under 'url'.
 *
 * @return NULL on error
 */
char *
TALER_JSON_wire_to_payto (const json_t *wire_s)
{
  json_t *payto_o;
  const char *payto_str;

  payto_o = json_object_get (wire_s,
                             "url");
  if ( (NULL == payto_o) ||
       (NULL == (payto_str = json_string_value (payto_o))) )
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Fatally malformed wire record encountered: lacks payto://-url\n");
    return NULL;
  }
  return GNUNET_strdup (payto_str);
}


/**
 * Obtain the wire method associated with the given
 * wire account details.  @a wire_s must contain a payto://-URL
 * under 'url'.
 *
 * @return NULL on error
 */
char *
TALER_JSON_wire_to_method (const json_t *wire_s)
{
  json_t *payto_o;
  const char *payto_str;

  payto_o = json_object_get (wire_s,
                             "url");
  if ( (NULL == payto_o) ||
       (NULL == (payto_str = json_string_value (payto_o))) )
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Fatally malformed wire record encountered: lacks payto://-url\n");
    return NULL;
  }
  return TALER_WIRE_payto_get_method (payto_str);
}


/* end of json_wire.c */
