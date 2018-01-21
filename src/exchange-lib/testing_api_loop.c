/*
  This file is part of TALER
  Copyright (C) 2018 Taler Systems SA

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
 * @file exchange-lib/testing_api_loop.c
 * @brief main interpreter loop for testcases
 * @author Christian Grothoff
 * @author Marcello Stanisci
 */
#include "platform.h"
#include "taler_json_lib.h"
#include <gnunet/gnunet_curl_lib.h>
#include "exchange_api_handle.h"
#include "taler_signatures.h"
#include "taler_testing_lib.h"

/**
 * Global state of the interpreter, used by a command
 * to access information about other commands.
 */
struct TALER_TESTING_Interpreter
{

  /**
   * Commands the interpreter will run.
   */
  struct TALER_TESTING_Command *commands;

  /**
   * Interpreter task (if one is scheduled).
   */
  struct GNUNET_SCHEDULER_Task *task;

  /**
   * Main execution context for the main loop.
   */
  struct GNUNET_CURL_Context *ctx;

  /**
   * Context for running the CURL event loop.
   */
  struct GNUNET_CURL_RescheduleContext *rc;

  /**
   * Task run on timeout.
   */
  struct GNUNET_SCHEDULER_Task *timeout_task;

  /**
   * Instruction pointer.  Tells #interpreter_run() which
   * instruction to run next.
   */
  unsigned int ip;

  /**
   * Result of the testcases, #GNUNET_OK on success
   */
  int result;

  /**
   * Pipe used to communicate child death via signal.
   */
  struct GNUNET_DISK_PipeHandle *sigpipe;

};


/**
 * Lookup command by label.
 *
 * @param is interpreter state to search
 * @param label label to look for
 * @return NULL if command was not found
 */
const struct TALER_TESTING_Command *
TALER_TESTING_interpreter_lookup_command (struct TALER_TESTING_Interpreter *is,
                                          const char *label)
{
  const struct TALER_TESTING_Command *cmd;

  if (NULL == label)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                "Attempt to lookup command for empty label\n");
    return NULL;
  }
  for (unsigned int i=0;NULL != (cmd = &is->commands[i])->label;i++)
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
 * Obtain main execution context for the main loop.
 */
struct GNUNET_CURL_Context *
TALER_TESTING_interpreter_get_context (struct TALER_TESTING_Interpreter *i)
{
  return i->ctx;
}


/**
 * Run the main interpreter loop that performs exchange operations.
 *
 * @param cls contains the `struct InterpreterState`
 */
static void
interpreter_run (void *cls);


/**
 * Current command is done, run the next one.
 */
void
TALER_TESTING_interpreter_next (struct TALER_TESTING_Interpreter *i)
{
  if (GNUNET_SYSERR == i->result)
    return; /* ignore, we already failed! */
  i->ip++;
  i->task = GNUNET_SCHEDULER_add_now (&interpreter_run,
                                      i);
}


/**
 * Current command failed, clean up and fail the test case.
 */
void
TALER_TESTING_interpreter_fail (struct TALER_TESTING_Interpreter *i)
{
  i->result = GNUNET_SYSERR;
  GNUNET_SCHEDULER_shutdown ();
}


/**
 * Create command array terminator.
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_end (void)
{
  static struct TALER_TESTING_Command cmd;

  return cmd;
}


/**
 * Run the main interpreter loop that performs exchange operations.
 *
 * @param cls contains the `struct TALER_TESTING_Interpreter`
 */
static void
interpreter_run (void *cls)
{
  struct TALER_TESTING_Interpreter *is = cls;
  struct TALER_TESTING_Command *cmd = &is->commands[is->ip];

  is->task = NULL;

  if (NULL == cmd->label)
  {
    is->result = GNUNET_OK;
    GNUNET_SCHEDULER_shutdown ();
    return;
  }
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Running command `%s'\n",
              cmd->label);
  cmd->run (cmd->cls,
            cmd,
            is);
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
  struct TALER_TESTING_Interpreter *is = cls;
  struct TALER_TESTING_Command *cmd;

  GNUNET_log (GNUNET_ERROR_TYPE_INFO,
              "Executing shutdown at `%s'\n",
              is->commands[is->ip].label);
  for (unsigned int j=0;NULL != (cmd = &is->commands[j])->label;j++)
    cmd->cleanup (cmd->cls,
                  cmd);
  if (NULL != is->task)
  {
    GNUNET_SCHEDULER_cancel (is->task);
    is->task = NULL;
  }
  if (NULL != is->ctx)
  {
    GNUNET_CURL_fini (is->ctx);
    is->ctx = NULL;
  }
  if (NULL != is->rc)
  {
    GNUNET_CURL_gnunet_rc_destroy (is->rc);
    is->rc = NULL;
  }
  if (NULL != is->timeout_task)
  {
    GNUNET_SCHEDULER_cancel (is->timeout_task);
    is->timeout_task = NULL;
  }
  GNUNET_free_non_null (is->commands);
}


/**
 * Function run when the test terminates (good or bad) with timeout.
 *
 * @param cls NULL
 */
static void
do_timeout (void *cls)
{
  struct TALER_TESTING_Interpreter *is = cls;

  is->timeout_task = NULL;
  GNUNET_SCHEDULER_shutdown ();
}


void
TALER_TESTING_run (struct TALER_TESTING_Interpreter *is,
                   struct TALER_TESTING_Command *commands)
{
  unsigned int i;

  for (i=0;NULL != commands[i].label;i++) ;
  is->commands = GNUNET_new_array (i + 1,
                                   struct TALER_TESTING_Command);
  memcpy (is->commands,
          commands,
          sizeof (struct TALER_TESTING_Command) * i);
  is->timeout_task
    = GNUNET_SCHEDULER_add_delayed (GNUNET_TIME_relative_multiply
                                    (GNUNET_TIME_UNIT_SECONDS, 300),
                                    &do_timeout,
                                    is);
  GNUNET_SCHEDULER_add_shutdown (&do_shutdown,
                                 is);
  is->task = GNUNET_SCHEDULER_add_now (&interpreter_run,
                                      is);
}


struct MainContext
{
  TALER_TESTING_Main main_cb;

  void *main_cb_cls;

  struct TALER_TESTING_Interpreter *is;

};


static void
main_wrapper (void *cls)
{
  struct MainContext *main_ctx = cls;
  struct TALER_TESTING_Interpreter *is = main_ctx->is;

  is->ctx = GNUNET_CURL_init (&GNUNET_CURL_gnunet_scheduler_reschedule,
                              &is->rc);
  GNUNET_assert (NULL != is->ctx);
  is->rc = GNUNET_CURL_gnunet_rc_create (is->ctx);
  main_ctx->main_cb (main_ctx->main_cb_cls,
                     main_ctx->is);
}


/**
 * Initialize scheduler loop and curl context for the testcase.
 */
int
TALER_TESTING_setup (TALER_TESTING_Main main_cb,
                     void *main_cb_cls)
{
  struct TALER_TESTING_Interpreter is;
  struct MainContext main_ctx = {
    .main_cb = main_cb,
    .main_cb_cls = main_cb_cls,
    .is = &is
  };

  memset (&is,
          0,
          sizeof (is));
  GNUNET_SCHEDULER_run (&main_wrapper,
                        &main_ctx);
  return is.result;
}


/* end of testing_api_loop.c */