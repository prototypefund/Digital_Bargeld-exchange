/*
  This file is part of TALER
  Copyright (C) 2015, 2016 Inria & GNUnet e.V.

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
 * @file exchangedb/plugin_exchangedb_common.c
 * @brief Functions shared across plugins, this file is meant to be
 *        included in each plugin.
 * @author Christian Grothoff
 */

/**
 * Free memory associated with the given reserve history.
 *
 * @param cls the @e cls of this struct with the plugin-specific state (unused)
 * @param rh history to free.
 */
static void
common_free_reserve_history (void *cls,
                             struct TALER_EXCHANGEDB_ReserveHistory *rh)
{
  struct TALER_EXCHANGEDB_BankTransfer *bt;
  struct TALER_EXCHANGEDB_CollectableBlindcoin *cbc;
  struct TALER_EXCHANGEDB_ReserveHistory *backref;

  while (NULL != rh)
  {
    switch(rh->type)
    {
    case TALER_EXCHANGEDB_RO_BANK_TO_EXCHANGE:
      bt = rh->details.bank;
      if (NULL != bt->sender_account_details)
        json_decref (bt->sender_account_details);
      if (NULL != bt->transfer_details)
        json_decref (bt->transfer_details);
      GNUNET_free (bt);
      break;
    case TALER_EXCHANGEDB_RO_WITHDRAW_COIN:
      cbc = rh->details.withdraw;
      GNUNET_CRYPTO_rsa_signature_free (cbc->sig.rsa_signature);
      GNUNET_CRYPTO_rsa_public_key_free (cbc->denom_pub.rsa_public_key);
      GNUNET_free (cbc);
      break;
    }
    backref = rh;
    rh = rh->next;
    GNUNET_free (backref);
  }
}


/**
 * Free memory of the link data list.
 *
 * @param cls the @e cls of this struct with the plugin-specific state (unused)
 * @param ldl link data list to release
 */
static void
common_free_link_data_list (void *cls,
                            struct TALER_EXCHANGEDB_LinkDataList *ldl)
{
  struct TALER_EXCHANGEDB_LinkDataList *next;

  while (NULL != ldl)
  {
    next = ldl->next;
    if (NULL != ldl->denom_pub.rsa_public_key)
        GNUNET_CRYPTO_rsa_public_key_free (ldl->denom_pub.rsa_public_key);
      if (NULL != ldl->ev_sig.rsa_signature)
        GNUNET_CRYPTO_rsa_signature_free (ldl->ev_sig.rsa_signature);
    GNUNET_free (ldl);
    ldl = next;
  }
}


/**
 * Free linked list of transactions.
 *
 * @param cls the @e cls of this struct with the plugin-specific state (unused)
 * @param list list to free
 */
static void
common_free_coin_transaction_list (void *cls,
                                   struct TALER_EXCHANGEDB_TransactionList *list)
{
  struct TALER_EXCHANGEDB_TransactionList *next;

  while (NULL != list)
  {
    next = list->next;

    switch (list->type)
    {
    case TALER_EXCHANGEDB_TT_DEPOSIT:
      json_decref (list->details.deposit->receiver_wire_account);
      if (NULL != list->details.deposit->coin.denom_pub.rsa_public_key)
        GNUNET_CRYPTO_rsa_public_key_free (list->details.deposit->coin.denom_pub.rsa_public_key);
      if (NULL != list->details.deposit->coin.denom_sig.rsa_signature)
        GNUNET_CRYPTO_rsa_signature_free (list->details.deposit->coin.denom_sig.rsa_signature);
      GNUNET_free (list->details.deposit);
      break;
    case TALER_EXCHANGEDB_TT_REFRESH_MELT:
      if (NULL != list->details.melt->coin.denom_pub.rsa_public_key)
        GNUNET_CRYPTO_rsa_public_key_free (list->details.melt->coin.denom_pub.rsa_public_key);
      if (NULL != list->details.melt->coin.denom_sig.rsa_signature)
        GNUNET_CRYPTO_rsa_signature_free (list->details.melt->coin.denom_sig.rsa_signature);
      GNUNET_free (list->details.melt);
      break;
    case TALER_EXCHANGEDB_TT_REFUND:
      if (NULL != list->details.refund->coin.denom_pub.rsa_public_key)
        GNUNET_CRYPTO_rsa_public_key_free (list->details.refund->coin.denom_pub.rsa_public_key);
      if (NULL != list->details.refund->coin.denom_sig.rsa_signature)
        GNUNET_CRYPTO_rsa_signature_free (list->details.refund->coin.denom_sig.rsa_signature);
      GNUNET_free (list->details.refund);
      break;
    }
    GNUNET_free (list);
    list = next;
  }
}


/**
 * Free melt commitment data.
 *
 * @param cls the @e cls of this struct with the plugin-specific state (unused)
 * @param mc data structure to free
 */
static void
common_free_melt_commitment (void *cls,
                             struct TALER_EXCHANGEDB_MeltCommitment *mc)
{
  unsigned int i;
  unsigned int k;

  if (NULL != mc->denom_pubs)
  {
    for (i=0;i<mc->num_newcoins;i++)
      if (NULL != mc->denom_pubs[i].rsa_public_key)
        GNUNET_CRYPTO_rsa_public_key_free (mc->denom_pubs[i].rsa_public_key);
    GNUNET_free (mc->denom_pubs);
  }
  for (k=0;k<TALER_CNC_KAPPA;k++)
  {
    if (NULL != mc->commit_coins[k])
    {
      for (i=0;i<mc->num_newcoins;i++)
      {
        /* NOTE: 'non_null' because this API is used also
           internally to clean up the struct on failures! */
        GNUNET_free_non_null (mc->commit_coins[k][i].coin_ev);
      }
      GNUNET_free (mc->commit_coins[k]);
    }
  }
  GNUNET_free (mc);
}

/* end of plugin_exchangedb_common.c */
