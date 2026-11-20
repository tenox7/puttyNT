/*
 * Version of the MessageBox API function that takes its strings in
 * either the system code page or UTF-8 (if 'utf8' is set).
 *
 * helpctxid would name a topic in the help file for a Help button,
 * but this port has no help file.
 */

#include "putty.h"

int message_box(HWND owner, LPCTSTR text, LPCTSTR caption, DWORD style,
                bool utf8, DWORD helpctxid)
{
#ifdef WIN32S_COMPAT
    /* Win32s dialogs are ANSI; the wide-string conversion above is
     * not reliable on that platform. */
    return MessageBoxA(owner, text, caption, style);
#else
    wchar_t *wtext;
    wchar_t *wcaption;
    int toret;

    if (utf8) {
        wtext = decode_utf8_to_wide_string(text);
        wcaption = decode_utf8_to_wide_string(caption);
    } else {
        wtext = dup_mb_to_wc(DEFAULT_CODEPAGE, text);
        wcaption = dup_mb_to_wc(DEFAULT_CODEPAGE, caption);
    }

    toret = MessageBoxW(owner, wtext, wcaption, style);

    sfree(wtext);
    sfree(wcaption);

    return toret;
#endif
}
