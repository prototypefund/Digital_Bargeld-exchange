/*
  This file is part of TALER
  Copyright (C) 2014-2017 GNUnet e.V.

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
 * @file taler-exchange-httpd_db.c
 * @brief Generic database operations for the exchange.
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
 * Run a database transaction for @a connection.
 * Starts a transaction and calls @a cb.  Upon success,
 * attempts to commit the transaction.  Upon soft failures,
 * retries @a cb a few times.  Upon hard or persistent soft
 * errors, generates an error message for @a connection.
 * 
 * @param connection MHD connection to run @a cb for
 * @param[out] set to MHD response code, if transaction failed
 * @param cb callback implementing transaction logic
 * @param cb_cls closure for @a cb, must be read-only!
 * @return #GNUNET_OK on success, #GNUNET_SYSERR on failure
 */
int
TEH_DB_run_transaction (struct MHD_Connection *connection,
			int *mhd_ret,
			TEH_DB_TransactionCallback cb,
			void *cb_cls)
{
  struct TALER_EXCHANGEDB_Session *session;

  *mhd_ret = -1; /* invalid value */
  if (NULL == (session = TEH_plugin->get_session (TEH_plugin->cls)))
  {
    GNUNET_break (0);
    *mhd_ret = TEH_RESPONSE_reply_internal_db_error (connection,
						     TALER_EC_DB_SETUP_FAILED);
    return GNUNET_SYSERR;
  }
  for (unsigned int retries = 0;retries < MAX_TRANSACTION_COMMIT_RETRIES; retries++)
  {
    enum GNUNET_DB_QueryStatus qs;

    if (GNUNET_OK !=                                            
	TEH_plugin->start (TEH_plugin->cls,                     
			   session))                            
    {                                      
      GNUNET_break (0);                                         
      *mhd_ret = TEH_RESPONSE_reply_internal_db_error (connection, 
						       TALER_EC_DB_START_FAILED);
      return GNUNET_SYSERR;
    }
    qs = cb (cb_cls,
	     connection,
	     session,
	     mhd_ret);
    if (0 > qs)
      TEH_plugin->rollback (TEH_plugin->cls,
			    session);      
    if (GNUNET_DB_STATUS_HARD_ERROR == qs)
      return GNUNET_SYSERR;
    if (0 <= qs)
      qs = TEH_plugin->commit (TEH_plugin->cls,
			       session);                              
    if (GNUNET_DB_STATUS_HARD_ERROR == qs)
    {
      *mhd_ret = TEH_RESPONSE_reply_commit_error (connection,
						  TALER_EC_DB_COMMIT_FAILED_HARD);
      return GNUNET_SYSERR;
    }
    /* make sure callback did not violate invariants! */
    GNUNET_assert (-1 == *mhd_ret);
    if (0 <= qs)
      return GNUNET_OK;
  }
  TALER_LOG_WARNING ("Transaction commit failed %u times\n",
		     MAX_TRANSACTION_COMMIT_RETRIES);
  *mhd_ret = TEH_RESPONSE_reply_commit_error (connection,
					      TALER_EC_DB_COMMIT_FAILED_ON_RETRY);
  return GNUNET_SYSERR;
}


/**
 * Calculate the total value of all transactions performed.
 * Stores @a off plus the cost of all transactions in @a tl
 * in @a ret.
 *
 * @param tl transaction list to process
 * @param off offset to use as the starting value
 * @param[out] ret where the resulting total is to be stored
 * @return #GNUNET_OK on success, #GNUNET_SYSERR on errors
 */
// FIXME: maybe move to another module, i.e. exchangedb???
int
TEH_DB_calculate_transaction_list_totals (struct TALER_EXCHANGEDB_TransactionList *tl,
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
    case TALER_EXCHANGEDB_TT_PAYBACK:
      /* spent += pos->value */
      if (GNUNET_OK !=
          TALER_amount_add (&spent,
                            &spent,
                            &pos->details.payback->value))
      {
        GNUNET_break (0);
        return GNUNET_SYSERR;
      }
      break;
    }
  }
  /* spent = spent - refunded */
  if (GNUNET_SYSERR ==
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


/* end of taler-exchange-httpd_db.c */
