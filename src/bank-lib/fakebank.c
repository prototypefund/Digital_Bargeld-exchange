/*
  This file is part of TALER
  (C) 2016 Inria and GNUnet e.V.

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
 * @file bank-lib/fakebank.c
 * @brief library that fakes being a Taler bank for testcases
 * @author Christian Grothoff <christian@grothoff.org>
 */

#include "platform.h"
#include "fakebank.h"

/**
 * Maximum POST request size (for /admin/add/incoming)
 */
#define REQUEST_BUFFER_MAX (4*1024)



/**
 * Details about a transcation we (as the simulated bank) received.
 */
struct Transaction
{

  /**
   * We store transactions in a DLL.
   */
  struct Transaction *next;

  /**
   * We store transactions in a DLL.
   */
  struct Transaction *prev;

  /**
   * Amount to be transferred.
   */
  struct TALER_Amount amount;

  /**
   * Account to debit.
   */
  uint64_t debit_account;

  /**
   * Account to credit.
   */
  uint64_t credit_account;

  /**
   * Subject of the transfer.
   */
  struct TALER_WireTransferIdentifierRawP wtid;
};


/**
 * Handle for the fake bank.
 */
struct FAKEBANK_Handle
{
  /**
   * We store transactions in a DLL.
   */
  struct Transaction *transactions_head;

  /**
   * We store transactions in a DLL.
   */
  struct Transaction *transactions_tail;

  /**
   * HTTP server we run to pretend to be the "test" bank.
   */
  struct MHD_Daemon *mhd_bank;

  /**
   * Task running HTTP server for the "test" bank.
   */
  struct GNUNET_SCHEDULER_Task *mhd_task;
};


/**
 * Check that the @a want_amount was transferred from
 * the @a want_debit to the @a want_credit account.  If
 * so, set the @a wtid to the transfer identifier.
 * If not, return #GNUNET_SYSERR.
 *
 * @param h bank instance
 * @param want_amount transfer amount desired
 * @param want_debit account that should have been debited
 * @param want_debit account that should have been credited
 * @param[out] wtid set to the wire transfer identifier
 * @return #GNUNET_OK on success
 */
int
FAKEBANK_check (struct FAKEBANK_Handle *h,
                const struct TALER_Amount *want_amount,
                uint64_t want_debit,
                uint64_t want_credit,
                struct TALER_WireTransferIdentifierRawP *wtid)
{
  struct Transaction *t;

  for (t = h->transactions_head; NULL != t; t = t->next)
  {
    if ( (want_debit == t->debit_account) &&
         (want_credit == t->credit_account) &&
         (0 == TALER_amount_cmp (want_amount,
                                 &t->amount)) )
    {
      GNUNET_CONTAINER_DLL_remove (h->transactions_head,
                                   h->transactions_tail,
                                   t);
      *wtid = t->wtid;
      GNUNET_free (t);
      return GNUNET_OK;
    }
  }
  fprintf (stderr,
           "Did not find matching transaction!\nI have:\n");
  for (t = h->transactions_head; NULL != t; t = t->next)
  {
    char *s;

    s = TALER_amount_to_string (&t->amount);
    fprintf (stderr,
             "%llu -> %llu (%s)\n",
             (unsigned long long) t->debit_account,
             (unsigned long long) t->credit_account,
             s);
    GNUNET_free (s);
  }
  return GNUNET_SYSERR;
}


/**
 * Check that no wire transfers were ordered (or at least none
 * that have not been taken care of via #FAKEBANK_check()).
 * If any transactions are onrecord, return #GNUNET_SYSERR.
 *
 * @param h bank instance
 * @return #GNUNET_OK on success
 */
int
FAKEBANK_check_empty (struct FAKEBANK_Handle *h)
{
  struct Transaction *t;

  if (NULL == h->transactions_head)
    return GNUNET_OK;

  fprintf (stderr,
           "Expected empty transaction set, but I have:\n");
  for (t = h->transactions_head; NULL != t; t = t->next)
  {
    char *s;

    s = TALER_amount_to_string (&t->amount);
    fprintf (stderr,
             "%llu -> %llu (%s)\n",
             (unsigned long long) t->debit_account,
             (unsigned long long) t->credit_account,
             s);
    GNUNET_free (s);
  }
  return GNUNET_SYSERR;
}


/**
 * Stop running the fake bank.
 *
 * @param h bank to stop
 */
void
FAKEBANK_stop (struct FAKEBANK_Handle *h)
{
  if (NULL != h->mhd_task)
  {
    GNUNET_SCHEDULER_cancel (h->mhd_task);
    h->mhd_task = NULL;
  }
  if (NULL != h->mhd_bank)
  {
    MHD_stop_daemon (h->mhd_bank);
    h->mhd_bank = NULL;
  }
  GNUNET_free (h);
}


/**
 * Function called whenever MHD is done with a request.  If the
 * request was a POST, we may have stored a `struct Buffer *` in the
 * @a con_cls that might still need to be cleaned up.  Call the
 * respective function to free the memory.
 *
 * @param cls client-defined closure
 * @param connection connection handle
 * @param con_cls value as set by the last call to
 *        the #MHD_AccessHandlerCallback
 * @param toe reason for request termination
 * @see #MHD_OPTION_NOTIFY_COMPLETED
 * @ingroup request
 */
static void
handle_mhd_completion_callback (void *cls,
                                struct MHD_Connection *connection,
                                void **con_cls,
                                enum MHD_RequestTerminationCode toe)
{
  /*  struct FAKEBANK_Handle *h = cls; */

  GNUNET_JSON_post_parser_cleanup (*con_cls);
  *con_cls = NULL;
}


/**
 * Handle incoming HTTP request.
 *
 * @param cls closure for MHD daemon (unused)
 * @param connection the connection
 * @param url the requested url
 * @param method the method (POST, GET, ...)
 * @param version HTTP version (ignored)
 * @param upload_data request data
 * @param upload_data_size size of @a upload_data in bytes
 * @param con_cls closure for request (a `struct Buffer *`)
 * @return MHD result code
 */
static int
handle_mhd_request (void *cls,
                    struct MHD_Connection *connection,
                    const char *url,
                    const char *method,
                    const char *version,
                    const char *upload_data,
                    size_t *upload_data_size,
                    void **con_cls)
{
  struct FAKEBANK_Handle *h = cls;
  enum GNUNET_JSON_PostResult pr;
  json_t *json;
  struct Transaction *t;
  struct MHD_Response *resp;
  int ret;

  if (0 != strcasecmp (url,
                       "/admin/add/incoming"))
  {
    /* Unexpected URI path, just close the connection. */
    /* we're rather impolite here, but it's a testcase. */
    GNUNET_break_op (0);
    return MHD_NO;
  }
  pr = GNUNET_JSON_post_parser (REQUEST_BUFFER_MAX,
                                con_cls,
                                upload_data,
                                upload_data_size,
                                &json);
  switch (pr)
  {
  case GNUNET_JSON_PR_OUT_OF_MEMORY:
    GNUNET_break (0);
    return MHD_NO;
  case GNUNET_JSON_PR_CONTINUE:
    return MHD_YES;
  case GNUNET_JSON_PR_REQUEST_TOO_LARGE:
    GNUNET_break (0);
    return MHD_NO;
  case GNUNET_JSON_PR_JSON_INVALID:
    GNUNET_break (0);
    return MHD_NO;
  case GNUNET_JSON_PR_SUCCESS:
    break;
  }
  t = GNUNET_new (struct Transaction);
  {
    struct GNUNET_JSON_Specification spec[] = {
      GNUNET_JSON_spec_fixed_auto ("wtid", &t->wtid),
      GNUNET_JSON_spec_uint64 ("debit_account", &t->debit_account),
      GNUNET_JSON_spec_uint64 ("credit_account", &t->credit_account),
      TALER_JSON_spec_amount ("amount", &t->amount),
      GNUNET_JSON_spec_end ()
    };
    if (GNUNET_OK !=
        GNUNET_JSON_parse (json,
                           spec,
                           NULL, NULL))
    {
      GNUNET_break (0);
      json_decref (json);
      return MHD_NO;
    }
    GNUNET_CONTAINER_DLL_insert (h->transactions_head,
                                 h->transactions_tail,
                                 t);
  }
  GNUNET_log (GNUNET_ERROR_TYPE_INFO,
              "Receiving incoming wire transfer: %llu->%llu\n",
              (unsigned long long) t->debit_account,
              (unsigned long long) t->credit_account);
  json_decref (json);
  resp = MHD_create_response_from_buffer (0, "", MHD_RESPMEM_PERSISTENT);
  ret = MHD_queue_response (connection,
                            MHD_HTTP_OK,
                            resp);
  MHD_destroy_response (resp);
  return ret;
}


/**
 * Task run whenever HTTP server operations are pending.
 *
 * @param cls the `struct FAKEBANK_Handle`
 */
static void
run_mhd (void *cls);


/**
 * Schedule MHD.  This function should be called initially when an
 * MHD is first getting its client socket, and will then automatically
 * always be called later whenever there is work to be done.
 */
static void
schedule_httpd (struct FAKEBANK_Handle *h)
{
  fd_set rs;
  fd_set ws;
  fd_set es;
  struct GNUNET_NETWORK_FDSet *wrs;
  struct GNUNET_NETWORK_FDSet *wws;
  int max;
  int haveto;
  MHD_UNSIGNED_LONG_LONG timeout;
  struct GNUNET_TIME_Relative tv;

  FD_ZERO (&rs);
  FD_ZERO (&ws);
  FD_ZERO (&es);
  max = -1;
  if (MHD_YES != MHD_get_fdset (h->mhd_bank, &rs, &ws, &es, &max))
  {
    GNUNET_assert (0);
    return;
  }
  haveto = MHD_get_timeout (h->mhd_bank, &timeout);
  if (MHD_YES == haveto)
    tv.rel_value_us = (uint64_t) timeout * 1000LL;
  else
    tv = GNUNET_TIME_UNIT_FOREVER_REL;
  if (-1 != max)
  {
    wrs = GNUNET_NETWORK_fdset_create ();
    wws = GNUNET_NETWORK_fdset_create ();
    GNUNET_NETWORK_fdset_copy_native (wrs, &rs, max + 1);
    GNUNET_NETWORK_fdset_copy_native (wws, &ws, max + 1);
  }
  else
  {
    wrs = NULL;
    wws = NULL;
  }
  if (NULL != h->mhd_task)
    GNUNET_SCHEDULER_cancel (h->mhd_task);
  h->mhd_task =
    GNUNET_SCHEDULER_add_select (GNUNET_SCHEDULER_PRIORITY_DEFAULT,
                                 tv,
                                 wrs,
                                 wws,
                                 &run_mhd, h);
  if (NULL != wrs)
    GNUNET_NETWORK_fdset_destroy (wrs);
  if (NULL != wws)
    GNUNET_NETWORK_fdset_destroy (wws);
}


/**
 * Task run whenever HTTP server operations are pending.
 *
 * @param cls the `struct FAKEBANK_Handle`
 */
static void
run_mhd (void *cls)
{
  struct FAKEBANK_Handle *h = cls;

  h->mhd_task = NULL;
  MHD_run (h->mhd_bank);
  schedule_httpd (h);
}


/**
 * Start the fake bank.
 *
 * @param port port to listen to
 * @return NULL on error
 */
struct FAKEBANK_Handle *
FAKEBANK_start (uint16_t port)
{
  struct FAKEBANK_Handle *h;

  h = GNUNET_new (struct FAKEBANK_Handle);
  h->mhd_bank = MHD_start_daemon (MHD_USE_DEBUG,
                                  port,
                                  NULL, NULL,
                                  &handle_mhd_request, h,
                                  MHD_OPTION_NOTIFY_COMPLETED,
                                  &handle_mhd_completion_callback, h,
                                  MHD_OPTION_END);
  if (NULL == h->mhd_bank)
  {
    GNUNET_free (h);
    return NULL;
  }
  schedule_httpd (h);
  return h;
}
