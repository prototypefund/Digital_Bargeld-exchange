/*
  This file is part of TALER
  Copyright (C) 2015 GNUnet e.V.

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
 * @file taler-mint-httpd_test.c
 * @brief Handle /test requests; parses the POST and JSON and
 *        checks that the client is binary-compatible
 * @author Christian Grothoff
 */
#include "platform.h"
#include <gnunet/gnunet_util_lib.h>
#include <jansson.h>
#include <microhttpd.h>
#include "taler_signatures.h"
#include "taler_util.h"
#include "taler-mint-httpd_test.h"
#include "taler-mint-httpd_parsing.h"
#include "taler-mint-httpd_responses.h"


/**
 * Handle a "/test/base32" request.  Parses the JSON in the post, runs
 * the Crockford Base32 decoder on the "input" field in the JSON,
 * hashes the result and sends the hashed value back as a JSON 
 * string with in Base32 Crockford encoding.  Thus, this API
 * allows testing the hashing and Crockford encoding/decoding
 * functions.
 *
 * @param rh context of the handler
 * @param connection the MHD connection to handle
 * @param[in,out] connection_cls the connection's closure (can be updated)
 * @param upload_data upload data
 * @param[in,out] upload_data_size number of bytes (left) in @a upload_data
 * @return MHD result code
 */
int
TMH_TEST_handler_test_base32 (struct TMH_RequestHandler *rh,
			      struct MHD_Connection *connection,
			      void **connection_cls,
			      const char *upload_data,
			      size_t *upload_data_size)
{
  json_t *json;
  int res;
  struct GNUNET_HashCode hc;
  struct TMH_PARSE_FieldSpecification spec[] = {
    TMH_PARSE_MEMBER_VARIABLE ("input"),
    TMH_PARSE_MEMBER_END
  };

  res = TMH_PARSE_post_json (connection,
                             connection_cls,
                             upload_data,
                             upload_data_size,
                             &json);
  if (GNUNET_SYSERR == res)
    return MHD_NO;
  if ( (GNUNET_NO == res) || (NULL == json) )
    return MHD_YES;
  res = TMH_PARSE_json_data (connection,
			     json,
			     spec);
  if (GNUNET_YES != res)
    return (GNUNET_NO == res) ? MHD_YES : MHD_NO;
  GNUNET_CRYPTO_hash (spec[0].destination,
		      spec[0].destination_size_out,
		      &hc);
  TMH_PARSE_release_data (spec);
  json_decref (json);
  return TMH_RESPONSE_reply_json_pack (connection,
				       MHD_HTTP_OK,
				       "{s:o}",
				       "output",
				       TALER_json_from_data (&hc, sizeof (struct GNUNET_HashCode)));
}


/**
 * Handle a "/test/encrypt" request.  Parses the JSON in the post,
 * runs the Crockford Base32 decoder on the "input" field in the JSON,
 * and encrypts the result with a shared secret derived using the HKDF
 * function with salt "skey" and IV derived with salt "iv" of the
 * Crockford Base32-encoded "key_hash" field in the JSON.  The
 * symmetric encryption is the AES/Twofish double-encryption used in
 * Taler/GNUnet.  The resulting ciphertext is returned as a Crockford
 * Base32 encoded JSON string.
 *
 * @param rh context of the handler
 * @param connection the MHD connection to handle
 * @param[in,out] connection_cls the connection's closure (can be updated)
 * @param upload_data upload data
 * @param[in,out] upload_data_size number of bytes (left) in @a upload_data
 * @return MHD result code
 */
int
TMH_TEST_handler_test_encrypt (struct TMH_RequestHandler *rh,
			       struct MHD_Connection *connection,
			       void **connection_cls,
			       const char *upload_data,
			       size_t *upload_data_size)
{
  json_t *json;
  int res;
  struct GNUNET_HashCode key;
  struct GNUNET_CRYPTO_SymmetricInitializationVector iv;
  struct GNUNET_CRYPTO_SymmetricSessionKey skey;
  struct TMH_PARSE_FieldSpecification spec[] = {
    TMH_PARSE_MEMBER_VARIABLE ("input"),
    TMH_PARSE_MEMBER_FIXED ("key_hash", &key),
    TMH_PARSE_MEMBER_END
  };
  char *out;

  res = TMH_PARSE_post_json (connection,
                             connection_cls,
                             upload_data,
                             upload_data_size,
                             &json);
  if (GNUNET_SYSERR == res)
    return MHD_NO;
  if ( (GNUNET_NO == res) || (NULL == json) )
    return MHD_YES;
  res = TMH_PARSE_json_data (connection,
			     json,
			     spec);
  json_decref (json);
  if (GNUNET_YES != res)
    return (GNUNET_NO == res) ? MHD_YES : MHD_NO;
  GNUNET_assert (GNUNET_YES ==
		 GNUNET_CRYPTO_kdf (&skey, sizeof (struct GNUNET_CRYPTO_SymmetricSessionKey),
				    "skey", strlen ("skey"),
				    &key, sizeof (key),
				    NULL, 0));
  GNUNET_assert (GNUNET_YES ==
		 GNUNET_CRYPTO_kdf (&iv, sizeof (struct GNUNET_CRYPTO_SymmetricInitializationVector),
				    "iv", strlen ("iv"),
				    &key, sizeof (key),
				    NULL, 0));
  out = GNUNET_malloc (spec[0].destination_size_out);
  GNUNET_break (spec[0].destination_size_out ==
		GNUNET_CRYPTO_symmetric_encrypt (spec[0].destination,
						 spec[0].destination_size_out,
						 &skey,
						 &iv,
						 out));
  json = TALER_json_from_data (out,
			       spec[0].destination_size_out);
  GNUNET_free (out);
  TMH_PARSE_release_data (spec);
  return TMH_RESPONSE_reply_json_pack (connection,
				       MHD_HTTP_OK,
				       "{s:o}",
				       "output",
				       json);
}


/**
 * Handle a "/test/hkdf" request.  Parses the JSON in the post, runs
 * the Crockford Base32 decoder on the "input" field in the JSON,
 * computes `HKDF(input, "salty")` and sends the result back as a JSON
 * string with in Base32 Crockford encoding.  Thus, this API allows
 * testing the use of the (H)KDF.  Note that the test fixes the
 * input and output sizes and the salt (and the hash functions used
 * by the HKDF), so this is only useful to test the HKDF in the
 * same way it will be used within Taler/GNUnet.
 *
 * @param rh context of the handler
 * @param connection the MHD connection to handle
 * @param[in,out] connection_cls the connection's closure (can be updated)
 * @param upload_data upload data
 * @param[in,out] upload_data_size number of bytes (left) in @a upload_data
 * @return MHD result code
 */
int
TMH_TEST_handler_test_hkdf (struct TMH_RequestHandler *rh,
			    struct MHD_Connection *connection,
			    void **connection_cls,
			    const char *upload_data,
			    size_t *upload_data_size)
{
  json_t *json;
  int res;
  struct GNUNET_HashCode hc;
  struct TMH_PARSE_FieldSpecification spec[] = {
    TMH_PARSE_MEMBER_VARIABLE ("input"),
    TMH_PARSE_MEMBER_END
  };
  
  res = TMH_PARSE_post_json (connection,
                             connection_cls,
                             upload_data,
                             upload_data_size,
                             &json);
  if (GNUNET_SYSERR == res)
    return MHD_NO;
  if ( (GNUNET_NO == res) || (NULL == json) )
    return MHD_YES;
  res = TMH_PARSE_json_data (connection,
			     json,
			     spec);
  json_decref (json);
  if (GNUNET_YES != res)
    return (GNUNET_NO == res) ? MHD_YES : MHD_NO;
  GNUNET_CRYPTO_kdf (&hc, sizeof (hc),
		     "salty", strlen ("salty"),
		     spec[0].destination,
		     spec[0].destination_size_out,
		     NULL, 0);
  TMH_PARSE_release_data (spec);
  json = TALER_json_from_data (&hc,
			       sizeof (struct GNUNET_HashCode));
  return TMH_RESPONSE_reply_json_pack (connection,
				       MHD_HTTP_OK,
				       "{s:o}",
				       "output",
				       json);
}


/**
 * Handle a "/test/ecdhe" request.  Parses the JSON in the post, which
 * must contain a "ecdhe_pub" with a public key and an "ecdhe_priv"
 * with a private key.  The reply is the resulting JSON is an object
 * with the field "ecdh_hash" containing a Crockford Base32-encoded
 * string representing the hash derived via ECDH of the two keys.
 *
 * @param rh context of the handler
 * @param connection the MHD connection to handle
 * @param[in,out] connection_cls the connection's closure (can be updated)
 * @param upload_data upload data
 * @param[in,out] upload_data_size number of bytes (left) in @a upload_data
 * @return MHD result code
  */
int
TMH_TEST_handler_test_ecdhe (struct TMH_RequestHandler *rh,
			     struct MHD_Connection *connection,
			     void **connection_cls,
			     const char *upload_data,
			     size_t *upload_data_size)
{
  json_t *json;
  int res;
  struct GNUNET_CRYPTO_EcdhePublicKey pub;
  struct GNUNET_CRYPTO_EcdhePrivateKey priv;
  struct GNUNET_HashCode hc;
  struct TMH_PARSE_FieldSpecification spec[] = {
    TMH_PARSE_MEMBER_FIXED ("ecdhe_pub", &pub),
    TMH_PARSE_MEMBER_FIXED ("ecdhe_priv", &priv),
    TMH_PARSE_MEMBER_END
  };

  res = TMH_PARSE_post_json (connection,
                             connection_cls,
                             upload_data,
                             upload_data_size,
                             &json);
  if (GNUNET_SYSERR == res)
    return MHD_NO;
  if ( (GNUNET_NO == res) || (NULL == json) )
    return MHD_YES;
  res = TMH_PARSE_json_data (connection,
			     json,
			     spec);
  json_decref (json);
  if (GNUNET_YES != res)
    return (GNUNET_NO == res) ? MHD_YES : MHD_NO;
  if (GNUNET_OK !=
      GNUNET_CRYPTO_ecc_ecdh (&priv,
			      &pub,
			      &hc))
  {
    TMH_PARSE_release_data (spec);
    return TMH_RESPONSE_reply_internal_error (connection,
					      "Failed to perform ECDH");
  }
  TMH_PARSE_release_data (spec);
  return TMH_RESPONSE_reply_json_pack (connection,
				       MHD_HTTP_OK,
				       "{s:o}",
				       "ecdh_hash",
				       TALER_json_from_data (&hc,
							     sizeof (hc)));
}


/**
 * Handle a "/test/eddsa" request.  Parses the JSON in the post, 
 * which must contain a "eddsa_pub" with a public key and an
 *"eddsa_sig" with the corresponding signature for a purpose
 * of #TALER_SIGNATURE_CLIENT_TEST_EDDSA.  If the signature is
 * valid, a reply with a #TALER_SIGNATURE_MINT_TEST_EDDSA is 
 * returned using the same JSON format.
 *
 * @param rh context of the handler
 * @param connection the MHD connection to handle
 * @param[in,out] connection_cls the connection's closure (can be updated)
 * @param upload_data upload data
 * @param[in,out] upload_data_size number of bytes (left) in @a upload_data
 * @return MHD result code
  */
int
TMH_TEST_handler_test_eddsa (struct TMH_RequestHandler *rh,
			     struct MHD_Connection *connection,
			     void **connection_cls,
			     const char *upload_data,
			     size_t *upload_data_size)
{
  json_t *json;
  int res;
  struct GNUNET_CRYPTO_EddsaPublicKey pub;
  struct GNUNET_CRYPTO_EddsaSignature sig;
  struct GNUNET_CRYPTO_EccSignaturePurpose purpose;
  struct TMH_PARSE_FieldSpecification spec[] = {
    TMH_PARSE_MEMBER_FIXED ("eddsa_pub", &pub),
    TMH_PARSE_MEMBER_FIXED ("eddsa_sig", &sig),
    TMH_PARSE_MEMBER_END
  };
  struct GNUNET_CRYPTO_EddsaPrivateKey *pk;

  res = TMH_PARSE_post_json (connection,
                             connection_cls,
                             upload_data,
                             upload_data_size,
                             &json);
  if (GNUNET_SYSERR == res)
    return MHD_NO;
  if ( (GNUNET_NO == res) || (NULL == json) )
    return MHD_YES;
  res = TMH_PARSE_json_data (connection,
			     json,
			     spec);
  json_decref (json);
  if (GNUNET_YES != res)
    return (GNUNET_NO == res) ? MHD_YES : MHD_NO;
  purpose.size = htonl (sizeof (struct GNUNET_CRYPTO_EccSignaturePurpose));
  purpose.purpose = htonl (TALER_SIGNATURE_CLIENT_TEST_EDDSA);
  if (GNUNET_OK !=
      GNUNET_CRYPTO_eddsa_verify (TALER_SIGNATURE_CLIENT_TEST_EDDSA,
				  &purpose,
				  &sig,
				  &pub))
  {
    TMH_PARSE_release_data (spec);
    return TMH_RESPONSE_reply_signature_invalid (connection,
						 "eddsa_sig");
  }
  TMH_PARSE_release_data (spec);
  pk = GNUNET_CRYPTO_eddsa_key_create ();
  purpose.purpose = htonl (TALER_SIGNATURE_MINT_TEST_EDDSA);
  if (GNUNET_OK !=
      GNUNET_CRYPTO_eddsa_sign (pk,
				&purpose,
				&sig))
  {
    GNUNET_free (pk);
    return TMH_RESPONSE_reply_internal_error (connection,
					      "Failed to EdDSA-sign");
  }
  GNUNET_CRYPTO_eddsa_key_get_public (pk,
				      &pub);
  GNUNET_free (pk);
  return TMH_RESPONSE_reply_json_pack (connection,
				       MHD_HTTP_OK,
				       "{s:o, s:o}",
				       "eddsa_pub",
				       TALER_json_from_data (&pub,
							     sizeof (pub)),
				       "eddsa_sig",
				       TALER_json_from_data (&sig,
							     sizeof (sig)));
}


/**
 * Handle a "/test/rsa" request.  Parses the JSON in the post, which
 * must contain an "blind_ev" blinded value.  An RSA public key
 * ("rsa_pub") and a blinded signature ("rsa_blind_sig") are returned.
 *
 * @param rh context of the handler
 * @param connection the MHD connection to handle
 * @param[in,out] connection_cls the connection's closure (can be updated)
 * @param upload_data upload data
 * @param[in,out] upload_data_size number of bytes (left) in @a upload_data
 * @return MHD result code
  */
int
TMH_TEST_handler_test_rsa (struct TMH_RequestHandler *rh,
			   struct MHD_Connection *connection,
			   void **connection_cls,
			   const char *upload_data,
			   size_t *upload_data_size)
{
  json_t *json;
  int res;
  struct GNUNET_CRYPTO_rsa_PublicKey *pub;
  struct GNUNET_CRYPTO_rsa_Signature *sig;
  struct TMH_PARSE_FieldSpecification spec[] = {
    TMH_PARSE_MEMBER_VARIABLE ("blind_ev"),
    TMH_PARSE_MEMBER_END
  };
  struct GNUNET_CRYPTO_rsa_PrivateKey *pk;

  res = TMH_PARSE_post_json (connection,
                             connection_cls,
                             upload_data,
                             upload_data_size,
                             &json);
  if (GNUNET_SYSERR == res)
    return MHD_NO;
  if ( (GNUNET_NO == res) || (NULL == json) )
    return MHD_YES;
  res = TMH_PARSE_json_data (connection,
			     json,
			     spec);
  json_decref (json);
  if (GNUNET_YES != res)
    return (GNUNET_NO == res) ? MHD_YES : MHD_NO;
  pk = GNUNET_CRYPTO_rsa_private_key_create (1024);
  if (NULL == pk)
  {
    GNUNET_break (0);
    TMH_PARSE_release_data (spec);
    return TMH_RESPONSE_reply_internal_error (connection,
					      "Failed to create RSA key");
  }
  sig = GNUNET_CRYPTO_rsa_sign (pk,
				spec[0].destination,
				spec[0].destination_size_out);
  if (NULL == sig)
  {
    GNUNET_break (0);
    GNUNET_CRYPTO_rsa_private_key_free (pk);
    TMH_PARSE_release_data (spec);
    return TMH_RESPONSE_reply_internal_error (connection,
					      "Failed to RSA-sign");
  }
  TMH_PARSE_release_data (spec);
  pub = GNUNET_CRYPTO_rsa_private_key_get_public (pk);
  GNUNET_CRYPTO_rsa_private_key_free (pk);
  if (NULL == pub)
  {
    GNUNET_break (0);
    GNUNET_CRYPTO_rsa_signature_free (sig);
    return TMH_RESPONSE_reply_internal_error (connection,
					      "Failed to get public RSA key");
  }
  res = TMH_RESPONSE_reply_json_pack (connection,
				      MHD_HTTP_OK,
				      "{s:o, s:o}",
				      "rsa_pub",
				      TALER_json_from_rsa_public_key (pub),
				      "rsa_blind_sig",
				      TALER_json_from_rsa_signature (sig));
  GNUNET_CRYPTO_rsa_signature_free (sig);
  GNUNET_CRYPTO_rsa_public_key_free (pub);
  return res;
}


/**
 * Handle a "/test/transfer" request.  Parses the JSON in the post, 
 * which must contain a "secret_enc" with the encrypted link secret,
 * a "trans_priv" with the transfer private key, a "coin_pub" with
 * a coin public key.  A reply with the decrypted "secret" is
 * returned.
 *
 * @param rh context of the handler
 * @param connection the MHD connection to handle
 * @param[in,out] connection_cls the connection's closure (can be updated)
 * @param upload_data upload data
 * @param[in,out] upload_data_size number of bytes (left) in @a upload_data
 * @return MHD result code
  */
int
TMH_TEST_handler_test_transfer (struct TMH_RequestHandler *rh,
				struct MHD_Connection *connection,
				void **connection_cls,
				const char *upload_data,
				size_t *upload_data_size)
{
  json_t *json;
  int res;
  struct TALER_EncryptedLinkSecretP secret_enc;
  struct TALER_TransferPrivateKeyP trans_priv;
  struct TALER_CoinSpendPublicKeyP coin_pub;
  struct TMH_PARSE_FieldSpecification spec[] = {
    TMH_PARSE_MEMBER_FIXED ("secret_enc", &secret_enc),
    TMH_PARSE_MEMBER_FIXED ("trans_priv", &trans_priv),
    TMH_PARSE_MEMBER_FIXED ("coin_pub", &coin_pub),
    TMH_PARSE_MEMBER_END
  };
  struct TALER_LinkSecretP secret;

  res = TMH_PARSE_post_json (connection,
                             connection_cls,
                             upload_data,
                             upload_data_size,
                             &json);
  if (GNUNET_SYSERR == res)
    return MHD_NO;
  if ( (GNUNET_NO == res) || (NULL == json) )
    return MHD_YES;
  res = TMH_PARSE_json_data (connection,
			     json,
			     spec);
  json_decref (json);
  if (GNUNET_YES != res)
    return (GNUNET_NO == res) ? MHD_YES : MHD_NO;
  if (GNUNET_OK !=
      TALER_link_decrypt_secret (&secret_enc,
				 &trans_priv,
				 &coin_pub,
				 &secret))
  {
    TMH_PARSE_release_data (spec);
    return TMH_RESPONSE_reply_internal_error (connection,
					      "Failed to decrypt secret");
  }
  return TMH_RESPONSE_reply_json_pack (connection,
				       MHD_HTTP_OK,
				       "{s:o}",
				       "secret",
				       TALER_json_from_data (&secret,
							     sizeof (secret)));
}


/**
 * Handle a "/test" request.  Parses the JSON in the post.
 *
 * @param rh context of the handler
 * @param connection the MHD connection to handle
 * @param[in,out] connection_cls the connection's closure (can be updated)
 * @param upload_data upload data
 * @param[in,out] upload_data_size number of bytes (left) in @a upload_data
 * @return MHD result code
  */
int
TMH_TEST_handler_test (struct TMH_RequestHandler *rh,
		       struct MHD_Connection *connection,
		       void **connection_cls,
		       const char *upload_data,
		       size_t *upload_data_size)
{
  json_t *json;
  int res;

  res = TMH_PARSE_post_json (connection,
                             connection_cls,
                             upload_data,
                             upload_data_size,
                             &json);
  if (GNUNET_SYSERR == res)
    return MHD_NO;
  if ( (GNUNET_NO == res) || (NULL == json) )
    return MHD_YES;

  json_decref (json);
  return MHD_NO;
}


/* end of taler-mint-httpd_test.c */
