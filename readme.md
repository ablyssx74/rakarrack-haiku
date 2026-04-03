### Rakarrack Haiku is a port in progress of the great [rakarrack project](https://rakarrack.sourceforge.net/)
AI was used to help make the port possible

To test: Run ./haiku_configure
```

## Basic hacks right now to get rakarrack built and tested

## Currently creates rakarrack-in rakarrack-out and input0 media nodes
## input0 node currently only hears raw mic input. Doesn't send any data to rakarrack
## You can actually now play your guitar  and hear the audio at the same time!  Some latency though
## rakarrack metronome works but you have to turn on the menu first by clicking the Sw button to the right of
## "Put Order in your Rack", then you have to change to a preset that isn't broken: Preset 4 "Go with Him" is not broken.
##  Then turn on metronome.  You should hear the beeping audio. Volume and speed controls work.


## Some basic Haiku dependencies... maybe missing one or two 
pkgman install fltk_devel xlibe_devel xlibe libsamplerate_devel \
fontconfig_devel libsndfile_devel freetype_devel zlib_devel

## Once rakarrack is built, it is best to open the preferences menu 
## and set the Bank Filename to point to src/data/Default.rkrb
## and set the User Directory to src/data/Default.rkrb


## Files Added/Updated

# configure - commented out aconnect
# src/config.h - Updated config and doc paths
# src/jack.C - Add Haiku code to get audio inputs, comment out jack code, add cleanup function at bottom.  
# jack.h - Add include jack.h linue 29, forward declarted line 31
# rakarrack.cxx Added code at top
# various.C - Added code at top to fix crash when trying to open preferences
# main.C - Added extern at top for clean shutdown and replace while loop at bottom for clean shutdown
# alsa/asoundlib.h, X11/xpm.h, jack/jack.h midiport.h transport.h  - Added to satisfy build
# haiku_stubs.cpp - Added to satisfy build
# Symlinked /boot/system/develop/headers/FL ./Fl - For local build
# haiku_configure - This file. To help build and other details

## Make will generate a bunch of lib errors, but this can be ignored
## The g++ code after the make will compile the stubs and rakarrack in src directory

# To Test
cd src
./rakarrack

```
