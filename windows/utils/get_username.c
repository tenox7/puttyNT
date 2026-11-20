/*
 * Implementation of get_username() for Windows.
 */

#include "putty.h"

char *get_username(void)
{
#ifdef WIN32S_COMPAT
    return dupstr("win32s");
#else
    DWORD namelen;
    char *user;

    namelen = 0;
    if (!GetUserName(NULL, &namelen)) {
        /*
         * Apparently this doesn't work at least on Windows XP SP2.
         * Thus assume a maximum of 256. It will fail again if it
         * doesn't fit.
         */
        namelen = 256;
    }

    user = snewn(namelen, char);
    if (!GetUserName(user, &namelen)) {
        sfree(user);
        return NULL;
    }
    return user;
#endif
}
