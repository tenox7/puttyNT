/*
 * Implementation of open_for_write_would_lose_data for Windows.
 *
 * It uses FindFirstFile, which every version of Windows NT has
 * (GetFileAttributesEx came with NT 4.0). FindFirstFile treats '*'
 * and '?' as wildcards, but no file name can contain those anyway.
 */

#include <wchar.h>

#include "putty.h"

#ifndef FILE_ATTRIBUTE_DEVICE
#define FILE_ATTRIBUTE_DEVICE 0x00000040   /* not in the older SDKs */
#endif

bool open_for_write_would_lose_data(const Filename *fn)
{
#ifdef WIN32S_COMPAT
    return false;
#else
    WIN32_FIND_DATAW fd;
    HANDLE h;

    if (wcschr(fn->wpath, L'*') || wcschr(fn->wpath, L'?'))
        return false;

    h = FindFirstFileW(fn->wpath, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        /*
         * Generally, if we don't identify a specific reason why we
         * should return true from this function, we return false, and
         * let the subsequent attempt to open the file for real give a
         * more useful error message.
         */
        return false;
    }
    FindClose(h);

    if (fd.dwFileAttributes & (FILE_ATTRIBUTE_DEVICE|FILE_ATTRIBUTE_DIRECTORY)) {
        /*
         * File is something other than an ordinary disk file, so
         * opening it for writing will not cause truncation. (It may
         * not _succeed_ either, but that's not our problem here!)
         */
        return false;
    }
    if (fd.nFileSizeHigh == 0 && fd.nFileSizeLow == 0) {
        /*
         * File is zero-length (or may be a named pipe, which
         * dwFileAttributes can't tell apart from a regular file), so
         * opening it for writing won't truncate any data away because
         * there's nothing to truncate anyway.
         */
        return false;
    }
    return true;
#endif
}
