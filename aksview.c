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

#define AKS_TRANSLATE
#include "aksmacro.h"

/* On POSIX, check that 64-bit file offsets enabled */
#ifdef AKS_POSIX
#ifdef _FILE_OFFSET_BITS
#if (_FILE_OFFSET_BITS != 64)
#error aksview: must declare _FILE_OFFSET_BITS=64
#endif
#else
#error aksview: must declare _FILE_OFFSET_BITS=64
#endif
#endif

/* OS-specific headers */
#ifdef AKS_WIN
/* Windows headers */
#include <windows.h>

#else
/* POSIX headers */
#include <fcntl.h>
#include <sys/mman.h>
#endif

/*
 * Constants
 * =========
 */

/*
 * Flags that can be used in the flags field of AKSVIEW structure.
 */
#define FLAG_RO (1)   /* Read-only */

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
#ifdef AKS_WIN
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
static void unview(AKSVIEW *pv);
static void mapByte(AKSVIEW *pv, int64_t b);

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
