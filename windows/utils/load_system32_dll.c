/*
 * Wrapper function to load a DLL out of c:\windows\system32 without
 * going through the full DLL search path. (Hence no attack is
 * possible by placing a substitute DLL earlier on that path.)
 */

#include "putty.h"

HMODULE load_system32_dll(const char *libname)
{
#ifdef WIN32S_COMPAT
    /* Win32s has no system32 directory; just load the library by
     * name and let the loader find it. */
    return LoadLibrary(libname);
#else
    char *fullpath;
    HMODULE ret;

    fullpath = dupcat(get_system_dir(), "\\", libname, NULL);
    ret = LoadLibrary(fullpath);
    sfree(fullpath);
    return ret;
#endif
}
