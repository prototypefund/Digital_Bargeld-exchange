/*
  This file is part of TALER
  Copyright (C) 2014, 2015, 2016 GNUnet e.V. and INRIA

  TALER is free software; you can redistribute it and/or modify it under the
  terms of the GNU Lesser General Public License as published by the Free Software
  Foundation; either version 2.1, or (at your option) any later version.

  TALER is distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
  A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License along with
  TALER; see the file COPYING.LGPL.  If not, If not, see <http://www.gnu.org/licenses/>
*/
/**
 * @file src/benchmark/taler-exchange-benchmark.c
 * @brief exchange's benchmark
 * @author Marcello Stanisci
 */
#include "platform.h"
#include <taler/taler_util.h>
#include <taler/taler_signatures.h>
#include <taler/taler_exchange_service.h>
#include <taler/taler_json_lib.h>
#include <gnunet/gnunet_util_lib.h>
#include <gnunet/gnunet_curl_lib.h>
#include <microhttpd.h>


static unsigned int pool_size = 100000;

/**
 * URI under which the exchange is reachable during the benchmark.
 */
#define EXCHANGE_URI "http://localhost:8081/"

/**
 * Main function that will be run by the scheduler.
 *
 * @param cls closure
 */
static void
run (void *cls)
{
}

int
main (int argc,
      char * const *argv)
{
  /**
   * 1 Pool's size as an option
   * 2 Connection to the exchange
   * 3 Allocation of large enough memory
   * 4 Withdraw
   */

  const struct GNUNET_GETOPT_CommandLineOption options[] = {
    {'s', "pool-size", NULL,
     "How many coins this benchmark should instantiate", GNUNET_YES,
     &GNUNET_GETOPT_set_uint, &pool_size}
    };

  GNUNET_SCHEDULER_run (&run, NULL);
}
