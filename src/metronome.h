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

#ifndef EFFECT_METRONOME_H
#define EFFECT_METRONOME_H
#include "global.h"
#include "AnalogFilter.h"

class metronome
{
public:
  metronome ();
  ~metronome ();
  void cleanup();
  void metronomeout (float * tickout);
  void set_tempo (int bpm);
  void set_meter (int counts);
  int markctr;

private:
  int tick_interval;
  int tickctr;
  int meter;
  int tickper;
  int ticktype;

  class AnalogFilter *dulltick,*sharptick, *hpf;  

};


#endif
