/*
  This file is part of TALER
  Copyright (C) 2017--2020 Taler Systems SA

  TALER is free software; you can redistribute it and/or
  modify it under the terms of the GNU General Public License
  as published by the Free Software Foundation; either version 3,
  or (at your option) any later version.

  TALER is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public
  License along with TALER; see the file COPYING.  If not,
  see <http://www.gnu.org/licenses/>
*/
/**
 * @file bank-lib/bank_api_history.c
 * @brief Implementation of the /history[-range]
 *        requests of the bank's HTTP API.
 * @author Christian Grothoff
 * @author Marcello Stanisci
 */
#include "platform.h"
#include "bank_api_common.h"
#include <microhttpd.h> /* just for HTTP status codes */
#include "taler_signatures.h"


/**
 * @brief A /history Handle
 */
struct TALER_BANK_CreditHistoryHandle
{

  /**
   * The url for this request.
   */
  char *request_url;

  /**
   * The base URL of the bank.
   */
  char *bank_base_url;

  /**
   * Handle for the request.
   */
  struct GNUNET_CURL_Job *job;

  /**
   * Function to call with the result.
   */
  TALER_BANK_CreditResultCallback hcb;

  /**
   * Closure for @a cb.
   */
  void *hcb_cls;
};


/**
 * Parse history given in JSON format and invoke the callback on each item.
 *
 * @param hh handle to the account history request
 * @param history JSON array with the history
 * @return #GNUNET_OK if history was valid and @a rhistory and @a balance
 *         were set,
 *         #GNUNET_SYSERR if there was a protocol violation in @a history
 */
static int
parse_account_history (struct TALER_BANK_CreditHistoryHandle *hh,
                       const json_t *history)
{
  json_t *history_array;

  if (NULL == (history_array = json_object_get (history,
                                                "data")))
  {
    GNUNET_break_op (0);
    return GNUNET_SYSERR;
  }
  if (! json_is_array (history_array))
  {
    GNUNET_break_op (0);
    return GNUNET_SYSERR;
  }
  for (unsigned int i = 0; i<json_array_size (history_array); i++)
  {
    struct TALER_BANK_CreditDetails td;
    uint64_t row_id;
    struct GNUNET_JSON_Specification hist_spec[] = {
      TALER_JSON_spec_amount ("amount",
                              &td.amount),
      GNUNET_JSON_spec_absolute_time ("date",
                                      &td.execution_date),
      GNUNET_JSON_spec_uint64 ("row_id",
                               &row_id),
      GNUNET_JSON_spec_fixed_auto ("reserve_pub",
                                   &td.reserve_pub),
      GNUNET_JSON_spec_string ("counterpart",
                               &td.account_url),
      GNUNET_JSON_spec_end ()
    };
    json_t *transaction = json_array_get (history_array,
                                          i);

    if (GNUNET_OK !=
        GNUNET_JSON_parse (transaction,
                           hist_spec,
                           NULL, NULL))
    {
      GNUNET_break_op (0);
      return GNUNET_SYSERR;
    }
    hh->hcb (hh->hcb_cls,
             MHD_HTTP_OK,
             TALER_EC_NONE,
             row_id,
             &td,
             transaction);
    GNUNET_JSON_parse_free (hist_spec);
  }
  return GNUNET_OK;
}


/**
 * Function called when we're done processing the
 * HTTP /history request.
 *
 * @param cls the `struct TALER_BANK_CreditHistoryHandle`
 * @param response_code HTTP response code, 0 on error
 * @param response parsed JSON result, NULL on error
 */
static void
handle_history_finished (void *cls,
                         long response_code,
                         const void *response)
{
  struct TALER_BANK_CreditHistoryHandle *hh = cls;
  enum TALER_ErrorCode ec;
  const json_t *j = response;

  hh->job = NULL;
  switch (response_code)
  {
  case 0:
    ec = TALER_EC_INVALID_RESPONSE;
    break;
  case MHD_HTTP_OK:
    if (GNUNET_OK !=
        parse_account_history (hh,
                               j))
    {
      GNUNET_break_op (0);
      response_code = 0;
      ec = TALER_EC_INVALID_RESPONSE;
      break;
    }
    response_code = MHD_HTTP_NO_CONTENT; /* signal end of list */
    ec = TALER_EC_NONE;
    break;
  case MHD_HTTP_NO_CONTENT:
    ec = TALER_EC_NONE;
    break;
  case MHD_HTTP_BAD_REQUEST:
    /* This should never happen, either us or the bank is buggy
       (or API version conflict); just pass JSON reply to the application */
    ec = TALER_BANK_parse_ec_ (j);
    break;
  case MHD_HTTP_FORBIDDEN:
    /* Access denied */
    ec = TALER_BANK_parse_ec_ (j);
    break;
  case MHD_HTTP_UNAUTHORIZED:
    /* Nothing really to verify, bank says one of the signatures is
       invalid; as we checked them, this should never happen, we
       should pass the JSON reply to the application */
    ec = TALER_BANK_parse_ec_ (j);
    break;
  case MHD_HTTP_NOT_FOUND:
    /* Nothing really to verify, this should never
       happen, we should pass the JSON reply to the application */
    ec = TALER_BANK_parse_ec_ (j);
    break;
  case MHD_HTTP_INTERNAL_SERVER_ERROR:
    /* Server had an internal issue; we should retry, but this API
       leaves this to the application */
    ec = TALER_BANK_parse_ec_ (j);
    break;
  default:
    /* unexpected response code */
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Unexpected response code %u\n",
                (unsigned int) response_code);
    GNUNET_break (0);
    ec = TALER_BANK_parse_ec_ (j);
    response_code = 0;
    break;
  }
  hh->hcb (hh->hcb_cls,
           response_code,
           ec,
           0LLU,
           NULL,
           j);
  TALER_BANK_credit_history_cancel (hh);
}


/**
 * Request the credit history of the exchange's bank account.
 *
 * @param ctx curl context for the event loop
 * @param bank_base_url URL of the base INCLUDING account number
 * @param auth authentication data to use
 * @param start_row from which row on do we want to get results,
 *        use UINT64_MAX for the latest; exclusive
 * @param num_results how many results do we want;
 *        negative numbers to go into the past, positive numbers
 *        to go into the future starting at @a start_row;
 *        must not be zero.
 * @param hres_cb the callback to call with the transaction
 *        history
 * @param hres_cb_cls closure for the above callback
 * @return NULL if the inputs are invalid (i.e. zero value for
 *         @e num_results). In this case, the callback is not
 *         called.
 */
struct TALER_BANK_CreditHistoryHandle *
TALER_BANK_credit_history (struct GNUNET_CURL_Context *ctx,
                           const char *bank_base_url,
                           const struct TALER_BANK_AuthenticationData *auth,
                           uint64_t start_row,
                           int64_t num_results,
                           TALER_BANK_CreditResultCallback hres_cb,
                           void *hres_cb_cls)
{
  char *url;
  struct TALER_BANK_CreditHistoryHandle *hh;
  CURL *eh;

  if (0 == num_results)
  {
    GNUNET_break (0);
    return NULL;
  }

  if (UINT64_MAX == start_row)
    GNUNET_asprintf (&url,
                     "/history&delta=%lld",
                     (long long) num_results);
  else
    GNUNET_asprintf (&url,
                     "/history&delta=%lld&start=%llu",
                     (long long) num_results,
                     start_row);
  hh = GNUNET_new (struct TALER_BANK_CreditHistoryHandle);
  hh->hcb = hres_cb;
  hh->hcb_cls = hres_cb_cls;
  hh->bank_base_url = GNUNET_strdup (bank_base_url);
  hh->request_url = TALER_BANK_path_to_url_ (bank_base_url,
                                             url);

  eh = curl_easy_init ();
  if ( (GNUNET_OK !=
        TALER_BANK_setup_auth_ (eh,
                                auth)) ||
       (CURLE_OK !=
        curl_easy_setopt (eh,
                          CURLOPT_URL,
                          hh->request_url)) )
  {
    GNUNET_break (0);
    TALER_BANK_credit_history_cancel (hh);
    curl_easy_cleanup (eh);
    GNUNET_free (url);
    return NULL;
  }
  hh->job = GNUNET_CURL_job_add2 (ctx,
                                  eh,
                                  NULL,
                                  &handle_history_finished,
                                  hh);
  GNUNET_free (url);
  return hh;
}


/**
 * Cancel a history request.  This function cannot be
 * used on a request handle if a response is already
 * served for it.
 *
 * @param hh the history request handle
 */
void
TALER_BANK_credit_history_cancel (struct TALER_BANK_CreditHistoryHandle *hh)
{
  if (NULL != hh->job)
  {
    GNUNET_CURL_job_cancel (hh->job);
    hh->job = NULL;
  }
  GNUNET_free (hh->request_url);
  GNUNET_free (hh->bank_base_url);
  GNUNET_free (hh);
}


/* end of bank_api_credit.c */