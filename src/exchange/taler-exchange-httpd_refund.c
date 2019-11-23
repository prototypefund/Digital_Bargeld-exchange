/*
  This file is part of TALER
  Copyright (C) 2014-2017 Inria and GNUnet e.V.

  TALER is free software; you can redistribute it and/or modify it under the
  terms of the GNU Affero General Public License as published by the Free Software
  Foundation; either version 3, or (at your option) any later version.

  TALER is distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
  A PARTICULAR PURPOSE.  See the GNU Affero General Public License for more details.

  You should have received a copy of the GNU Affero General Public License along with
  TALER; see the file COPYING.  If not, see <http://www.gnu.org/licenses/>
*/
/**
 * @file taler-exchange-httpd_refund.c
 * @brief Handle /refund requests; parses the POST and JSON and
 *        verifies the coin signature before handing things off
 *        to the database.
 * @author Florian Dold
 * @author Benedikt Mueller
 * @author Christian Grothoff
 */
#include "platform.h"
#include <gnunet/gnunet_util_lib.h>
#include <gnunet/gnunet_json_lib.h>
#include <jansson.h>
#include <microhttpd.h>
#include <pthread.h>
#include "taler_json_lib.h"
#include "taler_mhd_lib.h"
#include "taler-exchange-httpd_parsing.h"
#include "taler-exchange-httpd_refund.h"
#include "taler-exchange-httpd_responses.h"
#include "taler-exchange-httpd_keystate.h"
#include "taler-exchange-httpd_validation.h"


/**
 * Generate successful refund confirmation message.
 *
 * @param connection connection to the client
 * @param refund details about the successful refund
 * @return MHD result code
 */
static int
reply_refund_success (struct MHD_Connection *connection,
                      const struct TALER_EXCHANGEDB_Refund *refund)
{
  struct TALER_RefundConfirmationPS rc;
  struct TALER_ExchangePublicKeyP pub;
  struct TALER_ExchangeSignatureP sig;

  rc.purpose.purpose = htonl (TALER_SIGNATURE_EXCHANGE_CONFIRM_REFUND);
  rc.purpose.size = htonl (sizeof (struct TALER_RefundConfirmationPS));
  rc.h_contract_terms = refund->h_contract_terms;
  rc.coin_pub = refund->coin.coin_pub;
  rc.merchant = refund->merchant_pub;
  rc.rtransaction_id = GNUNET_htonll (refund->rtransaction_id);
  TALER_amount_hton (&rc.refund_amount,
                     &refund->refund_amount);
  TALER_amount_hton (&rc.refund_fee,
                     &refund->refund_fee);
  if (GNUNET_OK !=
      TEH_KS_sign (&rc.purpose,
                   &pub,
                   &sig))
  {
    return TALER_MHD_reply_with_error (connection,
                                       MHD_HTTP_INTERNAL_SERVER_ERROR,
                                       TALER_EC_EXCHANGE_BAD_CONFIGURATION,
                                       "no keys");
  }
  return TALER_MHD_reply_json_pack (connection,
                                    MHD_HTTP_OK,
                                    "{s:s, s:o, s:o}",
                                    "status", "REFUND_OK",
                                    "sig", GNUNET_JSON_from_data_auto (&sig),
                                    "pub", GNUNET_JSON_from_data_auto (&pub));
}


/**
 * Generate generic refund failure message. All the details
 * are in the @a response_code.  The body can be empty.
 *
 * @param connection connection to the client
 * @param response_code response code to generate
 * @param ec taler error code to include
 * @return MHD result code
 */
static int
reply_refund_failure (struct MHD_Connection *connection,
                      unsigned int response_code,
                      enum TALER_ErrorCode ec)
{
  return TALER_MHD_reply_json_pack (connection,
                                    response_code,
                                    "{s:s, s:I}",
                                    "hint", "refund failure",
                                    "code", (json_int_t) ec);
}


/**
 * Generate refund conflict failure message. Returns the
 * transaction list @a tl with the details about the conflict.
 *
 * @param connection connection to the client
 * @param tl transaction list showing the conflict
 * @return MHD result code
 */
static int
reply_refund_conflict (struct MHD_Connection *connection,
                       const struct TALER_EXCHANGEDB_TransactionList *tl)
{
  return TALER_MHD_reply_json_pack (connection,
                                    MHD_HTTP_CONFLICT,
                                    "{s:s, s:I, s:o}",
                                    "hint", "conflicting refund",
                                    "code",
                                    (json_int_t) TALER_EC_REFUND_CONFLICT,
                                    "history",
                                    TEH_RESPONSE_compile_transaction_history (
                                      tl));
}


/**
 * Execute a "/refund" transaction.  Returns a confirmation that the
 * refund was successful, or a failure if we are not aware of a
 * matching /deposit or if it is too late to do the refund.
 *
 * IF it returns a non-error code, the transaction logic MUST
 * NOT queue a MHD response.  IF it returns an hard error, the
 * transaction logic MUST queue a MHD response and set @a mhd_ret.  IF
 * it returns the soft error code, the function MAY be called again to
 * retry and MUST not queue a MHD response.
 *
 * @param cls closure with a `const struct TALER_EXCHANGEDB_Refund *`
 * @param connection MHD request which triggered the transaction
 * @param session database session to use
 * @param[out] mhd_ret set to MHD response status for @a connection,
 *             if transaction failed (!)
 * @return transaction status
 */
static enum GNUNET_DB_QueryStatus
refund_transaction (void *cls,
                    struct MHD_Connection *connection,
                    struct TALER_EXCHANGEDB_Session *session,
                    int *mhd_ret)
{
  const struct TALER_EXCHANGEDB_Refund *refund = cls;
  struct TALER_EXCHANGEDB_TransactionList *tl;
  const struct TALER_EXCHANGEDB_Deposit *dep;
  const struct TALER_EXCHANGEDB_Refund *ref;
  struct TEH_KS_StateHandle *mks;
  struct TALER_EXCHANGEDB_DenominationKeyIssueInformation *dki;
  struct TALER_Amount expect_fee;
  enum GNUNET_DB_QueryStatus qs;
  int deposit_found;
  int refund_found;
  int fee_cmp;
  unsigned int hc;
  enum TALER_ErrorCode ec;

  dep = NULL;
  ref = NULL;
  tl = NULL;
  qs = TEH_plugin->get_coin_transactions (TEH_plugin->cls,
                                          session,
                                          &refund->coin.coin_pub,
                                          GNUNET_NO,
                                          &tl);
  if (0 > qs)
  {
    if (GNUNET_DB_STATUS_HARD_ERROR == qs)
      *mhd_ret = reply_refund_failure (connection,
                                       MHD_HTTP_NOT_FOUND,
                                       TALER_EC_REFUND_COIN_NOT_FOUND);
    return qs;
  }
  deposit_found = GNUNET_NO;
  refund_found = GNUNET_NO;
  for (struct TALER_EXCHANGEDB_TransactionList *tlp = tl;
       NULL != tlp;
       tlp = tlp->next)
  {
    switch (tlp->type)
    {
    case TALER_EXCHANGEDB_TT_DEPOSIT:
      if (GNUNET_NO == deposit_found)
      {
        if ( (0 == memcmp (&tlp->details.deposit->merchant_pub,
                           &refund->merchant_pub,
                           sizeof (struct TALER_MerchantPublicKeyP))) &&
             (0 == memcmp (&tlp->details.deposit->h_contract_terms,
                           &refund->h_contract_terms,
                           sizeof (struct GNUNET_HashCode))) )
        {
          dep = tlp->details.deposit;
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
        /* First, check if existing refund request is identical */
        if ( (0 == memcmp (&tlp->details.refund->merchant_pub,
                           &refund->merchant_pub,
                           sizeof (struct TALER_MerchantPublicKeyP))) &&
             (0 == memcmp (&tlp->details.refund->h_contract_terms,
                           &refund->h_contract_terms,
                           sizeof (struct GNUNET_HashCode))) &&
             (tlp->details.refund->rtransaction_id == refund->rtransaction_id) )
        {
          ref = tlp->details.refund;
          refund_found = GNUNET_YES;
          break;
        }
        /* Second, check if existing refund request conflicts */
        if ( (0 == memcmp (&tlp->details.refund->merchant_pub,
                           &refund->merchant_pub,
                           sizeof (struct TALER_MerchantPublicKeyP))) &&
             (0 == memcmp (&tlp->details.refund->h_contract_terms,
                           &refund->h_contract_terms,
                           sizeof (struct GNUNET_HashCode))) &&
             (tlp->details.refund->rtransaction_id != refund->rtransaction_id) )
        {
          GNUNET_break_op (0); /* conflicting refund found */
          refund_found = GNUNET_SYSERR;
          /* NOTE: Alternatively we could total up all existing
             refunds and check if the sum still permits the
             refund requested (thus allowing multiple, partial
             refunds). Fow now, we keep it simple. */
          break;
        }
      }
      break;
    case TALER_EXCHANGEDB_TT_OLD_COIN_PAYBACK:
      /* Paybacks cannot be refunded, ignore here */
      break;
    case TALER_EXCHANGEDB_TT_PAYBACK:
      /* Paybacks cannot be refunded, ignore here */
      break;
    case TALER_EXCHANGEDB_TT_PAYBACK_REFRESH:
      /* Paybacks cannot be refunded, ignore here */
      break;
    }
  }
  /* handle if deposit was NOT found */
  if (GNUNET_NO == deposit_found)
  {
    TALER_LOG_WARNING ("Deposit to /refund was not found\n");
    TEH_plugin->free_coin_transaction_list (TEH_plugin->cls,
                                            tl);
    *mhd_ret = TALER_MHD_reply_with_error (connection,
                                           MHD_HTTP_NOT_FOUND,
                                           TALER_EC_REFUND_DEPOSIT_NOT_FOUND,
                                           "deposit unknown");
    return GNUNET_DB_STATUS_HARD_ERROR;
  }
  /* handle if conflicting refund found */
  if (GNUNET_SYSERR == refund_found)
  {
    *mhd_ret = reply_refund_conflict (connection,
                                      tl);
    TEH_plugin->free_coin_transaction_list (TEH_plugin->cls,
                                            tl);
    return GNUNET_DB_STATUS_HARD_ERROR;
  }
  /* handle if identical refund found */
  if (GNUNET_YES == refund_found)
  {
    /* /refund already done, simply re-transmit confirmation */
    *mhd_ret = reply_refund_success (connection,
                                     ref);
    TEH_plugin->free_coin_transaction_list (TEH_plugin->cls,
                                            tl);
    return GNUNET_DB_STATUS_HARD_ERROR;
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
    *mhd_ret = reply_refund_failure (connection,
                                     MHD_HTTP_PRECONDITION_FAILED,
                                     TALER_EC_REFUND_CURRENCY_MISSMATCH);
    return GNUNET_DB_STATUS_HARD_ERROR;
  }

  /* check if we already send the money for the /deposit */
  qs = TEH_plugin->test_deposit_done (TEH_plugin->cls,
                                      session,
                                      dep);
  if (GNUNET_DB_STATUS_HARD_ERROR == qs)
  {
    /* Internal error, we first had the deposit in the history,
       but now it is gone? */
    GNUNET_break (0);
    TEH_plugin->free_coin_transaction_list (TEH_plugin->cls,
                                            tl);
    *mhd_ret = TALER_MHD_reply_with_error (connection,
                                           MHD_HTTP_INTERNAL_SERVER_ERROR,
                                           TALER_EC_REFUND_DB_INCONSISTENT,
                                           "database inconsistent");
    return qs;
  }
  if (GNUNET_DB_STATUS_SOFT_ERROR == qs)
    return qs; /* go and retry */

  if (GNUNET_DB_STATUS_SUCCESS_ONE_RESULT == qs)
  {
    /* money was already transferred to merchant, can no longer refund */
    TEH_plugin->free_coin_transaction_list (TEH_plugin->cls,
                                            tl);
    *mhd_ret = reply_refund_failure (connection,
                                     MHD_HTTP_GONE,
                                     TALER_EC_REFUND_MERCHANT_ALREADY_PAID);
    return GNUNET_DB_STATUS_HARD_ERROR;
  }

  /* check refund amount is sufficiently low */
  if (1 == TALER_amount_cmp (&refund->refund_amount,
                             &dep->amount_with_fee) )
  {
    GNUNET_break_op (0); /* cannot refund more than original value */
    TEH_plugin->free_coin_transaction_list (TEH_plugin->cls,
                                            tl);
    *mhd_ret = reply_refund_failure (connection,
                                     MHD_HTTP_PRECONDITION_FAILED,
                                     TALER_EC_REFUND_INSUFFICIENT_FUNDS);
    return GNUNET_DB_STATUS_HARD_ERROR;
  }

  // FIXME: do this outside of transaction function?
  /* Check refund fee matches fee of denomination key! */
  mks = TEH_KS_acquire (GNUNET_TIME_absolute_get ());
  if (NULL == mks)
  {
    TALER_LOG_ERROR ("Lacking keys to operate\n");
    TEH_plugin->free_coin_transaction_list (TEH_plugin->cls,
                                            tl);
    *mhd_ret = TALER_MHD_reply_with_error (connection,
                                           MHD_HTTP_INTERNAL_SERVER_ERROR,
                                           TALER_EC_EXCHANGE_BAD_CONFIGURATION,
                                           "no keys");
    return GNUNET_DB_STATUS_HARD_ERROR;
  }
  dki = TEH_KS_denomination_key_lookup_by_hash (mks,
                                                &dep->coin.denom_pub_hash,
                                                TEH_KS_DKU_DEPOSIT,
                                                &ec,
                                                &hc);
  if (NULL == dki)
  {
    /* DKI not found, but we do have a coin with this DK in our database;
       not good... */
    GNUNET_break (0);
    TEH_KS_release (mks);
    TEH_plugin->free_coin_transaction_list (TEH_plugin->cls,
                                            tl);
    *mhd_ret = TALER_MHD_reply_with_error (connection,
                                           ec,
                                           hc,
                                           "denomination not found, but coin known");
    return GNUNET_DB_STATUS_HARD_ERROR;
  }
  TALER_amount_ntoh (&expect_fee,
                     &dki->issue.properties.fee_refund);
  fee_cmp = TALER_amount_cmp (&refund->refund_fee,
                              &expect_fee);
  TEH_KS_release (mks);

  if (-1 == fee_cmp)
  {
    TEH_plugin->free_coin_transaction_list (TEH_plugin->cls,
                                            tl);
    *mhd_ret = TALER_MHD_reply_with_error (connection,
                                           MHD_HTTP_BAD_REQUEST,
                                           TALER_EC_REFUND_FEE_TOO_LOW,
                                           "refund_fee");
    return GNUNET_DB_STATUS_HARD_ERROR;
  }
  if (1 == fee_cmp)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                "Refund fee proposed by merchant is higher than necessary.\n");
  }
  TEH_plugin->free_coin_transaction_list (TEH_plugin->cls,
                                          tl);

  /* Finally, store new refund data */
  qs = TEH_plugin->insert_refund (TEH_plugin->cls,
                                  session,
                                  refund);
  if (GNUNET_DB_STATUS_HARD_ERROR == qs)
  {
    TALER_LOG_WARNING ("Failed to store /refund information in database\n");
    *mhd_ret = TALER_MHD_reply_with_error (connection,
                                           MHD_HTTP_INTERNAL_SERVER_ERROR,
                                           TALER_EC_REFUND_STORE_DB_ERROR,
                                           "could not persist store information");
    return qs;
  }
  /* Success or soft failure */
  return qs;
}


/**
 * We have parsed the JSON information about the refund, do some basic
 * sanity checks (especially that the signature on the coin is valid)
 * and then execute the refund.  Note that we need the DB to check
 * the fee structure, so this is not done here.
 *
 * @param connection the MHD connection to handle
 * @param refund information about the refund
 * @return MHD result code
 */
static int
verify_and_execute_refund (struct MHD_Connection *connection,
                           const struct TALER_EXCHANGEDB_Refund *refund)
{
  struct TALER_RefundRequestPS rr;
  int mhd_ret;

  rr.purpose.purpose = htonl (TALER_SIGNATURE_MERCHANT_REFUND);
  rr.purpose.size = htonl (sizeof (struct TALER_RefundRequestPS));
  rr.h_contract_terms = refund->h_contract_terms;
  rr.coin_pub = refund->coin.coin_pub;
  rr.merchant = refund->merchant_pub;
  rr.rtransaction_id = GNUNET_htonll (refund->rtransaction_id);
  TALER_amount_hton (&rr.refund_amount,
                     &refund->refund_amount);
  TALER_amount_hton (&rr.refund_fee,
                     &refund->refund_fee);
  if (GNUNET_YES !=
      TALER_amount_cmp_currency (&refund->refund_amount,
                                 &refund->refund_fee) )
  {
    GNUNET_break_op (0);
    return TALER_MHD_reply_with_error (connection,
                                       MHD_HTTP_BAD_REQUEST,
                                       TALER_EC_REFUND_FEE_CURRENCY_MISSMATCH,
                                       "refund_fee");
  }
  if (-1 == TALER_amount_cmp (&refund->refund_amount,
                              &refund->refund_fee) )
  {
    GNUNET_break_op (0);
    return TALER_MHD_reply_with_error (connection,
                                       MHD_HTTP_BAD_REQUEST,
                                       TALER_EC_REFUND_FEE_ABOVE_AMOUNT,
                                       "refund_amount");
  }
  if (GNUNET_OK !=
      GNUNET_CRYPTO_eddsa_verify (TALER_SIGNATURE_MERCHANT_REFUND,
                                  &rr.purpose,
                                  &refund->merchant_sig.eddsa_sig,
                                  &refund->merchant_pub.eddsa_pub))
  {
    TALER_LOG_WARNING ("Invalid signature on /refund request\n");
    return TALER_MHD_reply_with_error (connection,
                                       MHD_HTTP_FORBIDDEN,
                                       TALER_EC_REFUND_MERCHANT_SIGNATURE_INVALID,
                                       "merchant_sig");
  }
  if (GNUNET_OK !=
      TEH_DB_run_transaction (connection,
                              "run refund",
                              &mhd_ret,
                              &refund_transaction,
                              (void *) refund))
    return mhd_ret;
  return reply_refund_success (connection,
                               refund);
}


/**
 * Handle a "/refund" request.  Parses the JSON, and, if successful,
 * passes the JSON data to #verify_and_execute_refund() to
 * further check the details of the operation specified.  If
 * everything checks out, this will ultimately lead to the "/refund"
 * being executed, or rejected.
 *
 * @param rh context of the handler
 * @param connection the MHD connection to handle
 * @param[in,out] connection_cls the connection's closure (can be updated)
 * @param upload_data upload data
 * @param[in,out] upload_data_size number of bytes (left) in @a upload_data
 * @return MHD result code
  */
int
TEH_REFUND_handler_refund (struct TEH_RequestHandler *rh,
                           struct MHD_Connection *connection,
                           void **connection_cls,
                           const char *upload_data,
                           size_t *upload_data_size)
{
  json_t *json;
  int res;
  struct TALER_EXCHANGEDB_Refund refund;
  struct GNUNET_JSON_Specification spec[] = {
    TALER_JSON_spec_amount ("refund_amount", &refund.refund_amount),
    TALER_JSON_spec_amount ("refund_fee", &refund.refund_fee),
    GNUNET_JSON_spec_fixed_auto ("h_contract_terms", &refund.h_contract_terms),
    GNUNET_JSON_spec_fixed_auto ("coin_pub", &refund.coin.coin_pub),
    GNUNET_JSON_spec_fixed_auto ("merchant_pub", &refund.merchant_pub),
    GNUNET_JSON_spec_uint64 ("rtransaction_id", &refund.rtransaction_id),
    GNUNET_JSON_spec_fixed_auto ("merchant_sig", &refund.merchant_sig),
    GNUNET_JSON_spec_end ()
  };

  (void) rh;
  res = TEH_PARSE_post_json (connection,
                             connection_cls,
                             upload_data,
                             upload_data_size,
                             &json);
  if (GNUNET_SYSERR == res)
    return MHD_NO;
  if ( (GNUNET_NO == res) || (NULL == json) )
    return MHD_YES;
  res = TEH_PARSE_json_data (connection,
                             json,
                             spec);
  json_decref (json);
  if (GNUNET_SYSERR == res)
    return MHD_NO; /* hard failure */
  if (GNUNET_NO == res)
    return MHD_YES; /* failure */
  res = verify_and_execute_refund (connection,
                                   &refund);
  GNUNET_JSON_parse_free (spec);
  return res;
}


/* end of taler-exchange-httpd_refund.c */
