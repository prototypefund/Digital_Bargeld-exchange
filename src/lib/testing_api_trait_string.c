/*
  This file is part of TALER
  Copyright (C) 2018-2020 Taler Systems SA

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
 * @file lib/testing_api_trait_string.c
 * @brief offers strings traits.
 * @author Marcello Stanisci
 * @author Christian Grothoff
 */
#include "platform.h"
#include "taler_json_lib.h"
#include <gnunet/gnunet_curl_lib.h>
#include "exchange_api_handle.h"
#include "taler_signatures.h"
#include "taler_testing_lib.h"

/**
 * FIXME: use json-t instead?
 */
#define TALER_TESTING_TRAIT_CONTRACT_TERMS "contract-terms"

/**
 * Some string. Avoid, use something more precise!
 */
#define TALER_TESTING_TRAIT_STRING "string"

/**
 * An HTTP-URL.
 */
#define TALER_TESTING_TRAIT_URL "url"

/**
 * A PAYTO-URL.
 */
#define TALER_TESTING_TRAIT_PAYTO "payto"

/**
 * String identifying an order.
 */
#define TALER_TESTING_TRAIT_ORDER_ID "order-id"


/**
 * Obtain contract terms from @a cmd.
 *
 * @param cmd command to extract the contract terms from.
 * @param index contract terms index number.
 * @param contract_terms[out] where to write the contract
 *        terms.
 * @return #GNUNET_OK on success.
 */
int
TALER_TESTING_get_trait_contract_terms
  (const struct TALER_TESTING_Command *cmd,
  unsigned int index,
  const json_t **contract_terms)
{
  return cmd->traits (cmd->cls,
                      (const void **) contract_terms,
                      TALER_TESTING_TRAIT_CONTRACT_TERMS,
                      index);
}


/**
 * Offer contract terms.
 *
 * @param index contract terms index number.
 * @param contract_terms contract terms to offer.
 * @return the trait.
 */
struct TALER_TESTING_Trait
TALER_TESTING_make_trait_contract_terms
  (unsigned int index,
  const json_t *contract_terms)
{
  struct TALER_TESTING_Trait ret = {
    .index = index,
    .trait_name = TALER_TESTING_TRAIT_CONTRACT_TERMS,
    .ptr = (const void *) contract_terms
  };
  return ret;
}


/**
 * Obtain a string from @a cmd.
 *
 * @param cmd command to extract the subject from.
 * @param index index number associated with the transfer
 *        subject to offer.
 * @param s[out] where to write the offered
 *        string
 * @return #GNUNET_OK on success.
 */
int
TALER_TESTING_get_trait_string
  (const struct TALER_TESTING_Command *cmd,
  unsigned int index,
  const char **s)
{
  return cmd->traits (cmd->cls,
                      (const void **) s,
                      TALER_TESTING_TRAIT_STRING,
                      index);
}


/**
 * Offer string.
 *
 * @param index index number associated with the transfer
 *        subject being offered.
 * @param s transfer subject to offer.
 * @return the trait.
 */
struct TALER_TESTING_Trait
TALER_TESTING_make_trait_string
  (unsigned int index,
  const char *s)
{
  struct TALER_TESTING_Trait ret = {
    .index = index,
    .trait_name = TALER_TESTING_TRAIT_STRING,
    .ptr = (const void *) s
  };
  return ret;
}


/**
 * Obtain a HTTP url from @a cmd.
 *
 * @param cmd command to extract the url from.
 * @param index which url is to be picked, in case
 *        multiple are offered.
 * @param url[out] where to write the url.
 *
 * @return #GNUNET_OK on success.
 */
int
TALER_TESTING_get_trait_url
  (const struct TALER_TESTING_Command *cmd,
  unsigned int index,
  const char **url)
{
  return cmd->traits (cmd->cls,
                      (const void **) url,
                      TALER_TESTING_TRAIT_URL,
                      index);
}


/**
 * Offer HTTP url in a trait.
 *
 * @param index which url is to be picked,
 *        in case multiple are offered.
 * @param url the url to offer.
 *
 * @return the trait.
 */
struct TALER_TESTING_Trait
TALER_TESTING_make_trait_url
  (unsigned int index,
  const char *url)
{
  struct TALER_TESTING_Trait ret = {
    .index = index,
    .trait_name = TALER_TESTING_TRAIT_URL,
    .ptr = (const void *) url
  };

  GNUNET_assert (0 != strncasecmp (url,
                                   "payto://",
                                   strlen ("payto://")));

  return ret;
}


/**
 * Obtain a order id from @a cmd.
 *
 * @param cmd command to extract the order id from.
 * @param index which order id is to be picked, in case
 *        multiple are offered.
 * @param order_id[out] where to write the order id.
 *
 * @return #GNUNET_OK on success.
 */
int
TALER_TESTING_get_trait_order_id
  (const struct TALER_TESTING_Command *cmd,
  unsigned int index,
  const char **order_id)
{
  return cmd->traits (cmd->cls,
                      (const void **) order_id,
                      TALER_TESTING_TRAIT_ORDER_ID,
                      index);
}


/**
 * Offer order id in a trait.
 *
 * @param index which order id is to be offered,
 *        in case multiple are offered.
 * @param order_id the order id to offer.
 *
 * @return the trait.
 */
struct TALER_TESTING_Trait
TALER_TESTING_make_trait_order_id
  (unsigned int index,
  const char *order_id)
{
  struct TALER_TESTING_Trait ret = {
    .index = index,
    .trait_name = TALER_TESTING_TRAIT_ORDER_ID,
    .ptr = (const void *) order_id
  };
  return ret;
}


/**
 * Obtain a PAYTO-url from @a cmd.
 *
 * @param cmd command to extract the url from.
 * @param index which url is to be picked, in case
 *        multiple are offered.
 * @param url[out] where to write the url.
 * @return #GNUNET_OK on success.
 */
int
TALER_TESTING_get_trait_payto
  (const struct TALER_TESTING_Command *cmd,
  unsigned int index,
  const char **url)
{
  return cmd->traits (cmd->cls,
                      (const void **) url,
                      TALER_TESTING_TRAIT_PAYTO,
                      index);
}


/**
 * Offer a "payto" URL reference.
 *
 * @param index which reference is to be offered,
 *        in case multiple are offered.
 * @param payto the payto URL
 * @return the trait.
 */
struct TALER_TESTING_Trait
TALER_TESTING_make_trait_payto
  (unsigned int index,
  const char *payto)
{
  struct TALER_TESTING_Trait ret = {
    .index = index,
    .trait_name = TALER_TESTING_TRAIT_PAYTO,
    .ptr = (const void *) payto
  };

  GNUNET_assert (0 == strncasecmp (payto,
                                   "payto://",
                                   strlen ("payto://")));
  return ret;
}


/* end of testing_api_trait_string.c */
