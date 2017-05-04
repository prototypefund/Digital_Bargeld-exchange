/*
  This file is part of TALER
  Copyright (C) 2017 GNUnet e.V. & Inria

  TALER is free software; you can redistribute it and/or modify it under the
  terms of the GNU General Public License as published by the Free Software
  Foundation; either version 3, or (at your option) any later version.

  TALER is distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
  A PARTICULAR PURPOSE.  See the GNU General Public License for more details.

  You should have received a copy of the GNU General Public License along with
  TALER; see the file COPYING.  If not, see
  <http://www.gnu.org/licenses/>
*/
/**
 * @file bank-lib/bank_api_history.c
 * @brief Implementation of the /history requests of the bank's HTTP API
 * @author Christian Grothoff
 */
#include "platform.h"
#include "bank_api_common.h"
#include <microhttpd.h> /* just for HTTP status codes */
#include "taler_signatures.h"


/**
 * @brief A /history Handle
 */
struct TALER_BANK_HistoryHandle
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
   * HTTP authentication-related headers for the request.
   */
  struct curl_slist *authh;

  /**
   * Function to call with the result.
   */
  TALER_BANK_HistoryResultCallback hcb;

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
parse_account_history (struct TALER_BANK_HistoryHandle *hh,
                       const json_t *history)
{
  for (unsigned int i=0;i<json_array_size (history);i++)
  {
    struct TALER_BANK_TransferDetails td;
    const char *sign;
    uint64_t other_account;
    uint64_t serial_id;
    enum TALER_BANK_Direction direction;
    struct GNUNET_JSON_Specification hist_spec[] = {
      GNUNET_JSON_spec_string ("sign",
                               &sign),
      TALER_JSON_spec_amount ("amount",
                              &td.amount),
      GNUNET_JSON_spec_absolute_time ("date",
                                      &td.execution_date),
      GNUNET_JSON_spec_uint64 ("row_id",
                               &serial_id),
      GNUNET_JSON_spec_string ("wt_subject",
                               &td.wire_transfer_subject),
      GNUNET_JSON_spec_uint64 ("counterpart",
                               &other_account),
      GNUNET_JSON_spec_end()
    };
    json_t *transaction = json_array_get (history,
                                          i);

    if (GNUNET_OK !=
        GNUNET_JSON_parse (transaction,
                           hist_spec,
                           NULL, NULL))
    {
      GNUNET_break_op (0);
      return GNUNET_SYSERR;
    }
    td.account_details = json_pack ("{s:s, s:s, s:I}",
                                    "type", "test",
                                    "bank_uri", hh->bank_base_url,
                                    "account_number", (json_int_t) other_account);
    direction = (0 == strcasecmp (sign,
                                  "+"))
      ? TALER_BANK_DIRECTION_CREDIT
      : TALER_BANK_DIRECTION_DEBIT;
    hh->hcb (hh->hcb_cls,
             MHD_HTTP_OK,
             direction,
             serial_id,
             &td,
             transaction);
    GNUNET_JSON_parse_free (hist_spec);
    json_decref (td.account_details);
  }
  return GNUNET_OK;
}


/**
 * Function called when we're done processing the
 * HTTP /admin/add/incoming request.
 *
 * @param cls the `struct TALER_BANK_HistoryHandle`
 * @param response_code HTTP response code, 0 on error
 * @param json parsed JSON result, NULL on error
 */
static void
handle_history_finished (void *cls,
                         long response_code,
                         const json_t *json)
{
  struct TALER_BANK_HistoryHandle *hh = cls;

  hh->job = NULL;
  switch (response_code)
  {
  case 0:
    break;
  case MHD_HTTP_OK:
    if (GNUNET_OK !=
        parse_account_history (hh,
                               json))
    {
      GNUNET_break_op (0);
      response_code = 0;
      break;
    }
    response_code = MHD_HTTP_NO_CONTENT; /* signal end of list */
    break;
  case MHD_HTTP_NO_CONTENT:
    break;
  case MHD_HTTP_BAD_REQUEST:
    /* This should never happen, either us or the bank is buggy
       (or API version conflict); just pass JSON reply to the application */
    break;
  case MHD_HTTP_FORBIDDEN:
    /* Access denied */
    break;
  case MHD_HTTP_UNAUTHORIZED:
    /* Nothing really to verify, bank says one of the signatures is
       invalid; as we checked them, this should never happen, we
       should pass the JSON reply to the application */
    break;
  case MHD_HTTP_NOT_FOUND:
    /* Nothing really to verify, this should never
       happen, we should pass the JSON reply to the application */
    break;
  case MHD_HTTP_INTERNAL_SERVER_ERROR:
    /* Server had an internal issue; we should retry, but this API
       leaves this to the application */
    break;
  default:
    /* unexpected response code */
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Unexpected response code %u\n",
                (unsigned int) response_code);
    GNUNET_break (0);
    response_code = 0;
    break;
  }
  hh->hcb (hh->hcb_cls,
           response_code,
           TALER_BANK_DIRECTION_NONE,
           0LLU,
           NULL,
           json);
  TALER_BANK_history_cancel (hh);
}


/**
 * Request the wire transfer history of a bank account.
 *
 * @param ctx curl context for the event loop
 * @param bank_base_url URL of the bank (used to execute this request)
 * @param auth authentication data to use
 * @param account_number which account number should we query
 * @param direction what kinds of wire transfers should be returned
 * @param start_row from which row on do we want to get results, use UINT64_MAX for the latest; exclusive
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
                    void *hres_cb_cls)
{
  struct TALER_BANK_HistoryHandle *hh;
  CURL *eh;
  char *url;

  if (0 == num_results)
  {
    GNUNET_break (0);
    return NULL;
  }
  if (TALER_BANK_DIRECTION_NONE == direction)
  {
    GNUNET_break (0);
    return NULL;
  }
  if (UINT64_MAX == start_row)
  {
    if (TALER_BANK_DIRECTION_BOTH == direction)
      GNUNET_asprintf (&url,
                       "/history?auth=basic&account_number=%llu&delta=%lld",
                       (unsigned long long) account_number,
                       (long long) num_results);
    else
      GNUNET_asprintf (&url,
                       "/history?auth=basic&account_number=%llu&delta=%lld&direction=%s",
                       (unsigned long long) account_number,
                       (long long) num_results,
                       (TALER_BANK_DIRECTION_CREDIT == direction) ? "credit" : "debit");

  }
  else
  {
    if (TALER_BANK_DIRECTION_BOTH == direction)
      GNUNET_asprintf (&url,
                       "/history?auth=basic&account_number=%llu&delta=%lld&start_row=%llu",
                       (unsigned long long) account_number,
                       (long long) num_results,
                       (unsigned long long) start_row);
    else
      GNUNET_asprintf (&url,
                       "/history?auth=basic&account_number=%llu&delta=%lld&start_row=%llu&direction=%s",
                       (unsigned long long) account_number,
                       (long long) num_results,
                       (unsigned long long) start_row,
                       (TALER_BANK_DIRECTION_CREDIT == direction) ? "credit" : "debit");
  }

  hh = GNUNET_new (struct TALER_BANK_HistoryHandle);
  hh->hcb = hres_cb;
  hh->hcb_cls = hres_cb_cls;
  hh->bank_base_url = GNUNET_strdup (bank_base_url);
  hh->request_url = TALER_BANK_path_to_url_ (bank_base_url,
                                             url);
  GNUNET_free (url);
  hh->authh = TALER_BANK_make_auth_header_ (auth);
  eh = curl_easy_init ();
  GNUNET_assert (CURLE_OK ==
                 curl_easy_setopt (eh,
                                   CURLOPT_HTTPHEADER,
                                   hh->authh));
  GNUNET_assert (CURLE_OK ==
                 curl_easy_setopt (eh,
                                   CURLOPT_URL,
                                   hh->request_url));
  hh->job = GNUNET_CURL_job_add (ctx,
                                 eh,
                                 GNUNET_NO,
                                 &handle_history_finished,
                                 hh);
  return hh;
}


/**
 * Cancel a history request.  This function cannot be used on a request
 * handle if a response is already served for it.
 *
 * @param hh the history request handle
 */
void
TALER_BANK_history_cancel (struct TALER_BANK_HistoryHandle *hh)
{
  if (NULL != hh->job)
  {
    GNUNET_CURL_job_cancel (hh->job);
    hh->job = NULL;
  }
  curl_slist_free_all (hh->authh);
  GNUNET_free (hh->request_url);
  GNUNET_free (hh->bank_base_url);
  GNUNET_free (hh);
}


/* end of bank_api_history.c */