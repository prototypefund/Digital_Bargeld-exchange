/*
  This file is part of TALER
  Copyright (C) 2014, 2015, 2016, 2017 GNUnet e.V.

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
 * @file exchangedb/test_exchangedb.c
 * @brief test cases for DB interaction functions
 * @author Sree Harsha Totakura
 * @author Christian Grothoff
 * @author Marcello Stanisci
 */
#include "platform.h"
#include "taler_exchangedb_lib.h"
#include "taler_json_lib.h"
#include "taler_exchangedb_plugin.h"

/**
 * Global result from the testcase.
 */
static int result;

/**
 * Report line of error if @a cond is true, and jump to label "drop".
 */
#define FAILIF(cond)                              \
  do {                                          \
    if (!(cond)){ break;}                      \
    GNUNET_break (0);                           \
    goto drop;                                  \
  } while (0)


/**
 * Initializes @a ptr with random data.
 */
#define RND_BLK(ptr)                                                    \
  GNUNET_CRYPTO_random_block (GNUNET_CRYPTO_QUALITY_WEAK, ptr, sizeof (*ptr))

/**
 * Initializes @a ptr with zeros.
 */
#define ZR_BLK(ptr) \
  memset (ptr, 0, sizeof (*ptr))


/**
 * Currency we use.
 */
#define CURRENCY "EUR"

/**
 * Database plugin under test.
 */
static struct TALER_EXCHANGEDB_Plugin *plugin;


/**
 * Callback that should never be called.
 */
static void
dead_prepare_cb (void *cls,
                 uint64_t rowid,
                 const char *wire_method,
                 const char *buf,
                 size_t buf_size)
{
  GNUNET_assert (0);
}


/**
 * Callback that is called with wire prepare data
 * and then marks it as finished.
 */
static void
mark_prepare_cb (void *cls,
                 uint64_t rowid,
                 const char *wire_method,
                 const char *buf,
                 size_t buf_size)
{
  struct TALER_EXCHANGEDB_Session *session = cls;

  GNUNET_assert (11 == buf_size);
  GNUNET_assert (0 == strcasecmp (wire_method,
                                  "testcase"));
  GNUNET_assert (0 == memcmp (buf,
                              "hello world",
                              buf_size));
  GNUNET_break (GNUNET_OK ==
                plugin->wire_prepare_data_mark_finished (plugin->cls,
                                                         session,
                                                         rowid));
}


/**
 * Test API relating to persisting the wire plugins preparation data.
 *
 * @param session database session to use for the test
 * @return #GNUNET_OK on success
 */
static int
test_wire_prepare (struct TALER_EXCHANGEDB_Session *session)
{
  FAILIF (GNUNET_NO !=
          plugin->wire_prepare_data_get (plugin->cls,
                                         session,
                                         &dead_prepare_cb,
                                         NULL));
  FAILIF (GNUNET_OK !=
          plugin->wire_prepare_data_insert (plugin->cls,
                                            session,
                                            "testcase",
                                            "hello world",
                                            11));
  FAILIF (GNUNET_OK !=
          plugin->wire_prepare_data_get (plugin->cls,
                                         session,
                                         &mark_prepare_cb,
                                         session));
  FAILIF (GNUNET_NO !=
          plugin->wire_prepare_data_get (plugin->cls,
                                         session,
                                         &dead_prepare_cb,
                                         NULL));
  return GNUNET_OK;
 drop:
  return GNUNET_SYSERR;
}


/**
 * Checks if the given reserve has the given amount of balance and expiry
 *
 * @param session the database connection
 * @param pub the public key of the reserve
 * @param value balance value
 * @param fraction balance fraction
 * @param currency currency of the reserve
 * @return #GNUNET_OK if the given reserve has the same balance and expiration
 *           as the given parameters; #GNUNET_SYSERR if not
 */
static int
check_reserve (struct TALER_EXCHANGEDB_Session *session,
               const struct TALER_ReservePublicKeyP *pub,
               uint64_t value,
               uint32_t fraction,
               const char *currency)
{
  struct TALER_EXCHANGEDB_Reserve reserve;

  reserve.pub = *pub;

  FAILIF (GNUNET_OK !=
          plugin->reserve_get (plugin->cls,
                               session,
                               &reserve));
  FAILIF (value != reserve.balance.value);
  FAILIF (fraction != reserve.balance.fraction);
  FAILIF (0 != strcmp (currency, reserve.balance.currency));

  return GNUNET_OK;
 drop:
  return GNUNET_SYSERR;
}


struct DenomKeyPair
{
  struct TALER_DenominationPrivateKey priv;
  struct TALER_DenominationPublicKey pub;
};


/**
 * Destroy a denomination key pair.  The key is not necessarily removed from the DB.
 *
 * @param dkp the keypair to destroy
 */
static void
destroy_denom_key_pair (struct DenomKeyPair *dkp)
{
  GNUNET_CRYPTO_rsa_public_key_free (dkp->pub.rsa_public_key);
  GNUNET_CRYPTO_rsa_private_key_free (dkp->priv.rsa_private_key);
  GNUNET_free (dkp);
}


/**
 * Create a denominaiton key pair by registering the denomination in the DB.
 *
 * @param size the size of the denomination key
 * @param session the DB session
 * @param now time to use for key generation, legal expiration will be 3h later.
 * @param fee_withdraw withdraw fee to use
 * @param fee_deposit deposit fee to use
 * @param fee_refresh refresh fee to use
 * @param fee_refund refund fee to use
 * @return the denominaiton key pair; NULL upon error
 */
static struct DenomKeyPair *
create_denom_key_pair (unsigned int size,
                       struct TALER_EXCHANGEDB_Session *session,
                       struct GNUNET_TIME_Absolute now,
                       const struct TALER_Amount *value,
                       const struct TALER_Amount *fee_withdraw,
                       const struct TALER_Amount *fee_deposit,
                       const struct TALER_Amount *fee_refresh,
                       const struct TALER_Amount *fee_refund)
{
  struct DenomKeyPair *dkp;
  struct TALER_EXCHANGEDB_DenominationKeyIssueInformation dki;
  struct TALER_EXCHANGEDB_DenominationKeyInformationP issue2;

  dkp = GNUNET_new (struct DenomKeyPair);
  dkp->priv.rsa_private_key = GNUNET_CRYPTO_rsa_private_key_create (size);
  GNUNET_assert (NULL != dkp->priv.rsa_private_key);
  dkp->pub.rsa_public_key
    = GNUNET_CRYPTO_rsa_private_key_get_public (dkp->priv.rsa_private_key);

  /* Using memset() as fields like master key and signature
     are not properly initialized for this test. */
  memset (&dki,
          0,
          sizeof (struct TALER_EXCHANGEDB_DenominationKeyIssueInformation));
  dki.denom_pub = dkp->pub;
  GNUNET_TIME_round_abs (&now);
  dki.issue.properties.start = GNUNET_TIME_absolute_hton (now);
  dki.issue.properties.expire_withdraw = GNUNET_TIME_absolute_hton
    (GNUNET_TIME_absolute_add (now,
                               GNUNET_TIME_UNIT_HOURS));
  dki.issue.properties.expire_deposit = GNUNET_TIME_absolute_hton
      (GNUNET_TIME_absolute_add
       (now,
        GNUNET_TIME_relative_multiply (GNUNET_TIME_UNIT_HOURS, 2)));
  dki.issue.properties.expire_legal = GNUNET_TIME_absolute_hton
      (GNUNET_TIME_absolute_add
       (now,
        GNUNET_TIME_relative_multiply (GNUNET_TIME_UNIT_HOURS, 3)));
  TALER_amount_hton (&dki.issue.properties.value, value);
  TALER_amount_hton (&dki.issue.properties.fee_withdraw, fee_withdraw);
  TALER_amount_hton (&dki.issue.properties.fee_deposit, fee_deposit);
  TALER_amount_hton (&dki.issue.properties.fee_refresh, fee_refresh);
  TALER_amount_hton (&dki.issue.properties.fee_refund, fee_refund);
  GNUNET_CRYPTO_rsa_public_key_hash (dkp->pub.rsa_public_key,
                                     &dki.issue.properties.denom_hash);

  dki.issue.properties.purpose.size = htonl (sizeof (struct TALER_DenominationKeyValidityPS));
  dki.issue.properties.purpose.purpose = htonl (TALER_SIGNATURE_MASTER_DENOMINATION_KEY_VALIDITY);
  if (GNUNET_OK !=
      plugin->insert_denomination_info (plugin->cls,
                                        session,
                                        &dki.denom_pub,
                                        &dki.issue))
  {
    GNUNET_break(0);
    destroy_denom_key_pair (dkp);
    return NULL;
  }
  if (GNUNET_OK !=
      plugin->get_denomination_info (plugin->cls,
                                     session,
                                     &dki.denom_pub,
                                     &issue2))
  {
    GNUNET_break(0);
    destroy_denom_key_pair (dkp);
    return NULL;
  }
  if (0 != memcmp (&dki.issue,
                   &issue2,
                   sizeof (issue2)))
  {
    GNUNET_break(0);
    destroy_denom_key_pair (dkp);
    return NULL;
  }
  return dkp;
}

static struct TALER_Amount value;
static struct TALER_Amount fee_withdraw;
static struct TALER_Amount fee_deposit;
static struct TALER_Amount fee_refresh;
static struct TALER_Amount fee_refund;
static struct TALER_Amount amount_with_fee;


/**
 * Number of newly minted coins to use in the test.
 */
#define MELT_NEW_COINS 5

/**
 * Which index was 'randomly' chosen for the reveal for the test?
 */
#define MELT_NOREVEAL_INDEX 1


static struct TALER_EXCHANGEDB_RefreshCommitCoin *commit_coins;

/**
 * Test APIs related to the "insert_refresh_commit_coins" function.
 *
 * @param session database sesison to use
 * @param refresh_session details about the refresh session to use
 * @param session_hash refresh melt session hash to use
 * @return #GNUNET_OK on success
 */
static int
test_refresh_commit_coins (struct TALER_EXCHANGEDB_Session *session,
                           const struct TALER_EXCHANGEDB_RefreshSession *refresh_session,
                           const struct GNUNET_HashCode *session_hash)
{
  struct TALER_EXCHANGEDB_RefreshCommitCoin *ret_commit_coins;
  struct TALER_EXCHANGEDB_RefreshCommitCoin *a_ccoin;
  struct TALER_EXCHANGEDB_RefreshCommitCoin *b_ccoin;
  unsigned int cnt;
  int ret;

#define COIN_ENC_MAX_SIZE 512
  ret = GNUNET_SYSERR;
  ret_commit_coins = NULL;
  commit_coins
    = GNUNET_new_array (MELT_NEW_COINS,
                        struct TALER_EXCHANGEDB_RefreshCommitCoin);
  for (cnt=0; cnt < MELT_NEW_COINS; cnt++)
  {
    struct TALER_EXCHANGEDB_RefreshCommitCoin *ccoin;

    ccoin = &commit_coins[cnt];
    ccoin->coin_ev_size = GNUNET_CRYPTO_random_u64
      (GNUNET_CRYPTO_QUALITY_WEAK, COIN_ENC_MAX_SIZE);
    ccoin->coin_ev = GNUNET_malloc (ccoin->coin_ev_size);
    GNUNET_CRYPTO_random_block (GNUNET_CRYPTO_QUALITY_WEAK,
                                ccoin->coin_ev,
                                ccoin->coin_ev_size);
  }
  FAILIF (GNUNET_OK !=
          plugin->insert_refresh_commit_coins (plugin->cls,
                                               session,
                                               session_hash,
                                               MELT_NEW_COINS,
                                               commit_coins));
  ret_commit_coins = GNUNET_new_array (MELT_NEW_COINS,
                                       struct TALER_EXCHANGEDB_RefreshCommitCoin);
  FAILIF (GNUNET_OK !=
          plugin->get_refresh_commit_coins (plugin->cls,
                                            session,
                                            session_hash,
                                            MELT_NEW_COINS,
                                            ret_commit_coins));
  /* compare the refresh commit coin arrays */
  for (cnt = 0; cnt < MELT_NEW_COINS; cnt++)
  {
    a_ccoin = &commit_coins[cnt];
    b_ccoin = &ret_commit_coins[cnt];
    FAILIF (a_ccoin->coin_ev_size != b_ccoin->coin_ev_size);
    FAILIF (0 != memcmp (a_ccoin->coin_ev,
                         a_ccoin->coin_ev,
                         a_ccoin->coin_ev_size));
    GNUNET_free (ret_commit_coins[cnt].coin_ev);
  }
  GNUNET_free (ret_commit_coins);
  ret_commit_coins = NULL;
  ret = GNUNET_OK;
 drop:
  if (NULL != ret_commit_coins)
  {
    plugin->free_refresh_commit_coins (plugin->cls,
                                       MELT_NEW_COINS,
                                       ret_commit_coins);
    GNUNET_free (ret_commit_coins);
  }
  return ret;
}


static struct TALER_TransferPublicKeyP rctp[TALER_CNC_KAPPA];


/**
 * Test APIs related to the "insert_refresh_commit_coins" function.
 *
 * @param session database sesison to use
 * @param refresh_session details about the refresh session to use
 * @param session_hash refresh melt session hash to use
 * @return #GNUNET_OK on success
 */
static int
test_refresh_commit_links (struct TALER_EXCHANGEDB_Session *session,
                           const struct TALER_EXCHANGEDB_RefreshSession *refresh_session,
                           const struct GNUNET_HashCode *session_hash)
{
  int ret;
  struct TALER_TransferPublicKeyP tp;
  unsigned int i;

  ret = GNUNET_SYSERR;
  FAILIF (GNUNET_NO !=
          plugin->get_refresh_transfer_public_key (plugin->cls,
                                                   session,
                                                   session_hash,
                                                   &tp));
  for (i=0;i<TALER_CNC_KAPPA;i++)
    RND_BLK (&rctp[i]);
  FAILIF (GNUNET_OK !=
          plugin->insert_refresh_transfer_public_key (plugin->cls,
                                                      session,
                                                      session_hash,
                                                      &rctp[MELT_NOREVEAL_INDEX]));
  FAILIF (GNUNET_OK !=
          plugin->get_refresh_transfer_public_key (plugin->cls,
                                                   session,
                                                   session_hash,
                                                   &tp));
  FAILIF (0 !=
          memcmp (&rctp[MELT_NOREVEAL_INDEX],
                  &tp,
                  sizeof (struct TALER_TransferPublicKeyP)));
  ret = GNUNET_OK;
 drop:
  return ret;
}


static struct GNUNET_HashCode session_hash;


/**
 * Function called with the session hashes and transfer secret
 * information for a given coin.  Checks if they are as expected.
 *
 * @param cls closure
 * @param sh a session the coin was melted in
 * @param transfer_pub public transfer key for the session
 */
static void
check_transfer_data (void *cls,
                     const struct GNUNET_HashCode *sh,
                     const struct TALER_TransferPublicKeyP *transfer_pub)
{
  int *ok = cls;

  FAILIF (0 != memcmp (&rctp[MELT_NOREVEAL_INDEX],
                       transfer_pub,
                       sizeof (struct TALER_TransferPublicKeyP)));
  FAILIF (0 != memcmp (&session_hash,
                       sh,
                       sizeof (struct GNUNET_HashCode)));
  *ok = GNUNET_OK;
  return;
 drop:
  *ok = GNUNET_SYSERR;
}


/**
 * Counter used in auditor-related db functions. Used to count
 * expected rows.
 */
static unsigned int auditor_row_cnt;


/**
 * Function called with details about coins that were melted,
 * with the goal of auditing the refresh's execution.
 *
 *
 * @param cls closure
 * @param rowid unique serial ID for the refresh session in our DB
 * @param coin_pub public key of the coin
 * @param coin_sig signature from the coin
 * @param amount_with_fee amount that was deposited including fee
 * @param num_newcoins how many coins were issued
 * @param noreveal_index which index was picked by the exchange in cut-and-choose
 * @param session_hash what is the session hash
 * @return #GNUNET_OK to continue to iterate, #GNUNET_SYSERR to stop
 */
static int
audit_refresh_session_cb (void *cls,
                          uint64_t rowid,
                          const struct TALER_CoinSpendPublicKeyP *coin_pub,
                          const struct TALER_CoinSpendSignatureP *coin_sig,
                          const struct TALER_Amount *amount_with_fee,
                          uint16_t num_newcoins,
                          uint16_t noreveal_index,
                          const struct GNUNET_HashCode *session_hash)
{
  auditor_row_cnt++;
  return GNUNET_OK;
}


/**
 * Function to test melting of coins as part of a refresh session
 *
 * @param session the database session
 * @param refresh_session the refresh session
 * @return #GNUNET_OK if everything went well; #GNUNET_SYSERR if not
 */
static int
test_melting (struct TALER_EXCHANGEDB_Session *session)
{
  struct TALER_EXCHANGEDB_RefreshSession refresh_session;
  struct TALER_EXCHANGEDB_RefreshSession ret_refresh_session;
  struct DenomKeyPair *dkp;
  struct DenomKeyPair **new_dkp;
  /* struct TALER_CoinPublicInfo *coins; */
  struct TALER_EXCHANGEDB_RefreshMelt *meltp;
  struct TALER_DenominationPublicKey *new_denom_pubs;
  struct TALER_DenominationPublicKey *ret_denom_pubs;
  struct TALER_EXCHANGEDB_LinkDataList *ldl;
  struct TALER_EXCHANGEDB_LinkDataList *ldlp;
  struct TALER_DenominationSignature ev_sigs[MELT_NEW_COINS];
  unsigned int cnt;
  int ret;

  ret = GNUNET_SYSERR;
  memset (ev_sigs, 0, sizeof (ev_sigs));
  RND_BLK (&refresh_session);
  RND_BLK (&session_hash);
  dkp = NULL;
  new_dkp = NULL;
  new_denom_pubs = NULL;
  ret_denom_pubs = NULL;
  /* create and test a refresh session */
  refresh_session.num_newcoins = MELT_NEW_COINS;
  refresh_session.noreveal_index = MELT_NOREVEAL_INDEX;
  /* create a denomination (value: 1; fraction: 100) */
  dkp = create_denom_key_pair (512,
                               session,
                               GNUNET_TIME_absolute_get (),
                               &value,
                               &fee_withdraw,
                               &fee_deposit,
                               &fee_refresh,
			       &fee_refund);
  /* initialize refresh session melt data */
  {
    struct GNUNET_HashCode hc;

    meltp = &refresh_session.melt;
    RND_BLK (&meltp->coin.coin_pub);
    GNUNET_CRYPTO_hash (&meltp->coin.coin_pub,
                        sizeof (meltp->coin.coin_pub),
                        &hc);
    meltp->coin.denom_sig.rsa_signature =
        GNUNET_CRYPTO_rsa_sign_fdh (dkp->priv.rsa_private_key,
                                    &hc);
    GNUNET_assert (NULL != meltp->coin.denom_sig.rsa_signature);
    meltp->coin.denom_pub = dkp->pub;
    RND_BLK (&meltp->coin_sig);
    meltp->session_hash = session_hash;
    meltp->amount_with_fee = amount_with_fee;
    meltp->melt_fee = fee_refresh;
  }

  FAILIF (GNUNET_OK != plugin->create_refresh_session (plugin->cls,
                                                       session,
                                                       &session_hash,
                                                       &refresh_session));
  FAILIF (GNUNET_OK != plugin->get_refresh_session (plugin->cls,
                                                    session,
                                                    &session_hash,
                                                    &ret_refresh_session));

  auditor_row_cnt = 0;
  FAILIF (GNUNET_OK != plugin->select_refreshs_above_serial_id (plugin->cls,
                                                                session,
						 	        0,
						                &audit_refresh_session_cb,
							        NULL));
  FAILIF (1 != auditor_row_cnt);
  FAILIF (ret_refresh_session.num_newcoins != refresh_session.num_newcoins);
  FAILIF (ret_refresh_session.noreveal_index != refresh_session.noreveal_index);

  /* check refresh session melt data */
  {
    struct TALER_EXCHANGEDB_RefreshMelt *ret_melt;

    ret_melt = &ret_refresh_session.melt;
    FAILIF (0 != GNUNET_CRYPTO_rsa_signature_cmp
            (ret_melt->coin.denom_sig.rsa_signature,
             meltp->coin.denom_sig.rsa_signature));
    FAILIF (0 != memcmp (&ret_melt->coin.coin_pub,
                         &meltp->coin.coin_pub,
                         sizeof (ret_melt->coin.coin_pub)));
    FAILIF (0 != GNUNET_CRYPTO_rsa_public_key_cmp
            (ret_melt->coin.denom_pub.rsa_public_key,
             meltp->coin.denom_pub.rsa_public_key));
    FAILIF (0 != memcmp (&ret_melt->coin_sig,
                         &meltp->coin_sig,
                         sizeof (ret_melt->coin_sig)));
    FAILIF (0 != memcmp (&ret_melt->session_hash,
                         &meltp->session_hash,
                         sizeof (ret_melt->session_hash)));
    FAILIF (0 != TALER_amount_cmp (&ret_melt->amount_with_fee,
                                   &meltp->amount_with_fee));
    FAILIF (0 != TALER_amount_cmp (&ret_melt->melt_fee,
                                   &meltp->melt_fee));
    GNUNET_CRYPTO_rsa_signature_free (ret_melt->coin.denom_sig.rsa_signature);
    GNUNET_CRYPTO_rsa_public_key_free (ret_melt->coin.denom_pub.rsa_public_key);
  }
  new_dkp = GNUNET_new_array (MELT_NEW_COINS, struct DenomKeyPair *);
  new_denom_pubs = GNUNET_new_array (MELT_NEW_COINS,
                                     struct TALER_DenominationPublicKey);
  for (cnt=0; cnt < MELT_NEW_COINS; cnt++)
  {
    new_dkp[cnt] = create_denom_key_pair (1024,
                                          session,
                                          GNUNET_TIME_absolute_get (),
                                          &value,
                                          &fee_withdraw,
                                          &fee_deposit,
                                          &fee_refresh,
					  &fee_refund);
    new_denom_pubs[cnt] = new_dkp[cnt]->pub;
  }
  FAILIF (GNUNET_OK !=
          plugin->insert_refresh_order (plugin->cls,
                                        session,
                                        &session_hash,
                                        MELT_NEW_COINS,
                                        new_denom_pubs));
  ret_denom_pubs = GNUNET_new_array (MELT_NEW_COINS,
                                     struct TALER_DenominationPublicKey);
  FAILIF (GNUNET_OK !=
          plugin->get_refresh_order (plugin->cls,
                                     session,
                                     &session_hash,
                                     MELT_NEW_COINS,
                                     ret_denom_pubs));
  for (cnt=0; cnt < MELT_NEW_COINS; cnt++)
  {
    FAILIF (0 != GNUNET_CRYPTO_rsa_public_key_cmp
            (ret_denom_pubs[cnt].rsa_public_key,
             new_denom_pubs[cnt].rsa_public_key));
  }
  FAILIF (GNUNET_OK !=
          test_refresh_commit_coins (session,
                                     &refresh_session,
                                     &session_hash));
  FAILIF (GNUNET_OK !=
          test_refresh_commit_links (session,
                                     &refresh_session,
                                     &session_hash));

  for (cnt=0; cnt < MELT_NEW_COINS; cnt++)
  {
    struct GNUNET_HashCode hc;

    RND_BLK (&hc);
    ev_sigs[cnt].rsa_signature
      = GNUNET_CRYPTO_rsa_sign_fdh (new_dkp[cnt]->priv.rsa_private_key,
                                    &hc);
    GNUNET_assert (NULL != ev_sigs[cnt].rsa_signature);
    FAILIF (GNUNET_OK !=
            plugin->insert_refresh_out (plugin->cls,
                                        session,
                                        &session_hash,
                                        cnt,
                                        &ev_sigs[cnt]));
  }

  ldl = plugin->get_link_data_list (plugin->cls,
                                    session,
                                    &session_hash);
  FAILIF (NULL == ldl);
  for (ldlp = ldl; NULL != ldlp; ldlp = ldlp->next)
  {
    int found;

    found = GNUNET_NO;
    for (cnt=0;cnt < MELT_NEW_COINS;cnt++)
    {
      FAILIF (NULL == ldlp->ev_sig.rsa_signature);
      if ( (0 ==
            GNUNET_CRYPTO_rsa_public_key_cmp (ldlp->denom_pub.rsa_public_key,
                                              new_dkp[cnt]->pub.rsa_public_key)) &&
           (0 ==
            GNUNET_CRYPTO_rsa_signature_cmp (ldlp->ev_sig.rsa_signature,
                                             ev_sigs[cnt].rsa_signature)) )
      {
        found = GNUNET_YES;
        break;
      }
    }
    FAILIF (GNUNET_NO == found);
  }
  plugin->free_link_data_list (plugin->cls,
                               ldl);

  {
    int ok;

    ok = GNUNET_NO;
    FAILIF (GNUNET_OK !=
            plugin->get_transfer (plugin->cls,
                                  session,
                                  &meltp->coin.coin_pub,
                                  &check_transfer_data,
                                  &ok));
    FAILIF (GNUNET_OK != ok);
  }

  ret = GNUNET_OK;
 drop:
  for (cnt=0; cnt < MELT_NEW_COINS; cnt++)
    if (NULL != ev_sigs[cnt].rsa_signature)
      GNUNET_CRYPTO_rsa_signature_free (ev_sigs[cnt].rsa_signature);
  if (NULL != commit_coins)
  {
    plugin->free_refresh_commit_coins (plugin->cls,
                                       MELT_NEW_COINS,
                                       commit_coins);
    GNUNET_free (commit_coins);
    commit_coins = NULL;
  }
  destroy_denom_key_pair (dkp);
  GNUNET_CRYPTO_rsa_signature_free (meltp->coin.denom_sig.rsa_signature);
  for (cnt = 0;
       (NULL != ret_denom_pubs) && (cnt < MELT_NEW_COINS)
         && (NULL != ret_denom_pubs[cnt].rsa_public_key);
       cnt++)
    GNUNET_CRYPTO_rsa_public_key_free (ret_denom_pubs[cnt].rsa_public_key);
  GNUNET_free_non_null (ret_denom_pubs);
  GNUNET_free_non_null (new_denom_pubs);
  for (cnt = 0;
       (NULL != new_dkp) && (cnt < MELT_NEW_COINS) && (NULL != new_dkp[cnt]);
       cnt++)
    destroy_denom_key_pair (new_dkp[cnt]);
  GNUNET_free_non_null (new_dkp);
  return ret;
}


/**
 * Callback that should never be called.
 */
static void
cb_wt_never (void *cls,
             const struct TALER_MerchantPublicKeyP *merchant_pub,
             const char *wire_method,
             const struct GNUNET_HashCode *h_wire,
             struct GNUNET_TIME_Absolute exec_time,
             const struct GNUNET_HashCode *h_proposal_data,
             const struct TALER_CoinSpendPublicKeyP *coin_pub,
             const struct TALER_Amount *coin_value,
             const struct TALER_Amount *coin_fee)
{
  GNUNET_assert (0); /* this statement should be unreachable */
}


/**
 * Callback that should never be called.
 */
static void
cb_wtid_never (void *cls,
               const struct TALER_WireTransferIdentifierRawP *wtid,
               const struct TALER_Amount *coin_contribution,
               const struct TALER_Amount *coin_fee,
               struct GNUNET_TIME_Absolute execution_time)
{
  GNUNET_assert (0);
}


static struct TALER_MerchantPublicKeyP merchant_pub_wt;
static struct GNUNET_HashCode h_wire_wt;
static struct GNUNET_HashCode h_proposal_data_wt;
static struct TALER_CoinSpendPublicKeyP coin_pub_wt;
static struct TALER_Amount coin_value_wt;
static struct TALER_Amount coin_fee_wt;
static struct TALER_Amount transfer_value_wt;
static struct GNUNET_TIME_Absolute execution_time_wt;
static struct TALER_WireTransferIdentifierRawP wtid_wt;


/**
 * Callback that should be called with the WT data.
 */
static void
cb_wt_check (void *cls,
             const struct TALER_MerchantPublicKeyP *merchant_pub,
             const char *wire_method,
             const struct GNUNET_HashCode *h_wire,
             struct GNUNET_TIME_Absolute exec_time,
             const struct GNUNET_HashCode *h_proposal_data,
             const struct TALER_CoinSpendPublicKeyP *coin_pub,
             const struct TALER_Amount *coin_value,
             const struct TALER_Amount *coin_fee)
{
  GNUNET_assert (cls == &cb_wt_never);
  GNUNET_assert (0 == memcmp (merchant_pub,
                              &merchant_pub_wt,
                              sizeof (struct TALER_MerchantPublicKeyP)));
  GNUNET_assert (0 == strcmp (wire_method,
                              "SEPA"));
  GNUNET_assert (0 == memcmp (h_wire,
                              &h_wire_wt,
                              sizeof (struct GNUNET_HashCode)));
  GNUNET_assert (exec_time.abs_value_us == execution_time_wt.abs_value_us);
  GNUNET_assert (0 == memcmp (h_proposal_data,
                              &h_proposal_data_wt,
                              sizeof (struct GNUNET_HashCode)));
  GNUNET_assert (0 == memcmp (coin_pub,
                              &coin_pub_wt,
                              sizeof (struct TALER_CoinSpendPublicKeyP)));
  GNUNET_assert (0 == TALER_amount_cmp (coin_value,
                                        &coin_value_wt));
  GNUNET_assert (0 == TALER_amount_cmp (coin_fee,
                                        &coin_fee_wt));
}


/**
 * Callback that should be called with the WT data.
 */
static void
cb_wtid_check (void *cls,
               const struct TALER_WireTransferIdentifierRawP *wtid,
               const struct TALER_Amount *coin_contribution,
               const struct TALER_Amount *coin_fee,
               struct GNUNET_TIME_Absolute execution_time)
{
  GNUNET_assert (cls == &cb_wtid_never);
  GNUNET_assert (0 == memcmp (wtid,
                              &wtid_wt,
                              sizeof (struct TALER_WireTransferIdentifierRawP)));
  GNUNET_assert (execution_time.abs_value_us ==
                 execution_time_wt.abs_value_us);
  GNUNET_assert (0 == TALER_amount_cmp (coin_contribution,
                                        &coin_value_wt));
  GNUNET_assert (0 == TALER_amount_cmp (coin_fee,
                                        &coin_fee_wt));
}


/**
 * Here #deposit_cb() will store the row ID of the deposit.
 */
static uint64_t deposit_rowid;


/**
 * Function called with details about deposits that
 * have been made.  Called in the test on the
 * deposit given in @a cls.
 *
 * @param cls closure a `struct TALER_EXCHANGEDB_Deposit *`
 * @param rowid unique ID for the deposit in our DB, used for marking
 *              it as 'tiny' or 'done'
 * @param merchant_pub public key of the merchant
 * @param coin_pub public key of the coin
 * @param amount_with_fee amount that was deposited including fee
 * @param deposit_fee amount the exchange gets to keep as transaction fees
 * @param h_proposal_data hash of the proposal data known to merchant and customer
 * @param wire_deadline by which the merchant adviced that he would like the
 *        wire transfer to be executed
 * @param wire wire details for the merchant, NULL from iterate_matching_deposits()
 * @return #GNUNET_OK to continue to iterate, #GNUNET_SYSERR if deposit does
 *         not match our expectations
 */
static int
deposit_cb (void *cls,
            uint64_t rowid,
            const struct TALER_MerchantPublicKeyP *merchant_pub,
            const struct TALER_CoinSpendPublicKeyP *coin_pub,
            const struct TALER_Amount *amount_with_fee,
            const struct TALER_Amount *deposit_fee,
            const struct GNUNET_HashCode *h_proposal_data,
            struct GNUNET_TIME_Absolute wire_deadline,
            const json_t *wire)
{
  struct TALER_EXCHANGEDB_Deposit *deposit = cls;
  struct GNUNET_HashCode h_wire;

  deposit_rowid = rowid;
  if (NULL != wire)
    TALER_JSON_hash (wire, &h_wire);
  if ( (0 != memcmp (merchant_pub,
                     &deposit->merchant_pub,
                     sizeof (struct TALER_MerchantPublicKeyP))) ||
       (0 != TALER_amount_cmp (amount_with_fee,
                               &deposit->amount_with_fee)) ||
       (0 != TALER_amount_cmp (deposit_fee,
                               &deposit->deposit_fee)) ||
       (0 != memcmp (h_proposal_data,
                     &deposit->h_proposal_data,
                     sizeof (struct GNUNET_HashCode))) ||
       (0 != memcmp (coin_pub,
                     &deposit->coin.coin_pub,
                     sizeof (struct TALER_CoinSpendPublicKeyP))) ||
       ( (NULL != wire) &&
         (0 != memcmp (&h_wire,
                       &deposit->h_wire,
                       sizeof (struct GNUNET_HashCode))) ) )
  {
    GNUNET_break (0);
    return GNUNET_SYSERR;
  }

  return GNUNET_OK;
}


/**
 * Callback for #select_deposits_above_serial_id ()
 *
 * @param cls closure
 * @param rowid unique serial ID for the deposit in our DB
 * @param timestamp when did the deposit happen
 * @param merchant_pub public key of the merchant
 * @param coin_pub public key of the coin
 * @param coin_sig signature from the coin
 * @param amount_with_fee amount that was deposited including fee
 * @param h_proposal_data hash of the proposal data known to merchant and customer
 * @param refund_deadline by which the merchant adviced that he might want
 *        to get a refund
 * @param wire_deadline by which the merchant adviced that he would like the
 *        wire transfer to be executed
 * @param receiver_wire_account wire details for the merchant, NULL from iterate_matching_deposits()
 * @param done flag set if the deposit was already executed (or not)
 * @return #GNUNET_OK to continue to iterate, #GNUNET_SYSERR to stop
 */
static int
audit_deposit_cb (void *cls,
                  uint64_t rowid,
                  struct GNUNET_TIME_Absolute timestamp,
                  const struct TALER_MerchantPublicKeyP *merchant_pub,
                  const struct TALER_CoinSpendPublicKeyP *coin_pub,
                  const struct TALER_CoinSpendSignatureP *coin_sig,
                  const struct TALER_Amount *amount_with_fee,
                  const struct GNUNET_HashCode *h_proposal_data,
                  struct GNUNET_TIME_Absolute refund_deadline,
                  struct GNUNET_TIME_Absolute wire_deadline,
                  const json_t *receiver_wire_account,
                  int done)
{
  auditor_row_cnt++;
  return GNUNET_OK;
}


/**
 * Function called with details about coins that were refunding,
 * with the goal of auditing the refund's execution.
 *
 * @param cls closure
 * @param rowid unique serial ID for the refund in our DB
 * @param coin_pub public key of the coin
 * @param merchant_pub public key of the merchant
 * @param merchant_sig signature of the merchant
 * @param h_proposal_data hash of the proposal data in
 *                        the contract between merchant and customer
 * @param rtransaction_id refund transaction ID chosen by the merchant
 * @param amount_with_fee amount that was deposited including fee
 * @return #GNUNET_OK to continue to iterate, #GNUNET_SYSERR to stop
 */
static int
audit_refund_cb (void *cls,
                 uint64_t rowid,
                 const struct TALER_CoinSpendPublicKeyP *coin_pub,
                 const struct TALER_MerchantPublicKeyP *merchant_pub,
                 const struct TALER_MerchantSignatureP *merchant_sig,
                 const struct GNUNET_HashCode *h_proposal_data,
                 uint64_t rtransaction_id,
                 const struct TALER_Amount *amount_with_fee)
{
  auditor_row_cnt++;
  return GNUNET_OK;
}


/**
 * Function called with details about incoming wire transfers.
 *
 * @param cls closure
 * @param rowid unique serial ID for the refresh session in our DB
 * @param reserve_pub public key of the reserve (also the WTID)
 * @param credit amount that was received
 * @param sender_account_details information about the sender's bank account
 * @param transfer_details information that uniquely identifies the wire transfer
 * @param execution_date when did we receive the funds
 * @return #GNUNET_OK to continue to iterate, #GNUNET_SYSERR to stop
 */
static int
audit_reserve_in_cb (void *cls,
                     uint64_t rowid,
                     const struct TALER_ReservePublicKeyP *reserve_pub,
                     const struct TALER_Amount *credit,
                     const json_t *sender_account_details,
                     const json_t *transfer_details,
                     struct GNUNET_TIME_Absolute execution_date)
{
  auditor_row_cnt++;
  return GNUNET_OK;
}


/**
 * Function called with details about withdraw operations.
 *
 * @param cls closure
 * @param rowid unique serial ID for the refresh session in our DB
 * @param h_blind_ev blinded hash of the coin's public key
 * @param denom_pub public denomination key of the deposited coin
 * @param denom_sig signature over the deposited coin
 * @param reserve_pub public key of the reserve
 * @param reserve_sig signature over the withdraw operation
 * @param execution_date when did the wallet withdraw the coin
 * @param amount_with_fee amount that was withdrawn
 * @return #GNUNET_OK to continue to iterate, #GNUNET_SYSERR to stop
 */
static int
audit_reserve_out_cb (void *cls,
                      uint64_t rowid,
                      const struct GNUNET_HashCode *h_blind_ev,
                      const struct TALER_DenominationPublicKey *denom_pub,
                      const struct TALER_DenominationSignature *denom_sig,
                      const struct TALER_ReservePublicKeyP *reserve_pub,
                      const struct TALER_ReserveSignatureP *reserve_sig,
                      struct GNUNET_TIME_Absolute execution_date,
                      const struct TALER_Amount *amount_with_fee)
{
  auditor_row_cnt++;
  return GNUNET_OK;
}


/**
 * Test garbage collection.
 *
 * @param session DB session to use
 * @return #GNUNET_OK on success
 */
static int
test_gc (struct TALER_EXCHANGEDB_Session *session)
{
  struct DenomKeyPair *dkp;
  struct GNUNET_TIME_Absolute now;
  struct GNUNET_TIME_Absolute past;
  struct TALER_EXCHANGEDB_DenominationKeyInformationP issue2;

  now = GNUNET_TIME_absolute_get ();
  past = GNUNET_TIME_absolute_subtract (now,
                                        GNUNET_TIME_relative_multiply (GNUNET_TIME_UNIT_HOURS,
                                                                       4));
  dkp = create_denom_key_pair (1024,
                               session,
                               past,
                               &value,
                               &fee_withdraw,
                               &fee_deposit,
                               &fee_refresh,
                               &fee_refund);
  if (GNUNET_OK !=
      plugin->gc (plugin->cls))
  {
    GNUNET_break(0);
    destroy_denom_key_pair (dkp);
    return GNUNET_SYSERR;
  }
  if (GNUNET_OK ==
      plugin->get_denomination_info (plugin->cls,
                                     session,
                                     &dkp->pub,
                                     &issue2))
  {
    GNUNET_break(0);
    destroy_denom_key_pair (dkp);
    return GNUNET_SYSERR;
  }
  destroy_denom_key_pair (dkp);
  return GNUNET_OK;
}


/**
 * Test wire fee storage.
 *
 * @param session DB session to use
 * @return #GNUNET_OK on success
 */
static int
test_wire_fees (struct TALER_EXCHANGEDB_Session *session)
{
  struct GNUNET_TIME_Absolute start_date;
  struct GNUNET_TIME_Absolute end_date;
  struct TALER_Amount wire_fee;
  struct TALER_MasterSignatureP master_sig;
  struct GNUNET_TIME_Absolute sd;
  struct GNUNET_TIME_Absolute ed;
  struct TALER_Amount fee;
  struct TALER_MasterSignatureP ms;

  start_date = GNUNET_TIME_absolute_get ();
  end_date = GNUNET_TIME_relative_to_absolute (GNUNET_TIME_UNIT_MINUTES);
  GNUNET_assert (GNUNET_OK ==
                 TALER_string_to_amount (CURRENCY ":1.424242",
                                         &wire_fee));
  GNUNET_CRYPTO_random_block (GNUNET_CRYPTO_QUALITY_WEAK,
                              &master_sig,
                              sizeof (master_sig));
  if (GNUNET_OK !=
      plugin->insert_wire_fee (plugin->cls,
                               session,
                               "wire-method",
                               start_date,
                               end_date,
                               &wire_fee,
                               &master_sig))
  {
    GNUNET_break (0);
    return GNUNET_SYSERR;
  }
  if (GNUNET_NO !=
      plugin->insert_wire_fee (plugin->cls,
                               session,
                               "wire-method",
                               start_date,
                               end_date,
                               &wire_fee,
                               &master_sig))
  {
    GNUNET_break (0);
    return GNUNET_SYSERR;
  }
  /* This must fail as 'end_date' is NOT in the
     half-open interval [start_date,end_date) */
  if (GNUNET_OK ==
      plugin->get_wire_fee (plugin->cls,
                            session,
                            "wire-method",
                            end_date,
                            &sd,
                            &ed,
                            &fee,
                            &ms))
  {
    GNUNET_break (0);
    return GNUNET_SYSERR;
  }
  if (GNUNET_OK !=
      plugin->get_wire_fee (plugin->cls,
                            session,
                            "wire-method",
                            start_date,
                            &sd,
                            &ed,
                            &fee,
                            &ms))
  {
    GNUNET_break (0);
    return GNUNET_SYSERR;
  }
  if ( (sd.abs_value_us != start_date.abs_value_us) ||
       (ed.abs_value_us != end_date.abs_value_us) ||
       (0 != TALER_amount_cmp (&fee,
                               &wire_fee)) ||
       (0 != memcmp (&ms,
                     &master_sig,
                     sizeof (ms))) )
  {
    GNUNET_break (0);
    return GNUNET_SYSERR;
  }
  return GNUNET_OK;
}


static struct GNUNET_TIME_Absolute wire_out_date;

static struct TALER_WireTransferIdentifierRawP wire_out_wtid;

static json_t *wire_out_account;

static  struct TALER_Amount wire_out_amount;


/**
 * Callback with data about an executed wire transfer.
 *
 * @param cls closure
 * @param rowid identifier of the respective row in the database
 * @param date timestamp of the wire transfer (roughly)
 * @param wtid wire transfer subject
 * @param wire wire transfer details of the receiver
 * @param amount amount that was wired
 */
static void
audit_wire_cb (void *cls,
               uint64_t rowid,
               struct GNUNET_TIME_Absolute date,
               const struct TALER_WireTransferIdentifierRawP *wtid,
               const json_t *wire,
               const struct TALER_Amount *amount)
{
  auditor_row_cnt++;
  GNUNET_assert (0 ==
                 TALER_amount_cmp (amount,
                                   &wire_out_amount));
  GNUNET_assert (0 ==
                 memcmp (wtid,
                         &wire_out_wtid,
                         sizeof (*wtid)));
  GNUNET_assert (date.abs_value_us == wire_out_date.abs_value_us);
}


/**
 * Test API relating to wire_out handling.
 *
 * @param session database session to use for the test
 * @return #GNUNET_OK on success
 */
static int
test_wire_out (struct TALER_EXCHANGEDB_Session *session,
               const struct TALER_EXCHANGEDB_Deposit *deposit)
{
  auditor_row_cnt = 0;
  memset (&wire_out_wtid, 42, sizeof (wire_out_wtid));
  wire_out_date = GNUNET_TIME_absolute_get ();
  (void) GNUNET_TIME_round_abs (&wire_out_date);
  wire_out_account = json_loads ("{ \"account\":\"1\" }", 0, NULL);
  GNUNET_assert (NULL != wire_out_account);
  GNUNET_assert (GNUNET_OK ==
                 TALER_string_to_amount (CURRENCY ":1",
                                         &wire_out_amount));
  FAILIF (GNUNET_OK !=
          plugin->store_wire_transfer_out (plugin->cls,
                                           session,
                                           wire_out_date,
                                           &wire_out_wtid,
                                           wire_out_account,
                                           &wire_out_amount));
  FAILIF (GNUNET_OK !=
          plugin->select_wire_out_above_serial_id (plugin->cls,
                                                   session,
                                                   0,
                                                   &audit_wire_cb,
                                                   NULL));
  FAILIF (1 != auditor_row_cnt);

  /* setup values for wire transfer aggregation data */
  merchant_pub_wt = deposit->merchant_pub;
  h_wire_wt = deposit->h_wire;
  h_proposal_data_wt = deposit->h_proposal_data;
  coin_pub_wt = deposit->coin.coin_pub;
  execution_time_wt = GNUNET_TIME_absolute_get ();
  coin_value_wt = deposit->amount_with_fee;
  coin_fee_wt = fee_deposit;
  GNUNET_assert (GNUNET_OK ==
                 TALER_amount_subtract (&transfer_value_wt,
                                        &coin_value_wt,
                                        &coin_fee_wt));
  FAILIF (GNUNET_NO !=
          plugin->lookup_wire_transfer (plugin->cls,
                                        session,
                                        &wtid_wt,
                                        &cb_wt_never,
                                        NULL));

  {
    struct GNUNET_HashCode h_proposal_data_wt2 = h_proposal_data_wt;

    h_proposal_data_wt2.bits[0]++;
    FAILIF (GNUNET_NO !=
            plugin->wire_lookup_deposit_wtid (plugin->cls,
                                              session,
                                              &h_proposal_data_wt2,
                                              &h_wire_wt,
                                              &coin_pub_wt,
                                              &merchant_pub_wt,
                                              &cb_wtid_never,
                                              NULL));
  }
  /* insert WT data */
  FAILIF (GNUNET_OK !=
          plugin->insert_aggregation_tracking (plugin->cls,
                                               session,
                                               &wtid_wt,
                                               deposit_rowid,
                                               execution_time_wt));
  FAILIF (GNUNET_OK !=
          plugin->lookup_wire_transfer (plugin->cls,
                                        session,
                                        &wtid_wt,
                                        &cb_wt_check,
                                        &cb_wt_never));
  FAILIF (GNUNET_OK !=
          plugin->wire_lookup_deposit_wtid (plugin->cls,
                                            session,
                                            &h_proposal_data_wt,
                                            &h_wire_wt,
                                            &coin_pub_wt,
                                            &merchant_pub_wt,
                                            &cb_wtid_check,
                                            &cb_wtid_never));


  return GNUNET_OK;
 drop:
  return GNUNET_SYSERR;
}


/**
 * Main function that will be run by the scheduler.
 *
 * @param cls closure with config
 */
static void
run (void *cls)
{
  struct GNUNET_CONFIGURATION_Handle *cfg = cls;
  struct TALER_EXCHANGEDB_Session *session;
  struct TALER_ReservePublicKeyP reserve_pub;
  struct DenomKeyPair *dkp;
  struct TALER_EXCHANGEDB_CollectableBlindcoin cbc;
  struct TALER_EXCHANGEDB_CollectableBlindcoin cbc2;
  struct TALER_EXCHANGEDB_ReserveHistory *rh;
  struct TALER_EXCHANGEDB_ReserveHistory *rh_head;
  struct TALER_EXCHANGEDB_BankTransfer *bt;
  struct TALER_EXCHANGEDB_CollectableBlindcoin *withdraw;
  struct TALER_EXCHANGEDB_Deposit deposit;
  struct TALER_EXCHANGEDB_Deposit deposit2;
  struct TALER_EXCHANGEDB_Refund refund;
  struct TALER_EXCHANGEDB_TransactionList *tl;
  struct TALER_EXCHANGEDB_TransactionList *tlp;
  json_t *wire;
  json_t *just;
  json_t *sndr;
  unsigned int matched;
  const char * const json_wire_str =
      "{ \"type\":\"SEPA\", \
\"IBAN\":\"DE67830654080004822650\",                    \
\"name\":\"GNUnet e.V.\",                               \
\"bic\":\"GENODEF1SLR\",                                 \
\"wire_transfer_deadline\":\"1449930207000\",                                \
\"r\":123456789,                                     \
\"address\": \"foobar\"}";
  unsigned int cnt;

  dkp = NULL;
  rh = NULL;
  wire = NULL;
  session = NULL;
  ZR_BLK (&cbc);
  ZR_BLK (&cbc2);
  if (NULL ==
      (plugin = TALER_EXCHANGEDB_plugin_load (cfg)))
  {
    result = 77;
    return;
  }
  (void) plugin->drop_tables (plugin->cls);
  if (GNUNET_OK !=
      plugin->create_tables (plugin->cls))
  {
    result = 77;
    goto drop;
  }
  if (NULL ==
      (session = plugin->get_session (plugin->cls)))
  {
    result = 77;
    goto drop;
  }
  RND_BLK (&reserve_pub);
  GNUNET_assert (GNUNET_OK ==
                 TALER_string_to_amount (CURRENCY ":1.000010",
                                         &value));
  GNUNET_assert (GNUNET_OK ==
                 TALER_string_to_amount (CURRENCY ":0.000010",
                                         &fee_withdraw));
  GNUNET_assert (GNUNET_OK ==
                 TALER_string_to_amount (CURRENCY ":0.000010",
                                         &fee_deposit));
  GNUNET_assert (GNUNET_OK ==
                 TALER_string_to_amount (CURRENCY ":0.000010",
                                         &fee_refresh));
  GNUNET_assert (GNUNET_OK ==
                 TALER_string_to_amount (CURRENCY ":0.000010",
                                         &fee_refund));
  GNUNET_assert (GNUNET_OK ==
                 TALER_string_to_amount (CURRENCY ":1.000010",
                                         &amount_with_fee));

  result = 4;
  sndr = json_loads ("{ \"account\":\"1\" }", 0, NULL);
  GNUNET_assert (NULL != sndr);
  just = json_loads ("{ \"justification\":\"1\" }", 0, NULL);
  GNUNET_assert (NULL != just);
  FAILIF (GNUNET_OK !=
          plugin->reserves_in_insert (plugin->cls,
                                      session,
                                      &reserve_pub,
                                      &value,
                                      GNUNET_TIME_absolute_get (),
                                      sndr,
				      just));
  json_decref (just);
  FAILIF (GNUNET_OK !=
          check_reserve (session,
                         &reserve_pub,
                         value.value,
                         value.fraction,
                         value.currency));
  just = json_loads ("{ \"justification\":\"2\" }", 0, NULL);
  FAILIF (GNUNET_OK !=
          plugin->reserves_in_insert (plugin->cls,
                                      session,
                                      &reserve_pub,
                                      &value,
                                      GNUNET_TIME_absolute_get (),
				      sndr,
                                      just));
  json_decref (just);
  json_decref (sndr);
  FAILIF (GNUNET_OK !=
          check_reserve (session,
                         &reserve_pub,
                         value.value * 2,
                         value.fraction * 2,
                         value.currency));
  result = 5;
  dkp = create_denom_key_pair (1024,
                               session,
                               GNUNET_TIME_absolute_get (),
                               &value,
                               &fee_withdraw,
                               &fee_deposit,
                               &fee_refresh,
			       &fee_refund);
  RND_BLK(&cbc.h_coin_envelope);
  RND_BLK(&cbc.reserve_sig);
  cbc.denom_pub = dkp->pub;
  cbc.sig.rsa_signature
    = GNUNET_CRYPTO_rsa_sign_fdh (dkp->priv.rsa_private_key,
                                  &cbc.h_coin_envelope);
  cbc.reserve_pub = reserve_pub;
  cbc.amount_with_fee = value;
  GNUNET_assert (GNUNET_OK ==
                 TALER_amount_get_zero (CURRENCY, &cbc.withdraw_fee));
  FAILIF (GNUNET_OK !=
          plugin->insert_withdraw_info (plugin->cls,
                                        session,
                                        &cbc));
  FAILIF (GNUNET_OK !=
          check_reserve (session,
                         &reserve_pub,
                         value.value,
                         value.fraction,
                         value.currency));
  FAILIF (GNUNET_YES !=
          plugin->get_withdraw_info (plugin->cls,
                                     session,
                                     &cbc.h_coin_envelope,
                                     &cbc2));
  FAILIF (NULL == cbc2.denom_pub.rsa_public_key);
  FAILIF (0 != memcmp (&cbc2.reserve_sig,
                       &cbc.reserve_sig,
                       sizeof (cbc2.reserve_sig)));
  FAILIF (0 != memcmp (&cbc2.reserve_pub,
                       &cbc.reserve_pub,
                       sizeof (cbc2.reserve_pub)));
  result = 6;
  FAILIF (GNUNET_OK !=
          GNUNET_CRYPTO_rsa_verify (&cbc.h_coin_envelope,
                                    cbc2.sig.rsa_signature,
                                    dkp->pub.rsa_public_key));
  result = 7;
  rh = plugin->get_reserve_history (plugin->cls,
                                    session,
                                    &reserve_pub);
  FAILIF (NULL == rh);
  rh_head = rh;
  for (cnt=0; NULL != rh_head; rh_head=rh_head->next, cnt++)
  {
    switch (rh_head->type)
    {
    case TALER_EXCHANGEDB_RO_BANK_TO_EXCHANGE:
      bt = rh_head->details.bank;
      FAILIF (0 != memcmp (&bt->reserve_pub,
                           &reserve_pub,
                           sizeof (reserve_pub)));
      /* this is the amount we trasferred twice*/
      FAILIF (1 != bt->amount.value);
      FAILIF (1000 != bt->amount.fraction);
      FAILIF (0 != strcmp (CURRENCY, bt->amount.currency));
      FAILIF (NULL == bt->sender_account_details);
      FAILIF (NULL == bt->transfer_details);
      break;
    case TALER_EXCHANGEDB_RO_WITHDRAW_COIN:
      withdraw = rh_head->details.withdraw;
      FAILIF (0 != memcmp (&withdraw->reserve_pub,
                           &reserve_pub,
                           sizeof (reserve_pub)));
      FAILIF (0 != memcmp (&withdraw->h_coin_envelope,
                           &cbc.h_coin_envelope,
                           sizeof (cbc.h_coin_envelope)));
      break;
    }
  }
  FAILIF (3 != cnt);

  auditor_row_cnt = 0;
  FAILIF (GNUNET_OK !=
          plugin->select_reserves_in_above_serial_id (plugin->cls,
	                                              session,
						      0,
						      &audit_reserve_in_cb,
						      NULL));
  FAILIF (GNUNET_OK !=
          plugin->select_reserves_out_above_serial_id (plugin->cls,
	                                               session,
				                       0,
						       &audit_reserve_out_cb,
						       NULL));
  FAILIF (3 != auditor_row_cnt);
  /* Tests for deposits */
  memset (&deposit, 0, sizeof (deposit));
  RND_BLK (&deposit.coin.coin_pub);
  deposit.coin.denom_pub = dkp->pub;
  deposit.coin.denom_sig = cbc.sig;
  RND_BLK (&deposit.csig);
  RND_BLK (&deposit.merchant_pub);
  RND_BLK (&deposit.h_proposal_data);
  wire = json_loads (json_wire_str, 0, NULL);
  TALER_JSON_hash (wire,
                   &deposit.h_wire);
  deposit.receiver_wire_account = wire;
  deposit.amount_with_fee = value;
  deposit.deposit_fee = fee_deposit;
  result = 8;
  FAILIF (GNUNET_OK !=
          plugin->insert_deposit (plugin->cls,
                                  session, &deposit));
  FAILIF (GNUNET_YES !=
          plugin->have_deposit (plugin->cls,
                                session,
                                &deposit));
  auditor_row_cnt = 0;
  FAILIF (GNUNET_OK !=
          plugin->select_deposits_above_serial_id (plugin->cls,
	                                           session,
						   0,
						   &audit_deposit_cb,
						   NULL));
  FAILIF (1 != auditor_row_cnt);
  result = 9;
  FAILIF (1 !=
          plugin->iterate_matching_deposits (plugin->cls,
                                             session,
                                             &deposit.h_wire,
                                             &deposit.merchant_pub,
                                             &deposit_cb, &deposit,
                                             2));

  FAILIF (1 !=
          plugin->get_ready_deposit (plugin->cls,
                                     session,
                                     &deposit_cb,
                                     &deposit));
  FAILIF (GNUNET_OK !=
          plugin->start (plugin->cls,
                         session));
  FAILIF (GNUNET_OK !=
          plugin->mark_deposit_tiny (plugin->cls,
                                     session,
                                     deposit_rowid));
  FAILIF (0 !=
          plugin->get_ready_deposit (plugin->cls,
                                     session,
                                     &deposit_cb,
                                     &deposit));
  plugin->rollback (plugin->cls,
                    session);
  FAILIF (1 !=
          plugin->get_ready_deposit (plugin->cls,
                                     session,
                                     &deposit_cb,
                                     &deposit));
  FAILIF (GNUNET_OK !=
          plugin->start (plugin->cls,
                         session));
  FAILIF (GNUNET_NO !=
          plugin->test_deposit_done (plugin->cls,
                                     session,
                                     &deposit));
  FAILIF (GNUNET_OK !=
          plugin->mark_deposit_done (plugin->cls,
                                     session,
                                     deposit_rowid));
  FAILIF (GNUNET_OK !=
          plugin->commit (plugin->cls,
                          session));
  FAILIF (GNUNET_YES !=
          plugin->test_deposit_done (plugin->cls,
                                     session,
                                     &deposit));

  result = 10;
  deposit2 = deposit;
  RND_BLK (&deposit2.merchant_pub); /* should fail if merchant is different */
  FAILIF (GNUNET_NO !=
          plugin->have_deposit (plugin->cls,
                                session,
                                &deposit2));
  deposit2.merchant_pub = deposit.merchant_pub;
  RND_BLK (&deposit2.coin.coin_pub); /* should fail if coin is different */
  FAILIF (GNUNET_NO !=
          plugin->have_deposit (plugin->cls,
                                session,
                                &deposit2));
  FAILIF (GNUNET_OK != test_melting (session));


  /* test insert_refund! */
  refund.coin = deposit.coin;
  refund.merchant_pub = deposit.merchant_pub;
  RND_BLK (&refund.merchant_sig);
  refund.h_proposal_data = deposit.h_proposal_data;
  refund.rtransaction_id = GNUNET_CRYPTO_random_u64 (GNUNET_CRYPTO_QUALITY_WEAK, UINT64_MAX);
  refund.refund_amount = deposit.amount_with_fee;
  refund.refund_fee = fee_refund;
  FAILIF (GNUNET_OK !=
          plugin->insert_refund (plugin->cls,
                                 session,
                                 &refund));
  auditor_row_cnt = 0;
  FAILIF (GNUNET_OK !=
          plugin->select_refunds_above_serial_id (plugin->cls,
	                                          session,
						  0,
						  &audit_refund_cb,
						  NULL));

  FAILIF (1 != auditor_row_cnt);
  tl = plugin->get_coin_transactions (plugin->cls,
                                      session,
                                      &refund.coin.coin_pub);
  GNUNET_assert (NULL != tl);
  matched = 0;
  for (tlp = tl; NULL != tlp; tlp = tlp->next)
  {
    switch (tlp->type)
    {
    case TALER_EXCHANGEDB_TT_DEPOSIT:
      {
        struct TALER_EXCHANGEDB_Deposit *have = tlp->details.deposit;

        FAILIF (0 != memcmp (&have->coin.coin_pub,
                             &deposit.coin.coin_pub,
                             sizeof (struct TALER_CoinSpendPublicKeyP)));
        /* Note: we're not comparing the denomination keys, as there is
           still the question of whether we should even bother exporting
           them here. */
        FAILIF (0 != memcmp (&have->csig,
                             &deposit.csig,
                             sizeof (struct TALER_CoinSpendSignatureP)));
        FAILIF (0 != memcmp (&have->merchant_pub,
                             &deposit.merchant_pub,
                             sizeof (struct TALER_MerchantPublicKeyP)));
        FAILIF (0 != memcmp (&have->h_proposal_data,
                             &deposit.h_proposal_data,
                             sizeof (struct GNUNET_HashCode)));
        FAILIF (0 != memcmp (&have->h_wire,
                             &deposit.h_wire,
                             sizeof (struct GNUNET_HashCode)));
        /* Note: not comparing 'wire', seems truly redundant and would be tricky */
        FAILIF (have->timestamp.abs_value_us != deposit.timestamp.abs_value_us);
        FAILIF (have->refund_deadline.abs_value_us != deposit.refund_deadline.abs_value_us);
        FAILIF (have->wire_deadline.abs_value_us != deposit.wire_deadline.abs_value_us);
        FAILIF (0 != TALER_amount_cmp (&have->amount_with_fee,
                                       &deposit.amount_with_fee));
        FAILIF (0 != TALER_amount_cmp (&have->deposit_fee,
                                       &deposit.deposit_fee));
        matched |= 1;
        break;
      }
#if 0
      /* this coin pub was actually never melted... */
    case TALER_EXCHANGEDB_TT_REFRESH_MELT:
      FAILIF (0 != memcmp (&melt,
                           &tlp->details.melt,
                           sizeof (struct TALER_EXCHANGEDB_RefreshMelt)));
      matched |= 2;
      break;
#endif
    case TALER_EXCHANGEDB_TT_REFUND:
      {
        struct TALER_EXCHANGEDB_Refund *have = tlp->details.refund;

        FAILIF (0 != memcmp (&have->coin.coin_pub,
                             &refund.coin.coin_pub,
                             sizeof (struct TALER_CoinSpendPublicKeyP)));
        /* Note: we're not comparing the denomination keys, as there is
           still the question of whether we should even bother exporting
           them here. */
        FAILIF (0 != memcmp (&have->merchant_pub,
                             &refund.merchant_pub,
                             sizeof (struct TALER_MerchantPublicKeyP)));
        FAILIF (0 != memcmp (&have->merchant_sig,
                             &refund.merchant_sig,
                             sizeof (struct TALER_MerchantSignatureP)));
        FAILIF (0 != memcmp (&have->h_proposal_data,
                             &refund.h_proposal_data,
                             sizeof (struct GNUNET_HashCode)));
        FAILIF (have->rtransaction_id != refund.rtransaction_id);
        FAILIF (0 != TALER_amount_cmp (&have->refund_amount,
                                       &refund.refund_amount));
        FAILIF (0 != TALER_amount_cmp (&have->refund_fee,
                                       &refund.refund_fee));
        matched |= 4;
        break;
      }
    default:
      FAILIF (1);
      break;
    }
  }
  FAILIF (5 != matched);

  plugin->free_coin_transaction_list (plugin->cls,
                                      tl);

  FAILIF (GNUNET_OK !=
          test_wire_prepare (session));
  FAILIF (GNUNET_OK !=
          test_wire_out (session,
                         &deposit));
  FAILIF (GNUNET_OK !=
          test_gc (session));
  FAILIF (GNUNET_OK !=
          test_wire_fees (session));

  result = 0;

 drop:
  if (NULL != wire)
    json_decref (wire);
  if (NULL != rh)
    plugin->free_reserve_history (plugin->cls,
                                  rh);
  rh = NULL;
  GNUNET_break (GNUNET_OK ==
                plugin->drop_tables (plugin->cls));
  if (NULL != dkp)
    destroy_denom_key_pair (dkp);
  if (NULL != cbc.sig.rsa_signature)
    GNUNET_CRYPTO_rsa_signature_free (cbc.sig.rsa_signature);
  if (NULL != cbc2.denom_pub.rsa_public_key)
    GNUNET_CRYPTO_rsa_public_key_free (cbc2.denom_pub.rsa_public_key);
  if (NULL != cbc2.sig.rsa_signature)
    GNUNET_CRYPTO_rsa_signature_free (cbc2.sig.rsa_signature);
  dkp = NULL;
  TALER_EXCHANGEDB_plugin_unload (plugin);
  plugin = NULL;
}


int
main (int argc,
      char *const argv[])
{
  const char *plugin_name;
  char *config_filename;
  char *testname;
  struct GNUNET_CONFIGURATION_Handle *cfg;

  result = -1;
  if (NULL == (plugin_name = strrchr (argv[0], (int) '-')))
  {
    GNUNET_break (0);
    return -1;
  }
  GNUNET_log_setup (argv[0],
                    "WARNING",
                    NULL);
  plugin_name++;
  (void) GNUNET_asprintf (&testname,
                          "test-exchange-db-%s", plugin_name);
  (void) GNUNET_asprintf (&config_filename,
                          "%s.conf", testname);
  cfg = GNUNET_CONFIGURATION_create ();
  if (GNUNET_OK !=
      GNUNET_CONFIGURATION_parse (cfg,
                                  config_filename))
  {
    GNUNET_break (0);
    GNUNET_free (config_filename);
    GNUNET_free (testname);
    return 2;
  }
  GNUNET_SCHEDULER_run (&run, cfg);
  GNUNET_CONFIGURATION_destroy (cfg);
  GNUNET_free (config_filename);
  GNUNET_free (testname);
  return result;
}
