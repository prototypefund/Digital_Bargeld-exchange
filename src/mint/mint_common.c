/*
  This file is part of TALER
  (C) 2014 Christian Grothoff (and other contributing authors)

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
 * @file mint_common.c
 * @brief Common functionality for the mint
 * @author Florian Dold
 * @author Benedikt Mueller
 * @author Sree Harsha Totakura
 */

#include "platform.h"
#include "mint.h"

struct SignkeysIterateContext
{
  TALER_MINT_SignkeyIterator it;
  void *it_cls;
};


struct DenomkeysIterateContext
{
  const char *alias;
  TALER_MINT_DenomkeyIterator it;
  void *it_cls;
};


static int
signkeys_iterate_dir_iter (void *cls,
                           const char *filename)
{

  struct SignkeysIterateContext *skc = cls;
  ssize_t nread;
  struct TALER_MINT_SignKeyIssue issue;
  nread = GNUNET_DISK_fn_read (filename,
                               &issue,
                               sizeof (struct TALER_MINT_SignKeyIssue));
  if (nread != sizeof (struct TALER_MINT_SignKeyIssue))
  {
    GNUNET_log (GNUNET_ERROR_TYPE_WARNING, "Invalid signkey file: '%s'\n", filename);
    return GNUNET_OK;
  }
  return skc->it (skc->it_cls, &issue);
}


int
TALER_MINT_signkeys_iterate (const char *mint_base_dir,
                             TALER_MINT_SignkeyIterator it, void *cls)
{
  char *signkey_dir;
  size_t len;
  struct SignkeysIterateContext skc;

  len = GNUNET_asprintf (&signkey_dir, ("%s" DIR_SEPARATOR_STR DIR_SIGNKEYS), mint_base_dir);
  GNUNET_assert (len > 0);

  skc.it = it;
  skc.it_cls = cls;

  return GNUNET_DISK_directory_scan (signkey_dir, &signkeys_iterate_dir_iter, &skc);
}


/**
 * Import a denomination key from the given file
 *
 * @param filename the file to import the key from
 * @param dki pointer to return the imported denomination key
 * @return #GNUNET_OK upon success; #GNUNET_SYSERR upon failure
 */
int
TALER_MINT_read_denom_key (const char *filename,
                           struct TALER_MINT_DenomKeyIssue *dki)
{
  uint64_t size;
  size_t offset;
  void *data;
  struct TALER_RSA_PrivateKey *priv;
  int ret;

  ret = GNUNET_SYSERR;
  data = NULL;
  offset = sizeof (struct TALER_MINT_DenomKeyIssue)
      - offsetof (struct TALER_MINT_DenomKeyIssue, signature);
  if (GNUNET_OK != GNUNET_DISK_file_size (filename,
                                          &size,
                                          GNUNET_YES,
                                          GNUNET_YES))
    goto cleanup;
  if (size <= offset)
  {
    GNUNET_break (0);
    goto cleanup;
  }
  data = GNUNET_malloc (size);
  if (size != GNUNET_DISK_fn_read (filename,
                                   data,
                                   size))
    goto cleanup;
  if (NULL == (priv = TALER_RSA_decode_key (data + offset, size - offset)))
    goto cleanup;
  dki->denom_priv = priv;
  (void) memcpy (&dki->signature, data, offset);
  ret = GNUNET_OK;

 cleanup:
  GNUNET_free_non_null (data);
  return ret;
}


/**
 * Exports a denomination key to the given file
 *
 * @param filename the file where to write the denomination key
 * @param dki the denomination key
 * @return #GNUNET_OK upon success; #GNUNET_SYSERR upon failure.
 */
int
TALER_MINT_write_denom_key (const char *filename,
                            const struct TALER_MINT_DenomKeyIssue *dki)
{
  struct TALER_RSA_PrivateKeyBinaryEncoded *priv_enc;
  struct GNUNET_DISK_FileHandle *fh;
  ssize_t wrote;
  size_t wsize;
  int ret;

  fh = NULL;
  priv_enc = NULL;
  ret = GNUNET_SYSERR;
  if (NULL == (fh = GNUNET_DISK_file_open
               (filename,
                GNUNET_DISK_OPEN_WRITE | GNUNET_DISK_OPEN_CREATE | GNUNET_DISK_OPEN_TRUNCATE,
                GNUNET_DISK_PERM_USER_READ | GNUNET_DISK_PERM_USER_WRITE)))
    goto cleanup;
  if (NULL == (priv_enc = TALER_RSA_encode_key (dki->denom_priv)))
    goto cleanup;
  wsize = sizeof (struct TALER_MINT_DenomKeyIssue)
      - offsetof (struct TALER_MINT_DenomKeyIssue, signature);
  if (GNUNET_SYSERR == (wrote = GNUNET_DISK_file_write (fh,
                                                        &dki->signature,
                                                        wsize)))
    goto cleanup;
  if (wrote != wsize)
    goto cleanup;
  wsize = ntohs (priv_enc->len);
  if (GNUNET_SYSERR == (wrote = GNUNET_DISK_file_write (fh,
                                                        priv_enc,
                                                        wsize)))
    goto cleanup;
  if (wrote != wsize)
    goto cleanup;
  ret = GNUNET_OK;
 cleanup:
  GNUNET_free_non_null (priv_enc);
  if (NULL != fh)
    (void) GNUNET_DISK_file_close (fh);
  return ret;
}


static int
denomkeys_iterate_keydir_iter (void *cls,
                               const char *filename)
{

  struct DenomkeysIterateContext *dic = cls;
  struct TALER_MINT_DenomKeyIssue issue;

  if (GNUNET_OK != TALER_MINT_read_denom_key (filename, &issue))
  {
    GNUNET_log (GNUNET_ERROR_TYPE_WARNING, "Invalid denomkey file: '%s'\n", filename);
    return GNUNET_OK;
  }
  return dic->it (dic->it_cls, dic->alias, &issue);
}


static int
denomkeys_iterate_topdir_iter (void *cls,
                               const char *filename)
{

  struct DenomkeysIterateContext *dic = cls;
  dic->alias = GNUNET_STRINGS_get_short_name (filename);

  // FIXME: differentiate between error case and normal iteration abortion
  if (0 > GNUNET_DISK_directory_scan (filename, &denomkeys_iterate_keydir_iter, dic))
    return GNUNET_SYSERR;
  return GNUNET_OK;
}


int
TALER_MINT_denomkeys_iterate (const char *mint_base_dir,
                              TALER_MINT_DenomkeyIterator it, void *cls)
{
  char *dir;
  size_t len;
  struct DenomkeysIterateContext dic;
  len = GNUNET_asprintf (&dir, ("%s" DIR_SEPARATOR_STR DIR_DENOMKEYS),
                         mint_base_dir);
  GNUNET_assert (len > 0);

  dic.it = it;
  dic.it_cls = cls;

  // scan over alias dirs
  return GNUNET_DISK_directory_scan (dir, &denomkeys_iterate_topdir_iter, &dic);
}


struct GNUNET_CONFIGURATION_Handle *
TALER_MINT_config_load (const char *mint_base_dir)
{
  struct GNUNET_CONFIGURATION_Handle *cfg;
  char *cfg_dir;
  int res;

  res = GNUNET_asprintf (&cfg_dir, "%s" DIR_SEPARATOR_STR "config", mint_base_dir);
  GNUNET_assert (res > 0);

  cfg = GNUNET_CONFIGURATION_create ();
  res = GNUNET_CONFIGURATION_load_from (cfg, cfg_dir);
  GNUNET_free (cfg_dir);
  if (GNUNET_OK != res)
   return NULL;
  return cfg;
}

int
TALER_TALER_DB_extract_amount_nbo (PGresult *result, unsigned int row,
                            int indices[3], struct TALER_AmountNBO *denom_nbo)
{
  if ((indices[0] < 0) || (indices[1] < 0) || (indices[2] < 0))
    return GNUNET_NO;
  if (sizeof (uint32_t) != PQgetlength (result, row, indices[0]))
    return GNUNET_SYSERR;
  if (sizeof (uint32_t) != PQgetlength (result, row, indices[1]))
    return GNUNET_SYSERR;
  if (PQgetlength (result, row, indices[2]) > TALER_CURRENCY_LEN)
    return GNUNET_SYSERR;
  denom_nbo->value = *(uint32_t *) PQgetvalue (result, row, indices[0]);
  denom_nbo->fraction = *(uint32_t *) PQgetvalue (result, row, indices[1]);
  memset (denom_nbo->currency, 0, TALER_CURRENCY_LEN);
  memcpy (denom_nbo->currency, PQgetvalue (result, row, indices[2]), PQgetlength (result, row, indices[2]));
  return GNUNET_OK;
}


int
TALER_TALER_DB_extract_amount (PGresult *result, unsigned int row,
                        int indices[3], struct TALER_Amount *denom)
{
  struct TALER_AmountNBO denom_nbo;
  int res;

  res = TALER_TALER_DB_extract_amount_nbo (result, row, indices, &denom_nbo);
  if (GNUNET_OK != res)
    return res;
  *denom = TALER_amount_ntoh (denom_nbo);
  return GNUNET_OK;
}

/* end of mint_common.c */
