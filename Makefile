# Makefile for PuTTY on Windows NT 3.1, 3.51 and 4.0: x86, MIPS, Alpha
# and PowerPC; and ARM, PINT 3.5 and 4.0. It works with the Win32 SDK
# for Windows NT 3.1, with Visual C++ 2.0, with Visual C++ 4.x and the
# Win32 SDK of 1996, and with the PINT SDKs.
#
# The same Makefile also builds the Win32s port, for the 32-bit Windows
# subsystem on Windows 3.1 and Windows for Workgroups 3.11. That needs
# the Visual C++ 1.0 for Win32s toolchain, whose own ntwin32.mak
# provides the same set of macros the SDK's one does, so the only
# extra thing needed is the WIN32S_COMPAT define:
#
#   nmake          builds the eight programs
#   nmake WIN32S=1 builds the eight programs for Win32s
#   nmake clean    deletes what nmake made
#
# The SDK's environment variable CPU (i386, MIPS, ALPHA, PPC or ARM)
# picks the processor. With the SDK of 1996, APPVER=3.51 marks the
# programs for Windows NT 3.51; without it they need NT 4.0.
#
# Object files don't depend on header files here: after changing a
# header file, run "nmake clean" first.

# Optimised, without debug information. Delete this line for a debug
# build.
NODEBUG = 1

!include <ntwin32.mak>

# The MIPS compilers (the SDK's for NT 3.1, and Visual C++ 4.x) make
# wrong code when they optimise functions that keep many unsigned
# short values at once, such as the AES S-box: a value is read back
# from a stack slot that was never written. NT 3.1's compiler does it
# with every -O option, Visual C++ 4.x with -Og. So MIPS builds are
# not optimised, apart from the few files that use COMPILE_OPT below.
!IF "$(CPU)" == "MIPS"
OPTIMISE = $(cdebug:-Ox=-Od)
!ELSE
OPTIMISE = $(cdebug)
!ENDIF

# Building for Win32s rather than for Windows NT. The two are separate
# operating systems with different APIs, so the source needs to know
# which one it is being compiled for.
!IF "$(WIN32S)" == "1"
DEFINES = -DWIN32S_COMPAT
!ELSE
DEFINES =
!ENDIF

# nmake 1.12 empties a macro that a definition uses when the two names
# differ only in case (it takes it for a reference to itself): so no
# CFLAGS = $(cflags) here.
# On Win32s there is no registry either.
!IF "$(WIN32S)" == "1"
STORAGE_OBJ = windows\storage-file.obj
REGISTRY_OBJ = windows\utils\no-registry.obj
SECURITY_OBJ = windows\utils\no-security.obj
!ELSE
STORAGE_OBJ = windows\storage.obj
REGISTRY_OBJ = windows\utils\registry.obj
SECURITY_OBJ = windows\utils\security.obj
!ENDIF

COMPILE = $(cc) $(cflags) $(cvarsmt) $(OPTIMISE) $(DEFINES) -D_WINDOWS -I. -Iwindows -Iterminal
COMPILE_OPT = $(cc) $(cflags) $(cvarsmt) $(cdebug) $(DEFINES) -D_WINDOWS -I. -Iwindows -Iterminal
CONSOLE_LIBS = $(conlibsmt) advapi32.lib user32.lib
GUI_LIBS = $(guilibsmt) advapi32.lib shell32.lib

# link32, of the SDK for Windows NT 3.1, takes resources only after
# cvtres has made them into an object file. Later linkers take .res
# files.
!IF "$(cvtres)" == "cvtres"
RES = rbj
!ELSE
RES = res
!ENDIF

# For Win32s, build only the programs that work on that platform (the
# reference port made the same restriction): the console tools have no
# way to wait for handles, and Pageant cannot use the IPC channel a
# single-threaded system provides. winsftp.exe is built for Win32s too
# once it exists.
!IF "$(WIN32S)" == "1"
ALL_PROGS = putty.exe puttytel.exe puttygen.exe winsftp.exe
!ELSE
ALL_PROGS = putty.exe puttytel.exe plink.exe pscp.exe psftp.exe psocks.exe \
	pageant.exe puttygen.exe
!ENDIF

all: $(ALL_PROGS)

.SUFFIXES: .rbj

.c.obj:
	$(COMPILE) -Fo$*.obj $*.c

.rc.res:
	$(rc) -r -Iwindows -I. -fo$*.res $*.rc

.res.rbj:
	$(cvtres) -$(CPU) $*.res -o $*.rbj

# be_list.c lists the backends of a program: compiled once for each.

be_putty.obj: be_list.c
	$(COMPILE) -DAPPNAME=PuTTY -DSSH=1 -DSERIAL=1 -DOTHERBACKENDS=1 -Fobe_putty.obj be_list.c

be_puttytel.obj: be_list.c
	$(COMPILE) -DAPPNAME=PuTTYtel -DSSH=0 -DSERIAL=1 -DOTHERBACKENDS=1 -Fobe_puttytel.obj be_list.c

be_plink.obj: be_list.c
	$(COMPILE) -DAPPNAME=Plink -DSSH=1 -DSERIAL=1 -DOTHERBACKENDS=1 -Fobe_plink.obj be_list.c

be_pscp.obj: be_list.c
	$(COMPILE) -DAPPNAME=PSCP -DSSH=1 -DSERIAL=0 -DOTHERBACKENDS=0 -Fobe_pscp.obj be_list.c

be_psftp.obj: be_list.c
	$(COMPILE) -DAPPNAME=PSFTP -DSSH=1 -DSERIAL=0 -DOTHERBACKENDS=0 -Fobe_psftp.obj be_list.c

psftp_winsftp.obj: psftp.c
	$(COMPILE) -DWINSFTP_BUILD=1 -Dprintf=winsftp_printf -Dfprintf=winsftp_fprintf -Fopsftp_winsftp.obj psftp.c

windows\win_sftp_fileio.obj: windows\sftp.c
	$(COMPILE) -DWINSFTP_BUILD=1 -Fowindows\win_sftp_fileio.obj windows\sftp.c

windows\winsftp.obj: windows\winsftp.c
	$(COMPILE) -DWINSFTP_BUILD=1 -Dprintf=winsftp_printf -Dfprintf=winsftp_fprintf -Fowindows\winsftp.obj windows\winsftp.c

# The bignum, elliptic curve, NTRU, ML-KEM and Argon2 code does most of
# the work when PuTTY connects or unlocks a key. It is optimised on
# MIPS too: for these files both MIPS compilers make the right code
# (PuTTY's testcrypt tests pass), and connecting takes half the time.

crypto\mpint.obj: crypto\mpint.c
	$(COMPILE_OPT) -Focrypto\mpint.obj crypto\mpint.c

crypto\ecc-arithmetic.obj: crypto\ecc-arithmetic.c
	$(COMPILE_OPT) -Focrypto\ecc-arithmetic.obj crypto\ecc-arithmetic.c

crypto\ntru.obj: crypto\ntru.c
	$(COMPILE_OPT) -Focrypto\ntru.obj crypto\ntru.c

crypto\mlkem.obj: crypto\mlkem.c
	$(COMPILE_OPT) -Focrypto\mlkem.obj crypto\mlkem.c

crypto\argon2.obj: crypto\argon2.c
	$(COMPILE_OPT) -Focrypto\argon2.obj crypto\argon2.c

crypto\blake2.obj: crypto\blake2.c
	$(COMPILE_OPT) -Focrypto\blake2.obj crypto\blake2.c

PUTTY = \
	be_putty.obj callback.obj cmdline.obj config.obj \
	crypto\aes-common.obj crypto\aes-select.obj crypto\aes-sw.obj \
	crypto\aesgcm-select.obj crypto\aesgcm-sw.obj crypto\arcfour.obj \
	crypto\argon2.obj crypto\blake2.obj crypto\blowfish.obj \
	crypto\chacha20-poly1305.obj crypto\crc32.obj crypto\des.obj \
	crypto\diffie-hellman.obj crypto\dsa.obj crypto\ecc-arithmetic.obj \
	crypto\ecc-ssh.obj crypto\hash_simple.obj crypto\hmac.obj \
	crypto\kex-hybrid.obj crypto\mac.obj crypto\mac_simple.obj \
	crypto\md5.obj crypto\mlkem.obj crypto\mpint.obj crypto\ntru.obj \
	crypto\openssh-certs.obj crypto\prng.obj crypto\pubkey-ppk.obj \
	crypto\pubkey-ssh1.obj crypto\rfc6979.obj crypto\rsa.obj \
	crypto\sha1-common.obj crypto\sha1-select.obj crypto\sha1-sw.obj \
	crypto\sha256-common.obj crypto\sha256-select.obj \
	crypto\sha256-sw.obj crypto\sha3.obj crypto\sha512-common.obj \
	crypto\sha512-select.obj crypto\sha512-sw.obj crypto\xdmauth.obj \
	dialog.obj errsock.obj ldisc.obj logging.obj otherbackends\raw.obj \
	otherbackends\rlogin.obj otherbackends\supdup.obj \
	otherbackends\telnet.obj pinger.obj proxy\cproxy.obj \
	proxy\http.obj proxy\interactor.obj proxy\local.obj \
	proxy\newmain.obj proxy\proxy.obj proxy\socks4.obj \
	proxy\socks5.obj proxy\sshproxy.obj proxy\telnet.obj settings.obj \
	ssh\agentf.obj ssh\bpp-bare.obj ssh\bpp1.obj ssh\bpp2.obj \
	ssh\ca-config.obj ssh\censor1.obj ssh\censor2.obj ssh\common.obj \
	ssh\connection1-client.obj ssh\connection1.obj \
	ssh\connection2-client.obj ssh\connection2.obj \
	ssh\crc-attack-detector.obj ssh\kex2-client.obj ssh\login1.obj \
	ssh\mainchan.obj ssh\nocompress.obj ssh\portfwd.obj \
	ssh\sharing.obj ssh\ssh.obj ssh\transient-hostkey-cache.obj \
	ssh\transport2.obj ssh\userauth2-client.obj ssh\verstring.obj \
	ssh\x11fwd.obj ssh\zlib.obj sshpubk.obj sshrand.obj \
	stubs\no-console.obj stubs\no-gss.obj stubs\null-cipher.obj \
	stubs\null-key.obj stubs\null-lp.obj stubs\null-mac.obj \
	stubs\null-opener.obj stubs\null-plug.obj stubs\null-seat.obj \
	stubs\null-socket.obj terminal\bidi.obj terminal\lineedit.obj \
	terminal\terminal.obj timing.obj utils\antispoof.obj \
	utils\backend_socket_log.obj utils\base64_decode.obj \
	utils\base64_decode_atom.obj utils\base64_encode.obj \
	utils\base64_encode_atom.obj utils\base64_valid.obj \
	utils\bufchain.obj utils\buildinfo.obj utils\burnstr.obj \
	utils\burnwcs.obj utils\cert-expr.obj utils\chomp.obj \
	utils\cmdline_get_passwd_input_state_new.obj utils\conf.obj \
	utils\conf_data.obj utils\conf_dest.obj utils\conf_launchable.obj \
	utils\ctrlparse.obj utils\ctrlset_normalise.obj \
	utils\decode_utf8.obj utils\decode_utf8_to_wchar.obj \
	utils\decode_utf8_to_wide_string.obj utils\default_description.obj \
	utils\dup_mb_to_wc.obj utils\dup_wc_to_mb.obj utils\dupcat.obj \
	utils\dupprintf.obj utils\dupstr.obj utils\dupwcs.obj \
	utils\dupwcscat.obj utils\encode_utf8.obj \
	utils\encode_wide_string_as_utf8.obj utils\fgetline.obj \
	utils\format_connection_setup_command.obj \
	utils\host_ca_new_free.obj utils\host_strchr.obj \
	utils\host_strchr_internal.obj utils\host_strcspn.obj \
	utils\host_strduptrim.obj utils\host_strrchr.obj \
	utils\key_components.obj utils\log_proxy_stderr.obj \
	utils\logeventf.obj utils\make_spr_sw_abort_static.obj \
	utils\marshal.obj utils\memory.obj utils\memxor.obj \
	utils\nullstrcmp.obj utils\out_of_memory.obj \
	utils\parse_blocksize.obj utils\percent_decode.obj \
	utils\percent_encode.obj utils\prompts.obj utils\ptrlen.obj \
	utils\seat_connection_fatal.obj utils\seat_dialog_text.obj \
	utils\seat_nonfatal.obj utils\sessprep.obj \
	utils\sk_free_peer_info.obj utils\smemclr.obj utils\smemeq.obj \
	utils\spr_get_error_message.obj utils\ssh2_pick_fingerprint.obj \
	utils\sshutils.obj utils\strbuf.obj \
	utils\string_length_for_printf.obj utils\stripctrl.obj \
	utils\tempseat.obj utils\tree234.obj utils\uint64.obj \
	utils\validate_manual_hostkey.obj utils\version.obj \
	utils\wcwidth.obj utils\wildcard.obj \
	utils\write_c_string_literal.obj utils\x11_dehexify.obj \
	utils\x11_identify_auth_proto.obj utils\x11_make_greeting.obj \
	utils\x11_parse_ip.obj utils\x11authfile.obj \
	utils\x11authnames.obj windows\agent-client.obj \
	windows\commitid.obj windows\config.obj windows\controls.obj \
	windows\dialog.obj windows\handle-io.obj windows\handle-socket.obj \
	windows\handle-wait.obj windows\local-proxy.obj \
	windows\named-pipe-client.obj windows\named-pipe-server.obj \
	windows\network.obj windows\no-jump-list.obj windows\noise.obj \
	windows\printing.obj windows\putty.obj windows\select-gui.obj \
	windows\serial.obj windows\sharing.obj windows\sizetip.obj \
	$(STORAGE_OBJ) windows\unicode.obj \
	windows\utils\agent_named_pipe_name.obj \
	windows\utils\blink_time.obj windows\utils\centre_window.obj \
	windows\utils\cmdline_arg.obj windows\utils\cryptoapi.obj \
	windows\utils\defaults.obj \
	windows\utils\dll_hijacking_protection.obj \
	windows\utils\escape_registry_key.obj windows\utils\filename.obj \
	windows\utils\fontspec.obj windows\utils\get_system_dir.obj \
	windows\utils\get_username.obj \
	windows\utils\getdlgitemtext_alloc.obj \
	windows\utils\gui-timing.obj windows\utils\interprocess_mutex.obj \
	windows\utils\load_system32_dll.obj windows\utils\ltime.obj \
	windows\utils\makedlgitemborderless.obj \
	windows\utils\message_box.obj \
	windows\utils\open_for_write_would_lose_data.obj \
	windows\utils\pgp_fingerprints_msgbox.obj \
	windows\utils\platform_get_x_display.obj \
	$(REGISTRY_OBJ) windows\utils\request_file.obj \
	$(SECURITY_OBJ) windows\utils\shinydialogbox.obj \
	windows\utils\split_into_argv_w.obj \
	windows\utils\subprocess_waiter.obj windows\utils\version.obj \
	windows\utils\win_strerror.obj windows\window.obj windows\x11.obj \
	x11disp.obj

windows\putty.res: windows\putty.rc
windows\putty.rbj: windows\putty.res

putty.exe: $(PUTTY) windows\putty.$(RES)
	$(link) $(guiflags) $(ldebug) -out:putty.exe @<<
$(PUTTY) windows\putty.$(RES) $(GUI_LIBS)
<<

PUTTYTEL = \
	be_puttytel.obj callback.obj cmdline.obj config.obj dialog.obj \
	errsock.obj ldisc.obj logging.obj otherbackends\raw.obj \
	otherbackends\rlogin.obj otherbackends\supdup.obj \
	otherbackends\telnet.obj pinger.obj proxy\http.obj \
	proxy\interactor.obj proxy\local.obj proxy\newmain.obj \
	proxy\nocproxy.obj proxy\nosshproxy.obj proxy\proxy.obj \
	proxy\socks4.obj proxy\socks5.obj proxy\telnet.obj settings.obj \
	stubs\no-ca-config.obj stubs\no-console.obj stubs\no-gss.obj \
	stubs\no-rand.obj stubs\null-lp.obj stubs\null-opener.obj \
	stubs\null-seat.obj stubs\null-socket.obj terminal\bidi.obj \
	terminal\lineedit.obj terminal\terminal.obj timing.obj \
	utils\antispoof.obj utils\backend_socket_log.obj \
	utils\base64_decode.obj utils\base64_decode_atom.obj \
	utils\base64_encode.obj utils\base64_encode_atom.obj \
	utils\bufchain.obj utils\buildinfo.obj utils\burnstr.obj \
	utils\burnwcs.obj utils\cert-expr.obj utils\chomp.obj \
	utils\cmdline_get_passwd_input_state_new.obj utils\conf.obj \
	utils\conf_data.obj utils\conf_dest.obj utils\conf_launchable.obj \
	utils\ctrlparse.obj utils\ctrlset_normalise.obj \
	utils\decode_utf8.obj utils\decode_utf8_to_wchar.obj \
	utils\decode_utf8_to_wide_string.obj utils\default_description.obj \
	utils\dup_mb_to_wc.obj utils\dup_wc_to_mb.obj utils\dupcat.obj \
	utils\dupprintf.obj utils\dupstr.obj utils\dupwcs.obj \
	utils\dupwcscat.obj utils\encode_utf8.obj \
	utils\encode_wide_string_as_utf8.obj utils\fgetline.obj \
	utils\format_connection_setup_command.obj \
	utils\host_ca_new_free.obj utils\host_strchr.obj \
	utils\host_strchr_internal.obj utils\host_strcspn.obj \
	utils\host_strduptrim.obj utils\host_strrchr.obj \
	utils\log_proxy_stderr.obj utils\logeventf.obj \
	utils\make_spr_sw_abort_static.obj utils\marshal.obj \
	utils\memory.obj utils\out_of_memory.obj utils\percent_decode.obj \
	utils\percent_encode.obj utils\prompts.obj utils\ptrlen.obj \
	utils\seat_connection_fatal.obj utils\sessprep.obj \
	utils\sk_free_peer_info.obj utils\smemclr.obj \
	utils\spr_get_error_message.obj utils\strbuf.obj \
	utils\stripctrl.obj utils\tempseat.obj utils\tree234.obj \
	utils\validate_manual_hostkey.obj utils\version.obj \
	utils\wcwidth.obj utils\wildcard.obj \
	utils\write_c_string_literal.obj windows\commitid.obj \
	windows\config.obj windows\controls.obj windows\dialog.obj \
	windows\handle-io.obj windows\handle-socket.obj \
	windows\handle-wait.obj windows\local-proxy.obj \
	windows\network.obj windows\no-jump-list.obj windows\printing.obj \
	windows\putty.obj windows\select-gui.obj windows\serial.obj \
	windows\sizetip.obj $(STORAGE_OBJ) windows\unicode.obj \
	windows\utils\blink_time.obj windows\utils\centre_window.obj \
	windows\utils\cmdline_arg.obj windows\utils\defaults.obj \
	windows\utils\dll_hijacking_protection.obj \
	windows\utils\escape_registry_key.obj windows\utils\filename.obj \
	windows\utils\fontspec.obj windows\utils\get_system_dir.obj \
	windows\utils\get_username.obj \
	windows\utils\getdlgitemtext_alloc.obj \
	windows\utils\gui-timing.obj windows\utils\load_system32_dll.obj \
	windows\utils\ltime.obj windows\utils\makedlgitemborderless.obj \
	windows\utils\message_box.obj \
	windows\utils\open_for_write_would_lose_data.obj \
	windows\utils\pgp_fingerprints_msgbox.obj \
	$(REGISTRY_OBJ) windows\utils\request_file.obj \
	$(SECURITY_OBJ) windows\utils\shinydialogbox.obj \
	windows\utils\split_into_argv_w.obj \
	windows\utils\subprocess_waiter.obj windows\utils\version.obj \
	windows\utils\win_strerror.obj windows\window.obj

windows\puttytel.res: windows\puttytel.rc
windows\puttytel.rbj: windows\puttytel.res

puttytel.exe: $(PUTTYTEL) windows\puttytel.$(RES)
	$(link) $(guiflags) $(ldebug) -out:puttytel.exe @<<
$(PUTTYTEL) windows\puttytel.$(RES) $(GUI_LIBS)
<<

PLINK = \
	be_plink.obj callback.obj clicons.obj cmdline.obj console.obj \
	crypto\aes-common.obj crypto\aes-select.obj crypto\aes-sw.obj \
	crypto\aesgcm-select.obj crypto\aesgcm-sw.obj crypto\arcfour.obj \
	crypto\argon2.obj crypto\blake2.obj crypto\blowfish.obj \
	crypto\chacha20-poly1305.obj crypto\crc32.obj crypto\des.obj \
	crypto\diffie-hellman.obj crypto\dsa.obj crypto\ecc-arithmetic.obj \
	crypto\ecc-ssh.obj crypto\hash_simple.obj crypto\hmac.obj \
	crypto\kex-hybrid.obj crypto\mac.obj crypto\mac_simple.obj \
	crypto\md5.obj crypto\mlkem.obj crypto\mpint.obj crypto\ntru.obj \
	crypto\openssh-certs.obj crypto\prng.obj crypto\pubkey-ppk.obj \
	crypto\pubkey-ssh1.obj crypto\rfc6979.obj crypto\rsa.obj \
	crypto\sha1-common.obj crypto\sha1-select.obj crypto\sha1-sw.obj \
	crypto\sha256-common.obj crypto\sha256-select.obj \
	crypto\sha256-sw.obj crypto\sha3.obj crypto\sha512-common.obj \
	crypto\sha512-select.obj crypto\sha512-sw.obj crypto\xdmauth.obj \
	errsock.obj ldisc.obj logging.obj otherbackends\raw.obj \
	otherbackends\rlogin.obj otherbackends\supdup.obj \
	otherbackends\telnet.obj pinger.obj proxy\cproxy.obj \
	proxy\http.obj proxy\interactor.obj proxy\local.obj \
	proxy\newmain.obj proxy\proxy.obj proxy\socks4.obj \
	proxy\socks5.obj proxy\sshproxy.obj proxy\telnet.obj settings.obj \
	ssh\agentf.obj ssh\bpp-bare.obj ssh\bpp1.obj ssh\bpp2.obj \
	ssh\censor1.obj ssh\censor2.obj ssh\common.obj \
	ssh\connection1-client.obj ssh\connection1.obj \
	ssh\connection2-client.obj ssh\connection2.obj \
	ssh\crc-attack-detector.obj ssh\kex2-client.obj ssh\login1.obj \
	ssh\mainchan.obj ssh\nocompress.obj ssh\portfwd.obj \
	ssh\sharing.obj ssh\ssh.obj ssh\transient-hostkey-cache.obj \
	ssh\transport2.obj ssh\userauth2-client.obj ssh\verstring.obj \
	ssh\x11fwd.obj ssh\zlib.obj sshpubk.obj sshrand.obj \
	stubs\no-gss.obj stubs\no-lineedit.obj stubs\no-term.obj \
	stubs\null-cipher.obj stubs\null-key.obj stubs\null-lp.obj \
	stubs\null-mac.obj stubs\null-opener.obj stubs\null-plug.obj \
	stubs\null-seat.obj stubs\null-socket.obj timing.obj \
	utils\antispoof.obj utils\backend_socket_log.obj \
	utils\base64_decode.obj utils\base64_decode_atom.obj \
	utils\base64_encode.obj utils\base64_encode_atom.obj \
	utils\bufchain.obj utils\buildinfo.obj utils\burnstr.obj \
	utils\burnwcs.obj utils\cert-expr.obj utils\chomp.obj \
	utils\conf.obj utils\conf_data.obj utils\conf_dest.obj \
	utils\conf_launchable.obj utils\ctrlparse.obj \
	utils\decode_utf8.obj utils\decode_utf8_to_wchar.obj \
	utils\decode_utf8_to_wide_string.obj utils\default_description.obj \
	utils\dup_mb_to_wc.obj utils\dup_wc_to_mb.obj utils\dupcat.obj \
	utils\dupprintf.obj utils\dupstr.obj utils\dupwcs.obj \
	utils\encode_utf8.obj utils\encode_wide_string_as_utf8.obj \
	utils\fgetline.obj utils\format_connection_setup_command.obj \
	utils\host_ca_new_free.obj utils\host_strchr.obj \
	utils\host_strchr_internal.obj utils\host_strcspn.obj \
	utils\host_strduptrim.obj utils\host_strrchr.obj \
	utils\key_components.obj utils\log_proxy_stderr.obj \
	utils\logeventf.obj utils\make_spr_sw_abort_static.obj \
	utils\marshal.obj utils\memory.obj utils\memxor.obj \
	utils\nullstrcmp.obj utils\out_of_memory.obj \
	utils\parse_blocksize.obj utils\percent_decode.obj \
	utils\percent_encode.obj utils\prompts.obj utils\ptrlen.obj \
	utils\seat_connection_fatal.obj utils\seat_dialog_text.obj \
	utils\seat_nonfatal.obj utils\sessprep.obj \
	utils\sk_free_peer_info.obj utils\smemclr.obj utils\smemeq.obj \
	utils\spr_get_error_message.obj utils\ssh2_pick_fingerprint.obj \
	utils\sshutils.obj utils\strbuf.obj \
	utils\string_length_for_printf.obj utils\stripctrl.obj \
	utils\tempseat.obj utils\tree234.obj utils\uint64.obj \
	utils\validate_manual_hostkey.obj utils\version.obj \
	utils\wcwidth.obj utils\wildcard.obj utils\wordwrap.obj \
	utils\write_c_string_literal.obj utils\x11_dehexify.obj \
	utils\x11_identify_auth_proto.obj utils\x11_make_greeting.obj \
	utils\x11_parse_ip.obj utils\x11authfile.obj \
	utils\x11authnames.obj windows\agent-client.obj \
	windows\cliloop.obj windows\commitid.obj windows\console.obj \
	windows\handle-io.obj windows\handle-socket.obj \
	windows\handle-wait.obj windows\local-proxy.obj \
	windows\named-pipe-client.obj windows\named-pipe-server.obj \
	windows\network.obj windows\no-jump-list.obj windows\noise.obj \
	windows\plink.obj windows\select-cli.obj windows\select-gui.obj \
	windows\serial.obj windows\sharing.obj $(STORAGE_OBJ) \
	windows\unicode.obj windows\utils\agent_named_pipe_name.obj \
	windows\utils\cmdline_arg.obj windows\utils\cryptoapi.obj \
	windows\utils\defaults.obj \
	windows\utils\dll_hijacking_protection.obj \
	windows\utils\escape_registry_key.obj windows\utils\filename.obj \
	windows\utils\fontspec.obj windows\utils\get_system_dir.obj \
	windows\utils\get_username.obj \
	windows\utils\interprocess_mutex.obj \
	windows\utils\is_console_handle.obj \
	windows\utils\load_system32_dll.obj windows\utils\ltime.obj \
	windows\utils\make_spr_sw_abort_winerror.obj \
	windows\utils\open_for_write_would_lose_data.obj \
	windows\utils\platform_get_x_display.obj \
	$(REGISTRY_OBJ) $(SECURITY_OBJ) \
	windows\utils\split_into_argv_w.obj \
	windows\utils\subprocess_waiter.obj windows\utils\version.obj \
	windows\utils\win_strerror.obj windows\x11.obj x11disp.obj

windows\plink.res: windows\plink.rc
windows\plink.rbj: windows\plink.res

plink.exe: $(PLINK) windows\plink.$(RES)
	$(link) $(conflags) $(ldebug) -out:plink.exe @<<
$(PLINK) windows\plink.$(RES) $(CONSOLE_LIBS)
<<

PSCP = \
	be_pscp.obj callback.obj clicons.obj cmdline.obj console.obj \
	crypto\aes-common.obj crypto\aes-select.obj crypto\aes-sw.obj \
	crypto\aesgcm-select.obj crypto\aesgcm-sw.obj crypto\arcfour.obj \
	crypto\argon2.obj crypto\blake2.obj crypto\blowfish.obj \
	crypto\chacha20-poly1305.obj crypto\crc32.obj crypto\des.obj \
	crypto\diffie-hellman.obj crypto\dsa.obj crypto\ecc-arithmetic.obj \
	crypto\ecc-ssh.obj crypto\hash_simple.obj crypto\hmac.obj \
	crypto\kex-hybrid.obj crypto\mac.obj crypto\mac_simple.obj \
	crypto\md5.obj crypto\mlkem.obj crypto\mpint.obj crypto\ntru.obj \
	crypto\openssh-certs.obj crypto\prng.obj crypto\pubkey-ppk.obj \
	crypto\pubkey-ssh1.obj crypto\rfc6979.obj crypto\rsa.obj \
	crypto\sha1-common.obj crypto\sha1-select.obj crypto\sha1-sw.obj \
	crypto\sha256-common.obj crypto\sha256-select.obj \
	crypto\sha256-sw.obj crypto\sha3.obj crypto\sha512-common.obj \
	crypto\sha512-select.obj crypto\sha512-sw.obj crypto\xdmauth.obj \
	errsock.obj logging.obj pinger.obj proxy\cproxy.obj proxy\http.obj \
	proxy\interactor.obj proxy\local.obj proxy\newmain.obj \
	proxy\proxy.obj proxy\socks4.obj proxy\socks5.obj \
	proxy\sshproxy.obj proxy\telnet.obj pscp.obj psftpcommon.obj \
	settings.obj ssh\agentf.obj ssh\bpp-bare.obj ssh\bpp1.obj \
	ssh\bpp2.obj ssh\censor1.obj ssh\censor2.obj ssh\common.obj \
	ssh\connection1-client.obj ssh\connection1.obj \
	ssh\connection2-client.obj ssh\connection2.obj \
	ssh\crc-attack-detector.obj ssh\kex2-client.obj ssh\login1.obj \
	ssh\mainchan.obj ssh\nocompress.obj ssh\portfwd.obj ssh\sftp.obj \
	ssh\sftpcommon.obj ssh\sharing.obj ssh\ssh.obj \
	ssh\transient-hostkey-cache.obj ssh\transport2.obj \
	ssh\userauth2-client.obj ssh\verstring.obj ssh\x11fwd.obj \
	ssh\zlib.obj sshpubk.obj sshrand.obj stubs\no-gss.obj \
	stubs\null-cipher.obj stubs\null-key.obj stubs\null-lp.obj \
	stubs\null-mac.obj stubs\null-opener.obj stubs\null-plug.obj \
	stubs\null-seat.obj stubs\null-socket.obj timing.obj \
	utils\antispoof.obj utils\backend_socket_log.obj \
	utils\base64_decode.obj utils\base64_decode_atom.obj \
	utils\base64_encode.obj utils\base64_encode_atom.obj \
	utils\bufchain.obj utils\buildinfo.obj utils\burnstr.obj \
	utils\burnwcs.obj utils\cert-expr.obj utils\chomp.obj \
	utils\conf.obj utils\conf_data.obj utils\conf_dest.obj \
	utils\conf_launchable.obj utils\ctrlparse.obj \
	utils\decode_utf8.obj utils\decode_utf8_to_wchar.obj \
	utils\decode_utf8_to_wide_string.obj utils\default_description.obj \
	utils\dup_mb_to_wc.obj utils\dup_wc_to_mb.obj utils\dupcat.obj \
	utils\dupprintf.obj utils\dupstr.obj utils\dupwcs.obj \
	utils\encode_utf8.obj utils\encode_wide_string_as_utf8.obj \
	utils\fgetline.obj utils\format_connection_setup_command.obj \
	utils\host_ca_new_free.obj utils\host_strchr.obj \
	utils\host_strchr_internal.obj utils\host_strcspn.obj \
	utils\host_strduptrim.obj utils\host_strrchr.obj \
	utils\key_components.obj utils\log_proxy_stderr.obj \
	utils\logeventf.obj utils\make_spr_sw_abort_static.obj \
	utils\marshal.obj utils\memory.obj utils\memxor.obj \
	utils\nullstrcmp.obj utils\out_of_memory.obj \
	utils\parse_blocksize.obj utils\percent_decode.obj \
	utils\percent_encode.obj utils\prompts.obj utils\ptrlen.obj \
	utils\seat_connection_fatal.obj utils\seat_dialog_text.obj \
	utils\seat_nonfatal.obj utils\sessprep.obj \
	utils\sk_free_peer_info.obj utils\smemclr.obj utils\smemeq.obj \
	utils\spr_get_error_message.obj utils\ssh2_pick_fingerprint.obj \
	utils\sshutils.obj utils\strbuf.obj \
	utils\string_length_for_printf.obj utils\stripctrl.obj \
	utils\tempseat.obj utils\tree234.obj utils\uint64.obj \
	utils\validate_manual_hostkey.obj utils\version.obj \
	utils\wcwidth.obj utils\wildcard.obj utils\wordwrap.obj \
	utils\write_c_string_literal.obj utils\x11_dehexify.obj \
	utils\x11_identify_auth_proto.obj utils\x11_make_greeting.obj \
	utils\x11_parse_ip.obj utils\x11authnames.obj \
	windows\agent-client.obj windows\cliloop.obj windows\commitid.obj \
	windows\console.obj windows\handle-io.obj \
	windows\handle-socket.obj windows\handle-wait.obj \
	windows\local-proxy.obj windows\named-pipe-client.obj \
	windows\named-pipe-server.obj windows\network.obj \
	windows\no-jump-list.obj windows\noise.obj windows\select-cli.obj \
	windows\select-gui.obj windows\sftp.obj windows\sharing.obj \
	$(STORAGE_OBJ) windows\unicode.obj \
	windows\utils\agent_named_pipe_name.obj \
	windows\utils\cmdline_arg.obj windows\utils\cryptoapi.obj \
	windows\utils\defaults.obj \
	windows\utils\dll_hijacking_protection.obj \
	windows\utils\escape_registry_key.obj windows\utils\filename.obj \
	windows\utils\fontspec.obj windows\utils\get_system_dir.obj \
	windows\utils\get_username.obj \
	windows\utils\interprocess_mutex.obj \
	windows\utils\is_console_handle.obj \
	windows\utils\load_system32_dll.obj windows\utils\ltime.obj \
	windows\utils\make_spr_sw_abort_winerror.obj \
	windows\utils\open_for_write_would_lose_data.obj \
	windows\utils\platform_get_x_display.obj \
	$(REGISTRY_OBJ) $(SECURITY_OBJ) \
	windows\utils\split_into_argv_w.obj \
	windows\utils\subprocess_waiter.obj windows\utils\version.obj \
	windows\utils\win_strerror.obj x11disp.obj

windows\pscp.res: windows\pscp.rc
windows\pscp.rbj: windows\pscp.res

pscp.exe: $(PSCP) windows\pscp.$(RES)
	$(link) $(conflags) $(ldebug) -out:pscp.exe @<<
$(PSCP) windows\pscp.$(RES) $(CONSOLE_LIBS)
<<

PSFTP = \
	be_psftp.obj callback.obj clicons.obj cmdline.obj console.obj \
	crypto\aes-common.obj crypto\aes-select.obj crypto\aes-sw.obj \
	crypto\aesgcm-select.obj crypto\aesgcm-sw.obj crypto\arcfour.obj \
	crypto\argon2.obj crypto\blake2.obj crypto\blowfish.obj \
	crypto\chacha20-poly1305.obj crypto\crc32.obj crypto\des.obj \
	crypto\diffie-hellman.obj crypto\dsa.obj crypto\ecc-arithmetic.obj \
	crypto\ecc-ssh.obj crypto\hash_simple.obj crypto\hmac.obj \
	crypto\kex-hybrid.obj crypto\mac.obj crypto\mac_simple.obj \
	crypto\md5.obj crypto\mlkem.obj crypto\mpint.obj crypto\ntru.obj \
	crypto\openssh-certs.obj crypto\prng.obj crypto\pubkey-ppk.obj \
	crypto\pubkey-ssh1.obj crypto\rfc6979.obj crypto\rsa.obj \
	crypto\sha1-common.obj crypto\sha1-select.obj crypto\sha1-sw.obj \
	crypto\sha256-common.obj crypto\sha256-select.obj \
	crypto\sha256-sw.obj crypto\sha3.obj crypto\sha512-common.obj \
	crypto\sha512-select.obj crypto\sha512-sw.obj crypto\xdmauth.obj \
	errsock.obj logging.obj pinger.obj proxy\cproxy.obj proxy\http.obj \
	proxy\interactor.obj proxy\local.obj proxy\newmain.obj \
	proxy\proxy.obj proxy\socks4.obj proxy\socks5.obj \
	proxy\sshproxy.obj proxy\telnet.obj psftp.obj psftpcommon.obj \
	settings.obj ssh\agentf.obj ssh\bpp-bare.obj ssh\bpp1.obj \
	ssh\bpp2.obj ssh\censor1.obj ssh\censor2.obj ssh\common.obj \
	ssh\connection1-client.obj ssh\connection1.obj \
	ssh\connection2-client.obj ssh\connection2.obj \
	ssh\crc-attack-detector.obj ssh\kex2-client.obj ssh\login1.obj \
	ssh\mainchan.obj ssh\nocompress.obj ssh\portfwd.obj ssh\sftp.obj \
	ssh\sftpcommon.obj ssh\sharing.obj ssh\ssh.obj \
	ssh\transient-hostkey-cache.obj ssh\transport2.obj \
	ssh\userauth2-client.obj ssh\verstring.obj ssh\x11fwd.obj \
	ssh\zlib.obj sshpubk.obj sshrand.obj stubs\no-gss.obj \
	stubs\null-cipher.obj stubs\null-key.obj stubs\null-lp.obj \
	stubs\null-mac.obj stubs\null-opener.obj stubs\null-plug.obj \
	stubs\null-seat.obj stubs\null-socket.obj timing.obj \
	utils\antispoof.obj utils\backend_socket_log.obj \
	utils\base64_decode.obj utils\base64_decode_atom.obj \
	utils\base64_encode.obj utils\base64_encode_atom.obj \
	utils\bufchain.obj utils\buildinfo.obj utils\burnstr.obj \
	utils\burnwcs.obj utils\cert-expr.obj utils\chomp.obj \
	utils\conf.obj utils\conf_data.obj utils\conf_dest.obj \
	utils\conf_launchable.obj utils\ctrlparse.obj \
	utils\decode_utf8.obj utils\decode_utf8_to_wchar.obj \
	utils\decode_utf8_to_wide_string.obj utils\default_description.obj \
	utils\dup_mb_to_wc.obj utils\dup_wc_to_mb.obj utils\dupcat.obj \
	utils\dupprintf.obj utils\dupstr.obj utils\dupwcs.obj \
	utils\encode_utf8.obj utils\encode_wide_string_as_utf8.obj \
	utils\fgetline.obj utils\format_connection_setup_command.obj \
	utils\host_ca_new_free.obj utils\host_strchr.obj \
	utils\host_strchr_internal.obj utils\host_strcspn.obj \
	utils\host_strduptrim.obj utils\host_strrchr.obj \
	utils\key_components.obj utils\log_proxy_stderr.obj \
	utils\logeventf.obj utils\make_spr_sw_abort_static.obj \
	utils\marshal.obj utils\memory.obj utils\memxor.obj \
	utils\nullstrcmp.obj utils\out_of_memory.obj \
	utils\parse_blocksize.obj utils\percent_decode.obj \
	utils\percent_encode.obj utils\prompts.obj utils\ptrlen.obj \
	utils\seat_connection_fatal.obj utils\seat_dialog_text.obj \
	utils\seat_nonfatal.obj utils\sessprep.obj \
	utils\sk_free_peer_info.obj utils\smemclr.obj utils\smemeq.obj \
	utils\spr_get_error_message.obj utils\ssh2_pick_fingerprint.obj \
	utils\sshutils.obj utils\strbuf.obj \
	utils\string_length_for_printf.obj utils\stripctrl.obj \
	utils\tempseat.obj utils\tree234.obj utils\uint64.obj \
	utils\validate_manual_hostkey.obj utils\version.obj \
	utils\wcwidth.obj utils\wildcard.obj utils\wordwrap.obj \
	utils\write_c_string_literal.obj utils\x11_dehexify.obj \
	utils\x11_identify_auth_proto.obj utils\x11_make_greeting.obj \
	utils\x11_parse_ip.obj utils\x11authnames.obj \
	windows\agent-client.obj windows\cliloop.obj windows\commitid.obj \
	windows\console.obj windows\handle-io.obj \
	windows\handle-socket.obj windows\handle-wait.obj \
	windows\local-proxy.obj windows\named-pipe-client.obj \
	windows\named-pipe-server.obj windows\network.obj \
	windows\no-jump-list.obj windows\noise.obj windows\select-cli.obj \
	windows\select-gui.obj windows\sftp.obj windows\sharing.obj \
	$(STORAGE_OBJ) windows\unicode.obj \
	windows\utils\agent_named_pipe_name.obj \
	windows\utils\cmdline_arg.obj windows\utils\cryptoapi.obj \
	windows\utils\defaults.obj \
	windows\utils\dll_hijacking_protection.obj \
	windows\utils\escape_registry_key.obj windows\utils\filename.obj \
	windows\utils\fontspec.obj windows\utils\get_system_dir.obj \
	windows\utils\get_username.obj \
	windows\utils\interprocess_mutex.obj \
	windows\utils\is_console_handle.obj \
	windows\utils\load_system32_dll.obj windows\utils\ltime.obj \
	windows\utils\make_spr_sw_abort_winerror.obj \
	windows\utils\open_for_write_would_lose_data.obj \
	windows\utils\platform_get_x_display.obj \
	$(REGISTRY_OBJ) $(SECURITY_OBJ) \
	windows\utils\split_into_argv_w.obj \
	windows\utils\subprocess_waiter.obj windows\utils\version.obj \
	windows\utils\win_strerror.obj x11disp.obj

windows\psftp.res: windows\psftp.rc
windows\psftp.rbj: windows\psftp.res

psftp.exe: $(PSFTP) windows\psftp.$(RES)
	$(link) $(conflags) $(ldebug) -out:psftp.exe @<<
$(PSFTP) windows\psftp.$(RES) $(CONSOLE_LIBS)
<<

# winsftp.exe: a graphical SFTP client. Like psftp.exe, but with the
# console layer compiled out and replaced by the GUI in winsftp.c.
# Built only for Win32s (see ALL_PROGS).
WINSFTP = \
	be_psftp.obj callback.obj cmdline.obj \
	crypto\aes-common.obj crypto\aes-select.obj crypto\aes-sw.obj \
	crypto\aesgcm-select.obj crypto\aesgcm-sw.obj crypto\arcfour.obj \
	crypto\argon2.obj crypto\blake2.obj crypto\blowfish.obj \
	crypto\chacha20-poly1305.obj crypto\crc32.obj crypto\des.obj \
	crypto\diffie-hellman.obj crypto\dsa.obj crypto\ecc-arithmetic.obj \
	crypto\ecc-ssh.obj crypto\hash_simple.obj crypto\hmac.obj \
	crypto\kex-hybrid.obj crypto\mac.obj crypto\mac_simple.obj \
	crypto\md5.obj crypto\mlkem.obj crypto\mpint.obj crypto\ntru.obj \
	crypto\openssh-certs.obj crypto\prng.obj crypto\pubkey-ppk.obj \
	crypto\pubkey-ssh1.obj crypto\rfc6979.obj crypto\rsa.obj \
	crypto\sha1-common.obj crypto\sha1-select.obj crypto\sha1-sw.obj \
	crypto\sha256-common.obj crypto\sha256-select.obj \
	crypto\sha256-sw.obj crypto\sha3.obj crypto\sha512-common.obj \
	crypto\sha512-select.obj crypto\sha512-sw.obj crypto\xdmauth.obj \
	errsock.obj logging.obj pinger.obj proxy\cproxy.obj proxy\http.obj \
	proxy\interactor.obj proxy\local.obj proxy\newmain.obj \
	proxy\proxy.obj proxy\socks4.obj proxy\socks5.obj \
	proxy\sshproxy.obj proxy\telnet.obj psftp_winsftp.obj psftpcommon.obj \
	settings.obj ssh\agentf.obj ssh\bpp-bare.obj ssh\bpp1.obj \
	ssh\bpp2.obj ssh\censor1.obj ssh\censor2.obj ssh\common.obj \
	ssh\connection1-client.obj ssh\connection1.obj \
	ssh\connection2-client.obj ssh\connection2.obj \
	ssh\crc-attack-detector.obj ssh\kex2-client.obj ssh\login1.obj \
	ssh\mainchan.obj ssh\nocompress.obj ssh\portfwd.obj ssh\sftp.obj \
	ssh\sftpcommon.obj ssh\sharing.obj ssh\ssh.obj \
	ssh\transient-hostkey-cache.obj ssh\transport2.obj \
	ssh\userauth2-client.obj ssh\verstring.obj ssh\x11fwd.obj \
	ssh\zlib.obj sshpubk.obj sshrand.obj stubs\no-gss.obj \
	stubs\null-cipher.obj stubs\null-key.obj stubs\null-lp.obj \
	stubs\null-mac.obj stubs\null-opener.obj stubs\null-plug.obj \
	stubs\null-seat.obj stubs\null-socket.obj timing.obj \
	utils\antispoof.obj utils\backend_socket_log.obj \
	utils\base64_decode.obj utils\base64_decode_atom.obj \
	utils\base64_encode.obj utils\base64_encode_atom.obj \
	utils\bufchain.obj utils\buildinfo.obj utils\burnstr.obj \
	utils\burnwcs.obj utils\cert-expr.obj utils\chomp.obj \
	utils\conf.obj utils\conf_data.obj utils\conf_dest.obj \
	utils\conf_launchable.obj utils\ctrlparse.obj \
	utils\decode_utf8.obj utils\decode_utf8_to_wchar.obj \
	utils\decode_utf8_to_wide_string.obj utils\default_description.obj \
	utils\dup_mb_to_wc.obj utils\dup_wc_to_mb.obj utils\dupcat.obj \
	utils\dupprintf.obj utils\dupstr.obj utils\dupwcs.obj \
	utils\encode_utf8.obj utils\encode_wide_string_as_utf8.obj \
	utils\fgetline.obj utils\format_connection_setup_command.obj \
	utils\host_ca_new_free.obj utils\host_strchr.obj \
	utils\host_strchr_internal.obj utils\host_strcspn.obj \
	utils\host_strduptrim.obj utils\host_strrchr.obj \
	utils\key_components.obj utils\log_proxy_stderr.obj \
	utils\logeventf.obj utils\make_spr_sw_abort_static.obj \
	utils\marshal.obj utils\memory.obj utils\memxor.obj \
	utils\nullstrcmp.obj utils\out_of_memory.obj \
	utils\parse_blocksize.obj utils\percent_decode.obj \
	utils\percent_encode.obj utils\prompts.obj utils\ptrlen.obj \
	utils\seat_connection_fatal.obj utils\seat_dialog_text.obj \
	utils\seat_nonfatal.obj utils\sessprep.obj \
	utils\sk_free_peer_info.obj utils\smemclr.obj utils\smemeq.obj \
	utils\spr_get_error_message.obj utils\ssh2_pick_fingerprint.obj \
	utils\sshutils.obj utils\strbuf.obj \
	utils\string_length_for_printf.obj utils\stripctrl.obj \
	utils\tempseat.obj utils\tree234.obj utils\uint64.obj \
	utils\validate_manual_hostkey.obj utils\version.obj \
	utils\wcwidth.obj utils\wildcard.obj utils\wordwrap.obj \
	utils\write_c_string_literal.obj utils\x11_dehexify.obj \
	utils\x11_identify_auth_proto.obj utils\x11_make_greeting.obj \
	utils\x11_parse_ip.obj utils\x11authnames.obj \
	windows\agent-client.obj windows\commitid.obj \
	windows\handle-io.obj windows\handle-socket.obj \
	windows\handle-wait.obj windows\local-proxy.obj \
	windows\named-pipe-client.obj windows\named-pipe-server.obj \
	windows\network.obj windows\no-jump-list.obj windows\noise.obj \
	windows\select-gui.obj windows\win_sftp_fileio.obj \
	windows\sharing.obj windows\winsftp.obj \
	$(STORAGE_OBJ) windows\unicode.obj \
	windows\utils\agent_named_pipe_name.obj \
	windows\utils\cmdline_arg.obj windows\utils\cryptoapi.obj \
	windows\utils\defaults.obj \
	windows\utils\dll_hijacking_protection.obj \
	windows\utils\escape_registry_key.obj windows\utils\filename.obj \
	windows\utils\fontspec.obj \
	windows\utils\get_system_dir.obj windows\utils\get_username.obj \
	windows\utils\interprocess_mutex.obj \
	windows\utils\is_console_handle.obj windows\utils\ltime.obj \
	windows\utils\load_system32_dll.obj windows\utils\message_box.obj \
	windows\utils\make_spr_sw_abort_winerror.obj \
	windows\utils\open_for_write_would_lose_data.obj \
	windows\utils\platform_get_x_display.obj \
	$(REGISTRY_OBJ) $(SECURITY_OBJ) \
	windows\utils\split_into_argv_w.obj \
	windows\utils\subprocess_waiter.obj windows\utils\version.obj \
	windows\utils\win_strerror.obj x11disp.obj

windows\winsftp.res: windows\winsftp.rc
windows\winsftp.rbj: windows\winsftp.res

winsftp.exe: $(WINSFTP) windows\winsftp.$(RES)
	$(link) $(guiflags) $(ldebug) -out:winsftp.exe @<<
$(WINSFTP) windows\winsftp.$(RES) $(GUI_LIBS)
<<

PSOCKS = \
	callback.obj console.obj errsock.obj logging.obj proxy\http.obj \
	proxy\interactor.obj proxy\local.obj proxy\nocproxy.obj \
	proxy\nosshproxy.obj proxy\proxy.obj proxy\socks4.obj \
	proxy\socks5.obj proxy\telnet.obj psocks.obj ssh\portfwd.obj \
	stubs\no-rand.obj stubs\null-opener.obj stubs\null-plug.obj \
	stubs\null-seat.obj stubs\null-socket.obj timing.obj \
	utils\antispoof.obj utils\base64_encode_atom.obj \
	utils\bufchain.obj utils\burnstr.obj utils\burnwcs.obj \
	utils\conf.obj utils\conf_data.obj utils\conf_dest.obj \
	utils\decode_utf8.obj utils\decode_utf8_to_wchar.obj \
	utils\decode_utf8_to_wide_string.obj utils\dup_mb_to_wc.obj \
	utils\dup_wc_to_mb.obj utils\dupcat.obj utils\dupprintf.obj \
	utils\dupstr.obj utils\dupwcs.obj utils\encode_utf8.obj \
	utils\encode_wide_string_as_utf8.obj \
	utils\format_connection_setup_command.obj utils\host_strchr.obj \
	utils\host_strchr_internal.obj utils\host_strcspn.obj \
	utils\host_strduptrim.obj utils\log_proxy_stderr.obj \
	utils\logeventf.obj utils\make_spr_sw_abort_static.obj \
	utils\marshal.obj utils\memory.obj utils\nullstrcmp.obj \
	utils\out_of_memory.obj utils\prompts.obj utils\ptrlen.obj \
	utils\sk_free_peer_info.obj utils\smemclr.obj \
	utils\spr_get_error_message.obj utils\sshutils.obj \
	utils\strbuf.obj utils\stripctrl.obj utils\tempseat.obj \
	utils\tree234.obj utils\wcwidth.obj utils\wordwrap.obj \
	utils\write_c_string_literal.obj windows\cliloop.obj \
	windows\console.obj windows\handle-io.obj \
	windows\handle-socket.obj windows\handle-wait.obj \
	windows\local-proxy.obj windows\network.obj windows\psocks.obj \
	windows\select-cli.obj windows\select-gui.obj windows\unicode.obj \
	windows\utils\cmdline_arg.obj windows\utils\filename.obj \
	windows\utils\fontspec.obj windows\utils\get_system_dir.obj \
	windows\utils\is_console_handle.obj \
	windows\utils\load_system32_dll.obj windows\utils\ltime.obj \
	windows\utils\make_spr_sw_abort_winerror.obj \
	windows\utils\open_for_write_would_lose_data.obj \
	windows\utils\split_into_argv_w.obj \
	windows\utils\subprocess_waiter.obj windows\utils\version.obj \
	windows\utils\win_strerror.obj

psocks.exe: $(PSOCKS)
	$(link) $(conflags) $(ldebug) -out:psocks.exe @<<
$(PSOCKS) $(CONSOLE_LIBS)
<<

PAGEANT = \
	aqsync.obj callback.obj crypto\aes-common.obj \
	crypto\aes-select.obj crypto\aes-sw.obj crypto\aesgcm-select.obj \
	crypto\aesgcm-sw.obj crypto\argon2.obj crypto\blake2.obj \
	crypto\des.obj crypto\dsa.obj crypto\ecc-arithmetic.obj \
	crypto\ecc-ssh.obj crypto\hash_simple.obj crypto\hmac.obj \
	crypto\mac_simple.obj crypto\md5.obj crypto\mpint.obj \
	crypto\openssh-certs.obj crypto\pubkey-ppk.obj \
	crypto\pubkey-ssh1.obj crypto\rfc6979.obj crypto\rsa.obj \
	crypto\sha1-common.obj crypto\sha1-select.obj crypto\sha1-sw.obj \
	crypto\sha256-common.obj crypto\sha256-select.obj \
	crypto\sha256-sw.obj crypto\sha3.obj crypto\sha512-common.obj \
	crypto\sha512-select.obj crypto\sha512-sw.obj errsock.obj \
	pageant.obj sshpubk.obj stubs\null-cipher.obj stubs\null-key.obj \
	stubs\null-mac.obj stubs\null-plug.obj stubs\null-socket.obj \
	utils\base64_decode_atom.obj utils\base64_encode.obj \
	utils\base64_encode_atom.obj utils\bufchain.obj \
	utils\buildinfo.obj utils\burnstr.obj utils\burnwcs.obj \
	utils\conf.obj utils\conf_data.obj utils\decode_utf8.obj \
	utils\decode_utf8_to_wchar.obj \
	utils\decode_utf8_to_wide_string.obj utils\dup_mb_to_wc.obj \
	utils\dup_wc_to_mb.obj utils\dupcat.obj utils\dupprintf.obj \
	utils\dupstr.obj utils\dupwcs.obj utils\dupwcscat.obj \
	utils\encode_utf8.obj utils\encode_wide_string_as_utf8.obj \
	utils\host_strduptrim.obj utils\key_components.obj \
	utils\log_proxy_stderr.obj utils\marshal.obj utils\memory.obj \
	utils\memxor.obj utils\out_of_memory.obj utils\ptrlen.obj \
	utils\seat_dialog_text.obj utils\sk_free_peer_info.obj \
	utils\smemclr.obj utils\smemeq.obj utils\ssh2_pick_fingerprint.obj \
	utils\strbuf.obj utils\string_length_for_printf.obj \
	utils\base64_decode.obj utils\cert-expr.obj utils\host_ca_new_free.obj utils\percent_decode.obj utils\percent_encode.obj utils\wildcard.obj windows\no-jump-list.obj \
	utils\fgetline.obj utils\tree234.obj utils\uint64.obj utils\version.obj \
	utils\write_c_string_literal.obj windows\agent-client.obj \
	windows\commitid.obj windows\handle-io.obj \
	windows\handle-socket.obj windows\handle-wait.obj \
	windows\named-pipe-client.obj windows\named-pipe-server.obj \
	windows\network.obj windows\pageant.obj windows\select-gui.obj \
	windows\unicode.obj $(STORAGE_OBJ) \
	windows\utils\agent_mutex_name.obj \
	windows\utils\agent_named_pipe_name.obj \
	windows\utils\aux_match_opt.obj windows\utils\cmdline_arg.obj \
	windows\utils\cryptoapi.obj \
	windows\utils\dll_hijacking_protection.obj \
	windows\utils\escape_registry_key.obj windows\utils\filename.obj \
	windows\utils\fontspec.obj windows\utils\get_system_dir.obj \
	windows\utils\get_username.obj \
	windows\utils\getdlgitemtext_alloc.obj \
	windows\utils\interprocess_mutex.obj \
	windows\utils\load_system32_dll.obj \
	windows\utils\makedlgitemborderless.obj \
	windows\utils\message_box.obj \
	windows\utils\pgp_fingerprints_msgbox.obj \
	$(REGISTRY_OBJ) windows\utils\request_file.obj \
	$(SECURITY_OBJ) windows\utils\split_into_argv_w.obj \
	windows\utils\version.obj windows\utils\win_strerror.obj

windows\pageant.res: windows\pageant.rc
windows\pageant.rbj: windows\pageant.res

pageant.exe: $(PAGEANT) windows\pageant.$(RES)
	$(link) $(guiflags) $(ldebug) -out:pageant.exe @<<
$(PAGEANT) windows\pageant.$(RES) $(GUI_LIBS)
<<

PUTTYGEN = \
	crypto\aes-common.obj crypto\aes-select.obj crypto\aes-sw.obj \
	crypto\aesgcm-select.obj crypto\aesgcm-sw.obj crypto\argon2.obj \
	crypto\bcrypt.obj crypto\blake2.obj crypto\blowfish.obj \
	crypto\des.obj crypto\dsa.obj crypto\ecc-arithmetic.obj \
	crypto\ecc-ssh.obj crypto\hash_simple.obj crypto\hmac.obj \
	crypto\mac_simple.obj crypto\md5.obj crypto\mpint.obj \
	crypto\openssh-certs.obj crypto\prng.obj crypto\pubkey-pem.obj \
	crypto\pubkey-ppk.obj crypto\pubkey-ssh1.obj crypto\rfc6979.obj \
	crypto\rsa.obj crypto\sha1-common.obj crypto\sha1-select.obj \
	crypto\sha1-sw.obj crypto\sha256-common.obj \
	crypto\sha256-select.obj crypto\sha256-sw.obj crypto\sha3.obj \
	crypto\sha512-common.obj crypto\sha512-select.obj \
	crypto\sha512-sw.obj import.obj keygen\dsa.obj keygen\ecdsa.obj \
	keygen\millerrabin.obj keygen\mpunsafe.obj keygen\pockle.obj \
	keygen\prime.obj keygen\primecandidate.obj keygen\rsa.obj \
	keygen\smallprimes.obj sshpubk.obj sshrand.obj stubs\no-timing.obj \
	stubs\null-cipher.obj stubs\null-key.obj stubs\null-mac.obj \
	utils\base64_decode.obj utils\base64_decode_atom.obj \
	utils\base64_encode.obj utils\base64_encode_atom.obj \
	utils\bufchain.obj utils\buildinfo.obj utils\burnstr.obj \
	utils\burnwcs.obj utils\cert-expr.obj utils\conf.obj \
	utils\conf_data.obj utils\ctrlset_normalise.obj \
	utils\decode_utf8.obj utils\decode_utf8_to_wchar.obj \
	utils\decode_utf8_to_wide_string.obj utils\dup_mb_to_wc.obj \
	utils\dup_wc_to_mb.obj utils\dupcat.obj utils\dupprintf.obj \
	utils\dupstr.obj utils\dupwcs.obj utils\dupwcscat.obj \
	utils\encode_utf8.obj utils\encode_wide_string_as_utf8.obj \
	utils\host_ca_new_free.obj utils\key_components.obj \
	utils\marshal.obj utils\memory.obj utils\memxor.obj \
	utils\out_of_memory.obj utils\percent_decode.obj \
	utils\percent_encode.obj utils\ptrlen.obj \
	utils\seat_dialog_text.obj utils\smemclr.obj utils\smemeq.obj \
	utils\ssh_key_clone.obj utils\fgetline.obj utils\strbuf.obj \
	utils\string_length_for_printf.obj utils\tree234.obj \
	utils\uint64.obj utils\version.obj utils\wildcard.obj \
	utils\write_c_string_literal.obj windows\commitid.obj \
	windows\controls.obj windows\no-jump-list.obj windows\noise.obj \
	windows\puttygen.obj $(STORAGE_OBJ) windows\unicode.obj \
	windows\utils\aux_match_opt.obj windows\utils\cmdline_arg.obj \
	windows\utils\dll_hijacking_protection.obj \
	windows\utils\escape_registry_key.obj windows\utils\filename.obj \
	windows\utils\fontspec.obj windows\utils\get_system_dir.obj \
	windows\utils\getdlgitemtext_alloc.obj \
	windows\utils\load_system32_dll.obj windows\utils\ltime.obj \
	windows\utils\makedlgitemborderless.obj \
	windows\utils\message_box.obj \
	windows\utils\pgp_fingerprints_msgbox.obj \
	$(REGISTRY_OBJ) windows\utils\request_file.obj \
	$(SECURITY_OBJ) windows\utils\shinydialogbox.obj \
	windows\utils\split_into_argv_w.obj windows\utils\version.obj \
	windows\utils\win_strerror.obj

windows\puttygen.res: windows\puttygen.rc
windows\puttygen.rbj: windows\puttygen.res

puttygen.exe: $(PUTTYGEN) windows\puttygen.$(RES)
	$(link) $(guiflags) $(ldebug) -out:puttygen.exe @<<
$(PUTTYGEN) windows\puttygen.$(RES) $(GUI_LIBS)
<<

clean:
	-del *.exe
	-del *.obj
	-del crypto\*.obj
	-del keygen\*.obj
	-del otherbackends\*.obj
	-del proxy\*.obj
	-del ssh\*.obj
	-del stubs\*.obj
	-del terminal\*.obj
	-del utils\*.obj
	-del windows\*.obj
	-del windows\utils\*.obj
	-del windows\*.res
	-del windows\*.rbj
