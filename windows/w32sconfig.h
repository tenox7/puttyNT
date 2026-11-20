/*
 * w32sconfig.h: build settings for the Win32s port.
 *
 * Win32s is the 32-bit Windows subsystem that ran on Windows 3.1 and
 * Windows for Workgroups 3.11. It is a different operating system
 * from Windows NT, and it has a much smaller API: the list of what it
 * supports is in WIN32S.DAT, which ships with the Win32s SDK.
 *
 * This file answers the same questions as ntconfig.h does for the NT
 * port, but for Win32s. Which one gets used is decided by the
 * WIN32S_COMPAT define, which the Makefile sets when building the
 * Win32s flavour of the programs.
 */

#ifndef PUTTY_W32SCONFIG_H
#define PUTTY_W32SCONFIG_H

#define NO_IPV6                 /* WinSock 1.1 only */
#define NO_GSSAPI               /* no SSPI */
#define NO_MULTIMON             /* MonitorFromWindow: Windows 98/2000 */
#define NO_MANIFESTS            /* manifests came with Windows XP */
#define NO_HTMLHELP             /* no HTML Help (.chm) on Win32s */

#define HAVE_COUNTOF 0

#define HAVE_WINRESRC_H 0
#define HAVE_WINRES_H 0
#define HAVE_WIN_H 0
#define HAVE_AFUNIX_H 0
#define HAVE_ADDDLLDIRECTORY 0
#define HAVE_GETNAMEDPIPECLIENTPROCESSID 0
#define HAVE_SETDEFAULTDLLDIRECTORIES 0
#define HAVE_DWMAPI_H 0

/* MinGW (used for test builds on other machines) has GCP_RESULTSW */
#if defined __GNUC__
#define HAVE_GCP_RESULTSW 1
#else
#define HAVE_GCP_RESULTSW 0
#endif

#define HAVE_AES_NI 0
#define HAVE_SHA_NI 0
#define HAVE_SHAINTRIN_H 0
#define HAVE_CLMUL 0
#define HAVE_NEON_CRYPTO 0
#define HAVE_NEON_PMULL 0
#define HAVE_NEON_VADDQ_P128 0
#define HAVE_NEON_SHA512 0
#define HAVE_NEON_SHA512_INTRINSICS 0
#define HAVE_ARM64_NEON_H 0
#define HAVE_ARM_DIT 0

#endif /* PUTTY_W32SCONFIG_H */
