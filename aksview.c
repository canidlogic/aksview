/*
 * aksview.c
 * =========
 * 
 * Implementation of aksview.h
 * 
 * See the header for further information.
 */

#include "aksview.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AKS_TRANSLATE
#include "aksmacro.h"

/* OS-specific headers */
#ifdef AKS_WIN
/* Windows headers */
#include <windows.h>

#else
/* POSIX headers */
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>
#endif

/*
 * Constants
 * =========
 */

/*
 * Flags that can be used in the flags field of AKSVIEW structure.
 */
#define FLAG_RO (1)   /* Read-only */
#define FLAG_LE (2)   /* Platform is little endian */
#define FLAG_DT (4)   /* Dirty window */
#define FLAG_UT (8)   /* Update timestamp on close */

/*
 * (POSIX only) Read-write permissions for everyone.
 */
#ifdef AKS_POSIX
#define RWRWRW (S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP|S_IROTH|S_IWOTH)
#endif

/*
 * Type declarations
 * =================
 */

/*
 * AKSVIEW structure.
 * 
 * Prototype given in header.
 */
struct AKSVIEW_TAG {
  
  /*
   * Flags relevant to this object.
   * 
   * Combination of FLAG_ constants or zero if no flags active.
   */
  int flags;
  
  /*
   * The file handle of the underlying file.
   */
#ifdef AKS_POSIX
  int fh;
#else
  HANDLE fh;
#endif
  
  /*
   * On Windows only, the handle to the file mapping object.
   * 
   * This may be NULL if nothing is currently mapped, if the length of
   * the file is zero (which prevents a mapping from being created), or
   * if the file size has been changed but the mapping has been reloaded
   * yet.
   * 
   * POSIX doesn't have a separate file mapping handle.
   */
#ifdef AKS_WIN
  HANDLE fh_map;
#endif
  
  /*
   * On POSIX only, a copy of the path that was opened.
   * 
   * It is necessary to know this for utime().  On Windows, SetFileTime
   * acts on the handle and doesn't need the path.
   */
#ifdef AKS_POSIX
  char *pPathCopy;
#endif

  /*
   * The size of the file in bytes.
   * 
   * In range [0, AKSVIEW_MAXLEN].
   */
  int64_t flen;
  
  /*
   * The system page size in bytes.
   * 
   * Must be at least eight and be a multiple of eight.
   */
  int32_t pgsize;
  
  /*
   * The window size hint.
   * 
   * May have any value, including zero and negative.
   */
  int32_t hint;
  
  /*
   * The actual window size in bytes.
   * 
   * In range [1, flen] except if flen is zero, in which case this is
   * also zero.
   */
  int32_t wlen;
  
  /*
   * Pointer to the mapped window.
   * 
   * May be NULL if nothing is currently mapped.
   */
  uint8_t *pw;
  
  /*
   * The file offset of the first byte that is mapped in the window at
   * pw, or -1 if nothing is mapped.
   */
  int64_t wfirst;
  
  /*
   * The file offset of the last byte that is mapped in the window at
   * pw, or -1 if nothing is mapped.
   */
  int64_t wlast;
  
};

/*
 * Default fault and warn handlers
 * ===============================
 */

static void default_fault_handler(int line) {
  fprintf(stderr, "aksview fault line %d\n", line);
  exit(EXIT_FAILURE);
}

static void default_warn_handler(int line) {
  fprintf(stderr, "aksview warn line %d\n", line);
}

/*
 * Fault and warn pointers
 * =======================
 */

static void (*m_fpFault)(int) = &default_fault_handler;
static void (*m_fpWarn)(int) = &default_warn_handler;

/*
 * Fault and warn macros
 * =====================
 */

#define fault(line) m_fpFault(line)
#define warn(line) m_fpWarn(line)

/*
 * Local functions
 * ===============
 */

/* Prototypes */
static int isLESystem(void);
static int32_t getPageSize(void);
static int loadFileSize(AKSVIEW *pv);
static int computeWindow(AKSVIEW *pv);

static void unmap(AKSVIEW *pv);
static void unview(AKSVIEW *pv);
static void mapByte(AKSVIEW *pv, int64_t b);

/*
 * Determine whether the current system is little endian or big endian.
 * 
 * This also checks that signed integers are stored in two's complement,
 * causing a fault if they are not.
 * 
 * Return:
 * 
 *   non-zero if little-endian platform, zero if big endian
 */
static int isLESystem(void) {
  
  uint8_t buf[2];
  int16_t iv = 0;
  int result = 0;
  
  /* Initialize buffer */
  memset(buf, 0, 2);
  
  /* Set a value of -2 into the signed 16-bit value and then copy it to
   * the byte buffer */
  iv = (int16_t) -2;
  memcpy(buf, &iv, 2);
  
  /* Check the bit pattern */
  if ((buf[0] == 0xff) && (buf[1] == 0xfe)) {
    /* Big endian */
    result = 0;
    
  } else if ((buf[0] == 0xfe) && (buf[1] == 0xff)) {
    /* Little endian */
    result = 1;
    
  } else {
    /* Not a two's complement system */
    fault(__LINE__);
  }
  
  /* Return result */
  return result;
}

/*
 * Determine the system page size in bytes.
 * 
 * This also checks that the page size is at least eight and that it is
 * a multiple of eight, causing a fault if that is not the case.
 * 
 * Return:
 * 
 *   the system page size
 */
static int32_t getPageSize(void) {
  
  int32_t result;
#ifdef AKS_WIN
  SYSTEM_INFO si;
#else
  long val = 0;
#endif
  
  /* Initialize structures */
#ifdef AKS_WIN
  memset(&si, 0, sizeof(SYSTEM_INFO));
#endif
  
  /* Query the operating system */
#ifdef AKS_WIN
  GetSystemInfo(&si);
  result = (int32_t) si.dwAllocationGranularity;

#else
  val = sysconf(_SC_PAGE_SIZE);
  if (val < 8) {
    val = sysconf(_SC_PAGESIZE);
  }
  if (val < 0) {
    val = 4096;
  }
  result = (int32_t) val;
#endif
  
  /* Check that result is at least eight and a multiple of eight */
  if ((result < 8) || ((result & 0x7) != 0)) {
    fault(__LINE__);
  }
  
  /* Return result */
  return result;
}

/*
 * Load the current size of the file into the given AKSVIEW structure.
 * 
 * This is intended only for use during initialization of the structure.
 * 
 * Only the fh field of the structure must be properly filled in for
 * this function to work.  If successful, the flen field will be set to
 * the current length of the file in bytes.
 * 
 * Parameters:
 * 
 *   pv - the viewer structure
 * 
 * Return:
 * 
 *   non-zero if successful, zero if error
 */
static int loadFileSize(AKSVIEW *pv) {
  
  int status = 1;
  int64_t result = 0;
#ifdef AKS_POSIX
  struct stat st;
#else
  DWORD lo = 0;
  DWORD hi = 0;
#endif
  
  /* Initialize structures */
#ifdef AKS_POSIX
  memset(&st, 0, sizeof(struct stat));
#endif
  
  /* Check parameter */
  if (pv == NULL) {
    fault(__LINE__);
  }
  
  /* Check fh field */
#ifdef AKS_WIN
  if (pv->fh == INVALID_HANDLE_VALUE) {
    fault(__LINE__);
  }
#else
  if (pv->fh == -1) {
    fault(__LINE__);
  }
#endif
  
  /* Query file size */
#ifdef AKS_POSIX
  if (fstat(pv->fh, &st)) {
    status = 0;
  }
  if (status) {
    result = (int64_t) st.st_size;
  }

#else
  lo = GetFileSize(pv->fh, &hi);
  if (lo == INVALID_FILE_SIZE) {
    if (GetLastError() != NO_ERROR) {
      status = 0;
    }
  }
  if (status) {
    if (hi >= UINT32_C(0x80000000)) {
      status = 0;
    }
  }
  if (status) {
    result = (((int64_t) hi) << 32) | ((int64_t) lo);
  }
#endif
  
  /* Check result */
  if (status) {
    if ((result < 0) || (result > AKSVIEW_MAXLEN)) {
      status = 0;
    }
  }
  
  /* Write result into structure */
  if (status) {
    pv->flen = result;
  }
  
  /* Return status */
  return status;
}

/*
 * Based on the current state of an AKSVIEW structure, compute the
 * actual window size and store it in the structure.
 * 
 * This makes use of the flen, pgsize, and hint fields.  Based on the
 * values of those fields (which are not modified by this function), the
 * computed result is stored in the wlen field.
 * 
 * The return value stores whether the new value written to wlen is
 * different from the value that was previously there.  During
 * initialization, you can store -1 in the wlen field immediately before
 * calling this function since there is no previous valid value in that
 * case.
 * 
 * Parameters:
 * 
 *   pv - the viewer object
 * 
 * Return:
 * 
 *   non-zero if the new computed window length is different than the
 *   previous; zero if they are the same
 */
static int computeWindow(AKSVIEW *pv) {
  
  int32_t wl = 0;
  int result = 0;
  
  /* Check parameter and fields */
  if (pv == NULL) {
    fault(__LINE__);
  }
  if ((pv->flen < 0) || (pv->flen > AKSVIEW_MAXLEN)) {
    fault(__LINE__);
  }
  if ((pv->pgsize < 8) || ((pv->pgsize & 0x7) != 0)) {
    fault(__LINE__);
  }
  
  /* Begin with the hint */
  wl = pv->hint;
  
  /* Adjust the hint so it is at least the system page size */
  if (wl < pv->pgsize) {
    wl = pv->pgsize;
  }
  
  /* To prevent rounding problems, adjust the hint so it is no more than
   * one gigabyte */
  if (wl > INT32_C(1073741824)) {
    wl = INT32_C(1073741824);
  }
  
  /* If the hint is not page aligned, adjust it by rounding up */
  if ((wl % pv->pgsize) != 0) {
    wl = wl / pv->pgsize;
    wl++;
    wl = wl * pv->pgsize;
  }
  
  /* Finally, do not let the hint exceed the file size (even if this
   * will adjust the hint down to zero) */
  if (wl > pv->flen) {
    wl = (int32_t) pv->flen;
  }
  
  /* Check whether the computed window is different */
  if (pv->wlen != wl) {
    result = 1;
  } else {
    result = 0;
  }
  
  /* Update window length and return result */
  pv->wlen = wl;
  return result;
}

/*
 * Completely close any open file mapping.
 * 
 * On POSIX, this is equivalent to unview().  On Windows, this will
 * first unview() and then close any open file mapping object (but leave
 * the file handle open).
 * 
 * Parameters:
 * 
 *   pv - the viewer object
 */
static void unmap(AKSVIEW *pv) {
  
  /* Always begin by unviewing */
  unview(pv);
  
  /* (Windows only) If there is a file mapping handle, close it */
#ifdef AKS_WIN
  if (pv->fh_map != NULL) {
    if (!CloseHandle(pv->fh_map)) {
      warn(__LINE__);
    }
    pv->fh_map = NULL;
  }
#endif
}

/*
 * If there is a mapped window, unmap it.
 * 
 * If there is currently something mapped, it will be flushed before
 * being unmapped.
 * 
 * Parameters:
 * 
 *   pv - the viewer object
 */
static void unview(AKSVIEW *pv) {
  
  /* Check parameter */
  if (pv == NULL) {
    fault(__LINE__);
  }
  
  /* Only proceed if a window is mapped */
  if (pv->pw != NULL) {
  
    /* Flush view */
    aksview_flush(pv);
  
    /* Unmap the view */
#ifdef AKS_WIN
    if (!UnmapViewOfFile(pv->pw)) {
      warn(__LINE__);
    }
#else
    if (munmap(pv->pw, (size_t) (pv->wlast - pv->wfirst + 1))) {
      warn(__LINE__);
    }
#endif

    /* Update structure */
    pv->pw = NULL;
    pv->wfirst = -1;
    pv->wlast = -1;
  }
}

/*
 * Ensure that a window is mapped in the given viewer that includes the
 * given byte offset.
 * 
 * If there is currently a window mapped that includes the byte, this
 * function does nothing.  If there is currently a window mapped but it
 * doesn't include the byte, the window is unmapped and a window
 * including the byte is mapped.  If there is no current window mapped,
 * a window including the byte is mapped.
 * 
 * b must be greater than or equal to zero and less than the file length
 * or a fault occurs.
 * 
 * Windows are always aligned to at least eight-byte boundaries, so any
 * aligned integer up to 64-bit size that includes b will be fully
 * contained within the window.
 * 
 * Parameters:
 * 
 *   pv - the viewer object
 * 
 *   b - the byte offset to map
 */
static void mapByte(AKSVIEW *pv, int64_t b) {
  
  int64_t w = 0;
  int64_t r = 0;
  int32_t ws = 0;
  
  /* Check parameters */
  if (pv == NULL) {
    fault(__LINE__);
  }
  if ((b < 0) || (b >= pv->flen)) {
    fault(__LINE__);
  }
  
  /* Only proceed if byte not currently mapped */
  if ((b < pv->wfirst) || (b > pv->wlast)) {
    
    /* We need to change the view so first of all unmap any view that
     * may be mapped */
    unview(pv);
    
    /* Figure out which window the byte is in and get its starting
     * offset */
    w = b / ((int64_t) pv->wlen);
    w = w * ((int64_t) pv->wlen);
    
    /* Figure out how much remains in the file starting at this
     * window */
    r = pv->flen - w;
    
    /* Start with a window size equal to the computed window size */
    ws = pv->wlen;
    
    /* If remainder is less than window size, set adjusted window size
     * to remainder so we don't go past the end of the file */
    if (r < ws) {
      ws = (int32_t) r;
    }
    
    /* (Windows only) If no current file mapping object, open one */
#ifdef AKS_WIN
    if (pv->fh_map == NULL) {
      if (pv->flags & FLAG_RO)
        pv->fh_map = CreateFileMapping(
                      pv->fh,
                      NULL,
                      PAGE_READONLY,
                      0,
                      0,
                      NULL);
      } else {
        pv->fh_map = CreateFileMapping(
                      pv->fh,
                      NULL,
                      PAGE_READWRITE,
                      0,
                      0,
                      NULL);
      }
      if (pv->fh_map == NULL) {
        fault(__LINE__);
      }
    }
#endif

    /* Map the window */
#ifdef AKS_POSIX
    if (pv->flags & FLAG_RO) {
      pv->pw = (uint8_t *) mmap(
                            (void *) 0,
                            (size_t) ws,
                            PROT_READ,
                            MAP_PRIVATE,
                            pv->fh,
                            (off_t) w);
    } else {
      pv->pw = (uint8_t *) mmap(
                            (void *) 0,
                            (size_t) ws,
                            PROT_READ | PROT_WRITE,
                            MAP_SHARED,
                            pv->fh,
                            (off_t) w);
    }
    if (pv->pw == MAP_FAILED) {
      fault(__LINE__);
    }
#else
    if (pv->flags & FLAG_RO) {
      pv->pw = (uint8_t *) MapViewOfFile(
                            pv->fh_map,
                            FILE_MAP_READ,
                            (DWORD) (w >> 32),
                            (DWORD) (w & INT64_C(0xffffffff)),
                            (SIZE_T) ws);
    } else {
      pv->pw = (uint8_t *) MapViewOfFile(
                            pv->fh_map,
                            FILE_MAP_READ | FILE_MAP_WRITE,
                            (DWORD) (w >> 32),
                            (DWORD) (w & INT64_C(0xffffffff)),
                            (SIZE_T) ws);
    }
    if (pv->pw == NULL) {
      fault(__LINE__);
    }
#endif
    
    /* Update the window boundaries */
    pv->wfirst = w;
    pv->wlast = (w - 1) + ((int64_t) ws);
  }
}

/*
 * Public function implementations
 * ===============================
 * 
 * See the header for specifications.
 */

/*
 * aksview_onerror function.
 */
void aksview_onerror(void (*fpFault)(int), void (*fpWarn)(int)) {
  if (fpFault != NULL) {
    m_fpFault = fpFault;
  } else {
    m_fpFault = &default_fault_handler;
  }
  
  if (fpWarn != NULL) {
    m_fpWarn = fpWarn;
  } else {
    m_fpWarn = &default_warn_handler;
  }
}

/*
 * aksview_errstr function.
 */
const char *aksview_errstr(int code) {
  const char *pResult = NULL;
  
  switch (code) {
    case AKSVIEW_ERR_NONE:
      pResult = "No error";
      break;
    
    case AKSVIEW_ERR_BADMODE:
      pResult = "Invalid create viewer mode";
      break;
    
    case AKSVIEW_ERR_TRANSLATE:
      pResult = "Failed to translate path to wide characters";
      break;
    
    case AKSVIEW_ERR_OPEN:
      pResult = "Failed to open file path";
      break;
    
    case AKSVIEW_ERR_LENQUERY:
      pResult = "Failed to query length of file";
      break;
    
    default:
      pResult = "Unknown error";
  }
  
  return pResult;
}

/*
 * aksview_create function.
 */
AKSVIEW *aksview_create(const char *pPath, int mode, int *perr) {
  
  int status = 1;
  int dummy = 0;
  AKSVIEW *pv = NULL;
#ifdef AKS_POSIX
  int m = 0;
#endif
#ifdef AKS_WIN
  DWORD da = 0;
  DWORD shm = 0;
  DWORD cdp = 0;
#endif
#ifdef AKS_WIN_WAPI
  aks_tchar *pPathTrans = NULL;
#endif
  
  /* Initial parameter check */
  if (pPath == NULL) {
    fault(__LINE__);
  }
  
  /* If we weren't given an error return location, set it to dummy */
  if (perr == NULL) {
    perr = &dummy;
  }
  
  /* Reset error return code */
  *perr = AKSVIEW_ERR_NONE;
  
  /* Check that mode is recognized */
  if ((mode != AKSVIEW_READONLY) &&
      (mode != AKSVIEW_EXISTING) &&
      (mode != AKSVIEW_REGULAR) &&
      (mode != AKSVIEW_EXCLUSIVE)) {
    status = 0;
    *perr = AKSVIEW_ERR_BADMODE;
  }
  
  /* (Windows Unicode only) Translate path to wide characters */
#ifdef AKS_WIN_WAPI
  if (status) {
    pPathTrans = aks_toapi(pPath);
    if (pPathTrans == NULL) {
      status = 0;
      *perr = AKSVIEW_ERR_TRANSLATE;
    }
  }
#endif
  
  /* Allocate new viewer structure */
  if (status) {
    pv = (AKSVIEW *) calloc(1, sizeof(AKSVIEW));
    if (pv == NULL) {
      fault(__LINE__);
    }
  }
  
  /* Initialize all fields in viewer structure */
  if (status) {
    pv->flags = 0;
#ifdef AKS_WIN
    pv->fh = INVALID_HANDLE_VALUE;
    pv->fh_map = NULL;
#else
    pv->fh = -1;
    pv->pPathCopy = NULL;
#endif
    pv->flen = -1;
    pv->pgsize = -1;
    pv->hint = AKSVIEW_DEFAULT_HINT;
    pv->wlen = -1;
    pv->pw = NULL;
    pv->wfirst = -1;
    pv->wlast = -1;
  }
  
  /* Set flags based on open mode and platform endianness */
  if (status) {
    if (mode == AKSVIEW_READONLY) {
      pv->flags |= FLAG_RO;
    }
    if (isLESystem()) {
      pv->flags |= FLAG_LE;
    }
  }
  
  /* (POSIX only) Make a copy of the path and store it in the
   * structure */
#ifdef AKS_POSIX
  if (status) {
    pv->pPathCopy = (char *) malloc(strlen(pPath) + 1);
    if (pv->pPathCopy == NULL) {
      fault(__LINE__);
    }
    strcpy(pv->pPathCopy, pPath);
  }
#endif

  /* Store the page size */
  if (status) {
    pv->pgsize = getPageSize();
  }

  /* Open the file */
  if (status) {
#ifdef AKS_POSIX
    /* Opening file in POSIX -- first determine mode */
    if (mode == AKSVIEW_READONLY) {
      m = O_RDONLY;
      
    } else if (mode == AKSVIEW_EXISTING) {
      m = O_RDWR;
    
    } else if (mode == AKSVIEW_REGULAR) {
      m = O_RDWR | O_CREAT;
      
    } else if (mode == AKSVIEW_EXCLUSIVE) {
      m = O_RDWR | O_CREAT | O_EXCL;
      
    } else {
      /* Shouldn't happen */
      fault(__LINE__);
    }
    
    /* Call through to API function, passing R/W permissions for
     * everyone if a file might be created (modified by the process
     * umask) */
    if (m & O_CREAT) {
      pv->fh = open(pPath, m, RWRWRW);
    } else {
      pv->fh = open(pPath, m);
    }
    
    /* Check result */
    if (pv->fh == -1) {
      status = 0;
      *perr = AKSVIEW_ERR_OPEN;
    }

#else
    /* Opening file in Windows -- determine desired access, share mode,
     * and creation disposition */
    if (mode == AKSVIEW_READONLY) {
      da  = GENERIC_READ;
      shm = FILE_SHARE_READ;
      cdp = OPEN_EXISTING;
      
    } else if (mode == AKSVIEW_EXISTING) {
      da  = GENERIC_READ | GENERIC_WRITE;
      shm = 0;
      cdp = OPEN_EXISTING;
    
    } else if (mode == AKSVIEW_REGULAR) {
      da  = GENERIC_READ | GENERIC_WRITE;
      shm = 0;
      cdp = OPEN_ALWAYS;
      
    } else if (mode == AKSVIEW_EXCLUSIVE) {
      da  = GENERIC_READ | GENERIC_WRITE;
      shm = 0;
      cdp = CREATE_NEW;
      
    } else {
      /* Shouldn't happen */
      fault(__LINE__);
    }

    /* Open the file */
#ifdef AKS_WIN_WAPI
    pv->fh = CreateFile(
                pPathTrans,
                da,
                shm,
                NULL,
                cdp,
                FILE_ATTRIBUTE_NORMAL,
                NULL);
#else
    pv->fh = CreateFile(
                pPath,
                da,
                shm,
                NULL,
                cdp,
                FILE_ATTRIBUTE_NORMAL,
                NULL);
#endif
  
    /* Check result */
    if (pv->fh == INVALID_HANDLE_VALUE) {
      status = 0;
      *perr = AKSVIEW_ERR_OPEN;
    }

#endif
  }
  
  /* Load the initial file size */
  if (status) {
    if (!loadFileSize(pv)) {
      status = 0;
      *perr = AKSVIEW_ERR_LENQUERY;
    }
  }
  
  /* Compute the window size */
  if (status) {
    computeWindow(pv);
  }
  
  /* (Windows Unicode only) Free translated path if allocated */
#ifdef AKS_WIN_WAPI
  if (pPathTrans != NULL) {
    free(pPathTrans);
    pPathTrans = NULL;
  }
#endif

  /* If function failed, free viewer structure if allocated and make
   * sure the pointer is NULL */
  if (!status) {
    if (pv != NULL) {
      /* (POSIX only) Free path copy if allocated */
#ifdef AKS_POSIX
      if (pv->pPathCopy != NULL) {
        free(pv->pPathCopy);
        pv->pPathCopy = NULL;
      }
#endif

      /* Close file handle if open */
#ifdef AKS_WIN
      if (pv->fh != INVALID_HANDLE_VALUE) {
        if (!CloseHandle(pv->fh)) {
          warn(__LINE__);
        }
        pv->fh = INVALID_HANDLE_VALUE;
      }
#else
      if (pv->fh != -1) {
        if (close(pv->fh)) {
          warn(__LINE__);
        }
        pv->fh = -1;
      }
#endif

      /* Release structure and set pointer to NULL */
      free(pv);
      pv = NULL;
    }
  }
  
  /* Return structure or NULL */
  return pv;
}

/*
 * aksview_close function.
 */
void aksview_close(AKSVIEW *pv) {

#ifdef AKS_POSIX
  time_t t = 0;
  struct utimbuf tb;
#else
  FILETIME ft;
  SYSTEMTIME st;
#endif

  /* Initialize structures */
#ifdef AKS_WIN
  memset(&ft, 0, sizeof(FILETIME));
  memset(&st, 0, sizeof(SYSTEMTIME));
#else
  memset(&tb, 0, sizeof(struct utimbuf));
#endif

  /* Only proceed if non-NULL value passed */
  if (pv != NULL) {
  
    /* Completely unmap and view and file mapping object, which will
     * also flush if necessary */
    unmap(pv);
    
    /* If the update timestamp flag is set, update last-modified
     * timestamp on file */
    if (pv->flags & FLAG_UT) {
      
      /* First, we need to get the current time */
#ifdef AKS_POSIX
      t = time(NULL);
      if (t < 0) {
        fault(__LINE__);
      }
#else
      GetSystemTime(&st);
      if (!SystemTimeToFileTime(&st, &ft)) {
        fault(__LINE__);
      }
#endif
    
      /* Second, update the timestamp on the file */
#ifdef AKS_POSIX
      tb.actime  = t;
      tb.modtime = t;
      if (utime(pv->pPathCopy, &tb)) {
        fault(__LINE__);
      }
#else
      if (!SetFileTime(pv->fh, NULL, &ft, &ft)) {
        fault(__LINE__);
      }
#endif
    }
    
    /* (POSIX only) Free the path copy */
#ifdef AKS_POSIX
    if (pv->pPathCopy != NULL) {
      free(pv->pPathCopy);
      pv->pPathCopy = NULL;
    }
#endif
    
    /* Close the file handle */
#ifdef AKS_WIN
    if (pv->fh != INVALID_HANDLE_VALUE) {
      if (!CloseHandle(pv->fh)) {
        warn(__LINE__);
      }
      pv->fh = INVALID_HANDLE_VALUE;
    }
#else
    if (pv->fh != -1) {
      if (close(pv->fh)) {
        warn(__LINE__);
      }
      pv->fh = -1;
    }
#endif
    
    /* Release the structure */
    free(pv);
  }
}

/*
 * aksview_writable function.
 */
int aksview_writable(AKSVIEW *pv) {
  
  int result = 0;
  
  /* Check parameter */
  if (pv == NULL) {
    fault(__LINE__);
  }
  
  /* Query flags */
  if (pv->flags & FLAG_RO) {
    result = 0;
  } else {
    result = 1;
  }
  
  /* Return result */
  return result;
}

/*
 * aksview_getlen function.
 */
int64_t aksview_getlen(AKSVIEW *pv) {
  
  /* Check parameter */
  if (pv == NULL) {
    fault(__LINE__);
  }
  
  /* Return result */
  return pv->flen;
}

/*
 * aksview_setlen function.
 */
int aksview_setlen(AKSVIEW *pv, int64_t newlen) {
  
  int status = 1;
#ifdef AKS_POSIX
  uint8_t dummy = 0;
#endif
#ifdef AKS_WIN
  DWORD dhi = 0;
  DWORD dlo = 0;
  LONG  lhi = 0;
  LONG  llo = 0;
#endif
  
  /* Check parameters and state */
  if ((pv == NULL) || (newlen < 0) || (newlen > AKSVIEW_MAXLEN)) {
    fault(__LINE__);
  }
  if (pv->flags & FLAG_RO) {
    fault(__LINE__);
  }
  
  /* Only proceed if new length is actually different */
  if (newlen != pv->flen) {
  
    /* Begin by unmapping everything and flushing if necessary */
    unmap(pv);
    
    /* Change length of file */
#ifdef AKS_WIN
    /* On Windows, begin by splitting the new file length into two
     * LONG values */
    dhi = (DWORD) (newlen >> 32);
    dlo = (DWORD) (newlen & INT64_C(0xffffffff));
    
    memcpy(&lhi, &dhi, sizeof(LONG));
    memcpy(&llo, &dlo, sizeof(LONG));
    
    /* Position file pointer at new length */
    llo = SetFilePointer(pv->fh, llo, &lhi, FILE_BEGIN);
    if (llo == INVALID_SET_FILE_POINTER) {
      if (GetLastError() != NO_ERROR) {
        status = 0;
      }
    }
    
    /* Resize file so current file pointer is new length */
    if (status) {
      if (!SetEndOfFile(pv->fh)) {
        status = 0;
      }
    }
#else
    /* On POSIX, different handling depending on whether file is growing
     * or shrinking */
    if (newlen > pv->flen) {
      /* File is growing, so begin by seeking to the last byte in the
       * file after the enlargement */
      if (lseek(pv->fh, (off_t) (newlen - 1), SEEK_SET) == -1) {
        status = 0;
      }
      
      /* Write a single byte at the new last byte of the file to enlarge
       * the file */
      if (status) {
        if (write(pv->fh, &dummy, 1) != 1) {
          status = 0;
        }
      }
      
    } else if (newlen < pv->flen) {
      /* File is shrinking, so use truncation function */
      if (ftruncate(pv->fh, (off_t) newlen)) {
        status = 0;
      }
      
    } else {
      /* Shouldn't happen */
      fault(__LINE__);
    }
#endif
  
    /* Only proceed if we managed to change the file size */
    if (status) {
      
      /* Set the update timestamp flag */
      pv->flags |= FLAG_UT;
      
      /* Update the length recorded in the structure */
      pv->flen = newlen;
      
      /* Recompute the window size */
      computeWindow(pv);
    }
  }
  
  /* Return status */
  return status;
}

/*
 * aksview_sethint function.
 */
void aksview_sethint(AKSVIEW *pv, int32_t wlen) {
  
  /* Check parameters */
  if (pv == NULL) {
    fault(__LINE__);
  }
  
  /* Only proceed if new hint is actually different */
  if (wlen != pv->hint) {
    /* Write the new hint */
    pv->hint = wlen;
    
    /* Recompute the window size with the new hint, and if the window
     * size therefore changes, unmap any view that may be mapped */
    if (computeWindow(pv)) {
      unview(pv);
    }
  }
}

/*
 * aksview_flush function.
 */
void aksview_flush(AKSVIEW *pv) {
  
  /* Check parameters */
  if (pv == NULL) {
    fault(__LINE__);
  }
  
  /* Only proceed if the viewer object is has dirty flag set AND there
   * is currently a mapped window */
  if ((pv->flags & FLAG_DT) && (pv->pw != NULL)) {
    
    /* Flush any changes out to disk */
#ifdef AKS_WIN
    if (!FlushViewOfFile(pv->pw, 0)) {
      warn(__LINE__);
    }
#else
    if (msync(pv->pw, (size_t) (pv->wlast - pv->wfirst + 1), MS_SYNC)) {
      warn(__LINE__);
    }
#endif

    /* Invert the dirty flag to clear */
    pv->flags ^= FLAG_DT;
  }
}

/*
 * aksview_read8u function.
 */
uint8_t aksview_read8u(AKSVIEW *pv, int64_t pos) {
  /* Map the byte in the window, which also checks parameters */
  mapByte(pv, pos);
  
  /* Return the byte */
  return (pv->pw)[pos - pv->wfirst];
}

/*
 * aksview_read8s function.
 */
int8_t aksview_read8s(AKSVIEW *pv, int64_t pos) {
  
  int8_t result = 0;
  
  /* Map the byte in the window, which also checks parameters */
  mapByte(pv, pos);
  
  /* Copy and recast the byte to signed */
  memcpy(&result, &((pv->pw)[pos - pv->wfirst]), 1);
  
  /* Return result */
  return result;
}

/*
 * aksview_write8u function.
 */
void aksview_write8u(AKSVIEW *pv, int64_t pos, uint8_t v) {
  /* Map the byte in the window, which also checks parameters */
  mapByte(pv, pos);
  
  /* Check that not read-only */
  if (pv->flags & FLAG_RO) {
    fault(__LINE__);
  }
  
  /* Set dirty and update timestamp flags */
  pv->flags |= FLAG_DT;
  pv->flags |= FLAG_UT;
  
  /* Write the byte */
  (pv->pw)[pos - pv->wfirst] = v;
}

/*
 * aksview_write8s function.
 */
void aksview_write8s(AKSVIEW *pv, int64_t pos, int8_t v) {
  /* Map the byte in the window, which also checks parameters */
  mapByte(pv, pos);
  
  /* Check that not read-only */
  if (pv->flags & FLAG_RO) {
    fault(__LINE__);
  }
  
  /* Set dirty and update timestamp flags */
  pv->flags |= FLAG_DT;
  pv->flags |= FLAG_UT;
  
  /* Copy and recast the byte into the file */
  memcpy(&((pv->pw)[pos - pv->wfirst]), &v, 1);
}

/*
 * aksview_read16u function.
 */
uint16_t aksview_read16u(AKSVIEW *pv, int64_t pos, int le) {
  uint8_t bb[2];
  uint16_t result = 0;
  
  /* Rough check of parameters */
  if ((pos < 0) || (pos >= AKSVIEW_MAXLEN) || (pv == NULL)) {
    fault(__LINE__);
  }
  
  /* If le parameter is non-zero, replace it with FLAG_LE so we can do
   * an XOR check later */
  if (le) {
    le = FLAG_LE;
  }
  
  /* Different handling depending on alignment */
  if ((pos & 0x1) == 0) {
    /* Map the last byte into the window, which also checks parameters
     * and makes sure that the integer doesn't run beyond the end of the
     * file */
    mapByte(pv, pos + 1);
    
    /* Read the bytes, flipping if platform endianness and requested
     * endianness are different */
    if ((le ^ pv->flags) & FLAG_LE) {
      bb[1] = (pv->pw)[pos - pv->wfirst];
      bb[0] = (pv->pw)[pos - pv->wfirst + 1];
    } else {
      bb[0] = (pv->pw)[pos - pv->wfirst];
      bb[1] = (pv->pw)[pos - pv->wfirst + 1];
    }
    
    /* Copy and recast */
    memcpy(&result, bb, 2);
  
  } else {
    /* Unaligned so decompose call, flipping order of results if
     * platform endianness and requested endianness are different */
    if ((le ^ pv->flags) & FLAG_LE) {
      bb[1] = aksview_read8u(pv, pos);
      bb[0] = aksview_read8u(pv, pos + 1);
    } else {
      bb[0] = aksview_read8u(pv, pos);
      bb[1] = aksview_read8u(pv, pos + 1);
    }
    
    /* Copy and recast */
    memcpy(&result, bb, 2);
  }
  
  /* Return result */
  return result;
}

/*
 * aksview_read16s function.
 */
int16_t aksview_read16s(AKSVIEW *pv, int64_t pos, int le) {
  uint8_t bb[2];
  int16_t result = 0;
  
  /* Rough check of parameters */
  if ((pos < 0) || (pos >= AKSVIEW_MAXLEN) || (pv == NULL)) {
    fault(__LINE__);
  }
  
  /* If le parameter is non-zero, replace it with FLAG_LE so we can do
   * an XOR check later */
  if (le) {
    le = FLAG_LE;
  }
  
  /* Different handling depending on alignment */
  if ((pos & 0x1) == 0) {
    /* Map the last byte into the window, which also checks parameters
     * and makes sure that the integer doesn't run beyond the end of the
     * file */
    mapByte(pv, pos + 1);
    
    /* Read the bytes, flipping if platform endianness and requested
     * endianness are different */
    if ((le ^ pv->flags) & FLAG_LE) {
      bb[1] = (pv->pw)[pos - pv->wfirst];
      bb[0] = (pv->pw)[pos - pv->wfirst + 1];
    } else {
      bb[0] = (pv->pw)[pos - pv->wfirst];
      bb[1] = (pv->pw)[pos - pv->wfirst + 1];
    }
    
    /* Copy and recast */
    memcpy(&result, bb, 2);
  
  } else {
    /* Unaligned so decompose call, flipping order of results if
     * platform endianness and requested endianness are different */
    if ((le ^ pv->flags) & FLAG_LE) {
      bb[1] = aksview_read8u(pv, pos);
      bb[0] = aksview_read8u(pv, pos + 1);
    } else {
      bb[0] = aksview_read8u(pv, pos);
      bb[1] = aksview_read8u(pv, pos + 1);
    }
    
    /* Copy and recast */
    memcpy(&result, bb, 2);
  }
  
  /* Return result */
  return result;
}

/*
 * aksview_write16u function.
 */
void aksview_write16u(AKSVIEW *pv, int64_t pos, int le, uint16_t v) {
  uint8_t bb[2];
  
  /* Rough check of parameters */
  if ((pos < 0) || (pos >= AKSVIEW_MAXLEN) || (pv == NULL)) {
    fault(__LINE__);
  }
  
  /* If le parameter is non-zero, replace it with FLAG_LE so we can do
   * an XOR check later */
  if (le) {
    le = FLAG_LE;
  }
  
  /* Different handling depending on alignment */
  if ((pos & 0x1) == 0) {
    /* Copy and recast value to byte buffer */
    memcpy(bb, &v, 2);
    
    /* Map the last byte into the window, which also checks parameters
     * and makes sure that the integer doesn't run beyond the end of the
     * file */
    mapByte(pv, pos + 1);
    
    /* Check that not read-only */
    if (pv->flags & FLAG_RO) {
      fault(__LINE__);
    }
    
    /* Write the bytes, flipping if platform endianness and requested
     * endianness are different */
    if ((le ^ pv->flags) & FLAG_LE) {
      (pv->pw)[pos - pv->wfirst] = bb[1];
      (pv->pw)[pos - pv->wfirst + 1] = bb[0];
    } else {
      (pv->pw)[pos - pv->wfirst] = bb[0];
      (pv->pw)[pos - pv->wfirst + 1] = bb[1];
    }
    
    /* Set dirty and update timestamp flags */
    pv->flags |= FLAG_DT;
    pv->flags |= FLAG_UT;
  
  } else {
    /* Unaligned, so copy and recast value into byte buffer */
    memcpy(bb, &v, 2);
    
    /* Decompose call, flipping order of calls if platform endianness
     * and requested endianness are different */
    if ((le ^ pv->flags) & FLAG_LE) {
      aksview_write8u(pv, pos, bb[1]);
      aksview_write8u(pv, pos + 1, bb[0]);
    } else {
      aksview_write8u(pv, pos, bb[0]);
      aksview_write8u(pv, pos + 1, bb[1]);
    }
  }
}

/*
 * aksview_write16s function.
 */
void aksview_write16s(AKSVIEW *pv, int64_t pos, int le, int16_t v) {
  uint8_t bb[2];
  
  /* Rough check of parameters */
  if ((pos < 0) || (pos >= AKSVIEW_MAXLEN) || (pv == NULL)) {
    fault(__LINE__);
  }
  
  /* If le parameter is non-zero, replace it with FLAG_LE so we can do
   * an XOR check later */
  if (le) {
    le = FLAG_LE;
  }
  
  /* Different handling depending on alignment */
  if ((pos & 0x1) == 0) {
    /* Copy and recast value to byte buffer */
    memcpy(bb, &v, 2);
    
    /* Map the last byte into the window, which also checks parameters
     * and makes sure that the integer doesn't run beyond the end of the
     * file */
    mapByte(pv, pos + 1);
    
    /* Check that not read-only */
    if (pv->flags & FLAG_RO) {
      fault(__LINE__);
    }
    
    /* Write the bytes, flipping if platform endianness and requested
     * endianness are different */
    if ((le ^ pv->flags) & FLAG_LE) {
      (pv->pw)[pos - pv->wfirst] = bb[1];
      (pv->pw)[pos - pv->wfirst + 1] = bb[0];
    } else {
      (pv->pw)[pos - pv->wfirst] = bb[0];
      (pv->pw)[pos - pv->wfirst + 1] = bb[1];
    }
    
    /* Set dirty and update timestamp flags */
    pv->flags |= FLAG_DT;
    pv->flags |= FLAG_UT;
  
  } else {
    /* Unaligned, so copy and recast value into byte buffer */
    memcpy(bb, &v, 2);
    
    /* Decompose call, flipping order of calls if platform endianness
     * and requested endianness are different */
    if ((le ^ pv->flags) & FLAG_LE) {
      aksview_write8u(pv, pos, bb[1]);
      aksview_write8u(pv, pos + 1, bb[0]);
    } else {
      aksview_write8u(pv, pos, bb[0]);
      aksview_write8u(pv, pos + 1, bb[1]);
    }
  }
}

/*
 * aksview_read32u function.
 */
uint32_t aksview_read32u(AKSVIEW *pv, int64_t pos, int le) {
  uint8_t bb[4];
  uint16_t bw[2];
  uint32_t result = 0;
  
  /* Rough check of parameters */
  if ((pos < 0) || (pos >= AKSVIEW_MAXLEN) || (pv == NULL)) {
    fault(__LINE__);
  }
  
  /* If le parameter is non-zero, replace it with FLAG_LE so we can do
   * an XOR check later */
  if (le) {
    le = FLAG_LE;
  }
  
  /* Different handling depending on alignment */
  if ((pos & 0x3) == 0) {
    /* Map the last byte into the window, which also checks parameters
     * and makes sure that the integer doesn't run beyond the end of the
     * file */
    mapByte(pv, pos + 3);
    
    /* Read the bytes, flipping if platform endianness and requested
     * endianness are different */
    if ((le ^ pv->flags) & FLAG_LE) {
      bb[3] = (pv->pw)[pos - pv->wfirst];
      bb[2] = (pv->pw)[pos - pv->wfirst + 1];
      bb[1] = (pv->pw)[pos - pv->wfirst + 2];
      bb[0] = (pv->pw)[pos - pv->wfirst + 3];
    } else {
      bb[0] = (pv->pw)[pos - pv->wfirst];
      bb[1] = (pv->pw)[pos - pv->wfirst + 1];
      bb[2] = (pv->pw)[pos - pv->wfirst + 2];
      bb[3] = (pv->pw)[pos - pv->wfirst + 3];
    }
    
    /* Copy and recast */
    memcpy(&result, bb, 4);
  
  } else {
    /* Unaligned so decompose call, flipping order of results if
     * platform endianness and requested endianness are different */
    if ((le ^ pv->flags) & FLAG_LE) {
      bw[1] = aksview_read16u(pv, pos, le);
      bw[0] = aksview_read16u(pv, pos + 2, le);
    } else {
      bw[0] = aksview_read16u(pv, pos, le);
      bw[1] = aksview_read16u(pv, pos + 2, le);
    }
    
    /* Copy and recast */
    memcpy(&result, bw, 4);
  }
  
  /* Return result */
  return result;
}

/*
 * aksview_read32s function.
 */
int32_t aksview_read32s(AKSVIEW *pv, int64_t pos, int le) {
  uint8_t bb[4];
  uint16_t bw[2];
  int32_t result = 0;
  
  /* Rough check of parameters */
  if ((pos < 0) || (pos >= AKSVIEW_MAXLEN) || (pv == NULL)) {
    fault(__LINE__);
  }
  
  /* If le parameter is non-zero, replace it with FLAG_LE so we can do
   * an XOR check later */
  if (le) {
    le = FLAG_LE;
  }
  
  /* Different handling depending on alignment */
  if ((pos & 0x3) == 0) {
    /* Map the last byte into the window, which also checks parameters
     * and makes sure that the integer doesn't run beyond the end of the
     * file */
    mapByte(pv, pos + 3);
    
    /* Read the bytes, flipping if platform endianness and requested
     * endianness are different */
    if ((le ^ pv->flags) & FLAG_LE) {
      bb[3] = (pv->pw)[pos - pv->wfirst];
      bb[2] = (pv->pw)[pos - pv->wfirst + 1];
      bb[1] = (pv->pw)[pos - pv->wfirst + 2];
      bb[0] = (pv->pw)[pos - pv->wfirst + 3];
    } else {
      bb[0] = (pv->pw)[pos - pv->wfirst];
      bb[1] = (pv->pw)[pos - pv->wfirst + 1];
      bb[2] = (pv->pw)[pos - pv->wfirst + 2];
      bb[3] = (pv->pw)[pos - pv->wfirst + 3];
    }
    
    /* Copy and recast */
    memcpy(&result, bb, 4);
  
  } else {
    /* Unaligned so decompose call, flipping order of results if
     * platform endianness and requested endianness are different */
    if ((le ^ pv->flags) & FLAG_LE) {
      bw[1] = aksview_read16u(pv, pos, le);
      bw[0] = aksview_read16u(pv, pos + 2, le);
    } else {
      bw[0] = aksview_read16u(pv, pos, le);
      bw[1] = aksview_read16u(pv, pos + 2, le);
    }
    
    /* Copy and recast */
    memcpy(&result, bw, 4);
  }
  
  /* Return result */
  return result;
}

/*
 * aksview_write32u function.
 */
void aksview_write32u(AKSVIEW *pv, int64_t pos, int le, uint32_t v) {
  uint8_t bb[4];
  uint16_t bw[2];
  
  /* Rough check of parameters */
  if ((pos < 0) || (pos >= AKSVIEW_MAXLEN) || (pv == NULL)) {
    fault(__LINE__);
  }
  
  /* If le parameter is non-zero, replace it with FLAG_LE so we can do
   * an XOR check later */
  if (le) {
    le = FLAG_LE;
  }
  
  /* Different handling depending on alignment */
  if ((pos & 0x3) == 0) {
    /* Copy and recast */
    memcpy(bb, &v, 4);
    
    /* Map the last byte into the window, which also checks parameters
     * and makes sure that the integer doesn't run beyond the end of the
     * file */
    mapByte(pv, pos + 3);
    
    /* Check that not read-only */
    if (pv->flags & FLAG_RO) {
      fault(__LINE__);
    }
    
    /* Write the bytes, flipping if platform endianness and requested
     * endianness are different */
    if ((le ^ pv->flags) & FLAG_LE) {
      (pv->pw)[pos - pv->wfirst] = bb[3];
      (pv->pw)[pos - pv->wfirst + 1] = bb[2];
      (pv->pw)[pos - pv->wfirst + 2] = bb[1];
      (pv->pw)[pos - pv->wfirst + 3] = bb[0];
    } else {
      (pv->pw)[pos - pv->wfirst] = bb[0];
      (pv->pw)[pos - pv->wfirst + 1] = bb[1];
      (pv->pw)[pos - pv->wfirst + 2] = bb[2];
      (pv->pw)[pos - pv->wfirst + 3] = bb[3];
    }
    
    /* Set dirty and update timestamp flags */
    pv->flags |= FLAG_DT;
    pv->flags |= FLAG_UT;
  
  } else {
    /* Unaligned, so copy and recast value into word buffer */
    memcpy(bw, &v, 4);
    
    /* Decompose call, flipping order of results if platform endianness
     * and requested endianness are different */
    if ((le ^ pv->flags) & FLAG_LE) {
      aksview_write16u(pv, pos, le, bw[1]);
      aksview_write16u(pv, pos + 2, le, bw[0]);
    } else {
      aksview_write16u(pv, pos, le, bw[0]);
      aksview_write16u(pv, pos + 2, le, bw[1]);
    }
  }
}

/*
 * aksview_write32s function.
 */
void aksview_write32s(AKSVIEW *pv, int64_t pos, int le, int32_t v) {
  uint8_t bb[4];
  uint16_t bw[2];
  
  /* Rough check of parameters */
  if ((pos < 0) || (pos >= AKSVIEW_MAXLEN) || (pv == NULL)) {
    fault(__LINE__);
  }
  
  /* If le parameter is non-zero, replace it with FLAG_LE so we can do
   * an XOR check later */
  if (le) {
    le = FLAG_LE;
  }
  
  /* Different handling depending on alignment */
  if ((pos & 0x3) == 0) {
    /* Copy and recast */
    memcpy(bb, &v, 4);
    
    /* Map the last byte into the window, which also checks parameters
     * and makes sure that the integer doesn't run beyond the end of the
     * file */
    mapByte(pv, pos + 3);
    
    /* Check that not read-only */
    if (pv->flags & FLAG_RO) {
      fault(__LINE__);
    }
    
    /* Write the bytes, flipping if platform endianness and requested
     * endianness are different */
    if ((le ^ pv->flags) & FLAG_LE) {
      (pv->pw)[pos - pv->wfirst] = bb[3];
      (pv->pw)[pos - pv->wfirst + 1] = bb[2];
      (pv->pw)[pos - pv->wfirst + 2] = bb[1];
      (pv->pw)[pos - pv->wfirst + 3] = bb[0];
    } else {
      (pv->pw)[pos - pv->wfirst] = bb[0];
      (pv->pw)[pos - pv->wfirst + 1] = bb[1];
      (pv->pw)[pos - pv->wfirst + 2] = bb[2];
      (pv->pw)[pos - pv->wfirst + 3] = bb[3];
    }
    
    /* Set dirty and update timestamp flags */
    pv->flags |= FLAG_DT;
    pv->flags |= FLAG_UT;
  
  } else {
    /* Unaligned, so copy and recast value into word buffer */
    memcpy(bw, &v, 4);
    
    /* Decompose call, flipping order of results if platform endianness
     * and requested endianness are different */
    if ((le ^ pv->flags) & FLAG_LE) {
      aksview_write16u(pv, pos, le, bw[1]);
      aksview_write16u(pv, pos + 2, le, bw[0]);
    } else {
      aksview_write16u(pv, pos, le, bw[0]);
      aksview_write16u(pv, pos + 2, le, bw[1]);
    }
  }
}

/*
 * aksview_read64u function.
 */
uint64_t aksview_read64u(AKSVIEW *pv, int64_t pos, int le) {
  uint8_t bb[8];
  uint32_t bw[2];
  uint64_t result = 0;
  
  /* Rough check of parameters */
  if ((pos < 0) || (pos >= AKSVIEW_MAXLEN) || (pv == NULL)) {
    fault(__LINE__);
  }
  
  /* If le parameter is non-zero, replace it with FLAG_LE so we can do
   * an XOR check later */
  if (le) {
    le = FLAG_LE;
  }
  
  /* Different handling depending on alignment */
  if ((pos & 0x7) == 0) {
    /* Map the last byte into the window, which also checks parameters
     * and makes sure that the integer doesn't run beyond the end of the
     * file */
    mapByte(pv, pos + 7);
    
    /* Read the bytes, flipping if platform endianness and requested
     * endianness are different */
    if ((le ^ pv->flags) & FLAG_LE) {
      bb[7] = (pv->pw)[pos - pv->wfirst];
      bb[6] = (pv->pw)[pos - pv->wfirst + 1];
      bb[5] = (pv->pw)[pos - pv->wfirst + 2];
      bb[4] = (pv->pw)[pos - pv->wfirst + 3];
      bb[3] = (pv->pw)[pos - pv->wfirst + 4];
      bb[2] = (pv->pw)[pos - pv->wfirst + 5];
      bb[1] = (pv->pw)[pos - pv->wfirst + 6];
      bb[0] = (pv->pw)[pos - pv->wfirst + 7];
    } else {
      bb[0] = (pv->pw)[pos - pv->wfirst];
      bb[1] = (pv->pw)[pos - pv->wfirst + 1];
      bb[2] = (pv->pw)[pos - pv->wfirst + 2];
      bb[3] = (pv->pw)[pos - pv->wfirst + 3];
      bb[4] = (pv->pw)[pos - pv->wfirst + 4];
      bb[5] = (pv->pw)[pos - pv->wfirst + 5];
      bb[6] = (pv->pw)[pos - pv->wfirst + 6];
      bb[7] = (pv->pw)[pos - pv->wfirst + 7];
    }
    
    /* Copy and recast */
    memcpy(&result, bb, 8);
  
  } else {
    /* Unaligned so decompose call, flipping order of results if
     * platform endianness and requested endianness are different */
    if ((le ^ pv->flags) & FLAG_LE) {
      bw[1] = aksview_read32u(pv, pos, le);
      bw[0] = aksview_read32u(pv, pos + 4, le);
    } else {
      bw[0] = aksview_read32u(pv, pos, le);
      bw[1] = aksview_read32u(pv, pos + 4, le);
    }
    
    /* Copy and recast */
    memcpy(&result, bw, 8);
  }
  
  /* Return result */
  return result;
}

/*
 * aksview_read64s function.
 */
int64_t aksview_read64s(AKSVIEW *pv, int64_t pos, int le) {
  uint8_t bb[8];
  uint32_t bw[2];
  int64_t result = 0;
  
  /* Rough check of parameters */
  if ((pos < 0) || (pos >= AKSVIEW_MAXLEN) || (pv == NULL)) {
    fault(__LINE__);
  }
  
  /* If le parameter is non-zero, replace it with FLAG_LE so we can do
   * an XOR check later */
  if (le) {
    le = FLAG_LE;
  }
  
  /* Different handling depending on alignment */
  if ((pos & 0x7) == 0) {
    /* Map the last byte into the window, which also checks parameters
     * and makes sure that the integer doesn't run beyond the end of the
     * file */
    mapByte(pv, pos + 7);
    
    /* Read the bytes, flipping if platform endianness and requested
     * endianness are different */
    if ((le ^ pv->flags) & FLAG_LE) {
      bb[7] = (pv->pw)[pos - pv->wfirst];
      bb[6] = (pv->pw)[pos - pv->wfirst + 1];
      bb[5] = (pv->pw)[pos - pv->wfirst + 2];
      bb[4] = (pv->pw)[pos - pv->wfirst + 3];
      bb[3] = (pv->pw)[pos - pv->wfirst + 4];
      bb[2] = (pv->pw)[pos - pv->wfirst + 5];
      bb[1] = (pv->pw)[pos - pv->wfirst + 6];
      bb[0] = (pv->pw)[pos - pv->wfirst + 7];
    } else {
      bb[0] = (pv->pw)[pos - pv->wfirst];
      bb[1] = (pv->pw)[pos - pv->wfirst + 1];
      bb[2] = (pv->pw)[pos - pv->wfirst + 2];
      bb[3] = (pv->pw)[pos - pv->wfirst + 3];
      bb[4] = (pv->pw)[pos - pv->wfirst + 4];
      bb[5] = (pv->pw)[pos - pv->wfirst + 5];
      bb[6] = (pv->pw)[pos - pv->wfirst + 6];
      bb[7] = (pv->pw)[pos - pv->wfirst + 7];
    }
    
    /* Copy and recast */
    memcpy(&result, bb, 8);
  
  } else {
    /* Unaligned so decompose call, flipping order of results if
     * platform endianness and requested endianness are different */
    if ((le ^ pv->flags) & FLAG_LE) {
      bw[1] = aksview_read32u(pv, pos, le);
      bw[0] = aksview_read32u(pv, pos + 4, le);
    } else {
      bw[0] = aksview_read32u(pv, pos, le);
      bw[1] = aksview_read32u(pv, pos + 4, le);
    }
    
    /* Copy and recast */
    memcpy(&result, bw, 8);
  }
  
  /* Return result */
  return result;
}

/*
 * aksview_write64u function.
 */
void aksview_write64u(AKSVIEW *pv, int64_t pos, int le, uint64_t v) {
  uint8_t bb[8];
  uint32_t bw[2];
  
  /* Rough check of parameters */
  if ((pos < 0) || (pos >= AKSVIEW_MAXLEN) || (pv == NULL)) {
    fault(__LINE__);
  }
  
  /* If le parameter is non-zero, replace it with FLAG_LE so we can do
   * an XOR check later */
  if (le) {
    le = FLAG_LE;
  }
  
  /* Different handling depending on alignment */
  if ((pos & 0x7) == 0) {
    /* Copy and recast */
    memcpy(bb, &v, 8);
    
    /* Map the last byte into the window, which also checks parameters
     * and makes sure that the integer doesn't run beyond the end of the
     * file */
    mapByte(pv, pos + 7);
    
    /* Check that not read-only */
    if (pv->flags & FLAG_RO) {
      fault(__LINE__);
    }
    
    /* Write the bytes, flipping if platform endianness and requested
     * endianness are different */
    if ((le ^ pv->flags) & FLAG_LE) {
      (pv->pw)[pos - pv->wfirst] = bb[7];
      (pv->pw)[pos - pv->wfirst + 1] = bb[6];
      (pv->pw)[pos - pv->wfirst + 2] = bb[5];
      (pv->pw)[pos - pv->wfirst + 3] = bb[4];
      (pv->pw)[pos - pv->wfirst + 4] = bb[3];
      (pv->pw)[pos - pv->wfirst + 5] = bb[2];
      (pv->pw)[pos - pv->wfirst + 6] = bb[1];
      (pv->pw)[pos - pv->wfirst + 7] = bb[0];
    } else {
      (pv->pw)[pos - pv->wfirst] = bb[0];
      (pv->pw)[pos - pv->wfirst + 1] = bb[1];
      (pv->pw)[pos - pv->wfirst + 2] = bb[2];
      (pv->pw)[pos - pv->wfirst + 3] = bb[3];
      (pv->pw)[pos - pv->wfirst + 4] = bb[4];
      (pv->pw)[pos - pv->wfirst + 5] = bb[5];
      (pv->pw)[pos - pv->wfirst + 6] = bb[6];
      (pv->pw)[pos - pv->wfirst + 7] = bb[7];
    }
    
    /* Set dirty and update timestamp flags */
    pv->flags |= FLAG_DT;
    pv->flags |= FLAG_UT;
  
  } else {
    /* Unaligned, so copy and recast value into word buffer */
    memcpy(bw, &v, 8);
    
    /* Decompose call, flipping order of results if platform endianness
     * and requested endianness are different */
    if ((le ^ pv->flags) & FLAG_LE) {
      aksview_write32u(pv, pos, le, bw[1]);
      aksview_write32u(pv, pos + 4, le, bw[0]);
    } else {
      aksview_write32u(pv, pos, le, bw[0]);
      aksview_write32u(pv, pos + 4, le, bw[1]);
    }
  }
}

/*
 * aksview_write64s function.
 */
void aksview_write64s(AKSVIEW *pv, int64_t pos, int le, int64_t v) {
  uint8_t bb[8];
  uint32_t bw[2];
  
  /* Rough check of parameters */
  if ((pos < 0) || (pos >= AKSVIEW_MAXLEN) || (pv == NULL)) {
    fault(__LINE__);
  }
  
  /* If le parameter is non-zero, replace it with FLAG_LE so we can do
   * an XOR check later */
  if (le) {
    le = FLAG_LE;
  }
  
  /* Different handling depending on alignment */
  if ((pos & 0x7) == 0) {
    /* Copy and recast */
    memcpy(bb, &v, 8);
    
    /* Map the last byte into the window, which also checks parameters
     * and makes sure that the integer doesn't run beyond the end of the
     * file */
    mapByte(pv, pos + 7);
    
    /* Check that not read-only */
    if (pv->flags & FLAG_RO) {
      fault(__LINE__);
    }
    
    /* Write the bytes, flipping if platform endianness and requested
     * endianness are different */
    if ((le ^ pv->flags) & FLAG_LE) {
      (pv->pw)[pos - pv->wfirst] = bb[7];
      (pv->pw)[pos - pv->wfirst + 1] = bb[6];
      (pv->pw)[pos - pv->wfirst + 2] = bb[5];
      (pv->pw)[pos - pv->wfirst + 3] = bb[4];
      (pv->pw)[pos - pv->wfirst + 4] = bb[3];
      (pv->pw)[pos - pv->wfirst + 5] = bb[2];
      (pv->pw)[pos - pv->wfirst + 6] = bb[1];
      (pv->pw)[pos - pv->wfirst + 7] = bb[0];
    } else {
      (pv->pw)[pos - pv->wfirst] = bb[0];
      (pv->pw)[pos - pv->wfirst + 1] = bb[1];
      (pv->pw)[pos - pv->wfirst + 2] = bb[2];
      (pv->pw)[pos - pv->wfirst + 3] = bb[3];
      (pv->pw)[pos - pv->wfirst + 4] = bb[4];
      (pv->pw)[pos - pv->wfirst + 5] = bb[5];
      (pv->pw)[pos - pv->wfirst + 6] = bb[6];
      (pv->pw)[pos - pv->wfirst + 7] = bb[7];
    }
    
    /* Set dirty and update timestamp flags */
    pv->flags |= FLAG_DT;
    pv->flags |= FLAG_UT;
  
  } else {
    /* Unaligned, so copy and recast value into word buffer */
    memcpy(bw, &v, 8);
    
    /* Decompose call, flipping order of results if platform endianness
     * and requested endianness are different */
    if ((le ^ pv->flags) & FLAG_LE) {
      aksview_write32u(pv, pos, le, bw[1]);
      aksview_write32u(pv, pos + 4, le, bw[0]);
    } else {
      aksview_write32u(pv, pos, le, bw[0]);
      aksview_write32u(pv, pos + 4, le, bw[1]);
    }
  }
}
