/*
  This file is part of TALER
  (C) 2014 Christian Grothoff (and other contributing authors)

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
 * @file mint/test_mint_deposits.c
 * @brief testcase for mint deposits
 * @author Sree Harsha Totakura <sreeharsha@totakura.in>
 */

#include "platform.h"
#include <libpq-fe.h>
#include <gnunet/gnunet_util_lib.h>
#include "mint_db.h"
#include "db_pq.h"
#include "taler-mint-httpd.h"

#define DB_URI "postgres:///taler"

#define break_db_err(result) do { \
    GNUNET_break(0); \
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR, "Database failure: %s\n", PQresultErrorMessage (result)); \
  } while (0)

/**
 * Shorthand for exit jumps.
 */
#define EXITIF(cond)                                              \
  do {                                                            \
    if (cond) { GNUNET_break (0); goto EXITIF_exit; }             \
  } while (0)


/**
 * DB connection handle
 */
static PGconn *conn;

/**
 * Should we not interact with a temporary table?
 */
static int persistent;

/**
 * Testcase result
 */
static int result;


int
TALER_MINT_DB_init_deposits (PGconn *conn, int tmp)
{
  const char *tmp_str = (1 == tmp) ? "TEMPORARY" : "";
  char *sql;
  PGresult *res;
  int ret;

  res = NULL;
  (void) GNUNET_asprintf (&sql,
                          "CREATE %1$s TABLE IF NOT EXISTS deposits ("
                          " coin_pub BYTEA NOT NULL PRIMARY KEY CHECK (length(coin_pub)=32)"
                          ",denom_pub BYTEA NOT NULL CHECK (length(denom_pub)=32)"
                          ",transaction_id INT8 NOT NULL"
                          ",amount_value INT4 NOT NULL"
                          ",amount_fraction INT4 NOT NULL"
                          ",amount_currency VARCHAR(4) NOT NULL"
                          ",merchant_pub BYTEA NOT NULL"
                          ",h_contract BYTEA NOT NULL CHECK (length(h_contract)=64)"
                          ",h_wire BYTEA NOT NULL CHECK (length(h_wire)=64)"
                          ",coin_sig BYTEA NOT NULL CHECK (length(coin_sig)=64)"
                          ",wire TEXT NOT NULL"
                          ")",
                          tmp_str);
  res = PQexec (conn, sql);
  GNUNET_free (sql);
  if (PGRES_COMMAND_OK != PQresultStatus (res))
  {
    break_db_err (res);
    ret = GNUNET_SYSERR;
  }
  else
    ret = GNUNET_OK;
  PQclear (res);
  return ret;
}


static void
do_shutdown (void *cls, const struct GNUNET_SCHEDULER_TaskContext *tc)
{
  if (NULL != conn)
    PQfinish (conn);
  conn = NULL;
}


/**
 * Main function that will be run by the scheduler.
 *
 * @param cls closure
 * @param args remaining command-line arguments
 * @param cfgfile name of the configuration file used (for saving, can be NULL!)
 * @param config configuration
 */
static void
run (void *cls, char *const *args, const char *cfgfile,
     const struct GNUNET_CONFIGURATION_Handle *config)
{
  static const char wire[] = "{"
      "\"type\":\"SEPA\","
      "\"IBAN\":\"DE67830654080004822650\","
      "\"NAME\":\"GNUNET E.V\","
      "\"BIC\":\"GENODEF1SRL\""
      "}";
  struct Deposit *deposit;
  uint64_t transaction_id;

  deposit = NULL;
  GNUNET_SCHEDULER_add_delayed (GNUNET_TIME_UNIT_FOREVER_REL,
                                &do_shutdown, NULL);
  EXITIF (NULL == (conn = PQconnectdb(DB_URI)));
  EXITIF (GNUNET_OK != TALER_MINT_DB_init_deposits (conn, !persistent));
  EXITIF (GNUNET_OK != TALER_MINT_DB_prepare (conn));
  deposit = GNUNET_malloc (sizeof (struct Deposit) + sizeof (wire));
  /* Makeup a random coin public key */
  GNUNET_CRYPTO_random_block (GNUNET_CRYPTO_QUALITY_WEAK,
                              deposit,
                              sizeof (struct Deposit));
  /* Makeup a random 64bit transaction ID */
  transaction_id = GNUNET_CRYPTO_random_u64 (GNUNET_CRYPTO_QUALITY_WEAK,
                                             UINT64_MAX);
  deposit->transaction_id = GNUNET_htonll (transaction_id);
  /* Random amount */
  deposit->amount.value =
      htonl (GNUNET_CRYPTO_random_u32 (GNUNET_CRYPTO_QUALITY_WEAK, UINT32_MAX));
  deposit->amount.fraction =
      htonl (GNUNET_CRYPTO_random_u32 (GNUNET_CRYPTO_QUALITY_WEAK, UINT32_MAX));
  GNUNET_assert (strlen (MINT_CURRENCY) < sizeof (deposit->amount.currency));
  strcpy (deposit->amount.currency, MINT_CURRENCY);
  /* Copy wireformat */
  deposit->wire = json_loads (wire, 0, NULL);
  EXITIF (GNUNET_OK != TALER_MINT_DB_insert_deposit (conn,
                                                     deposit));
  EXITIF (GNUNET_OK != TALER_MINT_DB_have_deposit (conn,
                                                   deposit));
  result = GNUNET_OK;

 EXITIF_exit:
  GNUNET_free_non_null (deposit);
  GNUNET_SCHEDULER_shutdown ();
  return;
}


int main(int argc, char *const argv[])
{
  static const struct GNUNET_GETOPT_CommandLineOption options[] = {
    {'T', "persist", NULL,
     gettext_noop ("Use a persistent database table instead of a temporary one"),
     GNUNET_NO, &GNUNET_GETOPT_set_one, &persistent},
    GNUNET_GETOPT_OPTION_END
  };


  persistent = GNUNET_NO;
  result = GNUNET_SYSERR;
  if (GNUNET_OK !=
      GNUNET_PROGRAM_run (argc, argv,
                          "test-mint-deposits",
                          "testcase for mint deposits",
                          options, &run, NULL))
    return 3;
  return (GNUNET_OK == result) ? 0 : 1;
}
