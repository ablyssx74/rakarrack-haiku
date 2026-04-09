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


#ifndef PITCH_H
#define PITCH_H

#include <string.h>
#include <math.h>
#include <stdio.h>


#define MAX_FRAME_LENGTH 2048
class PitchShifter
{
public:PitchShifter (long fftFrameSize, long osamp, float sampleRate);
   ~PitchShifter ();
  void smbPitchShift (float pitchShift, long numSampsToProcess,
		      long fftFrameSize, long osamp, float sampleRate,
		      float *indata, float *outdata);
  void smbFft (float *fftBuffer, long fftFrameSize, long sign);
  double smbAtan2 (double x, double y);
  float ratio;
private:
  float gInFIFO[MAX_FRAME_LENGTH];
  float gOutFIFO[MAX_FRAME_LENGTH];
  float gFFTworksp[2 * MAX_FRAME_LENGTH];
  float gLastPhase[MAX_FRAME_LENGTH / 2 + 1];
  float gSumPhase[MAX_FRAME_LENGTH / 2 + 1];
  float gOutputAccum[2 * MAX_FRAME_LENGTH];
  float gAnaFreq[MAX_FRAME_LENGTH];
  float gAnaMagn[MAX_FRAME_LENGTH];
  float gSynFreq[MAX_FRAME_LENGTH];
  float gSynMagn[MAX_FRAME_LENGTH];
  double dfftFrameSize, coef_dfftFrameSize, dpi_coef;
  double magn, phase, tmp, window, real, imag;
  double freqPerBin, expct, coefPB, coef_dpi, coef_mpi;
  long k, qpd, index, inFifoLatency, stepSize, fftFrameSize2, gRover, FS_osamp;
};


#endif /*  */
