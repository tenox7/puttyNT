/*
 * Implementation of do_select() for network.c to use, that uses
 * WSAAsyncSelect to convert network activity into window messages,
 * for integration into a GUI event loop.
 */

#include "putty.h"
#include "tree234.h"

static void wm_netevent_callback(void *vctx);

static HWND winsel_hwnd = NULL;
static tree234 *moribund_sockets = NULL;
static tree234 *cli_sockets = NULL;
static int socket_cmp(void *av, void *bv)
{
    uintptr_t a = (uintptr_t)av, b = (uintptr_t)bv;
    return a < b ? -1 : a > b ? +1 : 0;
}

/* Return the one live socket, for front ends that drive the network
 * with WinSock's select() directly (winsftp.exe). INVALID_SOCKET when
 * none, or more than one. */
SOCKET winselcli_unique_socket(void)
{
    uintptr_t sk;
    if (!cli_sockets)
        return INVALID_SOCKET;
    assert(count234(cli_sockets) <= 1);
    sk = (uintptr_t)index234(cli_sockets, 0);
    return sk ? (SOCKET)sk : INVALID_SOCKET;
}

static int moribund_socket_cmp(void *av, void *bv)
{
    uintptr_t a = (uintptr_t)av, b = (uintptr_t)bv;
    return a < b ? -1 : a > b ? +1 : 0;
}

void winselgui_set_hwnd(HWND hwnd)
{
    winsel_hwnd = hwnd;
}

void winselgui_clear_hwnd(void)
{
    winsel_hwnd = NULL;
}

const char *do_select(SOCKET skt, bool enable)
{
    int msg, events;

    if (enable) {
        if (!cli_sockets)
            cli_sockets = newtree234(socket_cmp);
        add234(cli_sockets, (void *)(uintptr_t)skt);
    } else {
        if (cli_sockets)
            del234(cli_sockets, (void *)(uintptr_t)skt);
    }

    if (!winsel_hwnd) {
        /*
         * A console tool without a window (see select-cli.c), which
         * looks at its sockets with sk_poll. They must not block, as
         * WSAAsyncSelect would have made sure.
         */
        u_long nonblocking = 1;
        if (enable &&
            p_ioctlsocket(skt, FIONBIO, &nonblocking) == SOCKET_ERROR)
            return winsock_error_string(p_WSAGetLastError());
        return NULL;
    }

    if (enable) {
        msg = WM_NETEVENT;
        events = (FD_CONNECT | FD_READ | FD_WRITE |
                  FD_OOB | FD_CLOSE | FD_ACCEPT);
    } else {
        msg = events = 0;
    }

    if (p_WSAAsyncSelect(skt, winsel_hwnd, msg, events) == SOCKET_ERROR)
        return winsock_error_string(p_WSAGetLastError());

    return NULL;
}

struct wm_netevent_params {
    /* Used to pass data to wm_netevent_callback */
    WPARAM wParam;
    LPARAM lParam;
};

static bool callback_is_for_socket(
    void *predicate_ctx, toplevel_callback_fn_t fn, void *callback_ctx)
{
    struct wm_netevent_params *params;
    if (fn != wm_netevent_callback)
        return false;
    params = (struct wm_netevent_params *)callback_ctx;
    if (params->wParam != (WPARAM)(uintptr_t)predicate_ctx)
        return false;

    /* The 'struct wm_netevent_params' would have been freed by the
     * callback function wm_netevent_callback(). Now that isn't going
     * to run, so we must free it ourself. */
    sfree(callback_ctx);
    return true;
}

void done_with_socket(SOCKET skt)
{
    if (!winsel_hwnd)
        return;                        /* no messages to wait for */
    if (!moribund_sockets)
        moribund_sockets = newtree234(moribund_socket_cmp);
    PostMessage(winsel_hwnd, WM_DONE_WITH_SOCKET, (WPARAM)skt, 0);
    add234(moribund_sockets, (void *)skt);
    delete_callbacks(callback_is_for_socket, (void *)(uintptr_t)skt);
}

static void wm_netevent_callback(void *vctx)
{
    struct wm_netevent_params *params = (struct wm_netevent_params *)vctx;
    select_result(params->wParam, params->lParam);
    sfree(params);
}

void winselgui_response(UINT message, WPARAM wParam, LPARAM lParam)
{
    struct wm_netevent_params *params;
    if (message == WM_DONE_WITH_SOCKET) {
        del234(moribund_sockets, (void *)wParam);
        return;
    } else if (moribund_sockets &&
               find234(moribund_sockets, (void *)wParam, NULL)) {
        return;
    }

    /*
     * To protect against re-entrancy when Windows's recv()
     * immediately triggers a new WSAAsyncSelect window message, we
     * don't call select_result directly from this handler but instead
     * wait until we're back out at the top level of the message loop.
     */
    params = snew(struct wm_netevent_params);
    params->wParam = wParam;
    params->lParam = lParam;
    queue_toplevel_callback(wm_netevent_callback, params);
}
