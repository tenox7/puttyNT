/*
 * defs.h: initial definitions for PuTTY.
 *
 * The rule about this header file is that it can't depend on any
 * other header file in this code base. This is where we define
 * things, as much as we can, that other headers will want to refer
 * to, such as opaque structure types and their associated typedefs,
 * or macros that are used by other headers.
 */

#ifndef PUTTY_DEFS_H
#define PUTTY_DEFS_H

#ifdef NDEBUG
/*
 * PuTTY is a security project, so assertions are important - if an
 * assumption is violated, proceeding anyway may have far worse
 * consequences than simple program termination. This check and #error
 * should arrange that we don't ever accidentally compile assertions
 * out.
 */
#error Do not compile this code base with NDEBUG defined!
#endif

/*
 * Which platform are we building for? The NT port and the Win32s
 * port are separate operating systems with different APIs, so each
 * has its own answers to the questions in a config header. The
 * Makefile defines WIN32S_COMPAT when building the Win32s flavour.
 */
#if defined(WIN32S_COMPAT)
#include "w32sconfig.h"
#else
#include "ntconfig.h"
#endif

#include <stddef.h>
#include <stdio.h>                     /* for __MINGW_PRINTF_FORMAT */
#include <limits.h>

/*
 * The compilers for NT 3.1 to 4.0 (Microsoft C 8.00 to Visual C++
 * 4.2) are C89 compilers: they have no stdint.h, inttypes.h or
 * stdbool.h, and no inline. So what PuTTY uses from those is defined
 * here. int, long and pointers are 32 bits on every NT CPU (x86,
 * MIPS, Alpha and PowerPC), and so is size_t.
 */
typedef signed char int8_t;
typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef unsigned long uintmax_t;
#ifndef _UINTPTR_T_DEFINED
#define _UINTPTR_T_DEFINED
typedef unsigned int uintptr_t;
#endif

/*
 * A 64-bit unsigned integer, held as two 32-bit halves, because the
 * NT 3.1 compilers have no 64-bit integer type. misc.h has the
 * functions that do arithmetic on it.
 */
typedef struct uint64 { uint32_t hi, lo; } uint64;

#define UINT32_MAX 0xFFFFFFFFU
#ifndef SIZE_MAX
#define SIZE_MAX UINT_MAX
#endif

#define PRIu32 "u"
#define PRIx32 "x"
#define SIZEu "u"                      /* printf formats for size_t */
#define SIZEx "x"

/*
 * A bool is an int. Every conversion to bool in this code base is
 * written out ("!= 0" and so on), so a bool holds only 0 or 1, as a
 * C99 _Bool would.
 */
typedef int bool;
#define true 1
#define false 0

#ifdef _MSC_VER
#define inline __inline
#endif

#if defined __GNUC__ || defined __clang__
/*
 * On MinGW, the correct compiler format checking for vsnprintf() etc
 * can depend on compile-time flags; these control whether you get
 * ISO C or Microsoft's non-standard format strings.
 * We sometimes use __attribute__ ((format)) for our own printf-like
 * functions, which are ultimately interpreted by the toolchain-chosen
 * printf, so we need to take that into account to get correct warnings.
 */
#ifdef __MINGW_PRINTF_FORMAT
#define PRINTF_LIKE(fmt_index, ellipsis_index) \
    __attribute__ ((format (__MINGW_PRINTF_FORMAT, fmt_index, ellipsis_index)))
#else
#define PRINTF_LIKE(fmt_index, ellipsis_index) \
    __attribute__ ((format (printf, fmt_index, ellipsis_index)))
#endif
#else /* __GNUC__ */
#define PRINTF_LIKE(fmt_index, ellipsis_index)
#endif /* __GNUC__ */

typedef struct conf_tag Conf;
typedef struct ConfKeyInfo ConfKeyInfo;
typedef struct ConfSaveEnumValue ConfSaveEnumValue;
typedef struct ConfSaveEnumType ConfSaveEnumType;
typedef struct CmdlineArgList CmdlineArgList;
typedef struct CmdlineArg CmdlineArg;

typedef struct terminal_tag Terminal;
typedef struct term_utf8_decode term_utf8_decode;

typedef struct Filename Filename;
typedef struct FontSpec FontSpec;

typedef struct bufchain_tag bufchain;

typedef struct strbuf strbuf;
typedef struct LoadedFile LoadedFile;

typedef struct RSAKey RSAKey;

typedef struct BinarySink BinarySink;
typedef struct BinarySource BinarySource;
typedef struct stdio_sink stdio_sink;
typedef struct bufchain_sink bufchain_sink;
typedef struct buffer_sink buffer_sink;
typedef struct handle_sink handle_sink;

typedef struct IdempotentCallback IdempotentCallback;

typedef struct SockAddr SockAddr;

typedef struct Socket Socket;
typedef struct Plug Plug;
typedef struct SocketEndpointInfo SocketEndpointInfo;
typedef struct DeferredSocketOpener DeferredSocketOpener;
typedef struct DeferredSocketOpenerVtable DeferredSocketOpenerVtable;

typedef struct Backend Backend;
typedef struct BackendVtable BackendVtable;
typedef struct Interactor Interactor;
typedef struct InteractorVtable InteractorVtable;
typedef struct InteractionReadySeat InteractionReadySeat;

typedef struct Ldisc_tag Ldisc;
typedef struct LogContext LogContext;
typedef struct LogPolicy LogPolicy;
typedef struct LogPolicyVtable LogPolicyVtable;

typedef struct TermLineEditor TermLineEditor;
typedef struct TermLineEditorCallbackReceiver TermLineEditorCallbackReceiver;
typedef struct TermLineEditorCallbackReceiverVtable
    TermLineEditorCallbackReceiverVtable;

typedef struct Seat Seat;
typedef struct SeatVtable SeatVtable;
typedef struct SeatDialogText SeatDialogText;
typedef struct SeatDialogTextItem SeatDialogTextItem;
typedef struct SeatDialogPromptDescriptions SeatDialogPromptDescriptions;
typedef struct SeatPromptResult SeatPromptResult;

typedef struct cmdline_get_passwd_input_state cmdline_get_passwd_input_state;

typedef struct TermWin TermWin;
typedef struct TermWinVtable TermWinVtable;

typedef struct Ssh Ssh;

typedef struct mp_int mp_int;
typedef struct MontyContext MontyContext;

typedef struct WeierstrassCurve WeierstrassCurve;
typedef struct WeierstrassPoint WeierstrassPoint;
typedef struct MontgomeryCurve MontgomeryCurve;
typedef struct MontgomeryPoint MontgomeryPoint;
typedef struct EdwardsCurve EdwardsCurve;
typedef struct EdwardsPoint EdwardsPoint;

typedef struct SshServerConfig SshServerConfig;
typedef struct SftpServer SftpServer;
typedef struct SftpServerVtable SftpServerVtable;

typedef struct Channel Channel;
typedef struct SshChannel SshChannel;
typedef struct mainchan mainchan;

typedef struct CertExprBuilder CertExprBuilder;

typedef struct ssh_sharing_state ssh_sharing_state;
typedef struct ssh_sharing_connstate ssh_sharing_connstate;
typedef struct share_channel share_channel;

typedef struct PortFwdManager PortFwdManager;
typedef struct PortFwdRecord PortFwdRecord;
typedef struct ConnectionLayer ConnectionLayer;

typedef struct prng prng;
typedef struct ssh_hashalg ssh_hashalg;
typedef struct ssh_hash ssh_hash;
typedef struct ssh_kex ssh_kex;
typedef struct ssh_kexes ssh_kexes;
typedef struct ssh_keyalg ssh_keyalg;
typedef struct ssh_key ssh_key;
typedef struct ssh_compressor ssh_compressor;
typedef struct ssh_decompressor ssh_decompressor;
typedef struct ssh_compression_alg ssh_compression_alg;
typedef struct ssh2_userkey ssh2_userkey;
typedef struct ssh2_macalg ssh2_macalg;
typedef struct ssh2_mac ssh2_mac;
typedef struct ssh_cipheralg ssh_cipheralg;
typedef struct ssh_cipher ssh_cipher;
typedef struct ssh2_ciphers ssh2_ciphers;
typedef struct dh_ctx dh_ctx;
typedef struct ecdh_key ecdh_key;
typedef struct ecdh_keyalg ecdh_keyalg;
typedef struct pq_kemalg pq_kemalg;
typedef struct pq_kem_dk pq_kem_dk;
typedef struct NTRUKeyPair NTRUKeyPair;
typedef struct NTRUEncodeSchedule NTRUEncodeSchedule;
typedef struct RFC6979 RFC6979;
typedef struct RFC6979Result RFC6979Result;
typedef struct ShakeXOF ShakeXOF;

typedef struct dlgparam dlgparam;
typedef struct dlgcontrol dlgcontrol;

typedef struct settings_w settings_w;
typedef struct settings_r settings_r;
typedef struct settings_e settings_e;
typedef struct ca_options ca_options;
typedef struct host_ca host_ca;
typedef struct host_ca_enum host_ca_enum;

typedef struct SessionSpecial SessionSpecial;

typedef struct StripCtrlChars StripCtrlChars;

typedef struct BidiContext BidiContext;

typedef struct SubprocessWaiter SubprocessWaiter;

/*
 * A small structure wrapping up a (pointer, length) pair so that it
 * can be conveniently passed to or from a function.
 */
typedef struct ptrlen {
    const void *ptr;
    size_t len;
} ptrlen;

typedef struct logblank_t logblank_t;

typedef struct BinaryPacketProtocol BinaryPacketProtocol;
typedef struct PacketProtocolLayer PacketProtocolLayer;

struct unicode_data;

/* Do a compile-time type-check of 'to_check' (without evaluating it),
 * as a side effect of returning the value 'to_return'. Note that
 * although this macro double-*expands* to_return, it always
 * *evaluates* exactly one copy of it, so it's side-effect safe. */
#define TYPECHECK(to_check, to_return)                  \
    (sizeof(to_check) ? (to_return) : (to_return))

/* Return a pointer to the object of structure type 'type' whose field
 * with name 'field' is pointed at by 'object'. */
#define container_of(object, type, field)                               \
    TYPECHECK(object == &((type *)0)->field,                            \
              ((type *)(((char *)(object)) - offsetof(type, field))))

#if defined __GNUC__ || defined __clang__
#define NORETURN __attribute__((__noreturn__))
#else
#define NORETURN          /* Microsoft C has it only from Visual C++ 6 */
#endif

/*
 * Standard macro definitions. STR() behaves like the preprocessor
 * stringification # operator, and CAT() behaves like the token paste
 * ## operator, except that each one macro-expands its argument(s)
 * first, unlike the raw version. E.g.
 *
 *   #__LINE__               ->    "__LINE__"
 *   STR(__LINE__)           ->    "1234"             (or whatever)
 *
 * and similarly,
 *
 *   foo ## __LINE__         ->    foo__LINE__
 *   CAT(foo, __LINE__)      ->    foo1234            (or whatever)
 *
 * The expansion is achieved by having each macro pass its arguments
 * to a secondary inner macro, because parameter lists of a macro call
 * get expanded before the called macro is invoked. So STR(__LINE__)
 * -> STR_INNER(1234) -> #1234 -> "1234", and similarly for CAT.
 */
#define STR_INNER(x) #x
#define STR(x) STR_INNER(x)
#define CAT_INNER(x,y) x ## y
#define CAT(x,y) CAT_INNER(x,y)

/*
 * Structure shared between ssh.h and storage.h, giving strictness
 * options relating to checking of an OpenSSH certificate. It's a bit
 * cheaty to put something so specific in here, but more painful to
 * put it in putty.h.
 */
struct ca_options {
    bool permit_rsa_sha1, permit_rsa_sha256, permit_rsa_sha512;
};

/*
 * The two structures below belong with the Seat in putty.h. They are
 * here because headers such as ssh.h declare functions taking them
 * by value, and the Microsoft C compilers of 1993-1996 don't accept a
 * parameter of a struct type that hasn't been defined yet.
 */

/*
 * This structure type wraps a Seat pointer, in a way that has no
 * purpose except to be a different type.
 *
 * The Seat wrapper functions that present interactive prompts all
 * expect one of these in place of their ordinary Seat pointer. You
 * get one by calling interactor_announce (in putty.h), which will
 * print a message (if not already done) identifying the Interactor
 * that originated the prompt.
 *
 * This arranges that the C type system itself will check that no call
 * to any of those Seat methods has omitted the mandatory call to
 * interactor_announce beforehand.
 */
struct InteractionReadySeat {
    Seat *seat;
};

/*
 * The result of an interactive request from a backend to a Seat.
 */
/* Enum of result types in SeatPromptResult below */
typedef enum SeatPromptResultKind {
    /* Answer not yet available at all; either try again later or wait
     * for a callback (depending on the request's API) */
    SPRK_INCOMPLETE,

    /* We're abandoning the connection because the user interactively
     * told us to. (Hence, no need to present an error message
     * telling the user we're doing that: they already know.) */
    SPRK_USER_ABORT,

    /* We're abandoning the connection for some other reason (e.g. we
     * were unable to present the prompt at all, or a batch-mode
     * configuration told us to give the answer no). This may
     * ultimately have stemmed from some user configuration, but they
     * didn't _tell us right now_ to abandon this connection, so we
     * still need to inform them that we've done so. */
    SPRK_SW_ABORT,

    /* We're proceeding with the connection and have all requested
     * information (if any) */
    SPRK_OK
} SeatPromptResultKind;

/* Small struct to present the results of interactive requests from
 * backend to Seat (see below) */
struct SeatPromptResult {
    SeatPromptResultKind kind;

    /*
     * In the case of SPRK_SW_ABORT, the frontend provides an error
     * message to present to the user. But dynamically allocating it
     * up front would mean having to make sure it got freed at any
     * call site where one of these structs is received (and freed
     * _once_ no matter how many times the struct is copied). So
     * instead we provide a function that will generate the error
     * message into a BinarySink.
     */
    void (*errfn)(SeatPromptResult, BinarySink *);

    /*
     * And some fields the error function can use to construct the
     * message (holding, e.g. an OS error code).
     */
    const char *errdata_lit; /* statically allocated, e.g. a string literal */
    unsigned errdata_u;
};

#endif /* PUTTY_DEFS_H */
