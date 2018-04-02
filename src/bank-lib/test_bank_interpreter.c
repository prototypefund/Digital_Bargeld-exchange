/*
  This file is part of TALER
  Copyright (C) 2016, 2017 GNUnet e.V.

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
 * @file bank/test_bank_interpreter.c
 * @brief interpreter for tests of the bank's HTTP API interface
 * @author Christian Grothoff
 */
#include "platform.h"
#include "taler_util.h"
#include "taler_signatures.h"
#include "taler_bank_service.h"
#include <gnunet/gnunet_util_lib.h>
#include <gnunet/gnunet_curl_lib.h>
#include <microhttpd.h>
#include "test_bank_interpreter.h"
#include "taler_fakebank_lib.h"


/**
 * State of the interpreter loop.
 */
struct InterpreterState
{
  /**
   * Keys from the bank.
   */
  const struct TALER_BANK_Keys *keys;

  /**
   * Commands the interpreter will run.
   */
  struct TBI_Command *commands;

  /**
   * Interpreter task (if one is scheduled).
   */
  struct GNUNET_SCHEDULER_Task *task;

  /**
   * Main execution context for the main loop.
   */
  struct GNUNET_CURL_Context *ctx;

  /**
   * Task run on timeout.
   */
  struct GNUNET_SCHEDULER_Task *timeout_task;

  /**
   * Context for running the main loop with GNUnet's SCHEDULER API.
   */
  struct GNUNET_CURL_RescheduleContext *rc;

  /**
   * Where to store the final result.
   */
  int *resultp;

  /**
   * Fakebank, or NULL if we are not using the fakebank.
   */
  struct TALER_FAKEBANK_Handle *fakebank;

  /**
   * Instruction pointer.  Tells #interpreter_run() which
   * instruction to run next.
   */
  unsigned int ip;

};


/**
 * The testcase failed, return with an error code.
 *
 * @param is interpreter state to clean up
 */
static void
fail (struct InterpreterState *is)
{
  *is->resultp = GNUNET_SYSERR;
  GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
              "Interpreter failed at command `%s'\n",
              is->commands[is->ip].label);
  GNUNET_SCHEDULER_shutdown ();
}


/**
 * Find a command by label.
 *
 * @param is interpreter state to search
 * @param label label to look for
 * @return NULL if command was not found
 */
static const struct TBI_Command *
find_command (const struct InterpreterState *is,
              const char *label)
{
  const struct TBI_Command *cmd;

  if (NULL == label)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                "Attempt to lookup command for empty label\n");
    return NULL;
  }
  for (unsigned int i=0;TBI_OC_END != (cmd = &is->commands[i])->oc;i++)
    if ( (NULL != cmd->label) &&
         (0 == strcmp (cmd->label,
                       label)) )
      return cmd;
  GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
              "Command not found: %s\n",
              label);
  return NULL;
}


/**
 * Test if the /admin/add/incoming transaction at offset @a off
 * has been /rejected.
 *
 * @param is interpreter state (where we are right now)
 * @param off offset of the command to test for rejection
 * @return #GNUNET_YES if the command at @a off was cancelled
 */
static int
test_cancelled (struct InterpreterState *is,
                unsigned int off)
{
  const struct TBI_Command *cmd = &is->commands[off];

  for (unsigned int i=0;i<is->ip;i++)
  {
    const struct TBI_Command *c = &is->commands[i];

    if (TBI_OC_REJECT != c->oc)
      continue;
    if (0 == strcmp (c->details.reject.cmd_ref,
                     cmd->label))
      return GNUNET_YES;
  }
  return GNUNET_NO;
}


/**
 * Item in the transaction history, as reconstructed from the
 * command history.
 */
struct History
{

  /**
   * Wire details.
   */
  struct TALER_BANK_TransferDetails details;

  /**
   * Serial ID of the wire transfer.
   */
  uint64_t row_id;

  /**
   * Direction of the transfer.
   */
  enum TALER_BANK_Direction direction;

};


/**
 * Build history of transactions matching the current
 * command in @a is.
 *
 * @param is interpreter state
 * @param[out] rh history array to initialize
 * @return number of entries in @a rh
 */
static uint64_t
build_history (struct InterpreterState *is,
               struct History **rh)
{
  const struct TBI_Command *cmd = &is->commands[is->ip];
  uint64_t total;
  struct History *h;
  const struct TBI_Command *ref;
  int inc;
  unsigned int start;
  unsigned int end;
  int ok;

  GNUNET_assert (TBI_OC_HISTORY == cmd->oc);
  if (NULL != cmd->details.history.start_row_ref)
  {
    ref = find_command (is,
                        cmd->details.history.start_row_ref);
    GNUNET_assert (NULL != ref);
  }
  else
  {
    ref = NULL;
  }
  GNUNET_assert (0 != cmd->details.history.num_results);
  if (0 == is->ip)
  {
    *rh = NULL;
    return 0;
  }
  if (cmd->details.history.num_results > 0)
  {
    inc = 1;
    start = 0;
    end = is->ip - 1;
  }
  else
  {
    inc = -1;
    start = is->ip - 1;
    end = 0;
  }

  total = 0;
  ok = GNUNET_NO;
  if (NULL == ref)
    ok = GNUNET_YES;
  for (unsigned int off = start;off != end + inc; off += inc)
  {
    const struct TBI_Command *pos = &is->commands[off];
    int cancelled;

    if (TBI_OC_ADMIN_ADD_INCOMING != pos->oc)
      continue;
    if ( (NULL != ref) &&
         (ref->details.admin_add_incoming.row_id ==
          pos->details.admin_add_incoming.row_id) )
    {
      total = 0;
      ok = GNUNET_YES;
      continue;
    }
    if (GNUNET_NO == ok)
      continue; /* skip until we find the marker */
    if (total >= cmd->details.history.num_results * inc)
      break; /* hit limit specified by command */
    cancelled = test_cancelled (is,
                                off);
    if ( (GNUNET_YES == cancelled) &&
         (0 == (cmd->details.history.direction & TALER_BANK_DIRECTION_CANCEL)) )
      continue;
    if ( ( (0 != (cmd->details.history.direction & TALER_BANK_DIRECTION_CREDIT)) &&
           (cmd->details.history.account_number ==
            pos->details.admin_add_incoming.credit_account_no)) ||
         ( (0 != (cmd->details.history.direction & TALER_BANK_DIRECTION_DEBIT)) &&
           (cmd->details.history.account_number ==
            pos->details.admin_add_incoming.debit_account_no)) )
      total++; /* found matching record */
  }
  GNUNET_assert (GNUNET_YES == ok);
  if (0 == total)
  {
    *rh = NULL;
    return 0;
  }
  GNUNET_assert (total < UINT_MAX);
  h = GNUNET_new_array ((unsigned int) total,
                        struct History);
  total = 0;
  ok = GNUNET_NO;
  if (NULL == ref)
    ok = GNUNET_YES;
  for (unsigned int off = start;off != end + inc; off += inc)
  {
    const struct TBI_Command *pos = &is->commands[off];
    int cancelled;

    if (TBI_OC_ADMIN_ADD_INCOMING != pos->oc)
      continue;
    if ( (NULL != ref) &&
         (ref->details.admin_add_incoming.row_id ==
          pos->details.admin_add_incoming.row_id) )
    {
      total = 0;
      ok = GNUNET_YES;
      continue;
    }
    if (GNUNET_NO == ok)
      continue; /* skip until we find the marker */
    if (total >= cmd->details.history.num_results * inc)
      break; /* hit limit specified by command */
    if ( ( (0 != (cmd->details.history.direction & TALER_BANK_DIRECTION_CREDIT)) &&
           (cmd->details.history.account_number ==
            pos->details.admin_add_incoming.credit_account_no)) &&
         ( (0 != (cmd->details.history.direction & TALER_BANK_DIRECTION_DEBIT)) &&
           (cmd->details.history.account_number ==
            pos->details.admin_add_incoming.debit_account_no)) )
    {
      GNUNET_break (0);
      continue;
    }

    cancelled = test_cancelled (is,
                                off);
    if ( (GNUNET_YES == cancelled) &&
         (0 == (cmd->details.history.direction & TALER_BANK_DIRECTION_CANCEL)) )
      continue;

    if ( (0 != (cmd->details.history.direction & TALER_BANK_DIRECTION_CREDIT)) &&
         (cmd->details.history.account_number ==
          pos->details.admin_add_incoming.credit_account_no))
    {
      h[total].direction = TALER_BANK_DIRECTION_CREDIT;
      if (GNUNET_YES == cancelled)
        h[total].direction |= TALER_BANK_DIRECTION_CANCEL;
      GNUNET_asprintf (&h[total].details.account_url,
                       "payto://x-taler-bank/%s/%llu",
                       "http://localhost:8080",
                       (unsigned long long) pos->details.admin_add_incoming.debit_account_no);
    }
    if ( (0 != (cmd->details.history.direction & TALER_BANK_DIRECTION_DEBIT)) &&
           (cmd->details.history.account_number ==
            pos->details.admin_add_incoming.debit_account_no))
    {
      h[total].direction = TALER_BANK_DIRECTION_DEBIT;
      if (GNUNET_YES == cancelled)
        h[total].direction |= TALER_BANK_DIRECTION_CANCEL;
      GNUNET_asprintf (&h[total].details.account_url,
                       "payto://x-taler-bank/%s/%llu",
                       "http://localhost:8080",
                       (unsigned long long) pos->details.admin_add_incoming.credit_account_no);
    }
    if ( ( (0 != (cmd->details.history.direction & TALER_BANK_DIRECTION_CREDIT)) &&
           (cmd->details.history.account_number ==
            pos->details.admin_add_incoming.credit_account_no)) ||
         ( (0 != (cmd->details.history.direction & TALER_BANK_DIRECTION_DEBIT)) &&
           (cmd->details.history.account_number ==
            pos->details.admin_add_incoming.debit_account_no)) )
    {
      GNUNET_assert (GNUNET_OK ==
                     TALER_string_to_amount (pos->details.admin_add_incoming.amount,
                                             &h[total].details.amount));
      /* h[total].execution_date; // unknown here */
      h[total].row_id
        = pos->details.admin_add_incoming.row_id;
      GNUNET_asprintf (&h[total].details.wire_transfer_subject,
                       "%s %s",
                       pos->details.admin_add_incoming.subject,
                       pos->details.admin_add_incoming.exchange_base_url);
      total++;
    }
  }
  *rh = h;
  return total;
}


/**
 * Log which history we expected.
 *
 * @param h what we expected
 * @param h_len number of entries in @a h
 * @param off position of the missmatch
 */
static void
print_expected (struct History *h,
                uint64_t h_len,
                unsigned int off)
{
  GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
              "Transaction history missmatch at position %u/%llu\n",
              off,
              (unsigned long long) h_len);
  GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
              "Expected history:\n");
  for (uint64_t i=0;i<h_len;i++)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "H(%llu): %s%s (serial: %llu, subject: %s, to: %s)\n",
                (unsigned long long) i,
                (TALER_BANK_DIRECTION_CREDIT == h[i].direction) ? "+" : "-",
                TALER_amount2s (&h[i].details.amount),
                (unsigned long long) h[i].row_id,
                h[i].details.wire_transfer_subject,
                h[i].details.account_url);
  }
}


/**
 * Free history @a h of length @a h_len.
 *
 * @param h history array to free
 * @param h_len number of entries in @a h
 */
static void
free_history (struct History *h,
              uint64_t h_len)
{
  for (uint64_t off = 0;off<h_len;off++)
  {
    GNUNET_free (h[off].details.wire_transfer_subject);
    GNUNET_free (h[off].details.account_url);
  }
  GNUNET_free_non_null (h);
}


/**
 * Compute how many results we expect to be returned for
 * the history command at @a is.
 *
 * @param is the interpreter state to inspect
 * @return number of results expected
 */
static uint64_t
compute_result_count (struct InterpreterState *is)
{
  uint64_t total;
  struct History *h;

  total = build_history (is,
                         &h);
  free_history (h,
                total);
  return total;
}


/**
 * Check that @a dir and @a details are the transaction
 * results we expect at offset @a off in the history of
 * the current command executed by @a is
 *
 * @param is the interpreter state we are in
 * @param off the offset of the result
 * @param dir the direction of the transaction
 * @param details the transaction details to check
 * @return #GNUNET_OK if the transaction is what we expect
 */
static int
check_result (struct InterpreterState *is,
              unsigned int off,
              enum TALER_BANK_Direction dir,
              const struct TALER_BANK_TransferDetails *details)
{
  uint64_t total;
  struct History *h;

  total = build_history (is,
                         &h);
  if (off >= total)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Test says history has at most %u results, but got result #%u to check\n",
                (unsigned int) total,
                off);
    print_expected (h, total, off);
    return GNUNET_SYSERR;
  }
  if (h[off].direction != dir)
  {
    GNUNET_break (0);
    print_expected (h, total, off);
    free_history (h,
                  total);
    return GNUNET_SYSERR;
  }

  if ( (0 != strcmp (h[off].details.wire_transfer_subject,
                     details->wire_transfer_subject)) ||
       (0 != TALER_amount_cmp (&h[off].details.amount,
                               &details->amount)) ||
       (0 != strcasecmp (h[off].details.account_url,
                         details->account_url)) )
  {
    GNUNET_break (0);
    print_expected (h, total, off);
    free_history (h,
                  total);
    return GNUNET_SYSERR;
  }
  free_history (h,
                total);
  return GNUNET_OK;
}


/**
 * Run the main interpreter loop that performs bank operations.
 *
 * @param cls contains the `struct InterpreterState`
 */
static void
interpreter_run (void *cls);


/**
 * Run the next command.
 *
 * @param is interpreter to progress
 */
static void
next (struct InterpreterState *is)
{
  is->ip++;
  is->task = GNUNET_SCHEDULER_add_now (&interpreter_run,
                                       is);
}


/**
 * Function called upon completion of our /admin/add/incoming request.
 *
 * @param cls closure with the interpreter state
 * @param http_status HTTP response code, #MHD_HTTP_OK (200) for successful status request
 *                    0 if the bank's reply is bogus (fails to follow the protocol)
 * @param ec taler status code
 * @param row_id unique ID of the wire transfer in the bank's records; UINT64_MAX on error
 * @param json detailed response from the HTTPD, or NULL if reply was not in JSON
 */
static void
add_incoming_cb (void *cls,
                 unsigned int http_status,
                 enum TALER_ErrorCode ec,
                 uint64_t row_id,
                 const json_t *json)
{
  struct InterpreterState *is = cls;
  struct TBI_Command *cmd = &is->commands[is->ip];

  cmd->details.admin_add_incoming.aih = NULL;
  cmd->details.admin_add_incoming.row_id = row_id;
  if (cmd->details.admin_add_incoming.expected_response_code != http_status)
  {
    GNUNET_break (0);
    fprintf (stderr,
             "Unexpected response code %u:\n",
             http_status);
    if (NULL != json)
    {
      json_dumpf (json, stderr, 0);
      fprintf (stderr, "\n");
    }
    fail (is);
    return;
  }
  next (is);
}


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
 * @param ec taler status code
 * @param dir direction of the transfer
 * @param row_id monotonically increasing counter corresponding to the transaction
 * @param details details about the wire transfer
 * @param json detailed response from the HTTPD, or NULL if reply was not in JSON
 */
static void
history_cb (void *cls,
            unsigned int http_status,
            enum TALER_ErrorCode ec,
            enum TALER_BANK_Direction dir,
            uint64_t row_id,
            const struct TALER_BANK_TransferDetails *details,
            const json_t *json)
{
  struct InterpreterState *is = cls;
  struct TBI_Command *cmd = &is->commands[is->ip];

  if (MHD_HTTP_OK != http_status)
  {
    cmd->details.history.hh = NULL;
    if ( (cmd->details.history.results_obtained !=
          compute_result_count (is)) ||
         (GNUNET_YES ==
          cmd->details.history.failed) )
    {
      uint64_t total;
      struct History *h;

      GNUNET_break (0);
      total = build_history (is,
                             &h);
      GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                  "Expected history of length %llu, got %llu\n",
                  (unsigned long long) total,
                  (unsigned long long) cmd->details.history.results_obtained);
      print_expected (h,
                      total,
                      UINT_MAX);
      free_history (h,
                    total);
      fail (is);
      return;
    }
    next (is);
    return;
  }
  if (GNUNET_OK !=
      check_result (is,
                    cmd->details.history.results_obtained,
                    dir,
                    details))
  {
    GNUNET_break (0);
    {
      char *acc;

      acc = json_dumps (json,
                        JSON_COMPACT);
      GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                  "Result %u was `%s'\n",
                  (unsigned int) cmd->details.history.results_obtained,
                  acc);
      if (NULL != acc)
        free (acc);
    }

    cmd->details.history.failed = GNUNET_YES;
    return;
  }
  cmd->details.history.results_obtained++;
}


/**
 * Callbacks of this type are used to serve the result of asking
 * the bank to reject an incoming wire transfer.
 *
 * @param cls closure
 * @param http_status HTTP response code, #MHD_HTTP_NO_CONTENT (204) for successful status request;
 *                    #MHD_HTTP_NOT_FOUND if the rowid is unknown;
 *                    0 if the bank's reply is bogus (fails to follow the protocol),
 * @param ec detailed error code
 */
static void
reject_cb (void *cls,
           unsigned int http_status,
           enum TALER_ErrorCode ec)
{
  struct InterpreterState *is = cls;
  struct TBI_Command *cmd = &is->commands[is->ip];

  cmd->details.reject.rh = NULL;
  if (MHD_HTTP_NO_CONTENT != http_status)
  {
    GNUNET_break (0);
    fprintf (stderr,
             "Unexpected response code %u:\n",
             http_status);
    fail (is);
    return;
  }
  next (is);
}


/**
 * Run the main interpreter loop that performs bank operations.
 *
 * @param cls contains the `struct InterpreterState`
 */
static void
interpreter_run (void *cls)
{
  struct InterpreterState *is = cls;
  struct TBI_Command *cmd = &is->commands[is->ip];
  const struct TBI_Command *ref;
  struct TALER_Amount amount;
  const struct GNUNET_SCHEDULER_TaskContext *tc;
  struct TALER_BANK_AuthenticationData auth;
  uint64_t rowid;

  is->task = NULL;
  tc = GNUNET_SCHEDULER_get_task_context ();
  if (0 != (tc->reason & GNUNET_SCHEDULER_REASON_SHUTDOWN))
  {
    fprintf (stderr,
             "Test aborted by shutdown request\n");
    fail (is);
    return;
  }
  auth.method = TALER_BANK_AUTH_BASIC; /* or "NONE"? */
  auth.details.basic.username = "Exchange";
  auth.details.basic.password = "x";
  switch (cmd->oc)
  {
  case TBI_OC_END:
    *is->resultp = GNUNET_OK;
    GNUNET_SCHEDULER_shutdown ();
    return;
  case TBI_OC_ADMIN_ADD_INCOMING:
    if (GNUNET_OK !=
        TALER_string_to_amount (cmd->details.admin_add_incoming.amount,
                                &amount))
    {
      GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                  "Failed to parse amount `%s' at %u\n",
                  cmd->details.admin_add_incoming.amount,
                  is->ip);
      fail (is);
      return;
    }
    GNUNET_break (NULL != cmd->details.admin_add_incoming.subject);
    switch (cmd->details.admin_add_incoming.debit_account_no)
    {
    case 1:
      auth.details.basic.username = "Bank";
      break;
    case 2:
      auth.details.basic.username = "Exchange";
      break;
    case 3:
      auth.details.basic.username = "user3";
      auth.details.basic.password = "pass3";
      break;
    default:
      GNUNET_break (0);
      break;
    }
    cmd->details.admin_add_incoming.aih
      = TALER_BANK_admin_add_incoming (is->ctx,
                                       "http://localhost:8080",
                                       &auth,
                                       cmd->details.admin_add_incoming.exchange_base_url,
                                       cmd->details.admin_add_incoming.subject,
                                       &amount,
                                       cmd->details.admin_add_incoming.debit_account_no,
                                       cmd->details.admin_add_incoming.credit_account_no,
                                       &add_incoming_cb,
                                       is);
    if (NULL == cmd->details.admin_add_incoming.aih)
    {
      GNUNET_break (0);
      fail (is);
      return;
    }
    return;
  case TBI_OC_HISTORY:
    if (NULL != cmd->details.history.start_row_ref)
    {
      /*In case history is to be found from some other commad's
      output, like from /admin/add/incoming*/
      ref = find_command (is,
                          cmd->details.history.start_row_ref);
      GNUNET_assert (NULL != ref);
    }
    else
    {
      ref = NULL;
    }
    if (NULL != ref)
      rowid = ref->details.admin_add_incoming.row_id;
    else
      rowid = UINT64_MAX;
    switch (cmd->details.history.account_number)
    {
    case 1:
      auth.details.basic.username = "Bank";
      break;
    case 2:
      auth.details.basic.username = "Exchange";
      break;
    default:
      GNUNET_break (0);
      break;
    }
    cmd->details.history.hh
      = TALER_BANK_history (is->ctx,
                            "http://localhost:8080",
                            &auth,
                            cmd->details.history.account_number,
                            cmd->details.history.direction,
                            rowid,
                            cmd->details.history.num_results,
                            &history_cb,
                            is);
    if (NULL == cmd->details.history.hh)
    {
      GNUNET_break (0);
      fail (is);
      return;
    }
    return;
  case TBI_OC_EXPECT_TRANSFER:
    ref = find_command (is,
                        cmd->details.expect_transfer.cmd_ref);
    GNUNET_assert (NULL != ref);
    GNUNET_assert (GNUNET_OK ==
                   TALER_string_to_amount (ref->details.admin_add_incoming.amount,
                                           &amount));
    {
      char *subject;

      if (GNUNET_OK !=
          TALER_FAKEBANK_check (is->fakebank,
                                &amount,
                                ref->details.admin_add_incoming.debit_account_no,
                                ref->details.admin_add_incoming.credit_account_no,
                                ref->details.admin_add_incoming.exchange_base_url,
                                &subject))
      {
        GNUNET_break (0);
        fail (is);
        return;
      }
      if (0 != strcmp (ref->details.admin_add_incoming.subject,
                       subject))
      {
        GNUNET_free (subject);
        GNUNET_break (0);
        fail (is);
        return;
      }
      GNUNET_free (subject);
    }
    next (is);
   return;
  case TBI_OC_EXPECT_TRANSFERS_EMPTY:
    if (GNUNET_OK != TALER_FAKEBANK_check_empty (is->fakebank))
    {
      GNUNET_break (0);
      fail (is);
      return;
    }
    next (is);
    return;
  case TBI_OC_REJECT:
    ref = find_command (is,
                        cmd->details.reject.cmd_ref);
    GNUNET_assert (NULL != ref);
    GNUNET_assert (TBI_OC_ADMIN_ADD_INCOMING == ref->oc);
    switch (ref->details.admin_add_incoming.credit_account_no)
    {
    case 1:
      auth.details.basic.username = "Bank";
      break;
    case 2:
      auth.details.basic.username = "Exchange";
      break;
    default:
      GNUNET_break (0);
      break;
    }
    cmd->details.reject.rh
      = TALER_BANK_reject (is->ctx,
                           "http://localhost:8080",
                           &auth,
                           ref->details.admin_add_incoming.credit_account_no,
                           ref->details.admin_add_incoming.row_id,
                           &reject_cb,
                           is);
    if (NULL == cmd->details.reject.rh)
    {
      GNUNET_break (0);
      fail (is);
      return;
    }
    return;
  default:
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Unknown instruction %d at %u (%s)\n",
                cmd->oc,
                is->ip,
                cmd->label);
    fail (is);
    return;
  }
}


/**
 * Function run on timeout.
 *
 * @param cls the `struct InterpreterState`
 */
static void
do_timeout (void *cls)
{
  struct InterpreterState *is = cls;

  is->timeout_task = NULL;
  GNUNET_SCHEDULER_shutdown ();
}


/**
 * Function run when the test terminates (good or bad).
 * Cleans up our state.
 *
 * @param cls the interpreter state.
 */
static void
do_shutdown (void *cls)
{
  struct InterpreterState *is = cls;
  struct TBI_Command *cmd;

  if (NULL != is->timeout_task)
  {
    GNUNET_SCHEDULER_cancel (is->timeout_task);
    is->timeout_task = NULL;
  }

  for (unsigned int i=0;TBI_OC_END != (cmd = &is->commands[i])->oc;i++)
  {
    switch (cmd->oc)
    {
    case TBI_OC_END:
      GNUNET_assert (0);
      break;
    case TBI_OC_ADMIN_ADD_INCOMING:
      if (NULL != cmd->details.admin_add_incoming.aih)
      {
        GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                    "Command %u (%s) did not complete\n",
                    i,
                    cmd->label);
        TALER_BANK_admin_add_incoming_cancel (cmd->details.admin_add_incoming.aih);
        cmd->details.admin_add_incoming.aih = NULL;
      }
      break;
    case TBI_OC_HISTORY:
      if (NULL != cmd->details.history.hh)
      {
        GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                    "Command %u (%s) did not complete\n",
                    i,
                    cmd->label);
        TALER_BANK_history_cancel (cmd->details.history.hh);
        cmd->details.history.hh = NULL;
      }
      break;
    case TBI_OC_EXPECT_TRANSFER:
      break;
    case TBI_OC_EXPECT_TRANSFERS_EMPTY:
      break;
    case TBI_OC_REJECT:
      if (NULL != cmd->details.reject.rh)
      {
        GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                    "Command %u (%s) did not complete\n",
                    i,
                    cmd->label);
        TALER_BANK_reject_cancel (cmd->details.reject.rh);
        cmd->details.reject.rh = NULL;
      }
      break;
    default:
      GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                  "Unknown instruction %d at %u (%s)\n",
                  cmd->oc,
                  i,
                  cmd->label);
      break;
    }
  }
  if (NULL != is->task)
  {
    GNUNET_SCHEDULER_cancel (is->task);
    is->task = NULL;
  }
  if (NULL != is->fakebank)
  {
    TALER_FAKEBANK_stop (is->fakebank);
    is->fakebank = NULL;
  }
  GNUNET_CURL_fini (is->ctx);
  is->ctx = NULL;
  GNUNET_CURL_gnunet_rc_destroy (is->rc);
  GNUNET_free (is);
}


/**
 * Entry point to the interpeter.
 *
 * @param resultp where to store the final result
 * @param run_bank #GNUNET_YES to run the fakebank
 * @param commands list of commands to run
 */
void
TBI_run_interpreter (int *resultp,
                     int run_bank,
                     struct TBI_Command *commands)
{
  struct InterpreterState *is;

  is = GNUNET_new (struct InterpreterState);
  if (GNUNET_YES == run_bank)
    is->fakebank = TALER_FAKEBANK_start (8080);
  is->resultp = resultp;
  is->commands = commands;
  is->ctx = GNUNET_CURL_init (&GNUNET_CURL_gnunet_scheduler_reschedule,
                              &is->rc);
  GNUNET_assert (NULL != is->ctx);
  is->rc = GNUNET_CURL_gnunet_rc_create (is->ctx);
  is->task = GNUNET_SCHEDULER_add_now (&interpreter_run,
                                       is);
  is->timeout_task
    = GNUNET_SCHEDULER_add_delayed (GNUNET_TIME_relative_multiply
                                    (GNUNET_TIME_UNIT_SECONDS, 150),
                                    &do_timeout, is);
  GNUNET_SCHEDULER_add_shutdown (&do_shutdown, is);
}

/* end of test_bank_interpeter.c */
