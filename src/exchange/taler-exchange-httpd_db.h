/*
  This file is part of TALER
  Copyright (C) 2014, 2015 GNUnet e.V.

  TALER is free software; you can redistribute it and/or modify it under the
  terms of the GNU General Public License as published by the Free Software
  Foundation; either version 3, or (at your option) any later version.

  TALER is distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
  A PARTICULAR PURPOSE.  See the GNU General Public License for more details.

  You should have received a copy of the GNU General Public License along with
  TALER; see the file COPYING.  If not, see <http://www.gnu.org/licenses/>
*/
/**
 * @file exchange/taler-exchange-httpd_db.h
 * @brief High-level (transactional-layer) database operations for the exchange
 * @author Chrisitan Grothoff
 */
#ifndef TALER_EXCHANGE_HTTPD_DB_H
#define TALER_EXCHANGE_HTTPD_DB_H

#include <microhttpd.h>
#include "taler_exchangedb_plugin.h"

/**
 * Function implementing a database transaction.  Runs the transaction
 * logic; IF it returns a non-error code, the transaction logic MUST
 * NOT queue a MHD response.  IF it returns an hard error, the
 * transaction logic MUST queue a MHD response and set @a mhd_ret.  IF
 * it returns the soft error code, the function MAY be called again to
 * retry and MUST not queue a MHD response.
 *
 * @param cls closure
 * @param connection MHD request which triggered the transaction
 * @param session database session to use
 * @param[out] mhd_ret set to MHD response status for @a connection,
 *             if transaction failed (!)
 * @return transaction status
 */
typedef enum GNUNET_DB_QueryStatus
(*TEH_DB_TransactionCallback)(void *cls,
			      struct MHD_Connection *connection,
			      struct TALER_EXCHANGEDB_Session *session,
			      int *mhd_ret);


/**
 * Run a database transaction for @a connection.
 * Starts a transaction and calls @a cb.  Upon success,
 * attempts to commit the transaction.  Upon soft failures,
 * retries @a cb a few times.  Upon hard or persistent soft
 * errors, generates an error message for @a connection.
 * 
 * @param connection MHD connection to run @a cb for
 * @param[out] set to MHD response code, if transaction failed
 * @param cb callback implementing transaction logic
 * @param cb_cls closure for @a cb, must be read-only!
 * @return #GNUNET_OK on success, #GNUNET_SYSERR on failure
 */
int
TEH_DB_run_transaction (struct MHD_Connection *connection,
			int *mhd_ret,
			TEH_DB_TransactionCallback cb,
			void *cb_cls);


/**
 * Calculate the total value of all transactions performed.
 * Stores @a off plus the cost of all transactions in @a tl
 * in @a ret.
 *
 * @param tl transaction list to process
 * @param off offset to use as the starting value
 * @param[out] ret where the resulting total is to be stored
 * @return #GNUNET_OK on success, #GNUNET_SYSERR on errors
 */
// FIXME: maybe move to another module?
int
TEH_DB_calculate_transaction_list_totals (struct TALER_EXCHANGEDB_TransactionList *tl,
					  const struct TALER_Amount *off,
					  struct TALER_Amount *ret);


/**
 * Execute a "/refund".  Returns a confirmation that the refund
 * was successful, or a failure if we are not aware of a matching
 * /deposit or if it is too late to do the refund.
 *
 * @param connection the MHD connection to handle
 * @param refund refund details
 * @return MHD result code
 */
int
TEH_DB_execute_refund (struct MHD_Connection *connection,
                       const struct TALER_EXCHANGEDB_Refund *refund);


/**
 * @brief Details about a melt operation of an individual coin.
 */
struct TEH_DB_MeltDetails
{

  /**
   * Information about the coin being melted.
   */
  struct TALER_CoinPublicInfo coin_info;

  /**
   * Signature allowing the melt (using
   * a `struct TALER_EXCHANGEDB_RefreshMeltConfirmSignRequestBody`) to sign over.
   */
  struct TALER_CoinSpendSignatureP melt_sig;

  /**
   * How much of the coin's value did the client allow to be melted?
   * This amount includes the fees, so the final amount contributed
   * to the melt is this value minus the fee for melting the coin.
   */
  struct TALER_Amount melt_amount_with_fee;

  /**
   * What fee is earned by the exchange?  Set delayed during
   * #verify_coin_public_info().
   */
  struct TALER_Amount melt_fee;
};


/**
 * Execute a "/refresh/melt". We have been given a list of valid
 * coins and a request to melt them into the given
 * @a refresh_session_pub.  Check that the coins all have the
 * required value left and if so, store that they have been
 * melted and confirm the melting operation to the client.
 *
 * @param connection the MHD connection to handle
 * @param session_hash hash code of the session the coins are melted into
 * @param num_new_denoms number of entries in @a denom_pubs, size of y-dimension of @a commit_coin array
 * @param denom_pubs array of public denomination keys for the refresh (?)
 * @param coin_melt_detail signatures and (residual) value of and information about the respective coin to be melted
 * @param commit_coin 2d array of coin commitments (what the exchange is to sign
 *                    once the "/refres/reveal" of cut and choose is done)
 * @param transfer_pubs array of transfer public keys (what the exchange is
 *                    to return via "/refresh/link" to enable linkage in the
 *                    future) of length #TALER_CNC_KAPPA
 * @return MHD result code
 */
int
TEH_DB_execute_refresh_melt (struct MHD_Connection *connection,
                             const struct GNUNET_HashCode *session_hash,
                             unsigned int num_new_denoms,
                             const struct TALER_DenominationPublicKey *denom_pubs,
                             const struct TEH_DB_MeltDetails *coin_melt_detail,
                             struct TALER_EXCHANGEDB_RefreshCommitCoin *const* commit_coin,
                             const struct TALER_TransferPublicKeyP *transfer_pubs);


/**
 * Execute a "/refresh/reveal".  The client is revealing to us the
 * transfer keys for #TALER_CNC_KAPPA-1 sets of coins.  Verify that the
 * revealed transfer keys would allow linkage to the blinded coins,
 * and if so, return the signed coins for corresponding to the set of
 * coins that was not chosen.
 *
 * @param connection the MHD connection to handle
 * @param session_hash hash over the refresh session
 * @param transfer_privs array of length #TALER_CNC_KAPPA-1 with the revealed transfer keys
 * @return MHD result code
 */
int
TEH_DB_execute_refresh_reveal (struct MHD_Connection *connection,
                               const struct GNUNET_HashCode *session_hash,
                               struct TALER_TransferPrivateKeyP *transfer_privs);


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
TEH_DB_execute_refresh_link (struct MHD_Connection *connection,
                             const struct TALER_CoinSpendPublicKeyP *coin_pub);



/**
 * Add an incoming transaction to the database.
 *
 * @param connection the MHD connection to handle
 * @param reserve_pub public key of the reserve
 * @param amount amount to add to the reserve
 * @param execution_time when did we receive the wire transfer
 * @param sender_account_details which account send the funds
 * @param transfer_details information that uniquely identifies the transfer
 * @return MHD result code
 */
int
TEH_DB_execute_admin_add_incoming (struct MHD_Connection *connection,
                                   const struct TALER_ReservePublicKeyP *reserve_pub,
                                   const struct TALER_Amount *amount,
                                   struct GNUNET_TIME_Absolute execution_time,
                                   const json_t *sender_account_details,
                                   const json_t *transfer_details);


/**
 * Execute a "/track/transfer".  Returns the transaction information
 * associated with the given wire transfer identifier.
 *
 * @param connection the MHD connection to handle
 * @param wtid wire transfer identifier to resolve
 * @return MHD result code
 */
int
TEH_DB_execute_track_transfer (struct MHD_Connection *connection,
                               const struct TALER_WireTransferIdentifierRawP *wtid);


/**
 * Execute a "/track/transaction".  Returns the transfer information
 * associated with the given deposit.
 *
 * @param connection the MHD connection to handle
 * @param h_contract_terms hash of the contract
 * @param h_wire hash of the wire details
 * @param coin_pub public key of the coin to link
 * @param merchant_pub public key of the merchant
 * @return MHD result code
 */
int
TEH_DB_execute_track_transaction (struct MHD_Connection *connection,
                                  const struct GNUNET_HashCode *h_contract_terms,
                                  const struct GNUNET_HashCode *h_wire,
                                  const struct TALER_CoinSpendPublicKeyP *coin_pub,
                                  const struct TALER_MerchantPublicKeyP *merchant_pub);


/**
 * Execute a "/payback".  The validity of the coin and signature have
 * already been checked.  The database must now check that the coin is
 * not (double) spent, and execute the transaction (record details,
 * generate success or failure response).
 *
 * @param connection the MHD connection to handle
 * @param coin information about the coin
 * @param value how much are coins of the @a coin's denomination worth?
 * @param h_blind blinded coin to use for the lookup
 * @param coin_blind blinding factor used (for later verification by the auditor)
 * @param coin_sig signature of the coin
 * @return MHD result code
 */
int
TEH_DB_execute_payback (struct MHD_Connection *connection,
                        const struct TALER_CoinPublicInfo *coin,
                        const struct TALER_Amount *value,
                        const struct GNUNET_HashCode *h_blind,
                        const struct TALER_DenominationBlindingKeyP *coin_blind,
                        const struct TALER_CoinSpendSignatureP *coin_sig);


#endif
/* TALER_EXCHANGE_HTTPD_DB_H */
