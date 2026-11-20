/*
 * PuTTY key generation front end (Windows).
 */

#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

#include "putty.h"
#include "ssh.h"
#include "sshkeygen.h"
#include "licence.h"
#include "security-api.h"
#include "puttygen-rc.h"

#define WM_DONEKEY (WM_APP + 1)

#define DEFAULT_KEY_BITS 2048
#define DEFAULT_ECCURVE_INDEX 0
#define DEFAULT_EDCURVE_INDEX 0

static Filename *cmdline_keyfile = NULL;

/*
 * Print a modal (Really Bad) message box and perform a fatal exit.
 */
void modalfatalbox(const char *fmt, ...)
{
    va_list ap;
    char *stuff;

    va_start(ap, fmt);
    stuff = dupvprintf(fmt, ap);
    va_end(ap);
    MessageBox(NULL, stuff, "PuTTYgen Fatal Error",
               MB_SYSTEMMODAL | MB_ICONERROR | MB_OK);
    sfree(stuff);
    exit(1);
}

/*
 * Print a non-fatal message box and do not exit.
 */
void nonfatal(const char *fmt, ...)
{
    va_list ap;
    char *stuff;

    va_start(ap, fmt);
    stuff = dupvprintf(fmt, ap);
    va_end(ap);
    MessageBox(NULL, stuff, "PuTTYgen Error",
               MB_SYSTEMMODAL | MB_ICONERROR | MB_OK);
    sfree(stuff);
}

/* ----------------------------------------------------------------------
 * ProgressReceiver implementation.
 */

#define PROGRESSRANGE 65535

struct progressphase {
    double startpoint, total;
    /* For exponential phases */
    double exp_probability, exp_current_value;
};

struct progress {
    size_t nphases, phasessize;
    struct progressphase *phases, *currphase;

    double scale;
    HWND progbar;

    ProgressReceiver rec;
};

static ProgressPhase win_progress_add_linear(
    ProgressReceiver *prog, double overall_cost) {
    struct progress *p = container_of(prog, struct progress, rec);
    int phase;
    ProgressPhase ph;

    sgrowarray(p->phases, p->phasessize, p->nphases);
    phase = p->nphases++;

    p->phases[phase].total = overall_cost;

    ph.n = phase;
    return ph;
}

static ProgressPhase win_progress_add_probabilistic(
    ProgressReceiver *prog, double cost_per_attempt, double probability) {
    struct progress *p = container_of(prog, struct progress, rec);
    int phase;
    ProgressPhase ph;

    sgrowarray(p->phases, p->phasessize, p->nphases);
    phase = p->nphases++;

    p->phases[phase].exp_probability = 1.0 - probability;
    p->phases[phase].exp_current_value = 1.0;
    /* Expected number of attempts = 1 / probability of attempt succeeding */
    p->phases[phase].total = cost_per_attempt / probability;

    ph.n = phase;
    return ph;
}

static void win_progress_ready(ProgressReceiver *prog)
{
    struct progress *p = container_of(prog, struct progress, rec);

    double total = 0;
    int i;
    for (i = 0; i < p->nphases; i++) {
        p->phases[i].startpoint = total;
        total += p->phases[i].total;
    }
    p->scale = PROGRESSRANGE / total;

    SendMessage(p->progbar, PROGBAR_SETRANGE, 0,
                MAKELPARAM(0, PROGRESSRANGE));
}

static void win_progress_start_phase(ProgressReceiver *prog,
                                     ProgressPhase phase)
{
    struct progress *p = container_of(prog, struct progress, rec);

    assert(phase.n < p->nphases);
    p->currphase = &p->phases[phase.n];
}

static void win_progress_update(struct progress *p, double phasepos)
{
    double position = (p->currphase->startpoint +
                       p->currphase->total * phasepos);
    position *= p->scale;
    if (position < 0)
        position = 0;
    if (position > PROGRESSRANGE)
        position = PROGRESSRANGE;

    SendMessage(p->progbar, PROGBAR_SETPOS, (WPARAM)position, 0);
}

static void win_progress_report(ProgressReceiver *prog, double progress)
{
    struct progress *p = container_of(prog, struct progress, rec);

    win_progress_update(p, progress);
}

static void win_progress_report_attempt(ProgressReceiver *prog)
{
    struct progress *p = container_of(prog, struct progress, rec);

    p->currphase->exp_current_value *= p->currphase->exp_probability;
    win_progress_update(p, 1.0 - p->currphase->exp_current_value);
}

static void win_progress_report_phase_complete(ProgressReceiver *prog)
{
    struct progress *p = container_of(prog, struct progress, rec);

    win_progress_update(p, 1.0);
}

static const ProgressReceiverVtable win_progress_vt = {
    /* .add_linear = */ win_progress_add_linear,
    /* .add_probabilistic = */ win_progress_add_probabilistic,
    /* .ready = */ win_progress_ready,
    /* .start_phase = */ win_progress_start_phase,
    /* .report = */ win_progress_report,
    /* .report_attempt = */ win_progress_report_attempt,
    /* .report_phase_complete = */ win_progress_report_phase_complete,
};

static void win_progress_initialise(struct progress *p)
{
    p->nphases = p->phasessize = 0;
    p->phases = p->currphase = NULL;
    p->rec.vt = &win_progress_vt;
}

static void win_progress_cleanup(struct progress *p)
{
    sfree(p->phases);
}

struct PassphraseProcStruct {
    char **passphrase;
    char *comment;
};

/*
 * Dialog-box function for the passphrase box.
 */
static BOOL CALLBACK PassphraseProc(HWND hwnd, UINT msg,
                                    WPARAM wParam, LPARAM lParam)
{
    static char **passphrase = NULL;
    struct PassphraseProcStruct *p;

    switch (msg) {
      case WM_INITDIALOG:
        SetForegroundWindow(hwnd);
        SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);

        /*
         * Centre the window.
         */
        {                              /* centre the window */
            RECT rs, rd;
            HWND hw;

            hw = GetDesktopWindow();
            if (GetWindowRect(hw, &rs) && GetWindowRect(hwnd, &rd))
                MoveWindow(hwnd,
                           (rs.right + rs.left + rd.left - rd.right) / 2,
                           (rs.bottom + rs.top + rd.top - rd.bottom) / 2,
                           rd.right - rd.left, rd.bottom - rd.top, true);
        }

        p = (struct PassphraseProcStruct *) lParam;
        passphrase = p->passphrase;
        if (p->comment)
            SetDlgItemText(hwnd, 101, p->comment);
        burnstr(*passphrase);
        *passphrase = dupstr("");
        SetDlgItemText(hwnd, 102, *passphrase);
        return 0;
      case WM_COMMAND:
        switch (LOWORD(wParam)) {
          case IDOK:
            if (*passphrase)
                EndDialog(hwnd, 1);
            else
                MessageBeep(0);
            return 0;
          case IDCANCEL:
            EndDialog(hwnd, 0);
            return 0;
          case 102:                    /* edit box */
            if ((HIWORD(wParam) == EN_CHANGE) && passphrase) {
                burnstr(*passphrase);
                *passphrase = GetDlgItemText_alloc(hwnd, 102);
            }
            return 0;
        }
        return 0;
      case WM_CLOSE:
        EndDialog(hwnd, 0);
        return 0;
    }
    return 0;
}

static void try_get_dlg_item_uint32(HWND hwnd, int id, uint32_t *out)
{
    char buf[128];
    char *end;
    unsigned long val;
    if (!GetDlgItemText(hwnd, id, buf, sizeof(buf)))
        return;

    if (!*buf)
        return;

    val = strtoul(buf, &end, 10);
    if (*end)
        return;

    if ((val >> 16) >> 16)
        return;

    *out = val;
}

static ppk_save_parameters save_params;

struct PPKParams {
    ppk_save_parameters params;
    uint32_t time_passes, time_ms;
};

/*
 * Dialog-box function for the passphrase box.
 */
static BOOL CALLBACK PPKParamsProc(HWND hwnd, UINT msg,
                                   WPARAM wParam, LPARAM lParam)
{
    struct PPKParams *pp;
    char *buf;

    if (msg == WM_INITDIALOG) {
        pp = (struct PPKParams *)lParam;
        SetWindowLong(hwnd, GWL_USERDATA, (LONG)pp);
    } else {
        pp = (struct PPKParams *)GetWindowLong(hwnd, GWL_USERDATA);
    }

    switch (msg) {
      case WM_INITDIALOG:
        SetForegroundWindow(hwnd);
        SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);

        /*
         * Centre the window.
         */
        {                              /* centre the window */
            RECT rs, rd;
            HWND hw;

            hw = GetDesktopWindow();
            if (GetWindowRect(hw, &rs) && GetWindowRect(hwnd, &rd))
                MoveWindow(hwnd,
                           (rs.right + rs.left + rd.left - rd.right) / 2,
                           (rs.bottom + rs.top + rd.top - rd.bottom) / 2,
                           rd.right - rd.left, rd.bottom - rd.top, true);
        }

        CheckRadioButton(hwnd, IDC_PPKVER_2, IDC_PPKVER_3,
                         IDC_PPKVER_2 + (pp->params.fmt_version - 2));

        CheckRadioButton(
            hwnd, IDC_KDF_ARGON2ID, IDC_KDF_ARGON2D,
            (pp->params.argon2_flavour == Argon2id ?    IDC_KDF_ARGON2ID :
             pp->params.argon2_flavour == Argon2i  ?    IDC_KDF_ARGON2I  :
          /* pp->params.argon2_flavour == Argon2d  ? */ IDC_KDF_ARGON2D));

        buf = dupprintf("%"PRIu32, pp->params.argon2_mem);
        SetDlgItemText(hwnd, IDC_ARGON2_MEM, buf);
        sfree(buf);

        if (pp->params.argon2_passes_auto) {
            CheckRadioButton(hwnd, IDC_PPK_AUTO_YES, IDC_PPK_AUTO_NO,
                             IDC_PPK_AUTO_YES);
            buf = dupprintf("%"PRIu32, pp->time_ms);
            SetDlgItemText(hwnd, IDC_ARGON2_TIME, buf);
            sfree(buf);
        } else {
            CheckRadioButton(hwnd, IDC_PPK_AUTO_YES, IDC_PPK_AUTO_NO,
                             IDC_PPK_AUTO_NO);
            buf = dupprintf("%"PRIu32, pp->time_passes);
            SetDlgItemText(hwnd, IDC_ARGON2_TIME, buf);
            sfree(buf);
        }

        buf = dupprintf("%"PRIu32, pp->params.argon2_parallelism);
        SetDlgItemText(hwnd, IDC_ARGON2_PARALLEL, buf);
        sfree(buf);

        return 0;
      case WM_COMMAND:
        switch (LOWORD(wParam)) {
          case IDOK: {
            /* Check these parameters for validity before we adopt
             * them. At this stage we don't have the real passphrase
             * length, so make one up. */
            char *err = ppk_params_bad(&pp->params, true, 64);
            if (err) {
                MessageBox(hwnd, err, "Save parameters invalid",
                           MB_OK | MB_ICONERROR);
            } else {
                EndDialog(hwnd, 1);
            }
            return 0;
          }
          case IDCANCEL:
            EndDialog(hwnd, 0);
            return 0;
          case IDC_PPKVER_2:
            pp->params.fmt_version = 2;
            return 0;
          case IDC_PPKVER_3:
            pp->params.fmt_version = 3;
            return 0;
          case IDC_KDF_ARGON2ID:
            pp->params.argon2_flavour = Argon2id;
            return 0;
          case IDC_KDF_ARGON2I:
            pp->params.argon2_flavour = Argon2i;
            return 0;
          case IDC_KDF_ARGON2D:
            pp->params.argon2_flavour = Argon2d;
            return 0;
          case IDC_ARGON2_MEM:
            try_get_dlg_item_uint32(hwnd, IDC_ARGON2_MEM,
                                    &pp->params.argon2_mem);
            return 0;
          case IDC_PPK_AUTO_YES:
            pp->params.argon2_passes_auto = true;
            buf = dupprintf("%"PRIu32, pp->time_ms);
            SetDlgItemText(hwnd, IDC_ARGON2_TIME, buf);
            sfree(buf);
            return 0;
          case IDC_PPK_AUTO_NO:
            pp->params.argon2_passes_auto = false;
            buf = dupprintf("%"PRIu32, pp->time_passes);
            SetDlgItemText(hwnd, IDC_ARGON2_TIME, buf);
            sfree(buf);
            return 0;
          case IDC_ARGON2_TIME:
            try_get_dlg_item_uint32(hwnd, IDC_ARGON2_TIME,
                                    pp->params.argon2_passes_auto ?
                                    &pp->time_ms : &pp->time_passes);
            return 0;
          case IDC_ARGON2_PARALLEL:
            try_get_dlg_item_uint32(hwnd, IDC_ARGON2_PARALLEL,
                                    &pp->params.argon2_parallelism);
            return 0;
        }
        return 0;
      case WM_CLOSE:
        EndDialog(hwnd, 0);
        return 0;
    }
    return 0;
}

/*
 * Dialog-box function for the Licence box.
 */
static BOOL CALLBACK LicenceProc(HWND hwnd, UINT msg,
                                 WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
      case WM_INITDIALOG: {
        /*
         * Centre the window.
         */
        RECT rs, rd;
        HWND hw;

        hw = GetDesktopWindow();
        if (GetWindowRect(hw, &rs) && GetWindowRect(hwnd, &rd))
            MoveWindow(hwnd,
                       (rs.right + rs.left + rd.left - rd.right) / 2,
                       (rs.bottom + rs.top + rd.top - rd.bottom) / 2,
                       rd.right - rd.left, rd.bottom - rd.top, true);

        SetDlgItemText(hwnd, 1000, LICENCE_TEXT("\r\n\r\n"));
        return 1;
      }
      case WM_COMMAND:
        switch (LOWORD(wParam)) {
          case IDOK:
          case IDCANCEL:
            EndDialog(hwnd, 1);
            return 0;
        }
        return 0;
      case WM_CLOSE:
        EndDialog(hwnd, 1);
        return 0;
    }
    return 0;
}

/*
 * Dialog-box function for the About box.
 */
static BOOL CALLBACK AboutProc(HWND hwnd, UINT msg,
                               WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
      case WM_INITDIALOG:
        /*
         * Centre the window.
         */
        {                              /* centre the window */
            RECT rs, rd;
            HWND hw;

            hw = GetDesktopWindow();
            if (GetWindowRect(hw, &rs) && GetWindowRect(hwnd, &rd))
                MoveWindow(hwnd,
                           (rs.right + rs.left + rd.left - rd.right) / 2,
                           (rs.bottom + rs.top + rd.top - rd.bottom) / 2,
                           rd.right - rd.left, rd.bottom - rd.top, true);
        }

        {
            char *buildinfo_text = buildinfo("\r\n");
            char *text = dupprintf(
                "PuTTYgen\r\n\r\n%s\r\n\r\n%s\r\n\r\n%s",
                ver, buildinfo_text,
                "\251 " SHORT_COPYRIGHT_DETAILS ". All rights reserved.");
            sfree(buildinfo_text);
            SetDlgItemText(hwnd, 1000, text);
            MakeDlgItemBorderless(hwnd, 1000);
            sfree(text);
        }
        return 1;
      case WM_COMMAND:
        switch (LOWORD(wParam)) {
          case IDOK:
          case IDCANCEL:
            EndDialog(hwnd, 1);
            return 0;
          case 101:
            EnableWindow(hwnd, 0);
            DialogBox(hinst, MAKEINTRESOURCE(214), hwnd, LicenceProc);
            EnableWindow(hwnd, 1);
            SetActiveWindow(hwnd);
            return 0;
          case 102:
            /* Load web browser */
            ShellExecute(hwnd, "open",
                         "https://www.chiark.greenend.org.uk/~sgtatham/putty/",
                         0, 0, SW_SHOWDEFAULT);
            return 0;
        }
        return 0;
      case WM_CLOSE:
        EndDialog(hwnd, 1);
        return 0;
    }
    return 0;
}

typedef enum {RSA, DSA, ECDSA, EDDSA} keytype;

/*
 * Thread to generate a key.
 */
struct rsa_key_thread_params {
    HWND progressbar;                  /* notify this with progress */
    HWND dialog;                       /* notify this on completion */
    int key_bits;                      /* bits in key modulus (RSA, DSA) */
    int curve_bits;                    /* bits in elliptic curve (ECDSA) */
    keytype keytype;
    const PrimeGenerationPolicy *primepolicy;
    bool rsa_strong;
    union {
        RSAKey *key;
        struct dsa_key *dsakey;
        struct ecdsa_key *eckey;
        struct eddsa_key *edkey;
    };
};
static DWORD WINAPI generate_key_thread(void *param)
{
    struct rsa_key_thread_params *params =
        (struct rsa_key_thread_params *) param;
    struct progress prog;
    PrimeGenerationContext *pgc;
    prog.progbar = params->progressbar;

    win_progress_initialise(&prog);

    pgc = primegen_new_context(params->primepolicy);

    if (params->keytype == DSA)
        dsa_generate(params->dsakey, params->key_bits, pgc, &prog.rec);
    else if (params->keytype == ECDSA)
        ecdsa_generate(params->eckey, params->curve_bits);
    else if (params->keytype == EDDSA)
        eddsa_generate(params->edkey, params->curve_bits);
    else
        rsa_generate(params->key, params->key_bits, params->rsa_strong,
                     pgc, &prog.rec);

    primegen_free_context(pgc);

    PostMessage(params->dialog, WM_DONEKEY, 0, 0);

    win_progress_cleanup(&prog);

    sfree(params);
    return 0;
}

struct InitialParams {
    int keybutton;
    int primepolicybutton;
    bool rsa_strong;
    FingerprintType fptype;
    int keybits;
    int eccurve_index, edcurve_index;
};

struct MainDlgState {
    bool generation_thread_exists;
    bool key_exists;
    int entropy_got, entropy_required;
    strbuf *entropy;
    ULONG entropy_prev_msgtime;
    int key_bits, curve_bits;
    bool ssh2;
    keytype keytype;
    const PrimeGenerationPolicy *primepolicy;
    bool rsa_strong;
    FingerprintType fptype;
    char **commentptr;                 /* points to key.comment or ssh2key.comment */
    ssh2_userkey ssh2key;
    union {
        RSAKey key;
        struct dsa_key dsakey;
        struct ecdsa_key eckey;
        struct eddsa_key edkey;
    };
    HMENU filemenu, keymenu, cvtmenu;
};

/*
 * Rate limit for incrementing the entropy_got counter.
 *
 * Some pointing devices (e.g. gaming mice) can be set to send
 * mouse-movement events at an extremely high sample rate like 1kHz.
 * In that situation, there's likely to be a strong correlation
 * between the contents of successive movement events, so you have to
 * regard the mouse movements as containing less entropy each.
 *
 * A reasonably simple approach to this is to continue to buffer all
 * mouse data, but limit the rate at which we increment the counter
 * for how much entropy we think we've collected. That way, the user
 * still has to spend time wiggling the mouse back and forth in a way
 * that varies with muscle motions and introduces randomness.
 */
#define ENTROPY_RATE_LIMIT 10 /* in units of GetMessageTime(), i.e. ms */

static void hidemany(HWND hwnd, const int *ids, bool hideit)
{
    while (*ids) {
        ShowWindow(GetDlgItem(hwnd, *ids++), (hideit ? SW_HIDE : SW_SHOW));
    }
}

enum {
    controlidstart = 100,
    IDC_QUIT,
    IDC_TITLE,
    IDC_BOX_KEY,
    IDC_NOKEY,
    IDC_GENERATING,
    IDC_PROGRESS,
    IDC_PKSTATIC, IDC_KEYDISPLAY,
    IDC_CERTSTATIC, IDC_CERTMOREINFO,
    IDC_FPSTATIC, IDC_FINGERPRINT,
    IDC_COMMENTSTATIC, IDC_COMMENTEDIT,
    IDC_PASSPHRASE1STATIC, IDC_PASSPHRASE1EDIT,
    IDC_PASSPHRASE2STATIC, IDC_PASSPHRASE2EDIT,
    IDC_BOX_ACTIONS,
    IDC_GENSTATIC, IDC_GENERATE,
    IDC_LOADSTATIC, IDC_LOAD,
    IDC_SAVESTATIC, IDC_SAVE, IDC_SAVEPUB,
    IDC_BOX_PARAMS,
    IDC_TYPESTATIC, IDC_KEYSSH1, IDC_KEYSSH2RSA, IDC_KEYSSH2DSA,
    IDC_KEYSSH2ECDSA, IDC_KEYSSH2EDDSA,
    IDC_PRIMEGEN_PROB, IDC_PRIMEGEN_MAURER_SIMPLE, IDC_PRIMEGEN_MAURER_COMPLEX,
    IDC_RSA_STRONG,
    IDC_FPTYPE_SHA256, IDC_FPTYPE_MD5,
    IDC_PPK_PARAMS,
    IDC_BITSSTATIC, IDC_BITS,
    IDC_ECCURVESTATIC, IDC_ECCURVE,
    IDC_EDCURVESTATIC, IDC_EDCURVE,
    IDC_NOTHINGSTATIC,
    IDC_ABOUT,
    IDC_IMPORT,
    IDC_EXPORT_OPENSSH_AUTO, IDC_EXPORT_OPENSSH_NEW,
    IDC_EXPORT_SSHCOM,
    IDC_ADDCERT, IDC_REMCERT,
};

static void setupbigedit1(HWND hwnd, RSAKey *key)
{
    char *buffer;
    ShowWindow(GetDlgItem(hwnd, IDC_CERTSTATIC), SW_HIDE);
    ShowWindow(GetDlgItem(hwnd, IDC_CERTMOREINFO), SW_HIDE);
    ShowWindow(GetDlgItem(hwnd, IDC_PKSTATIC), SW_SHOW);
    ShowWindow(GetDlgItem(hwnd, IDC_KEYDISPLAY), SW_SHOW);

    SetDlgItemText(hwnd, IDC_PKSTATIC,
                   "&Public key for pasting into authorized_keys file:");

    buffer = ssh1_pubkey_str(key);
    SetDlgItemText(hwnd, IDC_KEYDISPLAY, buffer);
    sfree(buffer);
}

static void setupbigedit2(HWND hwnd, ssh2_userkey *key)
{
    if (ssh_key_alg(key->key)->is_certificate) {
        ShowWindow(GetDlgItem(hwnd, IDC_CERTSTATIC), SW_SHOW);
        ShowWindow(GetDlgItem(hwnd, IDC_CERTMOREINFO), SW_SHOW);
        ShowWindow(GetDlgItem(hwnd, IDC_PKSTATIC), SW_HIDE);
        ShowWindow(GetDlgItem(hwnd, IDC_KEYDISPLAY), SW_HIDE);

        SetDlgItemText(hwnd, IDC_CERTSTATIC,
                       "This public key contains an OpenSSH certificate.");
    } else {
        char *buffer;
        ShowWindow(GetDlgItem(hwnd, IDC_CERTSTATIC), SW_HIDE);
        ShowWindow(GetDlgItem(hwnd, IDC_CERTMOREINFO), SW_HIDE);
        ShowWindow(GetDlgItem(hwnd, IDC_PKSTATIC), SW_SHOW);
        ShowWindow(GetDlgItem(hwnd, IDC_KEYDISPLAY), SW_SHOW);

        SetDlgItemText(hwnd, IDC_PKSTATIC, "&Public key for pasting into "
                       "OpenSSH authorized_keys file:");

        buffer = ssh2_pubkey_openssh_str(key);
        SetDlgItemText(hwnd, IDC_KEYDISPLAY, buffer);
        sfree(buffer);
    }
}

/*
 * Warn about the obsolescent key file format.
 */
void old_keyfile_warning(void)
{
    static const char mbtitle[] = "PuTTY Key File Warning";
    static const char message[] =
        "You are loading an SSH-2 private key which has an\n"
        "old version of the file format. This means your key\n"
        "file is not fully tamperproof. Future versions of\n"
        "PuTTY may stop supporting this private key format,\n"
        "so we recommend you convert your key to the new\n"
        "format.\n"
        "\n"
        "Once the key is loaded into PuTTYgen, you can perform\n"
        "this conversion simply by saving it again.";

    MessageBox(NULL, message, mbtitle, MB_OK);
}

static const int nokey_ids[] = { IDC_NOKEY, 0 };
static const int generating_ids[] = { IDC_GENERATING, IDC_PROGRESS, 0 };
static const int gotkey_ids_unconditional[] = {
    IDC_FPSTATIC, IDC_FINGERPRINT,
    IDC_COMMENTSTATIC, IDC_COMMENTEDIT,
    IDC_PASSPHRASE1STATIC, IDC_PASSPHRASE1EDIT,
    IDC_PASSPHRASE2STATIC, IDC_PASSPHRASE2EDIT, 0
};
static const int gotkey_ids_conditional[] = {
    IDC_PKSTATIC, IDC_KEYDISPLAY,
    IDC_CERTSTATIC, IDC_CERTMOREINFO,
};

/*
 * Small UI helper function to switch the state of the main dialog
 * by enabling and disabling controls and menu items.
 */
void ui_set_state(HWND hwnd, struct MainDlgState *state, int status)
{
    int type;

    switch (status) {
      case 0:                          /* no key */
        hidemany(hwnd, nokey_ids, false);
        hidemany(hwnd, generating_ids, true);
        hidemany(hwnd, gotkey_ids_unconditional, true);
        hidemany(hwnd, gotkey_ids_conditional, true);
        EnableWindow(GetDlgItem(hwnd, IDC_GENERATE), 1);
        EnableWindow(GetDlgItem(hwnd, IDC_LOAD), 1);
        EnableWindow(GetDlgItem(hwnd, IDC_SAVE), 0);
        EnableWindow(GetDlgItem(hwnd, IDC_SAVEPUB), 0);
        EnableWindow(GetDlgItem(hwnd, IDC_KEYSSH1), 1);
        EnableWindow(GetDlgItem(hwnd, IDC_KEYSSH2RSA), 1);
        EnableWindow(GetDlgItem(hwnd, IDC_KEYSSH2DSA), 1);
        EnableWindow(GetDlgItem(hwnd, IDC_KEYSSH2ECDSA), 1);
        EnableWindow(GetDlgItem(hwnd, IDC_KEYSSH2EDDSA), 1);
        EnableWindow(GetDlgItem(hwnd, IDC_BITS), 1);
        EnableMenuItem(state->filemenu, IDC_LOAD, MF_ENABLED|MF_BYCOMMAND);
        EnableMenuItem(state->filemenu, IDC_SAVE, MF_GRAYED|MF_BYCOMMAND);
        EnableMenuItem(state->filemenu, IDC_SAVEPUB, MF_GRAYED|MF_BYCOMMAND);
        EnableMenuItem(state->keymenu, IDC_GENERATE, MF_ENABLED|MF_BYCOMMAND);
        EnableMenuItem(state->keymenu, IDC_KEYSSH1, MF_ENABLED|MF_BYCOMMAND);
        EnableMenuItem(state->keymenu, IDC_KEYSSH2RSA, MF_ENABLED|MF_BYCOMMAND);
        EnableMenuItem(state->keymenu, IDC_KEYSSH2DSA, MF_ENABLED|MF_BYCOMMAND);
        EnableMenuItem(state->keymenu, IDC_KEYSSH2ECDSA,
                       MF_ENABLED|MF_BYCOMMAND);
        EnableMenuItem(state->keymenu, IDC_KEYSSH2EDDSA,
                       MF_ENABLED|MF_BYCOMMAND);
        EnableMenuItem(state->cvtmenu, IDC_IMPORT, MF_ENABLED|MF_BYCOMMAND);
        EnableMenuItem(state->cvtmenu, IDC_EXPORT_OPENSSH_AUTO,
                       MF_GRAYED|MF_BYCOMMAND);
        EnableMenuItem(state->cvtmenu, IDC_EXPORT_OPENSSH_NEW,
                       MF_GRAYED|MF_BYCOMMAND);
        EnableMenuItem(state->cvtmenu, IDC_EXPORT_SSHCOM,
                       MF_GRAYED|MF_BYCOMMAND);
        EnableMenuItem(state->keymenu, IDC_ADDCERT, MF_GRAYED|MF_BYCOMMAND);
        EnableMenuItem(state->keymenu, IDC_REMCERT, MF_GRAYED|MF_BYCOMMAND);
        break;
      case 1:                          /* generating key */
        hidemany(hwnd, nokey_ids, true);
        hidemany(hwnd, generating_ids, false);
        hidemany(hwnd, gotkey_ids_unconditional, true);
        hidemany(hwnd, gotkey_ids_conditional, true);
        EnableWindow(GetDlgItem(hwnd, IDC_GENERATE), 0);
        EnableWindow(GetDlgItem(hwnd, IDC_LOAD), 0);
        EnableWindow(GetDlgItem(hwnd, IDC_SAVE), 0);
        EnableWindow(GetDlgItem(hwnd, IDC_SAVEPUB), 0);
        EnableWindow(GetDlgItem(hwnd, IDC_KEYSSH1), 0);
        EnableWindow(GetDlgItem(hwnd, IDC_KEYSSH2RSA), 0);
        EnableWindow(GetDlgItem(hwnd, IDC_KEYSSH2DSA), 0);
        EnableWindow(GetDlgItem(hwnd, IDC_KEYSSH2ECDSA), 0);
        EnableWindow(GetDlgItem(hwnd, IDC_KEYSSH2EDDSA), 0);
        EnableWindow(GetDlgItem(hwnd, IDC_BITS), 0);
        EnableMenuItem(state->filemenu, IDC_LOAD, MF_GRAYED|MF_BYCOMMAND);
        EnableMenuItem(state->filemenu, IDC_SAVE, MF_GRAYED|MF_BYCOMMAND);
        EnableMenuItem(state->filemenu, IDC_SAVEPUB, MF_GRAYED|MF_BYCOMMAND);
        EnableMenuItem(state->keymenu, IDC_GENERATE, MF_GRAYED|MF_BYCOMMAND);
        EnableMenuItem(state->keymenu, IDC_KEYSSH1, MF_GRAYED|MF_BYCOMMAND);
        EnableMenuItem(state->keymenu, IDC_KEYSSH2RSA, MF_GRAYED|MF_BYCOMMAND);
        EnableMenuItem(state->keymenu, IDC_KEYSSH2DSA, MF_GRAYED|MF_BYCOMMAND);
        EnableMenuItem(state->keymenu, IDC_KEYSSH2ECDSA,
                       MF_GRAYED|MF_BYCOMMAND);
        EnableMenuItem(state->keymenu, IDC_KEYSSH2EDDSA,
                       MF_GRAYED|MF_BYCOMMAND);
        EnableMenuItem(state->cvtmenu, IDC_IMPORT, MF_GRAYED|MF_BYCOMMAND);
        EnableMenuItem(state->cvtmenu, IDC_EXPORT_OPENSSH_AUTO,
                       MF_GRAYED|MF_BYCOMMAND);
        EnableMenuItem(state->cvtmenu, IDC_EXPORT_OPENSSH_NEW,
                       MF_GRAYED|MF_BYCOMMAND);
        EnableMenuItem(state->cvtmenu, IDC_EXPORT_SSHCOM,
                       MF_GRAYED|MF_BYCOMMAND);
        EnableMenuItem(state->keymenu, IDC_ADDCERT, MF_GRAYED|MF_BYCOMMAND);
        EnableMenuItem(state->keymenu, IDC_REMCERT, MF_GRAYED|MF_BYCOMMAND);
        break;
      case 2:
        hidemany(hwnd, nokey_ids, true);
        hidemany(hwnd, generating_ids, true);
        hidemany(hwnd, gotkey_ids_unconditional, false);
        /* gotkey_ids_conditional will be unhidden by setupbigedit2 */
        EnableWindow(GetDlgItem(hwnd, IDC_GENERATE), 1);
        EnableWindow(GetDlgItem(hwnd, IDC_LOAD), 1);
        EnableWindow(GetDlgItem(hwnd, IDC_SAVE), 1);
        EnableWindow(GetDlgItem(hwnd, IDC_SAVEPUB), 1);
        EnableWindow(GetDlgItem(hwnd, IDC_KEYSSH1), 1);
        EnableWindow(GetDlgItem(hwnd, IDC_KEYSSH2RSA), 1);
        EnableWindow(GetDlgItem(hwnd, IDC_KEYSSH2DSA), 1);
        EnableWindow(GetDlgItem(hwnd, IDC_KEYSSH2ECDSA), 1);
        EnableWindow(GetDlgItem(hwnd, IDC_KEYSSH2EDDSA), 1);
        EnableWindow(GetDlgItem(hwnd, IDC_BITS), 1);
        EnableMenuItem(state->filemenu, IDC_LOAD, MF_ENABLED|MF_BYCOMMAND);
        EnableMenuItem(state->filemenu, IDC_SAVE, MF_ENABLED|MF_BYCOMMAND);
        EnableMenuItem(state->filemenu, IDC_SAVEPUB, MF_ENABLED|MF_BYCOMMAND);
        EnableMenuItem(state->keymenu, IDC_GENERATE, MF_ENABLED|MF_BYCOMMAND);
        EnableMenuItem(state->keymenu, IDC_KEYSSH1, MF_ENABLED|MF_BYCOMMAND);
        EnableMenuItem(state->keymenu, IDC_KEYSSH2RSA,MF_ENABLED|MF_BYCOMMAND);
        EnableMenuItem(state->keymenu, IDC_KEYSSH2DSA,MF_ENABLED|MF_BYCOMMAND);
        EnableMenuItem(state->keymenu, IDC_KEYSSH2ECDSA,
                       MF_ENABLED|MF_BYCOMMAND);
        EnableMenuItem(state->keymenu, IDC_KEYSSH2EDDSA,
                       MF_ENABLED|MF_BYCOMMAND);
        EnableMenuItem(state->cvtmenu, IDC_IMPORT, MF_ENABLED|MF_BYCOMMAND);
        /*
         * Enable export menu items if and only if the key type
         * supports this kind of export.
         */
        type = state->ssh2 ? SSH_KEYTYPE_SSH2 : SSH_KEYTYPE_SSH1;
#define do_export_menuitem(x,y) \
    EnableMenuItem(state->cvtmenu, x, MF_BYCOMMAND | \
                       (import_target_type(y)==type?MF_ENABLED:MF_GRAYED))
        do_export_menuitem(IDC_EXPORT_OPENSSH_AUTO, SSH_KEYTYPE_OPENSSH_AUTO);
        do_export_menuitem(IDC_EXPORT_OPENSSH_NEW, SSH_KEYTYPE_OPENSSH_NEW);
        do_export_menuitem(IDC_EXPORT_SSHCOM, SSH_KEYTYPE_SSHCOM);
#undef do_export_menuitem
        /*
         * Enable certificate menu items similarly.
         */
        {
            bool add_cert_allowed = false, rem_cert_allowed = false;

            if (state->ssh2 && state->ssh2key.key) {
                const ssh_keyalg *alg = ssh_key_alg(state->ssh2key.key);
                size_t i;
                if (alg->is_certificate) {
                    /* If there's a certificate, we can remove it */
                    rem_cert_allowed = true;
                    /* And reset to the base algorithm for the next check */
                    alg = alg->base_alg;
                }

                /* Now, do we have any certified version of this alg? */
                for (i = 0; i < n_keyalgs; i++) {
                    if (all_keyalgs[i]->base_alg == alg) {
                        add_cert_allowed = true;
                        break;
                    }
                }
            }

            EnableMenuItem(state->keymenu, IDC_ADDCERT, MF_BYCOMMAND |
                           (add_cert_allowed ? MF_ENABLED : MF_GRAYED));
            EnableMenuItem(state->keymenu, IDC_REMCERT, MF_BYCOMMAND |
                           (rem_cert_allowed ? MF_ENABLED : MF_GRAYED));
        }
        break;
    }
}

/*
 * Helper functions to set the key type, taking care of keeping the
 * menu and radio button selections in sync and also showing/hiding
 * the appropriate size/curve control for the current key type.
 */
void ui_update_key_type_ctrls(HWND hwnd)
{
    enum { BITS, ECCURVE, EDCURVE, NOTHING } which;
    static const int bits_ids[] = {
        IDC_BITSSTATIC, IDC_BITS, 0
    };
    static const int eccurve_ids[] = {
        IDC_ECCURVESTATIC, IDC_ECCURVE, 0
    };
    static const int edcurve_ids[] = {
        IDC_EDCURVESTATIC, IDC_EDCURVE, 0
    };
    static const int nothing_ids[] = {
        IDC_NOTHINGSTATIC, 0
    };

    if (IsDlgButtonChecked(hwnd, IDC_KEYSSH1) ||
        IsDlgButtonChecked(hwnd, IDC_KEYSSH2RSA) ||
        IsDlgButtonChecked(hwnd, IDC_KEYSSH2DSA)) {
        which = BITS;
    } else if (IsDlgButtonChecked(hwnd, IDC_KEYSSH2ECDSA)) {
        which = ECCURVE;
    } else if (IsDlgButtonChecked(hwnd, IDC_KEYSSH2EDDSA)) {
        which = EDCURVE;
    } else {
        /* Currently not used since Ed25519 stopped being the only
         * thing in its class, but I'll keep it here in case it comes
         * in useful again */
        which = NOTHING;
    }

    hidemany(hwnd, bits_ids, which != BITS);
    hidemany(hwnd, eccurve_ids, which != ECCURVE);
    hidemany(hwnd, edcurve_ids, which != EDCURVE);
    hidemany(hwnd, nothing_ids, which != NOTHING);
}
/*
 * Check the menu item 'item' and uncheck the others with IDs from
 * 'first' to 'last'. (CheckMenuRadioItem, which came with Windows NT
 * 4.0, does this with round marks.)
 */
static void check_menu_item_of(HMENU menu, int first, int last, int item)
{
    int i;
    for (i = first; i <= last; i++)
        CheckMenuItem(menu, i, MF_BYCOMMAND |
                      (i == item ? MF_CHECKED : MF_UNCHECKED));
}
void ui_set_key_type(HWND hwnd, struct MainDlgState *state, int button)
{
    CheckRadioButton(hwnd, IDC_KEYSSH1, IDC_KEYSSH2EDDSA, button);
    check_menu_item_of(state->keymenu, IDC_KEYSSH1, IDC_KEYSSH2EDDSA, button);
    ui_update_key_type_ctrls(hwnd);
}
void ui_set_primepolicy(HWND hwnd, struct MainDlgState *state, int option)
{
    check_menu_item_of(state->keymenu, IDC_PRIMEGEN_PROB,
                       IDC_PRIMEGEN_MAURER_COMPLEX, option);
    switch (option) {
      case IDC_PRIMEGEN_PROB:
        state->primepolicy = &primegen_probabilistic;
        break;
      case IDC_PRIMEGEN_MAURER_SIMPLE:
        state->primepolicy = &primegen_provable_maurer_simple;
        break;
      case IDC_PRIMEGEN_MAURER_COMPLEX:
        state->primepolicy = &primegen_provable_maurer_complex;
        break;
    }
}
void ui_set_rsa_strong(HWND hwnd, struct MainDlgState *state, bool enable)
{
    state->rsa_strong = enable;
    CheckMenuItem(state->keymenu, IDC_RSA_STRONG,
                  (enable ? MF_CHECKED : 0) | MF_BYCOMMAND);
}
static FingerprintType idc_to_fptype(int option)
{
    switch (option) {
      case IDC_FPTYPE_SHA256:
        return SSH_FPTYPE_SHA256;
      case IDC_FPTYPE_MD5:
        return SSH_FPTYPE_MD5;
      default:
        unreachable("bad control id in idc_to_fptype");
    }
}
static int fptype_to_idc(FingerprintType fptype)
{
    switch (fptype) {
      case SSH_FPTYPE_SHA256:
        return IDC_FPTYPE_SHA256;
      case SSH_FPTYPE_MD5:
        return IDC_FPTYPE_MD5;
      default:
        unreachable("bad fptype in fptype_to_idc");
    }
}
void ui_set_fptype(HWND hwnd, struct MainDlgState *state, int option)
{
    check_menu_item_of(state->keymenu, IDC_FPTYPE_SHA256,
                       IDC_FPTYPE_MD5, option);

    state->fptype = idc_to_fptype(option);

    if (state->key_exists && state->ssh2) {
        char *fp = ssh2_fingerprint(state->ssh2key.key, state->fptype);
        SetDlgItemText(hwnd, IDC_FINGERPRINT, fp);
        sfree(fp);
    }
}

static void update_ui_after_ssh2_pubkey_change(
    HWND hwnd, struct MainDlgState *state)
{
    /* Smaller version of update_ui_after_load which doesn't need to
     * be told things like the passphrase, which we aren't changing
     * anyway */
    char *savecomment = state->ssh2key.comment;
    char *fp;
    state->ssh2key.comment = NULL;
    fp = ssh2_fingerprint(state->ssh2key.key, state->fptype);
    state->ssh2key.comment = savecomment;

    SetDlgItemText(hwnd, IDC_FINGERPRINT, fp);
    sfree(fp);

    setupbigedit2(hwnd, &state->ssh2key);
}

static void update_ui_after_load(HWND hwnd, struct MainDlgState *state,
                                 const char *passphrase, int type,
                                 RSAKey *newkey1, ssh2_userkey *newkey2)
{
    SetDlgItemText(hwnd, IDC_PASSPHRASE1EDIT, passphrase);
    SetDlgItemText(hwnd, IDC_PASSPHRASE2EDIT, passphrase);

    if (type == SSH_KEYTYPE_SSH1) {
        char *fingerprint, *savecomment;

        state->ssh2 = false;
        state->commentptr = &state->key.comment;
        state->key = *newkey1;         /* structure copy */

        /*
         * Set the key fingerprint.
         */
        savecomment = state->key.comment;
        state->key.comment = NULL;
        fingerprint = rsa_ssh1_fingerprint(&state->key);
        state->key.comment = savecomment;
        SetDlgItemText(hwnd, IDC_FINGERPRINT, fingerprint);
        sfree(fingerprint);

        /*
         * Construct a decimal representation of the key, for pasting
         * into .ssh/authorized_keys on a Unix box.
         */
        setupbigedit1(hwnd, &state->key);
    } else {
        state->ssh2 = true;
        state->commentptr = &state->ssh2key.comment;
        state->ssh2key = *newkey2;      /* structure copy */
        sfree(newkey2);

        update_ui_after_ssh2_pubkey_change(hwnd, state);
    }
    SetDlgItemText(hwnd, IDC_COMMENTEDIT,
                   *state->commentptr);

    /*
     * Finally, hide the progress bar and show the key data.
     */
    ui_set_state(hwnd, state, 2);
    state->key_exists = true;
}

void load_key_file(HWND hwnd, struct MainDlgState *state,
                   Filename *filename, bool was_import_cmd)
{
    char *passphrase;
    bool needs_pass;
    int type, realtype;
    int ret;
    const char *errmsg = NULL;
    char *comment;
    RSAKey newkey1;
    ssh2_userkey *newkey2 = NULL;

    type = realtype = key_type(filename);
    if (type != SSH_KEYTYPE_SSH1 &&
        type != SSH_KEYTYPE_SSH2 &&
        !import_possible(type)) {
        char *msg = dupprintf("Couldn't load private key (%s)",
                              key_type_to_str(type));
        message_box(hwnd, msg, "PuTTYgen Error", MB_OK | MB_ICONERROR,
                    false, HELPCTXID(errors_cantloadkey));
        sfree(msg);
        return;
    }

    if (type != SSH_KEYTYPE_SSH1 &&
        type != SSH_KEYTYPE_SSH2) {
        realtype = type;
        type = import_target_type(type);
    }

    comment = NULL;
    passphrase = NULL;
    if (realtype == SSH_KEYTYPE_SSH1)
        needs_pass = rsa1_encrypted_f(filename, &comment);
    else if (realtype == SSH_KEYTYPE_SSH2)
        needs_pass = ppk_encrypted_f(filename, &comment);
    else
        needs_pass = import_encrypted(filename, realtype, &comment);
    do {
        burnstr(passphrase);
        passphrase = NULL;

        if (needs_pass) {
            int dlgret;
            struct PassphraseProcStruct pps;
            pps.passphrase = &passphrase;
            pps.comment = comment;
            dlgret = DialogBoxParam(hinst,
                                    MAKEINTRESOURCE(210),
                                    NULL, PassphraseProc,
                                    (LPARAM) &pps);
            if (!dlgret) {
                ret = -2;
                break;
            }
            assert(passphrase != NULL);
        } else
            passphrase = dupstr("");
        if (type == SSH_KEYTYPE_SSH1) {
            if (realtype == type)
                ret = rsa1_load_f(filename, &newkey1, passphrase, &errmsg);
            else
                ret = import_ssh1(filename, realtype, &newkey1,
                                  passphrase, &errmsg);
        } else {
            if (realtype == type)
                newkey2 = ppk_load_f(filename, passphrase, &errmsg);
            else
                newkey2 = import_ssh2(filename, realtype, passphrase, &errmsg);
            if (newkey2 == SSH2_WRONG_PASSPHRASE)
                ret = -1;
            else if (!newkey2)
                ret = 0;
            else
                ret = 1;
        }
    } while (ret == -1);
    if (comment)
        sfree(comment);
    if (ret == 0) {
        char *msg = dupprintf("Couldn't load private key (%s)", errmsg);
        message_box(hwnd, msg, "PuTTYgen Error", MB_OK | MB_ICONERROR,
                    false, HELPCTXID(errors_cantloadkey));
        sfree(msg);
    } else if (ret == 1) {
        /*
         * Now update the key controls with all the
         * key data.
         */
        update_ui_after_load(hwnd, state, passphrase, type, &newkey1, newkey2);

        /*
         * If the user has imported a foreign key
         * using the Load command, let them know.
         * If they've used the Import command, be
         * silent.
         */
        if (realtype != type && !was_import_cmd) {
            char msg[512];
            sprintf(msg, "Successfully imported foreign key\n"
                    "(%s).\n"
                    "To use this key with PuTTY, you need to\n"
                    "use the \"Save private key\" command to\n"
                    "save it in PuTTY's own format.",
                    key_type_to_str(realtype));
            MessageBox(NULL, msg, "PuTTYgen Notice",
                       MB_OK | MB_ICONINFORMATION);
        }
    }
    burnstr(passphrase);
}

void add_certificate(HWND hwnd, struct MainDlgState *state,
                     Filename *filename)
{
    int type = key_type(filename);
    char *algname;
    char *comment;
    const char *error;
    strbuf *pub;
    const ssh_keyalg *alg;
    strbuf *old_basepub;
    ssh_key *new_pubkey;
    strbuf *new_basepub;
    bool match;
    strbuf *priv;
    ssh_key *newkey;
    if (type != SSH_KEYTYPE_SSH2_PUBLIC_RFC4716 &&
        type != SSH_KEYTYPE_SSH2_PUBLIC_OPENSSH) {
        char *msg = dupprintf("Couldn't load certificate (%s)",
                              key_type_to_str(type));
        message_box(hwnd, msg, "PuTTYgen Error", MB_OK | MB_ICONERROR,
                    false, HELPCTXID(errors_cantloadkey));
        sfree(msg);
        return;
    }

    algname = NULL;
    comment = NULL;
    error = NULL;
    pub = strbuf_new();
    if (!ppk_loadpub_f(filename, &algname, BinarySink_UPCAST(pub), &comment,
                       &error)) {
        char *msg = dupprintf("Couldn't load certificate (%s)", error);
        message_box(hwnd, msg, "PuTTYgen Error", MB_OK | MB_ICONERROR,
                    false, HELPCTXID(errors_cantloadkey));
        sfree(msg);
        strbuf_free(pub);
        return;
    }

    sfree(comment);

    alg = find_pubkey_alg(algname);
    if (!alg) {
        char *msg = dupprintf("Couldn't load certificate (unsupported "
                              "algorithm name '%s')", algname);
        message_box(hwnd, msg, "PuTTYgen Error", MB_OK | MB_ICONERROR,
                    false, HELPCTXID(errors_cantloadkey));
        sfree(msg);
        sfree(algname);
        strbuf_free(pub);
        return;
    }

    sfree(algname);

    /* Check the two public keys match apart from certificates */
    old_basepub = strbuf_new();
    ssh_key_public_blob(ssh_key_base_key(state->ssh2key.key),
                        BinarySink_UPCAST(old_basepub));

    new_pubkey = ssh_key_new_pub(alg, ptrlen_from_strbuf(pub));
    new_basepub = strbuf_new();
    ssh_key_public_blob(ssh_key_base_key(new_pubkey),
                        BinarySink_UPCAST(new_basepub));
    ssh_key_free(new_pubkey);

    match = ptrlen_eq_ptrlen(ptrlen_from_strbuf(old_basepub),
                             ptrlen_from_strbuf(new_basepub));
    strbuf_free(old_basepub);
    strbuf_free(new_basepub);

    if (!match) {
        char *msg = dupprintf("Certificate is for a different public key");
        message_box(hwnd, msg, "PuTTYgen Error", MB_OK | MB_ICONERROR,
                    false, HELPCTXID(errors_cantloadkey));
        sfree(msg);
        strbuf_free(pub);
        return;
    }

    priv = strbuf_new_nm();
    ssh_key_private_blob(state->ssh2key.key, BinarySink_UPCAST(priv));
    newkey = ssh_key_new_priv(
        alg, ptrlen_from_strbuf(pub), ptrlen_from_strbuf(priv));
    strbuf_free(pub);
    strbuf_free(priv);

    if (!newkey) {
        char *msg = dupprintf("Couldn't combine certificate with key");
        message_box(hwnd, msg, "PuTTYgen Error", MB_OK | MB_ICONERROR,
                    false, HELPCTXID(errors_cantloadkey));
        sfree(msg);
        return;
    }

    ssh_key_free(state->ssh2key.key);
    state->ssh2key.key = newkey;

    update_ui_after_ssh2_pubkey_change(hwnd, state);
    ui_set_state(hwnd, state, 2);
}

void remove_certificate(HWND hwnd, struct MainDlgState *state)
{
    ssh_key *newkey = ssh_key_clone(ssh_key_base_key(state->ssh2key.key));
    ssh_key_free(state->ssh2key.key);
    state->ssh2key.key = newkey;
    update_ui_after_ssh2_pubkey_change(hwnd, state);
    ui_set_state(hwnd, state, 2);
}

static void start_generating_key(HWND hwnd, struct MainDlgState *state)
{
    static const char generating_msg[] =
        "Please wait while a key is generated...";

    struct rsa_key_thread_params *params;
#ifndef WIN32S_COMPAT
    DWORD threadid;
    HANDLE hThread;
#endif

    SetDlgItemText(hwnd, IDC_GENERATING, generating_msg);
    SendDlgItemMessage(hwnd, IDC_PROGRESS, PROGBAR_SETRANGE, 0,
                       MAKELPARAM(0, PROGRESSRANGE));
    SendDlgItemMessage(hwnd, IDC_PROGRESS, PROGBAR_SETPOS, 0, 0);

    params = snew(struct rsa_key_thread_params);
    params->progressbar = GetDlgItem(hwnd, IDC_PROGRESS);
    params->dialog = hwnd;
    params->key_bits = state->key_bits;
    params->curve_bits = state->curve_bits;
    params->keytype = state->keytype;
    params->primepolicy = state->primepolicy;
    params->rsa_strong = state->rsa_strong;
    params->key = &state->key;
    params->dsakey = &state->dsakey;

#ifdef WIN32S_COMPAT
    /* No threads on Win32s: generate synchronously. The WM_DONEKEY
     * message this posts is handled next time the message loop runs. */
    generate_key_thread(params);
    state->generation_thread_exists = true;
#else
    hThread = CreateThread(NULL, 0, generate_key_thread,
                           params, 0, &threadid);
    if (!hThread) {
        MessageBox(hwnd, "Out of thread resources",
                   "Key generation error",
                   MB_OK | MB_ICONERROR);
        sfree(params);
    } else {
        CloseHandle(hThread);          /* we don't need the thread handle */
        state->generation_thread_exists = true;
    }
#endif /* WIN32S_COMPAT */
}

/*
 * Dialog-box function and context structure for the 'Certificate
 * info' button.
 */
struct certinfo_dialog_ctx {
    SeatDialogText *text;
};

static BOOL CertInfoProc(HWND hwnd, UINT msg, WPARAM wParam,
                         LPARAM lParam, void *vctx)
{
    struct certinfo_dialog_ctx *ctx = (struct certinfo_dialog_ctx *)vctx;

    switch (msg) {
      case WM_INITDIALOG: {
        int index = 100, y = 12;

        WPARAM font = SendMessage(hwnd, WM_GETFONT, 0, 0);

        const char *key = NULL;
        RECT r;
        HWND ctl;
        int oldheight;
        int newheight;
        SeatDialogTextItem *item;
        SeatDialogTextItem *end;
        for (item = ctx->text->items, end = item + ctx->text->nitems;
             item < end; item++) {
            switch (item->type) {
              case SDT_MORE_INFO_KEY:
                key = item->text;
                break;
              case SDT_MORE_INFO_VALUE_SHORT:
              case SDT_MORE_INFO_VALUE_BLOB: {
                RECT rk, rv;
                DWORD editstyle = WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                    ES_AUTOHSCROLL | ES_READONLY;
                HWND ctl;
                if (item->type == SDT_MORE_INFO_VALUE_BLOB) {
                    rk.left = 12;
                    rk.right = 286;
                    rk.top = y;
                    rk.bottom = 8;
                    y += 10;

                    editstyle |= ES_MULTILINE;
                    rv.left = 12;
                    rv.right = 286;
                    rv.top = y;
                    rv.bottom = 64;
                    y += 68;
                } else {
                    rk.left = 12;
                    rk.right = 130;
                    rk.top = y+2;
                    rk.bottom = 8;

                    rv.left = 150;
                    rv.right = 298;
                    rv.top = y;
                    rv.bottom = 12;

                    y += 16;
                }

                MapDialogRect(hwnd, &rk);
                ctl = CreateWindowEx(
                    0, "STATIC", key, WS_CHILD | WS_VISIBLE,
                    rk.left, rk.top, rk.right, rk.bottom,
                    hwnd, (HMENU)(UINT)index++, hinst, NULL);
                SendMessage(ctl, WM_SETFONT, font, MAKELPARAM(true, 0));

                MapDialogRect(hwnd, &rv);
                ctl = CreateWindowEx(
                    EDGE_EXSTYLE, "EDIT", item->text, editstyle | EDGE_STYLE,
                    rv.left, rv.top, rv.right, rv.bottom,
                    hwnd, (HMENU)(UINT)index++, hinst, NULL);
                SendMessage(ctl, WM_SETFONT, font, MAKELPARAM(true, 0));
                break;
              }
              default:
                break;
            }
        }

        /*
         * Now resize the overall window, and move the Close button at
         * the bottom.
         */
        r.left = 176;
        r.top = y + 10;
        r.right = r.bottom = 0;
        MapDialogRect(hwnd, &r);
        ctl = GetDlgItem(hwnd, IDOK);
        SetWindowPos(ctl, NULL, r.left, r.top, 0, 0,
                     SWP_NOSIZE | SWP_NOREDRAW | SWP_NOZORDER);

        r.left = r.top = r.right = 0;
        r.bottom = 300;
        MapDialogRect(hwnd, &r);
        oldheight = r.bottom;

        r.left = r.top = r.right = 0;
        r.bottom = y + 30;
        MapDialogRect(hwnd, &r);
        newheight = r.bottom;

        GetWindowRect(hwnd, &r);

        SetWindowPos(hwnd, NULL, 0, 0, r.right - r.left,
                     r.bottom - r.top + newheight - oldheight,
                     SWP_NOMOVE | SWP_NOREDRAW | SWP_NOZORDER);

        ShowWindow(hwnd, SW_SHOWNORMAL);
        return 1;
      }
      case WM_COMMAND:
        switch (LOWORD(wParam)) {
          case IDOK:
            ShinyEndDialog(hwnd, 0);
            return 0;
        }
        return 0;
      case WM_CLOSE:
        ShinyEndDialog(hwnd, 0);
        return 0;
    }
    return 0;
}

/*
 * Dialog-box function for the main PuTTYgen dialog box.
 */
static BOOL CALLBACK MainDlgProc(HWND hwnd, UINT msg,
                                 WPARAM wParam, LPARAM lParam)
{
    static const char entropy_msg[] =
        "Please generate some randomness by moving the mouse over the blank area.";
    struct MainDlgState *state;

    switch (msg) {
      struct InitialParams *params;
      case WM_INITDIALOG:
#ifdef WM_SETICON
        /* Windows NT 4.0 shows it in the title bar; the dialog can't
         * be minimised, so older versions never show it */
        SendMessage(hwnd, WM_SETICON, (WPARAM) ICON_BIG,
                    (LPARAM) LoadIcon(hinst, MAKEINTRESOURCE(200)));
#endif

        state = snew(struct MainDlgState);
        state->generation_thread_exists = false;
        state->entropy = NULL;
        state->key_exists = false;
        SetWindowLong(hwnd, GWL_USERDATA, (LONG) state);
#ifdef WIN32S_COMPAT
        /*
         * Win32s: dialog boxes don't reliably support SetMenu(), so
         * append our commands to the system menu instead. Everything
         * else about the menu handling is the same as on NT: the
         * EnableMenuItem calls below operate on the same HMENU.
         */
        {
            HMENU sysmenu = GetSystemMenu(hwnd, FALSE);

            AppendMenu(sysmenu, MF_SEPARATOR, 0, 0);
            AppendMenu(sysmenu, MF_ENABLED, IDC_LOAD, "&Load private key");
            AppendMenu(sysmenu, MF_ENABLED, IDC_SAVEPUB, "Save p&ublic key");
            AppendMenu(sysmenu, MF_ENABLED, IDC_SAVE, "&Save private key");
            AppendMenu(sysmenu, MF_SEPARATOR, 0, 0);
            AppendMenu(sysmenu, MF_ENABLED, IDC_GENERATE, "&Generate key pair");
            AppendMenu(sysmenu, MF_ENABLED, IDC_ADDCERT,
                       "Add certificate to key");
            AppendMenu(sysmenu, MF_ENABLED, IDC_REMCERT,
                       "Remove certificate from key");
            AppendMenu(sysmenu, MF_SEPARATOR, 0, 0);
            AppendMenu(sysmenu, MF_ENABLED, IDC_KEYSSH1, "SSH-1 key (RSA)");
            AppendMenu(sysmenu, MF_ENABLED, IDC_KEYSSH2RSA, "SSH-2 RSA key");
            AppendMenu(sysmenu, MF_ENABLED, IDC_KEYSSH2DSA, "SSH-2 DSA key");
            AppendMenu(sysmenu, MF_ENABLED, IDC_KEYSSH2ECDSA,
                       "SSH-2 ECDSA key");
            AppendMenu(sysmenu, MF_ENABLED, IDC_KEYSSH2EDDSA,
                       "SSH-2 EdDSA key");
            AppendMenu(sysmenu, MF_SEPARATOR, 0, 0);
            AppendMenu(sysmenu, MF_ENABLED, IDC_PRIMEGEN_PROB,
                       "Use probable primes (fast)");
            AppendMenu(sysmenu, MF_ENABLED, IDC_PRIMEGEN_MAURER_SIMPLE,
                       "Use proven primes (slower)");
            AppendMenu(sysmenu, MF_ENABLED, IDC_PRIMEGEN_MAURER_COMPLEX,
                       "Use proven primes with even distribution (slowest)");
            AppendMenu(sysmenu, MF_SEPARATOR, 0, 0);
            AppendMenu(sysmenu, MF_ENABLED, IDC_RSA_STRONG,
                       "Use strong primes as RSA key factors");
            AppendMenu(sysmenu, MF_SEPARATOR, 0, 0);
            AppendMenu(sysmenu, MF_ENABLED, IDC_PPK_PARAMS,
                       "Parameters for saving key files...");
            AppendMenu(sysmenu, MF_SEPARATOR, 0, 0);
            AppendMenu(sysmenu, MF_ENABLED, IDC_FPTYPE_SHA256,
                       "Show fingerprint as SHA256");
            AppendMenu(sysmenu, MF_ENABLED, IDC_FPTYPE_MD5,
                       "Show fingerprint as MD5");
            AppendMenu(sysmenu, MF_SEPARATOR, 0, 0);
            AppendMenu(sysmenu, MF_ENABLED, IDC_IMPORT, "&Import key");
            AppendMenu(sysmenu, MF_ENABLED, IDC_EXPORT_OPENSSH_AUTO,
                       "Export OpenSSH key");
            AppendMenu(sysmenu, MF_ENABLED, IDC_EXPORT_OPENSSH_NEW,
                       "Export OpenSSH key (new format)");
            AppendMenu(sysmenu, MF_ENABLED, IDC_EXPORT_SSHCOM,
                       "Export ssh.com key");
            AppendMenu(sysmenu, MF_SEPARATOR, 0, 0);
            AppendMenu(sysmenu, MF_ENABLED, IDC_ABOUT, "&About");

            state->filemenu = sysmenu;
            state->keymenu = sysmenu;
            state->cvtmenu = sysmenu;
        }
#else
        {
            HMENU menu, menu1;

            menu = CreateMenu();

            menu1 = CreateMenu();
            AppendMenu(menu1, MF_ENABLED, IDC_LOAD, "&Load private key");
            AppendMenu(menu1, MF_ENABLED, IDC_SAVEPUB, "Save p&ublic key");
            AppendMenu(menu1, MF_ENABLED, IDC_SAVE, "&Save private key");
            AppendMenu(menu1, MF_SEPARATOR, 0, 0);
            AppendMenu(menu1, MF_ENABLED, IDC_QUIT, "E&xit");
            AppendMenu(menu, MF_POPUP | MF_ENABLED, (UINT)menu1, "&File");
            state->filemenu = menu1;

            menu1 = CreateMenu();
            AppendMenu(menu1, MF_ENABLED, IDC_GENERATE, "&Generate key pair");
            AppendMenu(menu1, MF_SEPARATOR, 0, 0);
            AppendMenu(menu1, MF_ENABLED, IDC_ADDCERT,
                       "Add &certificate to key");
            AppendMenu(menu1, MF_ENABLED, IDC_REMCERT,
                       "Remove certificate from key");
            AppendMenu(menu1, MF_SEPARATOR, 0, 0);
            AppendMenu(menu1, MF_ENABLED, IDC_KEYSSH1, "SSH-&1 key (RSA)");
            AppendMenu(menu1, MF_ENABLED, IDC_KEYSSH2RSA, "SSH-2 &RSA key");
            AppendMenu(menu1, MF_ENABLED, IDC_KEYSSH2DSA, "SSH-2 &DSA key");
            AppendMenu(menu1, MF_ENABLED, IDC_KEYSSH2ECDSA, "SSH-2 &ECDSA key");
            AppendMenu(menu1, MF_ENABLED, IDC_KEYSSH2EDDSA, "SSH-2 EdD&SA key");
            AppendMenu(menu1, MF_SEPARATOR, 0, 0);
            AppendMenu(menu1, MF_ENABLED, IDC_PRIMEGEN_PROB,
                       "Use probable primes (fast)");
            AppendMenu(menu1, MF_ENABLED, IDC_PRIMEGEN_MAURER_SIMPLE,
                       "Use proven primes (slower)");
            AppendMenu(menu1, MF_ENABLED, IDC_PRIMEGEN_MAURER_COMPLEX,
                       "Use proven primes with even distribution (slowest)");
            AppendMenu(menu1, MF_SEPARATOR, 0, 0);
            AppendMenu(menu1, MF_ENABLED, IDC_RSA_STRONG,
                       "Use \"strong\" primes as RSA key factors");
            AppendMenu(menu1, MF_SEPARATOR, 0, 0);
            AppendMenu(menu1, MF_ENABLED, IDC_PPK_PARAMS,
                       "Parameters for saving key files...");
            AppendMenu(menu1, MF_SEPARATOR, 0, 0);
            AppendMenu(menu1, MF_ENABLED, IDC_FPTYPE_SHA256,
                       "Show fingerprint as SHA256");
            AppendMenu(menu1, MF_ENABLED, IDC_FPTYPE_MD5,
                       "Show fingerprint as MD5");
            AppendMenu(menu, MF_POPUP | MF_ENABLED, (UINT)menu1, "&Key");
            state->keymenu = menu1;

            menu1 = CreateMenu();
            AppendMenu(menu1, MF_ENABLED, IDC_IMPORT, "&Import key");
            AppendMenu(menu1, MF_SEPARATOR, 0, 0);
            AppendMenu(menu1, MF_ENABLED, IDC_EXPORT_OPENSSH_AUTO,
                       "Export &OpenSSH key");
            AppendMenu(menu1, MF_ENABLED, IDC_EXPORT_OPENSSH_NEW,
                       "Export &OpenSSH key (force new file format)");
            AppendMenu(menu1, MF_ENABLED, IDC_EXPORT_SSHCOM,
                       "Export &ssh.com key");
            AppendMenu(menu, MF_POPUP | MF_ENABLED, (UINT)menu1,
                       "Con&versions");
            state->cvtmenu = menu1;

            menu1 = CreateMenu();
            AppendMenu(menu1, MF_ENABLED, IDC_ABOUT, "&About");
            AppendMenu(menu, MF_POPUP | MF_ENABLED, (UINT)menu1, "&Help");

            SetMenu(hwnd, menu);
        }
#endif /* WIN32S_COMPAT */

        /*
         * Centre the window.
         */
        {                              /* centre the window */
            RECT rs, rd;
            HWND hw;

            hw = GetDesktopWindow();
            if (GetWindowRect(hw, &rs) && GetWindowRect(hwnd, &rd))
                MoveWindow(hwnd,
                           (rs.right + rs.left + rd.left - rd.right) / 2,
                           (rs.bottom + rs.top + rd.top - rd.bottom) / 2,
                           rd.right - rd.left, rd.bottom - rd.top, true);
        }

        {
            struct ctlpos cp, cp2;
            int ymax;

            /* Accelerators used: acglops1rbvde */

            ctlposinit(&cp, hwnd, 4, 4, 4);
            beginbox(&cp, "Key", IDC_BOX_KEY);
            cp2 = cp;
            statictext(&cp2, "No key.", 1, IDC_NOKEY);
            cp2 = cp;
            statictext(&cp2, "", 1, IDC_GENERATING);
            progressbar(&cp2, IDC_PROGRESS);
            cp2 = cp;
            bigeditctrl(&cp2, NULL, -1, IDC_CERTSTATIC, 3);
            {
                HWND child = GetDlgItem(hwnd, IDC_CERTSTATIC);
                LONG style = GetWindowLong(child, GWL_STYLE);
                style &= ~WS_VSCROLL;
                SetWindowLong(child, GWL_STYLE, style);
                SendMessage(child, EM_SETREADONLY, true, 0);
            }
            MakeDlgItemBorderless(hwnd, IDC_CERTSTATIC);
            cp2.xoff = cp2.width = cp2.width / 3;
            button(&cp2, "Certificate info...", IDC_CERTMOREINFO, false);
            bigeditctrl(&cp,
                        "&Public key for pasting into authorized_keys file:",
                        IDC_PKSTATIC, IDC_KEYDISPLAY, 5);
            SendDlgItemMessage(hwnd, IDC_KEYDISPLAY, EM_SETREADONLY, 1, 0);
            staticedit(&cp, "Key f&ingerprint:", IDC_FPSTATIC,
                       IDC_FINGERPRINT, 82);
            SendDlgItemMessage(hwnd, IDC_FINGERPRINT, EM_SETREADONLY, 1,
                               0);
            staticedit(&cp, "Key &comment:", IDC_COMMENTSTATIC,
                       IDC_COMMENTEDIT, 82);
            staticpassedit(&cp, "Key p&assphrase:", IDC_PASSPHRASE1STATIC,
                           IDC_PASSPHRASE1EDIT, 82);
            staticpassedit(&cp, "C&onfirm passphrase:",
                           IDC_PASSPHRASE2STATIC, IDC_PASSPHRASE2EDIT, 82);
            endbox(&cp);
            beginbox(&cp, "Actions", IDC_BOX_ACTIONS);
            staticbtn(&cp, "Generate a public/private key pair",
                      IDC_GENSTATIC, "&Generate", IDC_GENERATE);
            staticbtn(&cp, "Load an existing private key file",
                      IDC_LOADSTATIC, "&Load", IDC_LOAD);
            static2btn(&cp, "Save the generated key", IDC_SAVESTATIC,
                       "Save p&ublic key", IDC_SAVEPUB,
                       "&Save private key", IDC_SAVE);
            endbox(&cp);
            beginbox(&cp, "Parameters", IDC_BOX_PARAMS);
            radioline(&cp, "Type of key to generate:", IDC_TYPESTATIC, 5,
                      "&RSA", IDC_KEYSSH2RSA,
                      "&DSA", IDC_KEYSSH2DSA,
                      "&ECDSA", IDC_KEYSSH2ECDSA,
                      "EdD&SA", IDC_KEYSSH2EDDSA,
                      "SSH-&1 (RSA)", IDC_KEYSSH1,
                      NULL);
            cp2 = cp;
            staticedit(&cp2, "Number of &bits in a generated key:",
                       IDC_BITSSTATIC, IDC_BITS, 20);
            ymax = cp2.ypos;
            cp2 = cp;
            staticddl(&cp2, "Cur&ve to use for generating this key:",
                      IDC_ECCURVESTATIC, IDC_ECCURVE, 30);
            SendDlgItemMessage(hwnd, IDC_ECCURVE, CB_RESETCONTENT, 0, 0);
            {
                int i, bits;
                const struct ec_curve *curve;
                const ssh_keyalg *alg;

                for (i = 0; i < n_ec_nist_curve_lengths; i++) {
                    bits = ec_nist_curve_lengths[i];
                    ec_nist_alg_and_curve_by_bits(bits, &curve, &alg);
                    SendDlgItemMessage(hwnd, IDC_ECCURVE, CB_ADDSTRING, 0,
                                       (LPARAM)curve->textname);
                }
            }
            ymax = ymax > cp2.ypos ? ymax : cp2.ypos;
            cp2 = cp;
            staticddl(&cp2, "Cur&ve to use for generating this key:",
                      IDC_EDCURVESTATIC, IDC_EDCURVE, 30);
            SendDlgItemMessage(hwnd, IDC_EDCURVE, CB_RESETCONTENT, 0, 0);
            {
                int i, bits;
                const struct ec_curve *curve;
                const ssh_keyalg *alg;

                for (i = 0; i < n_ec_ed_curve_lengths; i++) {
                    char *desc;
                    bits = ec_ed_curve_lengths[i];
                    ec_ed_alg_and_curve_by_bits(bits, &curve, &alg);
                    desc = dupprintf("%s (%d bits)",
                                     curve->textname, bits);
                    SendDlgItemMessage(hwnd, IDC_EDCURVE, CB_ADDSTRING, 0,
                                       (LPARAM)desc);
                    sfree(desc);
                }
            }
            ymax = ymax > cp2.ypos ? ymax : cp2.ypos;
            cp2 = cp;
            statictext(&cp2, "(nothing to configure for this key type)",
                       1, IDC_NOTHINGSTATIC);
            ymax = ymax > cp2.ypos ? ymax : cp2.ypos;
            cp.ypos = ymax;
            endbox(&cp);
        }
        params = (struct InitialParams *)lParam;
        ui_set_key_type(hwnd, state, params->keybutton);
        ui_set_primepolicy(hwnd, state, params->primepolicybutton);
        ui_set_rsa_strong(hwnd, state, params->rsa_strong);
        ui_set_fptype(hwnd, state, fptype_to_idc(params->fptype));
        SetDlgItemInt(hwnd, IDC_BITS, params->keybits, false);
        SendDlgItemMessage(hwnd, IDC_ECCURVE, CB_SETCURSEL,
                           params->eccurve_index, 0);
        SendDlgItemMessage(hwnd, IDC_EDCURVE, CB_SETCURSEL,
                           params->edcurve_index, 0);

        /*
         * Initially, hide the progress bar and the key display,
         * and show the no-key display. Also disable the Save
         * buttons, because with no key we obviously can't save
         * anything.
         */
        ui_set_state(hwnd, state, 0);

        /*
         * Load a key file if one was provided on the command line.
         */
        if (cmdline_keyfile)
            load_key_file(hwnd, state, cmdline_keyfile, false);

        return 1;
      case WM_MOUSEMOVE:
        state = (struct MainDlgState *) GetWindowLong(hwnd, GWL_USERDATA);
        if (state->entropy && state->entropy_got < state->entropy_required) {
            ULONG msgtime = GetMessageTime();
            put_uint32(state->entropy, lParam);
            put_uint32(state->entropy, msgtime);
            if (msgtime - state->entropy_prev_msgtime > ENTROPY_RATE_LIMIT) {
                state->entropy_got += 2;
                state->entropy_prev_msgtime = msgtime;
            }
            SendDlgItemMessage(hwnd, IDC_PROGRESS, PROGBAR_SETPOS,
                               state->entropy_got, 0);
            if (state->entropy_got >= state->entropy_required) {
                /*
                 * Seed the entropy pool
                 */
                random_reseed(ptrlen_from_strbuf(state->entropy));
                strbuf_free(state->entropy);
                state->entropy = NULL;

                start_generating_key(hwnd, state);
            }
        }
        break;
#ifdef WIN32S_COMPAT
      case WM_SYSCOMMAND:
        /*
         * Forward custom system-menu commands (the Win32s build puts
         * puttygen's menu in the system menu) to the WM_COMMAND handler.
         */
        {
            UINT scmd = LOWORD(wParam);
            if (scmd >= controlidstart && scmd <= IDC_REMCERT) {
                SendMessage(hwnd, WM_COMMAND, MAKEWPARAM(scmd, 0), 0);
                return 0;
            }
        }
        break;
#endif
      case WM_COMMAND:
        switch (LOWORD(wParam)) {
          case IDC_KEYSSH1:
          case IDC_KEYSSH2RSA:
          case IDC_KEYSSH2DSA:
          case IDC_KEYSSH2ECDSA:
          case IDC_KEYSSH2EDDSA: {
            state = (struct MainDlgState *)
                GetWindowLong(hwnd, GWL_USERDATA);
            ui_set_key_type(hwnd, state, LOWORD(wParam));
            break;
          }
          case IDC_PRIMEGEN_PROB:
          case IDC_PRIMEGEN_MAURER_SIMPLE:
          case IDC_PRIMEGEN_MAURER_COMPLEX: {
            state = (struct MainDlgState *)
                GetWindowLong(hwnd, GWL_USERDATA);
            ui_set_primepolicy(hwnd, state, LOWORD(wParam));
            break;
          }
          case IDC_FPTYPE_SHA256:
          case IDC_FPTYPE_MD5: {
            state = (struct MainDlgState *)
                GetWindowLong(hwnd, GWL_USERDATA);
            ui_set_fptype(hwnd, state, LOWORD(wParam));
            break;
          }
          case IDC_RSA_STRONG: {
            state = (struct MainDlgState *)
                GetWindowLong(hwnd, GWL_USERDATA);
            ui_set_rsa_strong(hwnd, state, !state->rsa_strong);
            break;
          }
          case IDC_PPK_PARAMS: {
            struct PPKParams pp[1];
            int dlgret;
            pp->params = save_params;
            if (pp->params.argon2_passes_auto) {
                pp->time_ms = pp->params.argon2_milliseconds;
                pp->time_passes = 13;
            } else {
                pp->time_ms = 100;
                pp->time_passes = pp->params.argon2_passes;
            }
            dlgret = DialogBoxParam(hinst, MAKEINTRESOURCE(215),
                                    NULL, PPKParamsProc, (LPARAM)pp);
            if (dlgret) {
                if (pp->params.argon2_passes_auto) {
                    pp->params.argon2_milliseconds = pp->time_ms;
                } else {
                    pp->params.argon2_passes = pp->time_passes;
                }
                save_params = pp->params;
            }
            break;
          }
          case IDC_QUIT:
            PostMessage(hwnd, WM_CLOSE, 0, 0);
            break;
          case IDC_COMMENTEDIT:
            if (HIWORD(wParam) == EN_CHANGE) {
                state = (struct MainDlgState *)
                    GetWindowLong(hwnd, GWL_USERDATA);
                if (state->key_exists) {
                    HWND editctl = GetDlgItem(hwnd, IDC_COMMENTEDIT);
                    int len = GetWindowTextLength(editctl);
                    if (*state->commentptr)
                        sfree(*state->commentptr);
                    *state->commentptr = snewn(len + 1, char);
                    GetWindowText(editctl, *state->commentptr, len + 1);
                    if (state->ssh2) {
                        setupbigedit2(hwnd, &state->ssh2key);
                    } else {
                        setupbigedit1(hwnd, &state->key);
                    }
                }
            }
            break;
          case IDC_ABOUT:
            EnableWindow(hwnd, 0);
            DialogBox(hinst, MAKEINTRESOURCE(213), hwnd, AboutProc);
            EnableWindow(hwnd, 1);
            SetActiveWindow(hwnd);
            return 0;
          case IDC_GENERATE:
            if (HIWORD(wParam) != BN_CLICKED &&
                HIWORD(wParam) != BN_DOUBLECLICKED)
                break;
            state =
                (struct MainDlgState *) GetWindowLong(hwnd, GWL_USERDATA);
            if (!state->generation_thread_exists) {
                unsigned raw_entropy_required;
                unsigned char *raw_entropy_buf;
                BOOL ok;

                state->key_bits = GetDlgItemInt(hwnd, IDC_BITS, &ok, false);
                if (!ok)
                    state->key_bits = DEFAULT_KEY_BITS;
                state->ssh2 = true;

                if (IsDlgButtonChecked(hwnd, IDC_KEYSSH1)) {
                    state->ssh2 = false;
                    state->keytype = RSA;
                } else if (IsDlgButtonChecked(hwnd, IDC_KEYSSH2RSA)) {
                    state->keytype = RSA;
                } else if (IsDlgButtonChecked(hwnd, IDC_KEYSSH2DSA)) {
                    state->keytype = DSA;
                } else if (IsDlgButtonChecked(hwnd, IDC_KEYSSH2ECDSA)) {
                    int curveindex;
                    state->keytype = ECDSA;
                    curveindex = SendDlgItemMessage(hwnd, IDC_ECCURVE,
                                                    CB_GETCURSEL, 0, 0);
                    assert(curveindex >= 0);
                    assert(curveindex < n_ec_nist_curve_lengths);
                    state->curve_bits = ec_nist_curve_lengths[curveindex];
                } else if (IsDlgButtonChecked(hwnd, IDC_KEYSSH2EDDSA)) {
                    int curveindex;
                    state->keytype = EDDSA;
                    curveindex = SendDlgItemMessage(hwnd, IDC_EDCURVE,
                                                    CB_GETCURSEL, 0, 0);
                    assert(curveindex >= 0);
                    assert(curveindex < n_ec_ed_curve_lengths);
                    state->curve_bits = ec_ed_curve_lengths[curveindex];
                } else {
                    /* Somehow, no button was checked */
                    break;
                }

                if ((state->keytype == RSA || state->keytype == DSA) &&
                    state->key_bits < 256) {
                    char *message = dupprintf(
                        "PuTTYgen will not generate a key smaller than 256"
                        " bits.\nKey length reset to default %d. Continue?",
                        DEFAULT_KEY_BITS);
                    int ret = MessageBox(hwnd, message, "PuTTYgen Warning",
                                         MB_ICONWARNING | MB_OKCANCEL);
                    sfree(message);
                    if (ret != IDOK)
                        break;
                    state->key_bits = DEFAULT_KEY_BITS;
                    SetDlgItemInt(hwnd, IDC_BITS, DEFAULT_KEY_BITS, false);
                } else if ((state->keytype == RSA || state->keytype == DSA) &&
                           state->key_bits < DEFAULT_KEY_BITS) {
                    char *message = dupprintf(
                        "Keys shorter than %d bits are not recommended. "
                        "Really generate this key?", DEFAULT_KEY_BITS);
                    int ret = MessageBox(hwnd, message, "PuTTYgen Warning",
                                         MB_ICONWARNING | MB_OKCANCEL);
                    sfree(message);
                    if (ret != IDOK)
                        break;
                }

                if (state->keytype == RSA || state->keytype == DSA)
                    raw_entropy_required = (state->key_bits / 2) * 2;
                else if (state->keytype == ECDSA || state->keytype == EDDSA)
                    raw_entropy_required = (state->curve_bits / 2) * 2;
                else
                    unreachable("we must have initialised keytype by now");

                /* Bound the entropy collection above by the amount of
                 * data we can actually fit into the PRNG. Any more
                 * than that and it's doing no more good. */
                if (raw_entropy_required > random_seed_bits())
                    raw_entropy_required = random_seed_bits();

                raw_entropy_buf = snewn(raw_entropy_required, unsigned char);
                if (win_read_random(raw_entropy_buf, raw_entropy_required)) {
                    /*
                     * If we can get entropy from CryptGenRandom, use
                     * it. But CryptGenRandom isn't a kernel-level
                     * CPRNG (according to Wikipedia), and papers have
                     * been published cryptanalysing it. So we'll
                     * still do manual entropy collection; we'll just
                     * do it _as well_ as this.
                     */
                    random_reseed(
                        make_ptrlen(raw_entropy_buf, raw_entropy_required));
                }

                /*
                 * Manual entropy input, by making the user wave the
                 * mouse over the window a lot.
                 *
                 * My brief statistical tests on mouse movements
                 * suggest that there are about 2.5 bits of randomness
                 * in the x position, 2.5 in the y position, and 1.7
                 * in the message time, making 5.7 bits of
                 * unpredictability per mouse movement. However, other
                 * people have told me it's far less than that, so I'm
                 * going to be stupidly cautious and knock that down
                 * to a nice round 2. With this method, we require two
                 * words per mouse movement, so with 2 bits per mouse
                 * movement we expect 2 bits every 2 words, i.e. the
                 * number of _words_ of mouse data we want to collect
                 * is just the same as the number of _bits_ of entropy
                 * we want.
                 */
                state->entropy_required = raw_entropy_required;

                ui_set_state(hwnd, state, 1);
                SetDlgItemText(hwnd, IDC_GENERATING, entropy_msg);
                state->key_exists = false;

                state->entropy_got = 0;
                state->entropy = strbuf_new_nm();
                state->entropy_prev_msgtime = GetMessageTime();

                SendDlgItemMessage(hwnd, IDC_PROGRESS, PROGBAR_SETRANGE, 0,
                                   MAKELPARAM(0, state->entropy_required));
                SendDlgItemMessage(hwnd, IDC_PROGRESS, PROGBAR_SETPOS, 0, 0);

                smemclr(raw_entropy_buf, raw_entropy_required);
                sfree(raw_entropy_buf);
            }
            break;
          case IDC_SAVE:
          case IDC_EXPORT_OPENSSH_AUTO:
          case IDC_EXPORT_OPENSSH_NEW:
          case IDC_EXPORT_SSHCOM:
            if (HIWORD(wParam) != BN_CLICKED)
                break;
            state =
                (struct MainDlgState *) GetWindowLong(hwnd, GWL_USERDATA);
            if (state->key_exists) {
                char *passphrase, *passphrase2;
                int type, realtype;
                Filename *fn;

                if (state->ssh2)
                    realtype = SSH_KEYTYPE_SSH2;
                else
                    realtype = SSH_KEYTYPE_SSH1;

                if (LOWORD(wParam) == IDC_EXPORT_OPENSSH_AUTO)
                    type = SSH_KEYTYPE_OPENSSH_AUTO;
                else if (LOWORD(wParam) == IDC_EXPORT_OPENSSH_NEW)
                    type = SSH_KEYTYPE_OPENSSH_NEW;
                else if (LOWORD(wParam) == IDC_EXPORT_SSHCOM)
                    type = SSH_KEYTYPE_SSHCOM;
                else
                    type = realtype;

                if (type != realtype &&
                    import_target_type(type) != realtype) {
                    char msg[256];
                    sprintf(msg, "Cannot export an SSH-%d key in an SSH-%d"
                            " format", (state->ssh2 ? 2 : 1),
                            (state->ssh2 ? 1 : 2));
                    MessageBox(hwnd, msg,
                               "PuTTYgen Error", MB_OK | MB_ICONERROR);
                    break;
                }

                passphrase = GetDlgItemText_alloc(hwnd, IDC_PASSPHRASE1EDIT);
                passphrase2 = GetDlgItemText_alloc(hwnd, IDC_PASSPHRASE2EDIT);
                if (strcmp(passphrase, passphrase2)) {
                    MessageBox(hwnd,
                               "The two passphrases given do not match.",
                               "PuTTYgen Error", MB_OK | MB_ICONERROR);
                    burnstr(passphrase);
                    burnstr(passphrase2);
                    break;
                }
                burnstr(passphrase2);
                if (!*passphrase) {
                    int ret;
                    ret = MessageBox(hwnd,
                                     "Are you sure you want to save this key\n"
                                     "without a passphrase to protect it?",
                                     "PuTTYgen Warning",
                                     MB_YESNO | MB_ICONWARNING);
                    if (ret != IDYES) {
                        burnstr(passphrase);
                        break;
                    }
                }
                fn = request_file(
                    hwnd, "Save private key as:", NULL, true, NULL, false,
                    (type==realtype ? FILTER_KEY_FILES : FILTER_ALL_FILES));
                if (fn) {
                    int ret;
                    char *err = NULL;
                    FILE *fp = f_open(fn, "r", false);
                    if (fp) {
                        char *buffer;
                        fclose(fp);
                        buffer = dupprintf("Overwrite existing file\n%s?",
                                           filename_to_str(fn));
                        ret = MessageBox(hwnd, buffer, "PuTTYgen Warning",
                                         MB_YESNO | MB_ICONWARNING);
                        sfree(buffer);
                        if (ret != IDYES) {
                            burnstr(passphrase);
                            filename_free(fn);
                            break;
                        }
                    }

                    if (state->ssh2) {
                        if (type != realtype)
                            ret = export_ssh2(fn, type, &state->ssh2key,
                                              *passphrase ? passphrase : NULL);
                        else {
                            err = ppk_params_bad(&save_params,
                                                 *passphrase != '\0',
                                                 strlen(passphrase));
                            if (err) {
                                char *newerr = dupcat(
                                    "PPK parameters invalid: ", err, NULL);
                                sfree(err);
                                err = newerr;
                                ret = -1;
                            } else {
                                ret = ppk_save_f(
                                    fn, &state->ssh2key,
                                    *passphrase ? passphrase : NULL,
                                    &save_params);
                            }
                        }
                    } else {
                        if (type != realtype)
                            ret = export_ssh1(fn, type, &state->key,
                                              *passphrase ? passphrase : NULL);
                        else
                            ret = rsa1_save_f(fn, &state->key,
                                              *passphrase ? passphrase : NULL);
                    }
                    if (ret <= 0) {
                        MessageBox(hwnd, err ? err : "Unable to save key file",
                                   "PuTTYgen Error", MB_OK | MB_ICONERROR);
                    }
                    sfree(err);
                    filename_free(fn);
                }
                burnstr(passphrase);
            }
            break;
          case IDC_SAVEPUB:
            if (HIWORD(wParam) != BN_CLICKED)
                break;
            state =
                (struct MainDlgState *) GetWindowLong(hwnd, GWL_USERDATA);
            if (state->key_exists) {
                Filename *fn = request_file(
                    hwnd, "Save public key as:", NULL, true, NULL, false,
                    FILTER_ALL_FILES);
                if (fn) {
                    int ret;
                    FILE *fp = f_open(fn, "r", false);
                    if (fp) {
                        char *buffer;
                        fclose(fp);
                        buffer = dupprintf("Overwrite existing file\n%s?",
                                           filename_to_str(fn));
                        ret = MessageBox(hwnd, buffer, "PuTTYgen Warning",
                                         MB_YESNO | MB_ICONWARNING);
                        sfree(buffer);
                        if (ret != IDYES) {
                            filename_free(fn);
                            break;
                        }
                    }
                    fp = f_open(fn, "w", false);
                    if (!fp) {
                        MessageBox(hwnd, "Unable to open key file",
                                   "PuTTYgen Error", MB_OK | MB_ICONERROR);
                    } else {
                        if (state->ssh2) {
                            strbuf *blob = strbuf_new();
                            ssh_key_public_blob(
                                state->ssh2key.key, BinarySink_UPCAST(blob));
                            ssh2_write_pubkey(fp, state->ssh2key.comment,
                                              blob->u, blob->len,
                                              SSH_KEYTYPE_SSH2_PUBLIC_RFC4716);
                            strbuf_free(blob);
                        } else {
                            ssh1_write_pubkey(fp, &state->key);
                        }
                        if (fclose(fp) < 0) {
                            MessageBox(hwnd, "Unable to save key file",
                                       "PuTTYgen Error", MB_OK | MB_ICONERROR);
                        }
                    }
                    filename_free(fn);
                }
            }
            break;
          case IDC_LOAD:
          case IDC_IMPORT:
            if (HIWORD(wParam) != BN_CLICKED)
                break;
            state =
                (struct MainDlgState *) GetWindowLong(hwnd, GWL_USERDATA);
            if (!state->generation_thread_exists) {
                Filename *fn = request_file(
                    hwnd, "Load private key:", NULL, false, NULL, false,
                    (LOWORD(wParam) == IDC_LOAD ?
                     FILTER_KEY_FILES : FILTER_ALL_FILES));
                if (fn) {
                    load_key_file(hwnd, state, fn, LOWORD(wParam) != IDC_LOAD);
                    filename_free(fn);
                }
            }
            break;
          case IDC_ADDCERT:
            if (HIWORD(wParam) != BN_CLICKED)
                break;
            state =
                (struct MainDlgState *) GetWindowLong(hwnd, GWL_USERDATA);
            if (state->key_exists && !state->generation_thread_exists) {
                Filename *fn = request_file(
                    hwnd, "Load certificate:", NULL, false, NULL, false,
                    FILTER_ALL_FILES);
                if (fn) {
                    add_certificate(hwnd, state, fn);
                    filename_free(fn);
                }
            }
            break;
          case IDC_REMCERT:
            if (HIWORD(wParam) != BN_CLICKED)
                break;
            state =
                (struct MainDlgState *) GetWindowLong(hwnd, GWL_USERDATA);
            if (state->key_exists && !state->generation_thread_exists) {
                remove_certificate(hwnd, state);
            }
            break;
          case IDC_CERTMOREINFO: {
            struct certinfo_dialog_ctx ctx[1];
            if (HIWORD(wParam) != BN_CLICKED)
                break;
            state =
                (struct MainDlgState *) GetWindowLong(hwnd, GWL_USERDATA);
            if (!state->key_exists || !state->ssh2 || !state->ssh2key.key)
                break;
            if (!ssh_key_alg(state->ssh2key.key)->is_certificate)
                break;

            ctx->text = ssh_key_cert_info(state->ssh2key.key);
            ShinyDialogBox(hinst, MAKEINTRESOURCE(216),
                           "PuTTYgenCertInfo", hwnd, CertInfoProc, ctx);
            seat_dialog_text_free(ctx->text);
            break;
          }
        }
        return 0;
      case WM_DONEKEY:
        state = (struct MainDlgState *) GetWindowLong(hwnd, GWL_USERDATA);
        state->generation_thread_exists = false;
        state->key_exists = true;
        SendDlgItemMessage(hwnd, IDC_PROGRESS, PROGBAR_SETRANGE, 0,
                           MAKELPARAM(0, PROGRESSRANGE));
        SendDlgItemMessage(hwnd, IDC_PROGRESS, PROGBAR_SETPOS,
                           PROGRESSRANGE, 0);
        if (state->ssh2) {
            if (state->keytype == DSA) {
                state->ssh2key.key = &state->dsakey.sshk;
            } else if (state->keytype == ECDSA) {
                state->ssh2key.key = &state->eckey.sshk;
            } else if (state->keytype == EDDSA) {
                state->ssh2key.key = &state->edkey.sshk;
            } else {
                state->ssh2key.key = &state->key.sshk;
            }
            state->commentptr = &state->ssh2key.comment;
        } else {
            state->commentptr = &state->key.comment;
        }
        /*
         * Invent a comment for the key. We'll do this by including
         * the date in it. This will be so horrifyingly ugly that
         * the user will immediately want to change it, which is
         * what we want :-)
         */
        *state->commentptr = snewn(30, char);
        {
            struct tm tm;
            tm = ltime();
            if (state->keytype == DSA)
                strftime(*state->commentptr, 30, "dsa-key-%Y%m%d", &tm);
            else if (state->keytype == ECDSA)
                strftime(*state->commentptr, 30, "ecdsa-key-%Y%m%d", &tm);
            else if (state->keytype == EDDSA)
                strftime(*state->commentptr, 30, "eddsa-key-%Y%m%d", &tm);
            else
                strftime(*state->commentptr, 30, "rsa-key-%Y%m%d", &tm);
        }

        /*
         * Now update the key controls with all the key data.
         */
        {
            char *fp, *savecomment;
            /*
             * Blank passphrase, initially. This isn't dangerous,
             * because we will warn (Are You Sure?) before allowing
             * the user to save an unprotected private key.
             */
            SetDlgItemText(hwnd, IDC_PASSPHRASE1EDIT, "");
            SetDlgItemText(hwnd, IDC_PASSPHRASE2EDIT, "");
            /*
             * Set the comment.
             */
            SetDlgItemText(hwnd, IDC_COMMENTEDIT, *state->commentptr);
            /*
             * Set the key fingerprint.
             */
            savecomment = *state->commentptr;
            *state->commentptr = NULL;
            if (state->ssh2)
                fp = ssh2_fingerprint(state->ssh2key.key, state->fptype);
            else
                fp = rsa_ssh1_fingerprint(&state->key);
            SetDlgItemText(hwnd, IDC_FINGERPRINT, fp);
            sfree(fp);
            *state->commentptr = savecomment;
            /*
             * Construct a decimal representation of the key, for
             * pasting into .ssh/authorized_keys or
             * .ssh/authorized_keys2 on a Unix box.
             */
            if (state->ssh2) {
                setupbigedit2(hwnd, &state->ssh2key);
            } else {
                setupbigedit1(hwnd, &state->key);
            }
        }
        /*
         * Finally, hide the progress bar and show the key data.
         */
        ui_set_state(hwnd, state, 2);
        break;
      case WM_CLOSE:
        state = (struct MainDlgState *) GetWindowLong(hwnd, GWL_USERDATA);
        sfree(state);
        EndDialog(hwnd, 1);
        return 0;
    }
    return 0;
}

void cleanup_exit(int code)
{
    exit(code);
}

HINSTANCE hinst;

static NORETURN void opt_error(const char *fmt, ...)
{
    va_list ap;
    char *msg;
    va_start(ap, fmt);
    msg = dupvprintf(fmt, ap);
    va_end(ap);

    MessageBox(NULL, msg, "PuTTYgen command line error", MB_ICONERROR | MB_OK);

    exit(1);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmdline, int show)
{
    int ret;
    struct InitialParams params[1];
    int argbits;
    AuxMatchOpt amo;

    dll_hijacking_protection();
    enable_dit();

    init_common_controls();
    hinst = inst;

    params->keybutton = IDC_KEYSSH2RSA;
    params->primepolicybutton = IDC_PRIMEGEN_PROB;
    params->rsa_strong = false;
    params->fptype = SSH_FPTYPE_DEFAULT;
    params->keybits = DEFAULT_KEY_BITS;
    params->eccurve_index = DEFAULT_ECCURVE_INDEX;
    params->edcurve_index = DEFAULT_EDCURVE_INDEX;

    save_params = ppk_save_default_parameters;

    argbits = -1;
    amo = aux_match_opt_init(opt_error);
    while (!aux_match_done(&amo)) {
        CmdlineArg *valarg;
        if (aux_match_arg(&amo, &valarg)) {
            if (!cmdline_keyfile) {
                /*
                 * Assume the first argument to be a private key file, and
                 * attempt to load it.
                 */
                cmdline_keyfile = cmdline_arg_to_filename(valarg);
                continue;
            } else {
                opt_error("unexpected extra argument '%s'\n",
                          cmdline_arg_to_str(valarg));
            }
        } else if (aux_match_opt(&amo, NULL, "-pgpfp", NULL)) {
            pgp_fingerprints_msgbox(NULL);
            return 0;
        } else if (aux_match_opt(&amo, NULL, "-restrict-acl", "-restrict_acl",
                                 "-restrictacl", NULL)) {
            restrict_process_acl();
        } else if (aux_match_opt(&amo, &valarg, "-t", NULL)) {
            const char *val = cmdline_arg_to_str(valarg);
            if (!strcmp(val, "rsa") || !strcmp(val, "rsa2")) {
                params->keybutton = IDC_KEYSSH2RSA;
            } else if (!strcmp(val, "rsa1")) {
                params->keybutton = IDC_KEYSSH1;
            } else if (!strcmp(val, "dsa") || !strcmp(val, "dss")) {
                params->keybutton = IDC_KEYSSH2DSA;
            } else if (!strcmp(val, "ecdsa")) {
                params->keybutton = IDC_KEYSSH2ECDSA;
            } else if (!strcmp(val, "eddsa")) {
                params->keybutton = IDC_KEYSSH2EDDSA;
            } else if (!strcmp(val, "ed25519")) {
                params->keybutton = IDC_KEYSSH2EDDSA;
                argbits = 255;
            } else if (!strcmp(val, "ed448")) {
                params->keybutton = IDC_KEYSSH2EDDSA;
                argbits = 448;
            } else {
                opt_error("unknown key type '%s'\n", val);
            }
        } else if (aux_match_opt(&amo, &valarg, "-b", NULL)) {
            argbits = atoi(cmdline_arg_to_str(valarg));
        } else if (aux_match_opt(&amo, &valarg, "-E", NULL)) {
            const char *val = cmdline_arg_to_str(valarg);
            if (!strcmp(val, "md5"))
                params->fptype = SSH_FPTYPE_MD5;
            else if (!strcmp(val, "sha256"))
                params->fptype = SSH_FPTYPE_SHA256;
            else
                opt_error("unknown fingerprint type '%s'\n", val);
        } else if (aux_match_opt(&amo, &valarg, "-primes", NULL)) {
            const char *val = cmdline_arg_to_str(valarg);
            if (!strcmp(val, "probable") ||
                !strcmp(val, "probabilistic")) {
                params->primepolicybutton = IDC_PRIMEGEN_PROB;
            } else if (!strcmp(val, "provable") ||
                       !strcmp(val, "proven") ||
                       !strcmp(val, "simple") ||
                       !strcmp(val, "maurer-simple")) {
                params->primepolicybutton = IDC_PRIMEGEN_MAURER_SIMPLE;
            } else if (!strcmp(val, "provable-even") ||
                       !strcmp(val, "proven-even") ||
                       !strcmp(val, "even") ||
                       !strcmp(val, "complex") ||
                       !strcmp(val, "maurer-complex")) {
                params->primepolicybutton = IDC_PRIMEGEN_MAURER_COMPLEX;
            } else {
                opt_error("unrecognised prime-generation mode '%s'\n", val);
            }
        } else if (aux_match_opt(&amo, NULL, "-strong-rsa", NULL)) {
            params->rsa_strong = true;
        } else if (aux_match_opt(&amo, &valarg, "-ppk-param", "-ppk-params", NULL)) {
            char *val = dupstr(cmdline_arg_to_str(valarg));
            char *nextval;
            for (; val; val = nextval) {
                char *optvalue;
                char *end;
                unsigned long n;
                nextval = strchr(val, ',');
                if (nextval)
                    *nextval++ = '\0';

                optvalue = strchr(val, '=');
                if (!optvalue)
                    opt_error("PPK parameter '%s' expected a value\n", val);
                *optvalue++ = '\0';

                /* Non-numeric options */
                if (!strcmp(val, "kdf")) {
                    if (!strcmp(optvalue, "Argon2id") ||
                        !strcmp(optvalue, "argon2id")) {
                        save_params.argon2_flavour = Argon2id;
                    } else if (!strcmp(optvalue, "Argon2i") ||
                               !strcmp(optvalue, "argon2i")) {
                        save_params.argon2_flavour = Argon2i;
                    } else if (!strcmp(optvalue, "Argon2d") ||
                               !strcmp(optvalue, "argon2d")) {
                        save_params.argon2_flavour = Argon2d;
                    } else {
                        opt_error("unrecognised kdf '%s'\n", optvalue);
                    }
                    continue;
                }

                n = strtoul(optvalue, &end, 0);
                if (!*optvalue || *end)
                    opt_error("value '%s' for PPK parameter '%s': expected a "
                              "number\n", optvalue, val);

                if (!strcmp(val, "version")) {
                    save_params.fmt_version = n;
                } else if (!strcmp(val, "memory") ||
                           !strcmp(val, "mem")) {
                    save_params.argon2_mem = n;
                } else if (!strcmp(val, "time")) {
                    save_params.argon2_passes_auto = true;
                    save_params.argon2_milliseconds = n;
                } else if (!strcmp(val, "passes")) {
                    save_params.argon2_passes_auto = false;
                    save_params.argon2_passes = n;
                } else if (!strcmp(val, "parallelism") ||
                           !strcmp(val, "parallel")) {
                    save_params.argon2_parallelism = n;
                } else {
                    opt_error("unrecognised PPK parameter '%s'\n", val);
                }
            }
            sfree(val);
        } else {
            opt_error("unrecognised option '%s'\n",
                      cmdline_arg_to_str(amo.arglist->args[amo.index]));
        }
    }

    /* Translate argbits into eccurve_index and edcurve_index */
    if (argbits > 0) {
        switch (params->keybutton) {
          case IDC_KEYSSH2RSA:
          case IDC_KEYSSH1:
          case IDC_KEYSSH2DSA:
            params->keybits = argbits;
            break;
          case IDC_KEYSSH2ECDSA: {
            bool found = false;
            int j;
            for (j = 0; j < n_ec_nist_curve_lengths; j++)
                if (argbits == ec_nist_curve_lengths[j]) {
                    params->eccurve_index = j;
                    found = true;
                    break;
                }
            if (!found)
                opt_error("unsupported ECDSA bit length %d", argbits);
            break;
          }
          case IDC_KEYSSH2EDDSA: {
            bool found = false;
            int j;
            for (j = 0; j < n_ec_ed_curve_lengths; j++)
                if (argbits == ec_ed_curve_lengths[j]) {
                    params->edcurve_index = j;
                    found = true;
                    break;
                }
            if (!found)
                opt_error("unsupported EDDSA bit length %d", argbits);
            break;
          }
        }
    }

    random_setup_special();
    ret = DialogBoxParam(hinst, MAKEINTRESOURCE(201), NULL, MainDlgProc,
                         (LPARAM)params) != IDOK;

    cleanup_exit(ret);
    return ret;                        /* just in case optimiser complains */
}
