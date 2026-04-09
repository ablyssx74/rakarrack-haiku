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



#ifndef TUNER_H_
#define TUNER_H_

#include <math.h>
#include <stdlib.h>


class Tuner
{
public:
  Tuner ();
  ~Tuner ();
  void schmittFloat (int nframes, float *indatal, float *indatar);

  int note;
  int preparada;
  int note_actual;
  int cents;
  signed short int *schmittBuffer;
  signed short int *schmittPointer;
  const char **notes;
  float nfreq, afreq, freq;
  float *efxoutl;
  float *efxoutr;

private:

  void displayFrequency (float freq);
  void schmittInit (int size);
  void schmittS16LE (int nframes, signed short int *indata);
  void schmittFree ();

  int blockSize;

};

#endif /*TUNER_H_ */
