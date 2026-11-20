/*
 * No-registry flavour of the platform registry interface: used on
 * Win32s, where there is no registry at all (the flat Reg* functions
 * cannot express PuTTY's hierarchical settings storage, and the whole
 * *Ex API is unimplemented). Settings are stored in files instead;
 * see windows/storage-file.c.
 */

#include "putty.h"

HKEY open_regkey_ro(HKEY base, const char *path, ...)
{
    return NULL;
}

HKEY open_regkey_rw(HKEY base, const char *path, ...)
{
    return NULL;
}

HKEY create_regkey(HKEY base, const char *path, ...)
{
    return NULL;
}

void close_regkey(HKEY key)
{
}

void del_regkey(HKEY key, const char *name)
{
}

char *enum_regkey(HKEY key, int index)
{
    return NULL;
}

bool get_reg_dword(HKEY key, const char *name, DWORD *out)
{
    return false;
}

bool put_reg_dword(HKEY key, const char *name, DWORD value)
{
    return false;
}

char *get_reg_sz(HKEY key, const char *name)
{
    return NULL;
}

bool put_reg_sz(HKEY key, const char *name, const char *str)
{
    return false;
}

strbuf *get_reg_multi_sz(HKEY key, const char *name)
{
    return NULL;
}

bool put_reg_multi_sz(HKEY key, const char *name, strbuf *str)
{
    return false;
}

char *get_reg_sz_simple(HKEY key, const char *name, const char *leaf)
{
    return NULL;
}
