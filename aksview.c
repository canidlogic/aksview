/*
 * aksview.c
 * =========
 * 
 * Implementation of aksview.h
 * 
 * See the header for further information.
 */

#include "aksview.h"
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
#include <unistd.h>
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
 * Local functions
 * ===============
 */

/* Prototypes */
static int isLESystem(void);
static int32_t getPageSize(void);
static int loadFileSize(AKSVIEW *pv);
static void computeWindow(AKSVIEW *pv);

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
    abort();
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
    abort();
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
    abort();
  }
  
  /* Check fh field */
#ifdef AKS_WIN
  if (pv->fh == INVALID_HANDLE_VALUE) {
    abort();
  }
#else
  if (pv->fh == -1) {
    abort();
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
    abort();
  }
  if ((pv->flen < 0) || (pv->flen > AKSVIEW_MAXLEN)) {
    abort();
  }
  if ((pv->pgsize < 8) || ((pv->pgsize & 0x7) != 0)) {
    abort();
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
 * If there is a mapped window, unmap it.
 * 
 * Parameters:
 * 
 *   pv - the viewer object
 */
static void unview(AKSVIEW *pv) {
  
  /* Check parameter */
  if (pv == NULL) {
    abort();
  }
  
  /* Only proceed if a window is mapped */
  if (pv->pw != NULL) {
  
    /* Unmap the view */
#ifdef AKS_WIN
    if (!UnmapViewOfFile(pv->pw)) {
      abort();
    }
#else
    if (munmap(pv->pw, (size_t) (pv->wlast - pv->wfirst + 1))) {
      abort();
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
 * Provided that b is aligned, the whole aligned integer starting at b
 * up to a 64-bit size will be included in the mapped window.
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
    abort();
  }
  if ((b < 0) || (b >= pv->flen)) {
    abort();
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
        abort();
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
      abort();
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
      abort();
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
    abort();
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
      abort();
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
      abort();
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
      abort();
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
      abort();
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
          abort();
        }
        pv->fh = INVALID_HANDLE_VALUE;
      }
#else
      if (pv->fh != -1) {
        if (close(pv->fh)) {
          abort();
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
