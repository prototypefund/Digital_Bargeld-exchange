/*
  This file is part of TALER
  Copyright (C) 2018 Taler Systems SA

  TALER is free software; you can redistribute it and/or modify it
  under the terms of the GNU General Public License as published
  by the Free Software Foundation; either version 3, or (at your
  option) any later version.

  TALER is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public
  License along with TALER; see the file COPYING.  If not, see
  <http://www.gnu.org/licenses/>
*/
/**
 * @file testing/testing_api_trait_blinding_key.c
 * @brief offer blinding keys as traits.
 * @author Christian Grothoff
 * @author Marcello Stanisci
 */
#include "platform.h"
#include "taler_json_lib.h"
#include <gnunet/gnunet_curl_lib.h>
#include "taler_signatures.h"
#include "taler_testing_lib.h"

#define TALER_TESTING_TRAIT_BLINDING_KEY "blinding-key"


/**
 * Obtain a blinding key from a @a cmd.
 *
 * @param cmd command to extract trait from
 * @param index which coin to pick if @a cmd has multiple on offer.
 * @param[out] blinding_key set to the offered blinding key.
 * @return #GNUNET_OK on success.
 */
int
TALER_TESTING_get_trait_blinding_key
  (const struct TALER_TESTING_Command *cmd,
  unsigned int index,
  const struct TALER_DenominationBlindingKeyP **blinding_key)
{
  return cmd->traits (cmd->cls,
                      (const void **) blinding_key,
                      TALER_TESTING_TRAIT_BLINDING_KEY,
                      index);
}


/**
 * Offer blinding key.
 *
 * @param index index number to associate to the offered key.
 * @param blinding_key blinding key to offer.
 * @return the trait.
 */
struct TALER_TESTING_Trait
TALER_TESTING_make_trait_blinding_key
  (unsigned int index,
  const struct TALER_DenominationBlindingKeyP *blinding_key)
{
  struct TALER_TESTING_Trait ret = {
    .index = index,
    .trait_name = TALER_TESTING_TRAIT_BLINDING_KEY,
    .ptr = (const void *) blinding_key
  };

  return ret;
}


/* end of testing_api_trait_blinding_key.c */
