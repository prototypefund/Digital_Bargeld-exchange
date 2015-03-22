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
 * @file mint/taler-mint_httpd_db.h
 * @brief High-level (transactional-layer) database operations for the mint
 * @author Chrisitan Grothoff
 */
#ifndef TALER_MINT_HTTPD_DB_H
#define TALER_MINT_HTTPD_DB_H

#include <libpq-fe.h>
#include <microhttpd.h>
#include <gnunet/gnunet_util_lib.h>
#include "taler_util.h"
#include "taler_mintdb_plugin.h"


/**
 * Execute a "/deposit".  The validity of the coin and signature
 * have already been checked.  The database must now check that
 * the coin is not (double or over) spent, and execute the
 * transaction (record details, generate success or failure response).
 *
 * @param connection the MHD connection to handle
 * @param deposit information about the deposit
 * @return MHD result code
 */
int
TALER_MINT_db_execute_deposit (struct MHD_Connection *connection,
                               const struct Deposit *deposit);


/**
 * Execute a "/withdraw/status".  Given the public key of a reserve,
 * return the associated transaction history.
 *
 * @param connection the MHD connection to handle
 * @param reserve_pub public key of the reserve to check
 * @return MHD result code
 */
int
TALER_MINT_db_execute_withdraw_status (struct MHD_Connection *connection,
                                       const struct TALER_ReservePublicKey *reserve_pub);


/**
 * Execute a "/withdraw/sign".  Given a reserve and a properly signed
 * request to withdraw a coin, check the balance of the reserve and
 * if it is sufficient, store the request and return the signed
 * blinded envelope.
 *
 * @param connection the MHD connection to handle
 * @param reserve public key of the reserve
 * @param denomination_pub public key of the denomination requested
 * @param blinded_msg blinded message to be signed
 * @param blinded_msg_len number of bytes in @a blinded_msg
 * @param signature signature over the withdraw request, to be stored in DB
 * @return MHD result code
 */
int
TALER_MINT_db_execute_withdraw_sign (struct MHD_Connection *connection,
                                     const struct TALER_ReservePublicKey *reserve,
                                     const struct TALER_DenominationPublicKey *denomination_pub,
                                     const char *blinded_msg,
                                     size_t blinded_msg_len,
                                     const struct TALER_ReserveSignature *signature);


/**
 * Details about a melt operation of an individual coin.
 */
struct MeltDetails
{
  /**
   * Signature allowing the melt (using
   * a `struct RefreshMeltConfirmSignRequestBody`) to sign over.
   */
  struct TALER_CoinSpendSignature melt_sig;

  /**
   * How much of the coin's value did the client allow to be melted?
   * This amount includes the fees, so the final amount contributed
   * to the melt is this value minus the fee for melting the coin.
   */
  struct TALER_Amount melt_amount_with_fee;
};


/**
 * Execute a "/refresh/melt". We have been given a list of valid
 * coins and a request to melt them into the given
 * @a refresh_session_pub.  Check that the coins all have the
 * required value left and if so, store that they have been
 * melted and confirm the melting operation to the client.
 *
 * FIXME: some arguments are redundant here...
 *
 * @param connection the MHD connection to handle
 * @param melt_hash hash code of the session the coins are melted into
 * @param refresh_session_pub public key of the refresh session
 * @param client_signature signature of the client (matching @a refresh_session_pub)
 *         over the melting request
 * @param num_new_denoms number of entries in @a denom_pubs, size of y-dimension of @commit_coin array
 * @param denum_pubs array of public denomination keys for the refresh (?)
 * @param coin_count number of entries in @a coin_public_infos and @ a coin_melt_details, size of y-dimension of @commit_link array
 * @param coin_public_infos information about the coins to melt
 * @param coin_melt_details signatures and (residual) value of the respective coin should be melted
 * @param kappa size of x-dimension of @commit_coin and @commit_link arrays
 * @param commit_coin 2d array of coin commitments (what the mint is to sign
 *                    once the "/refres/reveal" of cut and choose is done)
 * @param commit_link 2d array of coin link commitments (what the mint is
 *                    to return via "/refresh/link" to enable linkage in the
 *                    future)
 * @return MHD result code
 */
int
TALER_MINT_db_execute_refresh_melt (struct MHD_Connection *connection,
                                    const struct GNUNET_HashCode *melt_hash,
                                    const struct TALER_SessionPublicKey *refresh_session_pub,
                                    const struct TALER_SessionSignature *client_signature,
                                    unsigned int num_new_denoms,
                                    const struct TALER_DenominationPublicKey *denom_pubs,
                                    unsigned int coin_count,
                                    const struct TALER_CoinPublicInfo *coin_public_infos,
                                    const struct MeltDetails *coin_melt_details,
                                    unsigned int kappa,
                                    struct RefreshCommitCoin *const* commit_coin,
                                    struct RefreshCommitLink *const* commit_link);


/**
 * Execute a "/refresh/reveal".  The client is revealing to us the
 * transfer keys for @a kappa-1 sets of coins.  Verify that the
 * revealed transfer keys would allow linkage to the blinded coins,
 * and if so, return the signed coins for corresponding to the set of
 * coins that was not chosen.
 *
 * @param connection the MHD connection to handle
 * @param refresh_session_pub public key of the refresh session
 * @param kappa size of x-dimension of @transfer_privs array plus one (!)
 * @param num_oldcoins size of y-dimension of @transfer_privs array
 * @param transfer_pubs array with the revealed transfer keys
 * @return MHD result code
 */
int
TALER_MINT_db_execute_refresh_reveal (struct MHD_Connection *connection,
                                      const struct TALER_SessionPublicKey *refresh_session_pub,
                                      unsigned int kappa,
                                      unsigned int num_oldcoins,
                                      struct TALER_TransferPrivateKey **transfer_privs);


/**
 * Execute a "/refresh/link".  Returns the linkage information that
 * will allow the owner of a coin to follow the refresh trail to the
 * refreshed coin.
 *
 * @param connection the MHD connection to handle
 * @param coin_pub public key of the coin to link
 * @return MHD result code
 */
int
TALER_MINT_db_execute_refresh_link (struct MHD_Connection *connection,
                                    const struct TALER_CoinSpendPublicKey *coin_pub);


#endif
/* TALER_MINT_HTTPD_DB_H */
