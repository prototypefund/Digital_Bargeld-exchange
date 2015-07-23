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
 * @file mintdb/perf_taler_mintdb_init.c
 * @brief Interpreter library for mint database performance analysis
 * @author Nicolas Fournier
 */
#include "platform.h"
#include "perf_taler_mintdb_init.h"
#include <gnunet/gnunet_signatures.h>
#include "taler_signatures.h"
#include "taler_amount_lib.h"


#define CURRENCY "EUR"
#define PERF_TALER_MINTDB_RSA_SIZE 256


/**
 * Generate a dummy DenominationKeyInformation for testing purposes
 * @return a dummy denomination key
 */
struct TALER_MINTDB_DenominationKeyIssueInformation *
PERF_TALER_MINTDB_denomination_init ()
{
  struct GNUNET_CRYPTO_EddsaPrivateKey *master_prvt;
  struct TALER_MINTDB_DenominationKeyIssueInformation *dki;
  struct TALER_DenominationPrivateKey denom_priv;
  struct TALER_DenominationPublicKey denom_pub;
  struct TALER_MINTDB_DenominationKeyInformationP issue;

  master_prvt = GNUNET_CRYPTO_eddsa_key_create();

  dki = GNUNET_new (struct TALER_MINTDB_DenominationKeyIssueInformation);
  GNUNET_assert (NULL != dki);
  denom_priv.rsa_private_key
    = GNUNET_CRYPTO_rsa_private_key_create (PERF_TALER_MINTDB_RSA_SIZE);
  GNUNET_assert (NULL != denom_priv.rsa_private_key);
  denom_pub.rsa_public_key =
    GNUNET_CRYPTO_rsa_private_key_get_public (denom_priv.rsa_private_key);
  GNUNET_assert (NULL != denom_pub.rsa_public_key);
  {/* issue */
    struct TALER_MasterSignatureP signature;
    struct TALER_DenominationKeyValidityPS properties;

    {/* properties */
      struct TALER_Amount amount;

      properties.purpose.purpose = htonl (TALER_SIGNATURE_MASTER_SIGNING_KEY_VALIDITY);
      properties.purpose.size = htonl (sizeof (struct TALER_DenominationKeyValidityPS));
      GNUNET_CRYPTO_eddsa_key_get_public (master_prvt,
                                          &properties.master.eddsa_pub);
      properties.start = GNUNET_TIME_absolute_hton (GNUNET_TIME_absolute_get());
      properties.expire_withdraw = GNUNET_TIME_absolute_hton (GNUNET_TIME_absolute_get_forever_());
      properties.expire_spend = GNUNET_TIME_absolute_hton (GNUNET_TIME_absolute_get_forever_());
      properties.expire_legal = GNUNET_TIME_absolute_hton (GNUNET_TIME_absolute_get_forever_());
      TALER_string_to_amount (CURRENCY ":1.1", &amount);
      TALER_amount_hton (&properties.value, &amount);
      TALER_string_to_amount (CURRENCY ":0.1", &amount);
      TALER_amount_hton (&properties.fee_withdraw, &amount);
      TALER_amount_hton (&properties.fee_deposit, &amount);
      TALER_amount_hton (&properties.fee_refresh, &amount);
      GNUNET_CRYPTO_rsa_public_key_hash (denom_pub.rsa_public_key,
                                         &properties.denom_hash);
      issue.properties = properties;
    }
    {/* signature */
      GNUNET_CRYPTO_eddsa_sign (master_prvt,
                                &properties.purpose,
                                &signature.eddsa_signature);
      issue.signature = signature;
    }
  }
  dki->denom_priv = denom_priv;
  dki->denom_pub = denom_pub;
  dki->issue = issue;
  GNUNET_free (master_prvt);
  return dki;
}


/**
 * Copies the given denomination
 * @param reserve the deposit copy
 * @return a copy of @a deposit; NULL if error
 */
struct TALER_MINTDB_DenominationKeyIssueInformation *
PERF_TALER_MINTDB_denomination_copy (const struct TALER_MINTDB_DenominationKeyIssueInformation *dki)
{
  struct TALER_MINTDB_DenominationKeyIssueInformation *copy;

  GNUNET_assert (NULL !=
                 (copy = GNUNET_new (struct TALER_MINTDB_DenominationKeyIssueInformation)));
  {/* denom_priv */
    copy->denom_priv.rsa_private_key =
      GNUNET_CRYPTO_rsa_private_key_dup ( dki->denom_priv.rsa_private_key);
  }
  {/* denom_pub */
    copy->denom_pub.rsa_public_key = 
      GNUNET_CRYPTO_rsa_public_key_dup (dki->denom_pub.rsa_public_key);
  }
  {/* issue */
    copy->issue.properties = dki->issue.properties;
    copy->issue.signature = dki->issue.signature;
  }
  return copy;
}


/**
 * Free memory of a DenominationKeyIssueInformation
 * @param dki pointer to the struct to free
 */
int
PERF_TALER_MINTDB_denomination_free (struct TALER_MINTDB_DenominationKeyIssueInformation *dki)
{
  if (NULL ==dki)
    return GNUNET_OK;
  GNUNET_CRYPTO_rsa_private_key_free (dki->denom_priv.rsa_private_key);
  GNUNET_CRYPTO_rsa_public_key_free (dki->denom_pub.rsa_public_key);

  return GNUNET_OK;
}


/**
 * Generate a dummy reserve for testing
 * @return a reserve with 1000 EUR in it
 */
struct PERF_TALER_MINTDB_Reserve *
PERF_TALER_MINTDB_reserve_init ()
{
  struct PERF_TALER_MINTDB_Reserve *reserve;

  GNUNET_assert (NULL !=
                 (reserve = GNUNET_new (struct PERF_TALER_MINTDB_Reserve)));
  {/* private */
    struct GNUNET_CRYPTO_EddsaPrivateKey *private;
    GNUNET_assert (NULL !=
                   (private = GNUNET_CRYPTO_eddsa_key_create ()));
    reserve->private = *private;
  }

  GNUNET_CRYPTO_eddsa_key_get_public (&reserve->private,
                                      &reserve->reserve.pub.eddsa_pub);
  GNUNET_assert (GNUNET_OK ==
                 TALER_string_to_amount (CURRENCY ":1000", &reserve->reserve.balance));
  reserve->reserve.expiry = GNUNET_TIME_absolute_get_forever_ ();
  return reserve;
}


/**
 * Copies the given reserve
 * @param reserve the reserve to copy
 * @return a copy of @a reserve; NULL if error
 */
struct PERF_TALER_MINTDB_Reserve *
PERF_TALER_MINTDB_reserve_copy (const struct PERF_TALER_MINTDB_Reserve *reserve)
{
  struct PERF_TALER_MINTDB_Reserve *copy;
  GNUNET_assert (NULL != 
                 (copy = GNUNET_new (struct PERF_TALER_MINTDB_Reserve)));
  *copy = *reserve;
  return copy;
} 


/**
 * Free memory of a reserve
 * @param reserve pointer to the structure to be freed
 */
int
PERF_TALER_MINTDB_reserve_free (struct PERF_TALER_MINTDB_Reserve *reserve)
{
  if (NULL == reserve)
    return GNUNET_OK;
  return GNUNET_OK;
}


/**
 * Generate a dummy deposit for testing purposes
 * @param dki the denomination key used to sign the key
 */
struct TALER_MINTDB_Deposit *
PERF_TALER_MINTDB_deposit_init (const struct PERF_TALER_MINTDB_Coin *coin)
{
  struct TALER_MINTDB_Deposit *deposit;
  struct TALER_CoinSpendSignatureP csig;
  struct TALER_MerchantPublicKeyP merchant_pub;
  struct GNUNET_HashCode h_contract;
  struct GNUNET_HashCode h_wire;
  const char wire[] = "{"
    "\"type\":\"SEPA\","
    "\"IBAN\":\"DE67830654080004822650\","
    "\"NAME\":\"GNUNET E.\","
    "\"BIC\":\"GENODEF1SRL\""
    "}";
  static uint64_t transaction_id = 0;
  struct GNUNET_TIME_Absolute timestamp;  
  struct GNUNET_TIME_Absolute refund_deadline;  
  struct TALER_Amount amount_with_fee;
  struct TALER_Amount deposit_fee;

  GNUNET_assert (NULL !=
                 (deposit = GNUNET_malloc (sizeof (struct TALER_MINTDB_Deposit) + sizeof (wire))));
  GNUNET_CRYPTO_hash_create_random (GNUNET_CRYPTO_QUALITY_WEAK,
                                    &h_contract);
  GNUNET_CRYPTO_hash_create_random (GNUNET_CRYPTO_QUALITY_WEAK,
                                    &h_wire);
  { //csig
    struct u32_presign
    {
      struct GNUNET_CRYPTO_EccSignaturePurpose purpose;
      struct GNUNET_HashCode h_wire;
      struct GNUNET_HashCode h_contract;
    } unsigned_data;

    unsigned_data.h_contract = h_contract;
    unsigned_data.h_wire = h_wire;
    unsigned_data.purpose.size = htonl (sizeof (struct u32_presign));
    unsigned_data.purpose.purpose = htonl (GNUNET_SIGNATURE_PURPOSE_TEST);
    GNUNET_assert (GNUNET_OK ==
                   GNUNET_CRYPTO_eddsa_sign (&coin->priv,
                                             &unsigned_data.purpose,
                                             &csig.eddsa_signature));
  }
  { //merchant_pub
    struct GNUNET_CRYPTO_EddsaPrivateKey *eddsa_prv;

    eddsa_prv = GNUNET_CRYPTO_eddsa_key_create ();
    GNUNET_assert(NULL != eddsa_prv);
    GNUNET_CRYPTO_eddsa_key_get_public (
      eddsa_prv,
      &merchant_pub.eddsa_pub);
    GNUNET_free (eddsa_prv);
  }
  timestamp = GNUNET_TIME_absolute_get ();
  refund_deadline = GNUNET_TIME_absolute_get ();
  GNUNET_assert (GNUNET_OK ==
                 TALER_string_to_amount (CURRENCY ":1.1", 
                                         &amount_with_fee));
  GNUNET_assert (GNUNET_OK ==
                 TALER_string_to_amount (CURRENCY ":0.1", 
                                         &deposit_fee));
  {
    deposit->coin.coin_pub = coin->public_info.coin_pub;
    deposit->coin.denom_pub.rsa_public_key = GNUNET_CRYPTO_rsa_public_key_dup (
      coin->public_info.denom_pub.rsa_public_key);
    GNUNET_assert (NULL != coin->public_info.denom_pub.rsa_public_key);
    deposit->coin.denom_sig.rsa_signature = GNUNET_CRYPTO_rsa_signature_dup (
      coin->public_info.denom_sig.rsa_signature);
    GNUNET_assert (NULL != coin->public_info.denom_sig.rsa_signature);
  }
  deposit->csig = csig;
  deposit->h_contract = h_contract;
  deposit->h_wire = h_wire;
  deposit->wire = json_loads (wire, 0, NULL);
  deposit->transaction_id = transaction_id++;
  deposit->timestamp = timestamp;
  deposit->refund_deadline = refund_deadline;
  deposit->amount_with_fee = amount_with_fee;
  deposit->deposit_fee = deposit_fee;
  return deposit;
}


/**
 * Copies the given deposit
 * @param reserve the deposit copy
 * @return a copy of @a deposit; NULL if error
 */
struct TALER_MINTDB_Deposit *
PERF_TALER_MINTDB_deposit_copy (const struct TALER_MINTDB_Deposit *deposit)
{
  struct TALER_MINTDB_Deposit *copy;

  GNUNET_assert (NULL != (copy = GNUNET_new (struct TALER_MINTDB_Deposit)));
  *copy = *deposit;
  json_incref (copy->wire);
  copy->coin.denom_pub.rsa_public_key = 
    GNUNET_CRYPTO_rsa_public_key_dup (deposit->coin.denom_pub.rsa_public_key);
  copy->coin.denom_sig.rsa_signature =
    GNUNET_CRYPTO_rsa_signature_dup (deposit->coin.denom_sig.rsa_signature);

  return copy;
}


/**
 * Free memory of a deposit
 * @param deposit pointer to the structure to free
 */
int
PERF_TALER_MINTDB_deposit_free (struct TALER_MINTDB_Deposit *deposit)
{
  if ( NULL == deposit)
    return GNUNET_OK;
  GNUNET_CRYPTO_rsa_public_key_free (deposit->coin.denom_pub.rsa_public_key);
  GNUNET_CRYPTO_rsa_signature_free (deposit->coin.denom_sig.rsa_signature);
  json_decref (deposit->wire);

  return GNUNET_OK;
}


/**
 * Generate a CollectableBlindcoin for testing purpuses
 * @param dki denomination key used to sign the coin
 * @param reserve reserve providing the money for the coin
 * @return a randomly generated CollectableBlindcoin
 */
struct PERF_TALER_MINTDB_Coin *
PERF_TALER_MINTDB_coin_init (
  const struct TALER_MINTDB_DenominationKeyIssueInformation *dki,
  const struct PERF_TALER_MINTDB_Reserve *reserve)
{
  struct PERF_TALER_MINTDB_Coin *coin;
  struct GNUNET_CRYPTO_EddsaPrivateKey *priv;

  coin = GNUNET_new (struct PERF_TALER_MINTDB_Coin);
  GNUNET_assert (NULL != coin);
  /* priv */

  priv = GNUNET_CRYPTO_eddsa_key_create();
  GNUNET_assert (NULL != priv);
  coin->priv = *priv;
  GNUNET_free (priv);

  /* public_info */
  GNUNET_CRYPTO_eddsa_key_get_public (&coin->priv,
                                      &coin->public_info.coin_pub.eddsa_pub);
  GNUNET_assert (NULL !=
                 (coin->public_info.denom_pub.rsa_public_key = 
                  GNUNET_CRYPTO_rsa_public_key_dup (dki->denom_pub.rsa_public_key)));
  GNUNET_assert (NULL !=
                 (coin->public_info.denom_sig.rsa_signature = 
                  GNUNET_CRYPTO_rsa_sign (dki->denom_priv.rsa_private_key,
                                          &coin->public_info.coin_pub,
                                          sizeof (struct TALER_CoinSpendPublicKeyP))));

  /* blind */
  GNUNET_assert (NULL !=
                 (coin->blind.sig.rsa_signature = 
                  GNUNET_CRYPTO_rsa_signature_dup (coin->public_info.denom_sig.rsa_signature)));
  GNUNET_assert (NULL !=
                 (coin->blind.denom_pub.rsa_public_key = 
                  GNUNET_CRYPTO_rsa_public_key_dup (dki->denom_pub.rsa_public_key)));
  TALER_amount_ntoh (&coin->blind.amount_with_fee,
                     &dki->issue.properties.value);
  TALER_amount_ntoh (&coin->blind.withdraw_fee,
                     &dki->issue.properties.fee_withdraw);
  coin->blind.reserve_pub = reserve->reserve.pub;
  GNUNET_CRYPTO_hash_create_random (GNUNET_CRYPTO_QUALITY_WEAK,
                                    &coin->blind.h_coin_envelope);
  {
    struct {
      struct TALER_ReservePublicKeyP reserve_pub;
      struct GNUNET_HashCode hash; 
    } data;

    data.reserve_pub = reserve->reserve.pub;
    data.hash = coin->blind.h_coin_envelope;
    GNUNET_assert (NULL !=
                   (coin->blind.sig.rsa_signature =
                    GNUNET_CRYPTO_rsa_sign (dki->denom_priv.rsa_private_key,
                                            &data,
                                            sizeof (data))));
  }
  return coin;
}


/**
 * Copies the given coin
 * @param coin the coin to copy
 * @return a copy of coin; NULL if error
 */
struct PERF_TALER_MINTDB_Coin *
PERF_TALER_MINTDB_coin_copy (const struct PERF_TALER_MINTDB_Coin *coin)
{
  struct PERF_TALER_MINTDB_Coin *copy;

  GNUNET_assert (NULL != 
                 (copy = GNUNET_new (struct PERF_TALER_MINTDB_Coin)));
  /* priv */
  copy->priv = coin->priv;
  /* public_info */
  copy->public_info.coin_pub = coin->public_info.coin_pub;
  copy->public_info.denom_pub.rsa_public_key = 
   GNUNET_CRYPTO_rsa_public_key_dup (coin->public_info.denom_pub.rsa_public_key);
  GNUNET_assert (NULL != copy->public_info.denom_pub.rsa_public_key);
  copy->public_info.denom_sig.rsa_signature = 
    GNUNET_CRYPTO_rsa_signature_dup (coin->public_info.denom_sig.rsa_signature);
  GNUNET_assert (NULL != coin->public_info.denom_sig.rsa_signature);

  /* blind */
  copy->blind.sig.rsa_signature = 
   GNUNET_CRYPTO_rsa_signature_dup (coin->blind.sig.rsa_signature);
  GNUNET_assert (NULL != copy->blind.sig.rsa_signature);
  copy->blind.denom_pub.rsa_public_key =
   GNUNET_CRYPTO_rsa_public_key_dup (coin->blind.denom_pub.rsa_public_key); 
  GNUNET_assert (NULL != copy->blind.denom_pub.rsa_public_key);
  copy->blind.amount_with_fee = coin->blind.amount_with_fee;
  copy->blind.withdraw_fee = coin->blind.withdraw_fee;
  copy->blind.reserve_pub = coin->blind.reserve_pub;
  copy->blind.h_coin_envelope = coin->blind.h_coin_envelope;
  copy->blind.reserve_sig = coin->blind.reserve_sig;

  return copy;
}


/**
 * Liberate memory of @a coin
 * @param coin pointer to the structure to free
 */
int
PERF_TALER_MINTDB_coin_free (struct PERF_TALER_MINTDB_Coin *coin)
{
  if (NULL == coin)
    return GNUNET_OK;
  GNUNET_CRYPTO_rsa_public_key_free (coin->public_info.denom_pub.rsa_public_key);
  GNUNET_CRYPTO_rsa_signature_free (coin->public_info.denom_sig.rsa_signature);
  GNUNET_CRYPTO_rsa_signature_free (coin->blind.sig.rsa_signature);
  GNUNET_CRYPTO_rsa_public_key_free (coin->blind.denom_pub.rsa_public_key);
  return GNUNET_OK;
}


/**
 * @return a randomly generated refresh session
 */
struct TALER_MINTDB_RefreshSession *
PERF_TALER_MINTDB_refresh_session_init ()
{
  struct TALER_MINTDB_RefreshSession *refresh_session;

  GNUNET_assert (NULL !=
                 (refresh_session = GNUNET_new (struct TALER_MINTDB_RefreshSession)));
  refresh_session->noreveal_index = 1;
  refresh_session->num_oldcoins = 1;
  refresh_session->num_newcoins = 1;

  return refresh_session;
}


/**
 * @return #GNUNET_OK if the copy was successful, #GNUNET_SYSERR if it wasn't
 */
int
PERF_TALER_MINTDB_refresh_session_copy (struct TALER_MINTDB_RefreshSession *session, 
                                        struct TALER_MINTDB_RefreshSession *copy)
{
  *copy = *session;
  return GNUNET_OK;
}


/**
 * Free a refresh session
 */
int
PERF_TALER_MINTDB_refresh_session_free (struct TALER_MINTDB_RefreshSession *refresh_session)
{
  if (NULL == refresh_session)
    return GNUNET_OK;
  return GNUNET_OK;
}


/**
 * Create a melt operation
 *
 * @param session the refresh session 
 * @param dki the denomination the melted coin uses
 * @return a pointer to a #TALER_MINTDB_RefreshMelt 
 */
struct TALER_MINTDB_RefreshMelt *
PERF_TALER_MINTDB_refresh_melt_init (struct GNUNET_HashCode *session,
                                     struct PERF_TALER_MINTDB_Coin *coin)
{
  struct TALER_MINTDB_RefreshMelt *melt;
  struct TALER_CoinSpendSignatureP coin_sig;
  struct TALER_Amount amount;
  struct TALER_Amount amount_with_fee; 

  {
    struct 
    {
      struct GNUNET_CRYPTO_EccSignaturePurpose purpose;
      struct GNUNET_HashCode session;
    } to_sign;
    
    to_sign.purpose.purpose = GNUNET_SIGNATURE_PURPOSE_TEST; 
    to_sign.purpose.size = htonl (sizeof (to_sign));
    to_sign.session = *session; 
    GNUNET_CRYPTO_eddsa_sign (&coin->priv,
                              &to_sign.purpose,
                              &coin_sig.eddsa_signature);
  }
  GNUNET_assert (GNUNET_OK == TALER_string_to_amount (CURRENCY ":1.1",
                                                      &amount));
  GNUNET_assert (GNUNET_OK == TALER_string_to_amount (CURRENCY ":0.1",
                                                      &amount_with_fee));
  melt = GNUNET_new (struct TALER_MINTDB_RefreshMelt); 
  melt->coin.coin_pub = coin->public_info.coin_pub;
  melt->coin.denom_sig.rsa_signature = 
    GNUNET_CRYPTO_rsa_signature_dup (coin->public_info.denom_sig.rsa_signature);
  melt->coin.denom_pub.rsa_public_key =
    GNUNET_CRYPTO_rsa_public_key_dup (coin->public_info.denom_pub.rsa_public_key);
  GNUNET_assert (NULL != melt->coin.denom_pub.rsa_public_key);
  GNUNET_assert (NULL != melt->coin.denom_sig.rsa_signature);
  melt->coin_sig = coin_sig;
  melt->session_hash = *session;
  melt->amount_with_fee = amount;
  melt->melt_fee = amount_with_fee;
  return melt;
}


/**
 * Copies the internals of a #TALER_MINTDB_RefreshMelt
 * 
 * @param melt the refresh melt to copy
 * @return an copy of @ melt
 */
struct TALER_MINTDB_RefreshMelt *
PERF_TALER_MINTDB_refresh_melt_copy (const struct TALER_MINTDB_RefreshMelt *melt)
{
  struct TALER_MINTDB_RefreshMelt *copy;

  copy = GNUNET_new (struct TALER_MINTDB_RefreshMelt);
  *copy = *melt;
  GNUNET_assert (NULL != 
                 (copy->coin.denom_sig.rsa_signature = 
                  GNUNET_CRYPTO_rsa_signature_dup (melt->coin.denom_sig.rsa_signature)));

  return copy;
}


/**
 * Free the internal memory of a #TALER_MINTDB_RefreshMelt
 *
 * @param melt the #TALER_MINTDB_RefreshMelt to free
 * @return #GNUNET_OK if the operation was successful, #GNUNET_SYSERROR
 */
int
PERF_TALER_MINTDB_refresh_melt_free (struct TALER_MINTDB_RefreshMelt *melt)
{
  GNUNET_CRYPTO_rsa_signature_free (melt->coin.denom_sig.rsa_signature);
  return GNUNET_OK;
}
