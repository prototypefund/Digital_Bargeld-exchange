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
 * @file include/taler_util_wallet.h
 * @brief Interface for common utility functions for wallets
 * @author Sree Harsha Totakura <sreeharsha@totakura.in>
 */
#ifndef TALER_UTIL_H
#define TALER_UTIL_H
#include <gnunet/gnunet_util_taler_wallet_lib.h>
#include "taler_amount_lib.h"
#include "taler_crypto_lib.h"


#ifdef __cplusplus
extern "C"
{
#if 0                           /* keep Emacsens' auto-indent happy */
}
#endif
#endif


/* Define logging functions */
#define TALER_LOG_DEBUG(...)                                  \
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG, __VA_ARGS__)

#define TALER_LOG_WARNING(...)                                \
  GNUNET_log (GNUNET_ERROR_TYPE_WARNING, __VA_ARGS__)

#define TALER_LOG_ERROR(...)                                  \
  GNUNET_log (GNUNET_ERROR_TYPE_ERROR, __VA_ARGS__)


/**
 * Tests a given as assertion and if failed prints it as a warning with the
 * given reason
 *
 * @param EXP the expression to test as assertion
 * @param reason string to print as warning
 */
#define TALER_assert_as(EXP, reason)                           \
  do {                                                          \
    if (EXP) break;                                             \
    TALER_LOG_ERROR ("%s at %s:%d\n", reason, __FILE__, __LINE__);       \
    abort ();                                                    \
  } while (0)


/**
 * Log an error message at log-level 'level' that indicates
 * a failure of the command 'cmd' with the message given
 * by gcry_strerror(rc).
 */
#define TALER_LOG_GCRY_ERROR(cmd, rc) do { TALER_LOG_ERROR ( \
                                             "`%s' failed at %s:%d with error: %s\n", \
                                             cmd, __FILE__, __LINE__, \
                                             gcry_strerror (rc)); } while (0)


#define TALER_gcry_ok(cmd) \
  do {int rc; rc = cmd; if (! rc) break; \
      TALER_LOG_ERROR ("A Gcrypt call failed at %s:%d with error: %s\n", \
                       __FILE__, \
                       __LINE__, gcry_strerror (rc)); abort (); } while (0)


/**
 * Initialize Gcrypt library.
 */
void
TALER_gcrypt_init (void);


/**
 * Obtain denomination amount from configuration file.
 *
 * @param cfg configuration to parse
 * @param section section of the configuration to access
 * @param option option of the configuration to access
 * @param[out] denom set to the amount found in configuration
 * @return #GNUNET_OK on success, #GNUNET_SYSERR on error
 */
int
TALER_config_get_denom (struct GNUNET_CONFIGURATION_Handle *cfg,
                        const char *section,
                        const char *option,
                        struct TALER_Amount *denom);


#if 0                           /* keep Emacsens' auto-indent happy */
{
#endif
#ifdef __cplusplus
}
#endif


#endif
