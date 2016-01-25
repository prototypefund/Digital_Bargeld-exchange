/*
  This file is part of TALER
  Copyright (C) 2016 GNUnet e.V.

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
 * @file plugin_wire_sepa.c
 * @brief wire plugin for transfers using SEPA/EBICS
 * @author Florian Dold
 * @author Christian Grothoff
 * @author Sree Harsha Totakura
 */
#include "platform.h"
#include "taler_wire_plugin.h"


/**
 * Type of the "cls" argument given to each of the functions in
 * our API.
 */
struct SepaClosure
{

  /**
   * Which currency do we support?
   */
  char *currency;

};


/**
 * Round amount DOWN to the amount that can be transferred via the wire
 * method.  For example, Taler may support 0.000001 EUR as a unit of
 * payment, but SEPA only supports 0.01 EUR.  This function would
 * round 0.125 EUR to 0.12 EUR in this case.
 *
 * @param cls the @e cls of this struct with the plugin-specific state
 * @param[in,out] amount amount to round down
 * @return #GNUNET_OK on success, #GNUNET_NO if rounding was unnecessary,
 *         #GNUNET_SYSERR if the amount or currency was invalid
 */
static int
sepa_amount_round (void *cls,
                   struct TALER_Amount *amount)
{
  struct SepaClosure *sc = cls;
  uint32_t delta;

  if (0 != strcasecmp (amount->currency,
                       sc->currency))
  {
    GNUNET_break (0);
    return GNUNET_SYSERR;
  }
  delta = amount->fraction % (TALER_AMOUNT_FRAC_BASE / 100);
  if (0 == delta)
    return GNUNET_NO;
  amount->fraction -= delta;
  return GNUNET_SYSERR;
}


/* Taken from GNU gettext */

/**
 * Entry in the country table.
 */
struct table_entry
{
  /**
   * 2-Character international country code.
   */
  const char *code;

  /**
   * Long English name of the country.
   */
  const char *english;
};


/* Keep the following table in sync with gettext.
   WARNING: the entries should stay sorted according to the code */
/**
 * List of country codes.
 */
static const struct table_entry country_table[] =
  {
    { "AE", "U.A.E." },
    { "AF", "Afghanistan" },
    { "AL", "Albania" },
    { "AM", "Armenia" },
    { "AN", "Netherlands Antilles" },
    { "AR", "Argentina" },
    { "AT", "Austria" },
    { "AU", "Australia" },
    { "AZ", "Azerbaijan" },
    { "BA", "Bosnia and Herzegovina" },
    { "BD", "Bangladesh" },
    { "BE", "Belgium" },
    { "BG", "Bulgaria" },
    { "BH", "Bahrain" },
    { "BN", "Brunei Darussalam" },
    { "BO", "Bolivia" },
    { "BR", "Brazil" },
    { "BT", "Bhutan" },
    { "BY", "Belarus" },
    { "BZ", "Belize" },
    { "CA", "Canada" },
    { "CG", "Congo" },
    { "CH", "Switzerland" },
    { "CI", "Cote d'Ivoire" },
    { "CL", "Chile" },
    { "CM", "Cameroon" },
    { "CN", "People's Republic of China" },
    { "CO", "Colombia" },
    { "CR", "Costa Rica" },
    { "CS", "Serbia and Montenegro" },
    { "CZ", "Czech Republic" },
    { "DE", "Germany" },
    { "DK", "Denmark" },
    { "DO", "Dominican Republic" },
    { "DZ", "Algeria" },
    { "EC", "Ecuador" },
    { "EE", "Estonia" },
    { "EG", "Egypt" },
    { "ER", "Eritrea" },
    { "ES", "Spain" },
    { "ET", "Ethiopia" },
    { "FI", "Finland" },
    { "FO", "Faroe Islands" },
    { "FR", "France" },
    { "GB", "United Kingdom" },
    { "GD", "Caribbean" },
    { "GE", "Georgia" },
    { "GL", "Greenland" },
    { "GR", "Greece" },
    { "GT", "Guatemala" },
    { "HK", "Hong Kong" },
    { "HK", "Hong Kong S.A.R." },
    { "HN", "Honduras" },
    { "HR", "Croatia" },
    { "HT", "Haiti" },
    { "HU", "Hungary" },
    { "ID", "Indonesia" },
    { "IE", "Ireland" },
    { "IL", "Israel" },
    { "IN", "India" },
    { "IQ", "Iraq" },
    { "IR", "Iran" },
    { "IS", "Iceland" },
    { "IT", "Italy" },
    { "JM", "Jamaica" },
    { "JO", "Jordan" },
    { "JP", "Japan" },
    { "KE", "Kenya" },
    { "KG", "Kyrgyzstan" },
    { "KH", "Cambodia" },
    { "KR", "South Korea" },
    { "KW", "Kuwait" },
    { "KZ", "Kazakhstan" },
    { "LA", "Laos" },
    { "LB", "Lebanon" },
    { "LI", "Liechtenstein" },
    { "LK", "Sri Lanka" },
    { "LT", "Lithuania" },
    { "LU", "Luxembourg" },
    { "LV", "Latvia" },
    { "LY", "Libya" },
    { "MA", "Morocco" },
    { "MC", "Principality of Monaco" },
    { "MD", "Moldava" },
    { "MD", "Moldova" },
    { "ME", "Montenegro" },
    { "MK", "Former Yugoslav Republic of Macedonia" },
    { "ML", "Mali" },
    { "MM", "Myanmar" },
    { "MN", "Mongolia" },
    { "MO", "Macau S.A.R." },
    { "MT", "Malta" },
    { "MV", "Maldives" },
    { "MX", "Mexico" },
    { "MY", "Malaysia" },
    { "NG", "Nigeria" },
    { "NI", "Nicaragua" },
    { "NL", "Netherlands" },
    { "NO", "Norway" },
    { "NP", "Nepal" },
    { "NZ", "New Zealand" },
    { "OM", "Oman" },
    { "PA", "Panama" },
    { "PE", "Peru" },
    { "PH", "Philippines" },
    { "PK", "Islamic Republic of Pakistan" },
    { "PL", "Poland" },
    { "PR", "Puerto Rico" },
    { "PT", "Portugal" },
    { "PY", "Paraguay" },
    { "QA", "Qatar" },
    { "RE", "Reunion" },
    { "RO", "Romania" },
    { "RS", "Serbia" },
    { "RU", "Russia" },
    { "RW", "Rwanda" },
    { "SA", "Saudi Arabia" },
    { "SE", "Sweden" },
    { "SG", "Singapore" },
    { "SI", "Slovenia" },
    { "SK", "Slovak" },
    { "SN", "Senegal" },
    { "SO", "Somalia" },
    { "SR", "Suriname" },
    { "SV", "El Salvador" },
    { "SY", "Syria" },
    { "TH", "Thailand" },
    { "TJ", "Tajikistan" },
    { "TM", "Turkmenistan" },
    { "TN", "Tunisia" },
    { "TR", "Turkey" },
    { "TT", "Trinidad and Tobago" },
    { "TW", "Taiwan" },
    { "TZ", "Tanzania" },
    { "UA", "Ukraine" },
    { "US", "United States" },
    { "UY", "Uruguay" },
    { "VA", "Vatican" },
    { "VE", "Venezuela" },
    { "VN", "Viet Nam" },
    { "YE", "Yemen" },
    { "ZA", "South Africa" },
    { "ZW", "Zimbabwe" }
  };


/**
 * Country code comparator function, for binary search with bsearch().
 *
 * @param ptr1 pointer to a `struct table_entry`
 * @param ptr2 pointer to a `struct table_entry`
 * @return result of strncmp()'ing the 2-digit country codes of the entries
 */
static int
cmp_country_code (const void *ptr1,
                  const void *ptr2)
{
  const struct table_entry *cc1 = ptr1;
  const struct table_entry *cc2 = ptr2;

  return strncmp (cc1->code,
                  cc2->code,
                  2);
}


/**
 * Validates given IBAN according to the European Banking Standards.  See:
 * http://www.europeanpaymentscouncil.eu/documents/ECBS%20IBAN%20standard%20EBS204_V3.2.pdf
 *
 * @param iban the IBAN number to validate
 * @return #GNUNET_YES if correctly formatted; #GNUNET_NO if not
 */
static int
validate_iban (const char *iban)
{
  char cc[2];
  char ibancpy[35];
  struct table_entry cc_entry;
  unsigned int len;
  char *nbuf;
  unsigned int i;
  unsigned int j;
  unsigned long long dividend;
  unsigned long long remainder;
  int nread;
  int ret;

  len = strlen (iban);
  if (len > 34)
    return GNUNET_NO;
  strncpy (cc, iban, 2);
  strncpy (ibancpy, iban + 4, len - 4);
  strncpy (ibancpy + len - 4, iban, 4);
  ibancpy[len] = '\0';
  cc_entry.code = cc;
  cc_entry.english = NULL;
  if (NULL ==
      bsearch (&cc_entry,
               country_table,
               sizeof (country_table) / sizeof (struct table_entry),
               sizeof (struct table_entry),
               &cmp_country_code))
    return GNUNET_NO;
  nbuf = GNUNET_malloc ((len * 2) + 1);
  for (i=0, j=0; i < len; i++)
  {
    if (isalpha ((int) ibancpy[i]))
    {
      if (2 != snprintf(&nbuf[j],
                        3,
                        "%2u",
                        (ibancpy[i] - 'A' + 10)))
      {
        GNUNET_free (nbuf);
        return GNUNET_NO;
      }
      j += 2;
      continue;
    }
    nbuf[j] = ibancpy[i];
    j++;
  }
  for (j=0;'\0' != nbuf[j];j++)
    GNUNET_assert (isdigit(nbuf[j]));
  GNUNET_assert (sizeof(dividend) >= 8);
  remainder = 0;
  for (i=0; i<j; i+=16)
  {
    if (1 !=
        (ret = sscanf (&nbuf[i],
                       "%16llu %n",
                       &dividend,
                       &nread)))
    {
      GNUNET_free (nbuf);
      return GNUNET_NO;
    }
    if (0 != remainder)
      dividend += remainder * (pow (10, nread));
    remainder = dividend % 97;
  }
  GNUNET_free (nbuf);
  if (1 == remainder)
    return GNUNET_YES;
  return GNUNET_NO;
}


/**
 * Check if the given wire format JSON object is correctly formatted
 *
 * @param wire the JSON wire format object
 * @return #GNUNET_YES if correctly formatted; #GNUNET_NO if not
 */
static int
sepa_wire_validate (const json_t *wire)
{
  json_error_t error;
  const char *type;
  const char *iban;
  const char *name;
  const char *bic;
  uint64_t r;
  const char *address;

  if (0 != json_unpack_ex
      ((json_t *) wire,
       &error, JSON_STRICT,
       "{"
       "s:s," /* TYPE: sepa */
       "s:s," /* IBAN: iban */
       "s:s," /* name: beneficiary name */
       "s:s," /* BIC: beneficiary bank's BIC */
       "s:i," /* r: random 64-bit integer nounce */
       "s:s"  /* address: address of the beneficiary */
       "}",
       "type", &type,
       "IBAN", &iban,
       "name", &name,
       "bic", &bic,
       "r", &r,
       "address", &address))
  {
    TALER_json_warn (error);
    return GNUNET_SYSERR;
  }
  if (0 != strcasecmp (type,
                       "sepa"))
  {
    GNUNET_log (GNUNET_ERROR_TYPE_INFO,
		"Transfer type `%s' invalid\n",
		type);
    return GNUNET_SYSERR;
  }
  if (1 != validate_iban (iban))
  {
    GNUNET_log (GNUNET_ERROR_TYPE_INFO,
		"IBAN `%s' invalid\n",
		iban);
    return GNUNET_NO;
  }
  return GNUNET_YES;
}


/**
 * Prepare for exeuction of a wire transfer.
 *
 * @param cls the @e cls of this struct with the plugin-specific state
 * @param wire valid wire account information
 * @param amount amount to transfer, already rounded
 * @param wtid wire transfer identifier to use
 * @param psc function to call with the prepared data to persist
 * @param psc_cls closure for @a psc
 * @return NULL on failure
 */
static struct TALER_WIRE_PrepareHandle *
sepa_prepare_wire_transfer (void *cls,
                            const json_t *wire,
                            const struct TALER_Amount *amount,
                            const struct TALER_WireTransferIdentifierRawP *wtid,
                            TALER_WIRE_PrepareTransactionCallback psc,
                            void *psc_cls)
{
  GNUNET_break (0); // FIXME: not implemented
  return NULL;
}


/**
 * Abort preparation of a wire transfer. For example,
 * because we are shutting down.
 *
 * @param cls the @e cls of this struct with the plugin-specific state
 * @param pth preparation to cancel
 */
static void
sepa_prepare_wire_transfer_cancel (void *cls,
                                   struct TALER_WIRE_PrepareHandle *pth)
{
  GNUNET_break (0); // FIXME: not implemented
}


/**
 * Execute a wire transfer.
 *
 * @param cls the @e cls of this struct with the plugin-specific state
 * @param buf buffer with the prepared execution details
 * @param buf_size number of bytes in @a buf
 * @param cc function to call upon success
 * @param cc_cls closure for @a cc
 * @return NULL on error
 */
static struct TALER_WIRE_ExecuteHandle *
sepa_execute_wire_transfer (void *cls,
                            const char *buf,
                            size_t buf_size,
                            TALER_WIRE_ConfirmationCallback cc,
                            void *cc_cls)
{
  GNUNET_break (0); // FIXME: not implemented
  return NULL;
}


/**
 * Abort execution of a wire transfer. For example, because we are
 * shutting down.  Note that if an execution is aborted, it may or
 * may not still succeed. The caller MUST run @e
 * execute_wire_transfer again for the same request as soon as
 * possilbe, to ensure that the request either ultimately succeeds
 * or ultimately fails. Until this has been done, the transaction is
 * in limbo (i.e. may or may not have been committed).
 *
 * @param cls the @e cls of this struct with the plugin-specific state
 * @param eh execution to cancel
 */
static void
sepa_execute_wire_transfer_cancel (void *cls,
                                   struct TALER_WIRE_ExecuteHandle *eh)
{
  GNUNET_break (0); // FIXME: not implemented
}


/**
 * Initialize sepa-wire subsystem.
 *
 * @param cls a configuration instance
 * @return NULL on error, otherwise a `struct TALER_WIRE_Plugin`
 */
void *
libtaler_plugin_wire_sepa_init (void *cls)
{
  struct GNUNET_CONFIGURATION_Handle *cfg = cls;
  struct SepaClosure *sc;
  struct TALER_WIRE_Plugin *plugin;

  sc = GNUNET_new (struct SepaClosure);
  if (GNUNET_OK !=
      GNUNET_CONFIGURATION_get_value_string (cfg,
                                             "mint",
                                             "CURRENCY",
                                             &sc->currency))
  {
    GNUNET_log_config_missing (GNUNET_ERROR_TYPE_ERROR,
                               "mint",
                               "CURRENCY");
    GNUNET_free (sc);
    return NULL;
  }

  plugin = GNUNET_new (struct TALER_WIRE_Plugin);
  plugin->cls = sc;
  plugin->amount_round = &sepa_amount_round;
  plugin->wire_validate = &sepa_wire_validate;
  plugin->prepare_wire_transfer = &sepa_prepare_wire_transfer;
  plugin->prepare_wire_transfer_cancel = &sepa_prepare_wire_transfer_cancel;
  plugin->execute_wire_transfer = &sepa_execute_wire_transfer;
  plugin->execute_wire_transfer_cancel = &sepa_execute_wire_transfer_cancel;
  return plugin;
}


/**
 * Shutdown Sepa wire subsystem.
 *
 * @param cls a `struct TALER_WIRE_Plugin`
 * @return NULL (always)
 */
void *
libtaler_plugin_wire_sepa_done (void *cls)
{
  struct TALER_WIRE_Plugin *plugin = cls;
  struct SepaClosure *sc = plugin->cls;

  GNUNET_free (sc->currency);
  GNUNET_free (sc);
  GNUNET_free (plugin);
  return NULL;
}

/* end of plugin_wire_sepa.c */
