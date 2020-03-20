/*
  This file is part of TALER
  Copyright (C) 2016-2020 Taler Systems SA

  TALER is free software; you can redistribute it and/or modify it under the
  terms of the GNU Affero Public License as published by the Free Software
  Foundation; either version 3, or (at your option) any later version.

  TALER is distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
  A PARTICULAR PURPOSE.  See the GNU Affero Public License for more details.

  You should have received a copy of the GNU Affero Public License along with
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
 *   given in the 'wire_out' table. This needs to be checked separately!
 *
 * TODO:
 * - reorganize: different passes are combined in one tool and one
 *   file here, we should split this up!
 * - likely should do an iteration over known_coins instead of checking
 *   those signatures again and again
 * - might want to bite the bullet and do asynchronous signature
 *   verification to improve parallelism / speed -- we'll need to scale
 *   this eventually anyway!
 *
 * UNDECIDED:
 * - do we care about checking the 'done' flag in deposit_cb?
 */
#include "platform.h"
#include <gnunet/gnunet_util_lib.h>
#include "taler_auditordb_plugin.h"
#include "taler_exchangedb_lib.h"
#include "taler_json_lib.h"
#include "taler_bank_service.h"
#include "taler_signatures.h"
#include "report-lib.h"

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
 * Use a 1 day grace period to deal with clocks not being perfectly synchronized.
 */
#define DEPOSIT_GRACE_PERIOD GNUNET_TIME_UNIT_DAYS

/**
 * Return value from main().
 */
static int global_ret;

/**
 * Checkpointing our progress for coins.
 */
static struct TALER_AUDITORDB_ProgressPointCoin ppc;

/**
 * Checkpointing our progress for coins.
 */
static struct TALER_AUDITORDB_ProgressPointCoin ppc_start;

/**
 * Array of reports about denomination keys with an
 * emergency (more value deposited than withdrawn)
 */
static json_t *report_emergencies;

/**
 * Array of reports about denomination keys with an
 * emergency (more coins deposited than withdrawn)
 */
static json_t *report_emergencies_by_count;

/**
 * Array of reports about row inconsitencies.
 */
static json_t *report_row_inconsistencies;

/**
 * Report about amount calculation differences (causing profit
 * or loss at the exchange).
 */
static json_t *report_amount_arithmetic_inconsistencies;

/**
 * Profits the exchange made by bad amount calculations.
 */
static struct TALER_Amount total_arithmetic_delta_plus;

/**
 * Losses the exchange made by bad amount calculations.
 */
static struct TALER_Amount total_arithmetic_delta_minus;

/**
 * Total amount reported in all calls to #report_emergency_by_count().
 */
static struct TALER_Amount reported_emergency_risk_by_count;

/**
 * Total amount reported in all calls to #report_emergency_by_amount().
 */
static struct TALER_Amount reported_emergency_risk_by_amount;

/**
 * Total amount in losses reported in all calls to #report_emergency_by_amount().
 */
static struct TALER_Amount reported_emergency_loss;

/**
 * Total amount in losses reported in all calls to #report_emergency_by_count().
 */
static struct TALER_Amount reported_emergency_loss_by_count;

/**
 * Expected balance in the escrow account.
 */
static struct TALER_Amount total_escrow_balance;

/**
 * Active risk exposure.
 */
static struct TALER_Amount total_risk;

/**
 * Actualized risk (= loss) from recoups.
 */
static struct TALER_Amount total_recoup_loss;

/**
 * Recoups we made on denominations that were not revoked (!?).
 */
static struct TALER_Amount total_irregular_recoups;

/**
 * Total deposit fees earned.
 */
static struct TALER_Amount total_deposit_fee_income;

/**
 * Total melt fees earned.
 */
static struct TALER_Amount total_melt_fee_income;

/**
 * Total refund fees earned.
 */
static struct TALER_Amount total_refund_fee_income;

/**
 * Array of reports about coin operations with bad signatures.
 */
static json_t *report_bad_sig_losses;

/**
 * Total amount lost by operations for which signatures were invalid.
 */
static struct TALER_Amount total_bad_sig_loss;

/**
 * Array of refresh transactions where the /refresh/reveal has not yet
 * happened (and may of course never happen).
 */
static json_t *report_refreshs_hanging;

/**
 * Total amount lost by operations for which signatures were invalid.
 */
static struct TALER_Amount total_refresh_hanging;


/* ***************************** Report logic **************************** */

/**
 * Called in case we detect an emergency situation where the exchange
 * is paying out a larger amount on a denomination than we issued in
 * that denomination.  This means that the exchange's private keys
 * might have gotten compromised, and that we need to trigger an
 * emergency request to all wallets to deposit pending coins for the
 * denomination (and as an exchange suffer a huge financial loss).
 *
 * @param issue denomination key where the loss was detected
 * @param risk maximum risk that might have just become real (coins created by this @a issue)
 * @param loss actual losses already (actualized before denomination was revoked)
 */
static void
report_emergency_by_amount (const struct TALER_DenominationKeyValidityPS *issue,
                            const struct TALER_Amount *risk,
                            const struct TALER_Amount *loss)
{
  report (report_emergencies,
          json_pack ("{s:o, s:o, s:o, s:o, s:o, s:o}",
                     "denompub_hash",
                     GNUNET_JSON_from_data_auto (&issue->denom_hash),
                     "denom_risk",
                     TALER_JSON_from_amount (risk),
                     "denom_loss",
                     TALER_JSON_from_amount (loss),
                     "start",
                     json_from_time_abs_nbo (issue->start),
                     "deposit_end",
                     json_from_time_abs_nbo (issue->expire_deposit),
                     "value",
                     TALER_JSON_from_amount_nbo (&issue->value)));
  GNUNET_assert (GNUNET_OK ==
                 TALER_amount_add (&reported_emergency_risk_by_amount,
                                   &reported_emergency_risk_by_amount,
                                   risk));
  GNUNET_assert (GNUNET_OK ==
                 TALER_amount_add (&reported_emergency_loss,
                                   &reported_emergency_loss,
                                   loss));
}


/**
 * Called in case we detect an emergency situation where the exchange
 * is paying out a larger NUMBER of coins of a denomination than we
 * issued in that denomination.  This means that the exchange's
 * private keys might have gotten compromised, and that we need to
 * trigger an emergency request to all wallets to deposit pending
 * coins for the denomination (and as an exchange suffer a huge
 * financial loss).
 *
 * @param issue denomination key where the loss was detected
 * @param num_issued number of coins that were issued
 * @param num_known number of coins that have been deposited
 * @param risk amount that is at risk
 */
static void
report_emergency_by_count (const struct TALER_DenominationKeyValidityPS *issue,
                           uint64_t num_issued,
                           uint64_t num_known,
                           const struct TALER_Amount *risk)
{
  struct TALER_Amount denom_value;

  report (report_emergencies_by_count,
          json_pack ("{s:o, s:I, s:I, s:o, s:o, s:o, s:o}",
                     "denompub_hash",
                     GNUNET_JSON_from_data_auto (&issue->denom_hash),
                     "num_issued",
                     (json_int_t) num_issued,
                     "num_known",
                     (json_int_t) num_known,
                     "denom_risk",
                     TALER_JSON_from_amount (risk),
                     "start",
                     json_from_time_abs_nbo (issue->start),
                     "deposit_end",
                     json_from_time_abs_nbo (issue->expire_deposit),
                     "value",
                     TALER_JSON_from_amount_nbo (&issue->value)));
  GNUNET_assert (GNUNET_OK ==
                 TALER_amount_add (&reported_emergency_risk_by_count,
                                   &reported_emergency_risk_by_count,
                                   risk));
  TALER_amount_ntoh (&denom_value,
                     &issue->value);
  for (uint64_t i = num_issued; i<num_known; i++)
    GNUNET_assert (GNUNET_OK ==
                   TALER_amount_add (&reported_emergency_loss_by_count,
                                     &reported_emergency_loss_by_count,
                                     &denom_value));

}


/**
 * Report a (serious) inconsistency in the exchange's database with
 * respect to calculations involving amounts.
 *
 * @param operation what operation had the inconsistency
 * @param rowid affected row, UINT64_MAX if row is missing
 * @param exchange amount calculated by exchange
 * @param auditor amount calculated by auditor
 * @param profitable 1 if @a exchange being larger than @a auditor is
 *           profitable for the exchange for this operation,
 *           -1 if @a exchange being smaller than @a auditor is
 *           profitable for the exchange, and 0 if it is unclear
 */
static void
report_amount_arithmetic_inconsistency (const char *operation,
                                        uint64_t rowid,
                                        const struct TALER_Amount *exchange,
                                        const struct TALER_Amount *auditor,
                                        int profitable)
{
  struct TALER_Amount delta;
  struct TALER_Amount *target;

  if (0 < TALER_amount_cmp (exchange,
                            auditor))
  {
    /* exchange > auditor */
    GNUNET_break (GNUNET_OK ==
                  TALER_amount_subtract (&delta,
                                         exchange,
                                         auditor));
  }
  else
  {
    /* auditor < exchange */
    profitable = -profitable;
    GNUNET_break (GNUNET_OK ==
                  TALER_amount_subtract (&delta,
                                         auditor,
                                         exchange));
  }
  report (report_amount_arithmetic_inconsistencies,
          json_pack ("{s:s, s:I, s:o, s:o, s:I}",
                     "operation", operation,
                     "rowid", (json_int_t) rowid,
                     "exchange", TALER_JSON_from_amount (exchange),
                     "auditor", TALER_JSON_from_amount (auditor),
                     "profitable", (json_int_t) profitable));
  if (0 != profitable)
  {
    target = (1 == profitable)
             ? &total_arithmetic_delta_plus
             : &total_arithmetic_delta_minus;
    GNUNET_break (GNUNET_OK ==
                  TALER_amount_add (target,
                                    target,
                                    &delta));
  }
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
  report (report_row_inconsistencies,
          json_pack ("{s:s, s:I, s:s}",
                     "table", table,
                     "row", (json_int_t) rowid,
                     "diagnostic", diagnostic));
}


/* ************************* Analyze coins ******************** */
/* This logic checks that the exchange did the right thing for each
   coin, checking deposits, refunds, refresh* and known_coins
   tables */


/**
 * Summary data we keep per denomination.
 */
struct DenominationSummary
{
  /**
   * Total value of outstanding (not deposited) coins issued with this
   * denomination key.
   */
  struct TALER_Amount denom_balance;

  /**
   * Total losses made (once coins deposited exceed
   * coins withdrawn and thus the @e denom_balance is
   * effectively negative).
   */
  struct TALER_Amount denom_loss;

  /**
   * Total value of coins issued with this denomination key.
   */
  struct TALER_Amount denom_risk;

  /**
   * Total value of coins subjected to recoup with this denomination key.
   */
  struct TALER_Amount denom_recoup;

  /**
   * How many coins (not their amount!) of this denomination
   * did the exchange issue overall?
   */
  uint64_t num_issued;

  /**
   * Denomination key information for this denomination.
   */
  const struct TALER_DenominationKeyValidityPS *issue;

  /**
   * #GNUNET_YES if this record already existed in the DB.
   * Used to decide between insert/update in
   * #sync_denomination().
   */
  int in_db;

  /**
   * Should we report an emergency for this denomination?
   */
  int report_emergency;

  /**
   * #GNUNET_YES if this denomination was revoked.
   */
  int was_revoked;
};


/**
 * Closure for callbacks during #analyze_coins().
 */
struct CoinContext
{

  /**
   * Map for tracking information about denominations.
   */
  struct GNUNET_CONTAINER_MultiHashMap *denom_summaries;

  /**
   * Current write/replace offset in the circular @e summaries buffer.
   */
  unsigned int summaries_off;

  /**
   * Transaction status code.
   */
  enum GNUNET_DB_QueryStatus qs;

};


/**
 * Initialize information about denomination from the database.
 *
 * @param denom_hash hash of the public key of the denomination
 * @param[out] ds summary to initialize
 * @return transaction status code
 */
static enum GNUNET_DB_QueryStatus
init_denomination (const struct GNUNET_HashCode *denom_hash,
                   struct DenominationSummary *ds)
{
  enum GNUNET_DB_QueryStatus qs;
  struct TALER_MasterSignatureP msig;
  uint64_t rowid;

  qs = adb->get_denomination_balance (adb->cls,
                                      asession,
                                      denom_hash,
                                      &ds->denom_balance,
                                      &ds->denom_loss,
                                      &ds->denom_risk,
                                      &ds->denom_recoup,
                                      &ds->num_issued);
  if (0 > qs)
  {
    GNUNET_break (GNUNET_DB_STATUS_SOFT_ERROR == qs);
    return qs;
  }
  if (GNUNET_DB_STATUS_SUCCESS_ONE_RESULT == qs)
  {
    ds->in_db = GNUNET_YES;
    GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                "Starting balance for denomination `%s' is %s\n",
                GNUNET_h2s (denom_hash),
                TALER_amount2s (&ds->denom_balance));
    return GNUNET_DB_STATUS_SUCCESS_ONE_RESULT;
  }
  qs = edb->get_denomination_revocation (edb->cls,
                                         esession,
                                         denom_hash,
                                         &msig,
                                         &rowid);
  if (0 > qs)
  {
    GNUNET_break (GNUNET_DB_STATUS_SOFT_ERROR == qs);
    return qs;
  }
  if (0 < qs)
  {
    /* check revocation signature */
    struct TALER_MasterDenominationKeyRevocationPS rm;

    rm.purpose.purpose = htonl (
      TALER_SIGNATURE_MASTER_DENOMINATION_KEY_REVOKED);
    rm.purpose.size = htonl (sizeof (rm));
    rm.h_denom_pub = *denom_hash;
    if (GNUNET_OK !=
        GNUNET_CRYPTO_eddsa_verify (
          TALER_SIGNATURE_MASTER_DENOMINATION_KEY_REVOKED,
          &rm.purpose,
          &msig.eddsa_signature,
          &master_pub.eddsa_pub))
    {
      report_row_inconsistency ("denomination revocation table",
                                rowid,
                                "revocation signature invalid");
    }
    else
    {
      ds->was_revoked = GNUNET_YES;
    }
  }
  GNUNET_assert (GNUNET_OK ==
                 TALER_amount_get_zero (currency,
                                        &ds->denom_balance));
  GNUNET_assert (GNUNET_OK ==
                 TALER_amount_get_zero (currency,
                                        &ds->denom_loss));
  GNUNET_assert (GNUNET_OK ==
                 TALER_amount_get_zero (currency,
                                        &ds->denom_risk));
  GNUNET_assert (GNUNET_OK ==
                 TALER_amount_get_zero (currency,
                                        &ds->denom_recoup));
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Starting balance for denomination `%s' is %s\n",
              GNUNET_h2s (denom_hash),
              TALER_amount2s (&ds->denom_balance));
  return GNUNET_DB_STATUS_SUCCESS_ONE_RESULT;
}


/**
 * Obtain the denomination summary for the given @a dh
 *
 * @param cc our execution context
 * @param issue denomination key information for @a dh
 * @param dh the denomination hash to use for the lookup
 * @return NULL on error
 */
static struct DenominationSummary *
get_denomination_summary (struct CoinContext *cc,
                          const struct TALER_DenominationKeyValidityPS *issue,
                          const struct GNUNET_HashCode *dh)
{
  struct DenominationSummary *ds;

  ds = GNUNET_CONTAINER_multihashmap_get (cc->denom_summaries,
                                          dh);
  if (NULL != ds)
    return ds;
  ds = GNUNET_new (struct DenominationSummary);
  ds->issue = issue;
  if (0 > (cc->qs = init_denomination (dh,
                                       ds)))
  {
    GNUNET_break (0);
    GNUNET_free (ds);
    return NULL;
  }
  GNUNET_assert (GNUNET_OK ==
                 GNUNET_CONTAINER_multihashmap_put (cc->denom_summaries,
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
 * @param denom_hash the hash of the denomination key
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
  const struct TALER_DenominationKeyValidityPS *issue = ds->issue;
  struct GNUNET_TIME_Absolute now;
  struct GNUNET_TIME_Absolute expire_deposit;
  struct GNUNET_TIME_Absolute expire_deposit_grace;
  enum GNUNET_DB_QueryStatus qs;

  now = GNUNET_TIME_absolute_get ();
  expire_deposit = GNUNET_TIME_absolute_ntoh (issue->expire_deposit);
  /* add day grace period to deal with clocks not being perfectly synchronized */
  expire_deposit_grace = GNUNET_TIME_absolute_add (expire_deposit,
                                                   DEPOSIT_GRACE_PERIOD);
  if (now.abs_value_us > expire_deposit_grace.abs_value_us)
  {
    /* Denominationkey has expired, book remaining balance of
       outstanding coins as revenue; and reduce cc->risk exposure. */
    if (ds->in_db)
      qs = adb->del_denomination_balance (adb->cls,
                                          asession,
                                          denom_hash);
    else
      qs = GNUNET_DB_STATUS_SUCCESS_ONE_RESULT;
    if ( (GNUNET_DB_STATUS_SUCCESS_ONE_RESULT == qs) &&
         ( (0 != ds->denom_risk.value) ||
           (0 != ds->denom_risk.fraction) ) )
    {
      /* The denomination expired and carried a balance; we can now
         book the remaining balance as profit, and reduce our risk
         exposure by the accumulated risk of the denomination. */
      if (GNUNET_SYSERR ==
          TALER_amount_subtract (&total_risk,
                                 &total_risk,
                                 &ds->denom_risk))
      {
        /* Holy smokes, our risk assessment was inconsistent!
           This is really, really bad. */
        GNUNET_break (0);
        cc->qs = GNUNET_DB_STATUS_HARD_ERROR;
      }
    }
    if ( (GNUNET_DB_STATUS_SUCCESS_ONE_RESULT == qs) &&
         ( (0 != ds->denom_balance.value) ||
           (0 != ds->denom_balance.fraction) ) )
    {
      /* book denom_balance coin expiration profits! */
      GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                  "Denomination `%s' expired, booking %s in expiration profits\n",
                  GNUNET_h2s (denom_hash),
                  TALER_amount2s (&ds->denom_balance));
      if (GNUNET_DB_STATUS_SUCCESS_ONE_RESULT !=
          (qs = adb->insert_historic_denom_revenue (adb->cls,
                                                    asession,
                                                    &master_pub,
                                                    denom_hash,
                                                    expire_deposit,
                                                    &ds->denom_balance,
                                                    &ds->denom_recoup)))
      {
        /* Failed to store profits? Bad database */
        GNUNET_break (GNUNET_DB_STATUS_SOFT_ERROR == qs);
        cc->qs = qs;
      }
    }
  }
  else
  {
    long long cnt;

    GNUNET_log (GNUNET_ERROR_TYPE_INFO,
                "Final balance for denomination `%s' is %s (%llu)\n",
                GNUNET_h2s (denom_hash),
                TALER_amount2s (&ds->denom_balance),
                (unsigned long long) ds->num_issued);
    cnt = edb->count_known_coins (edb->cls,
                                  esession,
                                  denom_hash);
    if (0 > cnt)
    {
      /* Failed to obtain count? Bad database */
      qs = (enum GNUNET_DB_QueryStatus) cnt;
      GNUNET_break (GNUNET_DB_STATUS_SOFT_ERROR == qs);
      cc->qs = qs;
    }
    else
    {
      if (ds->num_issued < (uint64_t) cnt)
      {
        report_emergency_by_count (issue,
                                   ds->num_issued,
                                   cnt,
                                   &ds->denom_risk);
      }
      if (GNUNET_YES == ds->report_emergency)
      {
        report_emergency_by_amount (issue,
                                    &ds->denom_risk,
                                    &ds->denom_loss);

      }
      if (ds->in_db)
        qs = adb->update_denomination_balance (adb->cls,
                                               asession,
                                               denom_hash,
                                               &ds->denom_balance,
                                               &ds->denom_loss,
                                               &ds->denom_risk,
                                               &ds->denom_recoup,
                                               ds->num_issued);
      else
        qs = adb->insert_denomination_balance (adb->cls,
                                               asession,
                                               denom_hash,
                                               &ds->denom_balance,
                                               &ds->denom_loss,
                                               &ds->denom_risk,
                                               &ds->denom_recoup,
                                               ds->num_issued);
    }
  }
  if (GNUNET_DB_STATUS_SUCCESS_ONE_RESULT != qs)
  {
    GNUNET_break (GNUNET_DB_STATUS_SOFT_ERROR == qs);
    cc->qs = qs;
  }
  GNUNET_assert (GNUNET_YES ==
                 GNUNET_CONTAINER_multihashmap_remove (cc->denom_summaries,
                                                       denom_hash,
                                                       ds));
  GNUNET_free (ds);
  if (GNUNET_DB_STATUS_SUCCESS_ONE_RESULT != cc->qs)
    return GNUNET_SYSERR;
  return GNUNET_OK;
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
             const struct TALER_ReservePublicKeyP *reserve_pub,
             const struct TALER_ReserveSignatureP *reserve_sig,
             struct GNUNET_TIME_Absolute execution_date,
             const struct TALER_Amount *amount_with_fee)
{
  struct CoinContext *cc = cls;
  struct DenominationSummary *ds;
  struct GNUNET_HashCode dh;
  const struct TALER_DenominationKeyValidityPS *issue;
  struct TALER_Amount value;
  enum GNUNET_DB_QueryStatus qs;

  (void) h_blind_ev;
  (void) reserve_pub;
  (void) reserve_sig;
  (void) execution_date;
  (void) amount_with_fee;
  GNUNET_assert (rowid >= ppc.last_withdraw_serial_id); /* should be monotonically increasing */
  ppc.last_withdraw_serial_id = rowid + 1;

  qs = get_denomination_info (denom_pub,
                              &issue,
                              &dh);
  if (GNUNET_DB_STATUS_SUCCESS_NO_RESULTS == qs)
  {
    report_row_inconsistency ("withdraw",
                              rowid,
                              "denomination key not found");
    return GNUNET_OK;
  }
  if (GNUNET_DB_STATUS_SUCCESS_ONE_RESULT != qs)
  {
    /* This really ought to be a transient DB error. */
    GNUNET_break (GNUNET_DB_STATUS_SOFT_ERROR == qs);
    cc->qs = qs;
    return GNUNET_SYSERR;
  }
  ds = get_denomination_summary (cc,
                                 issue,
                                 &dh);
  if (NULL == ds)
  {
    GNUNET_break (0);
    return GNUNET_SYSERR;
  }
  TALER_amount_ntoh (&value,
                     &issue->value);
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Issued coin in denomination `%s' of total value %s\n",
              GNUNET_h2s (&dh),
              TALER_amount2s (&value));
  ds->num_issued++;
  if (GNUNET_OK !=
      TALER_amount_add (&ds->denom_balance,
                        &ds->denom_balance,
                        &value))
  {
    GNUNET_break (0);
    cc->qs = GNUNET_DB_STATUS_HARD_ERROR;
    return GNUNET_SYSERR;
  }
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "New balance of denomination `%s' is %s\n",
              GNUNET_h2s (&dh),
              TALER_amount2s (&ds->denom_balance));
  if (GNUNET_OK !=
      TALER_amount_add (&total_escrow_balance,
                        &total_escrow_balance,
                        &value))
  {
    GNUNET_break (0);
    cc->qs = GNUNET_DB_STATUS_HARD_ERROR;
    return GNUNET_SYSERR;
  }
  if (GNUNET_OK !=
      TALER_amount_add (&total_risk,
                        &total_risk,
                        &value))
  {
    GNUNET_break (0);
    cc->qs = GNUNET_DB_STATUS_HARD_ERROR;
    return GNUNET_SYSERR;
  }
  if (GNUNET_OK !=
      TALER_amount_add (&ds->denom_risk,
                        &ds->denom_risk,
                        &value))
  {
    GNUNET_break (0);
    cc->qs = GNUNET_DB_STATUS_HARD_ERROR;
    return GNUNET_SYSERR;
  }
  return GNUNET_OK;
}


/**
 * Closure for #reveal_data_cb().
 */
struct RevealContext
{

  /**
   * Denomination public keys of the new coins.
   */
  struct TALER_DenominationPublicKey *new_dps;

  /**
   * Size of the @a new_dp and @a new_dps arrays.
   */
  unsigned int num_freshcoins;
};


/**
 * Function called with information about a refresh order.
 *
 * @param cls closure
 * @param num_freshcoins size of the @a rrcs array
 * @param rrcs array of @a num_freshcoins information about coins to be created
 * @param num_tprivs number of entries in @a tprivs, should be #TALER_CNC_KAPPA - 1
 * @param tprivs array of @e num_tprivs transfer private keys
 * @param tp transfer public key information
 */
static void
reveal_data_cb (void *cls,
                uint32_t num_freshcoins,
                const struct TALER_EXCHANGEDB_RefreshRevealedCoin *rrcs,
                unsigned int num_tprivs,
                const struct TALER_TransferPrivateKeyP *tprivs,
                const struct TALER_TransferPublicKeyP *tp)
{
  struct RevealContext *rctx = cls;

  (void) num_tprivs;
  (void) tprivs;
  (void) tp;
  rctx->num_freshcoins = num_freshcoins;
  rctx->new_dps = GNUNET_new_array (num_freshcoins,
                                    struct TALER_DenominationPublicKey);
  for (unsigned int i = 0; i<num_freshcoins; i++)
    rctx->new_dps[i].rsa_public_key
      = GNUNET_CRYPTO_rsa_public_key_dup (rrcs[i].denom_pub.rsa_public_key);
}


/**
 * Check that the @a coin_pub is a known coin with a proper
 * signature for denominatinon @a denom_pub. If not, report
 * a loss of @a loss_potential.
 *
 * @param coin_pub public key of a coin
 * @param denom_pub expected denomination of the coin
 * @param loss_potential how big could the loss be if the coin is
 *        not properly signed
 * @return database transaction status, on success
 *  #GNUNET_DB_STATUS_SUCCESS_ONE_RESULT
 */
static enum GNUNET_DB_QueryStatus
check_known_coin (const struct TALER_CoinSpendPublicKeyP *coin_pub,
                  const struct TALER_DenominationPublicKey *denom_pub,
                  const struct TALER_Amount *loss_potential)
{
  struct TALER_CoinPublicInfo ci;
  enum GNUNET_DB_QueryStatus qs;

  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Checking denomination signature on %s\n",
              TALER_B2S (coin_pub));
  qs = edb->get_known_coin (edb->cls,
                            esession,
                            coin_pub,
                            &ci);
  if (GNUNET_DB_STATUS_SUCCESS_ONE_RESULT != qs)
  {
    GNUNET_break (GNUNET_DB_STATUS_SOFT_ERROR == qs);
    return qs;
  }
  if (GNUNET_YES !=
      TALER_test_coin_valid (&ci,
                             denom_pub))
  {
    report (report_bad_sig_losses,
            json_pack ("{s:s, s:I, s:o, s:o}",
                       "operation", "known-coin",
                       "row", (json_int_t) -1,
                       "loss", TALER_JSON_from_amount (loss_potential),
                       "key_pub", GNUNET_JSON_from_data_auto (coin_pub)));
    GNUNET_break (GNUNET_OK ==
                  TALER_amount_add (&total_bad_sig_loss,
                                    &total_bad_sig_loss,
                                    loss_potential));

  }
  GNUNET_CRYPTO_rsa_signature_free (ci.denom_sig.rsa_signature);
  return qs;
}


/**
 * Function called with details about coins that were melted, with the
 * goal of auditing the refresh's execution.  Verifies the signature
 * and updates our information about coins outstanding (the old coin's
 * denomination has less, the fresh coins increased outstanding
 * balances).
 *
 * @param cls closure
 * @param rowid unique serial ID for the refresh session in our DB
 * @param denom_pub denomination public key of @a coin_pub
 * @param coin_pub public key of the coin
 * @param coin_sig signature from the coin
 * @param amount_with_fee amount that was deposited including fee
 * @param noreveal_index which index was picked by the exchange in cut-and-choose
 * @param rc what is the refresh commitment
 * @return #GNUNET_OK to continue to iterate, #GNUNET_SYSERR to stop
 */
static int
refresh_session_cb (void *cls,
                    uint64_t rowid,
                    const struct TALER_DenominationPublicKey *denom_pub,
                    const struct TALER_CoinSpendPublicKeyP *coin_pub,
                    const struct TALER_CoinSpendSignatureP *coin_sig,
                    const struct TALER_Amount *amount_with_fee,
                    uint32_t noreveal_index,
                    const struct TALER_RefreshCommitmentP *rc)
{
  struct CoinContext *cc = cls;
  struct TALER_RefreshMeltCoinAffirmationPS rmc;
  const struct TALER_DenominationKeyValidityPS *issue;
  struct DenominationSummary *dso;
  struct TALER_Amount amount_without_fee;
  struct TALER_Amount tmp;
  enum GNUNET_DB_QueryStatus qs;

  (void) noreveal_index;
  GNUNET_assert (rowid >= ppc.last_melt_serial_id); /* should be monotonically increasing */
  ppc.last_melt_serial_id = rowid + 1;

  qs = get_denomination_info (denom_pub,
                              &issue,
                              NULL);
  if (GNUNET_DB_STATUS_SUCCESS_NO_RESULTS == qs)
  {
    report_row_inconsistency ("melt",
                              rowid,
                              "denomination key not found");
    return GNUNET_OK;
  }
  if (GNUNET_DB_STATUS_SUCCESS_ONE_RESULT != qs)
  {
    GNUNET_break (GNUNET_DB_STATUS_SOFT_ERROR == qs);
    cc->qs = qs;
    return GNUNET_SYSERR;
  }
  qs = check_known_coin (coin_pub,
                         denom_pub,
                         amount_with_fee);
  if (GNUNET_DB_STATUS_SUCCESS_ONE_RESULT != qs)
  {
    GNUNET_break (GNUNET_DB_STATUS_SOFT_ERROR == qs);
    cc->qs = qs;
    return GNUNET_SYSERR;
  }

  /* verify melt signature */
  rmc.purpose.purpose = htonl (TALER_SIGNATURE_WALLET_COIN_MELT);
  rmc.purpose.size = htonl (sizeof (rmc));
  rmc.rc = *rc;
  TALER_amount_hton (&rmc.amount_with_fee,
                     amount_with_fee);
  rmc.melt_fee = issue->fee_refresh;
  rmc.coin_pub = *coin_pub;
  if (GNUNET_OK !=
      GNUNET_CRYPTO_eddsa_verify (TALER_SIGNATURE_WALLET_COIN_MELT,
                                  &rmc.purpose,
                                  &coin_sig->eddsa_signature,
                                  &coin_pub->eddsa_pub))
  {
    report (report_bad_sig_losses,
            json_pack ("{s:s, s:I, s:o, s:o}",
                       "operation", "melt",
                       "row", (json_int_t) rowid,
                       "loss", TALER_JSON_from_amount (amount_with_fee),
                       "key_pub", GNUNET_JSON_from_data_auto (coin_pub)));
    GNUNET_break (GNUNET_OK ==
                  TALER_amount_add (&total_bad_sig_loss,
                                    &total_bad_sig_loss,
                                    amount_with_fee));
    return GNUNET_OK;
  }
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Melting coin %s in denomination `%s' of value %s\n",
              TALER_B2S (coin_pub),
              GNUNET_h2s (&issue->denom_hash),
              TALER_amount2s (amount_with_fee));

  {
    struct RevealContext reveal_ctx;
    struct TALER_Amount refresh_cost;
    int err;

    GNUNET_assert (GNUNET_OK ==
                   TALER_amount_get_zero (amount_with_fee->currency,
                                          &refresh_cost));
    memset (&reveal_ctx,
            0,
            sizeof (reveal_ctx));
    qs = edb->get_refresh_reveal (edb->cls,
                                  esession,
                                  rc,
                                  &reveal_data_cb,
                                  &reveal_ctx);
    if (0 > qs)
    {
      GNUNET_break (GNUNET_DB_STATUS_SOFT_ERROR == qs);
      cc->qs = GNUNET_DB_STATUS_HARD_ERROR;
      return GNUNET_SYSERR;
    }
    if ( (GNUNET_DB_STATUS_SUCCESS_NO_RESULTS == qs) ||
         (0 == reveal_ctx.num_freshcoins) )
    {
      /* This can happen if /refresh/reveal was not yet called or only
         with invalid data, even if the exchange is correctly
         operating. We still report it. */
      report (report_refreshs_hanging,
              json_pack ("{s:I, s:o, s:o}",
                         "row", (json_int_t) rowid,
                         "amount", TALER_JSON_from_amount (amount_with_fee),
                         "coin_pub", GNUNET_JSON_from_data_auto (coin_pub)));
      GNUNET_break (GNUNET_OK ==
                    TALER_amount_add (&total_refresh_hanging,
                                      &total_refresh_hanging,
                                      amount_with_fee));
      return GNUNET_OK;
    }

    {
      const struct TALER_DenominationKeyValidityPS *new_issues[reveal_ctx.
                                                               num_freshcoins];

      /* Update outstanding amounts for all new coin's denominations, and check
         that the resulting amounts are consistent with the value being refreshed. */
      err = GNUNET_OK;
      for (unsigned int i = 0; i<reveal_ctx.num_freshcoins; i++)
      {
        /* lookup new coin denomination key */
        qs = get_denomination_info (&reveal_ctx.new_dps[i],
                                    &new_issues[i],
                                    NULL);
        if (GNUNET_DB_STATUS_SUCCESS_NO_RESULTS == qs)
        {
          report_row_inconsistency ("refresh_reveal",
                                    rowid,
                                    "denomination key not found");
          err = GNUNET_NO; /* terminate, but return "OK" */
        }
        else if (GNUNET_DB_STATUS_SUCCESS_ONE_RESULT != qs)
        {
          GNUNET_break (GNUNET_DB_STATUS_SOFT_ERROR == qs);
          cc->qs = qs;
          err = GNUNET_SYSERR; /* terminate, return GNUNET_SYSERR */
        }
        GNUNET_CRYPTO_rsa_public_key_free (
          reveal_ctx.new_dps[i].rsa_public_key);
        reveal_ctx.new_dps[i].rsa_public_key = NULL;
      }
      GNUNET_free (reveal_ctx.new_dps);
      reveal_ctx.new_dps = NULL;

      if (GNUNET_OK != err)
        return (GNUNET_SYSERR == err) ? GNUNET_SYSERR : GNUNET_OK;

      /* calculate total refresh cost */
      for (unsigned int i = 0; i<reveal_ctx.num_freshcoins; i++)
      {
        /* update cost of refresh */
        struct TALER_Amount fee;
        struct TALER_Amount value;

        TALER_amount_ntoh (&fee,
                           &new_issues[i]->fee_withdraw);
        TALER_amount_ntoh (&value,
                           &new_issues[i]->value);
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
          cc->qs = GNUNET_DB_STATUS_HARD_ERROR;
          return GNUNET_SYSERR;
        }
      }

      /* compute contribution of old coin */
      {
        struct TALER_Amount melt_fee;

        TALER_amount_ntoh (&melt_fee,
                           &issue->fee_refresh);
        if (GNUNET_OK !=
            TALER_amount_subtract (&amount_without_fee,
                                   amount_with_fee,
                                   &melt_fee))
        {
          GNUNET_break (0);
          cc->qs = GNUNET_DB_STATUS_HARD_ERROR;
          return GNUNET_SYSERR;
        }
      }

      /* check old coin covers complete expenses */
      if (1 == TALER_amount_cmp (&refresh_cost,
                                 &amount_without_fee))
      {
        /* refresh_cost > amount_without_fee */
        report_amount_arithmetic_inconsistency ("melt (fee)",
                                                rowid,
                                                &amount_without_fee,
                                                &refresh_cost,
                                                -1);
        return GNUNET_OK;
      }

      /* update outstanding denomination amounts */
      for (unsigned int i = 0; i<reveal_ctx.num_freshcoins; i++)
      {
        struct DenominationSummary *dsi;
        struct TALER_Amount value;

        dsi = get_denomination_summary (cc,
                                        new_issues[i],
                                        &new_issues[i]->denom_hash);
        if (NULL == dsi)
        {
          GNUNET_break (0);
          return GNUNET_SYSERR;
        }
        TALER_amount_ntoh (&value,
                           &new_issues[i]->value);
        GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                    "Created fresh coin in denomination `%s' of value %s\n",
                    GNUNET_h2s (&new_issues[i]->denom_hash),
                    TALER_amount2s (&value));
        dsi->num_issued++;
        if (GNUNET_OK !=
            TALER_amount_add (&dsi->denom_balance,
                              &dsi->denom_balance,
                              &value))
        {
          GNUNET_break (0);
          cc->qs = GNUNET_DB_STATUS_HARD_ERROR;
          return GNUNET_SYSERR;
        }
        if (GNUNET_OK !=
            TALER_amount_add (&dsi->denom_risk,
                              &dsi->denom_risk,
                              &value))
        {
          GNUNET_break (0);
          cc->qs = GNUNET_DB_STATUS_HARD_ERROR;
          return GNUNET_SYSERR;
        }
        GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                    "New balance of denomination `%s' is %s\n",
                    GNUNET_h2s (&new_issues[i]->denom_hash),
                    TALER_amount2s (&dsi->denom_balance));
        if (GNUNET_OK !=
            TALER_amount_add (&total_escrow_balance,
                              &total_escrow_balance,
                              &value))
        {
          GNUNET_break (0);
          cc->qs = GNUNET_DB_STATUS_HARD_ERROR;
          return GNUNET_SYSERR;
        }
        if (GNUNET_OK !=
            TALER_amount_add (&total_risk,
                              &total_risk,
                              &value))
        {
          GNUNET_break (0);
          cc->qs = GNUNET_DB_STATUS_HARD_ERROR;
          return GNUNET_SYSERR;
        }
      }
    }
  }

  /* update old coin's denomination balance */
  dso = get_denomination_summary (cc,
                                  issue,
                                  &issue->denom_hash);
  if (NULL == dso)
  {
    GNUNET_break (0);
    return GNUNET_SYSERR;
  }
  if (GNUNET_SYSERR ==
      TALER_amount_subtract (&tmp,
                             &dso->denom_balance,
                             amount_with_fee))
  {
    GNUNET_assert (GNUNET_OK ==
                   TALER_amount_add (&dso->denom_loss,
                                     &dso->denom_loss,
                                     amount_with_fee));
    dso->report_emergency = GNUNET_YES;
  }
  else
  {
    dso->denom_balance = tmp;
  }
  if (-1 == TALER_amount_cmp (&total_escrow_balance,
                              amount_with_fee))
  {
    /* This can theoretically happen if for example the exchange
       never issued any coins (i.e. escrow balance is zero), but
       accepted a forged coin (i.e. emergency situation after
       private key compromise). In that case, we cannot even
       subtract the profit we make from the fee from the escrow
       balance. Tested as part of test-auditor.sh, case #18 */report_amount_arithmetic_inconsistency (
      "subtracting refresh fee from escrow balance",
      rowid,
      &total_escrow_balance,
      amount_with_fee,
      0);
  }
  else
  {
    GNUNET_assert (GNUNET_SYSERR !=
                   TALER_amount_subtract (&total_escrow_balance,
                                          &total_escrow_balance,
                                          amount_with_fee));
  }

  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "New balance of denomination `%s' after melt is %s\n",
              GNUNET_h2s (&issue->denom_hash),
              TALER_amount2s (&dso->denom_balance));

  /* update global melt fees */
  {
    struct TALER_Amount rfee;

    TALER_amount_ntoh (&rfee,
                       &issue->fee_refresh);
    if (GNUNET_OK !=
        TALER_amount_add (&total_melt_fee_income,
                          &total_melt_fee_income,
                          &rfee))
    {
      GNUNET_break (0);
      cc->qs = GNUNET_DB_STATUS_HARD_ERROR;
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
 * @param cls closure
 * @param rowid unique serial ID for the deposit in our DB
 * @param timestamp when did the deposit happen
 * @param merchant_pub public key of the merchant
 * @param denom_pub denomination public key of @a coin_pub
 * @param coin_pub public key of the coin
 * @param coin_sig signature from the coin
 * @param amount_with_fee amount that was deposited including fee
 * @param h_contract_terms hash of the proposal data known to merchant and customer
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
            const struct TALER_DenominationPublicKey *denom_pub,
            const struct TALER_CoinSpendPublicKeyP *coin_pub,
            const struct TALER_CoinSpendSignatureP *coin_sig,
            const struct TALER_Amount *amount_with_fee,
            const struct GNUNET_HashCode *h_contract_terms,
            struct GNUNET_TIME_Absolute refund_deadline,
            struct GNUNET_TIME_Absolute wire_deadline,
            const json_t *receiver_wire_account,
            int done)
{
  struct CoinContext *cc = cls;
  const struct TALER_DenominationKeyValidityPS *issue;
  struct DenominationSummary *ds;
  struct TALER_DepositRequestPS dr;
  struct TALER_Amount tmp;
  enum GNUNET_DB_QueryStatus qs;

  (void) wire_deadline;
  (void) done;
  GNUNET_assert (rowid >= ppc.last_deposit_serial_id); /* should be monotonically increasing */
  ppc.last_deposit_serial_id = rowid + 1;

  qs = get_denomination_info (denom_pub,
                              &issue,
                              NULL);
  if (GNUNET_DB_STATUS_SUCCESS_NO_RESULTS == qs)
  {
    report_row_inconsistency ("deposits",
                              rowid,
                              "denomination key not found");
    return GNUNET_OK;
  }

  if (GNUNET_DB_STATUS_SUCCESS_ONE_RESULT != qs)
  {
    GNUNET_break (GNUNET_DB_STATUS_SOFT_ERROR == qs);
    cc->qs = qs;
    return GNUNET_SYSERR;
  }
  qs = check_known_coin (coin_pub,
                         denom_pub,
                         amount_with_fee);
  if (GNUNET_DB_STATUS_SUCCESS_ONE_RESULT != qs)
  {
    GNUNET_break (GNUNET_DB_STATUS_SOFT_ERROR == qs);
    cc->qs = qs;
    return GNUNET_SYSERR;
  }

  /* Verify deposit signature */
  dr.purpose.purpose = htonl (TALER_SIGNATURE_WALLET_COIN_DEPOSIT);
  dr.purpose.size = htonl (sizeof (dr));
  dr.h_contract_terms = *h_contract_terms;
  if (GNUNET_OK !=
      TALER_JSON_merchant_wire_signature_hash (receiver_wire_account,
                                               &dr.h_wire))
  {
    report (report_bad_sig_losses,
            json_pack ("{s:s, s:I, s:o, s:o}",
                       "operation", "deposit",
                       "row", (json_int_t) rowid,
                       "loss", TALER_JSON_from_amount (amount_with_fee),
                       "key_pub", GNUNET_JSON_from_data_auto (coin_pub)));
    GNUNET_break (GNUNET_OK ==
                  TALER_amount_add (&total_bad_sig_loss,
                                    &total_bad_sig_loss,
                                    amount_with_fee));
    return GNUNET_OK;
  }
  dr.timestamp = GNUNET_TIME_absolute_hton (timestamp);
  dr.refund_deadline = GNUNET_TIME_absolute_hton (refund_deadline);
  TALER_amount_hton (&dr.amount_with_fee,
                     amount_with_fee);
  dr.deposit_fee = issue->fee_deposit;
  dr.merchant = *merchant_pub;
  dr.coin_pub = *coin_pub;
  if (GNUNET_OK !=
      GNUNET_CRYPTO_eddsa_verify (TALER_SIGNATURE_WALLET_COIN_DEPOSIT,
                                  &dr.purpose,
                                  &coin_sig->eddsa_signature,
                                  &coin_pub->eddsa_pub))
  {
    report (report_bad_sig_losses,
            json_pack ("{s:s, s:I, s:o, s:o}",
                       "operation", "deposit",
                       "row", (json_int_t) rowid,
                       "loss", TALER_JSON_from_amount (amount_with_fee),
                       "key_pub", GNUNET_JSON_from_data_auto (coin_pub)));
    GNUNET_break (GNUNET_OK ==
                  TALER_amount_add (&total_bad_sig_loss,
                                    &total_bad_sig_loss,
                                    amount_with_fee));
    return GNUNET_OK;
  }
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Deposited coin %s in denomination `%s' of value %s\n",
              TALER_B2S (coin_pub),
              GNUNET_h2s (&issue->denom_hash),
              TALER_amount2s (amount_with_fee));

  /* update old coin's denomination balance */
  ds = get_denomination_summary (cc,
                                 issue,
                                 &issue->denom_hash);
  if (NULL == ds)
  {
    GNUNET_break (0);
    return GNUNET_SYSERR;
  }
  if (GNUNET_SYSERR ==
      TALER_amount_subtract (&tmp,
                             &ds->denom_balance,
                             amount_with_fee))
  {
    GNUNET_assert (GNUNET_OK ==
                   TALER_amount_add (&ds->denom_loss,
                                     &ds->denom_loss,
                                     amount_with_fee));
    ds->report_emergency = GNUNET_YES;
  }
  else
  {
    ds->denom_balance = tmp;
  }

  if (-1 == TALER_amount_cmp (&total_escrow_balance,
                              amount_with_fee))
  {
    /* This can theoretically happen if for example the exchange
       never issued any coins (i.e. escrow balance is zero), but
       accepted a forged coin (i.e. emergency situation after
       private key compromise). In that case, we cannot even
       subtract the profit we make from the fee from the escrow
       balance. Tested as part of test-auditor.sh, case #18 */report_amount_arithmetic_inconsistency (
      "subtracting deposit fee from escrow balance",
      rowid,
      &total_escrow_balance,
      amount_with_fee,
      0);
  }
  else
  {
    GNUNET_assert (GNUNET_SYSERR !=
                   TALER_amount_subtract (&total_escrow_balance,
                                          &total_escrow_balance,
                                          amount_with_fee));
  }

  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "New balance of denomination `%s' after deposit is %s\n",
              GNUNET_h2s (&issue->denom_hash),
              TALER_amount2s (&ds->denom_balance));

  /* update global up melt fees */
  {
    struct TALER_Amount dfee;

    TALER_amount_ntoh (&dfee,
                       &issue->fee_deposit);
    if (GNUNET_OK !=
        TALER_amount_add (&total_deposit_fee_income,
                          &total_deposit_fee_income,
                          &dfee))
    {
      GNUNET_break (0);
      cc->qs = GNUNET_DB_STATUS_HARD_ERROR;
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
 * @param cls closure
 * @param rowid unique serial ID for the refund in our DB
 * @param denom_pub denomination public key of @a coin_pub
 * @param coin_pub public key of the coin
 * @param merchant_pub public key of the merchant
 * @param merchant_sig signature of the merchant
 * @param h_contract_terms hash of the proposal data known to merchant and customer
 * @param rtransaction_id refund transaction ID chosen by the merchant
 * @param amount_with_fee amount that was deposited including fee
 * @return #GNUNET_OK to continue to iterate, #GNUNET_SYSERR to stop
 */
static int
refund_cb (void *cls,
           uint64_t rowid,
           const struct TALER_DenominationPublicKey *denom_pub,
           const struct TALER_CoinSpendPublicKeyP *coin_pub,
           const struct TALER_MerchantPublicKeyP *merchant_pub,
           const struct TALER_MerchantSignatureP *merchant_sig,
           const struct GNUNET_HashCode *h_contract_terms,
           uint64_t rtransaction_id,
           const struct TALER_Amount *amount_with_fee)
{
  struct CoinContext *cc = cls;
  const struct TALER_DenominationKeyValidityPS *issue;
  struct DenominationSummary *ds;
  struct TALER_RefundRequestPS rr;
  struct TALER_Amount amount_without_fee;
  struct TALER_Amount refund_fee;
  enum GNUNET_DB_QueryStatus qs;

  GNUNET_assert (rowid >= ppc.last_refund_serial_id); /* should be monotonically increasing */
  ppc.last_refund_serial_id = rowid + 1;

  qs = get_denomination_info (denom_pub,
                              &issue,
                              NULL);
  if (GNUNET_DB_STATUS_SUCCESS_NO_RESULTS == qs)
  {
    report_row_inconsistency ("refunds",
                              rowid,
                              "denomination key not found");
    return GNUNET_SYSERR;
  }
  if (GNUNET_DB_STATUS_SUCCESS_ONE_RESULT != qs)
  {
    GNUNET_break (GNUNET_DB_STATUS_SOFT_ERROR == qs);
    return GNUNET_SYSERR;
  }

  /* verify refund signature */
  rr.purpose.purpose = htonl (TALER_SIGNATURE_MERCHANT_REFUND);
  rr.purpose.size = htonl (sizeof (rr));
  rr.h_contract_terms = *h_contract_terms;
  rr.coin_pub = *coin_pub;
  rr.merchant = *merchant_pub;
  rr.rtransaction_id = GNUNET_htonll (rtransaction_id);
  TALER_amount_hton (&rr.refund_amount,
                     amount_with_fee);
  rr.refund_fee = issue->fee_refund;
  if (GNUNET_OK !=
      GNUNET_CRYPTO_eddsa_verify (TALER_SIGNATURE_MERCHANT_REFUND,
                                  &rr.purpose,
                                  &merchant_sig->eddsa_sig,
                                  &merchant_pub->eddsa_pub))
  {
    report (report_bad_sig_losses,
            json_pack ("{s:s, s:I, s:o, s:o}",
                       "operation", "refund",
                       "row", (json_int_t) rowid,
                       "loss", TALER_JSON_from_amount (amount_with_fee),
                       "key_pub", GNUNET_JSON_from_data_auto (merchant_pub)));
    GNUNET_break (GNUNET_OK ==
                  TALER_amount_add (&total_bad_sig_loss,
                                    &total_bad_sig_loss,
                                    amount_with_fee));
    return GNUNET_OK;
  }

  TALER_amount_ntoh (&refund_fee,
                     &issue->fee_refund);
  if (GNUNET_OK !=
      TALER_amount_subtract (&amount_without_fee,
                             amount_with_fee,
                             &refund_fee))
  {
    report_amount_arithmetic_inconsistency ("refund (fee)",
                                            rowid,
                                            &amount_without_fee,
                                            &refund_fee,
                                            -1);
    return GNUNET_OK;
  }

  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Refunding coin %s in denomination `%s' value %s\n",
              TALER_B2S (coin_pub),
              GNUNET_h2s (&issue->denom_hash),
              TALER_amount2s (amount_with_fee));

  /* update coin's denomination balance */
  ds = get_denomination_summary (cc,
                                 issue,
                                 &issue->denom_hash);
  if (NULL == ds)
  {
    GNUNET_break (0);
    return GNUNET_SYSERR;
  }
  if (GNUNET_OK !=
      TALER_amount_add (&ds->denom_balance,
                        &ds->denom_balance,
                        &amount_without_fee))
  {
    GNUNET_break (0);
    cc->qs = GNUNET_DB_STATUS_HARD_ERROR;
    return GNUNET_SYSERR;
  }
  if (GNUNET_OK !=
      TALER_amount_add (&ds->denom_risk,
                        &ds->denom_risk,
                        &amount_without_fee))
  {
    GNUNET_break (0);
    cc->qs = GNUNET_DB_STATUS_HARD_ERROR;
    return GNUNET_SYSERR;
  }
  if (GNUNET_OK !=
      TALER_amount_add (&total_escrow_balance,
                        &total_escrow_balance,
                        &amount_without_fee))
  {
    GNUNET_break (0);
    cc->qs = GNUNET_DB_STATUS_HARD_ERROR;
    return GNUNET_SYSERR;
  }
  if (GNUNET_OK !=
      TALER_amount_add (&total_risk,
                        &total_risk,
                        &amount_without_fee))
  {
    GNUNET_break (0);
    cc->qs = GNUNET_DB_STATUS_HARD_ERROR;
    return GNUNET_SYSERR;
  }

  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "New balance of denomination `%s' after refund is %s\n",
              GNUNET_h2s (&issue->denom_hash),
              TALER_amount2s (&ds->denom_balance));

  /* update total refund fee balance */
  if (GNUNET_OK !=
      TALER_amount_add (&total_refund_fee_income,
                        &total_refund_fee_income,
                        &refund_fee))
  {
    GNUNET_break (0);
    cc->qs = GNUNET_DB_STATUS_HARD_ERROR;
    return GNUNET_SYSERR;
  }

  return GNUNET_OK;
}


/**
 * Check that the recoup operation was properly initiated by a coin
 * and update the denomination's losses accordingly.
 *
 * @param cc the context with details about the coin
 * @param rowid row identifier used to uniquely identify the recoup operation
 * @param amount how much should be added back to the reserve
 * @param coin public information about the coin
 * @param denom_pub public key of the denomionation of @a coin
 * @param coin_sig signature with @e coin_pub of type #TALER_SIGNATURE_WALLET_COIN_RECOUP
 * @param coin_blind blinding factor used to blind the coin
 * @return #GNUNET_OK to continue to iterate, #GNUNET_SYSERR to stop
 */
static int
check_recoup (struct CoinContext *cc,
              uint64_t rowid,
              const struct TALER_Amount *amount,
              const struct TALER_CoinPublicInfo *coin,
              const struct TALER_DenominationPublicKey *denom_pub,
              const struct TALER_CoinSpendSignatureP *coin_sig,
              const struct TALER_DenominationBlindingKeyP *coin_blind)
{
  struct TALER_RecoupRequestPS pr;
  struct DenominationSummary *ds;
  enum GNUNET_DB_QueryStatus qs;
  const struct TALER_DenominationKeyValidityPS *issue;

  if (GNUNET_OK !=
      TALER_test_coin_valid (coin,
                             denom_pub))
  {
    report (report_bad_sig_losses,
            json_pack ("{s:s, s:I, s:o, s:o}",
                       "operation", "recoup",
                       "row", (json_int_t) rowid,
                       "loss", TALER_JSON_from_amount (amount),
                       "key_pub", GNUNET_JSON_from_data_auto (
                         &pr.h_denom_pub)));
    GNUNET_break (GNUNET_OK ==
                  TALER_amount_add (&total_bad_sig_loss,
                                    &total_bad_sig_loss,
                                    amount));
  }
  qs = get_denomination_info (denom_pub,
                              &issue,
                              &pr.h_denom_pub);
  if (GNUNET_DB_STATUS_SUCCESS_NO_RESULTS == qs)
  {
    report_row_inconsistency ("recoup",
                              rowid,
                              "denomination key not found");
    return GNUNET_OK;
  }
  if (GNUNET_DB_STATUS_SUCCESS_ONE_RESULT != qs)
  {
    /* The key not existing should be prevented by foreign key constraints,
       so must be a transient DB error. */
    GNUNET_break (GNUNET_DB_STATUS_SOFT_ERROR == qs);
    cc->qs = qs;
    return GNUNET_SYSERR;
  }
  pr.purpose.purpose = htonl (TALER_SIGNATURE_WALLET_COIN_RECOUP);
  pr.purpose.size = htonl (sizeof (pr));
  pr.coin_pub = coin->coin_pub;
  pr.coin_blind = *coin_blind;
  if (GNUNET_OK !=
      GNUNET_CRYPTO_eddsa_verify (TALER_SIGNATURE_WALLET_COIN_RECOUP,
                                  &pr.purpose,
                                  &coin_sig->eddsa_signature,
                                  &coin->coin_pub.eddsa_pub))
  {
    report (report_bad_sig_losses,
            json_pack ("{s:s, s:I, s:o, s:o}",
                       "operation", "recoup",
                       "row", (json_int_t) rowid,
                       "loss", TALER_JSON_from_amount (amount),
                       "coin_pub", GNUNET_JSON_from_data_auto (
                         &coin->coin_pub)));
    GNUNET_break (GNUNET_OK ==
                  TALER_amount_add (&total_bad_sig_loss,
                                    &total_bad_sig_loss,
                                    amount));
    return GNUNET_OK;
  }
  ds = get_denomination_summary (cc,
                                 issue,
                                 &issue->denom_hash);
  if (GNUNET_NO == ds->was_revoked)
  {
    /* Woopsie, we allowed recoup on non-revoked denomination!? */
    report (report_bad_sig_losses,
            json_pack ("{s:s, s:I, s:o, s:o}",
                       "operation", "recoup (denomination not revoked)",
                       "row", (json_int_t) rowid,
                       "loss", TALER_JSON_from_amount (amount),
                       "coin_pub", GNUNET_JSON_from_data_auto (
                         &coin->coin_pub)));
  }
  GNUNET_break (GNUNET_OK ==
                TALER_amount_add (&ds->denom_recoup,
                                  &ds->denom_recoup,
                                  amount));
  GNUNET_break (GNUNET_OK ==
                TALER_amount_add (&total_recoup_loss,
                                  &total_recoup_loss,
                                  amount));
  return GNUNET_OK;
}


/**
 * Function called about recoups the exchange has to perform.
 *
 * @param cls a `struct CoinContext *`
 * @param rowid row identifier used to uniquely identify the recoup operation
 * @param timestamp when did we receive the recoup request
 * @param amount how much should be added back to the reserve
 * @param reserve_pub public key of the reserve
 * @param coin public information about the coin
 * @param denom_pub denomination public key of @a coin
 * @param coin_sig signature with @e coin_pub of type #TALER_SIGNATURE_WALLET_COIN_RECOUP
 * @param coin_blind blinding factor used to blind the coin
 * @return #GNUNET_OK to continue to iterate, #GNUNET_SYSERR to stop
 */
static int
recoup_cb (void *cls,
           uint64_t rowid,
           struct GNUNET_TIME_Absolute timestamp,
           const struct TALER_Amount *amount,
           const struct TALER_ReservePublicKeyP *reserve_pub,
           const struct TALER_CoinPublicInfo *coin,
           const struct TALER_DenominationPublicKey *denom_pub,
           const struct TALER_CoinSpendSignatureP *coin_sig,
           const struct TALER_DenominationBlindingKeyP *coin_blind)
{
  struct CoinContext *cc = cls;

  (void) timestamp;
  (void) reserve_pub;
  return check_recoup (cc,
                       rowid,
                       amount,
                       coin,
                       denom_pub,
                       coin_sig,
                       coin_blind);
}


/**
 * Function called about recoups on refreshed coins the exchange has to
 * perform.
 *
 * @param cls a `struct CoinContext *`
 * @param rowid row identifier used to uniquely identify the recoup operation
 * @param timestamp when did we receive the recoup request
 * @param amount how much should be added back to the reserve
 * @param old_coin_pub original coin that was refreshed to create @a coin
 * @param coin public information about the coin
 * @param denom_pub denomination public key of @a coin
 * @param coin_sig signature with @e coin_pub of type #TALER_SIGNATURE_WALLET_COIN_RECOUP
 * @param coin_blind blinding factor used to blind the coin
 * @return #GNUNET_OK to continue to iterate, #GNUNET_SYSERR to stop
 */
static int
recoup_refresh_cb (void *cls,
                   uint64_t rowid,
                   struct GNUNET_TIME_Absolute timestamp,
                   const struct TALER_Amount *amount,
                   const struct TALER_CoinSpendPublicKeyP *old_coin_pub,
                   const struct TALER_CoinPublicInfo *coin,
                   const struct TALER_DenominationPublicKey *denom_pub,
                   const struct TALER_CoinSpendSignatureP *coin_sig,
                   const struct TALER_DenominationBlindingKeyP *coin_blind)
{
  struct CoinContext *cc = cls;

  (void) timestamp;
  (void) old_coin_pub;
  return check_recoup (cc,
                       rowid,
                       amount,
                       coin,
                       denom_pub,
                       coin_sig,
                       coin_blind);
}


/**
 * Analyze the exchange's processing of coins.
 *
 * @param cls closure
 * @return transaction status code
 */
static enum GNUNET_DB_QueryStatus
analyze_coins (void *cls)
{
  struct CoinContext cc;
  enum GNUNET_DB_QueryStatus qs;
  enum GNUNET_DB_QueryStatus qsx;
  enum GNUNET_DB_QueryStatus qsp;

  (void) cls;
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Analyzing coins\n");
  qsp = adb->get_auditor_progress_coin (adb->cls,
                                        asession,
                                        &master_pub,
                                        &ppc);
  if (0 > qsp)
  {
    GNUNET_break (GNUNET_DB_STATUS_SOFT_ERROR == qsp);
    return qsp;
  }
  if (GNUNET_DB_STATUS_SUCCESS_NO_RESULTS == qsp)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_MESSAGE,
                "First analysis using this auditor, starting from scratch\n");
  }
  else
  {
    ppc_start = ppc;
    GNUNET_log (GNUNET_ERROR_TYPE_INFO,
                "Resuming coin audit at %llu/%llu/%llu/%llu/%llu\n",
                (unsigned long long) ppc.last_deposit_serial_id,
                (unsigned long long) ppc.last_melt_serial_id,
                (unsigned long long) ppc.last_refund_serial_id,
                (unsigned long long) ppc.last_withdraw_serial_id,
                (unsigned long long) ppc.last_recoup_refresh_serial_id);
  }

  /* setup 'cc' */
  cc.qs = GNUNET_DB_STATUS_SUCCESS_ONE_RESULT;
  cc.denom_summaries = GNUNET_CONTAINER_multihashmap_create (256,
                                                             GNUNET_NO);
  qsx = adb->get_balance_summary (adb->cls,
                                  asession,
                                  &master_pub,
                                  &total_escrow_balance,
                                  &total_deposit_fee_income,
                                  &total_melt_fee_income,
                                  &total_refund_fee_income,
                                  &total_risk,
                                  &total_recoup_loss,
                                  &total_irregular_recoups);
  if (0 > qsx)
  {
    GNUNET_break (GNUNET_DB_STATUS_SOFT_ERROR == qsx);
    return qsx;
  }

  /* process withdrawals */
  if (0 >
      (qs = edb->select_withdrawals_above_serial_id (edb->cls,
                                                     esession,
                                                     ppc.
                                                     last_withdraw_serial_id,
                                                     &withdraw_cb,
                                                     &cc)) )
  {
    GNUNET_break (GNUNET_DB_STATUS_SOFT_ERROR == qs);
    return qs;
  }
  if (0 > cc.qs)
    return cc.qs;

  /* process refunds */
  if (0 >
      (qs = edb->select_refunds_above_serial_id (edb->cls,
                                                 esession,
                                                 ppc.last_refund_serial_id,
                                                 &refund_cb,
                                                 &cc)))
  {
    GNUNET_break (GNUNET_DB_STATUS_SOFT_ERROR == qs);
    return qs;
  }
  if (0 > cc.qs)
    return cc.qs;

  /* process refreshs */
  if (0 >
      (qs = edb->select_refreshes_above_serial_id (edb->cls,
                                                   esession,
                                                   ppc.last_melt_serial_id,
                                                   &refresh_session_cb,
                                                   &cc)))
  {
    GNUNET_break (GNUNET_DB_STATUS_SOFT_ERROR == qs);
    return qs;
  }
  if (0 > cc.qs)
    return cc.qs;

  /* process deposits */
  if (0 >
      (qs = edb->select_deposits_above_serial_id (edb->cls,
                                                  esession,
                                                  ppc.last_deposit_serial_id,
                                                  &deposit_cb,
                                                  &cc)))
  {
    GNUNET_break (GNUNET_DB_STATUS_SOFT_ERROR == qs);
    return qs;
  }
  if (0 > cc.qs)
    return cc.qs;

  /* process recoups */
  if (0 >
      (qs = edb->select_recoup_above_serial_id (edb->cls,
                                                esession,
                                                ppc.last_recoup_serial_id,
                                                &recoup_cb,
                                                &cc)))
  {
    GNUNET_break (GNUNET_DB_STATUS_SOFT_ERROR == qs);
    return qs;
  }
  if (0 > cc.qs)
    return cc.qs;
  if (0 >
      (qs = edb->select_recoup_refresh_above_serial_id (edb->cls,
                                                        esession,
                                                        ppc.
                                                        last_recoup_refresh_serial_id,
                                                        &recoup_refresh_cb,
                                                        &cc)))
  {
    GNUNET_break (GNUNET_DB_STATUS_SOFT_ERROR == qs);
    return qs;
  }
  if (0 > cc.qs)
    return cc.qs;

  /* sync 'cc' back to disk */
  cc.qs = GNUNET_DB_STATUS_SUCCESS_ONE_RESULT;
  GNUNET_CONTAINER_multihashmap_iterate (cc.denom_summaries,
                                         &sync_denomination,
                                         &cc);
  GNUNET_CONTAINER_multihashmap_destroy (cc.denom_summaries);
  if (0 > cc.qs)
  {
    GNUNET_break (GNUNET_DB_STATUS_SOFT_ERROR == cc.qs);
    return cc.qs;
  }
  if (GNUNET_DB_STATUS_SUCCESS_ONE_RESULT == qsx)
    qs = adb->update_balance_summary (adb->cls,
                                      asession,
                                      &master_pub,
                                      &total_escrow_balance,
                                      &total_deposit_fee_income,
                                      &total_melt_fee_income,
                                      &total_refund_fee_income,
                                      &total_risk,
                                      &total_recoup_loss,
                                      &total_irregular_recoups);
  else
    qs = adb->insert_balance_summary (adb->cls,
                                      asession,
                                      &master_pub,
                                      &total_escrow_balance,
                                      &total_deposit_fee_income,
                                      &total_melt_fee_income,
                                      &total_refund_fee_income,
                                      &total_risk,
                                      &total_recoup_loss,
                                      &total_irregular_recoups);
  if (0 >= qs)
  {
    GNUNET_break (GNUNET_DB_STATUS_SOFT_ERROR == qs);
    return qs;
  }

  if (GNUNET_DB_STATUS_SUCCESS_ONE_RESULT == qsp)
    qs = adb->update_auditor_progress_coin (adb->cls,
                                            asession,
                                            &master_pub,
                                            &ppc);
  else
    qs = adb->insert_auditor_progress_coin (adb->cls,
                                            asession,
                                            &master_pub,
                                            &ppc);
  if (0 >= qs)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_INFO,
                "Failed to update auditor DB, not recording progress\n");
    GNUNET_break (GNUNET_DB_STATUS_SOFT_ERROR == qs);
    return qs;
  }
  GNUNET_log (GNUNET_ERROR_TYPE_INFO,
              _ ("Concluded coin audit step at %llu/%llu/%llu/%llu/%llu\n"),
              (unsigned long long) ppc.last_deposit_serial_id,
              (unsigned long long) ppc.last_melt_serial_id,
              (unsigned long long) ppc.last_refund_serial_id,
              (unsigned long long) ppc.last_withdraw_serial_id,
              (unsigned long long) ppc.last_recoup_refresh_serial_id);
  return qs;
}


/**
 * Main function that will be run.
 *
 * @param cls closure
 * @param args remaining command-line arguments
 * @param cfgfile name of the configuration file used (for saving, can be NULL!)
 * @param c configuration
 */
static void
run (void *cls,
     char *const *args,
     const char *cfgfile,
     const struct GNUNET_CONFIGURATION_Handle *c)
{
  json_t *report;

  (void) cls;
  (void) args;
  (void) cfgfile;
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Launching auditor\n");
  if (GNUNET_OK !=
      setup_globals (c))
  {
    global_ret = 1;
    return;
  }
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Starting audit\n");
  GNUNET_assert (GNUNET_OK ==
                 TALER_amount_get_zero (currency,
                                        &reported_emergency_loss));
  GNUNET_assert (GNUNET_OK ==
                 TALER_amount_get_zero (currency,
                                        &reported_emergency_risk_by_amount));
  GNUNET_assert (GNUNET_OK ==
                 TALER_amount_get_zero (currency,
                                        &reported_emergency_risk_by_count));
  GNUNET_assert (GNUNET_OK ==
                 TALER_amount_get_zero (currency,
                                        &reported_emergency_loss_by_count));
  GNUNET_assert (GNUNET_OK ==
                 TALER_amount_get_zero (currency,
                                        &total_escrow_balance));
  GNUNET_assert (GNUNET_OK ==
                 TALER_amount_get_zero (currency,
                                        &total_risk));
  GNUNET_assert (GNUNET_OK ==
                 TALER_amount_get_zero (currency,
                                        &total_recoup_loss));
  GNUNET_assert (GNUNET_OK ==
                 TALER_amount_get_zero (currency,
                                        &total_irregular_recoups));
  GNUNET_assert (GNUNET_OK ==
                 TALER_amount_get_zero (currency,
                                        &total_deposit_fee_income));
  GNUNET_assert (GNUNET_OK ==
                 TALER_amount_get_zero (currency,
                                        &total_melt_fee_income));
  GNUNET_assert (GNUNET_OK ==
                 TALER_amount_get_zero (currency,
                                        &total_refund_fee_income));
  GNUNET_assert (GNUNET_OK ==
                 TALER_amount_get_zero (currency,
                                        &total_arithmetic_delta_plus));
  GNUNET_assert (GNUNET_OK ==
                 TALER_amount_get_zero (currency,
                                        &total_arithmetic_delta_minus));
  GNUNET_assert (GNUNET_OK ==
                 TALER_amount_get_zero (currency,
                                        &total_bad_sig_loss));
  GNUNET_assert (GNUNET_OK ==
                 TALER_amount_get_zero (currency,
                                        &total_refresh_hanging));
  GNUNET_assert (NULL !=
                 (report_emergencies = json_array ()));
  GNUNET_assert (NULL !=
                 (report_emergencies_by_count = json_array ()));
  GNUNET_assert (NULL !=
                 (report_row_inconsistencies = json_array ()));
  GNUNET_assert (NULL !=
                 (report_amount_arithmetic_inconsistencies = json_array ()));
  GNUNET_assert (NULL !=
                 (report_bad_sig_losses = json_array ()));
  GNUNET_assert (NULL !=
                 (report_refreshs_hanging = json_array ()));
  if (GNUNET_OK !=
      setup_sessions_and_run (&analyze_coins,
                              NULL))
  {
    global_ret = 1;
    return;
  }
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Audit complete\n");
  report = json_pack ("{s:o, s:o, s:o, s:o, s:o,"
                      " s:o, s:o, s:o, s:o, s:o,"
                      " s:o, s:o, s:o, s:o, s:o,"
                      " s:o, s:o, s:o, s:o, s:o,"
                      " s:I, s:I, s:I, s:I, s:I,"
                      " s:I, s:I, s:I, s:I, s:I,"
                      " s:I, s:I, s:o, s:o, s:o}",
                      /* Block #1 */
                      "total_escrow_balance",
                      TALER_JSON_from_amount (&total_escrow_balance),
                      "total_active_risk",
                      TALER_JSON_from_amount (&total_risk),
                      "total_deposit_fee_income",
                      TALER_JSON_from_amount (&total_deposit_fee_income),
                      "total_melt_fee_income",
                      TALER_JSON_from_amount (&total_melt_fee_income),
                      "total_refund_fee_income",
                      TALER_JSON_from_amount (&total_refund_fee_income),
                      /* Block #2 */
                      /* Tested in test-auditor.sh #18 */
                      "emergencies",
                      report_emergencies,
                      /* Tested in test-auditor.sh #18 */
                      "emergencies_risk_by_amount",
                      TALER_JSON_from_amount (
                        &reported_emergency_risk_by_amount),
                      /* Tested in test-auditor.sh #4/#5/#6/#7/#13 */
                      "bad_sig_losses",
                      report_bad_sig_losses,
                      /* Tested in test-auditor.sh #4/#5/#6/#7/#13 */
                      "total_bad_sig_loss",
                      TALER_JSON_from_amount (&total_bad_sig_loss),
                      /* Tested in test-auditor.sh #14/#15 */
                      "row_inconsistencies",
                      report_row_inconsistencies,
                      /* Block #3 */
                      "amount_arithmetic_inconsistencies",
                      report_amount_arithmetic_inconsistencies,
                      "total_arithmetic_delta_plus",
                      TALER_JSON_from_amount (&total_arithmetic_delta_plus),
                      "total_arithmetic_delta_minus",
                      TALER_JSON_from_amount (&total_arithmetic_delta_minus),
                      /* Tested in test-auditor.sh #12 */
                      "total_refresh_hanging",
                      TALER_JSON_from_amount (&total_refresh_hanging),
                      /* Tested in test-auditor.sh #12 */
                      "refresh_hanging",
                      report_refreshs_hanging,
                      /* Block #4 */
                      "total_recoup_loss",
                      TALER_JSON_from_amount (&total_recoup_loss),
                      /* Tested in test-auditor.sh #18 */
                      "emergencies_by_count",
                      report_emergencies_by_count,
                      /* Tested in test-auditor.sh #18 */
                      "emergencies_risk_by_count",
                      TALER_JSON_from_amount (
                        &reported_emergency_risk_by_count),
                      /* Tested in test-auditor.sh #18 */
                      "emergencies_loss",
                      TALER_JSON_from_amount (&reported_emergency_loss),
                      /* Tested in test-auditor.sh #18 */
                      "emergencies_loss_by_count",
                      TALER_JSON_from_amount (
                        &reported_emergency_loss_by_count),
                      /* Block #5 */
                      "start_ppc_withdraw_serial_id",
                      (json_int_t) ppc_start.last_withdraw_serial_id,
                      "start_ppc_deposit_serial_id",
                      (json_int_t) ppc_start.last_deposit_serial_id,
                      "start_ppc_melt_serial_id",
                      (json_int_t) ppc_start.last_melt_serial_id,
                      "start_ppc_refund_serial_id",
                      (json_int_t) ppc_start.last_refund_serial_id,
                      "start_ppc_recoup_serial_id",
                      (json_int_t) ppc_start.last_recoup_serial_id,
                      /* Block #6 */
                      "start_ppc_recoup_refresh_serial_id",
                      (json_int_t) ppc_start.last_recoup_refresh_serial_id,
                      "end_ppc_withdraw_serial_id",
                      (json_int_t) ppc.last_withdraw_serial_id,
                      "end_ppc_deposit_serial_id",
                      (json_int_t) ppc.last_deposit_serial_id,
                      "end_ppc_melt_serial_id",
                      (json_int_t) ppc.last_melt_serial_id,
                      "end_ppc_refund_serial_id",
                      (json_int_t) ppc.last_refund_serial_id,
                      /* Block #7 */
                      "end_ppc_recoup_serial_id",
                      (json_int_t) ppc.last_recoup_serial_id,
                      "end_ppc_recoup_refresh_serial_id",
                      (json_int_t) ppc.last_recoup_refresh_serial_id,
                      "auditor_start_time", json_string (
                        GNUNET_STRINGS_absolute_time_to_string (start_time)),
                      "auditor_end_time", json_string (
                        GNUNET_STRINGS_absolute_time_to_string (
                          GNUNET_TIME_absolute_get ())),
                      "total_irregular_recoups",
                      TALER_JSON_from_amount (&total_irregular_recoups)
                      );
  GNUNET_break (NULL != report);
  finish_report (report);
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
    GNUNET_GETOPT_option_base32_auto ('m',
                                      "exchange-key",
                                      "KEY",
                                      "public key of the exchange (Crockford base32 encoded)",
                                      &master_pub),
    GNUNET_GETOPT_option_flag ('r',
                               "restart",
                               "restart audit from the beginning (required on first run)",
                               &restart),
    GNUNET_GETOPT_option_timetravel ('T',
                                     "timetravel"),
    GNUNET_GETOPT_OPTION_END
  };

  /* force linker to link against libtalerutil; if we do
     not do this, the linker may "optimize" libtalerutil
     away and skip #TALER_OS_init(), which we do need */
  (void) TALER_project_data_default ();
  GNUNET_assert (GNUNET_OK ==
                 GNUNET_log_setup ("taler-auditor",
                                   "MESSAGE",
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