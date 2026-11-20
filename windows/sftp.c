/*
 * sftp.c: the Windows-specific parts of PSFTP and PSCP.
 */

#include <assert.h>

#include "putty.h"
#include "psftp.h"
#include "ssh.h"
#include "security-api.h"

#ifndef WINSFTP_BUILD
SeatPromptResult filexfer_get_userpass_input(Seat *seat, prompts_t *p)
{
    /* The file transfer tools don't support Restart Session, so we
     * can just have a single static cmdline_get_passwd_input_state
     * that's never reset */
    static cmdline_get_passwd_input_state cmdline_state =
        CMDLINE_GET_PASSWD_INPUT_STATE_INIT;

    SeatPromptResult spr;
    spr = cmdline_get_passwd_input(p, &cmdline_state, false);
    if (spr.kind == SPRK_INCOMPLETE)
        spr = console_get_userpass_input(p);
    return spr;
}

void platform_get_x11_auth(struct X11Display *display, Conf *conf)
{
    /* Do nothing, therefore no auth. */
}
const bool platform_uses_x11_unix_by_default = true;
#endif /* !WINSFTP_BUILD */

/* ----------------------------------------------------------------------
 * File access abstraction.
 */

/*
 * Set local current directory. Returns NULL on success, or else an
 * error message which must be freed after printing.
 */
char *psftp_lcd(char *dir)
{
    char *ret = NULL;

    if (!SetCurrentDirectory(dir)) {
#ifdef WIN32S_COMPAT
        /* FormatMessage is not supported on Win32s */
        ret = dupprintf("unable to change directory: %s",
                        win_strerror(GetLastError()));
#else
        LPVOID message;
        int i;
        FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER |
                      FORMAT_MESSAGE_FROM_SYSTEM |
                      FORMAT_MESSAGE_IGNORE_INSERTS,
                      NULL, GetLastError(),
                      MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                      (LPTSTR)&message, 0, NULL);
        i = strcspn((char *)message, "\n");
        ret = dupprintf("%.*s", i, (LPCTSTR)message);
        LocalFree(message);
#endif
    }

    return ret;
}

/*
 * Get local current directory. Returns a string which must be
 * freed.
 */
char *psftp_getcwd(void)
{
    char *ret = snewn(256, char);
    size_t len = GetCurrentDirectory(256, ret);
    if (len > 256)
        ret = sresize(ret, len, char);
    GetCurrentDirectory(len, ret);
    return ret;
}

/* A FILETIME counts 100ns ticks from 1601-01-01; Unix time counts
 * seconds from 1970-01-01, which is 11644473600 seconds later */
#define EPOCH_DIFF uint64_make(0x2, 0xB6109100)

static FILETIME time_posix_to_win(unsigned long t)
{
    uint64 secs = uint64_add(uint64_make(0, t), EPOCH_DIFF);
    uint64 ticks = uint64_mul32(secs.lo, 10000000);
    FILETIME ft;
    ticks.hi += secs.hi * 10000000;    /* secs < 2^35: no overflow */
    ft.dwLowDateTime = ticks.lo;
    ft.dwHighDateTime = ticks.hi;
    return ft;
}

static unsigned long time_win_to_posix(FILETIME ft)
{
    uint64 ticks = uint64_make(ft.dwHighDateTime, ft.dwLowDateTime);
    /* 10000000 ticks per second, divided in two steps of at most 65535 */
    uint64 secs = uint64_divmod(uint64_divmod(ticks, 10000, NULL), 1000, NULL);
    return uint64_sub(secs, EPOCH_DIFF).lo;
}

struct RFile {
    HANDLE h;
};

RFile *open_existing_file(const char *name, uint64 *size,
                          unsigned long *mtime, unsigned long *atime,
                          long *perms)
{
    HANDLE h;
    RFile *f;

    h = CreateFile(name, GENERIC_READ, FILE_SHARE_READ, NULL,
                   OPEN_EXISTING, 0, 0);
    if (h == INVALID_HANDLE_VALUE)
        return NULL;

    f = snew(RFile);
    f->h = h;

    if (size) {
        DWORD lo, hi;
        lo = GetFileSize(h, &hi);
        *size = uint64_make(hi, lo);
    }

    if (mtime || atime) {
        FILETIME actime, wrtime;
        GetFileTime(h, NULL, &actime, &wrtime);
        if (atime)
            *atime = time_win_to_posix(actime);
        if (mtime)
            *mtime = time_win_to_posix(wrtime);
    }

    if (perms)
        *perms = -1;

    return f;
}

int read_from_file(RFile *f, void *buffer, int length)
{
    DWORD read;
    if (!ReadFile(f->h, buffer, length, &read, NULL))
        return -1;                     /* error */
    else
        return read;
}

void close_rfile(RFile *f)
{
    CloseHandle(f->h);
    sfree(f);
}

struct WFile {
    HANDLE h;
};

WFile *open_new_file(const char *name, long perms)
{
    HANDLE h;
    WFile *f;

    h = CreateFile(name, GENERIC_WRITE, 0, NULL,
                   CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    if (h == INVALID_HANDLE_VALUE)
        return NULL;

    f = snew(WFile);
    f->h = h;

    return f;
}

WFile *open_existing_wfile(const char *name, uint64 *size)
{
    HANDLE h;
    WFile *f;

    h = CreateFile(name, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                   OPEN_EXISTING, 0, 0);
    if (h == INVALID_HANDLE_VALUE)
        return NULL;

    f = snew(WFile);
    f->h = h;

    if (size) {
        DWORD lo, hi;
        lo = GetFileSize(h, &hi);
        *size = uint64_make(hi, lo);
    }

    return f;
}

int write_to_file(WFile *f, void *buffer, int length)
{
    DWORD written;
    if (!WriteFile(f->h, buffer, length, &written, NULL))
        return -1;                     /* error */
    else
        return written;
}

void set_file_times(WFile *f, unsigned long mtime, unsigned long atime)
{
    FILETIME actime, wrtime;
    actime = time_posix_to_win(atime);
    wrtime = time_posix_to_win(mtime);
    SetFileTime(f->h, NULL, &actime, &wrtime);
}

void close_wfile(WFile *f)
{
    CloseHandle(f->h);
    sfree(f);
}

/* Seek offset bytes through file, from whence, where whence is
   FROM_START, FROM_CURRENT, or FROM_END */
int seek_file(WFile *f, uint64 offset, int whence)
{
    DWORD movemethod;

    switch (whence) {
      case FROM_START:
        movemethod = FILE_BEGIN;
        break;
      case FROM_CURRENT:
        movemethod = FILE_CURRENT;
        break;
      case FROM_END:
        movemethod = FILE_END;
        break;
      default:
        return -1;
    }

    {
        LONG lo = offset.lo, hi = offset.hi;
        SetFilePointer(f->h, lo, &hi, movemethod);
    }

    if (GetLastError() != NO_ERROR)
        return -1;
    else
        return 0;
}

uint64 get_file_posn(WFile *f)
{
    LONG lo, hi = 0;

    lo = SetFilePointer(f->h, 0L, &hi, FILE_CURRENT);
    return uint64_make(hi, lo);
}

int file_type(const char *name)
{
    DWORD attr;
    attr = GetFileAttributes(name);
    /* We know of no `weird' files under Windows. */
    if (attr == (DWORD)-1)
        return FILE_TYPE_NONEXISTENT;
    else if (attr & FILE_ATTRIBUTE_DIRECTORY)
        return FILE_TYPE_DIRECTORY;
    else
        return FILE_TYPE_FILE;
}

struct DirHandle {
    HANDLE h;
    char *name;
};

DirHandle *open_directory(const char *name, const char **errmsg)
{
    HANDLE h;
    WIN32_FIND_DATA fdat;
    char *findfile;
    DirHandle *dir;

    /* Enumerate files in dir `foo'. */
    findfile = dupcat(name, "/*", NULL);
    h = FindFirstFile(findfile, &fdat);
    if (h == INVALID_HANDLE_VALUE) {
        *errmsg = win_strerror(GetLastError());
        return NULL;
    }
    sfree(findfile);

    dir = snew(DirHandle);
    dir->h = h;
    dir->name = dupstr(fdat.cFileName);
    return dir;
}

char *read_filename(DirHandle *dir)
{
    do {

        if (!dir->name) {
            WIN32_FIND_DATA fdat;
            if (!FindNextFile(dir->h, &fdat))
                return NULL;
            else
                dir->name = dupstr(fdat.cFileName);
        }

        assert(dir->name);
        if (dir->name[0] == '.' &&
            (dir->name[1] == '\0' ||
             (dir->name[1] == '.' && dir->name[2] == '\0'))) {
            sfree(dir->name);
            dir->name = NULL;
        }

    } while (!dir->name);

    if (dir->name) {
        char *ret = dir->name;
        dir->name = NULL;
        return ret;
    } else
        return NULL;
}

void close_directory(DirHandle *dir)
{
    FindClose(dir->h);
    if (dir->name)
        sfree(dir->name);
    sfree(dir);
}

int test_wildcard(const char *name, bool cmdline)
{
    HANDLE fh;
    WIN32_FIND_DATA fdat;

    /* First see if the exact name exists. */
    if (GetFileAttributes(name) != (DWORD)-1)
        return WCTYPE_FILENAME;

    /* Otherwise see if a wildcard match finds anything. */
    fh = FindFirstFile(name, &fdat);
    if (fh == INVALID_HANDLE_VALUE)
        return WCTYPE_NONEXISTENT;

    FindClose(fh);
    return WCTYPE_WILDCARD;
}

struct WildcardMatcher {
    HANDLE h;
    char *name;
    char *srcpath;
};

char *stripslashes(const char *str, bool local)
{
    char *p;

    /*
     * On Windows, \ / : are all path component separators.
     */

    if (local) {
        p = strchr(str, ':');
        if (p) str = p+1;
    }

    p = strrchr(str, '/');
    if (p) str = p+1;

    if (local) {
        p = strrchr(str, '\\');
        if (p) str = p+1;
    }

    return (char *)str;
}

WildcardMatcher *begin_wildcard_matching(const char *name)
{
    HANDLE h;
    WIN32_FIND_DATA fdat;
    WildcardMatcher *dir;
    char *last;

    h = FindFirstFile(name, &fdat);
    if (h == INVALID_HANDLE_VALUE)
        return NULL;

    dir = snew(WildcardMatcher);
    dir->h = h;
    dir->srcpath = dupstr(name);
    last = stripslashes(dir->srcpath, true);
    *last = '\0';
    if (fdat.cFileName[0] == '.' &&
        (fdat.cFileName[1] == '\0' ||
         (fdat.cFileName[1] == '.' && fdat.cFileName[2] == '\0')))
        dir->name = NULL;
    else
        dir->name = dupcat(dir->srcpath, fdat.cFileName, NULL);

    return dir;
}

char *wildcard_get_filename(WildcardMatcher *dir)
{
    while (!dir->name) {
        WIN32_FIND_DATA fdat;

        if (!FindNextFile(dir->h, &fdat))
            return NULL;

        if (fdat.cFileName[0] == '.' &&
            (fdat.cFileName[1] == '\0' ||
             (fdat.cFileName[1] == '.' && fdat.cFileName[2] == '\0')))
            dir->name = NULL;
        else
            dir->name = dupcat(dir->srcpath, fdat.cFileName, NULL);
    }

    if (dir->name) {
        char *ret = dir->name;
        dir->name = NULL;
        return ret;
    } else
        return NULL;
}

void finish_wildcard_matching(WildcardMatcher *dir)
{
    FindClose(dir->h);
    if (dir->name)
        sfree(dir->name);
    sfree(dir->srcpath);
    sfree(dir);
}

bool vet_filename(const char *name)
{
    if (strchr(name, '/') || strchr(name, '\\') || strchr(name, ':'))
        return false;

    if (!name[strspn(name, ".")])      /* entirely composed of dots */
        return false;

    return true;
}

bool create_directory(const char *name)
{
    return CreateDirectory(name, NULL) != 0;
}

char *dir_file_cat(const char *dir, const char *file)
{
    ptrlen dir_pl = ptrlen_from_asciz(dir);
    return dupcat(
        dir, (ptrlen_endswith(dir_pl, PTRLEN_LITERAL("\\"), NULL) ||
              ptrlen_endswith(dir_pl, PTRLEN_LITERAL("/"), NULL)) ? "" : "\\",
        file, NULL);
}

/* ----------------------------------------------------------------------
 * Platform-specific network handling.
 */
struct winsftp_cliloop_ctx {
    HANDLE other_event;
    int toret;
};
#ifndef WINSFTP_BUILD
static bool winsftp_cliloop_pre(void *vctx, const HANDLE **extra_handles,
                                size_t *n_extra_handles)
{
    struct winsftp_cliloop_ctx *ctx = (struct winsftp_cliloop_ctx *)vctx;

    if (ctx->other_event != INVALID_HANDLE_VALUE) {
        *extra_handles = &ctx->other_event;
        *n_extra_handles = 1;
    }

    return true;
}
static bool winsftp_cliloop_post(void *vctx, size_t extra_handle_index)
{
    struct winsftp_cliloop_ctx *ctx = (struct winsftp_cliloop_ctx *)vctx;

    if (ctx->other_event != INVALID_HANDLE_VALUE &&
        extra_handle_index == 0)
        ctx->toret = 1;       /* other_event was set */

    return false; /* always run only one loop iteration */
}
int do_eventsel_loop(HANDLE other_event)
{
    struct winsftp_cliloop_ctx ctx[1];
    ctx->other_event = other_event;
    ctx->toret = 0;
    cli_main_loop(winsftp_cliloop_pre, winsftp_cliloop_post, ctx);
    return ctx->toret;
}

/*
 * Wait for some network data and process it.
 */
int ssh_sftp_loop_iteration(void)
{
    return do_eventsel_loop(INVALID_HANDLE_VALUE);
}

/*
 * Read a command line from standard input.
 *
 * A second thread reads it, while this one goes on handling the
 * network, so that we can send keepalives and respond to server
 * events even while waiting at the PSFTP command prompt.
 */
struct command_read_ctx {
    HANDLE event;
    char *line;
};

static DWORD WINAPI command_read_thread(void *param)
{
    struct command_read_ctx *ctx = (struct command_read_ctx *) param;

    ctx->line = fgetline(stdin);

    SetEvent(ctx->event);

    return 0;
}

char *ssh_sftp_get_cmdline(const char *prompt, bool no_fds_ok)
{
    int ret;
    struct command_read_ctx ctx[1];
    DWORD threadid;
    HANDLE hThread;
    int socketstate;

    fputs(prompt, stdout);
    fflush(stdout);

    if (first_socket(&socketstate) == INVALID_SOCKET && no_fds_ok) {
        return fgetline(stdin);        /* very simple */
    }

    /*
     * Create a second thread to read from stdin. Process network
     * and timing events until it terminates.
     */
    ctx->event = CreateEvent(NULL, false, false, NULL);
    ctx->line = NULL;

    hThread = CreateThread(NULL, 0, command_read_thread, ctx, 0, &threadid);
    if (!hThread) {
        CloseHandle(ctx->event);
        fprintf(stderr, "Unable to create command input thread\n");
        cleanup_exit(1);
    }

    do {
        ret = do_eventsel_loop(ctx->event);

        /* do_eventsel_loop can't return an error (unlike
         * ssh_sftp_loop_iteration, which can return -1 if select goes
         * wrong or if the socket doesn't exist). */
        assert(ret >= 0);
    } while (ret == 0);

    CloseHandle(hThread);
    CloseHandle(ctx->event);

    return ctx->line;
}

void platform_psftp_pre_conn_setup(LogPolicy *lp)
{
    winselcli_setup();

    if (restricted_acl()) {
        lp_eventlog(lp, "Running with restricted process ACL");
    }
}

/* ----------------------------------------------------------------------
 * Main program. Parse arguments etc.
 */
int main(int argc, char *argv[])
{
    int ret;
    CmdlineArgList *arglist;

    dll_hijacking_protection();
    enable_dit();

    arglist = cmdline_arg_list_from_GetCommandLineW();
    ret = psftp_main(arglist);

    return ret;
}

#endif /* !WINSFTP_BUILD */
