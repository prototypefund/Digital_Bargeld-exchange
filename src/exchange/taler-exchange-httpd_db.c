/*
  This file is part of TALER
  Copyright (C) 2014-2017 Taler Systems SA

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
#include "taler_mhd_lib.h"
#include "taler-exchange-httpd_responses.h"
#include "taler-exchange-httpd_keystate.h"

/**
 * How often should we retry a transaction before giving up
 * (for transactions resulting in serialization/dead locks only).
 *
 * The current value is likely too high for production. We might want to
 * benchmark good values once we have a good database setup.  The code is
 * expected to work correctly with any positive value, albeit inefficiently if
 * we too aggressively force clients to retry the HTTP request merely because
 * we have database serialization issues.
 */
#define MAX_TRANSACTION_COMMIT_RETRIES 100


/**
 * Execute database transaction to ensure coin is known. Run the transaction
 * logic; IF it returns a non-error code, the transaction logic MUST
 * NOT queue a MHD response.  IF it returns an hard error, the
 * transaction logic MUST queue a MHD response and set @a mhd_ret.  IF
 * it returns the soft error code, the function MAY be called again to
 * retry and MUST not queue a MHD response.
 *
 * @param cls a `struct TEH_DB_KnowCoinContext`
 * @param connection MHD request context, must not be NULL
 * @param session database session and transaction to use
 * @param[out] mhd_ret set to MHD status on error, must not be NULL
 * @return transaction status
 */
enum GNUNET_DB_QueryStatus
TEH_DB_know_coin_transaction (void *cls,
                              struct MHD_Connection *connection,
                              struct TALER_EXCHANGEDB_Session *session,
                              MHD_RESULT *mhd_ret)
{
  struct TEH_DB_KnowCoinContext *kcc = cls;
  enum GNUNET_DB_QueryStatus qs;

  GNUNET_assert (NULL != mhd_ret);
  qs = TEH_plugin->ensure_coin_known (TEH_plugin->cls,
                                      session,
                                      kcc->coin);
  if (GNUNET_DB_STATUS_HARD_ERROR == qs)
  {
    *mhd_ret
      = TALER_MHD_reply_with_error (connection,
                                    MHD_HTTP_INTERNAL_SERVER_ERROR,
                                    TALER_EC_DB_COIN_HISTORY_STORE_ERROR,
                                    "could not persist coin data");
    return GNUNET_DB_STATUS_HARD_ERROR;
  }
  return qs;
}


/**
 * Run a database transaction for @a connection.
 * Starts a transaction and calls @a cb.  Upon success,
 * attempts to commit the transaction.  Upon soft failures,
 * retries @a cb a few times.  Upon hard or persistent soft
 * errors, generates an error message for @a connection.
 *
 * @param connection MHD connection to run @a cb for, can be NULL
 * @param name name of the transaction (for debugging)
 * @param[out] mhd_ret set to MHD response code, if transaction failed;
 *             NULL if we are not running with a @a connection and thus
 *             must not queue MHD replies
 * @param cb callback implementing transaction logic
 * @param cb_cls closure for @a cb, must be read-only!
 * @return #GNUNET_OK on success, #GNUNET_SYSERR on failure
 */
enum GNUNET_GenericReturnValue
TEH_DB_run_transaction (struct MHD_Connection *connection,
                        const char *name,
                        MHD_RESULT *mhd_ret,
                        TEH_DB_TransactionCallback cb,
                        void *cb_cls)
{
  struct TALER_EXCHANGEDB_Session *session;

  if (NULL != mhd_ret)
    *mhd_ret = -1; /* set to invalid value, to help detect bugs */
  if (NULL == (session = TEH_plugin->get_session (TEH_plugin->cls)))
  {
    GNUNET_break (0);
    if (NULL != mhd_ret)
      *mhd_ret = TALER_MHD_reply_with_error (connection,
                                             MHD_HTTP_INTERNAL_SERVER_ERROR,
                                             TALER_EC_DB_SETUP_FAILED,
                                             "could not establish database session");
    return GNUNET_SYSERR;
  }
  for (unsigned int retries = 0;
       retries < MAX_TRANSACTION_COMMIT_RETRIES;
       retries++)
  {
    enum GNUNET_DB_QueryStatus qs;

    if (GNUNET_OK !=
        TEH_plugin->start (TEH_plugin->cls,
                           session,
                           name))
    {
      GNUNET_break (0);
      if (NULL != mhd_ret)
        *mhd_ret = TALER_MHD_reply_with_error (connection,
                                               MHD_HTTP_INTERNAL_SERVER_ERROR,
                                               TALER_EC_DB_START_FAILED,
                                               "could not begin transaction");
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
      if (NULL != mhd_ret)
        *mhd_ret = TALER_MHD_reply_with_error (connection,
                                               MHD_HTTP_INTERNAL_SERVER_ERROR,
                                               TALER_EC_DB_COMMIT_FAILED_HARD,
                                               "could not commit database transaction");
      return GNUNET_SYSERR;
    }
    /* make sure callback did not violate invariants! */
    GNUNET_assert ( (NULL == mhd_ret) ||
                    (-1 == *mhd_ret) );
    if (0 <= qs)
      return GNUNET_OK;
  }
  TALER_LOG_ERROR ("Transaction `%s' commit failed %u times\n",
                   name,
                   MAX_TRANSACTION_COMMIT_RETRIES);
  if (NULL != mhd_ret)
    *mhd_ret = TALER_MHD_reply_with_error (connection,
                                           MHD_HTTP_INTERNAL_SERVER_ERROR,
                                           TALER_EC_DB_COMMIT_FAILED_ON_RETRY,
                                           "repatedly failed to serialize database transaction");
  return GNUNET_SYSERR;
}


/* end of taler-exchange-httpd_db.c */
