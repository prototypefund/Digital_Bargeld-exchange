/*
     This file is part of GNUnet.
     Copyright (C) 2006-2014 Christian Grothoff (and other contributing authors)

     GNUnet is free software; you can redistribute it and/or modify
     it under the terms of the GNU General Public License as published
     by the Free Software Foundation; either version 3, or (at your
     option) any later version.

     GNUnet is distributed in the hope that it will be useful, but
     WITHOUT ANY WARRANTY; without even the implied warranty of
     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
     General Public License for more details.

     You should have received a copy of the GNU General Public License
     along with GNUnet; see the file COPYING.  If not, write to the
     Free Software Foundation, Inc., 59 Temple Place - Suite 330,
     Boston, MA 02111-1307, USA.
*/

/**
 * @file os_installation.c
 * @brief get paths used by the program; based heavily on the
 *        corresponding GNUnet file, just adapted for Taler.
 * @author Milan
 */
#include "platform.h"
#include <gnunet/gnunet_util_lib.h>
#if DARWIN
#include <mach-o/ldsyms.h>
#include <mach-o/dyld.h>
#elif WINDOWS
#include <windows.h>
#endif


#define LOG(kind,...) GNUNET_log_from (kind, "util", __VA_ARGS__)

#define LOG_STRERROR_FILE(kind,syscall,filename) GNUNET_log_from_strerror_file (kind, "util", syscall, filename)


#if LINUX
/**
 * Try to determine path by reading /proc/PID/exe
 *
 * @return NULL on error
 */
static char *
get_path_from_proc_maps ()
{
  char fn[64];
  char line[1024];
  char dir[1024];
  FILE *f;
  char *lgu;

  GNUNET_snprintf (fn, sizeof (fn), "/proc/%u/maps", getpid ());
  if (NULL == (f = FOPEN (fn, "r")))
    return NULL;
  while (NULL != fgets (line, sizeof (line), f))
  {
    if ((1 ==
         SSCANF (line, "%*x-%*x %*c%*c%*c%*c %*x %*2x:%*2x %*u%*[ ]%1023s", dir)) &&
        (NULL != (lgu = strstr (dir, "libtalerutil"))))
    {
      lgu[0] = '\0';
      FCLOSE (f);
      return GNUNET_strdup (dir);
    }
  }
  FCLOSE (f);
  return NULL;
}


/**
 * Try to determine path by reading /proc/PID/exe
 *
 * @return NULL on error
 */
static char *
get_path_from_proc_exe ()
{
  char fn[64];
  char lnk[1024];
  ssize_t size;

  GNUNET_snprintf (fn, sizeof (fn), "/proc/%u/exe", getpid ());
  size = readlink (fn, lnk, sizeof (lnk) - 1);
  if (size <= 0)
  {
    LOG_STRERROR_FILE (GNUNET_ERROR_TYPE_ERROR, "readlink", fn);
    return NULL;
  }
  GNUNET_assert (size < sizeof (lnk));
  lnk[size] = '\0';
  while ((lnk[size] != '/') && (size > 0))
    size--;
  /* test for being in lib/taler/libexec/ or lib/MULTIARCH/taler/libexec */
  if ( (size > strlen ("/taler/libexec/")) &&
       (0 == strcmp ("/taler/libexec/",
		     &lnk[size - strlen ("/taler/libexec/")])) )
    size -= strlen ("taler/libexec/");
  if ((size < 4) || (lnk[size - 4] != '/'))
  {
    /* not installed in "/bin/" -- binary path probably useless */
    return NULL;
  }
  lnk[size] = '\0';
  return GNUNET_strdup (lnk);
}
#endif


#if WINDOWS
static HINSTANCE dll_instance;


/**
 * GNUNET_util_cl_init() in common_logging.c is preferred.
 * This function is only for thread-local storage (not used in GNUnet)
 * and hInstance saving.
 */
BOOL WINAPI
DllMain (HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
  switch (fdwReason)
  {
    case DLL_PROCESS_ATTACH:
      dll_instance = hinstDLL;
      break;
    case DLL_THREAD_ATTACH:
      break;
    case DLL_THREAD_DETACH:
      break;
    case DLL_PROCESS_DETACH:
      break;
  }
  return TRUE;
}


/**
 * Try to determine path with win32-specific function
 *
 * @return NULL on error
 */
static char *
get_path_from_module_filename ()
{
  size_t pathlen = 512;
  DWORD real_pathlen;
  wchar_t *idx;
  wchar_t *modulepath = NULL;
  char *upath;
  uint8_t *u8_string;
  size_t u8_string_length;

  /* This braindead function won't tell us how much space it needs, so
   * we start at 1024 and double the space up if it doesn't fit, until
   * it fits, or we exceed the threshold.
   */
  do
  {
    pathlen = pathlen * 2;
    modulepath = GNUNET_realloc (modulepath, pathlen * sizeof (wchar_t));
    SetLastError (0);
    real_pathlen = GetModuleFileNameW (dll_instance, modulepath, pathlen * sizeof (wchar_t));
  } while (real_pathlen >= pathlen && pathlen < 16*1024);
  if (real_pathlen >= pathlen)
    GNUNET_assert (0);
  /* To be safe */
  modulepath[real_pathlen] = '\0';

  idx = modulepath + real_pathlen;
  while ((idx > modulepath) && (*idx != L'\\') && (*idx != L'/'))
    idx--;
  *idx = L'\0';

  /* Now modulepath holds full path to the directory where libtalerutil is.
   * This directory should look like <TALER_PREFIX>/bin or <TALER_PREFIX>.
   */
  if (wcschr (modulepath, L'/') || wcschr (modulepath, L'\\'))
  {
    /* At least one directory component (i.e. we're not in a root directory) */
    wchar_t *dirname = idx;
    while ((dirname > modulepath) && (*dirname != L'\\') && (*dirname != L'/'))
      dirname--;
    *dirname = L'\0';
    if (dirname > modulepath)
    {
      dirname++;
      /* Now modulepath holds full path to the parent directory of the directory
       * where libtalerutil is.
       * dirname holds the name of the directory where libtalerutil is.
       */
      if (wcsicmp (dirname, L"bin") == 0)
      {
        /* pass */
      }
      else
      {
        /* Roll back our changes to modulepath */
        dirname--;
        *dirname = L'/';
      }
    }
  }

  /* modulepath is TALER_PREFIX */
  u8_string = u16_to_u8 (modulepath, wcslen (modulepath), NULL, &u8_string_length);
  if (NULL == u8_string)
    GNUNET_assert (0);

  upath = GNUNET_malloc (u8_string_length + 1);
  memcpy (upath, u8_string, u8_string_length);
  upath[u8_string_length] = '\0';

  free (u8_string);
  GNUNET_free (modulepath);

  return upath;
}
#endif


#if DARWIN
/**
 * Signature of the '_NSGetExecutablePath" function.
 *
 * @param buf where to write the path
 * @param number of bytes available in 'buf'
 * @return 0 on success, otherwise desired number of bytes is stored in 'bufsize'
 */
typedef int (*MyNSGetExecutablePathProto) (char *buf, size_t * bufsize);


/**
 * Try to obtain the path of our executable using '_NSGetExecutablePath'.
 *
 * @return NULL on error
 */
static char *
get_path_from_NSGetExecutablePath ()
{
  static char zero = '\0';
  char *path;
  size_t len;
  MyNSGetExecutablePathProto func;

  path = NULL;
  if (NULL == (func =
	       (MyNSGetExecutablePathProto) dlsym (RTLD_DEFAULT, "_NSGetExecutablePath")))
    return NULL;
  path = &zero;
  len = 0;
  /* get the path len, including the trailing \0 */
  (void) func (path, &len);
  if (0 == len)
    return NULL;
  path = GNUNET_malloc (len);
  if (0 != func (path, &len))
  {
    GNUNET_free (path);
    return NULL;
  }
  len = strlen (path);
  while ((path[len] != '/') && (len > 0))
    len--;
  path[len] = '\0';
  return path;
}


/**
 * Try to obtain the path of our executable using '_dyld_image' API.
 *
 * @return NULL on error
 */
static char *
get_path_from_dyld_image ()
{
  const char *path;
  char *p;
  char *s;
  unsigned int i;
  int c;

  c = _dyld_image_count ();
  for (i = 0; i < c; i++)
  {
    if (((const void *) _dyld_get_image_header (i)) != (const void *)&_mh_dylib_header)
      continue;
    path = _dyld_get_image_name (i);
    if ( (NULL == path) || (0 == strlen (path)) )
      continue;
    p = GNUNET_strdup (path);
    s = p + strlen (p);
    while ((s > p) && ('/' != *s))
      s--;
    s++;
    *s = '\0';
    return p;
  }
  return NULL;
}
#endif


/**
 * Return the actual path to a file found in the current
 * PATH environment variable.
 *
 * @param binary the name of the file to find
 * @return path to binary, NULL if not found
 */
static char *
get_path_from_PATH (const char *binary)
{
  char *path;
  char *pos;
  char *end;
  char *buf;
  const char *p;

  if (NULL == (p = getenv ("PATH")))
    return NULL;
#if WINDOWS
  /* On W32 look in CWD first. */
  GNUNET_asprintf (&path, ".%c%s", PATH_SEPARATOR, p);
#else
  path = GNUNET_strdup (p);     /* because we write on it */
#endif
  buf = GNUNET_malloc (strlen (path) + strlen (binary) + 1 + 1);
  pos = path;
  while (NULL != (end = strchr (pos, PATH_SEPARATOR)))
  {
    *end = '\0';
    sprintf (buf, "%s/%s", pos, binary);
    if (GNUNET_DISK_file_test (buf) == GNUNET_YES)
    {
      pos = GNUNET_strdup (pos);
      GNUNET_free (buf);
      GNUNET_free (path);
      return pos;
    }
    pos = end + 1;
  }
  sprintf (buf, "%s/%s", pos, binary);
  if (GNUNET_YES == GNUNET_DISK_file_test (buf))
  {
    pos = GNUNET_strdup (pos);
    GNUNET_free (buf);
    GNUNET_free (path);
    return pos;
  }
  GNUNET_free (buf);
  GNUNET_free (path);
  return NULL;
}


/**
 * Try to obtain the installation path using the "TALER_PREFIX" environment
 * variable.
 *
 * @return NULL on error (environment variable not set)
 */
static char *
get_path_from_TALER_PREFIX ()
{
  const char *p;

  if (NULL != (p = getenv ("TALER_PREFIX")))
    return GNUNET_strdup (p);
  return NULL;
}


/**
 * @brief get the path to Taler bin/ or lib/, prefering the lib/ path
 * @author Milan
 *
 * @return a pointer to the executable path, or NULL on error
 */
static char *
os_get_taler_path ()
{
  char *ret;

  if (NULL != (ret = get_path_from_TALER_PREFIX ()))
    return ret;
#if LINUX
  if (NULL != (ret = get_path_from_proc_maps ()))
    return ret;
  /* try path *first*, before /proc/exe, as /proc/exe can be wrong */
  if (NULL != (ret = get_path_from_PATH ("taler-mint-httpd")))
    return ret;
  if (NULL != (ret = get_path_from_proc_exe ()))
    return ret;
#endif
#if WINDOWS
  if (NULL != (ret = get_path_from_module_filename ()))
    return ret;
#endif
#if DARWIN
  if (NULL != (ret = get_path_from_dyld_image ()))
    return ret;
  if (NULL != (ret = get_path_from_NSGetExecutablePath ()))
    return ret;
#endif
  if (NULL != (ret = get_path_from_PATH ("taler-mint-httpd")))
    return ret;
  /* other attempts here */
  LOG (GNUNET_ERROR_TYPE_ERROR,
       _("Could not determine installation path for %s.  Set `%s' environment variable.\n"),
       "Taler", "TALER_PREFIX");
  return NULL;
}


/**
 * @brief get the path to current app's bin/
 * @author Milan
 *
 * @return a pointer to the executable path, or NULL on error
 */
static char *
os_get_exec_path ()
{
  char *ret = NULL;

#if LINUX
  if (NULL != (ret = get_path_from_proc_exe ()))
    return ret;
#endif
#if WINDOWS
  if (NULL != (ret = get_path_from_module_filename ()))
    return ret;
#endif
#if DARWIN
  if (NULL != (ret = get_path_from_NSGetExecutablePath ()))
    return ret;
#endif
  /* other attempts here */
  return ret;
}


/**
 * @brief get the path to a specific Taler installation directory or,
 * with #TALER_OS_IPK_SELF_PREFIX, the current running apps installation directory
 * @author Milan
 * @return a pointer to the dir path (to be freed by the caller)
 */
char *
TALER_OS_installation_get_path (enum GNUNET_OS_InstallationPathKind dirkind)
{
  size_t n;
  const char *dirname;
  char *execpath = NULL;
  char *tmp;
  char *multiarch;
  char *libdir;
  int isbasedir;

  /* if wanted, try to get the current app's bin/ */
  if (dirkind == GNUNET_OS_IPK_SELF_PREFIX)
    execpath = os_get_exec_path ();

  /* try to get Taler's bin/ or lib/, or if previous was unsuccessful some
   * guess for the current app */
  if (NULL == execpath)
    execpath = os_get_taler_path ();

  if (NULL == execpath)
    return NULL;

  n = strlen (execpath);
  if (0 == n)
  {
    /* should never happen, but better safe than sorry */
    GNUNET_free (execpath);
    return NULL;
  }
  /* remove filename itself */
  while ((n > 1) && (DIR_SEPARATOR == execpath[n - 1]))
    execpath[--n] = '\0';

  isbasedir = 1;
  if ((n > 6) &&
      ((0 == strcasecmp (&execpath[n - 6], "/lib32")) ||
       (0 == strcasecmp (&execpath[n - 6], "/lib64"))))
  {
    if ( (GNUNET_OS_IPK_LIBDIR != dirkind) &&
	 (GNUNET_OS_IPK_LIBEXECDIR != dirkind) )
    {
      /* strip '/lib32' or '/lib64' */
      execpath[n - 6] = '\0';
      n -= 6;
    }
    else
      isbasedir = 0;
  }
  else if ((n > 4) &&
           ((0 == strcasecmp (&execpath[n - 4], "/bin")) ||
            (0 == strcasecmp (&execpath[n - 4], "/lib"))))
  {
    /* strip '/bin' or '/lib' */
    execpath[n - 4] = '\0';
    n -= 4;
  }
  multiarch = NULL;
  if (NULL != (libdir = strstr (execpath, "/lib/")))
  {
    /* test for multi-arch path of the form "PREFIX/lib/MULTIARCH/";
       here we need to re-add 'multiarch' to lib and libexec paths later! */
    multiarch = &libdir[5];
    if (NULL == strchr (multiarch, '/'))
      libdir[0] = '\0'; /* Debian multiarch format, cut of from 'execpath' but preserve in multicarch */
    else
      multiarch = NULL; /* maybe not, multiarch still has a '/', which is not OK */
  }
  /* in case this was a directory named foo-bin, remove "foo-" */
  while ((n > 1) && (execpath[n - 1] == DIR_SEPARATOR))
    execpath[--n] = '\0';
  switch (dirkind)
  {
  case GNUNET_OS_IPK_PREFIX:
  case GNUNET_OS_IPK_SELF_PREFIX:
    dirname = DIR_SEPARATOR_STR;
    break;
  case GNUNET_OS_IPK_BINDIR:
    dirname = DIR_SEPARATOR_STR "bin" DIR_SEPARATOR_STR;
    break;
  case GNUNET_OS_IPK_LIBDIR:
    if (isbasedir)
    {
      GNUNET_asprintf (&tmp,
                       "%s%s%s%s%s",
                       execpath,
                       DIR_SEPARATOR_STR "lib",
                       (NULL != multiarch) ? DIR_SEPARATOR_STR : "",
                       (NULL != multiarch) ? multiarch : "",
                       DIR_SEPARATOR_STR "taler" DIR_SEPARATOR_STR);
      if (GNUNET_YES ==
          GNUNET_DISK_directory_test (tmp, GNUNET_YES))
      {
        GNUNET_free (execpath);
        return tmp;
      }
      GNUNET_free (tmp);
      tmp = NULL;
      if (4 == sizeof (void *))
      {
	dirname =
	  DIR_SEPARATOR_STR "lib32" DIR_SEPARATOR_STR "taler" DIR_SEPARATOR_STR;
	GNUNET_asprintf (&tmp,
                         "%s%s",
                         execpath,
                         dirname);
      }
      if (8 == sizeof (void *))
      {
	dirname =
	  DIR_SEPARATOR_STR "lib64" DIR_SEPARATOR_STR "taler" DIR_SEPARATOR_STR;
	GNUNET_asprintf (&tmp,
                         "%s%s",
                         execpath,
                         dirname);
      }

      if ( (NULL != tmp) &&
           (GNUNET_YES ==
            GNUNET_DISK_directory_test (tmp, GNUNET_YES)) )
      {
        GNUNET_free (execpath);
        return tmp;
      }
      GNUNET_free (tmp);
    }
    dirname = DIR_SEPARATOR_STR "taler" DIR_SEPARATOR_STR;
    break;
  case GNUNET_OS_IPK_DATADIR:
    dirname =
        DIR_SEPARATOR_STR "share" DIR_SEPARATOR_STR "taler" DIR_SEPARATOR_STR;
    break;
  case GNUNET_OS_IPK_LOCALEDIR:
    dirname =
        DIR_SEPARATOR_STR "share" DIR_SEPARATOR_STR "locale" DIR_SEPARATOR_STR;
    break;
  case GNUNET_OS_IPK_ICONDIR:
    dirname =
        DIR_SEPARATOR_STR "share" DIR_SEPARATOR_STR "icons" DIR_SEPARATOR_STR;
    break;
  case GNUNET_OS_IPK_DOCDIR:
    dirname =
        DIR_SEPARATOR_STR "share" DIR_SEPARATOR_STR "doc" DIR_SEPARATOR_STR \
        "gnunet" DIR_SEPARATOR_STR;
    break;
  case GNUNET_OS_IPK_LIBEXECDIR:
    if (isbasedir)
    {
      dirname =
        DIR_SEPARATOR_STR "taler" DIR_SEPARATOR_STR "libexec" DIR_SEPARATOR_STR;
      GNUNET_asprintf (&tmp,
                       "%s%s%s%s",
                       execpath,
                       DIR_SEPARATOR_STR "lib" DIR_SEPARATOR_STR,
                       (NULL != multiarch) ? multiarch : "",
                       dirname);
      if (GNUNET_YES ==
          GNUNET_DISK_directory_test (tmp, GNUNET_YES))
      {
        GNUNET_free (execpath);
        return tmp;
      }
      GNUNET_free (tmp);
      tmp = NULL;
      if (4 == sizeof (void *))
      {
	dirname =
	  DIR_SEPARATOR_STR "lib32" DIR_SEPARATOR_STR "taler" DIR_SEPARATOR_STR \
	  "libexec" DIR_SEPARATOR_STR;
	GNUNET_asprintf (&tmp,
                         "%s%s",
                         execpath,
                         dirname);
      }
      if (8 == sizeof (void *))
      {
	dirname =
	  DIR_SEPARATOR_STR "lib64" DIR_SEPARATOR_STR "taler" DIR_SEPARATOR_STR \
	  "libexec" DIR_SEPARATOR_STR;
	GNUNET_asprintf (&tmp,
                         "%s%s",
                         execpath,
                         dirname);
      }
      if ( (NULL != tmp) &&
           (GNUNET_YES ==
            GNUNET_DISK_directory_test (tmp, GNUNET_YES)) )
      {
        GNUNET_free (execpath);
        return tmp;
      }

      GNUNET_free (tmp);
    }
    dirname =
      DIR_SEPARATOR_STR "taler" DIR_SEPARATOR_STR \
      "libexec" DIR_SEPARATOR_STR;
    break;
  default:
    GNUNET_free (execpath);
    return NULL;
  }
  GNUNET_asprintf (&tmp,
                   "%s%s",
                   execpath,
                   dirname);
  GNUNET_free (execpath);
  return tmp;
}


/**
 * Given the name of a taler-helper, taler-service or taler-daemon
 * binary, try to prefix it with the libexec/-directory to get the
 * full path.
 *
 * @param progname name of the binary
 * @return full path to the binary, if possible, otherwise copy of 'progname'
 */
char *
TALER_OS_get_libexec_binary_path (const char *progname)
{
  static char *cache;
  char *libexecdir;
  char *binary;

  if ( (DIR_SEPARATOR == progname[0]) ||
       (GNUNET_YES == GNUNET_STRINGS_path_is_absolute (progname, GNUNET_NO, NULL, NULL)) )
    return GNUNET_strdup (progname);
  if (NULL != cache)
    libexecdir = cache;
  else
    libexecdir = GNUNET_OS_installation_get_path (GNUNET_OS_IPK_LIBEXECDIR);
  if (NULL == libexecdir)
    return GNUNET_strdup (progname);
  GNUNET_asprintf (&binary,
		   "%s%s",
		   libexecdir,
		   progname);
  cache = libexecdir;
  return binary;
}



/* end of os_installation.c */
