#ifndef AKSVIEW_H_INCLUDED
#define AKSVIEW_H_INCLUDED

/*
 * aksview.h
 * =========
 * 
 * AKSView library.
 * 
 * See the README.md file for further information.
 */

#include <stddef.h>
#include <stdint.h>

/*
 * The maximum allowed length for a file.
 * 
 * This is just to prevent overflow errors while working with offsets.
 * The limit given here is so huge (more than one million terabytes)
 * that you shouldn't ever hit it.
 */
#define AKSVIEW_MAXLEN (INT64_MAX / 2)

/*
 * The default window size hint in bytes.
 * 
 * This is the hint that is always used initially when viewer objects
 * are constructed.
 */
#define AKSVIEW_DEFAULT_HINT (INT32_C(16777216))

/*
 * Structure prototype for AKSVIEW.
 * 
 * Definition given in the implementation file.
 */
struct AKSVIEW_TAG;
typedef struct AKSVIEW_TAG AKSVIEW;

/*
 * Flags used for aksview_create().
 */
#define AKSVIEW_RDONLY  (1)
#define AKSVIEW_RDWR    (2)
#define AKSVIEW_CREAT   (4)
#define AKSVIEW_EXCL    (8)

/*
 * Error code definitions.
 * 
 * Use aksview_errstr() to convert these to error messages.
 */
#define AKSVIEW_ERR_NONE  (0)

/*
 * Given an error code, return an error message for it.
 * 
 * If AKSVIEW_ERR_NONE is passed, "No error" is returned.  If an
 * unrecognized code is passed, "Unknown error" is returned.
 * 
 * The error message is statically allocated and should not be freed.
 * 
 * Parameters:
 * 
 *   code - the error code
 * 
 * Return:
 * 
 *   an error message for that code
 */
const char *aksview_errstr(int code);

/*
 * Create a new viewer object on a given file.
 * 
 * pPath is the path to the file to open.  If this is Windows and you
 * are building in Unicode mode (UNICODE and _UNICODE defined), the path
 * should be in UTF-8 and it will be translated automatically to UTF-16,
 * with an error occurring if it there is any translation error.  On
 * POSIX and Windows in ANSI mode, the path is passed straight through
 * to the operating system.
 * 
 * (If you get translation errors, check that AKSView and the client
 * application are both compiled in the same mode.)
 * 
 * flags must be one of the following four combinations:
 * 
 *   (1) AKSVIEW_RDONLY
 *   (2) AKSVIEW_RDWR
 *   (3) AKSVIEW_RDWR | AKSVIEW_CREAT
 *   (4) AKSVIEW_RDWR | AKSVIEW_CREAT | AKSVIEW_EXCL
 * 
 * Unrecognized flags are ignored.
 * 
 * In modes (1) and (2), the file must already exist or the function
 * fails.  The two modes differ in that (1) is read-only while (2)
 * allows for writing.
 * 
 * In mode (3), a new file will be created if it doesn't exist.  If a
 * file already does exist, the function will attempt to overwrite it
 * and clear its length to zero.
 * 
 * In mode (4), the file must not already exist.  It will be created by
 * this function.
 * 
 * perr is optionally a pointer to an integer that will receive an error
 * code.  If there is no error, AKSVIEW_ERR_NONE (0) is written.
 * Otherwise, one of the other AKSVIEW_ERR_ constants is written.  You
 * can use aksview_errstr() to get an error message from the code.  If
 * NULL is passed, no error code is returned and you can check whether
 * the function succeeded from the return value.
 * 
 * If a viewer object is successfully opened, you should close it
 * eventually with aksview_close().
 * 
 * This function will cache the system page size in the constructed
 * object.  If the system page size is not a multiple of eight bytes,
 * this function will fail with an error.
 * 
 * Parameters:
 * 
 *   pPath - path to the file to create or open
 * 
 *   flags - option flags
 * 
 *   perr - pointer to error code variable or NULL
 * 
 * Return:
 * 
 *   a new viewer object or NULL if the function failed
 */
AKSVIEW *aksview_create(const char *pPath, int flags, int *perr);

/*
 * Close a viewer object.
 * 
 * If NULL is passed, nothing is done.
 * 
 * Closing the viewer object will automatically invoke aksview_flush()
 * before the object is closed down.
 * 
 * Parameters:
 * 
 *   pv - the viewer object, or NULL
 */
void aksview_close(AKSVIEW *pv);

/*
 * Check whether a given viewer object supports read-write operation.
 * 
 * Parameters:
 * 
 *   pv - the viewer object
 * 
 * Return:
 * 
 *   non-zero for read-write, zero for read-only
 */
int aksview_writable(AKSVIEW *pv);

/*
 * Get the length in bytes of a file opened in a viewer.
 * 
 * The return value can be zero or greater.  It will not exceed
 * AKSVIEW_MAXLEN.  The value is cached, so this function is fast and
 * doesn't involve any system call.
 * 
 * Parameters:
 * 
 *   pv - the viewer object
 * 
 * Return:
 * 
 *   the length in bytes of the viewed file
 */
int64_t aksview_getlen(AKSVIEW *pv);

/*
 * Set the length in bytes of a file opened in a viewer.
 * 
 * A fault occurs if you call this on a viewer object that was opened
 * read-only.
 * 
 * newlen is the new length of the file, which can be zero or greater.
 * It must not exceed AKSVIEW_MAXLEN or a fault occurs.  This function
 * will check whether the given new length matches the current length,
 * and do nothing if that is the case.
 * 
 * If the length of a file is reduced, data is dropped from the end of
 * the file.  If the length of a file is increased, data is added to the
 * end of the file.  The content of this new data is undefined.
 * 
 * Changing the file length always unmaps any mapped file view, so
 * changing the file length has significant overhead.  A change in file
 * size will cause the window size to be recalculated because the window
 * size depends on both the hint and the file size.  See aksview_sethint
 * for further information.
 * 
 * If the function fails, the length of the file is unchanged.
 * 
 * Parameters:
 * 
 *   pv - the viewer object
 * 
 *   newlen - the new file length in bytes
 * 
 * Return:
 * 
 *   non-zero if successful, zero if error
 */
int aksview_setlen(AKSVIEW *pv, int64_t newlen);

/*
 * Change the window size hint of the viewer.
 * 
 * wlen is the new window hint in bytes.  It may have any value.  (If
 * it is zero or negative, it will effectively make the windows as small
 * as possible, but note that this is usually a bad idea.)
 * 
 * Initially, viewer objects always have a hint of AKSVIEW_DEFAULT_HINT.
 * No memory map is allocated initially, so if you call aksview_sethint
 * right away you can change the hint before anything is mapped.
 * 
 * If the new hint is equal in size to the current hint, this function
 * call is ignored.
 * 
 * You can call this function at any time on a viewer object, but be
 * careful.  Changing the hint might unmap any view that is currently
 * mapped, so there may be significant overhead.
 * 
 * To determine the actual window size, first adjust the hint to the
 * maximum of the system page size and the original hint, so that it is
 * at least the system page size.  Then, round the adjusted hint upwards
 * if necessary so that it is aligned at a system page boundary.
 * Finally, take the minimum of the adjusted and rounded hint, and the
 * size of the underlying file, so that the window's size does not
 * exceed the actual length of the file.
 * 
 * The result of this process will be a window size of zero if the
 * underlying file is empty.  This is OK, because the file will never be
 * mapped in that case.
 * 
 * If the new window size is different than the current window size, any
 * currently mapped views are unmapped.  Also, changing the file size
 * with aksview_setlen() will automatically cause the window size to be
 * recomputed from the hint.
 * 
 * With memory mapping, the best strategy is to have large windows, and
 * preferably to have the whole file fit in a single window.  However,
 * if you have huge files or multiple files mapped, you have to be
 * careful not to exhaust the process address space.
 * 
 * Parameters:
 * 
 *   pv - the viewer object
 * 
 *   wlen - the new window hint in bytes
 */
void aksview_sethint(AKSVIEW *pv, int32_t wlen);

/*
 * Flush any changes out to disk.
 * 
 * This call is ignored if you call it on a read-only viewer.
 * 
 * Viewer objects are automatically flushed before they are closed.
 * 
 * Parameters:
 * 
 *   pv - the viewer object
 */
void aksview_flush(AKSVIEW *pv);

/*
 * The load and store functions.
 * 
 * The following documentation applies to all of the 16 functions.
 * 
 * All functions require a pos parameter that indicates the file offset
 * of the first byte of the integer being loaded or stored.  This file
 * offset must be greater than or equal to zero.  Also, the offset plus
 * the size in bytes of the requested integer size must be less than or
 * equal to the size of the file.  A fault occurs if the integer
 * location indicated by the given pos is not fully within the
 * boundaries of the file.
 * 
 * Also, the functions are significantly more efficient when accessing
 * aligned integers.  That is, if you are accessing a 16-bit integer,
 * it is aligned if pos is divisible by two; if you are accessing a
 * 32-bit integer, it is aligned if pos is divisible by four; and if you
 * are accessing a 64-bit integer, it is aligned if pos is divisible by
 * eight.  (8-bit integers are always aligned.)
 * 
 * Unaligned access is allowed, but the call will be automatically
 * decomposed into multiple aligned access calls, which is less
 * efficient.
 * 
 * All functions beyond the 8-bit functions have an le parameter that is
 * non-zero to select little endian, or zero to select big endian.  In
 * big endian (also known as network order), the most significant byte
 * of the integer is first and the least significant byte is last.  In
 * little endian, the least significant byte of the integer is first and
 * the most significant byte is last.
 * 
 * Signed values are stored in two's-complement format in the file,
 * which is the usual signed format for integers.
 * 
 * The write functions may not be used with read-only viewer objects or
 * a fault will occur.
 * 
 * Note that memory mapping will not actually write changes to the file
 * right away.  To ensure changes are actually written, use
 * aksview_flush(), or close the viewer object, which will automatically
 * call aksview_flush() before closing down.
 * 
 * Parameters:
 * 
 *   pv - the viewer object
 * 
 *   pos - the file offset to read or write at
 * 
 *   le - (16-bit, 32-bit, 64-bit functions only) non-zero for little
 *   endian, zero for big endian
 * 
 *   v - (write functions only) the value to store
 * 
 * Return:
 * 
 *   (read functions only) the value that was loaded
 */
 uint8_t aksview_read8u(  AKSVIEW *pv, int64_t pos);
  int8_t aksview_read8s(  AKSVIEW *pv, int64_t pos);
    void aksview_write8u( AKSVIEW *pv, int64_t pos, uint8_t v);
    void aksview_write8s( AKSVIEW *pv, int64_t pos,  int8_t v);

uint16_t aksview_read16u( AKSVIEW *pv, int64_t pos, int le);
 int16_t aksview_read16s( AKSVIEW *pv, int64_t pos, int le);
    void aksview_write16u(AKSVIEW *pv, int64_t pos, int le, uint16_t v);
    void aksview_write16s(AKSVIEW *pv, int64_t pos, int le,  int16_t v);

uint32_t aksview_read32u( AKSVIEW *pv, int64_t pos, int le);
 int32_t aksview_read32s( AKSVIEW *pv, int64_t pos, int le);
    void aksview_write32u(AKSVIEW *pv, int64_t pos, int le, uint32_t v);
    void aksview_write32s(AKSVIEW *pv, int64_t pos, int le,  int32_t v);

uint64_t aksview_read64u( AKSVIEW *pv, int64_t pos, int le);
 int64_t aksview_read64s( AKSVIEW *pv, int64_t pos, int le);
    void aksview_write64u(AKSVIEW *pv, int64_t pos, int le, uint64_t v);
    void aksview_write64s(AKSVIEW *pv, int64_t pos, int le,  int64_t v);

#endif
