/*
   This file is part of TALER
   Copyright (C) 2014, 2015 Christian Grothoff (and other contributing authors)

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
 * @file mintdb/perf_taler_mintdb_interpreter.c
 * @brief Interpreter library for mint database performance analysis
 * @author Nicolas Fournier
 */
#include "perf_taler_mintdb_interpreter.h"
#include "perf_taler_mintdb_init.h"
#include "../include/gauger.h"


/**
 * Represents the state of the interpreter
 */
struct PERF_TALER_MINTDB_interpreter_state
{
  /**
   * State of the commands
   */
  struct PERF_TALER_MINTDB_Cmd *cmd;

  /**
   * Database plugin
   */
  struct TALER_MINTDB_Plugin *plugin;

  /**
   * Current database session
   */
  struct TALER_MINTDB_Session *session;

  /**
   * The current index of the interpreter
   */
  int i;
};

/**
 * Free the memory of @a data, with data of type @a type
 */
static void
data_free (union PERF_TALER_MINTDB_Data *data, enum PERF_TALER_MINTDB_Type type){
  switch (type)
  {
    case PERF_TALER_MINTDB_DEPOSIT:
      deposit_free (data->deposit);
      data->deposit = NULL;
      return;

    default:
      return;
  }
} 



/**
 * Finds the first command in cmd with the name search
 *
 * @return the index of the first command with name search
 * GNUNET_SYSERR if none found
 */
static int
cmd_find (const struct  PERF_TALER_MINTDB_Cmd *cmd, const char *search)
{
  int i;

  for (i=0; PERF_TALER_MINTDB_CMD_END != cmd[i].command; i++)
    if (0 == strcmp (cmd[i].label, search))
      return i;
  return GNUNET_SYSERR;
}


/**
 * Initialization of a command array      
 */
static int
cmd_init (struct PERF_TALER_MINTDB_Cmd cmd[])
{
  int i = 0;
  for (i=0; PERF_TALER_MINTDB_CMD_END != cmd[i].command; i++)
  {
    switch (cmd[i].command)
    {
      case PERF_TALER_MINTDB_CMD_SAVE_ARRAY:
        // Allocation of memory for saving data
        cmd[i].details.save_array.data_saved = 
          GNUNET_new_array (cmd[i].details.save_array.nb_saved, 
                            union PERF_TALER_MINTDB_Data);
        break;

      case PERF_TALER_MINTDB_CMD_LOAD_ARRAY:
        // Creating the permutation array to randomize the data order
        cmd[i].details.load_array.permutation =
          GNUNET_CRYPTO_random_permute (
            GNUNET_CRYPTO_QUALITY_WEAK,
            cmd[cmd_find(cmd, cmd[i].details.load_array.label_save)].details.save_array.nb_saved);

        // Initializing the type based on the type of the saved array
        cmd[i].exposed_type = cmd[
          cmd_find (cmd, cmd[i].details.load_array.label_save)
        ].details.save_array.type_saved;
        break;

      default:
        break;
    }
  }
  return GNUNET_OK;
}


/**
 * Free the memory of the command chain
 */
static int
cmd_clean (struct PERF_TALER_MINTDB_Cmd cmd[])
{
  int i = 0;
  for (i=0; PERF_TALER_MINTDB_CMD_END != cmd[i].command; i++)
  {
    switch (cmd[i].command)
    {
      case PERF_TALER_MINTDB_CMD_SAVE_ARRAY:
        {
          int j;
          for (j = 0; j < cmd[i].details.save_array.nb_saved; j++)
          {
            data_free (&cmd[i].details.save_array.data_saved[j],
              cmd[i].details.save_array.type_saved);
          }

          GNUNET_free (cmd[i].details.save_array.data_saved);
          cmd[i].details.save_array.data_saved = NULL;
        }

      case PERF_TALER_MINTDB_CMD_INSERT_DEPOSIT:
        deposit_free (cmd[i].exposed.deposit);
        break;

      case PERF_TALER_MINTDB_CMD_LOAD_ARRAY:
        GNUNET_free (cmd[i].details.load_array.permutation);
        break;

      default:
        break;

    }
    i++;
  }
  return GNUNET_OK;
};


/**
 * Handles the command END_LOOP for the interpreter
 */
static void
interpret_end_loop (struct PERF_TALER_MINTDB_interpreter_state *state)
{
  int jump = cmd_find (state->cmd, state->cmd[state->i].details.end_loop.label_loop);
  // If the loop is not finished
  if (state->cmd[jump].details.loop.max_iterations > state->cmd[jump].details.loop.curr_iteration)
  {
    // jump back to the start
    state->i = jump -1;
  }else{
    // Reset the loop counter and continue running
    state->cmd[jump].details.loop.curr_iteration = -1;
  }
  // Cleaning up the memory in the loop
  int j;
  for (j = jump; j < state->i; j++)
  {
    // If the exposed variable has not been copied
    if ( 0 == state->cmd[j].exposed_saved)
    {
      // It is freed
      data_free (&state->cmd[j].exposed, state->cmd[j].exposed_type);
    }
    state->cmd[j].exposed_saved = 0;
  }
}



/**
 * /TODO cut it into pieces
 */
static int
interpret (struct PERF_TALER_MINTDB_interpreter_state *state)
{

  for (state->i=0; PERF_TALER_MINTDB_CMD_END != state->cmd[state->i].command; state->i++)
  {
    switch (state->cmd[state->i].command)
    {
      case PERF_TALER_MINTDB_CMD_END:
        return GNUNET_YES;

      case PERF_TALER_MINTDB_CMD_LOOP:
        state->cmd[state->i].details.loop.curr_iteration++;
        break;

      case PERF_TALER_MINTDB_CMD_END_LOOP:
        interpret_end_loop (state);
        break;

      case PERF_TALER_MINTDB_CMD_GET_TIME:
        clock_gettime (CLOCK_MONOTONIC, &state->cmd[state->i].exposed.time);
        break;

      case PERF_TALER_MINTDB_CMD_GAUGER:
        {
          int start_index = cmd_find (state->cmd, state->cmd[state->i].details.gauger.label_start);
          int stop_index  = cmd_find (state->cmd, state->cmd[state->i].details.gauger.label_stop);
          struct timespec start = state->cmd [start_index].exposed.time;
          struct timespec stop = state->cmd [stop_index].exposed.time;

          unsigned long elapsed_ms = (start.tv_sec - stop.tv_sec) * 1000 + (start.tv_nsec - stop.tv_nsec) / 1000000;

          GAUGER ("MINTDB", state->cmd[state->i].details.gauger.description, elapsed_ms, "milliseconds");
        }
        break;

      case PERF_TALER_MINTDB_CMD_NEW_SESSION:
        state->session = state->plugin->get_session (state->plugin->cls, GNUNET_YES);
        // TODO what about the old session ?
        break;

      case PERF_TALER_MINTDB_CMD_START_TRANSACTION:
        state->plugin->start (state->plugin->cls, state->session);
        break;

      case PERF_TALER_MINTDB_CMD_COMMIT_TRANSACTION:
        state->plugin->commit (state->plugin->cls, state->session);
        break;

      case PERF_TALER_MINTDB_CMD_ABORT_TRANSACTION:
        state->plugin->rollback (state->plugin->cls, state->session);

      case PERF_TALER_MINTDB_CMD_SAVE_ARRAY:
        {
          // Array initialization on first loop iteration
          // Alows for nested loops
          if (state->cmd[cmd_find (state->cmd, state->cmd[state->i].details.save_array.label_loop)].details.loop.curr_iteration == 0)
          {
            state->cmd[state->i].details.save_array.index = 0;
          }

          // TODO check the logic here. It probably can be improved
          
          int loop_index = cmd_find (state->cmd, state->cmd[state->i].details.save_array.label_loop);
          int proba = state->cmd[loop_index].details.loop.max_iterations / state->cmd[state->i].details.save_array.nb_saved;
          int rnd = GNUNET_CRYPTO_random_u32 (GNUNET_CRYPTO_QUALITY_WEAK, proba);


          /**
           * If the room available is equal to the remaining number of
           * iterations, the item is automaticly saved.
           *
           * Else it is saved only if rdn is 0
           */
          if ((state->cmd[loop_index].details.loop.max_iterations - state->cmd[loop_index].details.loop.curr_iteration ==
              state->cmd[state->i].details.save_array.nb_saved - state->cmd[state->i].details.save_array.index) ||
            (rnd == 0))
          {

            union PERF_TALER_MINTDB_Data *save_location =
              &state->cmd[state->i].details.save_array.data_saved[state->cmd[state->i].details.save_array.index];
            union PERF_TALER_MINTDB_Data *item_saved =
              &state->cmd[cmd_find (state->cmd, state->cmd[state->i].details.save_array.label_save)].exposed;


            switch (state->cmd[state->i].details.save_array.type_saved)
            {
              case PERF_TALER_MINTDB_DEPOSIT:
                save_location->deposit = item_saved->deposit;
                break;

              case PERF_TALER_MINTDB_TIME:
                save_location->time = item_saved->time;
                break;

              default:
                break;
            }
            state->cmd[state->i].details.save_array.index++;
          }
        }
        break;

      case PERF_TALER_MINTDB_CMD_LOAD_ARRAY:
        {
          int loop_index = cmd_find (state->cmd, state->cmd[state->i].details.load_array.label_loop);
          int save_index = cmd_find (state->cmd, state->cmd[state->i].details.load_array.label_save);

          /* Extracting the data from the loop_indexth indice in save_index
           * array.
           */
          union PERF_TALER_MINTDB_Data loaded_data =
            state->cmd[save_index].details.save_array.data_saved[
            state->cmd[state->i].details.load_array.permutation[
              state->cmd[loop_index].details.loop.curr_iteration
            ]];


          switch (state->cmd[state->i].exposed_type)
          {
            case PERF_TALER_MINTDB_DEPOSIT:
              state->cmd[state->i].exposed.deposit = loaded_data.deposit;
              break;

            case PERF_TALER_MINTDB_TIME:
              state->cmd[state->i].exposed.time = loaded_data.time;
              break;

            default:
              break;
          }
        }
        break;

      case PERF_TALER_MINTDB_CMD_INSERT_DEPOSIT:
        {
          struct TALER_MINTDB_Deposit *deposit = deposit_init (-1);
          state->plugin->insert_deposit (state->plugin->cls, state->session, deposit);

          state->cmd[state->i].exposed.deposit = deposit;
        }
        break;

      case PERF_TALER_MINTDB_CMD_GET_DEPOSIT:
        {
          int source_index = cmd_find (state->cmd, state->cmd[state->i].details.get_deposit.label_source); // Find the source location
          struct TALER_MINTDB_Deposit *deposit = state->cmd[source_index].exposed.deposit; // Get the deposit from the source
          state->plugin->have_deposit (state->plugin->cls, state->session, deposit);
        }
        break;

      default :
        break;
    }
  }
  return GNUNET_OK;
}


/**
 * Runs the commands given in @a cmd, working with
 * the database referenced by @a db_plugin
 */
int
PERF_TALER_MINTDB_interpret (struct TALER_MINTDB_Plugin *db_plugin,
  struct PERF_TALER_MINTDB_Cmd cmd[])
{
  // Initializing commands
  cmd_init (cmd);

  // Running the interpreter
  struct PERF_TALER_MINTDB_interpreter_state state = 
  {.i = 0, .cmd = cmd, .plugin = db_plugin};
  state.session = db_plugin->get_session (db_plugin->cls, GNUNET_YES);

  interpret (&state);

  // Cleaning the memory
  cmd_clean (cmd);

  return GNUNET_YES;
}
