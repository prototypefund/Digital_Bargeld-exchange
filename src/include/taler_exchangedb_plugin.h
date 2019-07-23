/*
  This file is part of TALER
  Copyright (C) 2014-2017 Taler Systems SA

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
 * @file include/taler_exchangedb_plugin.h
 * @brief Low-level (statement-level) database access for the exchange
 * @author Florian Dold
 * @author Christian Grothoff
 */
#ifndef TALER_EXCHANGEDB_PLUGIN_H
#define TALER_EXCHANGEDB_PLUGIN_H
#include <jansson.h>
#include <gnunet/gnunet_util_lib.h>
#include <gnunet/gnunet_db_lib.h>
#include "taler_exchangedb_lib.h"


/**
 * @brief Information we keep on bank transfer(s) that established a reserve.
 */
struct TALER_EXCHANGEDB_BankTransfer
{

  /**
   * Public key of the reserve that was filled.
   */
  struct TALER_ReservePublicKeyP reserve_pub;

  /**
   * Amount that was transferred to the exchange.
   */
  struct TALER_Amount amount;

  /**
   * When did the exchange receive the incoming transaction?
   * (This is the execution date of the exchange's database,
   * the execution date of the bank should be in @e wire).
   */
  struct GNUNET_TIME_Absolute execution_date;

  /**
   * Detailed wire information about the sending account
   * in "payto://" format.
   */
  char *sender_account_details;

  /**
   * Data uniquely identifying the wire transfer (wire transfer-type specific)
   */
  void *wire_reference;

  /**
   * Number of bytes in @e wire_reference.
   */
  size_t wire_reference_size;

};


/**
 * @brief Information we keep on bank transfer(s) that
 * closed a reserve.
 */
struct TALER_EXCHANGEDB_ClosingTransfer
{

  /**
   * Public key of the reserve that was depleted.
   */
  struct TALER_ReservePublicKeyP reserve_pub;

  /**
   * Amount that was transferred to the exchange.
   */
  struct TALER_Amount amount;

  /**
   * Amount that was charged by the exchange.
   */
  struct TALER_Amount closing_fee;

  /**
   * When did the exchange execute the transaction?
   */
  struct GNUNET_TIME_Absolute execution_date;

  /**
   * Detailed wire information about the receiving account
   * in payto://-format.
   */
  char *receiver_account_details;

  /**
   * Detailed wire transfer information that uniquely identifies the
   * wire transfer.
   */
  struct TALER_WireTransferIdentifierRawP wtid;

};


/**
 * @brief A summary of a Reserve
 */
struct TALER_EXCHANGEDB_Reserve
{
  /**
   * The reserve's public key.  This uniquely identifies the reserve
   */
  struct TALER_ReservePublicKeyP pub;

  /**
   * The balance amount existing in the reserve
   */
  struct TALER_Amount balance;

  /**
   * The expiration date of this reserve
   */
  struct GNUNET_TIME_Absolute expiry;
};


/**
 * @brief Information we keep for a withdrawn coin to reproduce
 * the /withdraw operation if needed, and to have proof
 * that a reserve was drained by this amount.
 */
struct TALER_EXCHANGEDB_CollectableBlindcoin
{

  /**
   * Our signature over the (blinded) coin.
   */
  struct TALER_DenominationSignature sig;

  /**
   * Hash of the denomination key (which coin was generated).
   */
  struct GNUNET_HashCode denom_pub_hash;

  /**
   * Value of the coin being exchangeed (matching the denomination key)
   * plus the transaction fee.  We include this in what is being
   * signed so that we can verify a reserve's remaining total balance
   * without needing to access the respective denomination key
   * information each time.
   */
  struct TALER_Amount amount_with_fee;

  /**
   * Withdrawl fee charged by the exchange.  This must match the Exchange's
   * denomination key's withdrawl fee.  If the client puts in an
   * invalid withdrawl fee (too high or too low) that does not match
   * the Exchange's denomination key, the withdraw operation is invalid
   * and will be rejected by the exchange.  The @e amount_with_fee minus
   * the @e withdraw_fee is must match the value of the generated
   * coin.  We include this in what is being signed so that we can
   * verify a exchange's accounting without needing to access the
   * respective denomination key information each time.
   */
  struct TALER_Amount withdraw_fee;

  /**
   * Public key of the reserve that was drained.
   */
  struct TALER_ReservePublicKeyP reserve_pub;

  /**
   * Hash over the blinded message, needed to verify
   * the @e reserve_sig.
   */
  struct GNUNET_HashCode h_coin_envelope;

  /**
   * Signature confirming the withdrawl, matching @e reserve_pub,
   * @e denom_pub and @e h_coin_envelope.
   */
  struct TALER_ReserveSignatureP reserve_sig;
};


/**
 * Information the exchange records about a /payback request.
 */
struct TALER_EXCHANGEDB_Payback
{

  /**
   * Information about the coin that was paid back.
   */
  struct TALER_CoinPublicInfo coin;

  /**
   * Blinding factor supplied to prove to the exchange that
   * the coin came from this reserve.
   */
  struct TALER_DenominationBlindingKeyP coin_blind;

  /**
   * Signature of the coin of type
   * #TALER_SIGNATURE_WALLET_COIN_PAYBACK.
   */
  struct TALER_CoinSpendSignatureP coin_sig;

  /**
   * Public key of the reserve the coin was paid back into.
   */
  struct TALER_ReservePublicKeyP reserve_pub;

  /**
   * How much was the coin still worth at this time?
   */
  struct TALER_Amount value;

  /**
   * When did the /payback operation happen?
   */
  struct GNUNET_TIME_Absolute timestamp;

};


/**
 * Information the exchange records about a /payback-refresh request.
 */
struct TALER_EXCHANGEDB_PaybackRefresh
{

  /**
   * Information about the coin that was paid back.
   */
  struct TALER_CoinPublicInfo coin;

  /**
   * Blinding factor supplied to prove to the exchange that
   * the coin came from this reserve.
   */
  struct TALER_DenominationBlindingKeyP coin_blind;

  /**
   * Signature of the coin of type
   * #TALER_SIGNATURE_WALLET_COIN_PAYBACK.
   */
  struct TALER_CoinSpendSignatureP coin_sig;

  /**
   * Public key of the old coin that the refresh'ed coin was paid back to.
   */
  struct TALER_CoinSpendPublicKeyP old_coin_pub;

  /**
   * How much was the coin still worth at this time?
   */
  struct TALER_Amount value;

  /**
   * When did the /payback operation happen?
   */
  struct GNUNET_TIME_Absolute timestamp;

};


/**
 * @brief Types of operations on a reserve.
 */
enum TALER_EXCHANGEDB_ReserveOperation
{
  /**
   * Money was deposited into the reserve via a bank transfer.
   * This is how customers establish a reserve at the exchange.
   */
  TALER_EXCHANGEDB_RO_BANK_TO_EXCHANGE = 0,

  /**
   * A Coin was withdrawn from the reserve using /withdraw.
   */
  TALER_EXCHANGEDB_RO_WITHDRAW_COIN = 1,

  /**
   * A coin was returned to the reserve using /payback.
   */
  TALER_EXCHANGEDB_RO_PAYBACK_COIN = 2,

  /**
   * The exchange send inactive funds back from the reserve to the
   * customer's bank account.  This happens when the exchange
   * closes a reserve with a non-zero amount left in it.
   */
  TALER_EXCHANGEDB_RO_EXCHANGE_TO_BANK = 3
};


/**
 * @brief Reserve history as a linked list.  Lists all of the transactions
 * associated with this reserve (such as the bank transfers that
 * established the reserve and all /withdraw operations we have done
 * since).
 */
struct TALER_EXCHANGEDB_ReserveHistory
{

  /**
   * Next entry in the reserve history.
   */
  struct TALER_EXCHANGEDB_ReserveHistory *next;

  /**
   * Type of the event, determins @e details.
   */
  enum TALER_EXCHANGEDB_ReserveOperation type;

  /**
   * Details of the operation, depending on @e type.
   */
  union
  {

    /**
     * Details about a bank transfer to the exchange (reserve
     * was established).
     */
    struct TALER_EXCHANGEDB_BankTransfer *bank;

    /**
     * Details about a /withdraw operation.
     */
    struct TALER_EXCHANGEDB_CollectableBlindcoin *withdraw;

    /**
     * Details about a /payback operation.
     */
    struct TALER_EXCHANGEDB_Payback *payback;

    /**
     * Details about a bank transfer from the exchange (reserve
     * was closed).
     */
    struct TALER_EXCHANGEDB_ClosingTransfer *closing;

  } details;

};


/**
 * @brief Specification for a /deposit operation.  The combination of
 * the coin's public key, the merchant's public key and the
 * transaction ID must be unique.  While a coin can (theoretically) be
 * deposited at the same merchant twice (with partial spending), the
 * merchant must either use a different public key or a different
 * transaction ID for the two transactions.  The same coin must not
 * be used twice at the same merchant for the same transaction
 * (as determined by transaction ID).
 */
struct TALER_EXCHANGEDB_Deposit
{
  /**
   * Information about the coin that is being deposited.
   */
  struct TALER_CoinPublicInfo coin;

  /**
   * ECDSA signature affirming that the customer intends
   * this coin to be deposited at the merchant identified
   * by @e h_wire in relation to the proposal data identified
   * by @e h_contract_terms.
   */
  struct TALER_CoinSpendSignatureP csig;

  /**
   * Public key of the merchant.  Enables later identification
   * of the merchant in case of a need to rollback transactions.
   */
  struct TALER_MerchantPublicKeyP merchant_pub;

  /**
   * Hash over the proposa data between merchant and customer
   * (remains unknown to the Exchange).
   */
  struct GNUNET_HashCode h_contract_terms;

  /**
   * Hash of the (canonical) representation of @e wire, used
   * to check the signature on the request.  Generated by
   * the exchange from the detailed wire data provided by the
   * merchant.
   */
  struct GNUNET_HashCode h_wire;

  /**
   * Detailed information about the receiver for executing the transaction.
   * Includes URL in payto://-format and salt.
   */
  json_t *receiver_wire_account;

  /**
   * Time when this request was generated.  Used, for example, to
   * assess when (roughly) the income was achieved for tax purposes.
   * Note that the Exchange will only check that the timestamp is not "too
   * far" into the future (i.e. several days).  The fact that the
   * timestamp falls within the validity period of the coin's
   * denomination key is irrelevant for the validity of the deposit
   * request, as obviously the customer and merchant could conspire to
   * set any timestamp.  Also, the Exchange must accept very old deposit
   * requests, as the merchant might have been unable to transmit the
   * deposit request in a timely fashion (so back-dating is not
   * prevented).
   */
  struct GNUNET_TIME_Absolute timestamp;

  /**
   * How much time does the merchant have to issue a refund request?
   * Zero if refunds are not allowed.  After this time, the coin
   * cannot be refunded.
   */
  struct GNUNET_TIME_Absolute refund_deadline;

  /**
   * How much time does the merchant have to execute the wire transfer?
   * This time is advisory for aggregating transactions, not a hard
   * constraint (as the merchant can theoretically pick any time,
   * including one in the past).
   */
  struct GNUNET_TIME_Absolute wire_deadline;

  /**
   * Fraction of the coin's remaining value to be deposited, including
   * depositing fee (if any).  The coin is identified by @e coin_pub.
   */
  struct TALER_Amount amount_with_fee;

  /**
   * Depositing fee.
   */
  struct TALER_Amount deposit_fee;

};


/**
 * @brief Specification for a /refund operation.  The combination of
 * the coin's public key, the merchant's public key and the
 * transaction ID must be unique.  While a coin can (theoretically) be
 * deposited at the same merchant twice (with partial spending), the
 * merchant must either use a different public key or a different
 * transaction ID for the two transactions.  The same goes for
 * refunds, hence we also have a "rtransaction" ID which is disjoint
 * from the transaction ID.  The same coin must not be used twice at
 * the same merchant for the same transaction or rtransaction ID.
 */
struct TALER_EXCHANGEDB_Refund
{
  /**
   * Information about the coin that is being refunded.
   */
  struct TALER_CoinPublicInfo coin;

  /**
   * Public key of the merchant.
   */
  struct TALER_MerchantPublicKeyP merchant_pub;

  /**
   * Signature from the merchant affirming the refund.
   */
  struct TALER_MerchantSignatureP merchant_sig;

  /**
   * Hash over the proposal data between merchant and customer
   * (remains unknown to the Exchange).
   */
  struct GNUNET_HashCode h_contract_terms;

  /**
   * Merchant-generated REFUND transaction ID to detect duplicate
   * refunds.
   */
  uint64_t rtransaction_id;

  /**
   * Fraction of the original deposit's value to be refunded, including
   * refund fee (if any).  The coin is identified by @e coin_pub.
   */
  struct TALER_Amount refund_amount;

  /**
   * Refund fee to be covered by the customer.
   */
  struct TALER_Amount refund_fee;

};


/**
 * @brief Specification for coin in a /refresh/melt operation.
 */
struct TALER_EXCHANGEDB_RefreshSession
{
  /**
   * Information about the coin that is being melted.
   */
  struct TALER_CoinPublicInfo coin;

  /**
   * Signature over the melting operation.
   */
  struct TALER_CoinSpendSignatureP coin_sig;

  /**
   * Refresh commitment this coin is melted into.
   */
  struct TALER_RefreshCommitmentP rc;

  /**
   * How much value is being melted?  This amount includes the fees,
   * so the final amount contributed to the melt is this value minus
   * the fee for melting the coin.  We include the fee in what is
   * being signed so that we can verify a reserve's remaining total
   * balance without needing to access the respective denomination key
   * information each time.
   */
  struct TALER_Amount amount_with_fee;

  /**
   * Index (smaller #TALER_CNC_KAPPA) which the exchange has chosen to not
   * have revealed during cut and choose.
   */
  uint32_t noreveal_index;

};


/**
 * Information about a /refresh/melt operation in the transaction history.
 */
struct TALER_EXCHANGEDB_RefreshMelt
{

  /**
   * Overall session data.
   */
  struct TALER_EXCHANGEDB_RefreshSession session;

  /**
   * Melt fee the exchange charged.
   */
  struct TALER_Amount melt_fee;

};


/**
 * @brief Linked list of refresh information linked to a coin.
 */
struct TALER_EXCHANGEDB_LinkDataList
{
  /**
   * Information is stored in a NULL-terminated linked list.
   */
  struct TALER_EXCHANGEDB_LinkDataList *next;

  /**
   * Denomination public key, determines the value of the coin.
   */
  struct TALER_DenominationPublicKey denom_pub;

  /**
   * Signature over the blinded envelope.
   */
  struct TALER_DenominationSignature ev_sig;

  /**
   * Signature of the original coin being refreshed over the
   * link data, of type #TALER_SIGNATURE_WALLET_COIN_LINK
   */
  struct TALER_CoinSpendSignatureP orig_coin_link_sig;
};


/**
 * @brief Enumeration to classify the different types of transactions
 * that can be done with a coin.
 */
enum TALER_EXCHANGEDB_TransactionType {

  /**
   * /deposit operation.
   */
  TALER_EXCHANGEDB_TT_DEPOSIT = 0,

  /**
   * /refresh/melt operation.
   */
  TALER_EXCHANGEDB_TT_REFRESH_MELT = 1,

  /**
   * /refund operation.
   */
  TALER_EXCHANGEDB_TT_REFUND = 2,

  /**
   * /payback-refresh operation (on the old coin, adding to the old coin's value)
   */
  TALER_EXCHANGEDB_TT_OLD_COIN_PAYBACK = 3,

  /**
   * /payback operation.
   */
  TALER_EXCHANGEDB_TT_PAYBACK = 4,

  /**
   * /payback-refresh operation (on the new coin, eliminating its value)
   */
  TALER_EXCHANGEDB_TT_PAYBACK_REFRESH = 5

};


/**
 * @brief List of transactions we performed for a particular coin.
 */
struct TALER_EXCHANGEDB_TransactionList
{

  /**
   * Next pointer in the NULL-terminated linked list.
   */
  struct TALER_EXCHANGEDB_TransactionList *next;

  /**
   * Type of the transaction, determines what is stored in @e details.
   */
  enum TALER_EXCHANGEDB_TransactionType type;

  /**
   * Details about the transaction, depending on @e type.
   */
  union
  {

    /**
     * Details if transaction was a /deposit operation.
     * (#TALER_EXCHANGEDB_TT_DEPOSIT)
     */
    struct TALER_EXCHANGEDB_Deposit *deposit;

    /**
     * Details if transaction was a /refresh/melt operation.
     * (#TALER_EXCHANGEDB_TT_REFRESH_MELT)
     */
    struct TALER_EXCHANGEDB_RefreshMelt *melt;

    /**
     * Details if transaction was a /refund operation.
     * (#TALER_EXCHANGEDB_TT_REFUND)
     */
    struct TALER_EXCHANGEDB_Refund *refund;

    /**
     * Details if transaction was a /payback-refund operation where
     * this coin was the OLD coin.
     * (#TALER_EXCHANGEDB_TT_OLD_COIN_PAYBACK).
     */
    struct TALER_EXCHANGEDB_PaybackRefresh *old_coin_payback;

    /**
     * Details if transaction was a /payback operation.
     * (#TALER_EXCHANGEDB_TT_PAYBACK)
     */
    struct TALER_EXCHANGEDB_Payback *payback;

    /**
     * Details if transaction was a /payback-refund operation where
     * this coin was the REFRESHED coin.
     * (#TALER_EXCHANGEDB_TT_PAYBACK_REFRESH)
     */
    struct TALER_EXCHANGEDB_PaybackRefresh *payback_refresh;

  } details;

};


/**
 * @brief Handle for a database session (per-thread, for transactions).
 */
struct TALER_EXCHANGEDB_Session;


/**
 * Function called with details about deposits that have been made,
 * with the goal of executing the corresponding wire transaction.
 *
 * @param cls closure
 * @param rowid unique ID for the deposit in our DB, used for marking
 *              it as 'tiny' or 'done'
 * @param merchant_pub public key of the merchant
 * @param coin_pub public key of the coin
 * @param amount_with_fee amount that was deposited including fee
 * @param deposit_fee amount the exchange gets to keep as transaction fees
 * @param h_contract_terms hash of the proposal data known to merchant and customer
 * @param wire_deadline by which the merchant adviced that he would like the
 *        wire transfer to be executed
 * @param receiver_wire_account wire details for the merchant, includes
 *        'url' in payto://-format; NULL from iterate_matching_deposits()
 * @return transaction status code, #GNUNET_DB_STATUS_SUCCESS_ONE_RESULT to continue to iterate
 */
typedef enum GNUNET_DB_QueryStatus
(*TALER_EXCHANGEDB_DepositIterator)(void *cls,
                                    uint64_t rowid,
                                    const struct TALER_MerchantPublicKeyP *merchant_pub,
                                    const struct TALER_CoinSpendPublicKeyP *coin_pub,
                                    const struct TALER_Amount *amount_with_fee,
                                    const struct TALER_Amount *deposit_fee,
                                    const struct GNUNET_HashCode *h_contract_terms,
                                    struct GNUNET_TIME_Absolute wire_deadline,
                                    const json_t *receiver_wire_account);


/**
 * Callback with data about a prepared wire transfer.
 *
 * @param cls closure
 * @param rowid row identifier used to mark prepared transaction as done
 * @param wire_method which wire method is this preparation data for
 * @param buf transaction data that was persisted, NULL on error
 * @param buf_size number of bytes in @a buf, 0 on error
 */
typedef void
(*TALER_EXCHANGEDB_WirePreparationIterator) (void *cls,
                                             uint64_t rowid,
                                             const char *wire_method,
                                             const char *buf,
                                             size_t buf_size);


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
 * @param receiver_wire_account wire details for the merchant including 'url' in payto://-format;
 *        NULL from iterate_matching_deposits()
 * @param done flag set if the deposit was already executed (or not)
 * @return #GNUNET_OK to continue to iterate, #GNUNET_SYSERR to stop
 */
typedef int
(*TALER_EXCHANGEDB_DepositCallback)(void *cls,
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
                                    int done);


/**
 * Callback used to process data of a merchant under KYC monitoring.
 *
 * @param cls closure
 * @param payto_url payto URL of this particular
 *        merchant (bank account)
 * @param general_id general identificator valid
 *        at the KYC-caring institution
 * @param kyc_checked status of KYC check:
 *        if GNUNET_OK, the merchant was checked at least once,
 *        never otherwise.
 * @param merchant_serial_id serial ID identifying
 *        this merchant (bank account) into the database system;
 *        it helps making more efficient queries than the payto
 *        URL.
 */
typedef void
(*TALER_EXCHANGEDB_KycStatusCallback)(void *cls,
                                      const char *payto_url,
                                      const char *general_id,
                                      uint8_t kyc_checked,
                                      uint64_t merchant_serial_id);

/**
 * Function called with details about coins that were melted,
 * with the goal of auditing the refresh's execution.
 *
 * @param cls closure
 * @param rowid unique serial ID for the refresh session in our DB
 * @param denom_pub denomination public key of @a coin_pub
 * @param coin_pub public key of the coin
 * @param coin_sig signature from the coin
 * @param amount_with_fee amount that was deposited including fee
 * @param noreveal_index which index was picked by the exchange in cut-and-choose
 * @param rc what is the commitment
 * @return #GNUNET_OK to continue to iterate, #GNUNET_SYSERR to stop
 */
typedef int
(*TALER_EXCHANGEDB_RefreshSessionCallback)(void *cls,
                                           uint64_t rowid,
                                           const struct TALER_DenominationPublicKey *denom_pub,
                                           const struct TALER_CoinSpendPublicKeyP *coin_pub,
                                           const struct TALER_CoinSpendSignatureP *coin_sig,
                                           const struct TALER_Amount *amount_with_fee,
                                           uint32_t noreveal_index,
                                           const struct TALER_RefreshCommitmentP *rc);


/**
 * Callback invoked with information about refunds applicable
 * to a particular coin.
 *
 * @param cls closure
 * @param merchant_pub public key of merchant who authorized refund
 * @param merchant_sig signature of merchant authorizing refund
 * @param h_contract hash of contract being refunded
 * @param rtransaction_id refund transaction ID
 * @param amount_with_fee amount being refunded
 * @param refund_fee fee the exchange keeps for the refund processing
 * @return #GNUNET_OK to continue to iterate, #GNUNET_SYSERR to stop
 */
typedef int
(*TALER_EXCHANGEDB_RefundCoinCallback)(void *cls,
				       const struct TALER_MerchantPublicKeyP *merchant_pub,
				       const struct TALER_MerchantSignatureP *merchant_sig,
				       const struct GNUNET_HashCode *h_contract,
				       uint64_t rtransaction_id,
				       const struct TALER_Amount *amount_with_fee,
				       const struct TALER_Amount *refund_fee);


/**
 * Information about a coin that was revealed to the exchange
 * during /refresh/reveal.
 */
struct TALER_EXCHANGEDB_RefreshRevealedCoin
{
  /**
   * Public denomination key of the coin.
   */
  struct TALER_DenominationPublicKey denom_pub;

  /**
   * Signature of the original coin being refreshed over the
   * link data, of type #TALER_SIGNATURE_WALLET_COIN_LINK
   */
  struct TALER_CoinSpendSignatureP orig_coin_link_sig;

  /**
   * Blinded message to be signed (in envelope), with @e coin_env_size bytes.
   */
  char *coin_ev;

  /**
   * Number of bytes in @e coin_ev.
   */
  size_t coin_ev_size;

  /**
   * Signature generated by the exchange over the coin (in blinded format).
   */
  struct TALER_DenominationSignature coin_sig;
};


/**
 * Function called with information about a refresh order.
 *
 * @param cls closure
 * @param rowid unique serial ID for the row in our database
 * @param num_newcoins size of the @a rrcs array
 * @param rrcs array of @a num_newcoins information about coins to be created
 * @param num_tprivs number of entries in @a tprivs, should be #TALER_CNC_KAPPA - 1
 * @param tprivs array of @e num_tprivs transfer private keys
 * @param tp transfer public key information
 */
typedef void
(*TALER_EXCHANGEDB_RefreshCallback)(void *cls,
                                    uint32_t num_newcoins,
                                    const struct TALER_EXCHANGEDB_RefreshRevealedCoin *rrcs,
                                    unsigned int num_tprivs,
                                    const struct TALER_TransferPrivateKeyP *tprivs,
                                    const struct TALER_TransferPublicKeyP *tp);


/**
 * Callback for handling a KYC timestamped event associated with
 * a certain customer (= merchant).
 *
 * @param cls closure
 * @param url "payto" URL associated with the customer
 * @param timeout last time when the KYC was issued to the customer.
 */
typedef void
(*TALER_EXCHANGEDB_KycCallback)(void *cls,
                                const char *url,
                                struct GNUNET_TIME_Absolute timeout);

/**
 * Function called with details about coins that were refunding,
 * with the goal of auditing the refund's execution.
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
typedef int
(*TALER_EXCHANGEDB_RefundCallback)(void *cls,
                                   uint64_t rowid,
                                   const struct TALER_DenominationPublicKey *denom_pub,
                                   const struct TALER_CoinSpendPublicKeyP *coin_pub,
                                   const struct TALER_MerchantPublicKeyP *merchant_pub,
                                   const struct TALER_MerchantSignatureP *merchant_sig,
                                   const struct GNUNET_HashCode *h_contract_terms,
                                   uint64_t rtransaction_id,
                                   const struct TALER_Amount *amount_with_fee);


/**
 * Function called with details about incoming wire transfers.
 *
 * @param cls closure
 * @param rowid unique serial ID for the refresh session in our DB
 * @param reserve_pub public key of the reserve (also the WTID)
 * @param credit amount that was received
 * @param sender_account_details information about the sender's bank account, in payto://-format
 * @param wire_reference unique identifier for the wire transfer (plugin-specific format)
 * @param wire_reference_size number of bytes in @a wire_reference
 * @param execution_date when did we receive the funds
 * @return #GNUNET_OK to continue to iterate, #GNUNET_SYSERR to stop
 */
typedef int
(*TALER_EXCHANGEDB_ReserveInCallback)(void *cls,
                                      uint64_t rowid,
                                      const struct TALER_ReservePublicKeyP *reserve_pub,
                                      const struct TALER_Amount *credit,
                                      const char *sender_account_details,
                                      const void *wire_reference,
                                      size_t wire_reference_size,
                                      struct GNUNET_TIME_Absolute execution_date);


/**
 * Function called with details about withdraw operations.
 *
 * @param cls closure
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
typedef int
(*TALER_EXCHANGEDB_WithdrawCallback)(void *cls,
                                     uint64_t rowid,
                                     const struct GNUNET_HashCode *h_blind_ev,
                                     const struct TALER_DenominationPublicKey *denom_pub,
                                     const struct TALER_DenominationSignature *denom_sig,
                                     const struct TALER_ReservePublicKeyP *reserve_pub,
                                     const struct TALER_ReserveSignatureP *reserve_sig,
                                     struct GNUNET_TIME_Absolute execution_date,
                                     const struct TALER_Amount *amount_with_fee);


/**
 * Function called with the session hashes and transfer secret
 * information for a given coin.
 *
 * @param cls closure
 * @param transfer_pub public transfer key for the session
 * @param ldl link data for @a transfer_pub
 */
typedef void
(*TALER_EXCHANGEDB_LinkDataCallback)(void *cls,
                                     const struct TALER_TransferPublicKeyP *transfer_pub,
                                     const struct TALER_EXCHANGEDB_LinkDataList *ldl);


/**
 * Function called with the results of the lookup of the
 * wire transfer identifier information.  Only called if
 * we are at least aware of the transaction existing.
 *
 * @param cls closure
 * @param wtid wire transfer identifier, NULL
 *         if the transaction was not yet done
 * @param coin_contribution how much did the coin we asked about
 *        contribute to the total transfer value? (deposit value including fee)
 * @param coin_fee how much did the exchange charge for the deposit fee
 * @param execution_time when was the transaction done, or
 *         when we expect it to be done (if @a wtid was NULL)
 */
typedef void
(*TALER_EXCHANGEDB_TrackTransactionCallback)(void *cls,
                                             const struct TALER_WireTransferIdentifierRawP *wtid,
                                             const struct TALER_Amount *coin_contribution,
                                             const struct TALER_Amount *coin_fee,
                                             struct GNUNET_TIME_Absolute execution_time);


/**
 * Function called with the results of the lookup of the
 * transaction data associated with a wire transfer identifier.
 *
 * @param cls closure
 * @param rowid which row in the table is the information from (for diagnostics)
 * @param merchant_pub public key of the merchant (should be same for all callbacks with the same @e cls)
 * @param h_wire hash of wire transfer details of the merchant (should be same for all callbacks with the same @e cls)
 * @param account_details which account did the transfer go to?
 * @param exec_time execution time of the wire transfer (should be same for all callbacks with the same @e cls)
 * @param h_contract_terms which proposal was this payment about
 * @param coin_pub which public key was this payment about
 * @param coin_value amount contributed by this coin in total (with fee)
 * @param coin_fee applicable fee for this coin
 */
typedef void
(*TALER_EXCHANGEDB_WireTransferDataCallback)(void *cls,
                                             uint64_t rowid,
                                             const struct TALER_MerchantPublicKeyP *merchant_pub,
                                             const struct GNUNET_HashCode *h_wire,
                                             const json_t *account_details,
                                             struct GNUNET_TIME_Absolute exec_time,
                                             const struct GNUNET_HashCode *h_contract_terms,
                                             const struct TALER_CoinSpendPublicKeyP *coin_pub,
                                             const struct TALER_Amount *coin_value,
                                             const struct TALER_Amount *coin_fee);


/**
 * Function called with the results of the lookup of the
 * wire transfer data of the exchange.
 *
 * @param cls closure
 * @param rowid identifier of the respective row in the database
 * @param date timestamp of the wire transfer (roughly)
 * @param wtid wire transfer subject
 * @param wire wire transfer details of the receiver, including "url" in payto://-format
 * @param amount amount that was wired
 * @return #GNUNET_OK to continue, #GNUNET_SYSERR to stop iteration
 */
typedef int
(*TALER_EXCHANGEDB_WireTransferOutCallback)(void *cls,
                                            uint64_t rowid,
                                            struct GNUNET_TIME_Absolute date,
                                            const struct TALER_WireTransferIdentifierRawP *wtid,
                                            const json_t *wire,
                                            const struct TALER_Amount *amount);


/**
 * Callback with data about a prepared wire transfer.
 *
 * @param cls closure
 * @param rowid row identifier used to mark prepared transaction as done
 * @param wire_method which wire method is this preparation data for
 * @param buf transaction data that was persisted, NULL on error
 * @param buf_size number of bytes in @a buf, 0 on error
 * @param finished did we complete the transfer yet?
 * @return #GNUNET_OK to continue, #GNUNET_SYSERR to stop iteration
 */
typedef int
(*TALER_EXCHANGEDB_WirePreparationCallback)(void *cls,
                                            uint64_t rowid,
                                            const char *wire_method,
                                            const char *buf,
                                            size_t buf_size,
                                            int finished);


/**
 * Function called about paybacks the exchange has to perform.
 *
 * @param cls closure
 * @param rowid row identifier used to uniquely identify the payback operation
 * @param timestamp when did we receive the payback request
 * @param amount how much should be added back to the reserve
 * @param reserve_pub public key of the reserve
 * @param coin public information about the coin
 * @param coin_sig signature with @e coin_pub of type #TALER_SIGNATURE_WALLET_COIN_PAYBACK
 * @param coin_blind blinding factor used to blind the coin
 * @return #GNUNET_OK to continue to iterate, #GNUNET_SYSERR to stop
 */
typedef int
(*TALER_EXCHANGEDB_PaybackCallback)(void *cls,
                                    uint64_t rowid,
                                    struct GNUNET_TIME_Absolute timestamp,
                                    const struct TALER_Amount *amount,
                                    const struct TALER_ReservePublicKeyP *reserve_pub,
                                    const struct TALER_CoinPublicInfo *coin,
                                    const struct TALER_DenominationPublicKey *denom_pub,
                                    const struct TALER_CoinSpendSignatureP *coin_sig,
                                    const struct TALER_DenominationBlindingKeyP *coin_blind);



/**
 * Function called about paybacks on refreshed coins the exchange has to
 * perform.
 *
 * @param cls closure
 * @param rowid row identifier used to uniquely identify the payback operation
 * @param timestamp when did we receive the payback request
 * @param amount how much should be added back to the reserve
 * @param old_coin_pub original coin that was refreshed to create @a coin
 * @param coin public information about the coin
 * @param coin_sig signature with @e coin_pub of type #TALER_SIGNATURE_WALLET_COIN_PAYBACK
 * @param coin_blind blinding factor used to blind the coin
 * @return #GNUNET_OK to continue to iterate, #GNUNET_SYSERR to stop
 */
typedef int
(*TALER_EXCHANGEDB_PaybackRefreshCallback)(void *cls,
                                           uint64_t rowid,
                                           struct GNUNET_TIME_Absolute timestamp,
                                           const struct TALER_Amount *amount,
                                           const struct TALER_CoinSpendPublicKeyP *old_coin_pub,
                                           const struct TALER_CoinPublicInfo *coin,
                                           const struct TALER_DenominationPublicKey *denom_pub,
                                           const struct TALER_CoinSpendSignatureP *coin_sig,
                                           const struct TALER_DenominationBlindingKeyP *coin_blind);




/**
 * Function called about reserve closing operations
 * the aggregator triggered.
 *
 * @param cls closure
 * @param rowid row identifier used to uniquely identify the reserve closing operation
 * @param execution_date when did we execute the close operation
 * @param amount_with_fee how much did we debit the reserve
 * @param closing_fee how much did we charge for closing the reserve
 * @param reserve_pub public key of the reserve
 * @param receiver_account where did we send the funds, in payto://-format
 * @param wtid identifier used for the wire transfer
 * @return #GNUNET_OK to continue to iterate, #GNUNET_SYSERR to stop
 */
typedef int
(*TALER_EXCHANGEDB_ReserveClosedCallback)(void *cls,
					  uint64_t rowid,
					  struct GNUNET_TIME_Absolute execution_date,
					  const struct TALER_Amount *amount_with_fee,
					  const struct TALER_Amount *closing_fee,
					  const struct TALER_ReservePublicKeyP *reserve_pub,
					  const char *receiver_account,
					  const struct TALER_WireTransferIdentifierRawP *wtid);


/**
 * Function called with details about expired reserves.
 *
 * @param cls closure
 * @param reserve_pub public key of the reserve
 * @param left amount left in the reserve
 * @param account_details information about the reserve's bank account, in payto://-format
 * @param expiration_date when did the reserve expire
 * @return transaction status code to pass on
 */
typedef enum GNUNET_DB_QueryStatus
(*TALER_EXCHANGEDB_ReserveExpiredCallback)(void *cls,
					   const struct TALER_ReservePublicKeyP *reserve_pub,
					   const struct TALER_Amount *left,
					   const char *account_details,
					   struct GNUNET_TIME_Absolute expiration_date);


/**
 * Function called with information justifying an aggregate payback.
 * (usually implemented by the auditor when verifying losses from paybacks).
 *
 * @param cls closure
 * @param rowid row identifier used to uniquely identify the payback operation
 * @param coin information about the coin
 * @param coin_sig signature of the coin of type #TALER_SIGNATURE_WALLET_COIN_PAYBACK
 * @param coin_blind blinding key of the coin
 * @param h_blind_ev blinded envelope, as calculated by the exchange
 * @param amount total amount to be paid back
 */
typedef void
(*TALER_EXCHANGEDB_PaybackJustificationCallback)(void *cls,
                                                 uint64_t rowid,
                                                 const struct TALER_CoinPublicInfo *coin,
                                                 const struct TALER_CoinSpendSignatureP *coin_sig,
                                                 const struct TALER_DenominationBlindingKeyP *coin_blind,
                                                 const struct GNUNET_HashCode *h_blinded_ev,
                                                 const struct TALER_Amount *amount);


/**
 * Function called on deposits that are past their due date
 * and have not yet seen a wire transfer.
 *
 * @param cls closure
 * @param rowid deposit table row of the coin's deposit
 * @param coin_pub public key of the coin
 * @param amount value of the deposit, including fee
 * @param wire where should the funds be wired, including 'url' in payto://-format
 * @param deadline what was the requested wire transfer deadline
 * @param tiny did the exchange defer this transfer because it is too small?
 * @param done did the exchange claim that it made a transfer?
 */
typedef void
(*TALER_EXCHANGEDB_WireMissingCallback)(void *cls,
					uint64_t rowid,
					const struct TALER_CoinSpendPublicKeyP *coin_pub,
					const struct TALER_Amount *amount,
					const json_t *wire,
					struct GNUNET_TIME_Absolute deadline,
					/* bool? */ int tiny,
					/* bool? */ int done);


/**
 * Function called with information about the exchange's denomination keys.
 *
 * @parma cls closure
 * @param denom_pub public key of the denomination
 * @param issue detailed information about the denomination (value, expiration times, fees)
 */
typedef void
(*TALER_EXCHANGEDB_DenominationInfoIterator)(void *cls,
                                             const struct TALER_DenominationPublicKey *denom_pub,
                                             const struct TALER_EXCHANGEDB_DenominationKeyInformationP *issue);


/**
 * @brief The plugin API, returned from the plugin's "init" function.
 * The argument given to "init" is simply a configuration handle.
 */
struct TALER_EXCHANGEDB_Plugin
{

  /**
   * Closure for all callbacks.
   */
  void *cls;

  /**
   * Name of the library which generated this plugin.  Set by the
   * plugin loader.
   */
  char *library_name;

  /**
   * Get the thread-local database-handle.
   * Connect to the db if the connection does not exist yet.
   *
   * @param cls the @e cls of this struct with the plugin-specific state
   * @param the database connection, or NULL on error
   */
  struct TALER_EXCHANGEDB_Session *
  (*get_session) (void *cls);


  /**
   * Drop the Taler tables.  This should only be used in testcases.
   *
   * @param cls the @e cls of this struct with the plugin-specific state
   * @return #GNUNET_OK upon success; #GNUNET_SYSERR upon failure
   */
  int
  (*drop_tables) (void *cls);


  /**
   * Create the necessary tables if they are not present
   *
   * @param cls the @e cls of this struct with the plugin-specific state
   * @return #GNUNET_OK upon success; #GNUNET_SYSERR upon failure
   */
  int
  (*create_tables) (void *cls);


  /**
   * Start a transaction.
   *
   * @param cls the @e cls of this struct with the plugin-specific state
   * @param session connection to use
   * @param name unique name identifying the transaction (for debugging),
   *             must point to a constant
   * @return #GNUNET_OK on success
   */
  int
  (*start) (void *cls,
            struct TALER_EXCHANGEDB_Session *session,
            const char *name);


  /**
   * Commit a transaction.
   *
   * @param cls the @e cls of this struct with the plugin-specific state
   * @param session connection to use
   * @return transaction status
   */
  enum GNUNET_DB_QueryStatus
  (*commit) (void *cls,
             struct TALER_EXCHANGEDB_Session *session);


  /**
   * Do a pre-flight check that we are not in an uncommitted transaction.
   * If we are, try to commit the previous transaction and output a warning.
   * Does not return anything, as we will continue regardless of the outcome.
   *
   * @param cls the `struct PostgresClosure` with the plugin-specific state
   * @param session the database connection
   */
  void
  (*preflight) (void *cls,
                struct TALER_EXCHANGEDB_Session *session);


  /**
   * Abort/rollback a transaction.
   *
   * @param cls the @e cls of this struct with the plugin-specific state
   * @param session connection to use
   */
  void
  (*rollback) (void *cls,
               struct TALER_EXCHANGEDB_Session *session);


  /**
   * Insert information about a denomination key and in particular
   * the properties (value, fees, expiration times) the coins signed
   * with this key have.
   *
   * @param cls the @e cls of this struct with the plugin-specific state
   * @param session connection to use
   * @param denom_pub the public key used for signing coins of this denomination
   * @param issue issuing information with value, fees and other info about the denomination
   * @return status of the query
   */
  enum GNUNET_DB_QueryStatus
  (*insert_denomination_info) (void *cls,
                               struct TALER_EXCHANGEDB_Session *session,
                               const struct TALER_DenominationPublicKey *denom_pub,
                               const struct TALER_EXCHANGEDB_DenominationKeyInformationP *issue);


  /**
   * Fetch information about a denomination key.
   *
   * @param cls the @e cls of this struct with the plugin-specific state
   * @param session connection to use
   * @param denom_pub_hash hash of the public key used for signing coins of this denomination
   * @param[out] issue set to issue information with value, fees and other info about the coin
   * @return transaction status code
   */
  enum GNUNET_DB_QueryStatus
  (*get_denomination_info) (void *cls,
                            struct TALER_EXCHANGEDB_Session *session,
                            const struct GNUNET_HashCode *denom_pub_hash,
                            struct TALER_EXCHANGEDB_DenominationKeyInformationP *issue);


  /**
   * Function called on every known denomination key.  Runs in its
   * own read-only transaction (hence no session provided).
   *
   * @param cls the @e cls of this struct with the plugin-specific state
   * @param cb function to call on each denomination key
   * @param cb_cls closure for @a cb
   * @return transaction status code
   */
  enum GNUNET_DB_QueryStatus
  (*iterate_denomination_info) (void *cls,
                                TALER_EXCHANGEDB_DenominationInfoIterator cb,
                                void *cb_cls);

  /**
   * Get the summary of a reserve.
   *
   * @param cls the @e cls of this struct with the plugin-specific state
   * @param db the database connection handle
   * @param[in,out] reserve the reserve data.  The public key of the reserve should be set
   *          in this structure; it is used to query the database.  The balance
   *          and expiration are then filled accordingly.
   * @return transaction status
   */
  enum GNUNET_DB_QueryStatus
  (*reserve_get) (void *cls,
                  struct TALER_EXCHANGEDB_Session *db,
                  struct TALER_EXCHANGEDB_Reserve *reserve);


  /**
   * Insert a incoming transaction into reserves.  New reserves are
   * also created through this function.  Note that this API call
   * starts (and stops) its own transaction scope (so the application
   * must not do so).
   *
   * @param cls the @e cls of this struct with the plugin-specific state
   * @param db the database connection handle
   * @param reserve_pub public key of the reserve
   * @param balance the amount that has to be added to the reserve
   * @param execution_time when was the amount added
   * @param sender_account_details information about the sender's bank account, in payto://-format
   * @param wire_reference unique reference identifying the wire transfer (binary blob)
   * @param wire_reference_size number of bytes in @a wire_reference
   * @return transaction status code
   */
  enum GNUNET_DB_QueryStatus
  (*reserves_in_insert) (void *cls,
                         struct TALER_EXCHANGEDB_Session *db,
                         const struct TALER_ReservePublicKeyP *reserve_pub,
                         const struct TALER_Amount *balance,
                         struct GNUNET_TIME_Absolute execution_time,
                         const char *sender_account_details,
                         const char *exchange_account_name,
                         const void *wire_reference,
                         size_t wire_reference_size);


  /**
   * Obtain the most recent @a wire_reference that was inserted via @e reserves_in_insert.
   *
   * @param cls the @e cls of this struct with the plugin-specific state
   * @param db the database connection handle
   * @param[out] wire_reference set to unique reference identifying the wire transfer (binary blob)
   * @param[out] wire_reference_size set to number of bytes in @a wire_reference
   * @return transaction status code
   */
  enum GNUNET_DB_QueryStatus
  (*get_latest_reserve_in_reference)(void *cls,
                                     struct TALER_EXCHANGEDB_Session *db,
                                     const char *exchange_account_name,
                                     void **wire_reference,
                                     size_t *wire_reference_size);


  /**
   * Locate the response for a /withdraw request under the
   * key of the hash of the blinded message.
   *
   * @param cls the @e cls of this struct with the plugin-specific state
   * @param session database connection to use
   * @param h_blind hash of the blinded coin to be signed (will match
   *                `h_coin_envelope` in the @a collectable to be returned)
   * @param collectable corresponding collectable coin (blind signature)
   *                    if a coin is found
   * @return statement execution status
   */
  enum GNUNET_DB_QueryStatus
  (*get_withdraw_info) (void *cls,
                        struct TALER_EXCHANGEDB_Session *session,
                        const struct GNUNET_HashCode *h_blind,
                        struct TALER_EXCHANGEDB_CollectableBlindcoin *collectable);


  /**
   * Store collectable bit coin under the corresponding
   * hash of the blinded message.
   *
   * @param cls the @e cls of this struct with the plugin-specific state
   * @param session database connection to use
   * @param collectable corresponding collectable coin (blind signature)
   *                    if a coin is found
   * @return statement execution status
   */
  enum GNUNET_DB_QueryStatus
  (*insert_withdraw_info) (void *cls,
                           struct TALER_EXCHANGEDB_Session *session,
                           const struct TALER_EXCHANGEDB_CollectableBlindcoin *collectable);


  /**
   * Get all of the transaction history associated with the specified
   * reserve.
   *
   * @param cls the @e cls of this struct with the plugin-specific state
   * @param session connection to use
   * @param reserve_pub public key of the reserve
   * @param[out] rhp set to known transaction history (NULL if reserve is unknown)
   * @return transaction status
   */
  enum GNUNET_DB_QueryStatus
  (*get_reserve_history) (void *cls,
                          struct TALER_EXCHANGEDB_Session *session,
                          const struct TALER_ReservePublicKeyP *reserve_pub,
			  struct TALER_EXCHANGEDB_ReserveHistory **rhp);


  /**
   * Free memory associated with the given reserve history.
   *
   * @param cls the @e cls of this struct with the plugin-specific state
   * @param rh history to free.
   */
  void
  (*free_reserve_history) (void *cls,
                           struct TALER_EXCHANGEDB_ReserveHistory *rh);


  /**
   * Count the number of known coins by denomination.
   *
   * @param cls database connection plugin state
   * @param session database session
   * @param denom_pub_hash denomination to count by
   * @return number of coins if non-negative, otherwise an `enum GNUNET_DB_QueryStatus`
   */
  long long
  (*count_known_coins) (void *cls,
                        struct TALER_EXCHANGEDB_Session *session,
                        const struct GNUNET_HashCode *denom_pub_hash);


  /**
   * Make sure the given @a coin is known to the database.
   *
   * @param cls database connection plugin state
   * @param session database session
   * @param coin the coin that must be made known
   * @return database transaction status, non-negative on success
   */
  enum GNUNET_DB_QueryStatus
  (*ensure_coin_known) (void *cls,
                        struct TALER_EXCHANGEDB_Session *session,
                        const struct TALER_CoinPublicInfo *coin);


  /**
   * Retrieve information about the given @a coin from the database.
   *
   * @param cls database connection plugin state
   * @param session database session
   * @param coin the coin that must be made known
   * @return database transaction status, non-negative on success
   */
  enum GNUNET_DB_QueryStatus
  (*get_known_coin) (void *cls,
                     struct TALER_EXCHANGEDB_Session *session,
                     const struct TALER_CoinSpendPublicKeyP *coin_pub,
                     struct TALER_CoinPublicInfo *coin_info);

  /**
   * Check if we have the specified deposit already in the database.
   *
   * @param cls the @e cls of this struct with the plugin-specific state
   * @param session database connection
   * @param deposit deposit to search for
   * @param check_extras wether to check extra fields or not
   * @return 1 if we know this operation,
   *         0 if this exact deposit is unknown to us,
   *         otherwise transaction error status
   */
  enum GNUNET_DB_QueryStatus
  (*have_deposit) (void *cls,
                   struct TALER_EXCHANGEDB_Session *session,
                   const struct TALER_EXCHANGEDB_Deposit *deposit,
                   int check_extras);


  /**
   * Insert information about deposited coin into the database.
   *
   * @param cls the @e cls of this struct with the plugin-specific state
   * @param session connection to the database
   * @param deposit deposit information to store
   * @return query result status
   */
  enum GNUNET_DB_QueryStatus
  (*insert_deposit) (void *cls,
                     struct TALER_EXCHANGEDB_Session *session,
                     const struct TALER_EXCHANGEDB_Deposit *deposit);


  /**
   * Insert information about refunded coin into the database.
   *
   * @param cls the @e cls of this struct with the plugin-specific state
   * @param session connection to the database
   * @param refund refund information to store
   * @return query result status
   */
  enum GNUNET_DB_QueryStatus
  (*insert_refund) (void *cls,
                    struct TALER_EXCHANGEDB_Session *session,
                    const struct TALER_EXCHANGEDB_Refund *refund);

  /**
   * Select refunds by @a coin_pub.
   *
   * @param cls closure of plugin
   * @param session database handle to use
   * @param coin_pub coin to get refunds for
   * @param cb function to call for each refund found
   * @param cb_cls closure for @a cb
   * @return query result status
   */
  enum GNUNET_DB_QueryStatus
  (*select_refunds_by_coin)(void *cls,
			    struct TALER_EXCHANGEDB_Session *session,
			    const struct TALER_CoinSpendPublicKeyP *coin_pub,
			    TALER_EXCHANGEDB_RefundCoinCallback cb,
			    void *cb_cls);


  /**
   * Mark a deposit as tiny, thereby declaring that it cannot be
   * executed by itself and should no longer be returned by
   * @e iterate_ready_deposits()
   *
   * @param cls the @e cls of this struct with the plugin-specific state
   * @param session connection to the database
   * @param deposit_rowid identifies the deposit row to modify
   * @return query result status
   */
  enum GNUNET_DB_QueryStatus
  (*mark_deposit_tiny) (void *cls,
                        struct TALER_EXCHANGEDB_Session *session,
                        uint64_t rowid);


  /**
   * Test if a deposit was marked as done, thereby declaring that it
   * cannot be refunded anymore.
   *
   * @param cls the @e cls of this struct with the plugin-specific state
   * @param session connection to the database
   * @param deposit the deposit to check
   * @return #GNUNET_DB_STATUS_SUCCESS_ONE_RESULT if is is marked done,
   *         #GNUNET_DB_STATUS_SUCCESS_NO_RESULTS if not,
   *         otherwise transaction error status (incl. deposit unknown)
   */
  enum GNUNET_DB_QueryStatus
  (*test_deposit_done) (void *cls,
                        struct TALER_EXCHANGEDB_Session *session,
                        const struct TALER_EXCHANGEDB_Deposit *deposit);


  /**
   * Mark a deposit as done, thereby declaring that it cannot be
   * executed at all anymore, and should no longer be returned by
   * @e iterate_ready_deposits() or @e iterate_matching_deposits().
   *
   * @param cls the @e cls of this struct with the plugin-specific state
   * @param session connection to the database
   * @param deposit_rowid identifies the deposit row to modify
   * @return query result status
   */
  enum GNUNET_DB_QueryStatus
  (*mark_deposit_done) (void *cls,
                        struct TALER_EXCHANGEDB_Session *session,
                        uint64_t rowid);


  /**
   * Obtain information about deposits that are ready to be executed.
   * Such deposits must not be marked as "tiny" or "done", and the
   * execution time and refund deadlines must both be in the past.
   *
   * @param cls the @e cls of this struct with the plugin-specific state
   * @param session connection to the database
   * @param deposit_cb function to call for ONE such deposit
   * @param deposit_cb_cls closure for @a deposit_cb
   * @return transaction status code
   */
  enum GNUNET_DB_QueryStatus
  (*get_ready_deposit) (void *cls,
                        struct TALER_EXCHANGEDB_Session *session,
                        TALER_EXCHANGEDB_DepositIterator deposit_cb,
                        void *deposit_cb_cls);


/**
 * Maximum number of results we return from iterate_matching_deposits().
 *
 * Limit on the number of transactions we aggregate at once.  Note
 * that the limit must be big enough to ensure that when transactions
 * of the smallest possible unit are aggregated, they do surpass the
 * "tiny" threshold beyond which we never trigger a wire transaction!
 */
#define TALER_EXCHANGEDB_MATCHING_DEPOSITS_LIMIT 10000
#define TALER_EXCHANGEDB_MATCHING_DEPOSITS_LIMIT_STR "10000"

  /**
   * Obtain information about other pending deposits for the same
   * destination.  Those deposits must not already be "done".
   *
   * @param cls the @e cls of this struct with the plugin-specific state
   * @param session connection to the database
   * @param h_wire destination of the wire transfer
   * @param merchant_pub public key of the merchant
   * @param deposit_cb function to call for each deposit
   * @param deposit_cb_cls closure for @a deposit_cb
   * @param limit maximum number of matching deposits to return; should
   *        be #TALER_EXCHANGEDB_MATCHING_DEPOSITS_LIMIT, larger values
   *        are not supported, smaller values would be inefficient.
   * @return number of rows processed, 0 if none exist,
   *         transaction status code on error
   */
  enum GNUNET_DB_QueryStatus
  (*iterate_matching_deposits) (void *cls,
                                struct TALER_EXCHANGEDB_Session *session,
                                const struct GNUNET_HashCode *h_wire,
                                const struct TALER_MerchantPublicKeyP *merchant_pub,
                                TALER_EXCHANGEDB_DepositIterator deposit_cb,
                                void *deposit_cb_cls,
                                uint32_t limit);


  /**
   * Store new refresh melt commitment data.
   *
   * @param cls the @e cls of this struct with the plugin-specific state
   * @param session database handle to use
   * @param refresh_session operational data to store
   * @return query status for the transaction
   */
  enum GNUNET_DB_QueryStatus
  (*insert_melt) (void *cls,
                  struct TALER_EXCHANGEDB_Session *session,
                  const struct TALER_EXCHANGEDB_RefreshSession *refresh_session);


  /**
   * Lookup refresh melt commitment data under the given @a rc.
   *
   * @param cls the @e cls of this struct with the plugin-specific state
   * @param session database handle to use
   * @param rc commitment to use for the lookup
   * @param[out] refresh_melt where to store the result
   * @return transaction status
   */
  enum GNUNET_DB_QueryStatus
  (*get_melt) (void *cls,
               struct TALER_EXCHANGEDB_Session *session,
               const struct TALER_RefreshCommitmentP *rc,
               struct TALER_EXCHANGEDB_RefreshMelt *refresh_melt);


  /**
   * Lookup noreveal index of a previous melt operation under the given
   * @a rc.
   *
   * @param cls the `struct PostgresClosure` with the plugin-specific state
   * @param session database handle to use
   * @param rc commitment hash to use to locate the operation
   * @param[out] refresh_melt where to store the result
   * @return transaction status
   */
  enum GNUNET_DB_QueryStatus
  (*get_melt_index) (void *cls,
                     struct TALER_EXCHANGEDB_Session *session,
                     const struct TALER_RefreshCommitmentP *rc,
                     uint32_t *noreveal_index);


  /**
   * Store in the database which coin(s) the wallet wanted to create
   * in a given refresh operation and all of the other information
   * we learned or created in the /refresh/reveal step.
   *
   * @param cls the @e cls of this struct with the plugin-specific state
   * @param session database connection
   * @param rc identify commitment and thus refresh operation
   * @param num_rrcs_newcoins number of coins to generate, size of the
   *            @a rrcs array
   * @param rrcs information about the new coins
   * @param num_tprivs number of entries in @a tprivs, should be #TALER_CNC_KAPPA - 1
   * @param tprivs transfer private keys to store
   * @param tp public key to store
   * @return query status for the transaction
   */
  enum GNUNET_DB_QueryStatus
  (*insert_refresh_reveal) (void *cls,
                            struct TALER_EXCHANGEDB_Session *session,
                            const struct TALER_RefreshCommitmentP *rc,
                            uint32_t num_rrcs,
                            const struct TALER_EXCHANGEDB_RefreshRevealedCoin *rrcs,
                            unsigned int num_tprivs,
                            const struct TALER_TransferPrivateKeyP *tprivs,
                            const struct TALER_TransferPublicKeyP *tp);


  /**
   * Lookup in the database for the @a num_newcoins coins that we
   * created in the given refresh operation.
   *
   * @param cls the @e cls of this struct with the plugin-specific state
   * @param session database connection
   * @param rc identify commitment and thus refresh operation
   * @param cb function to call with the results
   * @param cb_cls closure for @a cb
   * @return transaction status
   */
  enum GNUNET_DB_QueryStatus
  (*get_refresh_reveal) (void *cls,
                         struct TALER_EXCHANGEDB_Session *session,
                         const struct TALER_RefreshCommitmentP *rc,
                         TALER_EXCHANGEDB_RefreshCallback cb,
                         void *cb_cls);


  /**
   * Obtain shared secret and transfer public key from the public key of
   * the coin.  This information and the link information returned by
   * @e get_link_data_list() enable the owner of an old coin to determine
   * the private keys of the new coins after the melt.
   *
   * @param cls the @e cls of this struct with the plugin-specific state
   * @param session database connection
   * @param coin_pub public key of the coin
   * @param ldc function to call for each session the coin was melted into
   * @param ldc_cls closure for @a tdc
   * @return statement execution status
   */
  enum GNUNET_DB_QueryStatus
  (*get_link_data) (void *cls,
                    struct TALER_EXCHANGEDB_Session *session,
                    const struct TALER_CoinSpendPublicKeyP *coin_pub,
                    TALER_EXCHANGEDB_LinkDataCallback ldc,
                    void *tdc_cls);


  /**
   * Compile a list of all (historic) transactions performed
   * with the given coin (/refresh/melt and /deposit operations).
   *
   * @param cls the @e cls of this struct with the plugin-specific state
   * @param session database connection
   * @param coin_pub coin to investigate
   * @param include_payback include payback transactions of the coin?
   * @param[out] tlp set to list of transactions, NULL if coin is fresh
   * @return database transaction status
   */
  enum GNUNET_DB_QueryStatus
  (*get_coin_transactions) (void *cls,
                            struct TALER_EXCHANGEDB_Session *session,
                            const struct TALER_CoinSpendPublicKeyP *coin_pub,
                            int include_payback,
			    struct TALER_EXCHANGEDB_TransactionList **tlp);


  /**
   * Free linked list of transactions.
   *
   * @param cls the @e cls of this struct with the plugin-specific state
   * @param list list to free
   */
  void
  (*free_coin_transaction_list) (void *cls,
                                 struct TALER_EXCHANGEDB_TransactionList *list);


  /**
   * Lookup the list of Taler transactions that was aggregated
   * into a wire transfer by the respective @a raw_wtid.
   *
   * @param cls the @e cls of this struct with the plugin-specific state
   * @param session database connection
   * @param wtid the raw wire transfer identifier we used
   * @param cb function to call on each transaction found
   * @param cb_cls closure for @a cb
   * @return query status of the transaction
   */
  enum GNUNET_DB_QueryStatus
  (*lookup_wire_transfer) (void *cls,
                           struct TALER_EXCHANGEDB_Session *session,
                           const struct TALER_WireTransferIdentifierRawP *wtid,
                           TALER_EXCHANGEDB_WireTransferDataCallback cb,
                           void *cb_cls);


  /**
   * Try to find the wire transfer details for a deposit operation.
   * If we did not execute the deposit yet, return when it is supposed
   * to be executed.
   *
   * @param cls closure
   * @param session database connection
   * @param h_contract_terms hash of the proposal data
   * @param h_wire hash of merchant wire details
   * @param coin_pub public key of deposited coin
   * @param merchant_pub merchant public key
   * @param cb function to call with the result
   * @param cb_cls closure to pass to @a cb
   * @return transaction status code
   */
  enum GNUNET_DB_QueryStatus
  (*wire_lookup_deposit_wtid)(void *cls,
                              struct TALER_EXCHANGEDB_Session *session,
			      const struct GNUNET_HashCode *h_contract_terms,
			      const struct GNUNET_HashCode *h_wire,
			      const struct TALER_CoinSpendPublicKeyP *coin_pub,
			      const struct TALER_MerchantPublicKeyP *merchant_pub,
			      TALER_EXCHANGEDB_TrackTransactionCallback cb,
			      void *cb_cls);


  /**
   * Function called to insert aggregation information into the DB.
   *
   * @param cls closure
   * @param session database connection
   * @param wtid the raw wire transfer identifier we used
   * @param deposit_serial_id row in the deposits table for which this is aggregation data
   * @return transaction status code
   */
  enum GNUNET_DB_QueryStatus
  (*insert_aggregation_tracking)(void *cls,
                                 struct TALER_EXCHANGEDB_Session *session,
                                 const struct TALER_WireTransferIdentifierRawP *wtid,
                                 unsigned long long deposit_serial_id);


  /**
   * Insert wire transfer fee into database.
   *
   * @param cls closure
   * @param session database connection
   * @param wire_method which wire method is the fee about?
   * @param start_date when does the fee go into effect
   * @param end_date when does the fee end being valid
   * @param wire_fee how high is the wire transfer fee
   * @param closing_fee how high is the closing fee
   * @param master_sig signature over the above by the exchange master key
   * @return transaction status code
   */
  enum GNUNET_DB_QueryStatus
  (*insert_wire_fee)(void *cls,
                     struct TALER_EXCHANGEDB_Session *session,
                     const char *wire_method,
                     struct GNUNET_TIME_Absolute start_date,
                     struct GNUNET_TIME_Absolute end_date,
                     const struct TALER_Amount *wire_fee,
                     const struct TALER_Amount *closing_fee,
                     const struct TALER_MasterSignatureP *master_sig);


  /**
   * Obtain wire fee from database.
   *
   * @param cls closure
   * @param session database connection
   * @param type type of wire transfer the fee applies for
   * @param date for which date do we want the fee?
   * @param[out] start_date when does the fee go into effect
   * @param[out] end_date when does the fee end being valid
   * @param[out] wire_fee how high is the wire transfer fee
   * @param[out] closing_fee how high is the closing fee
   * @param[out] master_sig signature over the above by the exchange master key
   * @return query status of the transaction
   */
  enum GNUNET_DB_QueryStatus
  (*get_wire_fee) (void *cls,
                   struct TALER_EXCHANGEDB_Session *session,
                   const char *type,
                   struct GNUNET_TIME_Absolute date,
                   struct GNUNET_TIME_Absolute *start_date,
                   struct GNUNET_TIME_Absolute *end_date,
                   struct TALER_Amount *wire_fee,
		   struct TALER_Amount *closing_fee,
                   struct TALER_MasterSignatureP *master_sig);


  /**
   * Obtain information about expired reserves and their
   * remaining balances.
   *
   * @param cls closure of the plugin
   * @param session database connection
   * @param now timestamp based on which we decide expiration
   * @param rec function to call on expired reserves
   * @param rec_cls closure for @a rec
   * @return transaction status
   */
  enum GNUNET_DB_QueryStatus
  (*get_expired_reserves)(void *cls,
			  struct TALER_EXCHANGEDB_Session *session,
			  struct GNUNET_TIME_Absolute now,
			  TALER_EXCHANGEDB_ReserveExpiredCallback rec,
			  void *rec_cls);


  /**
   * Insert reserve close operation into database.
   *
   * @param cls closure
   * @param session database connection
   * @param reserve_pub which reserve is this about?
   * @param execution_date when did we perform the transfer?
   * @param receiver_account to which account do we transfer, in payto://-format
   * @param wtid identifier for the wire transfer
   * @param amount_with_fee amount we charged to the reserve
   * @param closing_fee how high is the closing fee
   * @return transaction status code
   */
  enum GNUNET_DB_QueryStatus
  (*insert_reserve_closed)(void *cls,
			   struct TALER_EXCHANGEDB_Session *session,
			   const struct TALER_ReservePublicKeyP *reserve_pub,
			   struct GNUNET_TIME_Absolute execution_date,
			   const char *receiver_account,
			   const struct TALER_WireTransferIdentifierRawP *wtid,
			   const struct TALER_Amount *amount_with_fee,
			   const struct TALER_Amount *closing_fee);


  /**
   * Function called to insert wire transfer commit data into the DB.
   *
   * @param cls closure
   * @param session database connection
   * @param type type of the wire transfer (i.e. "iban")
   * @param buf buffer with wire transfer preparation data
   * @param buf_size number of bytes in @a buf
   * @return query status code
   */
  enum GNUNET_DB_QueryStatus
  (*wire_prepare_data_insert)(void *cls,
                              struct TALER_EXCHANGEDB_Session *session,
                              const char *type,
                              const char *buf,
                              size_t buf_size);


  /**
   * Function called to mark wire transfer commit data as finished.
   *
   * @param cls closure
   * @param session database connection
   * @param rowid which entry to mark as finished
   * @return transaction status code
   */
  enum GNUNET_DB_QueryStatus
  (*wire_prepare_data_mark_finished)(void *cls,
                                     struct TALER_EXCHANGEDB_Session *session,
                                     uint64_t rowid);


  /**
   * Function called to get an unfinished wire transfer
   * preparation data. Fetches at most one item.
   *
   * @param cls closure
   * @param session database connection
   * @param cb function to call for ONE unfinished item
   * @param cb_cls closure for @a cb
   * @return transaction status code
   */
  enum GNUNET_DB_QueryStatus
  (*wire_prepare_data_get)(void *cls,
                           struct TALER_EXCHANGEDB_Session *session,
                           TALER_EXCHANGEDB_WirePreparationIterator cb,
                           void *cb_cls);


  /**
   * Start a transaction where we transiently violate the foreign
   * constraints on the "wire_out" table as we insert aggregations
   * and only add the wire transfer out at the end.
   *
   * @param cls the @e cls of this struct with the plugin-specific state
   * @param session connection to use
   * @return #GNUNET_OK on success
   */
  int
  (*start_deferred_wire_out) (void *cls,
                              struct TALER_EXCHANGEDB_Session *session);



  /**
   * Store information about an outgoing wire transfer that was executed.
   *
   * @param cls closure
   * @param session database connection
   * @param date time of the wire transfer
   * @param wtid subject of the wire transfer
   * @param wire_account details about the receiver account of the wire transfer,
   *        including 'url' in payto://-format
   * @param amount amount that was transmitted
   * @param exchange_account_section configuration section of the exchange specifying the
   *        exchange's bank account being used
   * @return transaction status code
   */
  enum GNUNET_DB_QueryStatus
  (*store_wire_transfer_out)(void *cls,
                             struct TALER_EXCHANGEDB_Session *session,
                             struct GNUNET_TIME_Absolute date,
                             const struct TALER_WireTransferIdentifierRawP *wtid,
                             const json_t *wire_account,
                             const char *exchange_account_section,
                             const struct TALER_Amount *amount);


  /**
   * Function called to perform "garbage collection" on the
   * database, expiring records we no longer require.
   *
   * FIXME: we probably need to consider here which entries the
   * auditor still needs to check, at least with respect to GC of the
   * prewire table (for denominations, we can assume that the auditor
   * runs long before the DK expire_legal time is hit).  Thus, this
   * function probably should take the "last_prewire_serial_id"
   * from the "auditor_progress" table as an extra argument (which
   * the user would then have to manually specify).
   *
   * @param cls closure
   * @return #GNUNET_OK on success,
   *         #GNUNET_SYSERR on DB errors
   */
  int
  (*gc) (void *cls);


  /**
   * Select deposits above @a serial_id in monotonically increasing
   * order.
   *
   * @param cls closure
   * @param session database connection
   * @param serial_id highest serial ID to exclude (select strictly larger)
   * @param cb function to call on each result
   * @param cb_cls closure for @a cb
   * @return transaction status code
   */
  enum GNUNET_DB_QueryStatus
  (*select_deposits_above_serial_id)(void *cls,
                                     struct TALER_EXCHANGEDB_Session *session,
                                     uint64_t serial_id,
                                     TALER_EXCHANGEDB_DepositCallback cb,
                                     void *cb_cls);

  /**
   * Select refresh sessions above @a serial_id in monotonically increasing
   * order.
   *
   * @param cls closure
   * @param session database connection
   * @param serial_id highest serial ID to exclude (select strictly larger)
   * @param cb function to call on each result
   * @param cb_cls closure for @a cb
   * @return transaction status code
   */
  enum GNUNET_DB_QueryStatus
  (*select_refreshs_above_serial_id)(void *cls,
                                     struct TALER_EXCHANGEDB_Session *session,
                                     uint64_t serial_id,
                                     TALER_EXCHANGEDB_RefreshSessionCallback cb,
                                     void *cb_cls);


  /**
   * Select refunds above @a serial_id in monotonically increasing
   * order.
   *
   * @param cls closure
   * @param session database connection
   * @param serial_id highest serial ID to exclude (select strictly larger)
   * @param cb function to call on each result
   * @param cb_cls closure for @a cb
   * @return transaction status code
   */
  enum GNUNET_DB_QueryStatus
  (*select_refunds_above_serial_id)(void *cls,
                                    struct TALER_EXCHANGEDB_Session *session,
                                    uint64_t serial_id,
                                    TALER_EXCHANGEDB_RefundCallback cb,
                                    void *cb_cls);


  /**
   * Select inbound wire transfers into reserves_in above @a serial_id
   * in monotonically increasing order.
   *
   * @param cls closure
   * @param session database connection
   * @param serial_id highest serial ID to exclude (select strictly larger)
   * @param cb function to call on each result
   * @param cb_cls closure for @a cb
   * @return transaction status code
   */
  enum GNUNET_DB_QueryStatus
  (*select_reserves_in_above_serial_id)(void *cls,
                                        struct TALER_EXCHANGEDB_Session *session,
                                        uint64_t serial_id,
                                        TALER_EXCHANGEDB_ReserveInCallback cb,
                                        void *cb_cls);

  /**
   * Select inbound wire transfers into reserves_in above @a serial_id
   * in monotonically increasing order by @a account_name.
   *
   * @param cls closure
   * @param session database connection
   * @param account_name name of the account for which we do the selection
   * @param serial_id highest serial ID to exclude (select strictly larger)
   * @param cb function to call on each result
   * @param cb_cls closure for @a cb
   * @return transaction status code
   */
  enum GNUNET_DB_QueryStatus
  (*select_reserves_in_above_serial_id_by_account)(void *cls,
                                                   struct TALER_EXCHANGEDB_Session *session,
                                                   const char *account_name,
                                                   uint64_t serial_id,
                                                   TALER_EXCHANGEDB_ReserveInCallback cb,
                                                   void *cb_cls);

  /**
   * Select withdraw operations from reserves_out above @a serial_id
   * in monotonically increasing order.
   *
   * @param cls closure
   * @param session database connection
   * @param account_name name of the account for which we do the selection
   * @param serial_id highest serial ID to exclude (select strictly larger)
   * @param cb function to call on each result
   * @param cb_cls closure for @a cb
   * @return transaction status code
   */
  enum GNUNET_DB_QueryStatus
  (*select_reserves_out_above_serial_id)(void *cls,
                                         struct TALER_EXCHANGEDB_Session *session,
                                         uint64_t serial_id,
                                         TALER_EXCHANGEDB_WithdrawCallback cb,
                                         void *cb_cls);

  /**
   * Function called to select outgoing wire transfers the exchange
   * executed, ordered by serial ID (monotonically increasing).
   *
   * @param cls closure
   * @param session database connection
   * @param serial_id lowest serial ID to include (select larger or equal)
   * @param cb function to call for ONE unfinished item
   * @param cb_cls closure for @a cb
   * @return transaction status code
   */
  enum GNUNET_DB_QueryStatus
  (*select_wire_out_above_serial_id)(void *cls,
                                     struct TALER_EXCHANGEDB_Session *session,
                                     uint64_t serial_id,
                                     TALER_EXCHANGEDB_WireTransferOutCallback cb,
                                     void *cb_cls);

  /**
   * Function called to select outgoing wire transfers the exchange
   * executed, ordered by serial ID (monotonically increasing).
   *
   * @param cls closure
   * @param session database connection
   * @param account_name name to select by
   * @param serial_id lowest serial ID to include (select larger or equal)
   * @param cb function to call for ONE unfinished item
   * @param cb_cls closure for @a cb
   * @return transaction status code
   */
  enum GNUNET_DB_QueryStatus
  (*select_wire_out_above_serial_id_by_account)(void *cls,
                                                struct TALER_EXCHANGEDB_Session *session,
                                                const char *account_name,
                                                uint64_t serial_id,
                                                TALER_EXCHANGEDB_WireTransferOutCallback cb,
                                                void *cb_cls);


  /**
   * Function called to select payback requests the exchange
   * received, ordered by serial ID (monotonically increasing).
   *
   * @param cls closure
   * @param session database connection
   * @param serial_id lowest serial ID to include (select larger or equal)
   * @param cb function to call for ONE unfinished item
   * @param cb_cls closure for @a cb
   * @return transaction status code
   */
  enum GNUNET_DB_QueryStatus
  (*select_payback_above_serial_id)(void *cls,
                                    struct TALER_EXCHANGEDB_Session *session,
                                    uint64_t serial_id,
                                    TALER_EXCHANGEDB_PaybackCallback cb,
                                    void *cb_cls);


  /**
   * Function called to select payback requests the exchange received for
   * refreshed coins, ordered by serial ID (monotonically increasing).
   *
   * @param cls closure
   * @param session database connection
   * @param serial_id lowest serial ID to include (select larger or equal)
   * @param cb function to call for ONE unfinished item
   * @param cb_cls closure for @a cb
   * @return transaction status code
   */
  enum GNUNET_DB_QueryStatus
  (*select_payback_refresh_above_serial_id)(void *cls,
                                            struct TALER_EXCHANGEDB_Session *session,
                                            uint64_t serial_id,
                                            TALER_EXCHANGEDB_PaybackRefreshCallback cb,
                                            void *cb_cls);


  /**
   * Function called to select reserve close operations the aggregator
   * triggered, ordered by serial ID (monotonically increasing).
   *
   * @param cls closure
   * @param session database connection
   * @param serial_id lowest serial ID to include (select larger or equal)
   * @param cb function to call
   * @param cb_cls closure for @a cb
   * @return transaction status code
   */
  enum GNUNET_DB_QueryStatus
  (*select_reserve_closed_above_serial_id)(void *cls,
                                           struct TALER_EXCHANGEDB_Session *session,
                                           uint64_t serial_id,
                                           TALER_EXCHANGEDB_ReserveClosedCallback cb,
                                           void *cb_cls);


  /**
   * Function called to add a request for an emergency payback for a
   * coin.  The funds are to be added back to the reserve.
   *
   * @param cls closure
   * @param session database connection
   * @param reserve_pub public key of the reserve that is being refunded
   * @param coin public information about a coin
   * @param coin_sig signature of the coin of type #TALER_SIGNATURE_WALLET_COIN_PAYBACK
   * @param coin_blind blinding key of the coin
   * @param h_blind_ev blinded envelope, as calculated by the exchange
   * @param amount total amount to be paid back
   * @param h_blind_ev hash of the blinded coin's envelope (must match reserves_out entry)
   * @param timestamp the timestamp to store
   * @return transaction result status
   */
  enum GNUNET_DB_QueryStatus
  (*insert_payback_request)(void *cls,
                            struct TALER_EXCHANGEDB_Session *session,
                            const struct TALER_ReservePublicKeyP *reserve_pub,
                            const struct TALER_CoinPublicInfo *coin,
                            const struct TALER_CoinSpendSignatureP *coin_sig,
                            const struct TALER_DenominationBlindingKeyP *coin_blind,
                            const struct TALER_Amount *amount,
                            const struct GNUNET_HashCode *h_blind_ev,
                            struct GNUNET_TIME_Absolute timestamp);


  /**
   * Function called to add a request for an emergency payback for a
   * refreshed coin.  The funds are to be added back to the original coin.
   *
   * @param cls closure
   * @param session database connection
   * @param coin public information about the refreshed coin
   * @param coin_sig signature of the coin of type #TALER_SIGNATURE_WALLET_COIN_PAYBACK
   * @param coin_blind blinding key of the coin
   * @param h_blind_ev blinded envelope, as calculated by the exchange
   * @param amount total amount to be paid back
   * @param h_blind_ev hash of the blinded coin's envelope (must match reserves_out entry)
   * @param timestamp a timestamp to store
   * @return transaction result status
   */
  enum GNUNET_DB_QueryStatus
  (*insert_payback_refresh_request)(void *cls,
                                    struct TALER_EXCHANGEDB_Session *session,
                                    const struct TALER_CoinPublicInfo *coin,
                                    const struct TALER_CoinSpendSignatureP *coin_sig,
                                    const struct TALER_DenominationBlindingKeyP *coin_blind,
                                    const struct TALER_Amount *amount,
                                    const struct GNUNET_HashCode *h_blind_ev,
                                    struct GNUNET_TIME_Absolute timestamp);


  /**
   * Obtain information about which reserve a coin was generated
   * from given the hash of the blinded coin.
   *
   * @param cls closure
   * @param session a session
   * @param h_blind_ev hash of the blinded coin
   * @param[out] reserve_pub set to information about the reserve (on success only)
   * @return transaction status code
   */
  enum GNUNET_DB_QueryStatus
  (*get_reserve_by_h_blind)(void *cls,
                            struct TALER_EXCHANGEDB_Session *session,
                            const struct GNUNET_HashCode *h_blind_ev,
                            struct TALER_ReservePublicKeyP *reserve_pub);


  /**
   * Obtain information about which old coin a coin was refreshed
   * given the hash of the blinded (fresh) coin.
   *
   * @param cls closure
   * @param session a session
   * @param h_blind_ev hash of the blinded coin
   * @param[out] old_coin_pub set to information about the old coin (on success only)
   * @return transaction status code
   */
  enum GNUNET_DB_QueryStatus
  (*get_old_coin_by_h_blind)(void *cls,
                             struct TALER_EXCHANGEDB_Session *session,
                             const struct GNUNET_HashCode *h_blind_ev,
                             struct TALER_CoinSpendPublicKeyP *old_coin_pub);


  /**
   * Store information that a denomination key was revoked
   * in the database.
   *
   * @param cls closure
   * @param session a session
   * @param denom_pub_hash hash of the revoked denomination key
   * @param master_sig signature affirming the revocation
   * @return transaction status code
   */
  enum GNUNET_DB_QueryStatus
  (*insert_denomination_revocation)(void *cls,
                                    struct TALER_EXCHANGEDB_Session *session,
                                    const struct GNUNET_HashCode *denom_pub_hash,
                                    const struct TALER_MasterSignatureP *master_sig);


  /**
   * Obtain information about a denomination key's revocation from
   * the database.
   *
   * @param cls closure
   * @param session a session
   * @param denom_pub_hash hash of the revoked denomination key
   * @param[out] master_sig signature affirming the revocation
   * @param[out] rowid row where the information is stored
   * @return transaction status code
   */
  enum GNUNET_DB_QueryStatus
  (*get_denomination_revocation)(void *cls,
                                 struct TALER_EXCHANGEDB_Session *session,
                                 const struct GNUNET_HashCode *denom_pub_hash,
                                 struct TALER_MasterSignatureP *master_sig,
                                 uint64_t *rowid);


  /**
   * Select all of those deposits in the database for which we do
   * not have a wire transfer (or a refund) and which should have
   * been deposited between @a start_date and @a end_date.
   *
   * @param cls closure
   * @param session a session
   * @param start_date lower bound on the requested wire execution date
   * @param end_date upper bound on the requested wire execution date
   * @param cb function to call on all such deposits
   * @param cb_cls closure for @a cb
   * @return transaction status code
   */
  enum GNUNET_DB_QueryStatus
  (*select_deposits_missing_wire)(void *cls,
                                  struct TALER_EXCHANGEDB_Session *session,
                                  struct GNUNET_TIME_Absolute start_date,
                                  struct GNUNET_TIME_Absolute end_date,
                                  TALER_EXCHANGEDB_WireMissingCallback cb,
                                  void *cb_cls);

  /**
   * Insert a merchant into the KYC monitor table, namely it
   * associates a flag to the merchant that indicates whether
   * a KYC check has been done or not on this merchant.
   *
   * @param cls closure
   * @param session db session
   * @param general_id identificator at the KYC-aware institution,
   *        can be NULL if this is in-line wiht the rules.
   * @param payto_url payto:// URL indentifying the merchant
   *        bank account.
   * @return database transaction status.
   */
  enum GNUNET_DB_QueryStatus
  (*insert_kyc_merchant)(void *cls,
                         struct TALER_EXCHANGEDB_Session *session,
                         const char *general_id,
                         const char *payto_url);

  /**
   * Mark a merchant as KYC-checked.
   *
   * @param cls closure
   * @param session db session
   * @param payto_url payto:// URL indentifying the merchant
   *        to check.  Note, different banks may have different
   *        policies to check their customers.
   * @return database transaction status.
   */
  enum GNUNET_DB_QueryStatus
  (*mark_kyc_merchant)(void *cls,
                       struct TALER_EXCHANGEDB_Session *session,
                       const char *payto_url);


  /**
   * Mark a merchant as NOT KYC-checked.
   *
   * @param cls closure
   * @param session db session
   * @param payto_url payto:// URL indentifying the merchant
   *        to unmark.  Note, different banks may have different
   *        policies to check their customers.
   * @return database transaction status.
   */
  enum GNUNET_DB_QueryStatus
  (*unmark_kyc_merchant)(void *cls,
                         struct TALER_EXCHANGEDB_Session *session,
                         const char *payto_url);


  /**
   * Retrieve KYC-check status related to a particular merchant.
   *
   * @param cls closure
   * @param session db session
   * @param payto_url URL identifying a merchant bank account,
   *        whose KYC is going to be retrieved.
   * @param ksc callback to process all the row's columns.  As
   *        expectable, it will only be called _if_ a row is found.
   * @param ksc_cls closure for above callback.
   * @return transaction status.
   */
  enum GNUNET_DB_QueryStatus
  (*get_kyc_status)(void *cls,
                    struct TALER_EXCHANGEDB_Session *session,
                    const char *payto_url,
                    TALER_EXCHANGEDB_KycStatusCallback ksc,
                    void *ksc_cls);

  /**
   * Record timestamp where a particular merchant performed
   * a wire transfer.
   *
   * @param cls closure.
   * @param session db session.
   * @param merchant_serial_id serial id of the merchant who
   *        performed the wire transfer.
   * @param amount amount of the wire transfer being monitored.
   * @return database transaction status.
   */
  enum GNUNET_DB_QueryStatus
  (*insert_kyc_event)(void *cls,
                      struct TALER_EXCHANGEDB_Session *session,
                      uint64_t merchant_serial_id,
                      struct TALER_Amount *amount);


  /**
   * Calculate sum of money flow related to a particular merchant,
   * used for KYC monitoring.
   *
   * @param cls closure
   * @param session DB session
   * @param merchant_serial_id serial id identifying the merchant
   *        into the KYC monitoring system.
   * @param amount[out] will store the amount of money received
   *        by this merchant.
   */
  enum GNUNET_DB_QueryStatus
  (*get_kyc_events)(void *cls,
                    struct TALER_EXCHANGEDB_Session *session,
                    uint64_t merchant_serial_id,
                    struct TALER_Amount *amount);

  /**
   * Delete wire transfer records related to a particular merchant.
   * This method would be called by the logic once that merchant
   * gets successfully KYC checked.
   *
   * @param cls closure
   * @param session DB session
   * @param merchant_serial_id serial id of the merchant whose
   *        KYC records have to be deleted.
   * @return DB transaction status.
   */
  enum GNUNET_DB_QueryStatus
  (*clean_kyc_events)(void *cls,
                      struct TALER_EXCHANGEDB_Session *session,
                      uint64_t merchant_serial_id);
};

#endif /* _TALER_EXCHANGE_DB_H */
