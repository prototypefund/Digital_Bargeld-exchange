/*
  This file is part of TALER
  Copyright (C) 2018 Taler Systems SA

  TALER is free software; you can redistribute it and/or modify it
  under the terms of the GNU General Public License as published
  by the Free Software Foundation; either version 3, or (at your
  option) any later version.

  TALER is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  General Public License for more details.

  You should have received a copy of the GNU General Public
  License along with TALER; see the file COPYING.  If not, see
  <http://www.gnu.org/licenses/>
*/

/**
 * @file exchange-lib/testing_api_trait_number.c
 * @brief traits to offer numbers
 * @author Marcello Stanisci
 */
#include "platform.h"
#include "taler_json_lib.h"
#include <gnunet/gnunet_curl_lib.h>
#include "exchange_api_handle.h"
#include "taler_signatures.h"
#include "taler_testing_lib.h"

#define TALER_TESTING_TRAIT_UINT "uint"
#define TALER_TESTING_TRAIT_UINT64 "uint-64"

/**
 * Obtain a number from @a cmd.
 *
 * @param cmd command to extract the number from.
 * @param index the number's index number.
 * @param n[out] set to the number coming from @a cmd.
 * @return #GNUNET_OK on success.
 */
int
TALER_TESTING_get_trait_uint
  (const struct TALER_TESTING_Command *cmd,
  unsigned int index,
  const unsigned int **n)
{
  return cmd->traits (cmd->cls,
                      (const void **) n,
                      TALER_TESTING_TRAIT_UINT,
                      index);
}


/**
 * Offer a number.
 *
 * @param index the number's index number.
 * @param n the number to offer.
 * @return #GNUNET_OK on success.
 */
struct TALER_TESTING_Trait
TALER_TESTING_make_trait_uint
  (unsigned int index,
  const unsigned int *n)
{
  struct TALER_TESTING_Trait ret = {
    .index = index,
    .trait_name = TALER_TESTING_TRAIT_UINT,
    .ptr = (const void *) n
  };
  return ret;
}


/**
 * Obtain a "number" value from @a cmd, 64-bit version.
 *
 * @param cmd command to extract the number from.
 * @param index the number's index number.
 * @param n[out] set to the number coming from @a cmd.
 *
 * @return #GNUNET_OK on success.
 */
int
TALER_TESTING_get_trait_uint64
  (const struct TALER_TESTING_Command *cmd,
  unsigned int index,
  const uint64_t **n)
{
  return cmd->traits (cmd->cls,
                      (const void **) n,
                      TALER_TESTING_TRAIT_UINT64,
                      index);
}


/**
 * Offer number trait, 64-bit version.
 *
 * @param index the number's index number.
 * @param n number to offer.
 */
struct TALER_TESTING_Trait
TALER_TESTING_make_trait_uint64
  (unsigned int index,
  const uint64_t *n)
{
  struct TALER_TESTING_Trait ret = {
    .index = index,
    .trait_name = TALER_TESTING_TRAIT_UINT64,
    .ptr = (const void *) n
  };
  return ret;
}


/* end of testing_api_trait_number.c */
