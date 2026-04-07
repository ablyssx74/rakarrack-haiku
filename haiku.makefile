# Optimized Haiku Build Script
SHELL := /bin/bash

# --- CPU Feature Detection ---

CPU_FEATURES := $(shell sysinfo -cpu)

# Default values if not specified on the command line
FRAMES ?= 16
RATE   ?= 48000.0


ifneq ($(findstring AVX512,$(CPU_FEATURES)),)
    SIMD_FLAGS := -O3 -march=skylake-avx512 -ffp-contract=fast
# AMD Ryzen Detection (Zen 1, 2, 3)
else ifneq ($(findstring znver,$(CPU_FEATURES)),)
    SIMD_FLAGS := -O3 -march=znver2 -ffp-contract=fast
# Your Xeon (Haswell/Broadwell)
else ifneq ($(findstring AVX2,$(CPU_FEATURES)),)
    SIMD_FLAGS := -O3 -march=haswell -ffp-contract=fast
# Sandy/Ivy Bridge
else ifneq ($(findstring AVX,$(CPU_FEATURES)),)
    SIMD_FLAGS := -O3 -march=sandybridge
# Nehalem / Westmere
else ifneq ($(findstring SSE4.2,$(CPU_FEATURES)),)
    SIMD_FLAGS := -O3 -march=nehalem
# Pentium 4 / Early Core2 (SSE3)
else ifneq ($(findstring SSE3,$(CPU_FEATURES)),)
    SIMD_FLAGS := -O3 -march=prescott
# Fallback: Automatic detection or generic 64-bit
else
    SIMD_FLAGS := -O3 -march=native
endif



# Optimization & Size Settings
# We use SIMD_FLAGS instead of a generic -O3
BUILD_FLAGS = $(SIMD_FLAGS) -ffast-math -ffunction-sections -fdata-sections -s
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
	./configure --enable-datadir --datadir="$(PWD)/data" --enable-docdir --docdir="$(PWD)/doc/" --with-frame-rate=$(RATE) --with-buffer-frames="$(FRAMES)"
	

build: haiku_stubs.o
	@echo "=========================================================="
	@echo " Building Rakarrack for Haiku [$(FRAMES) frames @ $(RATE)Hz]"
	@echo " SIMD Level: $(shell echo $(SIMD_FLAGS) | grep -o 'march=[^ ]*')"
	@echo "=========================================================="
	touch configure.in aclocal.m4 Makefile.am Makefile.in configure config.status
	$(MAKE) -j$(nproc) \
		CXXFLAGS="-include $(PWD)/jack/jack.h $(HAIKU_FIXES) $(FLTK_CXX) $(BUILD_FLAGS) -fpermissive -I. -I$(PWD)/jack" \
		LIBS="$(FLTK_LD) $(EXTRA_LIBS) $(HAIKU_LIBS) $(LD_OPTIMIZE) $(PWD)/haiku_stubs.o"
	g++ -o rakarrack src/*.o haiku_stubs.o \
		$(BUILD_FLAGS) \
		-include $(PWD)/jack/jack.h \
		-lfltk_images -lfltk $(EXTRA_LIBS) $(HAIKU_LIBS) $(LD_OPTIMIZE)
	mimeset -f src/rakarrack

haiku_stubs.o: haiku_stubs.cpp
	g++ -c $< -o $@ -I$(PWD)/jack -I. -I./src $(BUILD_FLAGS) -fpermissive


clean:
	@echo "Performing deep clean (distclean)..."
	# Remove the binaries and stubs
	rm -f rakarrack haiku_stubs.o
	#rm -f aclocal.m4 configure
	# Call the internal Makefile's distclean if it exists
	-[ -f Makefile ] && $(MAKE) distclean
	# Clean up leftover Autotools files and caches
	rm -rf autom4te.cache config.cache configure~ config.log config.status Makefile src/Makefile \
	       man/Makefile data/Makefile icons/Makefile doc/Makefile \
	       doc/help/Makefile doc/help/imagenes/Makefile doc/help/css/Makefile extra/Makefile
	autoreconf -vif
	@echo "Deep clean complete."

