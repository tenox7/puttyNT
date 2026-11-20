/*
 * Pageant: the PuTTY Authentication Agent.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <ctype.h>
#include <assert.h>
#include <tchar.h>

#include "putty.h"
#include "ssh.h"
#include "misc.h"
#include "tree234.h"
#include "security-api.h"
#include "cryptoapi.h"
#include "pageant.h"
#include "licence.h"
#include "pageant-rc.h"
#include "storage.h"
#include <shellapi.h>


#ifdef NIM_ADD
#define WM_SYSTRAY   (WM_APP + 6)
#define WM_SYSTRAY2  (WM_APP + 7)
#endif

#define APPNAME "Pageant"

/* Titles and class names for invisible windows. IPCWINTITLE and
 * IPCCLASSNAME are critical to backwards compatibility: WM_COPYDATA
 * based Pageant clients will call FindWindow with those parameters
 * and expect to find the Pageant IPC receiver. */
#define TRAYWINTITLE  "Pageant"
#define TRAYCLASSNAME "PageantSysTray"
#define IPCWINTITLE   "Pageant"
#define IPCCLASSNAME  "Pageant"

static HWND traywindow;
static HWND keylist;
static HWND aboutbox;
static HMENU systray_menu, session_menu;
static bool already_running;

/*
 * Whether Pageant's menu is on an icon in the system tray. If not
 * (there is no tray in Windows NT 3.x, nor in NT 4.0 without
 * Explorer), Pageant is a minimised window - an icon on the desktop
 * or a button on the taskbar - and its menu is that window's system
 * menu.
 */
static bool tray_icon = false;
static FingerprintType fptype = SSH_FPTYPE_DEFAULT;

static char *putty_path;
static bool restrict_putty_acl = false;

/* CWD for "add key" file requester. */
static filereq_saved_dir *keypath = NULL;

/* From MSDN: In the WM_SYSCOMMAND message, the four low-order bits of
 * wParam are used by Windows, and should be masked off, so we shouldn't
 * attempt to store information in them. Hence all these identifiers have
 * the low 4 bits clear. Also, identifiers should < 0xF000. */

#define IDM_CLOSE              0x0010
#define IDM_VIEWKEYS           0x0020
#define IDM_ADDKEY             0x0030
#define IDM_ADDKEY_ENCRYPTED   0x0040
#define IDM_REMOVE_ALL         0x0050
#define IDM_REENCRYPT_ALL      0x0060
#define IDM_ABOUT              0x0080
#define IDM_PUTTY              0x0090
#define IDM_SESSIONS_BASE      0x1000
#define IDM_SESSIONS_MAX       0x2000
#define PUTTY_REGKEY      "Software\\SimonTatham\\PuTTY\\Sessions"
#define PUTTY_DEFAULT     "Default%20Settings"
static int initial_menuitems_count;

/*
 * Print a modal (Really Bad) message box and perform a fatal exit.
 */
void modalfatalbox(const char *fmt, ...)
{
    va_list ap;
    char *buf;

    va_start(ap, fmt);
    buf = dupvprintf(fmt, ap);
    va_end(ap);
    MessageBox(traywindow, buf, "Pageant Fatal Error",
               MB_SYSTEMMODAL | MB_ICONERROR | MB_OK);
    sfree(buf);
    exit(1);
}

struct PassphraseProcStruct {
    bool modal;
    PageantClientDialogId *dlgid;
    char *passphrase;
    const char *comment;
};

/*
 * Dialog-box function for the Licence box.
 */
static BOOL CALLBACK LicenceProc(HWND hwnd, UINT msg,
                                 WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
      case WM_INITDIALOG:
        SetDlgItemText(hwnd, IDC_LICENCE_TEXTBOX, LICENCE_TEXT("\r\n\r\n"));
        return 1;
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
      case WM_INITDIALOG: {
        char *buildinfo_text = buildinfo("\r\n");
        char *text = dupprintf(
            "Pageant\r\n\r\n%s\r\n\r\n%s\r\n\r\n%s",
            ver, buildinfo_text,
            "\251 " SHORT_COPYRIGHT_DETAILS ". All rights reserved.");
        sfree(buildinfo_text);
        SetDlgItemText(hwnd, IDC_ABOUT_TEXTBOX, text);
        MakeDlgItemBorderless(hwnd, IDC_ABOUT_TEXTBOX);
        sfree(text);
        return 1;
      }
      case WM_COMMAND:
        switch (LOWORD(wParam)) {
          case IDOK:
          case IDCANCEL:
            aboutbox = NULL;
            DestroyWindow(hwnd);
            return 0;
          case IDC_ABOUT_LICENCE:
            EnableWindow(hwnd, 0);
            DialogBox(hinst, MAKEINTRESOURCE(IDD_LICENCE), hwnd, LicenceProc);
            EnableWindow(hwnd, 1);
            SetActiveWindow(hwnd);
            return 0;
          case IDC_ABOUT_WEBSITE:
            /* Load web browser */
            ShellExecute(hwnd, "open",
                         "https://www.chiark.greenend.org.uk/~sgtatham/putty/",
                         0, 0, SW_SHOWDEFAULT);
            return 0;
        }
        return 0;
      case WM_CLOSE:
        aboutbox = NULL;
        DestroyWindow(hwnd);
        return 0;
    }
    return 0;
}

static HWND modal_passphrase_hwnd = NULL;
static HWND nonmodal_passphrase_hwnd = NULL;

static void end_passphrase_dialog(HWND hwnd, int result)
{
    struct PassphraseProcStruct *p = (struct PassphraseProcStruct *)
        GetWindowLong(hwnd, GWL_USERDATA);

    if (p->modal) {
        EndDialog(hwnd, result);
    } else {
        /*
         * Destroy this passphrase dialog box before passing the
         * results back to the main pageant.c, to avoid re-entrancy
         * issues.
         *
         * If we successfully got a passphrase from the user, but it
         * was _wrong_, then pageant_passphrase_request_success will
         * respond by calling back - synchronously - to our
         * ask_passphrase() implementation, which will expect the
         * previous value of nonmodal_passphrase_hwnd to have already
         * been cleaned up.
         */
        SetWindowLong(hwnd, GWL_USERDATA, (LONG) NULL);
        DestroyWindow(hwnd);
        nonmodal_passphrase_hwnd = NULL;

        if (result)
            pageant_passphrase_request_success(
                p->dlgid, ptrlen_from_asciz(p->passphrase));
        else
            pageant_passphrase_request_refused(p->dlgid);

        burnstr(p->passphrase);
        sfree(p);
    }
}

/*
 * Dialog-box function for the passphrase box.
 */
static BOOL CALLBACK PassphraseProc(HWND hwnd, UINT msg,
                                    WPARAM wParam, LPARAM lParam)
{
    struct PassphraseProcStruct *p;

    if (msg == WM_INITDIALOG) {
        p = (struct PassphraseProcStruct *) lParam;
        SetWindowLong(hwnd, GWL_USERDATA, (LONG) p);
    } else {
        p = (struct PassphraseProcStruct *)
            GetWindowLong(hwnd, GWL_USERDATA);
    }

    switch (msg) {
      case WM_INITDIALOG: {
        RECT rs;
        RECT rd;
        HWND hw;
        if (p->modal)
            modal_passphrase_hwnd = hwnd;

        /*
         * Centre the window.
         */

        hw = GetDesktopWindow();
        if (GetWindowRect(hw, &rs) && GetWindowRect(hwnd, &rd))
            MoveWindow(hwnd,
                       (rs.right + rs.left + rd.left - rd.right) / 2,
                       (rs.bottom + rs.top + rd.top - rd.bottom) / 2,
                       rd.right - rd.left, rd.bottom - rd.top, true);

        SetForegroundWindow(hwnd);
        SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        if (!p->modal)
            SetActiveWindow(hwnd); /* this won't have happened automatically */
        if (p->comment)
            SetDlgItemText(hwnd, IDC_PASSPHRASE_FINGERPRINT, p->comment);
        burnstr(p->passphrase);
        p->passphrase = dupstr("");
        SetDlgItemText(hwnd, IDC_PASSPHRASE_EDITBOX, p->passphrase);
        return 0;
      }
      case WM_COMMAND:
        switch (LOWORD(wParam)) {
          case IDOK:
            if (p->passphrase)
                end_passphrase_dialog(hwnd, 1);
            else
                MessageBeep(0);
            return 0;
          case IDCANCEL:
            end_passphrase_dialog(hwnd, 0);
            return 0;
          case IDC_PASSPHRASE_EDITBOX:
            if ((HIWORD(wParam) == EN_CHANGE) && p->passphrase) {
                burnstr(p->passphrase);
                p->passphrase = GetDlgItemText_alloc(
                    hwnd, IDC_PASSPHRASE_EDITBOX);
            }
            return 0;
        }
        return 0;
      case WM_CLOSE:
        end_passphrase_dialog(hwnd, 0);
        return 0;
    }
    return 0;
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
        "You can perform this conversion by loading the key\n"
        "into PuTTYgen and then saving it again.";

    MessageBox(NULL, message, mbtitle, MB_OK);
}

struct keylist_update_ctx {
    HDC hdc;
    int algbitswidth, algwidth, bitswidth, hashwidth;
    bool enable_remove_controls;
    bool enable_reencrypt_controls;
};

struct keylist_display_data {
    strbuf *alg, *bits, *hash, *comment, *info;
};

static void keylist_update_callback(
    void *vctx, char **fingerprints, const char *comment, uint32_t ext_flags,
    struct pageant_pubkey *key)
{
    struct keylist_update_ctx *ctx = (struct keylist_update_ctx *)vctx;
    FingerprintType this_type = ssh2_pick_fingerprint(fingerprints, fptype);
    ptrlen fingerprint = ptrlen_from_asciz(fingerprints[this_type]);

    struct keylist_display_data *disp = snew(struct keylist_display_data);
    SIZE sz;

    memset(&sz, 0, sizeof(sz));         /* GetTextExtentPoint32 fails on Win32s */
    disp->alg = strbuf_new();
    disp->bits = strbuf_new();
    disp->hash = strbuf_new();
    disp->comment = strbuf_new();
    disp->info = strbuf_new();

    /* There is at least one key, so the controls for removing keys
     * should be enabled */
    ctx->enable_remove_controls = true;

    switch (key->ssh_version) {
      case 1: {
        /*
         * Expect the fingerprint to contain two words: bit count and
         * hash.
         */
        put_dataz(disp->alg, "SSH-1");
        put_datapl(disp->bits, ptrlen_get_word(&fingerprint, " "));
        put_datapl(disp->hash, ptrlen_get_word(&fingerprint, " "));
        break;
      }

      case 2: {
        /*
         * Expect the fingerprint to contain three words: algorithm
         * name, bit count, hash.
         */
        const ssh_keyalg *alg = pubkey_blob_to_alg(
            ptrlen_from_strbuf(key->blob));

        ptrlen keytype_word = ptrlen_get_word(&fingerprint, " ");
        ptrlen bits_word;
        if (alg) {
            /* Use our own human-legible algorithm names if available,
             * because they fit better in the space. (Certificate key
             * algorithm names in particular are terribly long.) */
            char *alg_desc = ssh_keyalg_desc(alg);
            put_dataz(disp->alg, alg_desc);
            sfree(alg_desc);
        } else {
            put_datapl(disp->alg, keytype_word);
        }

        bits_word = ptrlen_get_word(&fingerprint, " ");
        if (alg && ssh_keyalg_variable_size(alg))
            put_datapl(disp->bits, bits_word);

        put_datapl(disp->hash, ptrlen_get_word(&fingerprint, " "));
      }
    }

    put_dataz(disp->comment, comment);

    if (disp->bits->len) {
        GetTextExtentPoint32(ctx->hdc, disp->alg->s, disp->alg->len, &sz);
        if (ctx->algwidth < sz.cx) ctx->algwidth = sz.cx;
        GetTextExtentPoint32(ctx->hdc, disp->bits->s, disp->bits->len, &sz);
        if (ctx->bitswidth < sz.cx) ctx->bitswidth = sz.cx;
    } else {
        GetTextExtentPoint32(ctx->hdc, disp->alg->s, disp->alg->len, &sz);
        if (ctx->algbitswidth < sz.cx) ctx->algbitswidth = sz.cx;
    }
    GetTextExtentPoint32(ctx->hdc, disp->hash->s, disp->hash->len, &sz);
    if (ctx->hashwidth < sz.cx) ctx->hashwidth = sz.cx;

    if (ext_flags & LIST_EXTENDED_FLAG_HAS_NO_CLEARTEXT_KEY) {
        BinarySink_put_fmt(BinarySink_UPCAST(disp->info), "(encrypted)");
    } else if (ext_flags & LIST_EXTENDED_FLAG_HAS_ENCRYPTED_KEY_FILE) {
        BinarySink_put_fmt(BinarySink_UPCAST(disp->info), "(re-encryptable)");

        /* At least one key can be re-encrypted */
        ctx->enable_reencrypt_controls = true;
    }

    /* This list box is owner-drawn but doesn't have LBS_HASSTRINGS,
     * so we can use LB_ADDSTRING to hand the list box our display
     * info pointer */
    SendDlgItemMessage(keylist, IDC_KEYLIST_LISTBOX,
                       LB_ADDSTRING, 0, (LPARAM)disp);
}

/* Column start positions for the list box, in pixels (not dialog units). */
static int colpos_bits, colpos_hash, colpos_comment;

/*
 * Update the visible key list.
 */
void keylist_update(void)
{
    if (keylist) {
        char *errmsg;
        struct keylist_update_ctx ctx[1];
        int status;
        SIZE sz;
        int gutter;
        /*
         * Clear the previous list box content and free their display
         * structures.
         */
        {
            int nitems = SendDlgItemMessage(keylist, IDC_KEYLIST_LISTBOX,
                                            LB_GETCOUNT, 0, 0);
            int i;
            for (i = 0; i < nitems; i++) {
                struct keylist_display_data *disp =
                    (struct keylist_display_data *)SendDlgItemMessage(
                        keylist, IDC_KEYLIST_LISTBOX, LB_GETITEMDATA, i, 0);
                strbuf_free(disp->alg);
                strbuf_free(disp->bits);
                strbuf_free(disp->hash);
                strbuf_free(disp->comment);
                strbuf_free(disp->info);
                sfree(disp);
            }
        }
        SendDlgItemMessage(keylist, IDC_KEYLIST_LISTBOX,
                           LB_RESETCONTENT, 0, 0);

        ctx->enable_remove_controls = false;
        ctx->enable_reencrypt_controls = false;
        ctx->algbitswidth = ctx->algwidth = 0;
        ctx->bitswidth = ctx->hashwidth = 0;
        ctx->hdc = GetDC(keylist);
        SelectObject(ctx->hdc, (HFONT)SendMessage(keylist, WM_GETFONT, 0, 0));
        status = pageant_enum_keys(keylist_update_callback, ctx, &errmsg);

        GetTextExtentPoint32(ctx->hdc, "MM", 2, &sz);
        gutter = sz.cx;

        ReleaseDC(keylist, ctx->hdc);
        colpos_hash = ctx->algwidth + ctx->bitswidth + 2*gutter;
        if (colpos_hash < ctx->algbitswidth + gutter)
            colpos_hash = ctx->algbitswidth + gutter;
        colpos_bits = colpos_hash - ctx->bitswidth - gutter;
        colpos_comment = colpos_hash + ctx->hashwidth + gutter;
        assert(status == PAGEANT_ACTION_OK);
        assert(!errmsg);

        SendDlgItemMessage(keylist, IDC_KEYLIST_LISTBOX,
                           LB_SETCURSEL, (WPARAM) - 1, 0);

        EnableWindow(GetDlgItem(keylist, IDC_KEYLIST_REMOVE),
                     ctx->enable_remove_controls);
        EnableWindow(GetDlgItem(keylist, IDC_KEYLIST_REENCRYPT),
                     ctx->enable_reencrypt_controls);
    }
}

static void win_add_keyfile(Filename *filename, bool encrypted)
{
    char *err;
    int ret;

    /*
     * Try loading the key without a passphrase. (Or rather, without a
     * _new_ passphrase; pageant_add_keyfile will take care of trying
     * all the passphrases we've already stored.)
     */
    ret = pageant_add_keyfile(filename, NULL, &err, encrypted);
    if (ret == PAGEANT_ACTION_OK) {
        goto done;
    } else if (ret == PAGEANT_ACTION_FAILURE) {
        goto error;
    }

    /*
     * OK, a passphrase is needed, and we've been given the key
     * comment to use in the passphrase prompt.
     */
    while (1) {
        int dlgret;
        struct PassphraseProcStruct pps;
        pps.modal = true;
        pps.dlgid = NULL;
        pps.passphrase = NULL;
        pps.comment = err;
        dlgret = DialogBoxParam(
            hinst, MAKEINTRESOURCE(IDD_LOAD_PASSPHRASE),
            NULL, PassphraseProc, (LPARAM) &pps);
        modal_passphrase_hwnd = NULL;

        if (!dlgret) {
            burnstr(pps.passphrase);
            goto done;                 /* operation cancelled */
        }

        sfree(err);

        assert(pps.passphrase != NULL);

        ret = pageant_add_keyfile(filename, pps.passphrase, &err, false);
        burnstr(pps.passphrase);

        if (ret == PAGEANT_ACTION_OK) {
            goto done;
        } else if (ret == PAGEANT_ACTION_FAILURE) {
            goto error;
        }
    }

  error:
    message_box(traywindow, err, APPNAME, MB_OK | MB_ICONERROR, false,
                HELPCTXID(errors_cantloadkey));
  done:
    sfree(err);
    return;
}

/*
 * Prompt for a key file to add, and add it.
 */
static void prompt_add_keyfile(bool encrypted)
{
    struct request_multi_file_return *rmf;
    if (!keypath)
        keypath = filereq_saved_dir_new();

    rmf = request_multi_file(
        traywindow, "Select Private Key File", NULL, false,
        keypath, true, FILTER_KEY_FILES);

    if (rmf) {
        size_t i;
        for (i = 0; i < rmf->nfilenames; i++)
            win_add_keyfile(rmf->filenames[i], encrypted);
        request_multi_file_free(rmf);

        keylist_update();
        pageant_forget_passphrases();
    }
}

/*
 * Dialog-box function for the key list box.
 */
static BOOL CALLBACK KeyListProc(HWND hwnd, UINT msg,
                                 WPARAM wParam, LPARAM lParam)
{
    static const struct {
        const char *name;
        FingerprintType value;
    } fptypes[] = {
        {"SHA256", SSH_FPTYPE_SHA256},
        {"MD5", SSH_FPTYPE_MD5},
        {"SHA256 including certificate", SSH_FPTYPE_SHA256_CERT},
        {"MD5 including certificate", SSH_FPTYPE_MD5_CERT},
    };

    switch (msg) {
      case WM_INITDIALOG: {
        /*
         * Centre the window.
         */
        RECT rs, rd;
        HWND hw;
        int selection;
        size_t i;

        hw = GetDesktopWindow();
        if (GetWindowRect(hw, &rs) && GetWindowRect(hwnd, &rd))
            MoveWindow(hwnd,
                       (rs.right + rs.left + rd.left - rd.right) / 2,
                       (rs.bottom + rs.top + rd.top - rd.bottom) / 2,
                       rd.right - rd.left, rd.bottom - rd.top, true);

        keylist = hwnd;

        selection = 0;
        for (i = 0; i < lenof(fptypes); i++) {
            SendDlgItemMessage(hwnd, IDC_KEYLIST_FPTYPE, CB_ADDSTRING,
                               0, (LPARAM)fptypes[i].name);
            if (fptype == fptypes[i].value)
                selection = (int)i;
        }
        SendDlgItemMessage(hwnd, IDC_KEYLIST_FPTYPE,
                           CB_SETCURSEL, 0, selection);

        keylist_update();
        return 0;
      }
      case WM_MEASUREITEM: {
        MEASUREITEMSTRUCT *mi;
        RECT r;
        assert(wParam == IDC_KEYLIST_LISTBOX);

        mi = (MEASUREITEMSTRUCT *)lParam;

        /*
         * Our list box is owner-drawn, but we put normal text in it.
         * So the line height is the same as it would normally be,
         * which is 8 dialog units.
         */
        r.left = r.right = r.top = 0;
        r.bottom = 8;
        MapDialogRect(hwnd, &r);
        mi->itemHeight = r.bottom;

        return 0;
      }
      case WM_DRAWITEM: {
        DRAWITEMSTRUCT *di;
        assert(wParam == IDC_KEYLIST_LISTBOX);

        di = (DRAWITEMSTRUCT *)lParam;

        if (di->itemAction == ODA_FOCUS) {
            /* Just toggle the focus rectangle either on or off. This
             * is an XOR-type function, so it's the same call in
             * either case. */
            DrawFocusRect(di->hDC, &di->rcItem);
        } else {
            /* Draw the full text. */
            bool selected = (di->itemState & ODS_SELECTED);
            COLORREF newfg = GetSysColor(
                selected ? COLOR_HIGHLIGHTTEXT : COLOR_WINDOWTEXT);
            COLORREF newbg = GetSysColor(
                selected ? COLOR_HIGHLIGHT : COLOR_WINDOW);
            COLORREF oldfg = SetTextColor(di->hDC, newfg);
            COLORREF oldbg = SetBkColor(di->hDC, newbg);

            HFONT font = (HFONT)SendMessage(hwnd, WM_GETFONT, 0, 0);
            HFONT oldfont = SelectObject(di->hDC, font);
            struct keylist_display_data *disp;
            RECT r;
            strbuf *sb;

            /* ExtTextOut("") is an easy way to just draw the
             * background rectangle */
            ExtTextOut(di->hDC, di->rcItem.left, di->rcItem.top,
                       ETO_OPAQUE | ETO_CLIPPED, &di->rcItem, "", 0, NULL);

            disp = (struct keylist_display_data *)di->itemData;


            /* Apparently real list boxes start drawing at x=1, not x=0 */
            r.left = r.top = r.bottom = 0;
            r.right = 1;
            MapDialogRect(hwnd, &r);
            ExtTextOut(di->hDC, di->rcItem.left + r.right, di->rcItem.top,
                       ETO_CLIPPED, &di->rcItem, disp->alg->s,
                       disp->alg->len, NULL);

            if (disp->bits->len) {
                ExtTextOut(di->hDC, di->rcItem.left + r.right + colpos_bits,
                           di->rcItem.top, ETO_CLIPPED, &di->rcItem,
                           disp->bits->s, disp->bits->len, NULL);
            }

            ExtTextOut(di->hDC, di->rcItem.left + r.right + colpos_hash,
                       di->rcItem.top, ETO_CLIPPED, &di->rcItem,
                       disp->hash->s, disp->hash->len, NULL);

            sb = strbuf_new();
            put_datapl(sb, ptrlen_from_strbuf(disp->comment));
            if (disp->info->len) {
                put_byte(sb, '\t');
                put_datapl(sb, ptrlen_from_strbuf(disp->info));
            }

            TabbedTextOut(di->hDC, di->rcItem.left + r.right + colpos_comment,
                          di->rcItem.top, sb->s, sb->len, 0, NULL, 0);

            strbuf_free(sb);

            SetTextColor(di->hDC, oldfg);
            SetBkColor(di->hDC, oldbg);
            SelectObject(di->hDC, oldfont);

            if (di->itemState & ODS_FOCUS)
                DrawFocusRect(di->hDC, &di->rcItem);
        }
        return 0;
      }
      case WM_COMMAND:
        switch (LOWORD(wParam)) {
          case IDOK:
          case IDCANCEL:
            keylist = NULL;
            DestroyWindow(hwnd);
            return 0;
          case IDC_KEYLIST_ADDKEY:
          case IDC_KEYLIST_ADDKEY_ENC:
            if (HIWORD(wParam) == BN_CLICKED ||
                HIWORD(wParam) == BN_DOUBLECLICKED) {
                if (modal_passphrase_hwnd) {
                    MessageBeep(MB_ICONERROR);
                    SetForegroundWindow(modal_passphrase_hwnd);
                    break;
                }
                prompt_add_keyfile(LOWORD(wParam) == IDC_KEYLIST_ADDKEY_ENC);
            }
            return 0;
          case IDC_KEYLIST_REMOVE:
          case IDC_KEYLIST_REENCRYPT:
            if (HIWORD(wParam) == BN_CLICKED ||
                HIWORD(wParam) == BN_DOUBLECLICKED) {
                int i;
                int rCount, sCount;
                int *selectedArray;

                /* our counter within the array of selected items */
                int itemNum;

                /* get the number of items selected in the list */
                int numSelected = SendDlgItemMessage(
                    hwnd, IDC_KEYLIST_LISTBOX, LB_GETSELCOUNT, 0, 0);

                /* none selected? that was silly */
                if (numSelected == 0) {
                    MessageBeep(0);
                    break;
                }

                /* get item indices in an array */
                selectedArray = snewn(numSelected, int);
                SendDlgItemMessage(hwnd, IDC_KEYLIST_LISTBOX, LB_GETSELITEMS,
                                   numSelected, (WPARAM)selectedArray);

                itemNum = numSelected - 1;
                rCount = pageant_count_ssh1_keys();
                sCount = pageant_count_ssh2_keys();

                /* go through the non-rsakeys until we've covered them all,
                 * and/or we're out of selected items to check. note that
                 * we go *backwards*, to avoid complications from deleting
                 * things hence altering the offset of subsequent items
                 */
                for (i = sCount - 1; (itemNum >= 0) && (i >= 0); i--) {
                    if (selectedArray[itemNum] == rCount + i) {
                        switch (LOWORD(wParam)) {
                          case IDC_KEYLIST_REMOVE:
                            pageant_delete_nth_ssh2_key(i);
                            break;
                          case IDC_KEYLIST_REENCRYPT:
                            pageant_reencrypt_nth_ssh2_key(i);
                            break;
                        }
                        itemNum--;
                    }
                }

                /* do the same for the rsa keys */
                for (i = rCount - 1; (itemNum >= 0) && (i >= 0); i--) {
                    if (selectedArray[itemNum] == i) {
                        switch (LOWORD(wParam)) {
                          case IDC_KEYLIST_REMOVE:
                            pageant_delete_nth_ssh1_key(i);
                            break;
                          case IDC_KEYLIST_REENCRYPT:
                            /* SSH-1 keys can't be re-encrypted */
                            break;
                        }
                        itemNum--;
                    }
                }

                sfree(selectedArray);
                keylist_update();
            }
            return 0;
          case IDC_KEYLIST_FPTYPE:
            if (HIWORD(wParam) == CBN_SELCHANGE) {
                int selection = SendDlgItemMessage(
                    hwnd, IDC_KEYLIST_FPTYPE, CB_GETCURSEL, 0, 0);
                if (selection >= 0 && (size_t)selection < lenof(fptypes)) {
                    fptype = fptypes[selection].value;
                    keylist_update();
                }
            }
            return 0;
        }
        return 0;
      case WM_CLOSE:
        keylist = NULL;
        DestroyWindow(hwnd);
        return 0;
    }
    return 0;
}

#ifdef NIM_ADD
/* Set up a system tray icon */
static BOOL AddTrayIcon(HWND hwnd)
{
    NOTIFYICONDATA tnid;

    tnid.cbSize = sizeof(NOTIFYICONDATA);
    tnid.hWnd = hwnd;
    tnid.uID = 1;              /* unique within this systray use */
    tnid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    tnid.uCallbackMessage = WM_SYSTRAY;
    tnid.hIcon = LoadIcon(hinst, MAKEINTRESOURCE(IDI_TRAYICON));
    strcpy(tnid.szTip, "Pageant (PuTTY authentication agent)");

    return Shell_NotifyIcon(NIM_ADD, &tnid);
}
#endif

/* Update the saved-sessions menu. */
static void update_sessions(void)
{
    int num_entries;
    settings_e *handle;
    strbuf *sb;

    int index_menu;

    if (!putty_path)
        return;

    for (num_entries = GetMenuItemCount(session_menu);
         num_entries > initial_menuitems_count;
         num_entries--)
        RemoveMenu(session_menu, 0, MF_BYPOSITION);

    index_menu = 0;

    sb = strbuf_new();
    if ((handle = enum_settings_start()) != NULL) {
        while (enum_settings_next(handle, sb)) {
            if (strcmp(sb->s, DEFAULT_SESSION_NAME) != 0) {
                InsertMenu(session_menu, index_menu, MF_BYPOSITION | MF_STRING,
                           (index_menu * 16) + IDM_SESSIONS_BASE, sb->s);
                index_menu++;
            }
            strbuf_clear(sb);
        }
        enum_settings_finish(handle);
    }
    strbuf_free(sb);

    if (index_menu == 0)
        InsertMenu(session_menu, index_menu,
                   MF_BYPOSITION | MF_STRING | MF_GRAYED, 0, "(No sessions)");
}

/*
 * Versions of Pageant prior to 0.61 expected this SID on incoming
 * communications. For backwards compatibility, and more particularly
 * for compatibility with derived works of PuTTY still using the old
 * Pageant client code, we accept it as an alternative to the one
 * returned from get_user_sid().
 */
PSID get_default_sid(void)
{
#ifdef WIN32S_COMPAT
    /* Win32s has no security descriptors to derive a SID from */
    return NULL;
#else
    HANDLE proc = NULL;
    DWORD sidlen;
    PSECURITY_DESCRIPTOR psd = NULL;
    PSID sid = NULL, copy = NULL, ret = NULL;

    if ((proc = OpenProcess(MAXIMUM_ALLOWED, false,
                            GetCurrentProcessId())) == NULL)
        goto cleanup;

    if (!get_object_owner(proc, &sid, &psd))
        goto cleanup;

    sidlen = GetLengthSid(sid);

    copy = (PSID)smalloc(sidlen);

    if (!CopySid(sidlen, copy, sid))
        goto cleanup;

    /* Success. Move sid into the return value slot, and null it out
     * to stop the cleanup code freeing it. */
    ret = copy;
    copy = NULL;

  cleanup:
    if (proc != NULL)
        CloseHandle(proc);
    if (psd != NULL)
        LocalFree(psd);
    if (copy != NULL)
        sfree(copy);

    return ret;
#endif /* !WIN32S_COMPAT */
}

struct WmCopydataTransaction {
    char *length, *body;
    size_t bodysize, bodylen;
    HANDLE ev_msg_ready, ev_reply_ready;
} wmct;

static struct PageantClient wmcpc;

static void wm_copydata_got_msg(void *vctx)
{
    pageant_handle_msg(&wmcpc, NULL, make_ptrlen(wmct.body, wmct.bodylen));
}

static void wm_copydata_got_response(
    PageantClient *pc, PageantClientRequestId *reqid, ptrlen response)
{
    if (response.len > wmct.bodysize) {
        /* Output would overflow message buffer. Replace with a
         * failure message. */
        static const unsigned char failure[] = { SSH_AGENT_FAILURE };
        response = make_ptrlen(failure, lenof(failure));
        assert(response.len <= wmct.bodysize);
    }

    PUT_32BIT_MSB_FIRST(wmct.length, response.len);
    memcpy(wmct.body, response.ptr, response.len);

#ifdef WIN32S_COMPAT
    /* Nothing to signal: the request was handled synchronously by the
     * caller, above */
    (void) 0;
#else
    SetEvent(wmct.ev_reply_ready);
#endif
}

static bool ask_passphrase_common(PageantClientDialogId *dlgid,
                                  const char *comment)
{
    struct PassphraseProcStruct *pps;
    /* Pageant core should be serialising requests, so we never expect
     * a passphrase prompt to exist already at this point */
    assert(!nonmodal_passphrase_hwnd);

    pps = snew(struct PassphraseProcStruct);
    pps->modal = false;
    pps->dlgid = dlgid;
    pps->passphrase = NULL;
    pps->comment = comment;

    nonmodal_passphrase_hwnd = CreateDialogParam(
        hinst, MAKEINTRESOURCE(IDD_ONDEMAND_PASSPHRASE),
        NULL, PassphraseProc, (LPARAM)pps);

    /*
     * Try to put this passphrase prompt into the foreground.
     *
     * This will probably not succeed in giving it the actual keyboard
     * focus, because Windows is quite opposed to applications being
     * able to suddenly steal the focus on their own initiative.
     *
     * That makes sense in a lot of situations, as a defensive
     * measure. If you were about to type a password or other secret
     * data into the window you already had focused, and some
     * malicious app stole the focus, it might manage to trick you
     * into typing your secrets into _it_ instead.
     *
     * In this case it's possible to regard the same defensive measure
     * as counterproductive, because the effect if we _do_ steal focus
     * is that you type something into our passphrase prompt that
     * isn't the passphrase, and we fail to decrypt the key, and no
     * harm is done. Whereas the effect of the user wrongly _assuming_
     * the new passphrase prompt has the focus is much worse: now you
     * type your highly secret passphrase into some other window you
     * didn't mean to trust with that information - such as the
     * agent-forwarded PuTTY in which you just ran an ssh command,
     * which the _whole point_ was to avoid telling your passphrase to!
     *
     * On the other hand, I'm sure _every_ application author can come
     * up with an argument for why they think _they_ should be allowed
     * to steal the focus. Probably most of them include the claim
     * that no harm is done if their application receives data
     * intended for something else, and of course that's not always
     * true!
     *
     * In any case, I don't know of anything I can do about it, or
     * anything I _should_ do about it if I could. If anyone thinks
     * they can improve on all this, patches are welcome.
     */
    SetForegroundWindow(nonmodal_passphrase_hwnd);

    return true;
}

static bool wm_copydata_ask_passphrase(
    PageantClient *pc, PageantClientDialogId *dlgid, const char *comment)
{
    return ask_passphrase_common(dlgid, comment);
}

static const PageantClientVtable wmcpc_vtable = {
    /* .log = */ NULL, /* no logging in this client */
    /* .got_response = */ wm_copydata_got_response,
    /* .ask_passphrase = */ wm_copydata_ask_passphrase,
};

static char *answer_filemapping_message(const char *mapname)
{
    HANDLE maphandle = INVALID_HANDLE_VALUE;
    void *mapaddr = NULL;
    char *err = NULL;
    size_t mapsize;
    unsigned msglen;

    PSID mapsid = NULL;
    PSID expectedsid = NULL;
    PSID expectedsid_bc = NULL;
    PSECURITY_DESCRIPTOR psd = NULL;

    wmct.length = wmct.body = NULL;

#ifdef DEBUG_IPC
    debug(("mapname = \"%s\"\n", mapname));
#endif

    maphandle = OpenFileMapping(FILE_MAP_ALL_ACCESS, false, mapname);
    if (maphandle == NULL || maphandle == INVALID_HANDLE_VALUE) {
        err = dupprintf("OpenFileMapping(\"%s\"): %s",
                        mapname, win_strerror(GetLastError()));
        goto cleanup;
    }

#ifdef DEBUG_IPC
    debug(("maphandle = %p\n", maphandle));
#endif

#ifndef WIN32S_COMPAT
    if ((expectedsid = get_user_sid()) == NULL) {
        err = dupstr("unable to get user SID");
        goto cleanup;
    }

    if ((expectedsid_bc = get_default_sid()) == NULL) {
        err = dupstr("unable to get default SID");
        goto cleanup;
    }

    if (!get_object_owner(maphandle, &mapsid, &psd)) {
        err = dupprintf("unable to get owner of file mapping: %s",
                        win_strerror(GetLastError()));
        goto cleanup;
    }

    if (!EqualSid(mapsid, expectedsid) &&
        !EqualSid(mapsid, expectedsid_bc)) {
        err = dupstr("wrong owning SID of file mapping");
        goto cleanup;
    }
#else
    /*
     * Win32s implements no process security model at all: any process
     * can already open the mapping and send this message. The check is
     * meaningless there, and refusing every caller would make the
     * agent useless, so skip it.
     */
#endif /* WIN32S_COMPAT */

    mapaddr = MapViewOfFile(maphandle, FILE_MAP_WRITE, 0, 0, 0);
    if (!mapaddr) {
        err = dupprintf("unable to obtain view of file mapping: %s",
                        win_strerror(GetLastError()));
        goto cleanup;
    }

#ifdef DEBUG_IPC
    debug(("mapped address = %p\n", mapaddr));
#endif

    {
        MEMORY_BASIC_INFORMATION mbi;
        size_t mbiSize = VirtualQuery(mapaddr, &mbi, sizeof(mbi));
        if (mbiSize == 0) {
            err = dupprintf("unable to query view of file mapping: %s",
                            win_strerror(GetLastError()));
            goto cleanup;
        }
        if (mbiSize < (offsetof(MEMORY_BASIC_INFORMATION, RegionSize) +
                       sizeof(mbi.RegionSize))) {
            err = dupstr("VirtualQuery returned too little data to get "
                         "region size");
            goto cleanup;
        }

        mapsize = mbi.RegionSize;
    }
#ifdef DEBUG_IPC
    debug(("region size = %"SIZEu"\n", mapsize));
#endif
    if (mapsize < 5) {
        err = dupstr("mapping smaller than smallest possible request");
        goto cleanup;
    }

    wmct.length = (char *)mapaddr;
    msglen = GET_32BIT_MSB_FIRST(wmct.length);

#ifdef DEBUG_IPC
    debug(("msg length=%08x, msg type=%02x\n",
           msglen, (unsigned)((unsigned char *) mapaddr)[4]));
#endif

    wmct.body = wmct.length + 4;
    wmct.bodysize = mapsize - 4;

    if (msglen > wmct.bodysize) {
        /* Incoming length field is too large. Emit a failure response
         * without even trying to handle the request.
         *
         * (We know this must fit, because we checked mapsize >= 5
         * above.) */
        PUT_32BIT_MSB_FIRST(wmct.length, 1);
        *wmct.body = SSH_AGENT_FAILURE;
    } else {
        wmct.bodylen = msglen;
#ifdef WIN32S_COMPAT
        /* No threads on Win32s: handle the request synchronously.
         * The window receives WM_COPYDATA in its own message; the
         * reply is written straight into the file mapping below. */
        wm_copydata_got_msg(NULL);
#else
        SetEvent(wmct.ev_msg_ready);
        WaitForSingleObject(wmct.ev_reply_ready, INFINITE);
#endif
    }

  cleanup:
    /* expectedsid has the lifetime of the program, so we don't free it */
    sfree(expectedsid_bc);
    if (psd)
        LocalFree(psd);
    if (mapaddr)
        UnmapViewOfFile(mapaddr);
    if (maphandle != NULL && maphandle != INVALID_HANDLE_VALUE)
        CloseHandle(maphandle);
    return err;
}

static void create_keylist_window(void)
{
    if (keylist)
        return;

    keylist = CreateDialog(hinst, MAKEINTRESOURCE(IDD_KEYLIST),
                           NULL, KeyListProc);
    ShowWindow(keylist, SW_SHOWNORMAL);
}

static LRESULT CALLBACK TrayWndProc(HWND hwnd, UINT message,
                                    WPARAM wParam, LPARAM lParam)
{
#ifdef NIM_ADD
    static bool menuinprogress;
    static UINT msgTaskbarCreated = 0;
#endif

    switch (message) {
#ifdef NIM_ADD
      case WM_CREATE:
        msgTaskbarCreated = RegisterWindowMessage(_T("TaskbarCreated"));
        break;
      default:
        if (message == msgTaskbarCreated && tray_icon) {
            /*
             * Explorer has been restarted, so the tray icon will
             * have been lost.
             */
            AddTrayIcon(hwnd);
        }
        break;

      case WM_SYSTRAY:
        if (lParam == WM_RBUTTONUP) {
            POINT cursorpos;
            GetCursorPos(&cursorpos);
            PostMessage(hwnd, WM_SYSTRAY2, cursorpos.x, cursorpos.y);
        } else if (lParam == WM_LBUTTONDBLCLK) {
            /* Run the default menu item. */
            UINT menuitem = GetMenuDefaultItem(systray_menu, false, 0);
            if (menuitem != -1)
                PostMessage(hwnd, WM_COMMAND, menuitem, 0);
        }
        break;
      case WM_SYSTRAY2:
        if (!menuinprogress) {
            menuinprogress = true;
            update_sessions();
            SetForegroundWindow(hwnd);
            TrackPopupMenu(systray_menu,
                           TPM_RIGHTALIGN | TPM_BOTTOMALIGN |
                           TPM_RIGHTBUTTON,
                           wParam, lParam, 0, hwnd, NULL);
            menuinprogress = false;
        }
        break;
#endif
      case WM_INITMENU:
        /* The system menu of the minimised window is about to appear */
        if (!tray_icon)
            update_sessions();
        break;
      case WM_QUERYOPEN:
        /* Double-clicking the minimised window shows the key list,
         * as double-clicking the tray icon does; the window stays
         * minimised. */
        PostMessage(hwnd, WM_COMMAND, IDM_VIEWKEYS, 0);
        return 0;
      case WM_COMMAND:
      case WM_SYSCOMMAND: {
        unsigned command = wParam & ~0xF; /* low 4 bits reserved to Windows */
        switch (command) {
          case IDM_PUTTY: {
            TCHAR cmdline[10];
            cmdline[0] = '\0';
            if (restrict_putty_acl)
                strcat(cmdline, "&R");

            if ((int)ShellExecute(hwnd, NULL, putty_path, cmdline,
                                  _T(""), SW_SHOW) <= 32) {
                MessageBox(NULL, "Unable to execute PuTTY!",
                           "Error", MB_OK | MB_ICONERROR);
            }
            break;
          }
          case IDM_CLOSE:
            if (modal_passphrase_hwnd)
                SendMessage(modal_passphrase_hwnd, WM_CLOSE, 0, 0);
            SendMessage(hwnd, WM_CLOSE, 0, 0);
            break;
          case IDM_VIEWKEYS:
            create_keylist_window();
            /*
             * Sometimes the window comes up minimised / hidden for
             * no obvious reason. Prevent this. This also brings it
             * to the front if it's already present (the user
             * selected View Keys because they wanted to _see_ the
             * thing).
             */
            SetForegroundWindow(keylist);
            SetWindowPos(keylist, HWND_TOP, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
            break;
          case IDM_ADDKEY:
          case IDM_ADDKEY_ENCRYPTED:
            if (modal_passphrase_hwnd) {
                MessageBeep(MB_ICONERROR);
                SetForegroundWindow(modal_passphrase_hwnd);
                break;
            }
            prompt_add_keyfile(command == IDM_ADDKEY_ENCRYPTED);
            break;
          case IDM_REMOVE_ALL:
            pageant_delete_all();
            keylist_update();
            break;
          case IDM_REENCRYPT_ALL:
            pageant_reencrypt_all();
            keylist_update();
            break;
          case IDM_ABOUT:
            if (!aboutbox) {
                aboutbox = CreateDialog(hinst, MAKEINTRESOURCE(IDD_ABOUT),
                                        NULL, AboutProc);
                ShowWindow(aboutbox, SW_SHOWNORMAL);
                /*
                 * Sometimes the window comes up minimised / hidden
                 * for no obvious reason. Prevent this.
                 */
                SetForegroundWindow(aboutbox);
                SetWindowPos(aboutbox, HWND_TOP, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
            }
            break;
          default: {
            if (command >= IDM_SESSIONS_BASE && command <= IDM_SESSIONS_MAX) {
                char buf[MAX_PATH + 1];
                char *param;
                GetMenuString(session_menu, command, buf, sizeof(buf),
                              MF_BYCOMMAND);
                param = dupprintf("%s@%s", restrict_putty_acl ? "&R" : "",
                                  buf);
                if ((int)ShellExecute(hwnd, NULL, putty_path, param,
                                      _T(""), SW_SHOW) <= 32) {
                    MessageBox(NULL, "Unable to execute PuTTY!", "Error",
                               MB_OK | MB_ICONERROR);
                }
                sfree(param);
            }
            break;
          }
        }
        break;
      }
      case WM_NETEVENT:
      case WM_DONE_WITH_SOCKET:
        winselgui_response(message, wParam, lParam);
        return 0;
      case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProc(hwnd, message, wParam, lParam);
}

static LRESULT CALLBACK wm_copydata_WndProc(HWND hwnd, UINT message,
                                            WPARAM wParam, LPARAM lParam)
{
    switch (message) {
      case WM_COPYDATA: {
        COPYDATASTRUCT *cds;
        char *mapname, *err;

        cds = (COPYDATASTRUCT *) lParam;
        if (cds->dwData != AGENT_COPYDATA_ID)
            return 0;              /* not our message, mate */
        mapname = (char *) cds->lpData;
        if (mapname[cds->cbData - 1] != '\0')
            return 0;              /* failure to be ASCIZ! */
        err = answer_filemapping_message(mapname);
        if (err) {
#ifdef DEBUG_IPC
            debug(("IPC failed: %s\n", err));
#endif
            sfree(err);
            return 0;
        }
        return 1;
      }
    }

    return DefWindowProc(hwnd, message, wParam, lParam);
}

#ifndef WIN32S_COMPAT
static DWORD WINAPI wm_copydata_threadfunc(void *param)
{
    HINSTANCE inst = *(HINSTANCE *)param;

    HWND ipchwnd = CreateWindow(IPCCLASSNAME, IPCWINTITLE,
                                WS_OVERLAPPEDWINDOW | WS_VSCROLL,
                                CW_USEDEFAULT, CW_USEDEFAULT,
                                100, 100, NULL, NULL, inst, NULL);
    MSG msg;
    ShowWindow(ipchwnd, SW_HIDE);

    while (GetMessage(&msg, NULL, 0, 0) == 1) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return 0;
}
#endif /* !WIN32S_COMPAT */

/*
 * Fork and Exec the command in cmdline. [DBW]
 */
void spawn_cmd(const char *cmdline, const char *args, int show)
{
    if (ShellExecute(NULL, _T("open"), cmdline,
                     args, NULL, show) <= (HINSTANCE) 32) {
        char *msg;
        msg = dupprintf("Failed to run \"%s\": %s", cmdline,
                        win_strerror(GetLastError()));
        MessageBox(NULL, msg, APPNAME, MB_OK | MB_ICONEXCLAMATION);
        sfree(msg);
    }
}

void noise_ultralight(NoiseSourceId id, unsigned long data)
{
    /* Pageant doesn't use random numbers, so we ignore this */
}

void cleanup_exit(int code)
{
    exit(code);
}

static bool winpgnt_listener_ask_passphrase(
    PageantListenerClient *plc, PageantClientDialogId *dlgid,
    const char *comment)
{
    return ask_passphrase_common(dlgid, comment);
}

struct winpgnt_client {
    PageantListenerClient plc;
};
static const PageantListenerClientVtable winpgnt_vtable = {
    /* .log = */ NULL, /* no logging */
    /* .ask_passphrase = */ winpgnt_listener_ask_passphrase,
};

static struct winpgnt_client wpc[1];

HINSTANCE hinst;

static NORETURN void opt_error(const char *fmt, ...)
{
    va_list ap;
    char *msg;
    va_start(ap, fmt);
    msg = dupvprintf(fmt, ap);
    va_end(ap);

    MessageBox(NULL, msg, "Pageant command line error", MB_ICONERROR | MB_OK);

    exit(1);
}

BOOL sw_PeekMessage(LPMSG msg, HWND hwnd, UINT min, UINT max, UINT remove)
{
    static bool unicode_unavailable = false;
    if (!unicode_unavailable) {
        BOOL ret = PeekMessageW(msg, hwnd, min, max, remove);
        if (!ret && GetLastError() == ERROR_CALL_NOT_IMPLEMENTED)
            unicode_unavailable = true; /* don't try again */
        else
            return ret;
    }
    return PeekMessageA(msg, hwnd, min, max, remove);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmdline, int show)
{
    MSG msg;
    const char *command = NULL;
    bool show_keylist_on_startup = false;
    Filename *openssh_config_file = NULL;

    typedef struct CommandLineKey {
        Filename *fn;
        bool add_encrypted;
    } CommandLineKey;

    CommandLineKey *clkeys = NULL;
    size_t nclkeys = 0, clkeysize = 0;
    bool add_keys_encrypted;
    AuxMatchOpt amo;
    HANDLE mutex;
    size_t i;

    dll_hijacking_protection();
    enable_dit();

    hinst = inst;

    /*
     * Look for the PuTTY binary (we will enable the saved session
     * submenu if we find it).
     */
    {
        char b[2048], *p, *q, *r;
        FILE *fp;
        GetModuleFileName(NULL, b, sizeof(b) - 16);
        r = b;
        p = strrchr(b, '\\');
        if (p && p >= r) r = p+1;
        q = strrchr(b, ':');
        if (q && q >= r) r = q+1;
        strcpy(r, "putty.exe");
        if ( (fp = fopen(b, "r")) != NULL) {
            putty_path = dupstr(b);
            fclose(fp);
        } else
            putty_path = NULL;
    }

    /*
     * Process the command line, handling anything that can be done
     * immediately, but deferring adding keys until after we've
     * started up the main agent. Details of keys to be added are
     * stored in the 'clkeys' array.
     */
    add_keys_encrypted = false;
    amo = aux_match_opt_init(opt_error);
    while (!aux_match_done(&amo)) {
        CmdlineArg *valarg;
        if (aux_match_arg(&amo, &valarg)) {
            CommandLineKey *clkey;
            /*
             * Non-option arguments are expected to be key files, and
             * added to clkeys.
             */
            sgrowarray(clkeys, clkeysize, nclkeys);
            clkey = &clkeys[nclkeys++];
            clkey->fn = cmdline_arg_to_filename(valarg);
            clkey->add_encrypted = add_keys_encrypted;
        } else if (aux_match_opt(&amo, NULL, "-pgpfp", NULL)) {
            pgp_fingerprints_msgbox(NULL);
            return 0;
        } else if (aux_match_opt(&amo, NULL, "-restrict-acl", "-restrict_acl",
                                 "-restrictacl", NULL)) {
            restrict_process_acl();
        } else if (aux_match_opt(&amo, NULL, "-restrict-putty-acl", "-restrict_putty_acl", NULL)) {
            restrict_putty_acl = true;
        } else if (aux_match_opt(&amo, NULL, "-no-decrypt", "-no_decrypt",
                                 "-nodecrypt", "-encrypted", NULL)) {
            add_keys_encrypted = true;
        } else if (aux_match_opt(&amo, NULL, "-keylist", NULL)) {
            show_keylist_on_startup = true;
        } else if (aux_match_opt(&amo, &valarg, "-openssh-config", "-openssh_config", NULL)) {
            openssh_config_file = cmdline_arg_to_filename(valarg);
        } else if (aux_match_opt(&amo, NULL, "-c", NULL)) {
            /*
             * If we see `-c', then the rest of the command line
             * should be treated as a command to be spawned.
             */
            if (amo.arglist->args[amo.index]) {
                /* UNICODE: should use the UTF-8 or wide version, and
                 * CreateProcessW, to pass through arbitrary command lines */
                command = cmdline_arg_remainder_acp(
                    amo.arglist->args[amo.index]);
            } else {
                command = "";
            }
            break;
        } else {
            opt_error("unrecognised option '%s'\n",
                      cmdline_arg_to_str(amo.arglist->args[amo.index]));
        }
    }

    /*
     * Create and lock an interprocess mutex while we figure out
     * whether we're going to be the Pageant server or a client. That
     * way, two Pageant processes started up simultaneously will be
     * able to agree on which one becomes the server without a race
     * condition.
     */
    {
        char *err;
        char *mutexname = agent_mutex_name();
        mutex = lock_interprocess_mutex(mutexname, &err);
        sfree(mutexname);
        if (!mutex) {
            MessageBox(NULL, err, "Pageant Error", MB_ICONERROR | MB_OK);
            return 1;
        }
    }

    /*
     * Find out if Pageant is already running.
     */
    already_running = agent_exists();

    /*
     * If it isn't, we're going to be the primary Pageant that stays
     * running, so set up all the machinery to answer requests.
     */
    if (!already_running) {
        DWORD wm_copydata_threadid;
        HANDLE hThread;
        /*
         * Set up the window class for the hidden window that receives
         * all the messages to do with our presence in the system tray.
         */

        if (!prev) {
            WNDCLASS wndclass;

            memset(&wndclass, 0, sizeof(wndclass));
            wndclass.lpfnWndProc = TrayWndProc;
            wndclass.hInstance = inst;
            wndclass.hIcon = LoadIcon(inst, MAKEINTRESOURCE(IDI_MAINICON));
            wndclass.lpszClassName = TRAYCLASSNAME;

            RegisterClass(&wndclass);
        }

        keylist = NULL;

        traywindow = CreateWindow(TRAYCLASSNAME, TRAYWINTITLE,
                                  WS_OVERLAPPEDWINDOW | WS_VSCROLL,
                                  CW_USEDEFAULT, CW_USEDEFAULT,
                                  100, 100, NULL, NULL, inst, NULL);
        winselgui_set_hwnd(traywindow);

        /*
         * Initialise the cross-platform Pageant code.
         */
        pageant_init();

        /*
         * Set up a named-pipe listener.
         */
        wpc->plc.vt = &winpgnt_vtable;
        wpc->plc.suppress_logging = true;
        {
            Plug *pl_plug;
            struct pageant_listen_state *pl =
                pageant_listener_new(&pl_plug, &wpc->plc);
            char *pipename = agent_named_pipe_name();
            Socket *sock = new_named_pipe_listener(pipename, pl_plug);
            if (sk_socket_error(sock)) {
                char *err = dupprintf("Unable to open named pipe at %s "
                                      "for SSH agent:\n%s", pipename,
                                      sk_socket_error(sock));
                MessageBox(NULL, err, "Pageant Error", MB_ICONERROR | MB_OK);
                return 1;
            }
            pageant_listener_got_socket(pl, sock);

            /*
             * If we've been asked to write out an OpenSSH config file
             * pointing at the named pipe, do so.
             */
            if (openssh_config_file) {
                FILE *fp = f_open(openssh_config_file, "w", true);
                const char *p;
                if (!fp) {
                    char *err = dupprintf(
                        "Unable to write OpenSSH config file to %s",
                        filename_to_str(openssh_config_file));
                    MessageBox(NULL, err, "Pageant Error",
                               MB_ICONERROR | MB_OK);
                    return 1;
                }
                fputs("IdentityAgent \"", fp);
                /* Some versions of Windows OpenSSH prefer / to \ as the path
                 * separator; others don't mind, but as far as we know, no
                 * version _objects_ to /, so we use it unconditionally. */
                for (p = pipename; *p; p++) {
                    char c = *p;
                    if (c == '\\')
                        c = '/';
                    fputc(c, fp);
                }
                fputs("\"\n", fp);
                fclose(fp);
            }

            sfree(pipename);
        }

        /*
         * Set up the window class for the hidden window that receives
         * the WM_COPYDATA message used by the old-style Pageant IPC
         * system.
         */
        if (!prev) {
            WNDCLASS wndclass;

            memset(&wndclass, 0, sizeof(wndclass));
            wndclass.lpfnWndProc = wm_copydata_WndProc;
            wndclass.hInstance = inst;
            wndclass.lpszClassName = IPCCLASSNAME;

            RegisterClass(&wndclass);
        }

        /*
         * And launch the subthread which will open that hidden window and
         * handle WM_COPYDATA messages on it.
         */
        wmcpc.vt = &wmcpc_vtable;
        wmcpc.suppress_logging = true;
        pageant_register_client(&wmcpc);
#ifdef WIN32S_COMPAT
        /*
         * No threads on Win32s. Create the hidden WM_COPYDATA
         * window in this thread; messages arrive through the same
         * PeekMessage/DispatchMessage loop as everything else.
         */
        {
            HWND ipchwnd = CreateWindow(IPCCLASSNAME, IPCWINTITLE,
                                        WS_OVERLAPPEDWINDOW | WS_VSCROLL,
                                        CW_USEDEFAULT, CW_USEDEFAULT,
                                        100, 100, NULL, NULL, inst, NULL);
            if (ipchwnd)
                ShowWindow(ipchwnd, SW_HIDE);
        }
#else
        wmct.ev_msg_ready = CreateEvent(NULL, false, false, NULL);
        wmct.ev_reply_ready = CreateEvent(NULL, false, false, NULL);
        hThread = CreateThread(NULL, 0, wm_copydata_threadfunc,
                               &inst, 0, &wm_copydata_threadid);
        if (hThread)
            CloseHandle(hThread); /* we don't need the thread handle */
        add_handle_wait(wmct.ev_msg_ready, wm_copydata_got_msg, NULL);
#endif /* WIN32S_COMPAT */
    }

    /*
     * Now we're either a fully set up Pageant server, or we know one
     * is running somewhere else. Either way, now it's safe to unlock
     * the mutex.
     */
    unlock_interprocess_mutex(mutex);

    /*
     * Add any keys provided on the command line.
     */
    for (i = 0; i < nclkeys; i++) {
        CommandLineKey *clkey = &clkeys[i];
        win_add_keyfile(clkey->fn, clkey->add_encrypted);
        filename_free(clkey->fn);
    }
    sfree(clkeys);
    /* And forget any passphrases we stashed during that loop. */
    pageant_forget_passphrases();

    /*
     * Now our keys are present, spawn a command, if we were asked to.
     */
    if (command) {
        char *args;
        if (command[0] == '"')
            args = strchr(++command, '"');
        else
            args = strchr(command, ' ');
        if (args) {
            *args++ = 0;
            while (*args && isspace((unsigned char)*args)) args++;
        }
        spawn_cmd(command, args, show);
    }

    /*
     * If Pageant was already running, we leave now. If we haven't
     * even taken any auxiliary action (spawned a command or added
     * keys), complain.
     */
    if (already_running) {
        if (!command && !nclkeys) {
            MessageBox(NULL, "Pageant is already running", "Pageant Error",
                       MB_ICONERROR | MB_OK);
        }
        return 0;
    }

#ifdef NIM_ADD
    /* Set up a system tray icon */
    tray_icon = AddTrayIcon(traywindow) != 0;
#endif

    /* Accelerators used: nsvkxa */
    if (tray_icon) {
        systray_menu = CreatePopupMenu();
    } else {
        /* The system menu becomes Move, then the tray icon's menu */
        int pos;
        systray_menu = GetSystemMenu(traywindow, false);
        for (pos = GetMenuItemCount(systray_menu) - 1; pos >= 0; pos--)
            if (GetMenuItemID(systray_menu, pos) != SC_MOVE)
                DeleteMenu(systray_menu, pos, MF_BYPOSITION);
        AppendMenu(systray_menu, MF_SEPARATOR, 0, 0);
    }
    if (putty_path) {
        session_menu = CreateMenu();
        AppendMenu(systray_menu, MF_ENABLED, IDM_PUTTY, "&New Session");
        AppendMenu(systray_menu, MF_POPUP | MF_ENABLED,
                   (UINT)session_menu, "&Saved Sessions");
        AppendMenu(systray_menu, MF_SEPARATOR, 0, 0);
    }
    AppendMenu(systray_menu, MF_ENABLED, IDM_VIEWKEYS,
               "&View Keys");
    AppendMenu(systray_menu, MF_ENABLED, IDM_ADDKEY, "Add &Key");
    AppendMenu(systray_menu, MF_ENABLED, IDM_ADDKEY_ENCRYPTED,
               "Add key (encrypted)");
    AppendMenu(systray_menu, MF_SEPARATOR, 0, 0);
    AppendMenu(systray_menu, MF_ENABLED, IDM_REMOVE_ALL,
               "Remove All Keys");
    AppendMenu(systray_menu, MF_ENABLED, IDM_REENCRYPT_ALL,
               "Re-encrypt All Keys");
    AppendMenu(systray_menu, MF_SEPARATOR, 0, 0);
    AppendMenu(systray_menu, MF_ENABLED, IDM_ABOUT, "&About");
    AppendMenu(systray_menu, MF_SEPARATOR, 0, 0);
    AppendMenu(systray_menu, MF_ENABLED, IDM_CLOSE, "E&xit");
    initial_menuitems_count = GetMenuItemCount(session_menu);

    if (tray_icon) {
#ifdef NIM_ADD
        /* Set the default menu item. */
        SetMenuDefaultItem(systray_menu, IDM_VIEWKEYS, false);
#endif
        ShowWindow(traywindow, SW_HIDE);
    } else {
        ShowWindow(traywindow, SW_SHOWMINNOACTIVE);
    }

    /* Open the visible key list window, if we've been asked to. */
    if (show_keylist_on_startup)
        create_keylist_window();

    /*
     * Main message loop.
     */
    while (true) {
        int n;

#ifdef WIN32S_COMPAT
        /*
         * No MsgWaitForMultipleObjects on Win32s, and no threads
         * either, so there are no handles to wait for. Just wait for
         * a message or a timer.
         */
        if (!toplevel_callback_pending() &&
            !sw_PeekMessage(&msg, NULL, 0, 0, PM_NOREMOVE)) {
            WaitMessage();
        }
#else
        HandleWaitList *hwl = get_handle_wait_list();

        DWORD timeout = toplevel_callback_pending() ? 0 : INFINITE;
        n = MsgWaitForMultipleObjects(hwl->nhandles, hwl->handles, false,
                                      timeout, QS_ALLINPUT);

        if ((unsigned)(n - WAIT_OBJECT_0) < (unsigned)hwl->nhandles)
            handle_wait_activate(hwl, n - WAIT_OBJECT_0);
        handle_wait_list_free(hwl);
#endif /* WIN32S_COMPAT */

        while (sw_PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT)
                goto finished;         /* two-level break */

            if (IsWindow(keylist) && IsDialogMessage(keylist, &msg))
                continue;
            if (IsWindow(aboutbox) && IsDialogMessage(aboutbox, &msg))
                continue;
            if (IsWindow(nonmodal_passphrase_hwnd) &&
                IsDialogMessage(nonmodal_passphrase_hwnd, &msg))
                continue;

            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        run_toplevel_callbacks();
    }
  finished:

#ifdef NIM_ADD
    /* Clean up the system tray icon */
    if (tray_icon) {
        NOTIFYICONDATA tnid;

        tnid.cbSize = sizeof(NOTIFYICONDATA);
        tnid.hWnd = traywindow;
        tnid.uID = 1;

        Shell_NotifyIcon(NIM_DELETE, &tnid);

        DestroyMenu(systray_menu);
    }
#endif

    if (keypath)
        filereq_saved_dir_free(keypath);

    if (openssh_config_file) {
        /*
         * Leave this file around, but empty it, so that it doesn't
         * refer to a pipe we aren't listening on any more.
         */
        FILE *fp = f_open(openssh_config_file, "w", true);
        if (fp)
            fclose(fp);
    }

    cleanup_exit(msg.wParam);
    return msg.wParam;                 /* just in case optimiser complains */
}
