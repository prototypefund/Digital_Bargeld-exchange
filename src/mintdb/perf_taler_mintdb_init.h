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
 * @file mintdb/perf_taler_mintdb_init.h
 * @brief Heler function for creating dummy inpus for the mint database
 * @author Nicolas Fournier
 */
#ifndef __PERF_TALER_MINTDB_INIT_H___
#define __PERF_TALER_MINTDB_INIT_H___

#include "taler_mintdb_plugin.h"


#define CURRENCY "EUR"



/**
 * Generate a dummy DenominationKeyInformation for testing purposes
 * @return a dummy denomination key
 */
struct TALER_MINTDB_DenominationKeyIssueInformation *
PERF_TALER_MINTDB_denomination_init (void);


/**
 * Copies the given denomination
 * @param reserve the deposit copy
 * @return a copy of @a deposit; NULL if error
 */
struct TALER_MINTDB_DenominationKeyIssueInformation *
PERF_TALER_MINTDB_denomination_copy (
  const struct TALER_MINTDB_DenominationKeyIssueInformation *dki);


/**
 * Free memory of a DenominationKeyIssueInformation
 * @param dki pointer to the struct to free
 */
int
PERF_TALER_MINTDB_denomination_free (
  struct TALER_MINTDB_DenominationKeyIssueInformation *dki);


/**
 * Generate a dummy reserve for testing
 * @return a reserve with 1000 EUR in it
 */
struct TALER_MINTDB_Reserve *
PERF_TALER_MINTDB_reserve_init (void);


/**
 * Copies the given reserve
 * @param reserve the reserve to copy
 * @return a copy of @a reserve; NULL if error
 */
struct TALER_MINTDB_Reserve *
PERF_TALER_MINTDB_reserve_copy (const struct TALER_MINTDB_Reserve *reserve);


/**
 * Free memory of a reserve
 * @param reserve pointer to the structure to be freed
 */
int
PERF_TALER_MINTDB_reserve_free (struct TALER_MINTDB_Reserve *reserve);


/**
 * Generate a dummy deposit for testing purposes
 * @param dki the denomination key used to sign the key
 */
struct TALER_MINTDB_Deposit *
PERF_TALER_MINTDB_deposit_init (
  const struct TALER_MINTDB_DenominationKeyIssueInformation *dki);


/**
 * Copies the given deposit
 * @param reserve the deposit copy
 * @return a copy of @a deposit; NULL if error
 */
struct TALER_MINTDB_Deposit *
PERF_TALER_MINTDB_deposit_copy (const struct TALER_MINTDB_Deposit *deposit);


/**
 * Free memory of a deposit
 * @param deposit pointer to the structure to free
 */
int
PERF_TALER_MINTDB_deposit_free (struct TALER_MINTDB_Deposit *deposit);


/**
 * Generate a CollectableBlindcoin for testing purpuses
 * @param dki denomination key used to sign the coin
 * @param reserve reserve providing the money for the coin
 * @return a randomly generated CollectableBlindcoin
 */
struct TALER_MINTDB_CollectableBlindcoin *
PERF_TALER_MINTDB_collectable_blindcoin_init (
  const struct TALER_MINTDB_DenominationKeyIssueInformation *dki,
  const struct TALER_MINTDB_Reserve *reserve);


/**
 * Copies the given coin
 * @param coin the coin to copy
 * @return a copy of coin; NULL if error
 */
struct TALER_MINTDB_CollectableBlindcoin *
PERF_TALER_MINTDB_collectable_blindcoin_copy (
  const struct TALER_MINTDB_CollectableBlindcoin *coin);


/**
 * Liberate memory of @a coin
 * @param coin pointer to the structure to free
 */
int
PERF_TALER_MINTDB_collectable_blindcoin_free (
  struct TALER_MINTDB_CollectableBlindcoin *coin);


/**
 * Generate a random CoinPublicInfo
 */
struct TALER_CoinPublicInfo *
PERF_TALER_MINTDB_coin_public_info_init (void);


/**
 * Free a CoinPublicInfo
 */
int PERF_TALER_MINTDB_coin_public_info_free (struct TALER_CoinPublicInfo *cpi);


/**
 * @return a randomly generated refresh session
 */
struct TALER_MINTDB_RefreshSession *
PERF_TALER_MINTDB_refresh_session_init (void);


/**
 * Frees memory of a refresh_session
 */
int
PERF_TALER_MINTDB_refresh_session_free (
  struct TALER_MINTDB_RefreshSession *refresh_session);

#endif
