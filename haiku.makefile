#----------------------------------------------------------
# Optimized Haiku Build Script
SHELL := /bin/bash
#----------------------------------------------------------
      

UNAME_M := $(shell uname -p)
ifeq ($(UNAME_M), x86)
CXX = g++-x86
CC = gcc-x86
LDFLAGS = -L/boot/system/develop/lib/x86 -L/boot/system/lib/x86 
CPPFLAGS = -I/boot/system/develop/headers/x86 -I$(PWD)
PackageInfo = PackageInfo32.tpl
MAKE := setarch x86 $(MAKE)
REQUIRED_PKGS =	fltk_x86 freetype_x86 libxfont2_x86 libsndfile_x86 libsamplerate_x86 libxpm_x86 
else ifeq ($(UNAME_M), x86_64)
CXX = g++
CC = gcc
LDFLAGS = -L/boot/system/develop/lib/ 
CPPFLAGS = -I$(PWD)
PackageInfo = PackageInfo64.tpl
REQUIRED_PKGS = fltk_devel fontconfig_devel freetype_devel libxfont2_devel libsndfile_devel libsamplerate_devel libxpm_devel
endif

#LDFLAGS="-L/boot/system/develop/lib/x86 -L/boot/system/lib/x86" \
#CPPFLAGS="-I/boot/system/develop/headers/x86 -I$(PWD)" \

      

#----------------------------------------------------------
# Default values if not specified on the command line
# E.g., custom build: 
# make -f haiku.makefile clean && make -f haiku.makefile config FRAMES=64 SIMD_FLAGS="-O3 -march=native && make -f haiku.makefile"
# To make rakarrack.hpkg
# haiku.makefile package
#----------------------------------------------------------
FRAMES ?= 2048
RATE   ?= 48000
SIMD_FLAGS ?= -O3 -mtune=generic -msse2 # Public Build Default
	
#----------------------------------------------------------
# CPU Features - Select native if building for personal use and maximum local speed	
# SIMD_FLAGS :=  -O3 -march=native
#----------------------------------------------------------


#----------------------------------------------------------
# Optimization & Size Settings
# We use SIMD_FLAGS instead of a generic -O3
#----------------------------------------------------------
BUILD_FLAGS = $(SIMD_FLAGS) -ffast-math -ffunction-sections -fdata-sections -s
LD_OPTIMIZE = -Wl,--gc-sections
HAIKU_FIXES = -include $(PWD)/haiku_fixes.h
HAIKU_LIBS = -lmedia -lbe -lmidi2 -ltranslation -lnetwork -lroot -lpthread
EXTRA_LIBS = -lsamplerate -lsndfile -lfltk_images -lfltk -lfltk_forms -lpng -lz

#----------------------------------------------------------
# Not used here but saving anyway
#----------------------------------------------------------
HAIKU_INC = -I/boot/system/develop/headers/os -I/boot/system/develop/headers/posix
HAIKU_LIB = -L/boot/system/develop/lib 

#----------------------------------------------------------
# Lazy evaluation: These will only run when the recipes actually execute
#----------------------------------------------------------
FLTK_CXX = $$(fltk-config --cxxflags)
FLTK_LD  = $$(fltk-config --ldflags)

.PHONY: all config build clean help deps


all: build

#----------------------------------------------------------
# Configure with overrides
#----------------------------------------------------------
config:
	./configure \
	LDFLAGS="$(LDFLAGS)" \
	CPPFLAGS="$(CPPFLAGS)" \
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
	--enable-datadir --datadir="/boot/system/data/rakarrack/share/rakarrack" \
	--enable-docdir --docdir="/boot/system/data/rakarrack/share/doc/rakarrack/html" \
	--with-frame-rate=$(RATE) \
	--with-buffer-frames="$(FRAMES)"
	
haiku_native/haiku-rakarrack.o: haiku_native/haiku-rakarrack.cpp
	$(CXX) -c $< -o $@ -I$(PWD)/jack -I. -I./src $(BUILD_FLAGS) -fpermissive $(HAIKU_FIXES)

build: haiku_stubs.o haiku_native/haiku-rakarrack.o
	@echo "=========================================================="
	@echo "      Building Rakarrack for Haiku $(SIMD_FLAGS)"
	@echo "=========================================================="
	touch configure.in aclocal.m4 Makefile.am Makefile.in configure config.status
	$(MAKE) -j4 \
		CXXFLAGS="-include $(PWD)/jack/jack.h $(HAIKU_FIXES) $(FLTK_CXX) $(BUILD_FLAGS) -fpermissive -I. -I$(PWD)/jack" \
		LIBS="$(PWD)/haiku_native/haiku-rakarrack.o $(FLTK_LD) $(EXTRA_LIBS) $(HAIKU_LIBS) $(LD_OPTIMIZE) $(PWD)/haiku_stubs.o -Wno-int-to-pointer-cast -Wno-write-strings"
	
	# The final link now combines everything correctly
	$(CXX) -o rakarrack src/*.o haiku_stubs.o haiku_native/haiku-rakarrack.o \
		$(BUILD_FLAGS) \
		-include $(PWD)/jack/jack.h \
		$(EXTRA_LIBS) $(HAIKU_LIBS) $(LD_OPTIMIZE)
	mimeset -f rakarrack


haiku_stubs.o: haiku_stubs.cpp
	$(CXX) -c $< -o $@ -I$(PWD)/jack -I. -I./src $(BUILD_FLAGS) -fpermissive


clean:
	@echo "Performing deep clean (distclean)..."
	rm -f rakarrack haiku_stubs.o
	rm -f $(PWD)/haiku_native/*.o
	@if [ -f Makefile ]; then $(MAKE) distclean; fi
	rm -rf *.hpkg build autom4te.cache config.cache config.log config.status Makefile src/Makefile \
	       man/Makefile data/Makefile icons/Makefile doc/Makefile \
	       doc/help/Makefile doc/help/imagenes/Makefile doc/help/css/Makefile extra/Makefile
	autoreconf -vif
	@echo "Deep clean complete."
	
# Small hack since 32bit Haiku refuses to install packages without _gcc2 appendix.
UNAME_M := $(shell uname -p)
ifeq ($(UNAME_M), x86)
    ARCH = x86_gcc2
else ifeq ($(UNAME_M), x86_64)
    ARCH = x86_64
endif    


PACKAGE_DIR := build/package
NAME = rakarrack
VERSION = 0.6.1

package: all
	@[ -n "$(PACKAGE_DIR)" ] || { echo "PACKAGE_DIR is undefined"; exit 1; }
	rm -rf "./$(PACKAGE_DIR)"
	mkdir -p $(PACKAGE_DIR)
	sed -e 's/$$(NAME)/$(NAME)/g' -e 's/$$(VERSION)/$(VERSION)/g' -e 's/$$(ARCH)/$(ARCH)/' -e 's/$$(YEAR)/$(shell date +%Y)/' $(PackageInfo) > $(PACKAGE_DIR)/.PackageInfo
	mkdir -p $(PACKAGE_DIR)/apps
	mkdir -p $(PACKAGE_DIR)/bin
	mkdir -p $(PACKAGE_DIR)/data/deskbar/menu/Applications
	mkdir -p $(PACKAGE_DIR)/data/$(NAME)/share/doc/$(NAME)
	mkdir -p $(PACKAGE_DIR)/data/$(NAME)/share/man
	#mkdir -p $(PACKAGE_DIR)/data/$(NAME)/share/pixmaps
	mkdir -p $(PACKAGE_DIR)/data/$(NAME)/share/man/man1
	mkdir -p $(PACKAGE_DIR)/data/$(NAME)/share/$(NAME)
	xres -o $(NAME) icon.rsrc  
	mimeset -f $(NAME)
	cp man/$(NAME).1 $(PACKAGE_DIR)/data/$(NAME)/share/man/man1
	#cp icons/*.png $(PACKAGE_DIR)/data/$(NAME)/share/pixmaps
	cp data/*.{rvb,dly,rkrb,wav} $(PACKAGE_DIR)/data/$(NAME)/share/$(NAME)
	cp -r doc/help $(PACKAGE_DIR)/data/$(NAME)/share/doc/$(NAME)/html
	cp -r AUTHORS $(PACKAGE_DIR)/data/$(NAME)/share/doc/$(NAME)/
	cp  COPYING $(PACKAGE_DIR)/data/$(NAME)/share/doc/$(NAME)/
	cp  ChangeLog $(PACKAGE_DIR)/data/$(NAME)/share/doc/$(NAME)/
	cp  NEWS $(PACKAGE_DIR)/data/$(NAME)/share/doc/$(NAME)/
	cp $(NAME) $(PACKAGE_DIR)/apps/$(NAME)
	ln -s ../apps/$(NAME) $(PACKAGE_DIR)/bin/rakarrack
	ln -s ../../../../apps/$(NAME) $(PACKAGE_DIR)/data/deskbar/menu/Applications/Rakarrack
	package create -C $(PACKAGE_DIR) $(NAME)-$(VERSION)-1-$(ARCH).hpkg
	
	
#----------------------------------------------------------
# Required packages- Informational purposes 
#----------------------------------------------------------

         
deps:
	@echo "Install these via pkgman to build source:"
	@echo "pkgman install $(REQUIRED_PKGS)"     	

#----------------------------------------------------------	
# Help
#----------------------------------------------------------
help:
	@echo "============================================================================"
	@echo " Building Rakarrack for Haiku 64bit"
	@echo ""
	@echo ""
	@echo " 1. Default Build: make -f haiku.makefile config"
	@echo ""
	@echo " 2. Custom Build:"
	@echo "     make -fhaiku.makefile clean "
	@echo "     make -f haiku.makefile config"
	@echo "     make -f haiku.makefile build SIMD_FLAGS=\"-O3 -march=native\""
	@echo ""
	@echo " 3. Build: make -f haiku.makefile"
	@echo ""
	@echo " 4. Package: make -f haiku.makefile package"
	@echo ""
	@echo " 5. Custom Package: make -f haiku.makefile package SIMD_FLAGS=\"-O3 -march=native\""
	@echo ""
	@echo " 6. Clean: make -f haiku.makefile clean"
	@echo ""
	@echo " 7. List Required Libs: make -f haiku.makefile deps"
	@echo ""
	@echo "============================================================================"	
