# Optimized Haiku Build Script - No shell-init calls
SHELL := /bin/bash

# Optimization & Size Settings
OPT_FLAGS = -O3 -s -ffunction-sections -fdata-sections
LD_OPTIMIZE = -Wl,--gc-sections
HAIKU_FIXES = -include $(PWD)/haiku_fixes.h
HAIKU_LIBS = -lmedia -lbe -ltranslation -lnetwork -lroot -lpthread
EXTRA_LIBS = -lX11 -lsamplerate -lsndfile -lfltk_images -lfltk_forms -lpng -lz


# Lazy evaluation: These will only run when the recipes actually execute
FLTK_CXX = $$(fltk-config --cxxflags)
FLTK_LD  = $$(fltk-config --ldflags)

.PHONY: all config build clean

all: build

# Configure with overrides
config:
	CPPFLAGS="-I$(PWD)" \
	ACONNECT=/bin/true \
	ac_cv_header_alsa_asoundlib_h=yes \
	ac_cv_lib_asound_main=yes \
	ac_cv_lib_samplerate_src_simple=yes \
	ac_cv_lib_sndfile_sf_open=yes \
	ac_cv_lib_jack_main=yes \
	ac_cv_lib_z_main=yes \
	ac_cv_lib_rt_main=yes \
	ac_cv_lib_pthread_main=yes \
	ac_cv_lib_m_main=yes \
	ac_cv_lib_freetype_main=yes \
	ac_cv_lib_fontconfig_main=yes \
	ac_cv_lib_fltk_main=yes \
	ac_cv_lib_dl_main=yes \
	ac_cv_lib_Xft_main=yes \
	ac_cv_lib_Xpm_main=yes \
	ac_cv_lib_Xext_main=yes \
	ac_cv_lib_Xrender_main=yes \
	ac_cv_lib_X11_main=yes \
	./configure --enable-datadir --datadir="$(PWD)/data" --enable-docdir --docdir="$(PWD)/data"

# Build logic
# Define the absolute path to your X11 folder
X11_PATH = $(PWD)/X11
X11_LIB_PATH = $(PWD)/X11

# Add search paths to your flags
# -I$(X11_PATH) finds headers; -L$(X11_PATH) finds libraries
LDFLAGS += -L$(X11_PATH)
CXXFLAGS += -I$(X11_PATH)


# Update your build target to use these flags
build: haiku_stubs.o
	touch configure.in aclocal.m4 Makefile.am Makefile.in configure config.status
	export LIBRARY_PATH="$$LIBRARY_PATH:$(X11_PATH)"; \
	$(MAKE) CXXFLAGS="$(HAIKU_FIXES) $(FLTK_CXX) -I. $(OPT_FLAGS) -fpermissive -I$(X11_PATH)" \
		LIBS="$(FLTK_LD) -L$(X11_PATH) $(EXTRA_LIBS) $(HAIKU_LIBS) $(LD_OPTIMIZE) $(PWD)/haiku_stubs.o"; \
	g++ -o rakarrack src/*.o haiku_stubs.o \
		-I$(X11_PATH) -L$(X11_PATH) \
		-lfltk_images -lfltk $(EXTRA_LIBS) $(HAIKU_LIBS) $(LD_OPTIMIZE) -s; \
		mimeset -f src/rakarrack


haiku_stubs.o: haiku_stubs.cpp
	g++ -c $< -o $@ -I. -I./src $(OPT_FLAGS) -fpermissive
	
	
clean:
	rm -f rakarrack haiku_stubs.o
	$(MAKE) clean
