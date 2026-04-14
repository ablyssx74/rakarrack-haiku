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
#include "MIDIConverter.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "global.h"
#include <Midi2Defs.h>
#include <MidiEndpoint.h>
#include <MidiProducer.h>
 



MIDIConverter::MIDIConverter ()
{

  velocity = 100;
  channel = 0;
  lanota = -1;
  preparada = 0;
  nota_actual = -1;
  TrigVal = .25f;
  hay = 0;
  ponla = 0;
  moutdatasize=0;
  ev_count=0;
  Moctave = 0;   
  schmittBuffer = NULL;
  schmittPointer = NULL;
  static const char *englishNotes[12] =
    { "A", "A#", "B", "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#" };
  notes = englishNotes;
  note = 0;
  nfreq = 0;
  afreq = 0;  
  fHaikuMidiOut = NULL;  
  schmittInit (32);
  port = NULL; 
  stable_count = 0; 
  p_trigfact = 0.5f;
  p_stable_threshold = 2;
  p_off_count_max = 5;
  
};


MIDIConverter::~MIDIConverter ()
{
	
	
  // Cleanup the Schmitt buffer to avoid memory leaks
  schmittFree();  
  // ALSA port is no longer used, so we just null it out
  port = NULL;
  
}



void
MIDIConverter::displayFrequency (float ffreq)
{
  int i;
  int noteoff = 0;
  int octave = 4;
  float ldf, mldf;
  float lfreq;

  if (ffreq < 1E-15)
    ffreq = 1E-15f;
  lfreq = logf (ffreq);
  while (lfreq < lfreqs[0] - LOG_D_NOTE * .5f)
    lfreq += LOG_2;
  while (lfreq >= lfreqs[0] + LOG_2 - LOG_D_NOTE * .5f)
    lfreq -= LOG_2;
  mldf = LOG_D_NOTE;
  for (i = 0; i < 12; i++)
    {
      ldf = fabsf (lfreq - lfreqs[i]);
      if (ldf < mldf)
	{
	  mldf = ldf;
	  note = i;
	}
    }
  nfreq = freqs[note];
  while (nfreq / ffreq > D_NOTE_SQRT)
    {
      nfreq *= .5f;
      octave--;
      if (octave < -2)
	{
	  noteoff = 1;
	  break;
	}
    }
  while (ffreq / nfreq > D_NOTE_SQRT)
    {
      nfreq *= 2.0f;
      octave++;
      if (octave > 9)
	{
	  noteoff = 1;
	  break;
	}
    }
  cents = lrintf (1200.0f * (logf (ffreq / nfreq) / LOG_2));
  lanota = 24 + (octave * 12) + note - 3;
  /*
  if (ffreq > 20.0f) { 
      printf("[MIDI DEBUG] Freq: %7.2f Hz | Note: %s%d (%d) | Offset: %+d cents\n", 
              ffreq, notes[note], octave, lanota, cents);
  }
*/
  
  if (noteoff) 
  {
      off_count++;
  } 
  else 
  {
      off_count = 0; 
  }

  // Only send Note Off if we've seen "silence" or "invalid pitch" for 10 consecutive cycles
  if (off_count > p_off_count_max && hay)
    {
      MIDI_Send_Note_Off (nota_actual);
      hay = 0;
      nota_actual = -1;
      stable_count = 0;
      off_count = 0;
    }


  // --- CONFIDENCE & HARMONIC REJECTION FILTER ---
  if (lanota > 0 && lanota < 128)
  {
  	 if (ffreq > 5000.0f) return; 
      if (lanota == preparada)
      {
          stable_count++;
      }
      else
      {
          // A harmonic jump happened. 
          // We DON'T reset nota_actual yet, we just start a new count for the potential new note.
          preparada = lanota;
          stable_count = 1;
      }

      // 1. High Threshold for Note Changes (requires 4 consistent frames)
      // 2. Only kill the old note if the new note is truly stable
      if (stable_count >= p_stable_threshold && lanota != nota_actual)
      {
          // We found a new, stable note.
         //printf(">>> STABLE CHANGE: %s%d (%d)\n", notes[note], octave, lanota);
          
          hay = 1;
          if (nota_actual != -1)
          {
              MIDI_Send_Note_Off(nota_actual);
          }
          MIDI_Send_Note_On(lanota);
          nota_actual = lanota;
      }
  }



  // --- STABILITY FILTER END ---
};


void
MIDIConverter::schmittInit (int size)
{
  blockSize = SAMPLE_RATE / size;
  schmittBuffer =
    (signed short int *) malloc (blockSize * sizeof (signed short int));
  schmittPointer = schmittBuffer;
};

void
MIDIConverter::schmittS16LE (int nframes, signed short int *indata)
{
  int i, j;
  float trigfact = p_trigfact;


  for (i = 0; i < nframes; i++)
    {
      *schmittPointer++ = indata[i];
      if (schmittPointer - schmittBuffer >= blockSize)
	{
	  int endpoint, startpoint, t1, t2, A1, A2, tc, schmittTriggered;

	  schmittPointer = schmittBuffer;

	  for (j = 0, A1 = 0, A2 = 0; j < blockSize; j++)
	    {
	      if (schmittBuffer[j] > 0 && A1 < schmittBuffer[j])
		A1 = schmittBuffer[j];
	      if (schmittBuffer[j] < 0 && A2 < -schmittBuffer[j])
		A2 = -schmittBuffer[j];
	    }
	  t1 = lrintf ((float)A1 * trigfact + 0.5f);
	  t2 = -lrintf ((float)A2 * trigfact + 0.5f);
	  startpoint = 0;
	  for (j = 1; schmittBuffer[j] <= t1 && j < blockSize; j++);
	  for (; !(schmittBuffer[j] >= t2 &&
		   schmittBuffer[j + 1] < t2) && j < blockSize; j++);
	  startpoint = j;
	  schmittTriggered = 0;
	  endpoint = startpoint + 1;
	  for (j = startpoint, tc = 0; j < blockSize; j++)
	    {
	      if (!schmittTriggered)
		{
		  schmittTriggered = (schmittBuffer[j] >= t1);
		}
	      else if (schmittBuffer[j] >= t2 && schmittBuffer[j + 1] < t2)
		{
		  endpoint = j;
		  tc++;
		  schmittTriggered = 0;
		}
	    }
	  if (endpoint > startpoint)
	    {
	      afreq =
		fSAMPLE_RATE *((float)tc / (float) (endpoint - startpoint));
	      displayFrequency (afreq);
	    }
	  }
    }
};

void
MIDIConverter::schmittFree ()
{
  free (schmittBuffer);
};

void MIDIConverter::schmittFloat (int nframes, float *indatal, float *indatar)
{
  int i;
  signed short int buf[nframes];
  
  // Increase the multiplier (e.g., from 32768 to 40000) to boost the signal
  // for the pitch detector without affecting the actual audio output.
  for (i = 0; i < nframes; i++)
  {
      float mixed = (TrigVal * indatal[i] + TrigVal * indatar[i]);
      buf[i] = (short) (mixed * 40000); 
  }
  schmittS16LE (nframes, buf);
};



void MIDIConverter::MIDI_Send_Note_On(int nota) {
    int anota = nota + (Moctave * 12);
    if((anota < 0) || (anota > 127)) return;

    // 1. SAFETY CHECK: Prevent overwriting memory
    if (ev_count >= 2046 || moutdatasize >= 2040) {
        //printf("[MIDI] Buffer full! Resetting.\n");
        ResetBuffers();
    }

    int k = lrintf ((val_sum + 48) * 2);
    if ((k > 0) && (k < 127))
        velocity = lrintf((float)k * VelVal);
   
    if (velocity > 127) velocity = 127;
    if (velocity < 1) velocity = 1;

    // 2. Haiku Midi Kit Spray
    if (fHaikuMidiOut) {
        fHaikuMidiOut->SprayNoteOn(channel, anota, velocity, system_time());
    }

    // 3. INTERNAL TRACKING (Fixing the index bug)
    // We use ev_count FIRST, then increment.
    Midi_event[ev_count].dataloc = &moutdata[moutdatasize];
    Midi_event[ev_count].time = 0;
    Midi_event[ev_count].len = 3;
    ev_count++; // Move to next slot for next call

    moutdata[moutdatasize] = 144 + channel;
    moutdatasize++;
    moutdata[moutdatasize] = anota;
    moutdatasize++;
    moutdata[moutdatasize] = velocity;
    moutdatasize++; 
}


void MIDIConverter::MIDI_Send_Note_Off(int nota) {
    // 1. Calculate note
    int anota = nota + (Moctave * 12);
    if((anota < 0) || (anota > 127)) return;

    // 2. Safety Guard: Reset if we are near the end of the 2048 arrays
    if (ev_count >= 2046 || moutdatasize >= 2040) {
       // printf("[MIDI WARNING] Buffer full! Resetting.\n");
        ResetBuffers();
    }

    // 3. Haiku Midi Kit Spray (Direct)
    if (fHaikuMidiOut) {
        fHaikuMidiOut->SprayNoteOff(channel, anota, 0, system_time());
    } else {
        return; // No sense recording events if there's no output
    }

    // 4. Record Event (For Internal Tracking/Jack)
    // Use the current ev_count, then increment AFTER
    Midi_event[ev_count].dataloc = &moutdata[moutdatasize];
    Midi_event[ev_count].time = 0;
    Midi_event[ev_count].len = 2;
    ev_count++; 

    // Record Data
    moutdata[moutdatasize] = 128 + channel;
    moutdatasize++;
    moutdata[moutdatasize] = anota;
    moutdatasize++;
}


void MIDIConverter::ResetBuffers() {
    ev_count = 0;
    moutdatasize = 0;
}


void
MIDIConverter::panic ()
{
 // printf("[MIDI DEBUG] Panic called! Current nota_actual: %d\n", nota_actual);
  int i;
  for (i = 0; i < 127; i++)
    MIDI_Send_Note_Off (i);
  hay = 0;
  nota_actual = -1;
}
void
MIDIConverter::setmidichannel (int chan)
{
  channel = chan;
};

void
MIDIConverter::setTriggerAdjust (int val)
{
  TrigVal = 1.0f / (float)val;
};

void
MIDIConverter::setVelAdjust (int val)
{
  VelVal = 100.0f / (float)val;
};


