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
 * @file mint/test_mint_db.c
 * @brief test cases for DB interaction functions
 * @author Sree Harsha Totakura <sreeharsha@totakura.in>
 */

#include "platform.h"
#include "mint_db.h"

static int result;

#define FAILIF(cond)                              \
  do {                                          \
    if (!(cond)){ break;}                      \
    GNUNET_break (0);                           \
    goto drop;                                  \
  } while (0)


/**
 * Checks if the given reserve has the given amount of balance and expiry
 *
 * @param db the database connection
 * @param pub the public key of the reserve
 * @param value balance value
 * @param fraction balance fraction
 * @param currency currency of the reserve
 * @param expiry expiration of the reserve
 * @return #GNUNET_OK if the given reserve has the same balance and expiration
 *           as the given parameters; #GNUNET_SYSERR if not
 */
int
check_reserve (PGconn *db,
               struct GNUNET_CRYPTO_EddsaPublicKey *pub,
               uint32_t value, uint32_t fraction, const char *currency,
               uint64_t expiry)
{
  struct Reserve reserve;
  reserve.pub = pub;

  FAILIF (GNUNET_OK != TALER_MINT_DB_reserve_get (db, &reserve));
  FAILIF (value != reserve.balance.value);
  FAILIF (fraction != reserve.balance.fraction);
  FAILIF (0 != strcmp (currency, reserve.balance.currency));
  FAILIF (expiry != reserve.expiry.abs_value_us);

  return GNUNET_OK;
 drop:
  return GNUNET_SYSERR;
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
  PGconn *db;
  struct GNUNET_CRYPTO_EddsaPublicKey pub;
  struct Reserve reserve;
  struct GNUNET_TIME_Absolute expiry;
  struct TALER_Amount amount;

  db = NULL;
  if (GNUNET_OK != TALER_MINT_DB_init ("postgres:///taler"))
  {
    result = 1;
    return;
  }
  if (GNUNET_OK != TALER_MINT_DB_create_tables (GNUNET_YES))
  {
    result = 2;
    goto drop;
  }
  if (NULL == (db = TALER_MINT_DB_get_connection(GNUNET_YES)))
  {
    result = 3;
    goto drop;
  }
  GNUNET_CRYPTO_random_block (GNUNET_CRYPTO_QUALITY_WEAK,
                              &pub, sizeof (pub));
  reserve.pub = &pub;
  amount.value = 1;
  amount.fraction = 1;
  strcpy (amount.currency, "EUR");
  expiry = GNUNET_TIME_absolute_add (GNUNET_TIME_absolute_get (),
                                     GNUNET_TIME_UNIT_HOURS);
  result = 4;
  FAILIF (GNUNET_OK != TALER_MINT_DB_reserves_in_insert (db,
                                                         &reserve,
                                                         amount,
                                                         expiry));
  FAILIF (GNUNET_OK != check_reserve (db,
                                      &pub,
                                      amount.value,
                                      amount.fraction,
                                      amount.currency,
                                      expiry.abs_value_us));
  FAILIF (GNUNET_OK != TALER_MINT_DB_reserves_in_insert (db,
                                                         &reserve,
                                                         amount,
                                                         expiry));
  FAILIF (GNUNET_OK != check_reserve (db,
                                      &pub,
                                      ++amount.value,
                                      ++amount.fraction,
                                      amount.currency,
                                      expiry.abs_value_us));
  result = 0;
 drop:
  if (NULL != db)
    GNUNET_break (GNUNET_OK == TALER_MINT_DB_drop_temporary (db));
}


int
main (int argc, char *const argv[])
{
   static const struct GNUNET_GETOPT_CommandLineOption options[] = {
    GNUNET_GETOPT_OPTION_END
  };

   result = -1;
  if (GNUNET_OK !=
      GNUNET_PROGRAM_run (argc, argv,
                          "test-mint-db",
                          "Test cases for mint database helper functions.",
                          options, &run, NULL))
    return 3;
  return result;
}
