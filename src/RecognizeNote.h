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


#ifndef RECOGNIZE_H_
#define RECOGNIZE_H_

#include <math.h>
#include "global.h"
#include "AnalogFilter.h"

class Recognize
{
public:
  Recognize (float * efxoutl_, float * efxoutr_, float trig);
  ~Recognize ();

  void schmittFloat (float *indatal, float *indatar);
  void sethpf(int value);
  void setlpf(int value);
  int note;

  signed short int *schmittBuffer;
  signed short int *schmittPointer;
  const char **notes;
  float trigfact;
  float lafreq;
  float nfreq, afreq, freq;
  float *efxoutl;
  float *efxoutr;
  


private:

  void displayFrequency (float freq);
  void schmittInit (int size);
  void schmittS16LE (signed short int *indata);
  void schmittFree ();

  int ultima;
  int blockSize;

  AnalogFilter *lpfl, *lpfr, *hpfl, *hpfr;

  class Sustainer *Sus;

};

#endif
