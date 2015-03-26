/*
  This file is part of TALER
  Copyright (C) 2014 Christian Grothoff (and other contributing authors)

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
 * @file util/db_pq.c
 * @brief helper functions for libpq (PostGres) interactions
 * @author Sree Harsha Totakura <sreeharsha@totakura.in>
 * @author Florian Dold
 * @author Christian Grothoff
 */
#include "platform.h"
#include <gnunet/gnunet_util_lib.h>
#include "db_pq.h"


/**
 * Execute a prepared statement.
 */
PGresult *
TALER_DB_exec_prepared (PGconn *db_conn,
                        const char *name,
                        const struct TALER_DB_QueryParam *params)
{
  unsigned len;
  unsigned i;

  /* count the number of parameters */
  {
    const struct TALER_DB_QueryParam *x;
    for (len = 0, x = params;
         x->more;
         len++, x++);
  }

  /* new scope to allow stack allocation without alloca */

  {
    void *param_values[len];
    int param_lengths[len];
    int param_formats[len];

    for (i = 0; i < len; i += 1)
    {
      param_values[i] = (void *) params[i].data;
      param_lengths[i] = params[i].size;
      param_formats[i] = 1;
    }
    return PQexecPrepared (db_conn, name, len,
                           (const char **) param_values,
                           param_lengths,
                           param_formats, 1);
  }
}


/**
 * Extract results from a query result according to the given specification.
 * If colums are NULL, the destination is not modified, and #GNUNET_NO
 * is returned.
 *
 * @return
 *   #GNUNET_YES if all results could be extracted
 *   #GNUNET_NO if at least one result was NULL
 *   #GNUNET_SYSERR if a result was invalid (non-existing field)
 */
int
TALER_DB_extract_result (PGresult *result,
                         struct TALER_DB_ResultSpec *rs,
                         int row)
{
  int had_null = GNUNET_NO;
  size_t len;
  unsigned int i;
  unsigned int j;

  for (i=0; NULL != rs[i].fname; i++)
  {
    int fnum;

    fnum = PQfnumber (result, rs[i].fname);
    if (fnum < 0)
    {
      GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                  "field '%s' does not exist in result\n",
                  rs[i].fname);
      return GNUNET_SYSERR;
    }

    /* if a field is null, continue but
     * remember that we now return a different result */
    if (PQgetisnull (result, row, fnum))
    {
      had_null = GNUNET_YES;
      continue;
    }
    const char *res;
    len = PQgetlength (result, row, fnum);
    if ( (0 != rs[i].dst_size) &&
         (rs[i].dst_size != len) )
    {
      GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                  "field '%s' has wrong size (got %u, expected %u)\n",
                  rs[i].fname,
                  (unsigned int) len,
                  (unsigned int) rs[i].dst_size);
      for (j=0; j<i; j++)
        if (0 == rs[j].dst_size)
        {
          GNUNET_free (rs[j].dst);
          rs[j].dst = NULL;
          *rs[j].result_size = 0;
        }
      return GNUNET_SYSERR;
    }
    res = PQgetvalue (result, row, fnum);
    GNUNET_assert (NULL != res);
    if (0 == rs[i].dst_size)
    {
      *rs[i].result_size = len;
      *((void **) rs[i].dst) = GNUNET_malloc (len);
      rs[i].dst = *((void **) rs[i].dst);
    }
    memcpy (rs[i].dst, res, len);
  }
  if (GNUNET_YES == had_null)
    return GNUNET_NO;
  return GNUNET_YES;
}


int
TALER_DB_field_isnull (PGresult *result,
                       int row,
                       const char *fname)
{
  int fnum;

  fnum = PQfnumber (result, fname);
  GNUNET_assert (fnum >= 0);
  if (PQgetisnull (result, row, fnum))
    return GNUNET_YES;
  return GNUNET_NO;
}


int
TALER_DB_extract_amount_nbo (PGresult *result,
                             int row,
                             const char *val_name,
                             const char *frac_name,
                             const char *curr_name,
                             struct TALER_AmountNBO *r_amount_nbo)
{
  int val_num;
  int frac_num;
  int curr_num;
  int len;

  GNUNET_assert (NULL != strstr (val_name, "_val"));
  GNUNET_assert (NULL != strstr (frac_name, "_frac"));
  GNUNET_assert (NULL != strstr (curr_name, "_curr"));

  val_num = PQfnumber (result, val_name);
  GNUNET_assert (val_num >= 0);
  frac_num = PQfnumber (result, frac_name);
  GNUNET_assert (frac_num >= 0);
  curr_num = PQfnumber (result, curr_name);
  GNUNET_assert (curr_num >= 0);

  r_amount_nbo->value = *(uint64_t *) PQgetvalue (result, row, val_num);
  r_amount_nbo->fraction = *(uint32_t *) PQgetvalue (result, row, frac_num);
  memset (r_amount_nbo->currency,
          0,
          TALER_CURRENCY_LEN);
  len = GNUNET_MIN (TALER_CURRENCY_LEN - 1,
                    PQgetlength (result, row, curr_num));
  memcpy (r_amount_nbo->currency,
          PQgetvalue (result,
                      row,
                      curr_num),
          len);
  return GNUNET_OK;
}


int
TALER_DB_extract_amount (PGresult *result,
                         int row,
                         const char *val_name,
                         const char *frac_name,
                         const char *curr_name,
                         struct TALER_Amount *r_amount)
{
  struct TALER_AmountNBO amount_nbo;

  (void)
      TALER_DB_extract_amount_nbo (result,
                                   row,
                                   val_name,
                                   frac_name,
                                   curr_name,
                                   &amount_nbo);
  r_amount->value = GNUNET_ntohll (amount_nbo.value);
  r_amount->fraction = ntohl (amount_nbo.fraction);
  (void) strncpy (r_amount->currency, amount_nbo.currency, TALER_CURRENCY_LEN);

  return GNUNET_OK;
}

/* end of pq/db_pq.c */
