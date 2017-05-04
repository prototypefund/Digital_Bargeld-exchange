/*
  This file is part of TALER
  Copyright (C) 2015, 2016, 2017 GNUnet e.V. & Inria

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
 * @file include/taler_bank_service.h
 * @brief C interface of libtalerbank, a C library to use the Taler bank's HTTP API
 *        This is currently ONLY used to provide the "test" wire transfer protocol.
 * @author Christian Grothoff
 */
#ifndef _TALER_BANK_SERVICE_H
#define _TALER_BANK_SERVICE_H

#include <jansson.h>
#include <gnunet/gnunet_curl_lib.h>
#include "taler_util.h"


/**
 * Authentication method types.
 */
enum TALER_BANK_AuthenticationMethod {

  /**
   * No authentication.
   */
  TALER_BANK_AUTH_NONE,

  /**
   * Basic authentication with cleartext username and password.
   */
  TALER_BANK_AUTH_BASIC
};


/**
 * Information used to authenticate to the bank.
 */
struct TALER_BANK_AuthenticationData
{

  /**
   * Which authentication method should we use?
   */
  enum TALER_BANK_AuthenticationMethod method;

  /**
   * Further details as per @e method.
   */
  union
  {

    /**
     * Details for #TALER_BANK_AUTH_BASIC.
     */
    struct
    {
      /**
       * Username to use.
       */
      char *username;

      /**
       * Password to use.
       */
      char *password;
    } basic;

  } details;

};


/* ********************* /admin/add/incoming *********************** */


/**
 * @brief A /admin/add/incoming Handle
 */
struct TALER_BANK_AdminAddIncomingHandle;


/**
 * Callbacks of this type are used to serve the result of submitting
 * information about an incoming transaction to a bank.
 *
 * @param cls closure
 * @param http_status HTTP response code, #MHD_HTTP_OK (200) for successful status request
 *                    0 if the bank's reply is bogus (fails to follow the protocol)
 * @param json detailed response from the HTTPD, or NULL if reply was not in JSON
 */
typedef void
(*TALER_BANK_AdminAddIncomingResultCallback) (void *cls,
                                              unsigned int http_status,
                                              const json_t *json);


/**
 * Notify the bank that we have received an incoming transaction
 * which fills a reserve.  Note that this API is an administrative
 * API and thus not accessible to typical bank clients, but only
 * to the operators of the bank.
 *
 * @param ctx curl context for the event loop
 * @param bank_base_url URL of the bank (used to execute this request)
 * @param auth authentication data to use
 * @param exchange_base_url base URL of the exchange (for tracking)
 * @param wtid wire transfer identifier for the transfer
 * @param amount amount that was deposited
 * @param debit_account_no account number to withdraw from (53 bits at most)
 * @param credit_account_no account number to deposit into (53 bits at most)
 * @param res_cb the callback to call when the final result for this request is available
 * @param res_cb_cls closure for the above callback
 * @return NULL
 *         if the inputs are invalid (i.e. invalid amount).
 *         In this case, the callback is not called.
 */
struct TALER_BANK_AdminAddIncomingHandle *
TALER_BANK_admin_add_incoming (struct GNUNET_CURL_Context *ctx,
                               const char *bank_base_url,
                               const struct TALER_BANK_AuthenticationData *auth,
                               const char *exchange_base_url,
                               const struct TALER_WireTransferIdentifierRawP *wtid,
                               const struct TALER_Amount *amount,
                               uint64_t debit_account_no,
                               uint64_t credit_account_no,
                               TALER_BANK_AdminAddIncomingResultCallback res_cb,
                               void *res_cb_cls);


/**
 * Cancel an add incoming.  This function cannot be used on a request
 * handle if a response is already served for it.
 *
 * @param aai the admin add incoming request handle
 */
void
TALER_BANK_admin_add_incoming_cancel (struct TALER_BANK_AdminAddIncomingHandle *aai);


/**
 * Which types of transactions should be returned?
 */
enum TALER_BANK_Direction {

  /**
   * Transactions where the bank account receives money.
   */
  TALER_BANK_DIRECTION_CREDIT = 1,

  /**
   * Transactions where the bank account looses money.
   */
  TALER_BANK_DIRECTION_DEBIT = 2,

  /**
   * Return both types of transactions.
   */
  TALER_BANK_DIRECTION_BOTH = (TALER_BANK_DIRECTION_CREDIT | TALER_BANK_DIRECTION_DEBIT)

};


/**
 * Handle for querying the bank's transaction history.
 */
struct TALER_BANK_HistoryHandle;

/**
 * Details about a wire transfer.
 */
struct TALER_BANK_TransferDetails
{
  /**
   * amount that was transferred
   */
  struct TALER_Amount amount;

  /**
   * when did the transfer happen
   */
  struct GNUNET_TIME_Absolute execution_date;

  /**
   * monotonically increasing counter corresponding to the transaction
   */
  uint64_t serial_id;

  /**
   * wire transfer subject
   */
  char *wire_transfer_subject;

  /**
   * what was the other account that was involved
   */
  json_t *account_details;
};


/**
 * Callbacks of this type are used to serve the result of asking
 * the bank for the transaction history.
 *
 * @param cls closure
 * @param http_status HTTP response code, #MHD_HTTP_OK (200) for successful status request
 *                    0 if the bank's reply is bogus (fails to follow the protocol),
 *                    #MHD_HTTP_NO_CONTENT if there are no more results; on success the
 *                    last callback is always of this status (even if `abs(num_results)` were
 *                    already returned).
 * @param dir direction of the transfer
 * @param details details about the wire transfer
 * @param json detailed response from the HTTPD, or NULL if reply was not in JSON
 */
typedef void
(*TALER_BANK_HistoryResultCallback) (void *cls,
                                     unsigned int http_status,
                                     enum TALER_BANK_Direction dir,
                                     const struct TALER_BANK_TransferDetails *details,
                                     const json_t *json);


/**
 * Notify the bank that we have received an incoming transaction
 * which fills a reserve.  Note that this API is an administrative
 * API and thus not accessible to typical bank clients, but only
 * to the operators of the bank.
 *
 * @param ctx curl context for the event loop
 * @param bank_base_url URL of the bank (used to execute this request)
 * @param auth authentication data to use
 * @param account_number which account number should we query
 * @param direction what kinds of wire transfers should be returned
 * @param start_row from which row on do we want to get results, use UINT64_MAX for the latest
 * @param num_results how many results do we want; negative numbers to go into the past,
 *                    positive numbers to go into the future starting at @a start_row;
 *                    must not be zero.
 * @param hres_cb the callback to call with the transaction history
 * @param hres_cb_cls closure for the above callback
 * @return NULL
 *         if the inputs are invalid (i.e. zero value for @e num_results).
 *         In this case, the callback is not called.
 */
struct TALER_BANK_HistoryHandle *
TALER_BANK_history (struct GNUNET_CURL_Context *ctx,
                    const char *bank_base_url,
                    const struct TALER_BANK_AuthenticationData *auth,
                    uint64_t account_number,
                    enum TALER_BANK_Direction direction,
                    uint64_t start_row,
                    int64_t num_results,
                    TALER_BANK_HistoryResultCallback hres_cb,
                    void *hres_cb_cls);


/**
 * Cancel an history request.  This function cannot be used on a request
 * handle if the last response (anything with a status code other than
 * 200) is already served for it.
 *
 * @param hh the history request handle
 */
void
TALER_BANK_history_cancel (struct TALER_BANK_HistoryHandle *hh);



#endif  /* _TALER_BANK_SERVICE_H */
