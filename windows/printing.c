/*
 * Printing interface for PuTTY.
 */

#include "putty.h"
#include <winspool.h>

struct printer_enum_tag {
    int nprinters;
    LPPRINTER_INFO_1 info;
};

struct printer_job_tag {
    HANDLE hprinter;
};

DECL_WINDOWS_FUNCTION(static, BOOL, EnumPrinters,
                      (DWORD, LPTSTR, DWORD, LPBYTE, DWORD, LPDWORD, LPDWORD));
DECL_WINDOWS_FUNCTION(static, BOOL, OpenPrinter,
                      (LPTSTR, LPHANDLE, LPPRINTER_DEFAULTS));
DECL_WINDOWS_FUNCTION(static, BOOL, ClosePrinter, (HANDLE));
DECL_WINDOWS_FUNCTION(static, DWORD, StartDocPrinter, (HANDLE, DWORD, LPBYTE));
DECL_WINDOWS_FUNCTION(static, BOOL, EndDocPrinter, (HANDLE));
DECL_WINDOWS_FUNCTION(static, BOOL, StartPagePrinter, (HANDLE));
DECL_WINDOWS_FUNCTION(static, BOOL, EndPagePrinter, (HANDLE));
DECL_WINDOWS_FUNCTION(static, BOOL, WritePrinter,
                      (HANDLE, LPVOID, DWORD, LPDWORD));

static void init_winfuncs(void)
{
    static bool initialised = false;
    if (initialised)
        return;
    {
        HMODULE winspool_module = load_system32_dll("winspool.drv");
        /* Some MSDN documentation claims that some of the below functions
         * should be loaded from spoolss.dll, but this doesn't seem to
         * be reliable in practice.
         * Nevertheless, we load spoolss.dll ourselves using our safe
         * loading method, against the possibility that winspool.drv
         * later loads it unsafely. */
#ifndef WIN32S_COMPAT
        (void) load_system32_dll("spoolss.dll");
#endif
        GET_WINDOWS_FUNCTION_PP(winspool_module, EnumPrinters);
        GET_WINDOWS_FUNCTION_PP(winspool_module, OpenPrinter);
        GET_WINDOWS_FUNCTION_PP(winspool_module, ClosePrinter);
        GET_WINDOWS_FUNCTION_PP(winspool_module, StartDocPrinter);
        GET_WINDOWS_FUNCTION_PP(winspool_module, EndDocPrinter);
        GET_WINDOWS_FUNCTION_PP(winspool_module, StartPagePrinter);
        GET_WINDOWS_FUNCTION_PP(winspool_module, EndPagePrinter);
        GET_WINDOWS_FUNCTION_PP(winspool_module, WritePrinter);
    }
    initialised = true;
}

static bool printer_add_enum(int param, DWORD level, char **buffer,
                             int offset, int *nprinters_ptr)
{
    DWORD needed = 0, nprinters = 0;

    init_winfuncs();

    if (!p_EnumPrinters)
        /* No spooler (Win32s): no printers to list */
        return false;

    *buffer = sresize(*buffer, offset+512, char);

    /*
     * Exploratory call to EnumPrinters to determine how much space
     * we'll need for the output.
     *
     * If we get ERROR_INSUFFICIENT_BUFFER, that's fine, we're
     * prepared to deal with it. Any other error, we return failure.
     */
    if (p_EnumPrinters(param, NULL, level, (LPBYTE)((*buffer)+offset), 512,
                       &needed, &nprinters) == 0 &&
        GetLastError() != ERROR_INSUFFICIENT_BUFFER)
        return false;

    if (needed < 512)
        needed = 512;

    *buffer = sresize(*buffer, offset+needed, char);

    if (p_EnumPrinters(param, NULL, level, (LPBYTE)((*buffer)+offset),
                       needed, &needed, &nprinters) == 0)
        return false;

    *nprinters_ptr += nprinters;

    return true;
}

printer_enum *printer_start_enum(int *nprinters_ptr)
{
    printer_enum *pe = snew(printer_enum);
    char *buffer = NULL;

    *nprinters_ptr = 0;                /* default return value */
    buffer = snewn(512, char);

    /*
     * Enumeration level 1 is the one every version of Windows NT has.
     * (Level 4, which is quicker on a network with many printers,
     * arrived with Windows NT 3.5.)
     */
    if (!printer_add_enum(PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS,
                          1, &buffer, 0, nprinters_ptr))
        goto error;

    pe->info = (LPPRINTER_INFO_1)buffer;
    pe->nprinters = *nprinters_ptr;

    return pe;

  error:
    sfree(buffer);
    sfree(pe);
    *nprinters_ptr = 0;
    return NULL;
}

char *printer_get_name(printer_enum *pe, int i)
{
    if (!pe)
        return NULL;
    if (i < 0 || i >= pe->nprinters)
        return NULL;
    return pe->info[i].pName;
}

void printer_finish_enum(printer_enum *pe)
{
    if (!pe)
        return;
    sfree(pe->info);
    sfree(pe);
}

printer_job *printer_start_job(char *printer)
{
    printer_job *pj = snew(printer_job);
    DOC_INFO_1 docinfo;
    bool jobstarted = false, pagestarted = false;

    init_winfuncs();

    pj->hprinter = NULL;
    if (!p_OpenPrinter || !p_StartDocPrinter || !p_StartPagePrinter
        || !p_EndPagePrinter || !p_EndDocPrinter || !p_ClosePrinter)
        goto error;
    if (!p_OpenPrinter(printer, &pj->hprinter, NULL))
        goto error;

    docinfo.pDocName = "PuTTY remote printer output";
    docinfo.pOutputFile = NULL;
    docinfo.pDatatype = "RAW";

    if (!p_StartDocPrinter(pj->hprinter, 1, (LPBYTE)&docinfo))
        goto error;
    jobstarted = true;

    if (!p_StartPagePrinter(pj->hprinter))
        goto error;
    pagestarted = true;

    return pj;

  error:
    if (pagestarted)
        p_EndPagePrinter(pj->hprinter);
    if (jobstarted)
        p_EndDocPrinter(pj->hprinter);
    if (pj->hprinter)
        p_ClosePrinter(pj->hprinter);
    sfree(pj);
    return NULL;
}

void printer_job_data(printer_job *pj, const void *data, size_t len)
{
    DWORD written;

    if (!pj)
        return;

    if (p_WritePrinter)
        p_WritePrinter(pj->hprinter, (void *)data, len, &written);
}

void printer_finish_job(printer_job *pj)
{
    if (!pj)
        return;

    if (p_EndPagePrinter)
        p_EndPagePrinter(pj->hprinter);
    if (p_EndDocPrinter)
        p_EndDocPrinter(pj->hprinter);
    if (p_ClosePrinter)
        p_ClosePrinter(pj->hprinter);
    sfree(pj);
}
