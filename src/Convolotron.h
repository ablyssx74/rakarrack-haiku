/*
  rakarrack - a guitar efects software

  jack.C  -   jack I/O
  Copyright (C) 2008-2010 Josep Andreu
  Author: Josep Andreu

  This program is free software; you can redistribute it and/or modify
  it under the terms of version 2 of the GNU General Public License
  as published by the Free Software Foundation.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License (version 2) for more details.

  You should have received a copy of the GNU General Public License
(version2)
  along with this program; if not, write to the Free Software Foundation,
  Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
  
  
  Updated by Kris Beazley aka ablyss for Haiku OS with the help of AI
  Copyright 2026
*/

#ifndef CONVOLOTRON_H
#define CONVOLOTRON_H

#include <sndfile.h>
#include "global.h"
#include "Resample.h"

class Convolotron
{
public:
  Convolotron (float * efxoutl_, float * efxoutr_,int DS, int uq, int dq);
  ~Convolotron ();
  void out (float * smpsl, float * smpr);
  void setpreset (int npreset);
  void changepar (int npar, int value);
  int getpar (int npar);
  void cleanup ();
  int setfile (int value);
  void adjust(int DS);
  void loaddefault();

  // Two-phase preset apply, added for native (Haiku) mode: setpreset()
  // above loads its IR file (via setfile() -> process_rbuf()) as part of
  // one synchronous call, which native mode was invoking with the
  // real-time audio callback's lock held -- fine for the other 10 cheap
  // int parameters a preset sets, but the disk read (and, if the file's
  // sample rate differs, a resample pass) could stall that lock for far
  // longer than the callback can tolerate, flooding the audio backend
  // with buffer underruns. setpresetPrefetch() below does that same read
  // into a private scratch buffer that out()/Alg() never touch, so it is
  // safe to call without holding any lock; setpresetCommit() then applies
  // everything -- the prefetched IR plus the other 10 parameters -- and
  // must be called with that lock held, same as any other changepar().
  // setpreset() itself is untouched and still used as-is elsewhere.
  void setpresetPrefetch (int npreset);
  void setpresetCommit ();

  // The same split, exposed directly for native mode's standalone "IR"
  // dropdown (a plain changepar(8, ...) outside of any preset) -- see
  // haiku_native/haiku-rakarrack.cpp's Convolotron box for how these two
  // are paired around an unlock/relock the same way.
  void prefetchIR (int value);
  void commitIR ();

  // A third way into the same problem: fileio.C's loadfile() (native
  // mode's "Load Preset") restores every effect through its own
  // changepar() loop -- for Convolotron that still means case 8 below
  // calls the slow, undivided setfile() same as ever, and loadfile()'s
  // caller (RakarrackWindow::HandleRefsReceived) holds jmutex around the
  // *entire* loadfile() call, same reasoning as SliderW/setpreset() above.
  // Rather than teach shared engine code (fileio.C, linked into the FLTK
  // build too) about a Haiku-only mutex, SetSuppressFileLoad(true) makes
  // changepar(8, ...) just remember the value instead of acting on it;
  // the caller then fetches it with TakeSuppressedFileValue() once
  // loadfile() returns and drives prefetchIR()/commitIR() itself, exactly
  // like the Preset/IR dropdowns do. Leaving this off (the default) makes
  // changepar(8, ...) behave exactly as it always has, so no other caller
  // -- FLTK's own loadfile(), setpreset(), the plain "IR" dropdown -- is
  // affected unless it explicitly opts in.
  void SetSuppressFileLoad (bool suppress) { fSuppressFileLoad = suppress; }
  bool TakeSuppressedFileValue (int *outValue);

  int Ppreset;

  float *efxoutl;
  float *efxoutr;
  float outvolume;

  char Filename[128];


private:
  //Parametrii
  int Pvolume;	//This is master wet/dry mix like other FX...but I am finding it is not useful
  int Ppanning;	//Panning
  int Plrcross;	// L/R Mixing  // This is a mono effect, so lrcross and panning are pointless
  int Phidamp;
  int Plevel;		//This should only adjust the level of the IR effect, and not wet/dry mix
  int Psafe;
  int Plength;		//5...500 ms// Set maximum length of IR.
  int Puser;		//-64...64//Feedback.
  int Filenum;
  int Pfb;		//-64 ... 64// amount of feedback
  void setvolume (int Pvolume);
  void setpanning (int Ppanning);
  void sethidamp (int Phidamp);
  void process_rbuf();

  // State for the setpresetPrefetch()/setpresetCommit() split above.
  // ioScratch/rsScratch are read/resample scratch space sized exactly
  // like buf/rbuf but never touched by out(), so prefetchIR() can fill
  // them without any lock; commitIR() copies the result into the real
  // rbuf (which out() does read) under the caller's lock.
  float *ioScratch, *rsScratch;
  int scratchLen;
  bool scratchValid;
  bool scratchOpenFailed;
  char scratchFilename[128];
  int scratchFilenum;
  int pendingParams[11];
  int pendingPreset;
  bool pendingValid;

  // State for SetSuppressFileLoad()/TakeSuppressedFileValue() above.
  bool fSuppressFileLoad;
  bool fHasSuppressedFileValue;
  int fSuppressedFileValue;

  int offset;
  int maxx_size,maxx_read,real_len,length;
  int DS_state;
  int nPERIOD;
  int nSAMPLE_RATE;

  double u_up;
  double u_down;
  float nfSAMPLE_RATE;


  float lpanning, rpanning, hidamp, alpha_hidamp, convlength, oldl;
  float *rbuf, *buf, *lxn;
  float *templ, *tempr;

  float level,fb, feedback;
  float levpanl,levpanr;

  SNDFILE *infile;
  SF_INFO sfinfo;

  //Parametrii reali
  
  class Resample *M_Resample;
  class Resample *U_Resample;
  class Resample *D_Resample;

   class FPreset *Fpre;


};


#endif
