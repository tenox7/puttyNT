/*
 * Return a string describing everything we know about how this
 * particular binary was built: from what source, for what target
 * platform, using what tools, with what settings, etc.
 */

#include "putty.h"

char *buildinfo(const char *newline)
{
    strbuf *buf = strbuf_new();

    BinarySink_put_fmt(BinarySink_UPCAST(buf), "Build platform: %d-bit %s",
                       (int)(CHAR_BIT * sizeof(void *)), BUILDINFO_PLATFORM);

#ifdef __clang_version__
#define FOUND_COMPILER
    BinarySink_put_fmt(BinarySink_UPCAST(buf), "%sCompiler: clang %s", newline, __clang_version__);
#elif defined __GNUC__ && defined __VERSION__
#define FOUND_COMPILER
    BinarySink_put_fmt(BinarySink_UPCAST(buf), "%sCompiler: gcc %s", newline, __VERSION__);
#endif

#if defined _MSC_VER
#ifndef FOUND_COMPILER
#define FOUND_COMPILER
    BinarySink_put_fmt(BinarySink_UPCAST(buf), "%sCompiler: ", newline);
#else
    BinarySink_put_fmt(BinarySink_UPCAST(buf), ", emulating ");
#endif
#if _MSC_VER >= 1100
    BinarySink_put_fmt(BinarySink_UPCAST(buf), "Visual Studio");
#else
    /* NT port: the compilers of 1993-1996 came before Visual Studio */
    BinarySink_put_fmt(BinarySink_UPCAST(buf), "Microsoft");
#endif

#if 0
    /*
     * List of _MSC_VER values and their translations taken from
     * https://docs.microsoft.com/en-us/cpp/preprocessor/predefined-macros
     *
     * The pointless #if 0 branch containing this comment is there so
     * that every real clause can start with #elif and there's no
     * anomalous first clause. That way the patch looks nicer when you
     * add extra ones.
     *
     * Mostly you can tell the version just from _MSC_VER, but in some
     * cases, two different compiler versions have the same _MSC_VER
     * value, and have to be distinguished by _MSC_FULL_VER.
     */
#elif _MSC_VER == 1933
    BinarySink_put_fmt(BinarySink_UPCAST(buf), " 2022 (17.3)");
#elif _MSC_VER == 1932
    BinarySink_put_fmt(BinarySink_UPCAST(buf), " 2022 (17.2)");
#elif _MSC_VER == 1931
    BinarySink_put_fmt(BinarySink_UPCAST(buf), " 2022 (17.1)");
#elif _MSC_VER == 1930
    BinarySink_put_fmt(BinarySink_UPCAST(buf), " 2022 (17.0)");
#elif _MSC_VER == 1929 && _MSC_FULL_VER >= 192930100
    BinarySink_put_fmt(BinarySink_UPCAST(buf), " 2019 (16.11)");
#elif _MSC_VER == 1929
    BinarySink_put_fmt(BinarySink_UPCAST(buf), " 2019 (16.10)");
#elif _MSC_VER == 1928 && _MSC_FULL_VER >= 192829500
    BinarySink_put_fmt(BinarySink_UPCAST(buf), " 2019 (16.9)");
#elif _MSC_VER == 1928
    BinarySink_put_fmt(BinarySink_UPCAST(buf), " 2019 (16.8)");
#elif _MSC_VER == 1927
    BinarySink_put_fmt(BinarySink_UPCAST(buf), " 2019 (16.7)");
#elif _MSC_VER == 1926
    BinarySink_put_fmt(BinarySink_UPCAST(buf), " 2019 (16.6)");
#elif _MSC_VER == 1925
    BinarySink_put_fmt(BinarySink_UPCAST(buf), " 2019 (16.5)");
#elif _MSC_VER == 1924
    BinarySink_put_fmt(BinarySink_UPCAST(buf), " 2019 (16.4)");
#elif _MSC_VER == 1923
    BinarySink_put_fmt(BinarySink_UPCAST(buf), " 2019 (16.3)");
#elif _MSC_VER == 1922
    BinarySink_put_fmt(BinarySink_UPCAST(buf), " 2019 (16.2)");
#elif _MSC_VER == 1921
    BinarySink_put_fmt(BinarySink_UPCAST(buf), " 2019 (16.1)");
#elif _MSC_VER == 1920
    BinarySink_put_fmt(BinarySink_UPCAST(buf), " 2019 (16.0)");
#elif _MSC_VER == 1916
    BinarySink_put_fmt(BinarySink_UPCAST(buf), " 2017 version 15.9");
#elif _MSC_VER == 1915
    BinarySink_put_fmt(BinarySink_UPCAST(buf), " 2017 version 15.8");
#elif _MSC_VER == 1914
    BinarySink_put_fmt(BinarySink_UPCAST(buf), " 2017 version 15.7");
#elif _MSC_VER == 1913
    BinarySink_put_fmt(BinarySink_UPCAST(buf), " 2017 version 15.6");
#elif _MSC_VER == 1912
    BinarySink_put_fmt(BinarySink_UPCAST(buf), " 2017 version 15.5");
#elif _MSC_VER == 1911
    BinarySink_put_fmt(BinarySink_UPCAST(buf), " 2017 version 15.3");
#elif _MSC_VER == 1910
    BinarySink_put_fmt(BinarySink_UPCAST(buf), " 2017 RTW (15.0)");
#elif _MSC_VER == 1900
    BinarySink_put_fmt(BinarySink_UPCAST(buf), " 2015 (14.0)");
#elif _MSC_VER == 1800
    BinarySink_put_fmt(BinarySink_UPCAST(buf), " 2013 (12.0)");
#elif _MSC_VER == 1700
    BinarySink_put_fmt(BinarySink_UPCAST(buf), " 2012 (11.0)");
#elif _MSC_VER == 1600
    BinarySink_put_fmt(BinarySink_UPCAST(buf), " 2010 (10.0)");
#elif _MSC_VER == 1500
    BinarySink_put_fmt(BinarySink_UPCAST(buf), " 2008 (9.0)");
#elif _MSC_VER == 1400
    BinarySink_put_fmt(BinarySink_UPCAST(buf), " 2005 (8.0)");
#elif _MSC_VER == 1310
    BinarySink_put_fmt(BinarySink_UPCAST(buf), " .NET 2003 (7.1)");
#elif _MSC_VER == 1300
    BinarySink_put_fmt(BinarySink_UPCAST(buf), " .NET 2002 (7.0)");
#elif _MSC_VER == 1200
    BinarySink_put_fmt(BinarySink_UPCAST(buf), " 6.0");
#elif _MSC_VER == 1100
    BinarySink_put_fmt(BinarySink_UPCAST(buf), " 97 (5.0)");
#elif _MSC_VER == 1020
    BinarySink_put_fmt(BinarySink_UPCAST(buf), " Visual C++ 4.2");
#elif _MSC_VER == 1010
    BinarySink_put_fmt(BinarySink_UPCAST(buf), " Visual C++ 4.1");
#elif _MSC_VER == 1000
    BinarySink_put_fmt(BinarySink_UPCAST(buf), " Visual C++ 4.0");
#elif _MSC_VER == 900
    BinarySink_put_fmt(BinarySink_UPCAST(buf), " Visual C++ 2.x");
#elif _MSC_VER == 800
    BinarySink_put_fmt(BinarySink_UPCAST(buf), " C/C++ 8.0");
#else
    BinarySink_put_fmt(BinarySink_UPCAST(buf), ", unrecognised version");
#endif
    BinarySink_put_fmt(BinarySink_UPCAST(buf), ", _MSC_VER=%d", (int)_MSC_VER);
#ifdef _MSC_FULL_VER
    BinarySink_put_fmt(BinarySink_UPCAST(buf), ", _MSC_FULL_VER=%d", (int)_MSC_FULL_VER);
#endif
#endif

#ifdef BUILDINFO_GTK
    {
        char *gtk_buildinfo = buildinfo_gtk_version();
        if (gtk_buildinfo) {
            BinarySink_put_fmt(BinarySink_UPCAST(buf), "%sCompiled against GTK version %s",
                               newline, gtk_buildinfo);
            sfree(gtk_buildinfo);
        }
    }
#endif
#if defined _WINDOWS && defined MINEFIELD
    BinarySink_put_fmt(BinarySink_UPCAST(buf), "%sBuild option: MINEFIELD", newline);
#endif
    BinarySink_put_fmt(BinarySink_UPCAST(buf), "%sBuild option: NO_IPV6", newline);
    BinarySink_put_fmt(BinarySink_UPCAST(buf), "%sBuild option: NO_GSSAPI", newline);
#ifdef WIN32S_COMPAT
    BinarySink_put_fmt(BinarySink_UPCAST(buf), "%sBuild option: WIN32S_COMPAT", newline);
#endif
#ifdef STATIC_GSSAPI
    BinarySink_put_fmt(BinarySink_UPCAST(buf), "%sBuild option: STATIC_GSSAPI", newline);
#endif
#ifdef UNPROTECT
    BinarySink_put_fmt(BinarySink_UPCAST(buf), "%sBuild option: UNPROTECT", newline);
#endif
#ifdef FUZZING
    BinarySink_put_fmt(BinarySink_UPCAST(buf), "%sBuild option: FUZZING", newline);
#endif
#ifdef DEBUG
    BinarySink_put_fmt(BinarySink_UPCAST(buf), "%sBuild option: DEBUG", newline);
#endif

    BinarySink_put_fmt(BinarySink_UPCAST(buf), "%sSource commit: %s", newline, commitid);

    return strbuf_to_str(buf);
}
