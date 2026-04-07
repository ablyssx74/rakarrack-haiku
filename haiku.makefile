# Optimized Haiku Build Script - No shell-init calls
SHELL := /bin/bash

# Directories (User can override these on the command line if needed)
X11_LIB_PATH = $(PWD)/X11

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
	./configure --enable-datadir --datadir="$(PWD)/data"

# Build logic
build: haiku_stubs.o
	export LIBRARY_PATH="$$LIBRARY_PATH:$(X11_LIB_PATH)"; \
	export BELIBRARIES="$$BELIBRARIES:$(X11_LIB_PATH)"; \
	$(MAKE) CXXFLAGS="$(HAIKU_FIXES) $(FLTK_CXX) -I. $(OPT_FLAGS) -fpermissive" \
		LIBS="$(FLTK_LD) $(EXTRA_LIBS) $(HAIKU_LIBS) $(LD_OPTIMIZE) $(PWD)/haiku_stubs.o"; \
	g++ -o rakarrack src/*.o haiku_stubs.o \
		-lfltk_images -lfltk $(EXTRA_LIBS) $(HAIKU_LIBS) $(LD_OPTIMIZE) -s; \
		mimeset -f src/rakarrack


haiku_stubs.o: haiku_stubs.cpp
	g++ -c $< -o $@ -I. -I./src $(OPT_FLAGS) -fpermissive
	
	
clean:
	rm -f rakarrack haiku_stubs.o
	$(MAKE) clean
