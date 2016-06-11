/*
  This file is part of TALER
  Copyright (C) 2014, 2015, 2016 GNUnet e.V.

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
 * @file taler-exchange-httpd_db.c
 * @brief High-level (transactional-layer) database operations for the exchange.
 * @author Christian Grothoff
 */
#include "platform.h"
#include <pthread.h>
#include <jansson.h>
#include <gnunet/gnunet_json_lib.h>
#include "taler_json_lib.h"
#include "taler-exchange-httpd_responses.h"
#include "taler-exchange-httpd_keystate.h"

/**
 * How often should we retry a transaction before giving up
 * (for transactions resulting in serialization/dead locks only).
 */
#define MAX_TRANSACTION_COMMIT_RETRIES 3

/**
 * Code to begin a transaction, must be inline as we define a block
 * that ends with #COMMIT_TRANSACTION() within which we perform a number
 * of retries.  Note that this code may call "return" internally, so
 * it must be called within a function where any cleanup will be done
 * by the caller. Furthermore, the function's return value must
 * match that of a #TMH_RESPONSE_reply_internal_db_error() status code.
 *
 * @param session session handle
 * @param connection connection handle
 */
#define START_TRANSACTION(session,connection)                 \
{ /* start new scope, will be ended by COMMIT_TRANSACTION() */\
  unsigned int transaction_retries = 0;                       \
  int transaction_commit_result;                              \
transaction_start_label: /* we will use goto for retries */   \
  if (GNUNET_OK !=                                            \
      TMH_plugin->start (TMH_plugin->cls,                     \
                         session))                            \
  {                                                           \
    GNUNET_break (0);                                         \
    return TMH_RESPONSE_reply_internal_db_error (connection); \
  }

/**
 * Code to conclude a transaction, dual to #START_TRANSACTION().  Note
 * that this code may call "return" internally, so it must be called
 * within a function where any cleanup will be done by the caller.
 * Furthermore, the function's return value must match that of a
 * #TMH_RESPONSE_reply_internal_db_error() status code.
 *
 * @param session session handle
 * @param connection connection handle
 */
#define COMMIT_TRANSACTION(session,connection)                             \
  transaction_commit_result =                                              \
    TMH_plugin->commit (TMH_plugin->cls,                                   \
                        session);                                          \
  if (GNUNET_SYSERR == transaction_commit_result)                          \
  {                                                                        \
    TALER_LOG_WARNING ("Transaction commit failed in %s\n", __FUNCTION__); \
    return TMH_RESPONSE_reply_commit_error (connection);                   \
  }                                                                        \
  if (GNUNET_NO == transaction_commit_result)                              \
  {                                                                        \
    TALER_LOG_WARNING ("Transaction commit failed in %s\n", __FUNCTION__); \
    if (transaction_retries++ <= MAX_TRANSACTION_COMMIT_RETRIES)           \
      goto transaction_start_label;                                        \
    TALER_LOG_WARNING ("Transaction commit failed %u times in %s\n",       \
                       transaction_retries,                                \
                       __FUNCTION__);                                      \
    return TMH_RESPONSE_reply_commit_error (connection);                   \
  }                                                                        \
} /* end of scope opened by BEGIN_TRANSACTION */


/**
 * Calculate the total value of all transactions performed.
 * Stores @a off plus the cost of all transactions in @a tl
 * in @a ret.
 *
 * @param tl transaction list to process
 * @param off offset to use as the starting value
 * @param ret where the resulting total is to be stored
 * @return #GNUNET_OK on success, #GNUNET_SYSERR on errors
 */
static int
calculate_transaction_list_totals (struct TALER_EXCHANGEDB_TransactionList *tl,
                                   const struct TALER_Amount *off,
                                   struct TALER_Amount *ret)
{
  struct TALER_Amount spent = *off;
  struct TALER_EXCHANGEDB_TransactionList *pos;
  struct TALER_Amount refunded;

  TALER_amount_get_zero (spent.currency,
                         &refunded);
  for (pos = tl; NULL != pos; pos = pos->next)
  {
    switch (pos->type)
    {
    case TALER_EXCHANGEDB_TT_DEPOSIT:
      /* spent += pos->amount_with_fee */
      if (GNUNET_OK !=
          TALER_amount_add (&spent,
                            &spent,
                            &pos->details.deposit->amount_with_fee))
      {
        GNUNET_break (0);
        return GNUNET_SYSERR;
      }
      break;
    case TALER_EXCHANGEDB_TT_REFRESH_MELT:
      /* spent += pos->amount_with_fee */
      if (GNUNET_OK !=
          TALER_amount_add (&spent,
                            &spent,
                            &pos->details.melt->amount_with_fee))
      {
        GNUNET_break (0);
        return GNUNET_SYSERR;
      }
      break;
    case TALER_EXCHANGEDB_TT_REFUND:
      /* refunded += pos->refund_amount - pos->refund_fee */
      if (GNUNET_OK !=
          TALER_amount_add (&refunded,
                            &refunded,
                            &pos->details.refund->refund_amount))
      {
        GNUNET_break (0);
        return GNUNET_SYSERR;
      }
      if (GNUNET_OK !=
          TALER_amount_subtract (&refunded,
                                 &refunded,
                                 &pos->details.refund->refund_fee))
      {
        GNUNET_break (0);
        return GNUNET_SYSERR;
      }
      break;
    }
  }
  /* spent = spent - refunded */
  if (GNUNET_OK !=
      TALER_amount_subtract (&spent,
                             &spent,
                             &refunded))
  {
    GNUNET_break (0);
    return GNUNET_SYSERR;
  }

  *ret = spent;
  return GNUNET_OK;
}


/**
 * Execute a deposit.  The validity of the coin and signature
 * have already been checked.  The database must now check that
 * the coin is not (double or over) spent, and execute the
 * transaction (record details, generate success or failure response).
 *
 * @param connection the MHD connection to handle
 * @param deposit information about the deposit
 * @return MHD result code
 */
int
TMH_DB_execute_deposit (struct MHD_Connection *connection,
                        const struct TALER_EXCHANGEDB_Deposit *deposit)
{
  struct TALER_EXCHANGEDB_Session *session;
  struct TALER_EXCHANGEDB_TransactionList *tl;
  struct TALER_Amount spent;
  struct TALER_Amount value;
  struct TALER_Amount amount_without_fee;
  struct TMH_KS_StateHandle *mks;
  struct TALER_EXCHANGEDB_DenominationKeyIssueInformation *dki;
  int ret;

  if (NULL == (session = TMH_plugin->get_session (TMH_plugin->cls)))
  {
    GNUNET_break (0);
    return TMH_RESPONSE_reply_internal_db_error (connection);
  }
  if (GNUNET_YES ==
      TMH_plugin->have_deposit (TMH_plugin->cls,
                                session,
                                deposit))
  {
    GNUNET_assert (GNUNET_OK ==
                   TALER_amount_subtract (&amount_without_fee,
                                          &deposit->amount_with_fee,
                                          &deposit->deposit_fee));
    return TMH_RESPONSE_reply_deposit_success (connection,
                                               &deposit->coin.coin_pub,
                                               &deposit->h_wire,
                                               &deposit->h_contract,
                                               deposit->transaction_id,
                                               deposit->timestamp,
                                               deposit->refund_deadline,
                                               &deposit->merchant_pub,
                                               &amount_without_fee);
  }
  mks = TMH_KS_acquire ();
  dki = TMH_KS_denomination_key_lookup (mks,
                                        &deposit->coin.denom_pub,
					TMH_KS_DKU_DEPOSIT);
  if (NULL == dki)
  {
    TMH_KS_release (mks);
    return TMH_RESPONSE_reply_arg_invalid (connection,
                                           "denom_pub");
  }
  TALER_amount_ntoh (&value,
                     &dki->issue.properties.value);
  TMH_KS_release (mks);

  START_TRANSACTION (session, connection);

  /* fee for THIS transaction */
  spent = deposit->amount_with_fee;
  /* add cost of all previous transactions */
  tl = TMH_plugin->get_coin_transactions (TMH_plugin->cls,
                                          session,
                                          &deposit->coin.coin_pub);
  if (GNUNET_OK !=
      calculate_transaction_list_totals (tl,
                                         &spent,
                                         &spent))
  {
    TMH_plugin->rollback (TMH_plugin->cls,
                          session);
    TMH_plugin->free_coin_transaction_list (TMH_plugin->cls,
                                            tl);
    return TMH_RESPONSE_reply_internal_db_error (connection);
  }
  /* Check that cost of all transactions is smaller than
     the value of the coin. */
  if (0 < TALER_amount_cmp (&spent,
                            &value))
  {
    TMH_plugin->rollback (TMH_plugin->cls,
                          session);
    ret = TMH_RESPONSE_reply_deposit_insufficient_funds (connection,
                                                         tl);
    TMH_plugin->free_coin_transaction_list (TMH_plugin->cls,
                                            tl);
    return ret;
  }
  TMH_plugin->free_coin_transaction_list (TMH_plugin->cls,
                                          tl);
  if (GNUNET_OK !=
      TMH_plugin->insert_deposit (TMH_plugin->cls,
                                  session,
                                  deposit))
  {
    TALER_LOG_WARNING ("Failed to store /deposit information in database\n");
    TMH_plugin->rollback (TMH_plugin->cls,
                          session);
    return TMH_RESPONSE_reply_internal_db_error (connection);
  }

  COMMIT_TRANSACTION(session, connection);
  GNUNET_assert (GNUNET_SYSERR !=
                 TALER_amount_subtract (&amount_without_fee,
                                        &deposit->amount_with_fee,
                                        &deposit->deposit_fee));
  return TMH_RESPONSE_reply_deposit_success (connection,
                                             &deposit->coin.coin_pub,
                                             &deposit->h_wire,
                                             &deposit->h_contract,
                                             deposit->transaction_id,
                                             deposit->timestamp,
                                             deposit->refund_deadline,
                                             &deposit->merchant_pub,
                                             &amount_without_fee);
}


/**
 * Execute a "/refund".  Returns a confirmation that the refund
 * was successful, or a failure if we are not aware of a matching
 * /deposit or if it is too late to do the refund.
 *
 * @param connection the MHD connection to handle
 * @param refund refund details
 * @return MHD result code
 */
int
TMH_DB_execute_refund (struct MHD_Connection *connection,
                       const struct TALER_EXCHANGEDB_Refund *refund)
{
  struct TALER_EXCHANGEDB_Session *session;
  struct TALER_EXCHANGEDB_TransactionList *tl;
  struct TALER_EXCHANGEDB_TransactionList *tlp;
  const struct TALER_EXCHANGEDB_Deposit *dep;
  const struct TALER_EXCHANGEDB_Refund *ref;
  struct TMH_KS_StateHandle *mks;
  struct TALER_EXCHANGEDB_DenominationKeyIssueInformation *dki;
  struct TALER_Amount expect_fee;
  int ret;
  int deposit_found;
  int refund_found;
  int done;
  int fee_cmp;

  if (NULL == (session = TMH_plugin->get_session (TMH_plugin->cls)))
  {
    GNUNET_break (0);
    return TMH_RESPONSE_reply_internal_db_error (connection);
  }
  dep = NULL;
  ref = NULL;
  START_TRANSACTION (session, connection);
  tl = TMH_plugin->get_coin_transactions (TMH_plugin->cls,
                                          session,
                                          &refund->coin.coin_pub);
  if (NULL == tl)
  {
    TMH_plugin->rollback (TMH_plugin->cls,
                          session);
    return TMH_RESPONSE_reply_refund_failure (connection,
                                              MHD_HTTP_NOT_FOUND);
  }
  deposit_found = GNUNET_NO;
  refund_found = GNUNET_NO;
  for (tlp = tl; NULL != tlp; tlp = tlp->next)
  {
    switch (tlp->type)
    {
    case TALER_EXCHANGEDB_TT_DEPOSIT:
      if (GNUNET_NO == deposit_found)
      {
        dep = tlp->details.deposit;
        if ( (0 == memcmp (&dep->merchant_pub,
                           &refund->merchant_pub,
                           sizeof (struct TALER_MerchantPublicKeyP))) &&
             (0 == memcmp (&dep->h_contract,
                           &refund->h_contract,
                           sizeof (struct GNUNET_HashCode))) &&
             (dep->transaction_id == refund->transaction_id) )
        {
          deposit_found = GNUNET_YES;
          break;
        }
      }
      break;
    case TALER_EXCHANGEDB_TT_REFRESH_MELT:
      /* Melts cannot be refunded, ignore here */
      break;
    case TALER_EXCHANGEDB_TT_REFUND:
      if (GNUNET_NO == refund_found)
      {
        ref = tlp->details.refund;
        /* First, check if existing refund request is identical */
        if ( (0 == memcmp (&ref->merchant_pub,
                           &refund->merchant_pub,
                           sizeof (struct TALER_MerchantPublicKeyP))) &&
             (0 == memcmp (&ref->h_contract,
                           &refund->h_contract,
                           sizeof (struct GNUNET_HashCode))) &&
             (ref->transaction_id == refund->transaction_id) &&
             (ref->rtransaction_id == refund->rtransaction_id) )
        {
          refund_found = GNUNET_YES;
          break;
        }
        /* Second, check if existing refund request conflicts */
        if ( (0 == memcmp (&ref->merchant_pub,
                           &refund->merchant_pub,
                           sizeof (struct TALER_MerchantPublicKeyP))) &&
             (0 == memcmp (&ref->h_contract,
                           &refund->h_contract,
                           sizeof (struct GNUNET_HashCode))) &&
             (ref->transaction_id == refund->transaction_id) &&
             (ref->rtransaction_id != refund->rtransaction_id) )
        {
          GNUNET_break_op (0); /* conflicting refound found */
          refund_found = GNUNET_SYSERR;
          /* NOTE: Alternatively we could total up all existing
             refunds and check if the sum still permits the
             refund requested (thus allowing multiple, partial
             refunds). Fow now, we keep it simple. */
          break;
        }
      }
      break;
    }
  }
  /* handle if deposit was NOT found */
  if (GNUNET_NO == deposit_found)
  {
    TMH_plugin->rollback (TMH_plugin->cls,
                          session);
    TMH_plugin->free_coin_transaction_list (TMH_plugin->cls,
                                            tl);
    return TMH_RESPONSE_reply_transaction_unknown (connection);
  }
  /* handle if conflicting refund found */
  if (GNUNET_SYSERR == refund_found)
  {
    TMH_plugin->rollback (TMH_plugin->cls,
                          session);
    ret = TMH_RESPONSE_reply_refund_conflict (connection,
                                              tl);
    TMH_plugin->free_coin_transaction_list (TMH_plugin->cls,
                                            tl);
    return ret;
  }
  /* handle if identical refund found */
  if (GNUNET_YES == refund_found)
  {
    /* /refund already done, simply re-transmit confirmation */
    TMH_plugin->rollback (TMH_plugin->cls,
                          session);
    ret = TMH_RESPONSE_reply_refund_success (connection,
                                             ref);
    TMH_plugin->free_coin_transaction_list (TMH_plugin->cls,
                                            tl);
    return ret;
  }

  /* check currency is compatible */
  if ( (GNUNET_YES !=
        TALER_amount_cmp_currency (&refund->refund_amount,
                                   &dep->amount_with_fee)) ||
       (GNUNET_YES !=
        TALER_amount_cmp_currency (&refund->refund_fee,
                                   &dep->deposit_fee)) )
  {
    GNUNET_break_op (0); /* currency missmatch */
    TMH_plugin->rollback (TMH_plugin->cls,
                          session);
    return TMH_RESPONSE_reply_refund_failure (connection,
                                              MHD_HTTP_PRECONDITION_FAILED);
  }

  /* check if we already send the money for the /deposit */
  done = TMH_plugin->test_deposit_done (TMH_plugin->cls,
                                        session,
                                        dep);
  if (GNUNET_SYSERR == done)
  {
    /* Internal error, we first had the deposit in the history,
       but now it is gone? */
    GNUNET_break (0);
    TMH_plugin->free_coin_transaction_list (TMH_plugin->cls,
                                            tl);
    TMH_plugin->rollback (TMH_plugin->cls,
                          session);
    return TMH_RESPONSE_reply_internal_error (connection,
                                              "database inconsistent");
  }
  if (GNUNET_YES == done)
  {
    /* money was already transferred to merchant, can no longer refund */
    TMH_plugin->rollback (TMH_plugin->cls,
                          session);
    TMH_plugin->free_coin_transaction_list (TMH_plugin->cls,
                                            tl);
    return TMH_RESPONSE_reply_refund_failure (connection,
                                              MHD_HTTP_GONE);
  }

  /* check refund amount is sufficiently low */
  if (1 == TALER_amount_cmp (&refund->refund_amount,
                             &dep->amount_with_fee) )
  {
    GNUNET_break_op (0); /* cannot refund more than original value */
    TMH_plugin->free_coin_transaction_list (TMH_plugin->cls,
                                            tl);
    return TMH_RESPONSE_reply_refund_failure (connection,
                                              MHD_HTTP_PRECONDITION_FAILED);
  }

  /* Check refund fee matches fee of denomination key! */
  mks = TMH_KS_acquire ();
  dki = TMH_KS_denomination_key_lookup (mks,
                                        &dep->coin.denom_pub,
					TMH_KS_DKU_DEPOSIT);
  if (NULL == dki)
  {
    /* DKI not found, but we do have a coin with this DK in our database;
       not good... */
    GNUNET_break (0);
    TMH_KS_release (mks);
    TMH_plugin->free_coin_transaction_list (TMH_plugin->cls,
                                            tl);
    return TMH_RESPONSE_reply_internal_error (connection,
                                              "denomination key not found");
  }
  TALER_amount_ntoh (&expect_fee,
                     &dki->issue.properties.fee_refund);
  fee_cmp = TALER_amount_cmp (&refund->refund_fee,
                              &expect_fee);
  TMH_KS_release (mks);

  if (-1 == fee_cmp)
  {
    TMH_plugin->rollback (TMH_plugin->cls,
                          session);
    TMH_plugin->free_coin_transaction_list (TMH_plugin->cls,
                                            tl);
    return TMH_RESPONSE_reply_arg_invalid (connection,
                                           "refund_fee");
  }
  if (1 == fee_cmp)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                "Refund fee proposed by merchant is higher than necessary.\n");
  }
  TMH_plugin->free_coin_transaction_list (TMH_plugin->cls,
                                          tl);

  /* Finally, store new refund data */
  if (GNUNET_OK !=
      TMH_plugin->insert_refund (TMH_plugin->cls,
                                 session,
                                 refund))
  {
    TALER_LOG_WARNING ("Failed to store /refund information in database\n");
    TMH_plugin->rollback (TMH_plugin->cls,
                          session);
    return TMH_RESPONSE_reply_internal_db_error (connection);
  }
  COMMIT_TRANSACTION(session, connection);

  return TMH_RESPONSE_reply_refund_success (connection,
                                            refund);
}


/**
 * Execute a /reserve/status.  Given the public key of a reserve,
 * return the associated transaction history.
 *
 * @param connection the MHD connection to handle
 * @param reserve_pub public key of the reserve to check
 * @return MHD result code
 */
int
TMH_DB_execute_reserve_status (struct MHD_Connection *connection,
                               const struct TALER_ReservePublicKeyP *reserve_pub)
{
  struct TALER_EXCHANGEDB_Session *session;
  struct TALER_EXCHANGEDB_ReserveHistory *rh;
  int res;

  if (NULL == (session = TMH_plugin->get_session (TMH_plugin->cls)))
  {
    GNUNET_break (0);
    return TMH_RESPONSE_reply_internal_db_error (connection);
  }
  rh = TMH_plugin->get_reserve_history (TMH_plugin->cls,
                                        session,
                                        reserve_pub);
  if (NULL == rh)
    return TMH_RESPONSE_reply_json_pack (connection,
                                         MHD_HTTP_NOT_FOUND,
                                         "{s:s, s:s}",
                                         "error", "Reserve not found",
                                         "parameter", "withdraw_pub");
  res = TMH_RESPONSE_reply_reserve_status_success (connection,
                                                    rh);
  TMH_plugin->free_reserve_history (TMH_plugin->cls,
                                    rh);
  return res;
}


/**
 * Try to execute /reserve/withdraw transaction.
 *
 * @param connection request we are handling
 * @param session database session we are using
 * @param key_state key state to lookup denomination pubs
 * @param reserve reserve to withdraw from
 * @param denomination_pub public key of the denomination requested
 * @param dki denomination to withdraw
 * @param blinded_msg blinded message to be signed
 * @param blinded_msg_len number of bytes in @a blinded_msg
 * @param h_blind hash of @a blinded_msg
 * @param signature signature over the withdraw request, to be stored in DB
 * @param denom_sig[out] where to write the resulting signature
 *        (used to release memory in case of transaction failure
 * @return MHD result code
 */
static int
execute_reserve_withdraw_transaction (struct MHD_Connection *connection,
                                      struct TALER_EXCHANGEDB_Session *session,
                                      struct TMH_KS_StateHandle *key_state,
                                      const struct TALER_ReservePublicKeyP *reserve,
                                      const struct TALER_DenominationPublicKey *denomination_pub,
                                      const struct TALER_EXCHANGEDB_DenominationKeyIssueInformation *dki,
                                      const char *blinded_msg,
                                      size_t blinded_msg_len,
                                      const struct GNUNET_HashCode *h_blind,
                                      const struct TALER_ReserveSignatureP *signature,
                                      struct TALER_DenominationSignature *denom_sig)
{
  struct TALER_EXCHANGEDB_ReserveHistory *rh;
  const struct TALER_EXCHANGEDB_ReserveHistory *pos;
  struct TALER_EXCHANGEDB_DenominationKeyIssueInformation *tdki;
  struct TALER_EXCHANGEDB_CollectableBlindcoin collectable;
  struct TALER_Amount amount_required;
  struct TALER_Amount deposit_total;
  struct TALER_Amount withdraw_total;
  struct TALER_Amount balance;
  struct TALER_Amount value;
  struct TALER_Amount fee_withdraw;
  int res;

  /* Check if balance is sufficient */
  START_TRANSACTION (session, connection);
  rh = TMH_plugin->get_reserve_history (TMH_plugin->cls,
                                        session,
                                        reserve);
  if (NULL == rh)
  {
    TMH_plugin->rollback (TMH_plugin->cls,
                          session);
    return TMH_RESPONSE_reply_arg_unknown (connection,
                                           "reserve_pub");
  }

  /* calculate amount required including fees */
  TALER_amount_ntoh (&value,
                     &dki->issue.properties.value);
  TALER_amount_ntoh (&fee_withdraw,
                     &dki->issue.properties.fee_withdraw);

  if (GNUNET_OK !=
      TALER_amount_add (&amount_required,
                        &value,
                        &fee_withdraw))
  {
    TMH_plugin->rollback (TMH_plugin->cls,
                          session);
    return TMH_RESPONSE_reply_internal_db_error (connection);
  }

  /* calculate balance of the reserve */
  res = 0;
  for (pos = rh; NULL != pos; pos = pos->next)
  {
    switch (pos->type)
    {
    case TALER_EXCHANGEDB_RO_BANK_TO_EXCHANGE:
      if (0 == (res & 1))
        deposit_total = pos->details.bank->amount;
      else
        if (GNUNET_OK !=
            TALER_amount_add (&deposit_total,
                              &deposit_total,
                              &pos->details.bank->amount))
        {
          TMH_plugin->rollback (TMH_plugin->cls,
                                session);
          return TMH_RESPONSE_reply_internal_db_error (connection);
        }
      res |= 1;
      break;
    case TALER_EXCHANGEDB_RO_WITHDRAW_COIN:
      tdki = TMH_KS_denomination_key_lookup (key_state,
                                             &pos->details.withdraw->denom_pub,
					     TMH_KS_DKU_WITHDRAW);
      if (NULL == tdki)
      {
        GNUNET_break (0);
        TMH_plugin->rollback (TMH_plugin->cls,
                              session);
        return TMH_RESPONSE_reply_internal_db_error (connection);
      }
      TALER_amount_ntoh (&value,
                         &tdki->issue.properties.value);
      if (0 == (res & 2))
        withdraw_total = value;
      else
        if (GNUNET_OK !=
            TALER_amount_add (&withdraw_total,
                              &withdraw_total,
                              &value))
        {
          TMH_plugin->rollback (TMH_plugin->cls,
                                session);
          return TMH_RESPONSE_reply_internal_db_error (connection);
        }
      res |= 2;
      break;
    }
  }
  if (0 == (res & 1))
  {
    /* did not encounter any deposit operations, how can we have a reserve? */
    GNUNET_break (0);
    return TMH_RESPONSE_reply_internal_db_error (connection);
  }
  if (0 == (res & 2))
  {
    /* did not encounter any withdraw operations, set to zero */
    TALER_amount_get_zero (deposit_total.currency,
                           &withdraw_total);
  }
  /* All reserve balances should be non-negative */
  GNUNET_assert (GNUNET_SYSERR !=
                 TALER_amount_subtract (&balance,
                                        &deposit_total,
                                        &withdraw_total));
  if (0 < TALER_amount_cmp (&amount_required,
                            &balance))
  {
    TMH_plugin->rollback (TMH_plugin->cls,
                          session);
    res = TMH_RESPONSE_reply_reserve_withdraw_insufficient_funds (connection,
                                                                  rh);
    TMH_plugin->free_reserve_history (TMH_plugin->cls,
                                      rh);
    return res;
  }
  TMH_plugin->free_reserve_history (TMH_plugin->cls,
                                    rh);

  /* Balance is good, sign the coin! */
  denom_sig->rsa_signature
    = GNUNET_CRYPTO_rsa_sign_blinded (dki->denom_priv.rsa_private_key,
                                      blinded_msg,
                                      blinded_msg_len);
  if (NULL == denom_sig->rsa_signature)
  {
    GNUNET_break (0);
    TMH_plugin->rollback (TMH_plugin->cls,
                          session);
    return TMH_RESPONSE_reply_internal_error (connection,
                                              "Internal error");
  }
  collectable.sig = *denom_sig;
  collectable.denom_pub = *denomination_pub;
  collectable.amount_with_fee = amount_required;
  collectable.withdraw_fee = fee_withdraw;
  collectable.reserve_pub = *reserve;
  collectable.h_coin_envelope = *h_blind;
  collectable.reserve_sig = *signature;
  if (GNUNET_OK !=
      TMH_plugin->insert_withdraw_info (TMH_plugin->cls,
                                        session,
                                        &collectable))
  {
    GNUNET_break (0);
    TMH_plugin->rollback (TMH_plugin->cls,
                          session);
    return TMH_RESPONSE_reply_internal_db_error (connection);
  }
  COMMIT_TRANSACTION (session, connection);

  return TMH_RESPONSE_reply_reserve_withdraw_success (connection,
                                                      &collectable);
}



/**
 * Execute a "/reserve/withdraw". Given a reserve and a properly signed
 * request to withdraw a coin, check the balance of the reserve and
 * if it is sufficient, store the request and return the signed
 * blinded envelope.
 *
 * @param connection the MHD connection to handle
 * @param reserve public key of the reserve
 * @param denomination_pub public key of the denomination requested
 * @param blinded_msg blinded message to be signed
 * @param blinded_msg_len number of bytes in @a blinded_msg
 * @param signature signature over the withdraw request, to be stored in DB
 * @return MHD result code
 */
int
TMH_DB_execute_reserve_withdraw (struct MHD_Connection *connection,
                                 const struct TALER_ReservePublicKeyP *reserve,
                                 const struct TALER_DenominationPublicKey *denomination_pub,
                                 const char *blinded_msg,
                                 size_t blinded_msg_len,
                                 const struct TALER_ReserveSignatureP *signature)
{
  struct TALER_EXCHANGEDB_Session *session;
  struct TMH_KS_StateHandle *key_state;
  struct TALER_EXCHANGEDB_DenominationKeyIssueInformation *dki;
  struct TALER_EXCHANGEDB_CollectableBlindcoin collectable;
  struct TALER_DenominationSignature denom_sig;
  struct GNUNET_HashCode h_blind;
  int res;

  GNUNET_CRYPTO_hash (blinded_msg,
                      blinded_msg_len,
                      &h_blind);
  if (NULL == (session = TMH_plugin->get_session (TMH_plugin->cls)))
  {
    GNUNET_break (0);
    return TMH_RESPONSE_reply_internal_db_error (connection);
  }
  res = TMH_plugin->get_withdraw_info (TMH_plugin->cls,
                                       session,
                                       &h_blind,
                                       &collectable);
  if (GNUNET_SYSERR == res)
  {
    GNUNET_break (0);
    return TMH_RESPONSE_reply_internal_db_error (connection);
  }

  /* Don't sign again if we have already signed the coin */
  if (GNUNET_YES == res)
  {
    res = TMH_RESPONSE_reply_reserve_withdraw_success (connection,
                                                       &collectable);
    GNUNET_CRYPTO_rsa_signature_free (collectable.sig.rsa_signature);
    GNUNET_CRYPTO_rsa_public_key_free (collectable.denom_pub.rsa_public_key);
    return res;
  }
  GNUNET_assert (GNUNET_NO == res);

  key_state = TMH_KS_acquire ();
  dki = TMH_KS_denomination_key_lookup (key_state,
                                        denomination_pub,
					TMH_KS_DKU_WITHDRAW);
  if (NULL == dki)
  {
    TMH_KS_release (key_state);
    return TMH_RESPONSE_reply_json_pack (connection,
                                         MHD_HTTP_NOT_FOUND,
                                         "{s:s}",
                                         "error",
                                         "Denomination not found");
  }
  denom_sig.rsa_signature = NULL;
  res = execute_reserve_withdraw_transaction (connection,
                                              session,
                                              key_state,
                                              reserve,
                                              denomination_pub,
                                              dki,
                                              blinded_msg,
                                              blinded_msg_len,
                                              &h_blind,
                                              signature,
                                              &denom_sig);
  if (NULL != denom_sig.rsa_signature)
    GNUNET_CRYPTO_rsa_signature_free (denom_sig.rsa_signature);
  TMH_KS_release (key_state);
  return res;
}


/**
 * Parse coin melt requests from a JSON object and write them to
 * the database.
 *
 * @param connection the connection to send errors to
 * @param session the database connection
 * @param key_state the exchange's key state
 * @param session_hash hash identifying the refresh session
 * @param coin_details details about the coin being melted
 * @param[out] meltp on success, set to melt details
 * @return #GNUNET_OK on success,
 *         #GNUNET_NO if an error message was generated,
 *         #GNUNET_SYSERR on internal errors (no response generated)
 */
static int
refresh_check_melt (struct MHD_Connection *connection,
                    struct TALER_EXCHANGEDB_Session *session,
                    const struct TMH_KS_StateHandle *key_state,
                    const struct GNUNET_HashCode *session_hash,
                    const struct TMH_DB_MeltDetails *coin_details,
                    struct TALER_EXCHANGEDB_RefreshMelt *meltp)
{
  struct TALER_EXCHANGEDB_DenominationKeyIssueInformation *dk;
  struct TALER_EXCHANGEDB_DenominationKeyInformationP *dki;
  struct TALER_EXCHANGEDB_TransactionList *tl;
  struct TALER_Amount coin_value;
  struct TALER_Amount coin_residual;
  struct TALER_Amount spent;
  int res;

  dk = TMH_KS_denomination_key_lookup (key_state,
                                       &coin_details->coin_info.denom_pub,
                                       TMH_KS_DKU_DEPOSIT);
  if (NULL == dk)
    return (MHD_YES ==
            TMH_RESPONSE_reply_arg_unknown (connection,
                                            "denom_pub"))
        ? GNUNET_NO : GNUNET_SYSERR;
  dki = &dk->issue;
  TALER_amount_ntoh (&coin_value,
                     &dki->properties.value);
  /* fee for THIS transaction; the melt amount includes the fee! */
  spent = coin_details->melt_amount_with_fee;
  /* add historic transaction costs of this coin */
  tl = TMH_plugin->get_coin_transactions (TMH_plugin->cls,
                                          session,
                                          &coin_details->coin_info.coin_pub);
  if (GNUNET_OK !=
      calculate_transaction_list_totals (tl,
                                         &spent,
                                         &spent))
  {
    GNUNET_break (0);
    TMH_plugin->free_coin_transaction_list (TMH_plugin->cls,
                                            tl);
    return (MHD_YES ==
            TMH_RESPONSE_reply_internal_db_error (connection))
      ? GNUNET_NO : GNUNET_SYSERR;
  }
  /* Refuse to refresh when the coin's value is insufficient
     for the cost of all transactions. */
  if (TALER_amount_cmp (&coin_value,
                        &spent) < 0)
  {
    GNUNET_assert (GNUNET_OK ==
                   TALER_amount_subtract (&coin_residual,
                                          &spent,
                                          &coin_details->melt_amount_with_fee));
    res = (MHD_YES ==
           TMH_RESPONSE_reply_refresh_melt_insufficient_funds (connection,
                                                               &coin_details->coin_info.coin_pub,
                                                               coin_value,
                                                               tl,
                                                               coin_details->melt_amount_with_fee,
                                                               coin_residual))
        ? GNUNET_NO : GNUNET_SYSERR;
    TMH_plugin->free_coin_transaction_list (TMH_plugin->cls,
                                            tl);
    return res;
  }
  TMH_plugin->free_coin_transaction_list (TMH_plugin->cls,
                                          tl);

  meltp->coin = coin_details->coin_info;
  meltp->coin_sig = coin_details->melt_sig;
  meltp->session_hash = *session_hash;
  meltp->amount_with_fee = coin_details->melt_amount_with_fee;
  meltp->melt_fee = coin_details->melt_fee;
  return GNUNET_OK;
}


/**
 * Execute a "/refresh/melt".  We have been given a list of valid
 * coins and a request to melt them into the given
 * @a refresh_session_pub.  Check that the coins all have the
 * required value left and if so, store that they have been
 * melted and confirm the melting operation to the client.
 *
 * @param connection the MHD connection to handle
 * @param session_hash hash code of the session the coins are melted into
 * @param num_new_denoms number of entries in @a denom_pubs, size of y-dimension of @a commit_coin array
 * @param denom_pubs public keys of the coins we want to withdraw in the end
 * @param coin_melt_detail signature and (residual) value of the respective coin should be melted
 * @param commit_coin 2d array of coin commitments (what the exchange is to sign
 *                    once the "/refres/reveal" of cut and choose is done),
 *                    x-dimension must be #TALER_CNC_KAPPA
 * @param commit_link array of coin link commitments (what the exchange is
 *                    to return via "/refresh/link" to enable linkage in the
 *                    future) of length #TALER_CNC_KAPPA
 * @return MHD result code
 */
int
TMH_DB_execute_refresh_melt (struct MHD_Connection *connection,
                             const struct GNUNET_HashCode *session_hash,
                             unsigned int num_new_denoms,
                             const struct TALER_DenominationPublicKey *denom_pubs,
                             const struct TMH_DB_MeltDetails *coin_melt_detail,
                             struct TALER_EXCHANGEDB_RefreshCommitCoin *const* commit_coin,
                             const struct TALER_RefreshCommitLinkP *commit_link)
{
  struct TMH_KS_StateHandle *key_state;
  struct TALER_EXCHANGEDB_RefreshSession refresh_session;
  struct TALER_EXCHANGEDB_Session *session;
  int res;
  unsigned int i;

  if (NULL == (session = TMH_plugin->get_session (TMH_plugin->cls)))
  {
    GNUNET_break (0);
    return TMH_RESPONSE_reply_internal_db_error (connection);
  }
  START_TRANSACTION (session, connection);
  res = TMH_plugin->get_refresh_session (TMH_plugin->cls,
                                         session,
                                         session_hash,
                                         &refresh_session);
  if (GNUNET_YES == res)
  {
    TMH_plugin->rollback (TMH_plugin->cls,
                          session);
    res = TMH_RESPONSE_reply_refresh_melt_success (connection,
                                                   session_hash,
                                                   refresh_session.noreveal_index);
    return (GNUNET_SYSERR == res) ? MHD_NO : MHD_YES;
  }
  if (GNUNET_SYSERR == res)
  {
    TMH_plugin->rollback (TMH_plugin->cls,
                          session);
    return TMH_RESPONSE_reply_internal_db_error (connection);
  }

  /* store 'global' session data */
  refresh_session.num_newcoins = num_new_denoms;
  refresh_session.noreveal_index
      = GNUNET_CRYPTO_random_u32 (GNUNET_CRYPTO_QUALITY_STRONG,
                                  TALER_CNC_KAPPA);
  key_state = TMH_KS_acquire ();
  if (GNUNET_OK !=
      (res = refresh_check_melt (connection,
                                 session,
                                 key_state,
                                 session_hash,
                                 coin_melt_detail,
                                 &refresh_session.melt)))
  {
    TMH_KS_release (key_state);
    TMH_plugin->rollback (TMH_plugin->cls,
                          session);
    return (GNUNET_SYSERR == res) ? MHD_NO : MHD_YES;
  }
  TMH_KS_release (key_state);

  if (GNUNET_OK !=
      (res = TMH_plugin->create_refresh_session (TMH_plugin->cls,
                                                 session,
                                                 session_hash,
                                                 &refresh_session)))
  {
    TMH_plugin->rollback (TMH_plugin->cls,
                          session);
    return TMH_RESPONSE_reply_internal_db_error (connection);
  }

  /* store requested new denominations */
  if (GNUNET_OK !=
      TMH_plugin->insert_refresh_order (TMH_plugin->cls,
                                        session,
                                        session_hash,
                                        num_new_denoms,
                                        denom_pubs))
  {
    TMH_plugin->rollback (TMH_plugin->cls,
                          session);
    return TMH_RESPONSE_reply_internal_db_error (connection);
  }

  for (i = 0; i < TALER_CNC_KAPPA; i++)
  {
    if (GNUNET_OK !=
        TMH_plugin->insert_refresh_commit_coins (TMH_plugin->cls,
                                                 session,
                                                 session_hash,
                                                 i,
                                                 num_new_denoms,
                                                 commit_coin[i]))
    {
      TMH_plugin->rollback (TMH_plugin->cls,
                            session);
      return TMH_RESPONSE_reply_internal_db_error (connection);
    }
  }
  for (i = 0; i < TALER_CNC_KAPPA; i++)
  {
    if (GNUNET_OK !=
        TMH_plugin->insert_refresh_commit_link (TMH_plugin->cls,
                                                session,
                                                session_hash,
                                                i,
                                                &commit_link[i]))
    {
      TMH_plugin->rollback (TMH_plugin->cls,
                            session);
      return TMH_RESPONSE_reply_internal_db_error (connection);
    }
  }

  COMMIT_TRANSACTION (session, connection);
  return TMH_RESPONSE_reply_refresh_melt_success (connection,
                                                  session_hash,
                                                  refresh_session.noreveal_index);
}


/**
 * Send an error response with the details of the original melt
 * commitment and the location of the mismatch.
 *
 * @param connection the MHD connection to handle
 * @param session database connection to use
 * @param session_hash hash of session to query
 * @param rm details about the original melt
 * @param off commitment offset to check
 * @param index index of the mismatch, UINT_MAX if there is no such index
 * @param object_name name of the object with the problem
 * @return #GNUNET_NO if we generated the error message
 *         #GNUNET_SYSERR if we could not even generate an error message
 */
static int
send_melt_commitment_error (struct MHD_Connection *connection,
                            struct TALER_EXCHANGEDB_Session *session,
                            const struct GNUNET_HashCode *session_hash,
                            const struct TALER_EXCHANGEDB_RefreshMelt *rm,
                            unsigned int off,
                            unsigned int index,
                            const char *object_name)
{
  struct TALER_EXCHANGEDB_MeltCommitment *mc;
  int ret;

  mc = TMH_plugin->get_melt_commitment (TMH_plugin->cls,
                                        session,
                                        session_hash);
  if (NULL == mc)
  {
    GNUNET_break (0);
    return (MHD_YES ==
            TMH_RESPONSE_reply_internal_error (connection,
                                               "Melt commitment assembly"))
      ? GNUNET_NO : GNUNET_SYSERR;
  }
  ret = (MHD_YES ==
         TMH_RESPONSE_reply_refresh_reveal_missmatch (connection,
                                                      rm,
                                                      mc,
                                                      off,
                                                      index,
                                                      object_name))
    ? GNUNET_NO : GNUNET_SYSERR;
  TMH_plugin->free_melt_commitment (TMH_plugin->cls,
                                    mc);
  return ret;
}


/**
 * Check if the given @a transfer_privs correspond to an honest
 * commitment for the given session.
 * Checks that the transfer private keys match their commitments.
 * Then derives the shared secret for each #TALER_CNC_KAPPA, and check that they match.
 *
 * @param connection the MHD connection to handle
 * @param session database connection to use
 * @param session_hash hash of session to query
 * @param off commitment offset to check
 * @param transfer_priv private transfer key
 * @param melt information about the melted coin
 * @param num_newcoins number of newcoins being generated
 * @param denom_pubs array of @a num_newcoins keys for the new coins
 * @return #GNUNET_OK if the committment was honest,
 *         #GNUNET_NO if there was a problem and we generated an error message
 *         #GNUNET_SYSERR if we could not even generate an error message
 */
static int
check_commitment (struct MHD_Connection *connection,
                  struct TALER_EXCHANGEDB_Session *session,
                  const struct GNUNET_HashCode *session_hash,
                  unsigned int off,
                  const struct TALER_TransferPrivateKeyP *transfer_priv,
                  const struct TALER_EXCHANGEDB_RefreshMelt *melt,
                  unsigned int num_newcoins,
                  const struct TALER_DenominationPublicKey *denom_pubs)
{
  struct TALER_RefreshCommitLinkP commit_link;
  struct TALER_LinkSecretP shared_secret;
  struct TALER_TransferPublicKeyP transfer_pub_check;
  struct TALER_EXCHANGEDB_RefreshCommitCoin *commit_coins;
  unsigned int j;
  int ret;

  if (GNUNET_OK !=
      TMH_plugin->get_refresh_commit_link (TMH_plugin->cls,
                                           session,
                                           session_hash,
                                           off,
                                           &commit_link))
  {
    GNUNET_break (0);
    return (MHD_YES == TMH_RESPONSE_reply_internal_db_error (connection))
        ? GNUNET_NO : GNUNET_SYSERR;
  }

  GNUNET_CRYPTO_ecdhe_key_get_public (&transfer_priv->ecdhe_priv,
                                      &transfer_pub_check.ecdhe_pub);
  if (0 !=
      memcmp (&transfer_pub_check,
              &commit_link.transfer_pub,
              sizeof (struct TALER_TransferPublicKeyP)))
  {
    GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                "transfer keys do not match\n");
    return send_melt_commitment_error (connection,
                                       session,
                                       session_hash,
                                       melt,
                                       off,
                                       UINT_MAX,
                                       "transfer key");
  }

  if (GNUNET_OK !=
      TALER_link_decrypt_secret (&commit_link.shared_secret_enc,
                                 transfer_priv,
                                 &melt->coin.coin_pub,
                                 &shared_secret))
  {
    return (MHD_YES ==
            TMH_RESPONSE_reply_internal_error (connection,
                                               "Transfer secret decryption error"))
      ? GNUNET_NO : GNUNET_SYSERR;
  }

  /* Check that the commitments for all new coins were correct */
  commit_coins = GNUNET_new_array (num_newcoins,
                                   struct TALER_EXCHANGEDB_RefreshCommitCoin);

  if (GNUNET_OK !=
      TMH_plugin->get_refresh_commit_coins (TMH_plugin->cls,
                                            session,
                                            session_hash,
                                            off,
                                            num_newcoins,
                                            commit_coins))
  {
    GNUNET_break (0);
    GNUNET_free (commit_coins);
    return (MHD_YES == TMH_RESPONSE_reply_internal_db_error (connection))
        ? GNUNET_NO : GNUNET_SYSERR;
  }

  for (j = 0; j < num_newcoins; j++)
  {
    struct TALER_RefreshLinkDecryptedP link_data;
    struct TALER_CoinSpendPublicKeyP coin_pub;
    struct GNUNET_HashCode h_msg;
    char *buf;
    size_t buf_len;

    TALER_refresh_decrypt (&commit_coins[j].refresh_link,
			   &shared_secret,
			   &link_data);
    GNUNET_CRYPTO_eddsa_key_get_public (&link_data.coin_priv.eddsa_priv,
                                        &coin_pub.eddsa_pub);
    GNUNET_CRYPTO_hash (&coin_pub,
                        sizeof (struct TALER_CoinSpendPublicKeyP),
                        &h_msg);
    if (GNUNET_YES !=
        GNUNET_CRYPTO_rsa_blind (&h_msg,
                                 &link_data.blinding_key.bks,
                                 denom_pubs[j].rsa_public_key,
                                 &buf,
                                 &buf_len))
    {
      GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                  "Blind failed (bad denomination key!?)\n");
      ret = (MHD_YES == TMH_RESPONSE_reply_internal_error (connection,
                                                            "Blinding error"))
          ? GNUNET_NO : GNUNET_SYSERR;
      goto cleanup;
    }

    if ( (buf_len != commit_coins[j].coin_ev_size) ||
         (0 != memcmp (buf,
                       commit_coins[j].coin_ev,
                       buf_len)) )
    {
      GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                  "blind envelope does not match for k=%u, old=%d\n",
                  off,
                  (int) j);
      GNUNET_free (buf);
      ret = send_melt_commitment_error (connection,
                                        session,
                                        session_hash,
                                        melt,
                                        off,
                                        j,
                                        "envelope");
      goto cleanup;
    }
    GNUNET_free (buf);
  }
  ret = GNUNET_OK;

 cleanup:
  for (j = 0; j < num_newcoins; j++)
    GNUNET_free (commit_coins[j].coin_ev);
  GNUNET_free (commit_coins);
  return ret;
}


/**
 * Exchange a coin as part of a refresh operation.  Obtains the
 * envelope from the database and performs the signing operation.
 *
 * @param connection the MHD connection to handle
 * @param session database connection to use
 * @param session_hash hash of session to query
 * @param key_state key state to lookup denomination pubs
 * @param denom_pub denomination key for the coin to create
 * @param commit_coin the coin that was committed
 * @param coin_off number of the coin
 * @return NULL on error, otherwise signature over the coin
 */
static struct TALER_DenominationSignature
refresh_exchange_coin (struct MHD_Connection *connection,
                       struct TALER_EXCHANGEDB_Session *session,
                       const struct GNUNET_HashCode *session_hash,
                       struct TMH_KS_StateHandle *key_state,
                       const struct TALER_DenominationPublicKey *denom_pub,
                       const struct TALER_EXCHANGEDB_RefreshCommitCoin *commit_coin,
                       unsigned int coin_off)
{
  struct TALER_EXCHANGEDB_DenominationKeyIssueInformation *dki;
  struct TALER_DenominationSignature ev_sig;

  dki = TMH_KS_denomination_key_lookup (key_state,
                                        denom_pub,
					TMH_KS_DKU_WITHDRAW);
  if (NULL == dki)
  {
    GNUNET_break (0);
    ev_sig.rsa_signature = NULL;
    return ev_sig;
  }
  ev_sig.rsa_signature
      = GNUNET_CRYPTO_rsa_sign_blinded (dki->denom_priv.rsa_private_key,
                                        commit_coin->coin_ev,
                                        commit_coin->coin_ev_size);
  if (NULL == ev_sig.rsa_signature)
  {
    GNUNET_break (0);
    return ev_sig;
  }
  if (GNUNET_OK !=
      TMH_plugin->insert_refresh_out (TMH_plugin->cls,
                                      session,
                                      session_hash,
                                      coin_off,
                                      &ev_sig))
  {
    GNUNET_break (0);
    GNUNET_CRYPTO_rsa_signature_free (ev_sig.rsa_signature);
    ev_sig.rsa_signature = NULL;
  }
  return ev_sig;
}


/**
 * The client request was well-formed, now execute the DB transaction
 * of a "/refresh/reveal" operation.  We use the @a ev_sigs and
 * @a commit_coins to clean up resources after this function returns
 * as we might experience retries of the database transaction.
 *
 * @param connection the MHD connection to handle
 * @param session database session
 * @param session_hash hash identifying the refresh session
 * @param refresh_session information about the refresh operation we are doing
 * @param denom_pubs array of "num_newcoins" denomination keys for the new coins
 * @param[out] ev_sigs where to store generated signatures for the new coins,
 *                     array of length "num_newcoins", memory released by the
 *                     caller
 * @param[out] commit_coins array of length "num_newcoins" to be used for
 *                     information about the new coins from the commitment.
 * @return MHD result code
 */
static int
execute_refresh_reveal_transaction (struct MHD_Connection *connection,
                                    struct TALER_EXCHANGEDB_Session *session,
                                    const struct GNUNET_HashCode *session_hash,
                                    const struct TALER_EXCHANGEDB_RefreshSession *refresh_session,
                                    const struct TALER_DenominationPublicKey *denom_pubs,
                                    struct TALER_DenominationSignature *ev_sigs,
                                    struct TALER_EXCHANGEDB_RefreshCommitCoin *commit_coins)
{
  unsigned int j;
  struct TMH_KS_StateHandle *key_state;
  int ret;

  START_TRANSACTION (session, connection);
  if (GNUNET_OK !=
      TMH_plugin->get_refresh_commit_coins (TMH_plugin->cls,
                                            session,
                                            session_hash,
                                            refresh_session->noreveal_index,
                                            refresh_session->num_newcoins,
                                            commit_coins))
  {
    GNUNET_break (0);
    return TMH_RESPONSE_reply_internal_db_error (connection);
  }
  key_state = TMH_KS_acquire ();
  for (j=0;j<refresh_session->num_newcoins;j++)
  {
    if (NULL == ev_sigs[j].rsa_signature) /* could be non-NULL during retries */
      ev_sigs[j] = refresh_exchange_coin (connection,
                                          session,
                                          session_hash,
                                          key_state,
                                          &denom_pubs[j],
                                          &commit_coins[j],
                                          j);
    if (NULL == ev_sigs[j].rsa_signature)
    {
      ret = TMH_RESPONSE_reply_internal_db_error (connection);
      goto cleanup;
    }
  }
  COMMIT_TRANSACTION (session, connection);
  ret = TMH_RESPONSE_reply_refresh_reveal_success (connection,
                                                   refresh_session->num_newcoins,
                                                   ev_sigs);
 cleanup:
  TMH_KS_release (key_state);
  return ret;
}


/**
 * Execute a "/refresh/reveal".  The client is revealing to us the
 * transfer keys for @a #TALER_CNC_KAPPA-1 sets of coins.  Verify that the
 * revealed transfer keys would allow linkage to the blinded coins,
 * and if so, return the signed coins for corresponding to the set of
 * coins that was not chosen.
 *
 * @param connection the MHD connection to handle
 * @param session_hash hash identifying the refresh session
 * @param transfer_privs array with the revealed transfer keys,
 *                      length must be #TALER_CNC_KAPPA - 1
 * @return MHD result code
 */
int
TMH_DB_execute_refresh_reveal (struct MHD_Connection *connection,
                               const struct GNUNET_HashCode *session_hash,
                               struct TALER_TransferPrivateKeyP *transfer_privs)
{
  int res;
  struct TALER_EXCHANGEDB_Session *session;
  struct TALER_EXCHANGEDB_RefreshSession refresh_session;
  struct TALER_DenominationPublicKey *denom_pubs;
  struct TALER_DenominationSignature *ev_sigs;
  struct TALER_EXCHANGEDB_RefreshCommitCoin *commit_coins;
  unsigned int i;
  unsigned int j;
  unsigned int off;

  if (NULL == (session = TMH_plugin->get_session (TMH_plugin->cls)))
  {
    GNUNET_break (0);
    return TMH_RESPONSE_reply_internal_db_error (connection);
  }

  res = TMH_plugin->get_refresh_session (TMH_plugin->cls,
                                         session,
                                         session_hash,
                                         &refresh_session);
  if (GNUNET_NO == res)
    return TMH_RESPONSE_reply_arg_invalid (connection,
                                           "session_hash");
  if (GNUNET_SYSERR == res)
    return TMH_RESPONSE_reply_internal_db_error (connection);
  denom_pubs = GNUNET_new_array (refresh_session.num_newcoins,
                                 struct TALER_DenominationPublicKey);
  if (GNUNET_OK !=
      TMH_plugin->get_refresh_order (TMH_plugin->cls,
                                     session,
                                     session_hash,
                                     refresh_session.num_newcoins,
                                     denom_pubs))
  {
    GNUNET_break (0);
    GNUNET_free (denom_pubs);
    GNUNET_CRYPTO_rsa_signature_free (refresh_session.melt.coin.denom_sig.rsa_signature);
    GNUNET_CRYPTO_rsa_public_key_free (refresh_session.melt.coin.denom_pub.rsa_public_key);
    return (MHD_YES == TMH_RESPONSE_reply_internal_db_error (connection))
        ? GNUNET_NO : GNUNET_SYSERR;
  }


  off = 0;
  for (i=0;i<TALER_CNC_KAPPA - 1;i++)
  {
    if (i == refresh_session.noreveal_index)
      off = 1;
    if (GNUNET_OK !=
        (res = check_commitment (connection,
                                 session,
                                 session_hash,
                                 i + off,
                                 &transfer_privs[i],
                                 &refresh_session.melt,
                                 refresh_session.num_newcoins,
                                 denom_pubs)))
    {
      for (j=0;j<refresh_session.num_newcoins;j++)
        GNUNET_CRYPTO_rsa_public_key_free (denom_pubs[j].rsa_public_key);
      GNUNET_free (denom_pubs);
      GNUNET_CRYPTO_rsa_signature_free (refresh_session.melt.coin.denom_sig.rsa_signature);
      GNUNET_CRYPTO_rsa_public_key_free (refresh_session.melt.coin.denom_pub.rsa_public_key);
      return (GNUNET_NO == res) ? MHD_YES : MHD_NO;
    }
  }

  /* Client request OK, start transaction */
  commit_coins = GNUNET_new_array (refresh_session.num_newcoins,
                                   struct TALER_EXCHANGEDB_RefreshCommitCoin);
  ev_sigs = GNUNET_new_array (refresh_session.num_newcoins,
                              struct TALER_DenominationSignature);
  res = execute_refresh_reveal_transaction (connection,
                                            session,
                                            session_hash,
                                            &refresh_session,
                                            denom_pubs,
                                            ev_sigs,
                                            commit_coins);
  for (i=0;i<refresh_session.num_newcoins;i++)
  {
    if (NULL != ev_sigs[i].rsa_signature)
      GNUNET_CRYPTO_rsa_signature_free (ev_sigs[i].rsa_signature);
    GNUNET_free (commit_coins[i].coin_ev);
  }
  for (j=0;j<refresh_session.num_newcoins;j++)
    if (NULL != denom_pubs[j].rsa_public_key)
      GNUNET_CRYPTO_rsa_public_key_free (denom_pubs[j].rsa_public_key);
  GNUNET_CRYPTO_rsa_signature_free (refresh_session.melt.coin.denom_sig.rsa_signature);
  GNUNET_CRYPTO_rsa_public_key_free (refresh_session.melt.coin.denom_pub.rsa_public_key);
  GNUNET_free (ev_sigs);
  GNUNET_free (denom_pubs);
  GNUNET_free (commit_coins);
  return res;
}


/**
 * Closure for #handle_transfer_data().
 */
struct HTD_Context
{

  /**
   * Session link data we collect.
   */
  struct TMH_RESPONSE_LinkSessionInfo *sessions;

  /**
   * Database session. Nothing to do with @a sessions.
   */
  struct TALER_EXCHANGEDB_Session *session;

  /**
   * MHD connection, for queueing replies.
   */
  struct MHD_Connection *connection;

  /**
   * Number of sessions the coin was melted into.
   */
  unsigned int num_sessions;

  /**
   * How are we expected to proceed. #GNUNET_SYSERR if we
   * failed to return an error (should return #MHD_NO).
   * #GNUNET_NO if we succeeded in queueing an MHD error
   * (should return #MHD_YES from #TMH_execute_refresh_link),
   * #GNUNET_OK if we should call #TMH_RESPONSE_reply_refresh_link_success().
   */
  int status;
};


/**
 * Function called with the session hashes and transfer secret
 * information for a given coin.  Gets the linkage data and
 * builds the reply for the client.
 *
 *
 * @param cls closure, a `struct HTD_Context`
 * @param session_hash a session the coin was melted in
 * @param transfer_pub public transfer key for the session
 * @param shared_secret_enc set to shared secret for the session
 */
static void
handle_transfer_data (void *cls,
                      const struct GNUNET_HashCode *session_hash,
                      const struct TALER_TransferPublicKeyP *transfer_pub,
                      const struct TALER_EncryptedLinkSecretP *shared_secret_enc)
{
  struct HTD_Context *ctx = cls;
  struct TALER_EXCHANGEDB_LinkDataList *ldl;
  struct TMH_RESPONSE_LinkSessionInfo *lsi;

  if (GNUNET_OK != ctx->status)
    return;
  ldl = TMH_plugin->get_link_data_list (TMH_plugin->cls,
                                        ctx->session,
                                        session_hash);
  if (NULL == ldl)
  {
    ctx->status = GNUNET_NO;
    if (MHD_NO ==
        TMH_RESPONSE_reply_json_pack (ctx->connection,
                                      MHD_HTTP_NOT_FOUND,
                                      "{s:s}",
                                      "error",
                                      "link data not found (link)"))
      ctx->status = GNUNET_SYSERR;
    return;
  }
  GNUNET_array_grow (ctx->sessions,
                     ctx->num_sessions,
                     ctx->num_sessions + 1);
  lsi = &ctx->sessions[ctx->num_sessions - 1];
  lsi->transfer_pub = *transfer_pub;
  lsi->shared_secret_enc = *shared_secret_enc;
  lsi->ldl = ldl;
}


/**
 * Execute a "/refresh/link".  Returns the linkage information that
 * will allow the owner of a coin to follow the refresh trail to
 * the refreshed coin.
 *
 * @param connection the MHD connection to handle
 * @param coin_pub public key of the coin to link
 * @return MHD result code
 */
int
TMH_DB_execute_refresh_link (struct MHD_Connection *connection,
                             const struct TALER_CoinSpendPublicKeyP *coin_pub)
{
  struct HTD_Context ctx;
  int res;
  unsigned int i;

  if (NULL == (ctx.session = TMH_plugin->get_session (TMH_plugin->cls)))
  {
    GNUNET_break (0);
    return TMH_RESPONSE_reply_internal_db_error (connection);
  }
  ctx.connection = connection;
  ctx.num_sessions = 0;
  ctx.sessions = NULL;
  ctx.status = GNUNET_OK;
  res = TMH_plugin->get_transfer (TMH_plugin->cls,
                                  ctx.session,
                                  coin_pub,
                                  &handle_transfer_data,
                                  &ctx);
  if (GNUNET_SYSERR == ctx.status)
  {
    res = MHD_NO;
    goto cleanup;
  }
  if (GNUNET_NO == ctx.status)
  {
    res = MHD_YES;
    goto cleanup;
  }
  GNUNET_assert (GNUNET_OK == ctx.status);
  if (0 == ctx.num_sessions)
    return TMH_RESPONSE_reply_arg_unknown (connection,
                                           "coin_pub");
  res = TMH_RESPONSE_reply_refresh_link_success (connection,
                                                 ctx.num_sessions,
                                                 ctx.sessions);
 cleanup:
  for (i=0;i<ctx.num_sessions;i++)
    TMH_plugin->free_link_data_list (TMH_plugin->cls,
                                     ctx.sessions[i].ldl);
  GNUNET_free_non_null (ctx.sessions);
  return res;
}


/**
 * Add an incoming transaction to the database.  Checks if the
 * transaction is fresh (not a duplicate) and if so adds it to
 * the database.
 *
 * @param connection the MHD connection to handle
 * @param reserve_pub public key of the reserve
 * @param amount amount to add to the reserve
 * @param execution_time when did we receive the wire transfer
 * @param sender_account_details which account send the funds
 * @param transfer_details information that uniquely identifies the transfer
 * @return MHD result code
 */
int
TMH_DB_execute_admin_add_incoming (struct MHD_Connection *connection,
                                   const struct TALER_ReservePublicKeyP *reserve_pub,
                                   const struct TALER_Amount *amount,
                                   struct GNUNET_TIME_Absolute execution_time,
                                   const json_t *sender_account_details,
                                   const json_t *transfer_details)
{
  struct TALER_EXCHANGEDB_Session *session;
  int ret;

  if (NULL == (session = TMH_plugin->get_session (TMH_plugin->cls)))
  {
    GNUNET_break (0);
    return TMH_RESPONSE_reply_internal_db_error (connection);
  }
  ret = TMH_plugin->reserves_in_insert (TMH_plugin->cls,
                                        session,
                                        reserve_pub,
                                        amount,
                                        execution_time,
                                        sender_account_details,
                                        transfer_details);
  if (GNUNET_SYSERR == ret)
  {
    GNUNET_break (0);
    return TMH_RESPONSE_reply_internal_db_error (connection);
  }
  return TMH_RESPONSE_reply_json_pack (connection,
                                       MHD_HTTP_OK,
                                       "{s:s}",
                                       "status",
                                       (GNUNET_OK == ret)
                                       ? "NEW"
                                       : "DUP");
}


/**
 * Closure for #handle_transaction_data.
 */
struct WtidTransactionContext
{

  /**
   * Total amount of the wire transfer, as calculated by
   * summing up the individual amounts. To be rounded down
   * to calculate the real transfer amount at the end.
   * Only valid if @e is_valid is #GNUNET_YES.
   */
  struct TALER_Amount total;

  /**
   * Public key of the merchant, only valid if @e is_valid
   * is #GNUNET_YES.
   */
  struct TALER_MerchantPublicKeyP merchant_pub;

  /**
   * Hash of the wire details of the merchant (identical for all
   * deposits), only valid if @e is_valid is #GNUNET_YES.
   */
  struct GNUNET_HashCode h_wire;

  /**
   * Head of DLL with details for /wire/deposit response.
   */
  struct TMH_TrackTransferDetail *wdd_head;

  /**
   * Head of DLL with details for /wire/deposit response.
   */
  struct TMH_TrackTransferDetail *wdd_tail;

  /**
   * JSON array with details about the individual deposits.
   */
  json_t *deposits;

  /**
   * Initially #GNUNET_NO, if we found no deposits so far.  Set to
   * #GNUNET_YES if we got transaction data, and the database replies
   * remained consistent with respect to @e merchant_pub and @e h_wire
   * (as they should).  Set to #GNUNET_SYSERR if we encountered an
   * internal error.
   */
  int is_valid;

};


/**
 * Function called with the results of the lookup of the
 * transaction data for the given wire transfer identifier.
 *
 * @param cls our context for transmission
 * @param merchant_pub public key of the merchant (should be same for all callbacks with the same @e cls)
 * @param h_wire hash of wire transfer details of the merchant (should be same for all callbacks with the same @e cls)
 * @param h_contract which contract was this payment about
 * @param transaction_id merchant's transaction ID for the payment
 * @param coin_pub which public key was this payment about
 * @param deposit_value amount contributed by this coin in total
 * @param deposit_fee deposit fee charged by exchange for this coin
 */
static void
handle_transaction_data (void *cls,
                         const struct TALER_MerchantPublicKeyP *merchant_pub,
                         const struct GNUNET_HashCode *h_wire,
                         const struct GNUNET_HashCode *h_contract,
                         uint64_t transaction_id,
                         const struct TALER_CoinSpendPublicKeyP *coin_pub,
                         const struct TALER_Amount *deposit_value,
                         const struct TALER_Amount *deposit_fee)
{
  struct WtidTransactionContext *ctx = cls;
  struct TALER_Amount delta;
  struct TMH_TrackTransferDetail *wdd;

  if (GNUNET_SYSERR == ctx->is_valid)
    return;
  if (GNUNET_NO == ctx->is_valid)
  {
    ctx->merchant_pub = *merchant_pub;
    ctx->h_wire = *h_wire;
    ctx->is_valid = GNUNET_YES;
    if (GNUNET_OK !=
        TALER_amount_subtract (&ctx->total,
                               deposit_value,
                               deposit_fee))
    {
      GNUNET_break (0);
      ctx->is_valid = GNUNET_SYSERR;
      return;
    }
  }
  else
  {
    if ( (0 != memcmp (&ctx->merchant_pub,
                       merchant_pub,
                       sizeof (struct TALER_MerchantPublicKeyP))) ||
         (0 != memcmp (&ctx->h_wire,
                       h_wire,
                       sizeof (struct GNUNET_HashCode))) )
    {
      GNUNET_break (0);
      ctx->is_valid = GNUNET_SYSERR;
      return;
    }
    if (GNUNET_OK !=
        TALER_amount_subtract (&delta,
                               deposit_value,
                               deposit_fee))
    {
      GNUNET_break (0);
      ctx->is_valid = GNUNET_SYSERR;
      return;
    }
    if (GNUNET_OK !=
        TALER_amount_add (&ctx->total,
                          &ctx->total,
                          &delta))
    {
      GNUNET_break (0);
      ctx->is_valid = GNUNET_SYSERR;
      return;
    }
  }
  wdd = GNUNET_new (struct TMH_TrackTransferDetail);
  wdd->deposit_value = *deposit_value;
  wdd->deposit_fee = *deposit_fee;
  wdd->h_contract = *h_contract;
  wdd->transaction_id = transaction_id;
  wdd->coin_pub = *coin_pub;
  GNUNET_CONTAINER_DLL_insert (ctx->wdd_head,
                               ctx->wdd_tail,
                               wdd);
}


/**
 * Execute a "/track/transfer".  Returns the transaction information
 * associated with the given wire transfer identifier.
 *
 * @param connection the MHD connection to handle
 * @param wtid wire transfer identifier to resolve
 * @return MHD result code
 */
int
TMH_DB_execute_track_transfer (struct MHD_Connection *connection,
                             const struct TALER_WireTransferIdentifierRawP *wtid)
{
  int ret;
  struct WtidTransactionContext ctx;
  struct TALER_EXCHANGEDB_Session *session;
  struct TMH_TrackTransferDetail *wdd;

  if (NULL == (session = TMH_plugin->get_session (TMH_plugin->cls)))
  {
    GNUNET_break (0);
    return TMH_RESPONSE_reply_internal_db_error (connection);
  }
  ctx.is_valid = GNUNET_NO;
  ctx.wdd_head = NULL;
  ctx.wdd_tail = NULL;
  ret = TMH_plugin->lookup_wire_transfer (TMH_plugin->cls,
                                          session,
                                          wtid,
                                          &handle_transaction_data,
                                          &ctx);
  if (GNUNET_SYSERR == ret)
  {
    GNUNET_break (0);
    ret = TMH_RESPONSE_reply_internal_db_error (connection);
    goto cleanup;
  }
  if (GNUNET_SYSERR == ctx.is_valid)
  {
    GNUNET_break (0);
    ret = TMH_RESPONSE_reply_internal_db_error (connection);
    goto cleanup;
  }
  if (GNUNET_NO == ctx.is_valid)
  {
    ret = TMH_RESPONSE_reply_arg_unknown (connection,
                                          "wtid");
    goto cleanup;
  }
  ret = TMH_RESPONSE_reply_track_transfer_details (connection,
                                                 &ctx.total,
                                                 &ctx.merchant_pub,
                                                 &ctx.h_wire,
                                                 ctx.wdd_head);
 cleanup:
  while (NULL != (wdd = ctx.wdd_head))
  {
    GNUNET_CONTAINER_DLL_remove (ctx.wdd_head,
                                 ctx.wdd_tail,
                                 wdd);
    GNUNET_free (wdd);
  }
  return ret;
}


/**
 * Closure for #handle_wtid_data.
 */
struct DepositWtidContext
{

  /**
   * Where should we send the reply?
   */
  struct MHD_Connection *connection;

  /**
   * Hash of the contract we are looking up.
   */
  struct GNUNET_HashCode h_contract;

  /**
   * Hash of the wire transfer details we are looking up.
   */
  struct GNUNET_HashCode h_wire;

  /**
   * Public key we are looking up.
   */
  struct TALER_CoinSpendPublicKeyP coin_pub;

  /**
   * Transaction ID we are looking up.
   */
  uint64_t transaction_id;

  /**
   * MHD result code to return.
   */
  int res;
};


/**
 * Function called with the results of the lookup of the
 * wire transfer identifier information.
 *
 * @param cls our context for transmission
 * @param wtid raw wire transfer identifier, NULL
 *         if the transaction was not yet done
 * @param coin_contribution how much did the coin we asked about
 *        contribute to the total transfer value? (deposit value including fee)
 * @param coin_fee how much did the exchange charge for the deposit fee
 * @param execution_time when was the transaction done, or
 *         when we expect it to be done (if @a wtid was NULL);
 *         #GNUNET_TIME_UNIT_FOREVER_ABS if the /deposit is unknown
 *         to the exchange
 */
static void
handle_wtid_data (void *cls,
		  const struct TALER_WireTransferIdentifierRawP *wtid,
                  const struct TALER_Amount *coin_contribution,
                  const struct TALER_Amount *coin_fee,
		  struct GNUNET_TIME_Absolute execution_time)
{
  struct DepositWtidContext *ctx = cls;
  struct TALER_Amount coin_delta;

  if (NULL == wtid)
  {
    ctx->res = TMH_RESPONSE_reply_transfer_pending (ctx->connection,
                                                    execution_time);
  }
  else
  {
    if (GNUNET_SYSERR ==
        TALER_amount_subtract (&coin_delta,
                               coin_contribution,
                               coin_fee))
    {
      GNUNET_break (0);
      ctx->res = TMH_RESPONSE_reply_internal_db_error (ctx->connection);
    }
    else
    {
      ctx->res = TMH_RESPONSE_reply_track_transaction (ctx->connection,
                                                  &ctx->h_contract,
                                                  &ctx->h_wire,
                                                  &ctx->coin_pub,
                                                  &coin_delta,
                                                  ctx->transaction_id,
                                                  wtid,
                                                  execution_time);
    }
  }
}


/**
 * Execute a "/track/transaction".  Returns the transfer information
 * associated with the given deposit.
 *
 * @param connection the MHD connection to handle
 * @param h_contract hash of the contract
 * @param h_wire hash of the wire details
 * @param coin_pub public key of the coin to link
 * @param merchant_pub public key of the merchant
 * @param transaction_id transaction ID of the merchant
 * @return MHD result code
 */
int
TMH_DB_execute_track_transaction (struct MHD_Connection *connection,
                                  const struct GNUNET_HashCode *h_contract,
                                  const struct GNUNET_HashCode *h_wire,
                                  const struct TALER_CoinSpendPublicKeyP *coin_pub,
                                  const struct TALER_MerchantPublicKeyP *merchant_pub,
                                  uint64_t transaction_id)
{
  int ret;
  struct DepositWtidContext ctx;
  struct TALER_EXCHANGEDB_Session *session;

  if (NULL == (session = TMH_plugin->get_session (TMH_plugin->cls)))
  {
    GNUNET_break (0);
    return TMH_RESPONSE_reply_internal_db_error (connection);
  }
  ctx.connection = connection;
  ctx.h_contract = *h_contract;
  ctx.h_wire = *h_wire;
  ctx.coin_pub = *coin_pub;
  ctx.transaction_id = transaction_id;
  ctx.res = GNUNET_SYSERR;
  ret = TMH_plugin->wire_lookup_deposit_wtid (TMH_plugin->cls,
                                              session,
					      h_contract,
					      h_wire,
					      coin_pub,
					      merchant_pub,
					      transaction_id,
					      &handle_wtid_data,
					      &ctx);
  if (GNUNET_SYSERR == ret)
  {
    GNUNET_break (0);
    GNUNET_break (GNUNET_SYSERR == ctx.res);
    return TMH_RESPONSE_reply_internal_db_error (connection);
  }
  if (GNUNET_NO == ret)
  {
    GNUNET_break (GNUNET_SYSERR == ctx.res);
    return TMH_RESPONSE_reply_transaction_unknown (connection);
  }
  if (GNUNET_SYSERR == ctx.res)
  {
    GNUNET_break (0);
    return TMH_RESPONSE_reply_internal_error (connection,
                                              "bug resolving deposit wtid");
  }
  return ctx.res;
}


/* end of taler-exchange-httpd_db.c */
