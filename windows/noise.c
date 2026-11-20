/*
 * Noise generation for PuTTY's cryptographic random number
 * generator.
 */

#include <stdio.h>

#include "putty.h"
#include "ssh.h"
#include "storage.h"

/*
 * The CryptoAPI came with Windows NT 4.0, so it is loaded at run time,
 * and its names are here because older SDKs have no wincrypt.h. A
 * provider handle (HCRYPTPROV) is a ULONG.
 */
#ifndef PROV_RSA_FULL
#define PROV_RSA_FULL 1
#endif
#ifndef CRYPT_VERIFYCONTEXT
#define CRYPT_VERIFYCONTEXT 0xF0000000
#endif
DECL_WINDOWS_FUNCTION(static, BOOL, CryptAcquireContextA,
                      (ULONG *, LPCSTR, LPCSTR, DWORD, DWORD));
DECL_WINDOWS_FUNCTION(static, BOOL, CryptGenRandom,
                      (ULONG, DWORD, BYTE *));
DECL_WINDOWS_FUNCTION(static, BOOL, CryptReleaseContext,
                      (ULONG, DWORD));
static HMODULE wincrypt_module = NULL;

bool win_read_random(void *buf, unsigned wanted)
{
    bool toret = false;
    ULONG crypt_provider;

    if (!wincrypt_module) {
        wincrypt_module = load_system32_dll("advapi32.dll");
        GET_WINDOWS_FUNCTION_NO_TYPECHECK(wincrypt_module,
                                          CryptAcquireContextA);
        GET_WINDOWS_FUNCTION_NO_TYPECHECK(wincrypt_module, CryptGenRandom);
        GET_WINDOWS_FUNCTION_NO_TYPECHECK(wincrypt_module,
                                          CryptReleaseContext);
    }

    if (wincrypt_module && p_CryptAcquireContextA &&
        p_CryptGenRandom && p_CryptReleaseContext &&
        p_CryptAcquireContextA(&crypt_provider, NULL, NULL, PROV_RSA_FULL,
                               CRYPT_VERIFYCONTEXT)) {
        toret = p_CryptGenRandom(crypt_provider, wanted, buf) != 0;
        p_CryptReleaseContext(crypt_provider, 0);
    }

    return toret;
}

/*
 * This function is called once, at PuTTY startup.
 */

void noise_get_heavy(void (*func) (void *, int))
{
    HANDLE srch;
    WIN32_FIND_DATA finddata;
    DWORD pid;
    char winpath[MAX_PATH + 3];
    BYTE buf[32];

    GetWindowsDirectory(winpath, sizeof(winpath));
    strcat(winpath, "\\*");
    srch = FindFirstFile(winpath, &finddata);
    if (srch != INVALID_HANDLE_VALUE) {
        do {
            func(&finddata, sizeof(finddata));
        } while (FindNextFile(srch, &finddata));
        FindClose(srch);
    }

    pid = GetCurrentProcessId();
    func(&pid, sizeof(pid));

    if (win_read_random(buf, sizeof(buf))) {
        func(buf, sizeof(buf));
        smemclr(buf, sizeof(buf));
    }

    read_random_seed(func);
}

/*
 * This function is called on a timer, and it will monitor
 * frequently changing quantities such as the state of physical and
 * virtual memory, the state of the process's message queue, which
 * window is in the foreground, which owns the clipboard, etc.
 */
void noise_regular(void)
{
    HWND w;
    DWORD z;
    POINT pt;
    MEMORYSTATUS memstat;
    FILETIME times[4];

    w = GetCapture();
    random_add_noise(NOISE_SOURCE_CAPTURE, &w, sizeof(w));
    w = GetClipboardOwner();
    random_add_noise(NOISE_SOURCE_CLIPBOARD, &w, sizeof(w));
    z = GetQueueStatus(QS_ALLEVENTS);
    random_add_noise(NOISE_SOURCE_QUEUE, &z, sizeof(z));

    GetCursorPos(&pt);
    random_add_noise(NOISE_SOURCE_CURSORPOS, &pt, sizeof(pt));

#ifdef WIN32S_COMPAT
    memset(&memstat, 0, sizeof(memstat));
#else
    w = GetForegroundWindow();
    random_add_noise(NOISE_SOURCE_FGWINDOW, &w, sizeof(w));
    GlobalMemoryStatus(&memstat);
#endif
    random_add_noise(NOISE_SOURCE_MEMINFO, &memstat, sizeof(memstat));

    GetThreadTimes(GetCurrentThread(), times, times + 1, times + 2,
                   times + 3);
    random_add_noise(NOISE_SOURCE_THREADTIME, &times, sizeof(times));
    GetProcessTimes(GetCurrentProcess(), times, times + 1, times + 2,
                    times + 3);
    random_add_noise(NOISE_SOURCE_PROCTIME, &times, sizeof(times));
}

/*
 * This function is called on every keypress or mouse move, and
 * will add the current Windows time and performance monitor
 * counter to the noise pool. It gets the scan code or mouse
 * position passed in.
 */
void noise_ultralight(NoiseSourceId id, unsigned long data)
{
    DWORD wintime;
    LARGE_INTEGER perftime;

    random_add_noise(id, &data, sizeof(DWORD));

    wintime = GetTickCount();
    random_add_noise(NOISE_SOURCE_TIME, &wintime, sizeof(DWORD));

    if (QueryPerformanceCounter(&perftime))
        random_add_noise(NOISE_SOURCE_PERFCOUNT, &perftime, sizeof(perftime));
}

uint32_t prng_reseed_time_ms(void)
{
    return GetTickCount();
}
