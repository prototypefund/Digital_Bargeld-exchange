/*
  This file is part of TALER
  Copyright (C) 2016, 2017 Inria

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
 * @file auditor/taler-auditor.c
 * @brief audits an exchange database.
 * @author Christian Grothoff
 *
 * NOTE:
 * - This auditor does not verify that 'reserves_in' actually matches
 *   the wire transfers from the bank. This needs to be checked separately!
 * - Similarly, we do not check that the outgoing wire transfers match those
 *   given in the aggregation_tracking table. This needs to be checked separately!
 *
 * TODO:
 * - modify auditordb to allow multiple last serial IDs per table in progress tracking
 * - implement coin/denomination audit
 * - implement merchant deposit audit
 *   - see if we need more tables there
 * - write reporting logic to output nice report beyond GNUNET_log()
 *
 * EXTERNAL:
 * - add tool to pay-back expired reserves (#4956), and support here
 * - add tool to verify 'reserves_in' from wire transfer inspection
 * - add tool to trigger computation of historic revenues
 *   (move balances from 'current' revenue/profits to 'historic' tables)
 */
#include "platform.h"
#include <gnunet/gnunet_util_lib.h>
#include "taler_auditordb_plugin.h"
#include "taler_exchangedb_plugin.h"
#include "taler_json_lib.h"
#include "taler_signatures.h"


/**
 * How many coin histories do we keep in RAM at any given point in
 * time? Used bound memory consumption of the auditor. Larger values
 * reduce database accesses.
 *
 * Set to a VERY low value here for testing. Practical values may be
 * in the millions.
 */
#define MAX_COIN_SUMMARIES 4

/**
 * Return value from main().
 */
static int global_ret;

/**
 * Handle to access the exchange's database.
 */
static struct TALER_EXCHANGEDB_Plugin *edb;

/**
 * Which currency are we doing the audit for?
 */
static char *currency;

/**
 * Our session with the #edb.
 */
static struct TALER_EXCHANGEDB_Session *esession;

/**
 * Handle to access the auditor's database.
 */
static struct TALER_AUDITORDB_Plugin *adb;

/**
 * Our session with the #adb.
 */
static struct TALER_AUDITORDB_Session *asession;

/**
 * Master public key of the exchange to audit.
 */
static struct TALER_MasterPublicKeyP master_pub;

/**
 * Last reserve_in serial ID seen.
 */
static struct TALER_AUDITORDB_ProgressPoint pp;


/* ***************************** Report logic **************************** */


/**
 * Called in case we detect an emergency situation where the exchange
 * is paying out a larger amount on a denomination than we issued in
 * that denomination.  This means that the exchange's private keys
 * might have gotten compromised, and that we need to trigger an
 * emergency request to all wallets to deposit pending coins for the
 * denomination (and as an exchange suffer a huge financial loss).
 *
 * @param dki denomination key where the loss was detected
 */
static void
report_emergency (const struct TALER_EXCHANGEDB_DenominationKeyInformationP *dki)
{
  /* TODO: properly implement #3887, including how to continue the
     audit after the emergency. */
  GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
              "Emergency detected for denomination %s\n",
              GNUNET_h2s (&dki->properties.denom_hash));
}


/**
 * Report a (serious) inconsistency in the exchange's database.
 *
 * @param table affected table
 * @param rowid affected row, UINT64_MAX if row is missing
 * @param diagnostic message explaining the problem
 */
static void
report_row_inconsistency (const char *table,
                          uint64_t rowid,
                          const char *diagnostic)
{
  // TODO: implement proper reporting logic writing to file.
  GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
              "Database inconsistency detected in table %s at row %llu: %s\n",
              table,
              (unsigned long long) rowid,
              diagnostic);
}


/**
 * Report a minor inconsistency in the exchange's database (i.e. something
 * relating to timestamps that should have no financial implications).
 *
 * @param table affected table
 * @param rowid affected row, UINT64_MAX if row is missing
 * @param diagnostic message explaining the problem
 */
static void
report_row_minor_inconsistency (const char *table,
                                uint64_t rowid,
                                const char *diagnostic)
{
  // TODO: implement proper reporting logic writing to file.
  GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
              "Minor inconsistency detected in table %s at row %llu: %s\n",
              table,
              (unsigned long long) rowid,
              diagnostic);
}


/**
 * Report a global inconsistency with respect to a reserve.
 *
 * @param reserve_pub the affected reserve
 * @param expected expected amount
 * @param observed observed amount
 * @param diagnostic message explaining what @a expected and @a observed refer to
 */
static void
report_reserve_inconsistency (const struct TALER_ReservePublicKeyP *reserve_pub,
                              const struct TALER_Amount *expected,
                              const struct TALER_Amount *observed,
                              const char *diagnostic)
{
  // TODO: implement proper reporting logic writing to file, include amounts.
  GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
              "Reserve inconsistency detected affecting reserve %s: %s\n",
              TALER_B2S (reserve_pub),
              diagnostic);
}


/**
 * Report the final result on the reserve balances of the exchange.
 * The reserve must have @a total_balance in its escrow account just
 * to cover outstanding reserve funds (outstanding coins are on top).
 * The reserve has made @a total_fee_balance in profit from withdrawal
 * operations alone.
 *
 * Note that this is for the "ongoing" reporting period.  Historic
 * revenue (as stored via the "insert_historic_reserve_revenue")
 * is not included in the @a total_fee_balance.
 *
 * @param total_balance how much money (in total) is left in all of the
 *        reserves (that has not been withdrawn)
 * @param total_fee_balance how much money (in total) did the reserve
 *        make from withdrawal fees
 */
static void
report_reserve_balance (const struct TALER_Amount *total_balance,
                        const struct TALER_Amount *total_fee_balance)
{
  char *balance;
  char *fees;

  balance = TALER_amount_to_string (total_balance);
  fees = TALER_amount_to_string (total_fee_balance);
  // TODO: implement proper reporting logic writing to file.
  GNUNET_log (GNUNET_ERROR_TYPE_MESSAGE,
              "Total escrow balance to be held for reserves: %s\n",
              balance);
  GNUNET_log (GNUNET_ERROR_TYPE_MESSAGE,
              "Total profits made from reserves: %s\n",
              fees);
  GNUNET_free (fees);
  GNUNET_free (balance);
}


/* ************************* Transaction-global state ************************ */

/**
 * Results about denominations, cached per-transaction.
 */
static struct GNUNET_CONTAINER_MultiHashMap *denominations;


/**
 * Obtain information about a @a denom_pub.
 *
 * @param denom_pub key to look up
 * @param[out] dki set to detailed information about @a denom_pub, NULL if not found, must
 *                 NOT be freed by caller
 * @param[out] dh set to the hash of @a denom_pub, may be NULL
 * @return #GNUNET_OK on success, #GNUNET_NO for not found, #GNUNET_SYSERR for DB error
 */
static int
get_denomination_info (const struct TALER_DenominationPublicKey *denom_pub,
                       const struct TALER_EXCHANGEDB_DenominationKeyInformationP **dki,
                       struct GNUNET_HashCode *dh)
{
  struct GNUNET_HashCode hc;
  struct TALER_EXCHANGEDB_DenominationKeyInformationP *dkip;
  int ret;

  if (NULL == dh)
    dh = &hc;
  GNUNET_CRYPTO_rsa_public_key_hash (denom_pub->rsa_public_key,
                                     dh);
  dkip = GNUNET_CONTAINER_multihashmap_get (denominations,
                                            dh);
  if (NULL != dkip)
    {
      /* cache hit */
      *dki = dkip;
      return GNUNET_OK;
    }
  dkip = GNUNET_new (struct TALER_EXCHANGEDB_DenominationKeyInformationP);
  ret = edb->get_denomination_info (edb->cls,
                                    esession,
                                    denom_pub,
                                    dkip);
  if (GNUNET_OK != ret)
  {
    GNUNET_free (dkip);
    GNUNET_break (GNUNET_NO == ret);
    *dki = NULL;
    return ret;
  }
  *dki = dkip;
  GNUNET_assert (GNUNET_OK ==
                 GNUNET_CONTAINER_multihashmap_put (denominations,
                                                    dh,
                                                    dkip,
                                                    GNUNET_CONTAINER_MULTIHASHMAPOPTION_UNIQUE_ONLY));
  return GNUNET_OK;
}


/**
 * Free denomination key information.
 *
 * @param cls NULL
 * @param key unused
 * @param value the `struct TALER_EXCHANGEDB_DenominationKeyInformationP *` to free
 * @return #GNUNET_OK (continue to iterate)
 */
static int
free_dk_info (void *cls,
              const struct GNUNET_HashCode *key,
              void *value)
{
  struct TALER_EXCHANGEDB_DenominationKeyInformationP *dki = value;

  GNUNET_free (dki);
  return GNUNET_OK;
}


/**
 * Purge transaction global state cache, the transaction is
 * done and we do not want to have the state cross over to
 * the next transaction.
 */
static void
clear_transaction_state_cache ()
{
  if (NULL == denominations)
    return;
  GNUNET_CONTAINER_multihashmap_iterate (denominations,
                                         &free_dk_info,
                                         NULL);
  GNUNET_CONTAINER_multihashmap_destroy (denominations);
  denominations = NULL;
}


/* ***************************** Analyze reserves ************************ */
/* This logic checks the reserves_in, reserves_out and reserves-tables */

/**
 * Summary data we keep per reserve.
 */
struct ReserveSummary
{
  /**
   * Public key of the reserve.
   * Always set when the struct is first initialized.
   */
  struct TALER_ReservePublicKeyP reserve_pub;

  /**
   * Sum of all incoming transfers during this transaction.
   * Updated only in #handle_reserve_in().
   */
  struct TALER_Amount total_in;

  /**
   * Sum of all outgoing transfers during this transaction (includes fees).
   * Updated only in #handle_reserve_out().
   */
  struct TALER_Amount total_out;

  /**
   * Sum of withdraw fees encountered during this transaction.
   */
  struct TALER_Amount total_fee;

  /**
   * Previous balance of the reserve as remembered by the auditor.
   * (updated based on @e total_in and @e total_out at the end).
   */
  struct TALER_Amount a_balance;

  /**
   * Previous withdraw fee balance of the reserve, as remembered by the auditor.
   * (updated based on @e total_fee at the end).
   */
  struct TALER_Amount a_withdraw_fee_balance;

  /**
   * Previous reserve expiration data, as remembered by the auditor.
   * (updated on-the-fly in #handle_reserve_in()).
   */
  struct GNUNET_TIME_Absolute a_expiration_date;

  /**
   * Previous last processed reserve_in serial ID, as remembered by the auditor.
   * (updated on-the-fly in #handle_reserve_in()).
   */
  uint64_t a_last_reserve_in_serial_id;

  /**
   * Previous last processed reserve_out serial ID, as remembered by the auditor.
   * (updated on-the-fly in #handle_reserve_out()).
   */
  uint64_t a_last_reserve_out_serial_id;

  /**
   * Did we have a previous reserve info?  Used to decide between
   * UPDATE and INSERT later.  Initialized in
   * #load_auditor_reserve_summary() together with the a-* values
   * (if available).
   */
  int had_ri;

};


/**
 * Load the auditor's remembered state about the reserve into @a rs.
 * The "total_in" and "total_out" amounts of @a rs must already be
 * initialized (so we can determine the currency).
 *
 * @param[in|out] rs reserve summary to (fully) initialize
 * @return #GNUNET_OK on success, #GNUNET_SYSERR on DB errors
 */
static int
load_auditor_reserve_summary (struct ReserveSummary *rs)
{
  int ret;
  uint64_t rowid;

  ret = adb->get_reserve_info (adb->cls,
                               asession,
                               &rs->reserve_pub,
                               &master_pub,
                               &rowid,
                               &rs->a_balance,
                               &rs->a_withdraw_fee_balance,
                               &rs->a_expiration_date,
                               &rs->a_last_reserve_in_serial_id,
                               &rs->a_last_reserve_out_serial_id);
  if (GNUNET_SYSERR == ret)
  {
    GNUNET_break (0);
    return GNUNET_SYSERR;
  }
  if (GNUNET_NO == ret)
  {
    rs->had_ri = GNUNET_NO;
    GNUNET_assert (GNUNET_OK ==
                   TALER_amount_get_zero (rs->total_in.currency,
                                          &rs->a_balance));
    GNUNET_assert (GNUNET_OK ==
                   TALER_amount_get_zero (rs->total_in.currency,
                                          &rs->a_withdraw_fee_balance));
    return GNUNET_OK;
  }
  rs->had_ri = GNUNET_YES;
  if ( (GNUNET_YES !=
        TALER_amount_cmp_currency (&rs->a_balance,
                                   &rs->a_withdraw_fee_balance)) ||
       (GNUNET_YES !=
        TALER_amount_cmp_currency (&rs->total_in,
                                   &rs->a_balance)) )
  {
    report_row_inconsistency ("auditor-reserve-info",
                              rowid,
                              "currencies for reserve differ");
    /* TODO: find a sane way to continue... */
    GNUNET_break (0);
    return GNUNET_SYSERR;
  }
  return GNUNET_OK;
}


/**
 * Closure to the various callbacks we make while checking a reserve.
 */
struct ReserveContext
{
  /**
   * Map from hash of reserve's public key to a `struct ReserveSummary`.
   */
  struct GNUNET_CONTAINER_MultiHashMap *reserves;

  /**
   * Total balance in all reserves (updated).
   */
  struct TALER_Amount total_balance;

  /**
   * Total withdraw fees gotten in all reserves (updated).
   */
  struct TALER_Amount total_fee_balance;

};


/**
 * Function called with details about incoming wire transfers.
 *
 * @param cls our `struct ReserveContext`
 * @param rowid unique serial ID for the refresh session in our DB
 * @param reserve_pub public key of the reserve (also the WTID)
 * @param credit amount that was received
 * @param sender_account_details information about the sender's bank account
 * @param transfer_details information that uniquely identifies the wire transfer
 * @param execution_date when did we receive the funds
 * @return #GNUNET_OK to continue to iterate, #GNUNET_SYSERR to stop
 */
static int
handle_reserve_in (void *cls,
                   uint64_t rowid,
                   const struct TALER_ReservePublicKeyP *reserve_pub,
                   const struct TALER_Amount *credit,
                   const json_t *sender_account_details,
                   const json_t *transfer_details,
                   struct GNUNET_TIME_Absolute execution_date)
{
  struct ReserveContext *rc = cls;
  struct GNUNET_HashCode key;
  struct ReserveSummary *rs;
  struct GNUNET_TIME_Absolute expiry;

  GNUNET_assert (rowid >= pp.last_reserve_in_serial_id); /* should be monotonically increasing */
  pp.last_reserve_in_serial_id = rowid + 1;
  GNUNET_CRYPTO_hash (reserve_pub,
                      sizeof (*reserve_pub),
                      &key);
  rs = GNUNET_CONTAINER_multihashmap_get (rc->reserves,
                                          &key);
  if (NULL == rs)
  {
    rs = GNUNET_new (struct ReserveSummary);
    rs->reserve_pub = *reserve_pub;
    rs->total_in = *credit;
    GNUNET_assert (GNUNET_OK ==
                   TALER_amount_get_zero (credit->currency,
                                          &rs->total_out));
    GNUNET_assert (GNUNET_OK ==
                   TALER_amount_get_zero (credit->currency,
                                          &rs->total_fee));
    if (GNUNET_OK !=
        load_auditor_reserve_summary (rs))
    {
      GNUNET_break (0);
      GNUNET_free (rs);
      return GNUNET_SYSERR;
    }
    GNUNET_assert (GNUNET_OK ==
                   GNUNET_CONTAINER_multihashmap_put (rc->reserves,
                                                      &key,
                                                      rs,
                                                      GNUNET_CONTAINER_MULTIHASHMAPOPTION_UNIQUE_ONLY));
  }
  else
  {
    GNUNET_assert (GNUNET_OK ==
                   TALER_amount_add (&rs->total_in,
                                     &rs->total_in,
                                     credit));
  }
  GNUNET_assert (rowid >= rs->a_last_reserve_in_serial_id);
  rs->a_last_reserve_in_serial_id = rowid + 1;
  expiry = GNUNET_TIME_absolute_add (execution_date,
                                     TALER_IDLE_RESERVE_EXPIRATION_TIME);
  rs->a_expiration_date = GNUNET_TIME_absolute_max (rs->a_expiration_date,
                                                    expiry);
  return GNUNET_OK;
}


/**
 * Function called with details about withdraw operations.  Verifies
 * the signature and updates the reserve's balance.
 *
 * @param cls our `struct ReserveContext`
 * @param rowid unique serial ID for the refresh session in our DB
 * @param h_blind_ev blinded hash of the coin's public key
 * @param denom_pub public denomination key of the deposited coin
 * @param denom_sig signature over the deposited coin
 * @param reserve_pub public key of the reserve
 * @param reserve_sig signature over the withdraw operation
 * @param execution_date when did the wallet withdraw the coin
 * @param amount_with_fee amount that was withdrawn
 * @return #GNUNET_OK to continue to iterate, #GNUNET_SYSERR to stop
 */
static int
handle_reserve_out (void *cls,
                    uint64_t rowid,
                    const struct GNUNET_HashCode *h_blind_ev,
                    const struct TALER_DenominationPublicKey *denom_pub,
                    const struct TALER_DenominationSignature *denom_sig,
                    const struct TALER_ReservePublicKeyP *reserve_pub,
                    const struct TALER_ReserveSignatureP *reserve_sig,
                    struct GNUNET_TIME_Absolute execution_date,
                    const struct TALER_Amount *amount_with_fee)
{
  struct ReserveContext *rc = cls;
  struct TALER_WithdrawRequestPS wsrd;
  struct GNUNET_HashCode key;
  struct ReserveSummary *rs;
  const struct TALER_EXCHANGEDB_DenominationKeyInformationP *dki;
  struct TALER_Amount withdraw_fee;
  struct GNUNET_TIME_Absolute valid_start;
  struct GNUNET_TIME_Absolute expire_withdraw;
  int ret;

  /* should be monotonically increasing */
  GNUNET_assert (rowid >= pp.last_reserve_out_serial_id);
  pp.last_reserve_out_serial_id = rowid + 1;

  /* lookup denomination pub data (make sure denom_pub is valid, establish fees) */
  ret = get_denomination_info (denom_pub,
                               &dki,
                               &wsrd.h_denomination_pub);
  if (GNUNET_SYSERR == ret)
  {
    GNUNET_break (0);
    return GNUNET_SYSERR;
  }
  if (GNUNET_NO == ret)
  {
    report_row_inconsistency ("withdraw",
                              rowid,
                              "denomination key not found (foreign key constraint violated)");
    return GNUNET_OK;
  }

  /* check that execution date is within withdraw range for denom_pub  */
  valid_start = GNUNET_TIME_absolute_ntoh (dki->properties.start);
  expire_withdraw = GNUNET_TIME_absolute_ntoh (dki->properties.expire_withdraw);
  if ( (valid_start.abs_value_us > execution_date.abs_value_us) ||
       (expire_withdraw.abs_value_us < execution_date.abs_value_us) )
  {
    report_row_minor_inconsistency ("withdraw",
                                    rowid,
                                    "denomination key not valid at time of withdrawal");
  }

  /* check reserve_sig */
  wsrd.purpose.purpose = htonl (TALER_SIGNATURE_WALLET_RESERVE_WITHDRAW);
  wsrd.purpose.size = htonl (sizeof (wsrd));
  TALER_amount_hton (&wsrd.amount_with_fee,
                     amount_with_fee);
  wsrd.withdraw_fee = dki->properties.fee_withdraw;
  wsrd.h_coin_envelope = *h_blind_ev;
  if (GNUNET_OK !=
      GNUNET_CRYPTO_eddsa_verify (TALER_SIGNATURE_WALLET_RESERVE_WITHDRAW,
                                  &wsrd.purpose,
                                  &reserve_sig->eddsa_signature,
                                  &reserve_pub->eddsa_pub))
  {
    report_row_inconsistency ("withdraw",
                              rowid,
                              "invalid signature for withdrawal");
    return GNUNET_OK;
  }

  GNUNET_CRYPTO_hash (reserve_pub,
                      sizeof (*reserve_pub),
                      &key);
  rs = GNUNET_CONTAINER_multihashmap_get (rc->reserves,
                                          &key);
  if (NULL == rs)
  {
    rs = GNUNET_new (struct ReserveSummary);
    rs->reserve_pub = *reserve_pub;
    rs->total_out = *amount_with_fee;
    GNUNET_assert (GNUNET_OK ==
                   TALER_amount_get_zero (amount_with_fee->currency,
                                          &rs->total_in));
    GNUNET_assert (GNUNET_OK ==
                   TALER_amount_get_zero (amount_with_fee->currency,
                                          &rs->total_fee));
    if (GNUNET_OK !=
        load_auditor_reserve_summary (rs))
    {
      GNUNET_break (0);
      GNUNET_free (rs);
      return GNUNET_SYSERR;
    }
    GNUNET_assert (GNUNET_OK ==
                   GNUNET_CONTAINER_multihashmap_put (rc->reserves,
                                                      &key,
                                                      rs,
                                                      GNUNET_CONTAINER_MULTIHASHMAPOPTION_UNIQUE_ONLY));
  }
  else
  {
    GNUNET_assert (GNUNET_OK ==
                   TALER_amount_add (&rs->total_out,
                                     &rs->total_out,
                                     amount_with_fee));
  }
  GNUNET_assert (rowid >= rs->a_last_reserve_out_serial_id);
  rs->a_last_reserve_out_serial_id = rowid + 1;

  TALER_amount_ntoh (&withdraw_fee,
                     &dki->properties.fee_withdraw);
  GNUNET_assert (GNUNET_OK ==
                 TALER_amount_add (&rs->total_fee,
                                   &rs->total_fee,
                                   &withdraw_fee));

  return GNUNET_OK;
}


/**
 * Check that the reserve summary matches what the exchange database
 * thinks about the reserve, and update our own state of the reserve.
 *
 * Remove all reserves that we are happy with from the DB.
 *
 * @param cls our `struct ReserveContext`
 * @param key hash of the reserve public key
 * @param value a `struct ReserveSummary`
 * @return #GNUNET_OK to process more entries
 */
static int
verify_reserve_balance (void *cls,
                        const struct GNUNET_HashCode *key,
                        void *value)
{
  struct ReserveContext *rc = cls;
  struct ReserveSummary *rs = value;
  struct TALER_EXCHANGEDB_Reserve reserve;
  struct TALER_Amount balance;
  int ret;

  ret = GNUNET_OK;
  reserve.pub = rs->reserve_pub;
  if (GNUNET_OK !=
      edb->reserve_get (edb->cls,
                        esession,
                        &reserve))
  {
    char *diag;

    GNUNET_asprintf (&diag,
                     "Failed to find summary for reserve `%s'\n",
                     TALER_B2S (&rs->reserve_pub));
    report_row_inconsistency ("reserve-summary",
                              UINT64_MAX,
                              diag);
    GNUNET_free (diag);
    return GNUNET_OK;
  }

  if (GNUNET_OK !=
      TALER_amount_add (&balance,
                        &rs->total_in,
                        &rs->a_balance))
  {
    report_reserve_inconsistency (&rs->reserve_pub,
                                  &rs->total_in,
                                  &rs->a_balance,
                                  "could not add old balance to new balance");
    goto cleanup;
  }

  if (GNUNET_SYSERR ==
      TALER_amount_subtract (&balance,
                             &balance,
                             &rs->total_out))
  {
    report_reserve_inconsistency (&rs->reserve_pub,
                                  &rs->total_in,
                                  &rs->total_out,
                                  "available balance insufficient to cover transfers");
    goto cleanup;
  }
  if (0 != TALER_amount_cmp (&balance,
                             &reserve.balance))
  {
    report_reserve_inconsistency (&rs->reserve_pub,
                                  &balance,
                                  &reserve.balance,
                                  "computed balance does not match stored balance");
    goto cleanup;
  }

  if (0 == GNUNET_TIME_absolute_get_remaining (rs->a_expiration_date).rel_value_us)
  {
    /* TODO: handle case where reserve is expired! (#4956) */
    /* NOTE: we may or may not have seen the wire-back transfer at this time,
       as the expiration may have just now happened.
       (That is, after we add the table structures and the logic to track
       such transfers...) */
  }

  if ( (0ULL == balance.value) &&
       (0U == balance.fraction) )
  {
    /* TODO: balance is zero, drop reserve details (and then do not update/insert) */
    if (rs->had_ri)
    {
      ret = adb->del_reserve_info (adb->cls,
                                   asession,
                                   &rs->reserve_pub,
                                   &master_pub);
      if (GNUNET_SYSERR == ret)
      {
        GNUNET_break (0);
        goto cleanup;
      }
      if (GNUNET_NO == ret)
      {
        GNUNET_break (0);
        ret = GNUNET_SYSERR;
        goto cleanup;
      }
    }
    ret = GNUNET_OK;
    goto cleanup;
  }

  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Reserve balance `%s' OK\n",
              TALER_B2S (&rs->reserve_pub));

  /* Add withdraw fees we encountered to totals */
  if (GNUNET_YES !=
      TALER_amount_add (&rs->a_withdraw_fee_balance,
                        &rs->a_withdraw_fee_balance,
                        &rs->total_fee))
  {
    GNUNET_break (0);
    ret = GNUNET_SYSERR;
    goto cleanup;
  }
  if (rs->had_ri)
    ret = adb->update_reserve_info (adb->cls,
                                    asession,
                                    &rs->reserve_pub,
                                    &master_pub,
                                    &balance,
                                    &rs->a_withdraw_fee_balance,
                                    rs->a_expiration_date,
                                    rs->a_last_reserve_in_serial_id,
                                    rs->a_last_reserve_out_serial_id);
  else
    ret = adb->insert_reserve_info (adb->cls,
                                    asession,
                                    &rs->reserve_pub,
                                    &master_pub,
                                    &balance,
                                    &rs->a_withdraw_fee_balance,
                                    rs->a_expiration_date,
                                    rs->a_last_reserve_in_serial_id,
                                    rs->a_last_reserve_out_serial_id);

  if ( (GNUNET_YES !=
        TALER_amount_add (&rc->total_balance,
                          &rc->total_balance,
                          &rs->total_in)) ||
       (GNUNET_SYSERR ==
        TALER_amount_subtract (&rc->total_balance,
                               &rc->total_balance,
                               &rs->total_out)) ||
       (GNUNET_YES !=
        TALER_amount_add (&rc->total_fee_balance,
                          &rc->total_fee_balance,
                          &rs->total_fee)) )
  {
    GNUNET_break (0);
    ret = GNUNET_SYSERR;
    goto cleanup;
  }


 cleanup:
  GNUNET_assert (GNUNET_YES ==
                 GNUNET_CONTAINER_multihashmap_remove (rc->reserves,
                                                       key,
                                                       rs));
  GNUNET_free (rs);
  return ret;
}


/**
 * Analyze reserves for being well-formed.
 *
 * @param cls NULL
 * @return #GNUNET_OK on success, #GNUNET_SYSERR on invariant violation
 */
static int
analyze_reserves (void *cls)
{
  struct ReserveContext rc;
  int ret;

  ret = adb->get_reserve_summary (adb->cls,
                                  asession,
                                  &master_pub,
                                  &rc.total_balance,
                                  &rc.total_fee_balance);
  if (GNUNET_SYSERR == ret)
  {
    GNUNET_break (0);
    return GNUNET_SYSERR;
  }

  rc.reserves = GNUNET_CONTAINER_multihashmap_create (512,
                                                      GNUNET_NO);

  if (GNUNET_OK !=
      edb->select_reserves_in_above_serial_id (edb->cls,
                                               esession,
                                               pp.last_reserve_in_serial_id,
                                               &handle_reserve_in,
                                               &rc))
    {
      GNUNET_break (0);
      return GNUNET_SYSERR;
    }
  if (GNUNET_OK !=
      edb->select_reserves_out_above_serial_id (edb->cls,
                                                esession,
                                                pp.last_reserve_out_serial_id,
                                                &handle_reserve_out,
                                                &rc))
    {
      GNUNET_break (0);
      return GNUNET_SYSERR;
    }
  /* TODO: iterate over table for reserve expiration refunds! (#4956) */

  GNUNET_CONTAINER_multihashmap_iterate (rc.reserves,
                                         &verify_reserve_balance,
                                         &rc);
  GNUNET_break (0 ==
                GNUNET_CONTAINER_multihashmap_size (rc.reserves));
  GNUNET_CONTAINER_multihashmap_destroy (rc.reserves);


  if (GNUNET_NO == ret)
  {
    ret = adb->insert_reserve_summary (adb->cls,
                                       asession,
                                       &master_pub,
                                       &rc.total_balance,
                                       &rc.total_fee_balance);
  }
  else
  {
    ret = adb->update_reserve_summary (adb->cls,
                                       asession,
                                       &master_pub,
                                       &rc.total_balance,
                                       &rc.total_fee_balance);
  }
  // FIXME: handle error in 'ret'!
  report_reserve_balance (&rc.total_balance,
                          &rc.total_fee_balance);
  return GNUNET_OK;
}


/* ************************* Analyze coins ******************** */
/* This logic checks that the exchange did the right thing for each
   coin, checking deposits, refunds, refresh* and known_coins
   tables */

/**
 * Summary data we keep per coin.
 */
struct CoinSummary
{
  /**
   * Denomination of the coin with fee structure.
   */
  const struct TALER_EXCHANGEDB_DenominationKeyInformationP *dki;

  /**
   * Hash of @e coin_pub.
   */
  struct GNUNET_HashCode coin_hash;

  /**
   * Public key of the coin.
   */
  struct TALER_CoinSpendPublicKeyP coin_pub;

  /**
   * List of transactions this coin was involved in.
   */
  struct TALER_EXCHANGEDB_TransactionList *tl;

};


/**
 * Summary data we keep per denomination.
 */
struct DenominationSummary
{
  /**
   * Total value of coins issued with this denomination key.
   */
  struct TALER_Amount denom_balance;

  /**
   * Up to which point have we processed reserves_out?
   */
  uint64_t last_reserve_out_serial_id;

  /**
   * Up to which point have we processed deposits?
   */
  uint64_t last_deposit_serial_id;

  /**
   * Up to which point have we processed melts?
   */
  uint64_t last_melt_serial_id;

  /**
   * Up to which point have we processed refunds?
   */
  uint64_t last_refund_serial_id;

  /**
   * #GNUNET_YES if this record already existed in the DB.
   * Used to decide between insert/update in
   * #sync_denomination().
   */
  int in_db;
};


/**
 * Closure for callbacks during #analyze_coins().
 */
struct CoinContext
{

  /**
   * Map for tracking information about coins.
   */
  struct GNUNET_CONTAINER_MultiHashMap *coins;

  /**
   * Array of the coins in @e coins.  Used to expire coins
   * in a circular ring-buffer like fashion (to keep the
   * working set in @e coins bounded).
   */
  struct CoinSummary summaries[MAX_COIN_SUMMARIES];

  /**
   * Map for tracking information about denominations.
   */
  struct GNUNET_CONTAINER_MultiHashMap *denominations;

  /**
   * Total outstanding balances across all denomination keys.
   */
  struct TALER_Amount denom_balance;

  /**
   * Total deposit fees earned so far.
   */
  struct TALER_Amount deposit_fee_balance;

  /**
   * Total melt fees earned so far.
   */
  struct TALER_Amount melt_fee_balance;

  /**
   * Total refund fees earned so far.
   */
  struct TALER_Amount refund_fee_balance;

  /**
   * Current financial risk of the exchange operator with respect
   * to key compromise.
   */
  struct TALER_Amount risk;

  /**
   * Current write/replace offset in the circular @e summaries buffer.
   */
  unsigned int summaries_off;

};


/**
 * Initialize information about denomination from the database.
 *
 * @param denom_hash hash of the public key of the denomination
 * @param[out] ds summary to initialize
 * @return #GNUNET_OK on success, #GNUNET_SYSERR on error
 */
static int
init_denomination (const struct GNUNET_HashCode *denom_hash,
                   struct DenominationSummary *ds)
{
  int ret;

  ret = adb->get_denomination_balance (adb->cls,
                                       asession,
                                       denom_hash,
                                       &ds->denom_balance,
                                       &ds->last_reserve_out_serial_id,
                                       &ds->last_deposit_serial_id,
                                       &ds->last_melt_serial_id,
                                       &ds->last_refund_serial_id);
  if (GNUNET_OK == ret)
  {
    ds->in_db = GNUNET_YES;
    return GNUNET_OK;
  }
  if (GNUNET_SYSERR == ret)
  {
    GNUNET_break (0);
    return GNUNET_SYSERR;
  }
  GNUNET_assert (GNUNET_OK ==
                 TALER_amount_get_zero (currency,
                                        &ds->denom_balance));
  return GNUNET_OK;
}


/**
 * Obtain the denomination summary for the given @a dh
 *
 * @param cc our execution context
 * @param dh the denomination hash to use for the lookup
 * @return NULL on error
 */
static struct DenominationSummary *
get_denomination_summary (struct CoinContext *cc,
                          const struct GNUNET_HashCode *dh)
{
  struct DenominationSummary *ds;

  ds = GNUNET_CONTAINER_multihashmap_get (cc->denominations,
                                          dh);
  if (NULL != ds)
    return ds;
  ds = GNUNET_new (struct DenominationSummary);
  if (GNUNET_OK !=
      init_denomination (dh,
                         ds))
  {
    GNUNET_break (0);
    GNUNET_free (ds);
    return NULL;
  }
  GNUNET_assert (GNUNET_OK ==
                 GNUNET_CONTAINER_multihashmap_put (cc->denominations,
                                                    dh,
                                                    ds,
                                                    GNUNET_CONTAINER_MULTIHASHMAPOPTION_UNIQUE_ONLY));
  return ds;
}


/**
 * Write information about the current knowledge about a denomination key
 * back to the database and update our global reporting data about the
 * denomination.  Also remove and free the memory of @a value.
 *
 * @param cls the `struct CoinContext`
 * @param key the hash of the denomination key
 * @param value a `struct DenominationSummary`
 * @return #GNUNET_OK (continue to iterate)
 */
static int
sync_denomination (void *cls,
                   const struct GNUNET_HashCode *denom_hash,
                   void *value)
{
  struct CoinContext *cc = cls;
  struct DenominationSummary *ds = value;
  int ret;

  // FIXME: if expired, insert into historic denomination revenue
  // and DELETE denomination balance.

  // FIXME: update "global" info about denominations (here?)

  if (ds->in_db)
    ret = adb->update_denomination_balance (adb->cls,
                                            asession,
                                            denom_hash,
                                            &ds->denom_balance,
                                            ds->last_reserve_out_serial_id,
                                            ds->last_deposit_serial_id,
                                            ds->last_melt_serial_id,
                                            ds->last_refund_serial_id);
  else
    ret = adb->insert_denomination_balance (adb->cls,
                                            asession,
                                            denom_hash,
                                            &ds->denom_balance,
                                            ds->last_reserve_out_serial_id,
                                            ds->last_deposit_serial_id,
                                            ds->last_melt_serial_id,
                                            ds->last_refund_serial_id);

  // FIXME handle errors in 'ret'

  GNUNET_assert (GNUNET_YES ==
                 GNUNET_CONTAINER_multihashmap_remove (cc->denominations,
                                                       denom_hash,
                                                       ds));
  GNUNET_free (ds);
  return GNUNET_OK;
}


/**
 * Release memory occupied by a coin summary.  Note that
 * the actual @a value is NOT allocated (comes from the
 * ring buffer), only the members of the struct need to be
 * freed.
 *
 * @param cls the `struct CoinContext`
 * @param key the hash of the coin's public key
 * @param value a `struct DenominationSummary`
 * @return #GNUNET_OK (continue to iterate)
 */
static int
free_coin (void *cls,
           const struct GNUNET_HashCode *denom_hash,
           void *value)
{
  struct CoinContext *cc = cls;
  struct CoinSummary *cs = value;

  GNUNET_assert (GNUNET_YES ==
                 GNUNET_CONTAINER_multihashmap_remove (cc->coins,
                                                       &cs->coin_hash,
                                                       cs));
  edb->free_coin_transaction_list (edb->cls,
                                   cs->tl);
  cs->tl = NULL;
  return GNUNET_OK;
}


/**
 * Check coin's transaction history for plausibility.  Does NOT check
 * the signatures (those are checked independently), but does check
 * that the amounts add up to a plausible overall picture.
 *
 * FIXME: is it wise to do this here? Maybe better to do this during
 * processing of payments to the merchants...
 *
 * @param coin_pub public key of the coin (for reporting)
 * @param dki denomination information about the coin
 * @param tl_head head of transaction history to verify
 */
static void
check_transaction_history (const struct TALER_CoinSpendPublicKeyP *coin_pub,
                           const struct TALER_EXCHANGEDB_DenominationKeyInformationP *dki,
                           const struct TALER_EXCHANGEDB_TransactionList *tl_head)
{
  struct TALER_Amount expenditures;
  struct TALER_Amount refunds;
  struct TALER_Amount fees;
  struct TALER_Amount final_expenditures;

  GNUNET_assert (NULL != tl_head);
  TALER_amount_get_zero (currency,
                         &expenditures);
  TALER_amount_get_zero (currency,
                         &refunds);
  TALER_amount_get_zero (currency,
                         &fees);
  for (const struct TALER_EXCHANGEDB_TransactionList *tl = tl_head;NULL != tl;tl = tl->next)
  {
    const struct TALER_Amount *amount_with_fee;
    const struct TALER_Amount *fee;
    const struct TALER_AmountNBO *fee_dki;
    struct TALER_Amount *add_to;
    struct TALER_Amount tmp;

    add_to = NULL;
    switch (tl->type) {
    case TALER_EXCHANGEDB_TT_DEPOSIT:
      amount_with_fee = &tl->details.deposit->amount_with_fee;
      fee = &tl->details.deposit->deposit_fee;
      fee_dki = &dki->properties.fee_deposit;
      add_to = &expenditures;
      break;
    case TALER_EXCHANGEDB_TT_REFRESH_MELT:
      amount_with_fee = &tl->details.melt->amount_with_fee;
      fee = &tl->details.melt->melt_fee;
      fee_dki = &dki->properties.fee_refresh;
      add_to = &expenditures;
      break;
    case TALER_EXCHANGEDB_TT_REFUND:
      amount_with_fee = &tl->details.refund->refund_amount;
      fee = &tl->details.refund->refund_fee;
      fee_dki = &dki->properties.fee_refund;
      add_to = &refunds;
      // FIXME: where do we check that the refund(s)
      // of the coin match the deposit(s) of the coin (by merchant, timestamp, etc.)?
      break;
    }
    GNUNET_assert (NULL != add_to); /* check switch was exhaustive */
    if (GNUNET_OK !=
        TALER_amount_add (add_to,
                          add_to,
                          amount_with_fee))
    {
      /* overflow in history already!? inconceivable! Bad DB! */
      GNUNET_break (0);
      // FIXME: report!
      return;
    }
    TALER_amount_ntoh (&tmp,
                       fee_dki);
    if (0 !=
        TALER_amount_cmp (&tmp,
                          fee))
    {
      /* Disagreement in fee structure within DB! */
      GNUNET_break (0);
      // FIXME: report!
      return;
    }
    if (GNUNET_OK !=
        TALER_amount_add (&fees,
                          &fees,
                          fee))
    {
      /* overflow in fee total? inconceivable! Bad DB! */
      GNUNET_break (0);
      // FIXME: report!
      return;
    }
  } /* for 'tl' */

  /* Finally, calculate total balance change, i.e. expenditures minus refunds */
  if (GNUNET_OK !=
      TALER_amount_subtract (&final_expenditures,
                             &expenditures,
                             &refunds))
  {
    /* refunds above expenditures? inconceivable! Bad DB! */
    GNUNET_break (0);
    // FIXME: report!
    return;
  }

}


/**
 * Obtain information about the coin from the cache or the database.
 *
 * If we obtain this information for the first time, also check that
 * the coin's transaction history is internally consistent.
 *
 * @param cc caching information
 * @param coin_pub public key of the coin to get information about
 * @return NULL on error
 */
// FIXME: move this to _outgoing_ transaction checking,
// replace HERE by something that just gets the denomination hash!
// (avoids confusion on checking coin's transaction history AND
//  makes this part WAY more efficient!)
static struct CoinSummary *
get_coin_summary (struct CoinContext *cc,
                  const struct TALER_CoinSpendPublicKeyP *coin_pub)
{
  struct CoinSummary *cs;
  struct GNUNET_HashCode chash;
  struct TALER_EXCHANGEDB_TransactionList *tl;
  const struct TALER_CoinPublicInfo *coin;
  const struct TALER_EXCHANGEDB_DenominationKeyInformationP *dki;

  /* Check cache */
  GNUNET_CRYPTO_hash (coin_pub,
                      sizeof (*coin_pub),
                      &chash);
  cs = GNUNET_CONTAINER_multihashmap_get (cc->coins,
                                          &chash);
  if (NULL != cs)
    return cs; /* cache hit */

  /* Get transaction history of @a coin_pub from DB */
  tl = edb->get_coin_transactions (edb->cls,
                                   esession,
                                   coin_pub);
  if (NULL == tl)
  {
    GNUNET_break (0);
    return NULL;
  }

  /* Obtain general denomination information about the coin */
  coin = NULL;
  switch (tl->type)
  {
  case TALER_EXCHANGEDB_TT_DEPOSIT:
    coin = &tl->details.deposit->coin;
    break;
  case TALER_EXCHANGEDB_TT_REFRESH_MELT:
    coin = &tl->details.melt->coin;
    break;
  case TALER_EXCHANGEDB_TT_REFUND:
    coin = &tl->details.refund->coin;
    break;
  }
  GNUNET_assert (NULL != coin); /* hard check that switch worked */
  if (GNUNET_OK !=
      get_denomination_info (&coin->denom_pub,
                             &dki,
                             NULL))
  {
    GNUNET_break (0);
    edb->free_coin_transaction_list (edb->cls,
                                     tl);
    return NULL;
  }

  /* verify that the transaction history we are given is reasonable */
  check_transaction_history (coin_pub,
                             dki,
                             tl);

  /* allocate coin slot in ring buffer */
  if (MAX_COIN_SUMMARIES >= cc->summaries_off)
    cc->summaries_off = 0;
  cs = &cc->summaries[cc->summaries_off++];
  GNUNET_assert (GNUNET_OK ==
                 free_coin (cc,
                            &cs->coin_hash,
                            cs));

  /* initialize 'cs' and add to cache */
  cs->coin_pub = *coin_pub;
  cs->coin_hash = chash;
  cs->tl = tl;
  cs->dki = dki;
  GNUNET_assert (GNUNET_YES ==
                 GNUNET_CONTAINER_multihashmap_put (cc->coins,
                                                    &cs->coin_hash,
                                                    cs,
                                                    GNUNET_CONTAINER_MULTIHASHMAPOPTION_UNIQUE_ONLY));
  return cs;
}


/**
 * Function called with details about all withdraw operations.
 * Updates the denomination balance and the overall balance as
 * we now have additional coins that have been issued.
 *
 * Note that the signature was already checked in
 * #handle_reserve_out(), so we do not check it again here.
 *
 * @param cls our `struct CoinContext`
 * @param rowid unique serial ID for the refresh session in our DB
 * @param h_blind_ev blinded hash of the coin's public key
 * @param denom_pub public denomination key of the deposited coin
 * @param denom_sig signature over the deposited coin
 * @param reserve_pub public key of the reserve
 * @param reserve_sig signature over the withdraw operation (verified elsewhere)
 * @param execution_date when did the wallet withdraw the coin
 * @param amount_with_fee amount that was withdrawn
 * @return #GNUNET_OK to continue to iterate, #GNUNET_SYSERR to stop
 */
static int
withdraw_cb (void *cls,
             uint64_t rowid,
             const struct GNUNET_HashCode *h_blind_ev,
             const struct TALER_DenominationPublicKey *denom_pub,
             const struct TALER_DenominationSignature *denom_sig,
             const struct TALER_ReservePublicKeyP *reserve_pub,
             const struct TALER_ReserveSignatureP *reserve_sig,
             struct GNUNET_TIME_Absolute execution_date,
             const struct TALER_Amount *amount_with_fee)
{
  struct CoinContext *cc = cls;
  struct DenominationSummary *ds;
  struct GNUNET_HashCode dh;
  const struct TALER_EXCHANGEDB_DenominationKeyInformationP *dki;
  struct TALER_Amount value;

  if (GNUNET_OK !=
      get_denomination_info (denom_pub,
                             &dki,
                             &dh))
  {
    GNUNET_break (0);
    return GNUNET_SYSERR;
  }
  ds = get_denomination_summary (cc,
                                 &dh);
  TALER_amount_ntoh (&value,
                     &dki->properties.value);
  if (GNUNET_OK !=
      TALER_amount_add (&ds->denom_balance,
                        &ds->denom_balance,
                        &value))
  {
    GNUNET_break (0);
    return GNUNET_SYSERR;
  }
  if (GNUNET_OK !=
      TALER_amount_add (&cc->denom_balance,
                        &cc->denom_balance,
                        &value))
  {
    GNUNET_break (0);
    return GNUNET_SYSERR;
  }
  return GNUNET_OK;
}


/**
 * Function called with details about coins that were melted, with the
 * goal of auditing the refresh's execution.  Verifies the signature
 * and updates our information about coins outstanding (the old coin's
 * denomination has less, the fresh coins increased outstanding
 * balances).  As a side-effect, #get_coin_summary will report
 * inconsistencies in the melted coin's balance.
 *
 * @param cls closure
 * @param rowid unique serial ID for the refresh session in our DB
 * @param coin_pub public key of the coin
 * @param coin_sig signature from the coin
 * @param amount_with_fee amount that was deposited including fee
 * @param num_newcoins how many coins were issued
 * @param noreveal_index which index was picked by the exchange in cut-and-choose
 * @param session_hash what is the session hash
 * @return #GNUNET_OK to continue to iterate, #GNUNET_SYSERR to stop
 */
static int
refresh_session_cb (void *cls,
                    uint64_t rowid,
                    const struct TALER_CoinSpendPublicKeyP *coin_pub,
                    const struct TALER_CoinSpendSignatureP *coin_sig,
                    const struct TALER_Amount *amount_with_fee,
                    uint16_t num_newcoins,
                    uint16_t noreveal_index,
                    const struct GNUNET_HashCode *session_hash)
{
  struct CoinContext *cc = cls;
  struct TALER_RefreshMeltCoinAffirmationPS rmc;
  struct CoinSummary *cs;
  const struct TALER_EXCHANGEDB_DenominationKeyInformationP *dki;
  struct DenominationSummary *dso;
  struct TALER_Amount amount_without_fee;

  cs = get_coin_summary (cc,
                         coin_pub);
  if (NULL == cs)
  {
    GNUNET_break (0);
    return GNUNET_SYSERR;
  }
  dki = cs->dki;

  /* verify melt signature */
  rmc.purpose.purpose = htonl (TALER_SIGNATURE_WALLET_COIN_MELT);
  rmc.purpose.size = htonl (sizeof (rmc));
  rmc.session_hash = *session_hash;
  TALER_amount_hton (&rmc.amount_with_fee,
                     amount_with_fee);
  rmc.melt_fee = dki->properties.fee_refresh;
  rmc.coin_pub = *coin_pub;
  if (GNUNET_OK !=
      GNUNET_CRYPTO_eddsa_verify (TALER_SIGNATURE_WALLET_COIN_MELT,
                                  &rmc.purpose,
                                  &coin_sig->eddsa_signature,
                                  &coin_pub->eddsa_pub))
  {
    report_row_inconsistency ("melt",
                              rowid,
                              "invalid signature for coin melt");
    return GNUNET_OK;
  }

  {
    struct TALER_DenominationPublicKey new_dp[num_newcoins];
    const struct TALER_EXCHANGEDB_DenominationKeyInformationP *new_dki[num_newcoins];
    struct TALER_Amount refresh_cost;

    GNUNET_assert (GNUNET_OK ==
                   TALER_amount_get_zero (amount_with_fee->currency,
                                          &refresh_cost));

    /* Update outstanding amounts for all new coin's denominations, and check
       that the resulting amounts are consistent with the value being refreshed. */
    for (unsigned int i=0;i<num_newcoins;i++)
    {
      /* lookup new coin denomination key */
      if (GNUNET_OK !=
          edb->get_refresh_order (edb->cls,
                                  esession,
                                  session_hash,
                                  i,
                                  &new_dp[i]))
      {
        GNUNET_break (0);
        return GNUNET_SYSERR;
      }
      if (GNUNET_OK !=
          get_denomination_info (&new_dp[i],
                                 &new_dki[i],
                                 NULL))
      {
        GNUNET_break (0);
        return GNUNET_SYSERR;
      }

      /* update cost of refresh */
      {
        struct TALER_Amount fee;
        struct TALER_Amount value;

        TALER_amount_ntoh (&fee,
                           &new_dki[i]->properties.fee_withdraw);
        TALER_amount_ntoh (&value,
                           &new_dki[i]->properties.value);
        if ( (GNUNET_OK !=
              TALER_amount_add (&refresh_cost,
                                &refresh_cost,
                                &fee)) ||
             (GNUNET_OK !=
              TALER_amount_add (&refresh_cost,
                                &refresh_cost,
                                &value)) )
        {
          GNUNET_break (0);
          return GNUNET_SYSERR;
        }
      }
    }

    /* compute contribution of old coin */
    {
      struct TALER_Amount melt_fee;

      TALER_amount_ntoh (&melt_fee,
                         &dki->properties.fee_refresh);
      if (GNUNET_OK !=
          TALER_amount_subtract (&amount_without_fee,
                                 amount_with_fee,
                                 &melt_fee))
      {
        GNUNET_break (0);
        return GNUNET_SYSERR;
      }
    }

    /* check old coin covers complete expenses */
    if (1 == TALER_amount_cmp (&refresh_cost,
                               &amount_without_fee))
    {
      /* refresh_cost > amount_without_fee */
      report_row_inconsistency ("melt",
                                rowid,
                                "refresh costs exceed value of melt");
      return GNUNET_OK;
    }

    /* update outstanding denomination amounts */
    for (unsigned int i=0;i<num_newcoins;i++)
    {
      struct DenominationSummary *dsi;
      struct TALER_Amount value;

      dsi = get_denomination_summary (cc,
                                      &new_dki[i]->properties.denom_hash);
      TALER_amount_ntoh (&value,
                         &new_dki[i]->properties.value);
      if (GNUNET_OK !=
          TALER_amount_add (&dsi->denom_balance,
                            &dsi->denom_balance,
                            &value))
      {
        GNUNET_break (0);
        return GNUNET_SYSERR;
      }
      if (GNUNET_OK !=
          TALER_amount_add (&cc->denom_balance,
                            &cc->denom_balance,
                            &value))
      {
        GNUNET_break (0);
        return GNUNET_SYSERR;
      }
    }
  }

  /* update old coin's denomination balance */
  dso = get_denomination_summary (cc,
                                  &dki->properties.denom_hash);
  if (GNUNET_OK !=
      TALER_amount_subtract (&dso->denom_balance,
                             &dso->denom_balance,
                             amount_with_fee))
  {
    report_emergency (dki);
    return GNUNET_SYSERR;
  }

  /* update global up melt fees */
  {
    struct TALER_Amount rfee;

    TALER_amount_ntoh (&rfee,
                       &dki->properties.fee_refresh);
    if (GNUNET_OK !=
        TALER_amount_add (&cc->melt_fee_balance,
                          &cc->melt_fee_balance,
                          &rfee))
    {
      GNUNET_break (0);
      return GNUNET_SYSERR;
    }
  }

  /* We're good! */
  return GNUNET_OK;
}


/**
 * Function called with details about deposits that have been made,
 * with the goal of auditing the deposit's execution.
 *
 * As a side-effect, #get_coin_summary will report
 * inconsistencies in the deposited coin's balance.
 *
 * @param cls closure
 * @param rowid unique serial ID for the deposit in our DB
 * @param timestamp when did the deposit happen
 * @param merchant_pub public key of the merchant
 * @param coin_pub public key of the coin
 * @param coin_sig signature from the coin
 * @param amount_with_fee amount that was deposited including fee
 * @param h_proposal_data hash of the proposal data known to merchant and customer
 * @param refund_deadline by which the merchant adviced that he might want
 *        to get a refund
 * @param wire_deadline by which the merchant adviced that he would like the
 *        wire transfer to be executed
 * @param receiver_wire_account wire details for the merchant, NULL from iterate_matching_deposits()
 * @param done flag set if the deposit was already executed (or not)
 * @return #GNUNET_OK to continue to iterate, #GNUNET_SYSERR to stop
 */
static int
deposit_cb (void *cls,
            uint64_t rowid,
            struct GNUNET_TIME_Absolute timestamp,
            const struct TALER_MerchantPublicKeyP *merchant_pub,
            const struct TALER_CoinSpendPublicKeyP *coin_pub,
            const struct TALER_CoinSpendSignatureP *coin_sig,
            const struct TALER_Amount *amount_with_fee,
            const struct GNUNET_HashCode *h_proposal_data,
            struct GNUNET_TIME_Absolute refund_deadline,
            struct GNUNET_TIME_Absolute wire_deadline,
            const json_t *receiver_wire_account,
            int done)
{
  struct CoinContext *cc = cls;
  struct CoinSummary *cs;
  const struct TALER_EXCHANGEDB_DenominationKeyInformationP *dki;
  struct DenominationSummary *ds;
  struct TALER_DepositRequestPS dr;

  cs = get_coin_summary (cc,
                         coin_pub);
  if (NULL == cs)
  {
    GNUNET_break (0);
    return GNUNET_SYSERR;
  }
  dki = cs->dki;

  /* Verify deposit signature */
  dr.purpose.purpose = htonl (TALER_SIGNATURE_WALLET_COIN_DEPOSIT);
  dr.purpose.size = htonl (sizeof (dr));
  dr.h_proposal_data = *h_proposal_data;
  if (GNUNET_OK !=
      TALER_JSON_hash (receiver_wire_account,
                       &dr.h_wire))
  {
    GNUNET_break (0);
    return GNUNET_SYSERR;
  }
  dr.timestamp = GNUNET_TIME_absolute_hton (timestamp);
  dr.refund_deadline = GNUNET_TIME_absolute_hton (refund_deadline);
  TALER_amount_hton (&dr.amount_with_fee,
                     amount_with_fee);
  dr.deposit_fee = dki->properties.fee_deposit;
  dr.merchant = *merchant_pub;
  dr.coin_pub = *coin_pub;
  if (GNUNET_OK !=
      GNUNET_CRYPTO_eddsa_verify (TALER_SIGNATURE_WALLET_COIN_DEPOSIT,
                                  &dr.purpose,
                                  &coin_sig->eddsa_signature,
                                  &coin_pub->eddsa_pub))
  {
    report_row_inconsistency ("deposit",
                              rowid,
                              "invalid signature for coin deposit");
    return GNUNET_OK;
  }

  /* update old coin's denomination balance */
  ds = get_denomination_summary (cc,
                                 &dki->properties.denom_hash);
  if (GNUNET_OK !=
      TALER_amount_subtract (&ds->denom_balance,
                             &ds->denom_balance,
                             amount_with_fee))
  {
    report_emergency (dki);
    return GNUNET_SYSERR;
  }

  /* update global up melt fees */
  {
    struct TALER_Amount dfee;

    TALER_amount_ntoh (&dfee,
                       &dki->properties.fee_deposit);
    if (GNUNET_OK !=
        TALER_amount_add (&cc->deposit_fee_balance,
                          &cc->deposit_fee_balance,
                          &dfee))
    {
      GNUNET_break (0);
      return GNUNET_SYSERR;
    }
  }

  return GNUNET_OK;
}


/**
 * Function called with details about coins that were refunding,
 * with the goal of auditing the refund's execution.  Adds the
 * refunded amount back to the outstanding balance of the respective
 * denomination.
 *
 * As a side-effect, #get_coin_summary will report
 * inconsistencies in the refunded coin's balance.
 *
 * @param cls closure
 * @param rowid unique serial ID for the refund in our DB
 * @param coin_pub public key of the coin
 * @param merchant_pub public key of the merchant
 * @param merchant_sig signature of the merchant
 * @param h_proposal_data hash of the proposal data known to merchant and customer
 * @param rtransaction_id refund transaction ID chosen by the merchant
 * @param amount_with_fee amount that was deposited including fee
 * @return #GNUNET_OK to continue to iterate, #GNUNET_SYSERR to stop
 */
static int
refund_cb (void *cls,
           uint64_t rowid,
           const struct TALER_CoinSpendPublicKeyP *coin_pub,
           const struct TALER_MerchantPublicKeyP *merchant_pub,
           const struct TALER_MerchantSignatureP *merchant_sig,
           const struct GNUNET_HashCode *h_proposal_data,
           uint64_t rtransaction_id,
           const struct TALER_Amount *amount_with_fee)
{
  struct CoinContext *cc = cls;
  struct CoinSummary *cs;
  const struct TALER_EXCHANGEDB_DenominationKeyInformationP *dki;
  struct DenominationSummary *ds;
  struct TALER_RefundRequestPS rr;
  struct TALER_Amount amount_without_fee;
  struct TALER_Amount refund_fee;

  cs = get_coin_summary (cc,
                         coin_pub);
  if (NULL == cs)
  {
    GNUNET_break (0);
    return GNUNET_SYSERR;
  }
  dki = cs->dki;

  /* verify refund signature */
  rr.purpose.purpose = htonl (TALER_SIGNATURE_MERCHANT_REFUND);
  rr.purpose.size = htonl (sizeof (rr));
  rr.h_proposal_data = *h_proposal_data;
  rr.coin_pub = *coin_pub;
  rr.merchant = *merchant_pub;
  rr.rtransaction_id = GNUNET_htonll (rtransaction_id);
  TALER_amount_hton (&rr.refund_amount,
                     amount_with_fee);
  rr.refund_fee = dki->properties.fee_refund;
  if (GNUNET_OK !=
      GNUNET_CRYPTO_eddsa_verify (TALER_SIGNATURE_MERCHANT_REFUND,
                                  &rr.purpose,
                                  &merchant_sig->eddsa_sig,
                                  &merchant_pub->eddsa_pub))
  {
    report_row_inconsistency ("refund",
                              rowid,
                              "invalid signature for refund");
    return GNUNET_OK;
  }

  TALER_amount_ntoh (&refund_fee,
                     &dki->properties.fee_refund);
  if (GNUNET_OK !=
      TALER_amount_subtract (&amount_without_fee,
                             amount_with_fee,
                             &refund_fee))
  {
    report_row_inconsistency ("refund",
                              rowid,
                              "refunded amount smaller than refund fee");
    return GNUNET_OK;
  }

  /* update coin's denomination balance */
  ds = get_denomination_summary (cc,
                                 &dki->properties.denom_hash);
  if (GNUNET_OK !=
      TALER_amount_add (&ds->denom_balance,
                        &ds->denom_balance,
                        &amount_without_fee))
  {
    GNUNET_break (0);
    return GNUNET_SYSERR;
  }

  /* update total refund fee balance */
  if (GNUNET_OK !=
      TALER_amount_add (&cc->refund_fee_balance,
                        &cc->refund_fee_balance,
                        &refund_fee))
  {
    GNUNET_break (0);
    return GNUNET_SYSERR;
  }

  return GNUNET_OK;
}


/**
 * Analyze the exchange's processing of coins.
 *
 * @param cls closure
 * @param int #GNUNET_OK on success, #GNUNET_SYSERR on hard errors
 */
static int
analyze_coins (void *cls)
{
  struct CoinContext cc;
  int dret;
  int rret;

  /* setup 'cc' */
  // FIXME: FIX misnomer "denomination_summary", as this is no longer exactly about denominations!
  dret = adb->get_denomination_summary (adb->cls,
                                        asession,
                                        &master_pub,
                                        &cc.denom_balance,
                                        &cc.deposit_fee_balance,
                                        &cc.melt_fee_balance,
                                        &cc.refund_fee_balance);
  if (GNUNET_SYSERR == dret)
  {
    GNUNET_break (0);
    return GNUNET_SYSERR;
  }
  if (GNUNET_NO == dret)
  {
    GNUNET_assert (GNUNET_OK ==
                   TALER_amount_get_zero (currency,
                                          &cc.denom_balance));
    GNUNET_assert (GNUNET_OK ==
                   TALER_amount_get_zero (currency,
                                          &cc.deposit_fee_balance));
    GNUNET_assert (GNUNET_OK ==
                   TALER_amount_get_zero (currency,
                                          &cc.melt_fee_balance));
    GNUNET_assert (GNUNET_OK ==
                   TALER_amount_get_zero (currency,
                                          &cc.refund_fee_balance));
  }
  rret = adb->get_risk_summary (adb->cls,
                                asession,
                                &master_pub,
                                &cc.risk);
  if (GNUNET_SYSERR == dret)
  {
    GNUNET_break (0);
    return GNUNET_SYSERR;
  }
  if (GNUNET_NO == dret)
  {
    /* FIXME: initialize cc->risk by other means... */
  }

  cc.coins = GNUNET_CONTAINER_multihashmap_create (1024,
                                                   GNUNET_NO);
  cc.denominations = GNUNET_CONTAINER_multihashmap_create (256,
                                                           GNUNET_NO);

  /* process withdrawals */
  if (GNUNET_OK !=
      edb->select_reserves_out_above_serial_id (edb->cls,
                                                esession,
                                                42LL, // FIXME
                                                &withdraw_cb,
                                                &cc))
  {
    // FIXME...
  }

  /* process refreshs */
  if (GNUNET_OK !=
      edb->select_refreshs_above_serial_id (edb->cls,
                                            esession,
                                            42LL, // FIXME
                                            &refresh_session_cb,
                                            &cc))
  {
    // FIXME...
  }

  /* process deposits */
  if (GNUNET_OK !=
      edb->select_deposits_above_serial_id (edb->cls,
                                            esession,
                                            42LL, // FIXME
                                            &deposit_cb,
                                            &cc))
  {
    // FIXME...
  }

  /* process refunds */
  if (GNUNET_OK !=
      edb->select_refunds_above_serial_id (edb->cls,
                                           esession,
                                           42LL, // FIXME
                                           &refund_cb,
                                           &cc))
  {
    // FIXME...
  }

  // FIXME...

  /* FIXME: check invariants */

  /* sync 'cc' back to disk */
  GNUNET_CONTAINER_multihashmap_iterate (cc.denominations,
                                         &sync_denomination,
                                         &cc);
  GNUNET_CONTAINER_multihashmap_destroy (cc.denominations);
  GNUNET_CONTAINER_multihashmap_iterate (cc.coins,
                                         &free_coin,
                                         &cc);
  GNUNET_CONTAINER_multihashmap_destroy (cc.coins);

  if (GNUNET_YES == rret)
    rret = adb->update_risk_summary (adb->cls,
                                     asession,
                                     &master_pub,
                                     &cc.risk);
  else
    rret = adb->insert_risk_summary (adb->cls,
                                     asession,
                                     &master_pub,
                                     &cc.risk);
  // FIXME: handle error in 'rret'!

  // FIXME: FIX misnomer "denomination_summary", as this is no longer about denominations!
  if (GNUNET_YES == dret)
      dret = adb->update_denomination_summary (adb->cls,
                                               asession,
                                               &master_pub,
                                               &cc.denom_balance,
                                               &cc.deposit_fee_balance,
                                               &cc.melt_fee_balance,
                                               &cc.refund_fee_balance);
  else
    dret = adb->insert_denomination_summary (adb->cls,
                                             asession,
                                             &master_pub,
                                             &cc.denom_balance,
                                             &cc.deposit_fee_balance,
                                             &cc.melt_fee_balance,
                                             &cc.refund_fee_balance);
  // FIXME: handle error in 'dret'!
  return GNUNET_OK;
}


/* ************************* Analyze merchants ******************** */
/* This logic checks that the aggregator did the right thing
   paying each merchant what they were due (and on time). */


/**
 * Summary data we keep per merchant.
 */
struct MerchantSummary
{

  /**
   * Which account were we supposed to pay?
   */
  struct GNUNET_HashCode h_wire;

  /**
   * Total due to be paid to @e h_wire.
   */
  struct TALER_Amount total_due;

  /**
   * Total paid to @e h_wire.
   */
  struct TALER_Amount total_paid;

  /**
   * Total wire fees charged.
   */
  struct TALER_Amount total_fees;

  /**
   * Last (expired) refund deadline of all the transactions totaled
   * up in @e due.
   */
  struct GNUNET_TIME_Absolute last_refund_deadline;

};


/**
 * Closure for callbacks during #analyze_merchants().
 */
struct MerchantContext
{

  /**
   * Map for tracking information about merchants.
   */
  struct GNUNET_CONTAINER_MultiHashMap *merchants;

};


/**
 * Analyze the exchange aggregator's payment processing.
 *
 * @param cls closure
 * @param int #GNUNET_OK on success, #GNUNET_SYSERR on hard errors
 */
static int
analyze_merchants (void *cls)
{
  struct MerchantContext mc;

  mc.merchants = GNUNET_CONTAINER_multihashmap_create (1024,
                                                       GNUNET_YES);

  // TODO

  GNUNET_CONTAINER_multihashmap_destroy (mc.merchants);
  return GNUNET_OK;
}


/* *************************** General transaction logic ****************** */

/**
 * Type of an analysis function.  Each analysis function runs in
 * its own transaction scope and must thus be internally consistent.
 *
 * @param cls closure
 * @param int #GNUNET_OK on success, #GNUNET_SYSERR on hard errors
 */
typedef int
(*Analysis)(void *cls);


/**
 * Perform the given @a analysis incrementally, checkpointing our
 * progress in the auditor DB.
 *
 * @param analysis analysis to run
 * @param analysis_cls closure for @a analysis
 * @return #GNUNET_OK if @a analysis succeessfully committed,
 *         #GNUNET_SYSERR on hard errors
 */
static int
incremental_processing (Analysis analysis,
                        void *analysis_cls)
{
  int ret;

  ret = adb->get_auditor_progress (adb->cls,
                                   asession,
                                   &master_pub,
                                   &pp);
  if (GNUNET_SYSERR == ret)
    {
      GNUNET_break (0);
      return GNUNET_SYSERR;
    }
  if (GNUNET_NO == ret)
    {
      GNUNET_log (GNUNET_ERROR_TYPE_MESSAGE,
                  _("First analysis using this auditor, starting audit from scratch\n"));
    }
  else
    {
      GNUNET_log (GNUNET_ERROR_TYPE_MESSAGE,
                  _("Resuming audit at %llu/%llu/%llu/%llu/%llu/%llu\n\n"),
                  (unsigned long long) pp.last_reserve_in_serial_id,
                  (unsigned long long) pp.last_reserve_out_serial_id,
                  (unsigned long long) pp.last_deposit_serial_id,
                  (unsigned long long) pp.last_melt_serial_id,
                  (unsigned long long) pp.last_refund_serial_id,
                  (unsigned long long) pp.last_prewire_serial_id);
    }
  ret = analysis (analysis_cls);
  if (GNUNET_OK != ret)
    {
      GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                  "Analysis phase failed, not recording progress\n");
      return GNUNET_SYSERR;
    }
  ret = adb->update_auditor_progress (adb->cls,
                                      asession,
                                      &master_pub,
                                      &pp);
  if (GNUNET_OK != ret)
    {
      GNUNET_break (0);
      return GNUNET_SYSERR;
    }
  GNUNET_log (GNUNET_ERROR_TYPE_MESSAGE,
              _("Resuming audit at %llu/%llu/%llu/%llu/%llu/%llu\n\n"),
              (unsigned long long) pp.last_reserve_in_serial_id,
              (unsigned long long) pp.last_reserve_out_serial_id,
              (unsigned long long) pp.last_deposit_serial_id,
              (unsigned long long) pp.last_melt_serial_id,
              (unsigned long long) pp.last_refund_serial_id,
              (unsigned long long) pp.last_prewire_serial_id);
  return GNUNET_OK;
}


/**
 * Perform the given @a analysis within a transaction scope.
 * Commit on success.
 *
 * @param analysis analysis to run
 * @param analysis_cls closure for @a analysis
 * @return #GNUNET_OK if @a analysis succeessfully committed,
 *         #GNUNET_NO if we had an error on commit (retry may help)
 *         #GNUNET_SYSERR on hard errors
 */
static int
transact (Analysis analysis,
          void *analysis_cls)
{
  int ret;

  ret = adb->start (adb->cls,
                    asession);
  if (GNUNET_OK != ret)
    {
      GNUNET_break (0);
      return GNUNET_SYSERR;
    }
  ret = edb->start (edb->cls,
                    esession);
  if (GNUNET_OK != ret)
    {
      GNUNET_break (0);
      return GNUNET_SYSERR;
    }
  ret = incremental_processing (analysis,
                                analysis_cls);
  if (GNUNET_OK == ret)
    {
      ret = edb->commit (edb->cls,
                         esession);
      if (GNUNET_OK != ret)
        {
          GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                      "Exchange DB commit failed, rolling back transaction\n");
          adb->rollback (adb->cls,
                         asession);
        }
      else
        {
          ret = adb->commit (adb->cls,
                             asession);
          if (GNUNET_OK != ret)
            {
              GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                          "Auditor DB commit failed!\n");
            }
        }
    }
  else
    {
      GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                  "Processing failed, rolling back transaction\n");
      adb->rollback (adb->cls,
                     asession);
      edb->rollback (edb->cls,
                     esession);
    }
  clear_transaction_state_cache ();
  return ret;
}


/**
 * Initialize DB sessions and run the analysis.
 */
static void
setup_sessions_and_run ()
{
  esession = edb->get_session (edb->cls);
  if (NULL == esession)
  {
    fprintf (stderr,
             "Failed to initialize exchange session.\n");
    global_ret = 1;
    return;
  }
  asession = adb->get_session (adb->cls);
  if (NULL == asession)
  {
    fprintf (stderr,
             "Failed to initialize auditor session.\n");
    global_ret = 1;
    return;
  }

  transact (&analyze_reserves,
            NULL);
  transact (&analyze_coins,
            NULL);
  transact (&analyze_merchants,
            NULL);
}


/**
 * Main function that will be run.
 *
 * @param cls closure
 * @param args remaining command-line arguments
 * @param cfgfile name of the configuration file used (for saving, can be NULL!)
 * @param cfg configuration
 */
static void
run (void *cls,
     char *const *args,
     const char *cfgfile,
     const struct GNUNET_CONFIGURATION_Handle *cfg)
{
  if (GNUNET_OK !=
      GNUNET_CONFIGURATION_get_value_string (cfg,
                                             "taler",
                                             "CURRENCY",
                                             &currency))
  {
    GNUNET_log_config_missing (GNUNET_ERROR_TYPE_ERROR,
                               "taler",
                               "CURRENCY");
    global_ret = 1;
    return;
  }
  if (NULL ==
      (edb = TALER_EXCHANGEDB_plugin_load (cfg)))
  {
    fprintf (stderr,
             "Failed to initialize exchange database plugin.\n");
    global_ret = 1;
    return;
  }
  if (NULL ==
      (adb = TALER_AUDITORDB_plugin_load (cfg)))
  {
    fprintf (stderr,
             "Failed to initialize auditor database plugin.\n");
    global_ret = 1;
    TALER_EXCHANGEDB_plugin_unload (edb);
    return;
  }
  setup_sessions_and_run ();
  TALER_AUDITORDB_plugin_unload (adb);
  TALER_EXCHANGEDB_plugin_unload (edb);
}


/**
 * The main function of the database initialization tool.
 * Used to initialize the Taler Exchange's database.
 *
 * @param argc number of arguments from the command line
 * @param argv command line arguments
 * @return 0 ok, 1 on error
 */
int
main (int argc,
      char *const *argv)
{
  const struct GNUNET_GETOPT_CommandLineOption options[] = {
    GNUNET_GETOPT_OPTION_MANDATORY
    (GNUNET_GETOPT_OPTION_SET_BASE32_AUTO ('m',
                                           "exchange-key",
                                           "KEY",
                                           "public key of the exchange (Crockford base32 encoded)",
                                           &master_pub)),
    GNUNET_GETOPT_OPTION_END
  };

  /* force linker to link against libtalerutil; if we do
     not do this, the linker may "optimize" libtalerutil
     away and skip #TALER_OS_init(), which we do need */
  (void) TALER_project_data_default ();
  GNUNET_assert (GNUNET_OK ==
                 GNUNET_log_setup ("taler-auditor",
                                   "INFO",
                                   NULL));
  if (GNUNET_OK !=
      GNUNET_PROGRAM_run (argc,
			  argv,
                          "taler-auditor",
			  "Audit Taler exchange database",
			  options,
			  &run,
			  NULL))
    return 1;
  return global_ret;
}


/* end of taler-auditor.c */
