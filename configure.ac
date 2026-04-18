AC_PREREQ(2.61)
AC_INIT([rakarrack],[0.6.1],[Josep Andreu holborn@telefonica.net]) 
AM_INIT_AUTOMAKE
AC_CONFIG_SRCDIR([src/main.C])
AC_CONFIG_HEADER([src/config.h])

AC_ARG_ENABLE(optimizations,
AC_HELP_STRING([--enable-optimizations[=yes/no]],[enable optimizations (default=yes)]),
[ac_optimizations="$enableval"], [ac_optimizations="yes"])

if test "x$ac_optimizations" = "xyes"; then
OPTS=""
else
OPTS="no"
fi


AC_ARG_ENABLE(sse,
AC_HELP_STRING([--enable-see],[enable sse (default=automatic check)]),
[ac_sse="$enableval"])

if test "x$ac_sse" = "xyes"; then
SSE="-msse -mfpmath=sse"
else
ac_sse="no"
fi

AC_ARG_ENABLE(sse2,
AC_HELP_STRING([--enable-see2],[enable sse2 (default=automatic check)]),
[ac_sse2="$enableval"])

if test "x$ac_sse2" = "xyes"; then
SSE="-msse2 -mfpmath=sse"
else 
ac_sse2="no"
fi

AC_ARG_ENABLE(altivec,
AC_HELP_STRING([--enable-altivec],[enable altivec (default=automatic check)]),
[ac_altivec="$enableval"])

if test "x$ac_altivec" = "xyes"; then
ALTIVEC="-maltivec"
else
ac_altivec="no"
fi


# Checks for programs.
AC_PROG_CXX
AC_PROG_CC
AC_PROG_MAKE_SET
AC_CHECK_PROG([FLTKCONFIG], [fltk-config], [fltk-config])
if test -z "$FLTKCONFIG"; then
  AC_MSG_ERROR([fltk not installed!])
fi
AC_CHECK_PROG([ACONNECT],[aconnect],[aconnect])
if test -z "$ACONNECT"; then
  AC_MSG_ERROR([aconnect not installed!])
fi

# Checks gcc version

GCC_VERSION=`$CC -dumpversion | cut -f 2 -d "."`
if test "$GCC_VERSION" = "5"; then
ADDS="-fvar-tracking-assignments-toggle -fexcess-precision=fast"
else
ADDS=""
fi

# Checks for typedefs, structures, and compiler characteristics.
AC_HEADER_STDBOOL
AC_C_CONST
AC_C_INLINE
AC_TYPE_SIZE_T

# Checks for library functions.
AC_PROG_GCC_TRADITIONAL
AC_FUNC_CLOSEDIR_VOID
AC_FUNC_MALLOC
AC_FUNC_SELECT_ARGTYPES
AC_CHECK_FUNCS([floor memset memmove pow select sqrt strdup strstr])


AC_CHECK_LIB([network], [main])
case "$host_os" in
    *haiku*)
        case "$host_cpu" in
            x86_64)
                if test -d "/boot/system/develop/lib"; then
                    LDFLAGS="$LDFLAGS -L/boot/system/develop/lib -L/boot/system/lib"
                    CPPFLAGS="$CPPFLAGS -I/boot/system/develop/headers -I/boot/system/develop/headers/X11"
                fi
                ;;
            i?86)
                # 32-bit (x86) logic
                if test -d "/boot/system/develop/lib/x86"; then
                    LDFLAGS="$LDFLAGS -L/boot/system/develop/lib/x86 -L/boot/system/lib/x86"
                    CPPFLAGS="$CPPFLAGS -I/boot/system/develop/headers/x86 -I/boot/system/develop/headers/x86/X11"
                fi
                ;;
        esac
        ;;
esac




# Checks for libraries.

AC_CHECK_LIB([X11], [XOpenDisplay],[ac_X11="yes"],[ac_X11="no"])
if test "x$ac_X11" = "xno"; then
AC_MSG_ERROR([Could not find XOpenDisplay in -lX11.])
else
   LIBS="$LIBS -lX11"
fi


AC_CHECK_LIB([Xft], [main], [ac_libXft="yes"], [ac_libXft="no"])
if test "x$ac_libXft" = "xno"; then
AC_MSG_ERROR([Xft library not found.])
else 
   LIBS="$LIBS -lXft"
fi

AC_CHECK_LIB([Xrender], [main], [ac_Xrender="yes"], [ac_Xrender="no"])
if test "x$ac_Xrender" = "xno"; then
AC_MSG_ERROR([Xrender library not found.])
else 
   LIBS="$LIBS -lXrender"

fi


AC_CHECK_LIB([dl], [main], [ac_dl="yes"], [ac_dl="no"])
if test "x$ac_dl" = "xno"; then
AC_MSG_ERROR([dl library not found.])
else 
   LIBS="$LIBS -ldl"

fi

AC_CHECK_LIB([fltk], [main], [ac_fltk="yes"], [ac_fltk="no"])
if test "x$ac_fltk" = "xno"; then
AC_MSG_ERROR([FLTK library not found.])
fi

AC_CHECK_LIB([Xpm],[main], [ac_Xpm="yes"], [ac_Xpm="no"])
if test "x$ac_Xpm" = "xno"; then
AC_MSG_ERROR([Xpm library not found.])
else 
   LIBS="$LIBS -lXpm"

fi

AC_CHECK_LIB([fontconfig], [main], [ac_fontconfig="yes"], [ac_fontconfig="no"])
if test "x$ac_fontconfig" = "xno"; then
AC_MSG_ERROR([fontconfig library not found.])
else 
   LIBS="$LIBS -lfontconfig"

fi

AC_CHECK_LIB([freetype], [main], [ac_freetype="yes"], [ac_freetype="no"])
if test "x$ac_freetype" = "xno"; then
AC_MSG_ERROR([freetype library not found.])
else 
   LIBS="$LIBS -lfreetype"

fi

AC_CHECK_LIB([m], [main], [ac_m="yes"], [ac_m="no"])
if test "x$ac_m" = "xno"; then
AC_MSG_ERROR([Math library not found.])
else 
   LIBS="$LIBS -lm"

fi

AC_CHECK_LIB([pthread], [main], [ac_pthread="yes"], [ac_pthread="no"] )
if test "x$ac_pthread" = "xno"; then
AC_MSG_ERROR([pthread library not found.])
else 
   LIBS="$LIBS -lpthread"
fi

AC_CHECK_LIB([rt], [main], [ac_rt="yes"], [ac_rt="no"])
if test "x$ac_rt" = "xno"; then
AC_MSG_ERROR([rt library not found.])
else 
   LIBS="$LIBS -lrt"

fi

AC_CHECK_LIB([z], [main], [ac_z="yes"], [ac_zm="no"])
if test "x$ac_z" = "xno"; then
AC_MSG_ERROR([z library not found.])
else 
   LIBS="$LIBS -lz"

fi

AC_CHECK_LIB([asound], [main], [ac_asound="yes"], [ac_asound="no"])
if test "x$ac_asound" = "xno"; then
AC_MSG_ERROR([ALSA libasound library not found.])
else 
   LIBS="$LIBS -lasound"

fi

AC_CHECK_LIB([jack],main, [ac_jack="yes"], [ac_jack="no"])
if test "x$ac_jack" = "xno"; then
AC_MSG_ERROR([JACK library not found.])
else 
   LIBS="$LIBS -ljack"

fi

AC_CHECK_LIB([sndfile],sf_open, [ac_sndfile="yes"], [ac_sndfile="no"])
if test "x$ac_sndfile" = "xno"; then
AC_MSG_ERROR([sndfile library not found.])
else 
   LIBS="$LIBS -lsndfile"

fi

AC_CHECK_LIB([samplerate],src_simple, [ac_samplerate="yes"], [ac_samplerate="no"])
if test "x$ac_samplerate" = "xno"; then
AC_MSG_ERROR([samplerate library not found.])
else 
   LIBS="$LIBS -lsamplerate"

fi

if test "x$prefix" = "xNONE"; then
AC_DEFINE_UNQUOTED(PREFIX, "/boot/home/rakarrack", [Install prefix])
else
AC_DEFINE_UNQUOTED(PREFIX, "$prefix", [Install prefix])
fi
AC_HEADER_DIRENT
AC_HEADER_STDC
AC_CHECK_HEADERS([fcntl.h libintl.h stdlib.h string.h sys/ioctl.h unistd.h sndfile.h samplerate.h X11/xpm.h])
AC_CHECK_HEADER(alsa/asoundlib.h, true, AC_MSG_ERROR([Alsa is required]))
AC_SUBST(SAMPLERATE_CFLAG)
AC_SUBST(SAMPLERATE_LIBS)


if test "x$prefix" = "xNONE"; then
prefix=/boot/home/rakarrack
fi



AC_ARG_ENABLE(datadir,
AC_HELP_STRING([--enable-datadir[=yes/no]],[use datadir (default=no)]),
[ac_dd="$enableval"], [ac_dd="no"])

if test "x$ac_dd" = "xyes"; then
DATADIR="$datadir"
else
DATADIR="$prefix/share/${PACKAGE}"
fi


AC_ARG_ENABLE(docdir,
AC_HELP_STRING([--enable-docdir[=yes/no]],[use docdir (default=no)]),
[ac_hd="$enableval"], [ac_hd="no"])

if test "x$ac_hd" = "xyes"; then
HELPDIR="$docdir"
else
HELPDIR="$prefix/share/doc/${PACKAGE}"
fi

AC_DEFINE_UNQUOTED(DATADIR,"$DATADIR",[DATADIR])
AC_DEFINE_UNQUOTED(HELPDIR,"$HELPDIR",[HELPDIR])
AC_DEFINE([WEBSITE],["rakarrack.sf.net"],[WEBSITE])

# Important Note on Audio Latency
# While -O3 is standard for Rakarrack, sometimes high optimization levels can 
# increase binary size or cause slight jitters in real-time audio threads. 
# If you notice "crackling" that wasn't there before, try -O2 -march=haswell 
# to see if it stabilizes.

OS_NAME="$(uname -s)"

# 1. CPU Feature Detection (SSE/AVX)
# We check if $SSE is already set to avoid overwriting user-provided flags.
if test -z "$SSE"; then
    if test "$OS_NAME" = "Haiku"; then
        CPU_FEATURES=$(sysinfo -cpu)
        if echo "$CPU_FEATURES" | grep -qi "avx512"; then
            SSE="-O3 -march=skylake-avx512 -ffp-contract=fast"
            ac_sse="yes (AVX-512)"
        elif echo "$CPU_FEATURES" | grep -qi "avx2"; then
            SSE="-O3 -march=haswell -ffp-contract=fast"
            ac_sse="yes (AVX2)"
        elif echo "$CPU_FEATURES" | grep -qi "avx"; then
            SSE="-O3 -march=sandybridge"
            ac_sse="yes (AVX)"
        elif echo "$CPU_FEATURES" | grep -qi "sse4.2"; then
            SSE="-O3 -march=nehalem -mfpmath=sse"
            ac_sse="yes (SSE4.2)"
        elif echo "$CPU_FEATURES" | grep -qi "sse2"; then
            SSE="-O3 -msse2 -mfpmath=sse"
            ac_sse="yes (SSE2)"
        else
            SSE="-O3"
            ac_sse="no"
        fi
    else
        # Fallback for Linux or other OS types
        if grep -q "sse2" /proc/cpuinfo 2>/dev/null; then
            SSE="-O3 -msse2 -mfpmath=sse"
            ac_sse="yes"
        else
            SSE="-O3"
            ac_sse="no"
        fi
    fi
fi

# 2. AltiVec Detection (PowerPC)
if test -z "$ALTIVEC"; then
    if test "$OS_NAME" = "Haiku"; then
        # Check if sysinfo returns any altivec string
        HAVE_ALTIVEC=$(sysinfo -cpu | grep -i "altivec")
    else
        HAVE_ALTIVEC=$(cat /proc/cpuinfo 2>/dev/null | grep -i altivec)
    fi
    
    if test -z "$HAVE_ALTIVEC"; then
        ALTIVEC=""
        ac_altivec="no"
    else
        ALTIVEC="-maltivec"
        ac_altivec="yes"
    fi
fi

dnl ===========================================================================
dnl Custom Argument: Frame Rate
dnl Usage: ./configure --with-frame-rate=44100.0
dnl ===========================================================================
AC_ARG_WITH([frame-rate],
    [AS_HELP_STRING([--with-frame-rate=RATE], 
    [Set the default audio frame rate (default: 48000.0)])],
    [frame_rate="$withval"],
    [frame_rate="48000.0"])

dnl Define the value in config.h (UNQUOTED lets you pass numbers/formulas)
AC_DEFINE_UNQUOTED([DEFAULT_FRAME_RATE], [$frame_rate], [Default audio frame rate])


dnl ===========================================================================
dnl Custom Argument: Buffer Frame Count (default: 128)
dnl Usage: ./configure --with-buffer-frames="128"
dnl ===========================================================================
AC_ARG_WITH([buffer-frames],
    [AS_HELP_STRING([--with-buffer-frames=N], 
    [Set the default audio buffer frame count (default: 128)])],
    [buffer_frames="$withval"],
    [buffer_frames="128"])


AC_DEFINE_UNQUOTED([DEFAULT_BUFFER_FRAMES], [$buffer_frames], [Default buffer frame count])




# 3. Final Flag Assembly
LIBS="`$FLTKCONFIG --use-images --ldflags`"
#LIBS="`$FLTKCONFIG --use-images --ldflags` $LIBS"
CXXFLAGS="-Wall $SSE $ALTIVEC -ffast-math -pipe $ADDS -fsigned-char `$FLTKCONFIG --use-images --cxxflags` `pkg-config --cflags sndfile` `pkg-config --cflags jack` $CXXFLAGS"

AC_CONFIG_FILES([Makefile src/Makefile man/Makefile data/Makefile icons/Makefile doc/Makefile doc/help/Makefile doc/help/imagenes/Makefile doc/help/css/Makefile extra/Makefile])
AC_OUTPUT


haiku_rakarrack_msg="Bypassed"

echo
echo  " $PACKAGE_NAME $PACKAGE_VERSION"
echo
echo
echo  " Optimizations__________________________________________________"
echo  " Use altivec. . . . . . . . . . . . . . . . . . . . . . .: $ac_altivec"
echo  " Use sse  . . . . . . . . . . . . . . . . . . . . . . . .: $ac_sse"
echo  " High-Performance SIMD  . . . . . . . . . . . . . . . . .: $ac_sse"

echo  
echo  " Library________________________________________________________"
echo  " X11 support  . . . . . . . . . . . . . . . . . . . . . .: $haiku_rakarrack_msg"
echo  " Xpm support (libxpm) . . . . . . . . . . . . . . . . . .: $ac_Xpm"
echo  " Xft support  . . . . . . . . . . . . . . . . . . . . . .: $ac_libXft"
echo  " Xrender support  . . . . . . . . . . . . . . . . . . . .: $haiku_rakarrack_msg"
echo  " Fontconfig support . . . . . . . . . . . . . . . . . . .: $ac_fontconfig"
echo  " Freetype support . . . . . . . . . . . . . . . . . . . .: $ac_freetype"
echo  " FLTK toolkit . . . . . . . . . . . . . . . . . . . . . .: $ac_fltk"
echo  " pthread support (libpthread) . . . . . . . . . . . . . .: $ac_pthread"
echo  " JACK Audio Connection Kit library  . . . . . . . . . . .: $haiku_rakarrack_msg"
echo  " ALSA MIDI Sequencer support  . . . . . . . . . . . . . .: $haiku_rakarrack_msg"
echo  " Samplerate conversion (libsamplerate). . . . . . . . . .: $ac_samplerate"
echo  " General audio file (libsndfile)  . . . . . . . . . . . .: $ac_sndfile"
echo  " Buffer Frame Count ( can be changed later in the UI ). .: $buffer_frames"
echo  " Frame Rate . . . . ( can be changed later in the UI ). .: $frame_rate" 

echo  
echo  " Now you can run 'make -f haiku.makefile build' to build the sources."
echo  " For custom build run 'make -f haiku.makefile help' to see how."
echo
echo

