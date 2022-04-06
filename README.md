# AKSView

Memory-mapped file viewer module of Arctic Kernel Services (AKS).  This module (`libaksview`, known henceforth as AKSView) provides an alternative file I/O mechanism that may work better than the standard C library `<stdio.h>` in certain cases.  AKSView does not use `<stdio.h>` at all, relying instead on the memory-mapping facility of the underlying operating system.  AKSView is compatible with both Windows and POSIX platforms.

AKSView should work in multithreaded environments provided that no viewer object is used at the same time from two different threads.  However, trying to pass viewer objects across process boundaries should be avoided, as this may have different behavior depending on how the underlying platform handles memory mapping.

## Compilation

The only dependency is on `aksmacro` for portability.  Since `aksmacro` is a macro library consisting of only a single header, all you need to do is make sure that `aksmacro.h` is in the include path when compiling AKSView.

There are two compilation strategies, documented in the subsections below.

### Compiling together with the client application

The first compilation strategy is to compile the `aksview.c` source file directly together with the rest of the application.  To do this, you need to make sure that the `aksview.h` header and the dependency `aksmacro.h` are in the include path, and then specify `aksview.c` as one of the modules that you are compiling, just as if it were another module in your application.

AKSView should be able to automatically detect whether it is being built on POSIX or Windows using the `aksmacro` header.  If for some reason it does not detect this correctly, you can manually define either `AKS_POSIX` or `AKS_WIN` while compiling to force the correct decision.

On POSIX only, you must define `_FILE_OFFSET_BITS=64` or else you will get a compilation error indicating that this definition is required.

On Windows, by default AKSView will be built in ANSI mode, which means that no translation macros are required, but you may not be able to access file paths that include Unicode characters.  If you define both `UNICODE` and `_UNICODE` then AKSView will be built in Unicode mode and automatically translate string parameters from UTF-8 to UTF-16 before passing them to Windows.  This allows for full support of Unicode file paths, but your application should then use the `aksmacro` translation macros consistently and also have a translated `maint` function so that Unicode parameters are correctly translated from UTF-16 into UTF-8.  (See `aksmacro` for further information.)

### Compiling as a static library

The second compilation strategy is compile AKSView as a static library that can then be included just like any other static library.

When compiling the object file for `aksview.c`, you will need to make sure that both `aksview.h` and `aksmacro.h` are in the include path.  As with the previous compilation strategy, the platform should be automatically detected, but you can manually override this decision by specifying either `AKS_POSIX` or `AKS_WIN` during compilation.  Also like the previous compilation strategy, you must define `_FILE_OFFSET_BITS=64` when building on POSIX.

On Windows, by default the static library will be built in ANSI mode, but you can build a Unicode mode library by specifying both `UNICODE` and `_UNICODE` while building, as explained in more detail in the previous section.

__Caution:__ On Windows, make sure that the mode chosen for the client application matches the mode chosen for the AKSView static library.  That is, if the client application is built in ANSI mode the AKSView static library should also have been built in ANSI mode, and if the client application is built in Unicode mode the AKSView static library should also have been built in Unicode mode.  If the build modes do not match, errors may occur unpredictably when opening file paths because one is performing UTF-8 translation on the path while the other is not.  For Windows, you should probably have two AKSView static library builds, one for ANSI and the other for Unicode, and be sure that you link the correct one to your client application.

## Viewer objects

AKSView uses `AKSVIEW *` pointers as handles.  You can create a handle to a new viewer object by using the following constructor:

    AKSVIEW *aksview_create(const char *pPath, int mode, int *perr);

The `pPath` parameter is the path to a file that you want to open.  It's best to stay with regular disk files here, as attempting to memory-map devices and files on network drives may have platform-specific behavior.  There is no way to map standard input, standard output, or standard error.  On POSIX and Windows in ANSI mode, the given path will be passed through to the operating system.  On Windows in Unicode mode, the given path should be in UTF-8, and it will be translated automatically to UTF-16 before being passed through to the operating system; an error will occur if the path is not UTF-8.

The `mode` parameter must be one of the following:

- `AKSVIEW_READONLY` - open existing for read-only
- `AKSVIEW_EXISTING` - open existing for read/write
- `AKSVIEW_REGULAR` - create file if it doesn't exist
- `AKSVIEW_EXCLUSIVE` - file must not already exist

The modes differ by whether the resulting viewer is read-only, whether they can open a file that already exists, and whether they can create a file if none already exists.  The following chart summarizes the differences:

       Mode    | Read-only? | Open existing? | Create new?
    ===========+============+================+=============
     READONLY  |    YES     |      YES       |     no
     EXISTING  |     no     |      YES       |     no
     REGULAR   |     no     |      YES       |     YES
     EXCLUSIVE |     no     |       no       |     YES

None of these options will truncate an existing file to length zero.  If you need to do this, you can easily use `aksview_setlen()`.

On POSIX systems, when a new file is created, the access mode specified is for everyone to have read and write access.  This specified access mode will then automatically be modified by the `umask` associated with the process to disable permissions that shouldn't be granted.

On Windows systems, the sharing mode for the opened file will disable all sharing because sharing doesn't work well with memory mapping, except if the viewer has been opened read-only, in which case read sharing will be permitted.

Finally, the `perr` parameter is an optional pointer to an integer that will receive an error code.  If the function fails, NULL is returned and the `*perr` is set to an error code.  (If the function succeeds, `*perr` is set to zero.)  You can pass NULL as the `perr` parameter if you do not require this additional error information.  The error code can be turned into an error message with the following function:

    const char *aksview_errstr(int code);

The return value is a pointer to a string containing an error message.  If the given code is zero, `No error` is returned.  If the given code is not recognized, `Unknown error` is returned.  The error message is statically allocated; do not attempt to release it.

You can check whether a viewer is read-write or read-only using the following function:

    int aksview_writable(AKSVIEW *pv);

This function returns non-zero for read-write viewers, and zero for read-only viewers.  Certain viewer functions described later will fault if you try to use them on read-only viewers.

You should eventually close each viewer object with the following function:

    void aksview_close(AKSVIEW *pv);

The function call is ignored if NULL is passed.  Otherwise, the viewer object is closed and released.  For viewers that were opened in read-write mode, changes are flushed out to disk before this function returns, using `msync` on POSIX and `FlushViewOfFile` on Windows.  Furthermore, for read-write viewers, the last-modified timestamp might not be updated correctly by memory-mapped operations, so the close function will also explicitly set the last-modified time of the file using `SetFileTime` on Windows and `utime` on POSIX.

## Sizing functions

The most basic viewer operations are to get and set the length of the viewed file.  You can get the current length of the viewed file at any time using the following function:

    int64_t aksview_getlen(AKSVIEW *pv);

The return value is the number of bytes in the file, which can be zero or greater.  The value is cached so that this function should be very quick to call.  The value won't change unless you explicitly change it with `aksview_setlen`.

To set the length of the viewed file, use the following function:

    int aksview_setlen(AKSVIEW *pv, int64_t newlen);

The `newlen` parameter must be zero or greater.  It specifies the new length of the file in bytes.  You can only use this function on read-write handles; a fault will occur if you try this on a read-only handle.  If the new length is shorter than the old length, data will be dropped from the end of the file.  If the new length is longer than the old length, the contents of the file between the old end of the file and the new end of the file are undefined.  The viewer will detect whether the given size is equal to the current size and do nothing in that case.

On Windows, `GetFileSize` is used to detect the initial size of the file and `SetFilePointer` along with `SetEndOfFile` are used to change the length of a file.  On POSIX, `fstat` is used to detect the initial size of the file, and `lseek` followed by a `write` to the new last byte of the file is used to increase the length of a file while `ftruncate` is used to decrease the length of a file.

Memory maps are unmapped during the resizing process, so you should avoid frequent resizing.  If you need a file to get longer and longer, either use a growing strategy such as doubling the file length, or use `<stdio.h>` instead of AKSView, since `<stdio.h>` is much more stream oriented.

## Window hints

Internally, AKSView uses memory mapping to perform fast, random-access I/O with the file.  The viewer divides the file into non-overlapping _windows_.  Only one window can be mapped at a time.  These windows should be large, and it is ideal if the whole file can fit within a single window.  The memory-mapped strategy is not efficient with small windows &mdash; `<stdio.h>` will work better if you are using small buffers.

The _window hint_ of a viewer object gives the viewer a guideline for the approximate maximum size of a window.  By default, this hint is 16 megabytes.  You can change the hint of a viewer at any time with the following function:

    void aksview_sethint(AKSVIEW *pv, int32_t wlen);

The `wlen` parameter gives the new window hint in bytes.  It may have any value.  See the documentation of this function in the header for specifics of how the hint is used to compute the actual window size.

Generally, the larger the hints the better.  The only issue is that if you are working with huge files or have multiple file viewer objects open at the same time, you have to be careful not to exhaust the process address space.

The window is __not__ an actual file buffer, because memory mapping will load and store pages on demand using the virtual memory system.  This is why large windows work quickly.  It is much better to let the highly optimized virtual memory system of the operating system figure out when to load what page than to attempt to implement your own caching system.  The only issue is not exceeding the process address space.

## Load and store functions

AKSView uses a load/store architecture that can access binary integers at any offset within the viewed file, in both big endian and little endian orderings.  The following are the load/store functions:

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

Each function takes a file offset `pos` to the first byte of data in the file to load or store.  This offset and all bytes required to form the binary integer value must be within the limits of the file or a fault occurs.

All functions beyond the 8-bit functions take an `le` parameter that is non-zero to use little endian ordering, or zero to use big endian ordering.

The `v` parameters of all the writing functions are the values to store.  Signed values will be stored in two's complement.

It is significantly faster to load and store at aligned file offset than at unaligned file offsets.  Unaligned file offsets will be automatically decomposed into multiple aligned operations, but this is less efficient.

AKSView requires the system page size to at least be a multiple of eight, so all aligned load and store operations will be contained within a single window.  If the current window does not contain the desired integer or no window is currently loaded, the memory map will be reloaded to position the correct window.

Due to the way memory mapping works, storing something in a viewer object does not necessarily update the disk file right away.  If you want to ensure that all outstanding changes have been actually written to the disk file, you can call the following function:

    void aksview_flush(AKSVIEW *pv);

This function will use `msync` on POSIX and `FlushViewOfFile` on Windows to ensure changes are actually written to disk.  A flush will only be performed if the contents of the file were somehow modified.  When viewer objects are closed, they are automatically flushed.
