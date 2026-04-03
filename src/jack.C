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

*/

// Added everything to line 203 or so and a cleanup function at the bottom for Haiku
// Using aboulute paths here because Looper.h is already defined by the app and will 
// Cause conflicts when trying to call Haiku's Looper.h
#include "/boot/system/develop/headers/os/app/Looper.h"
#include "/boot/system/develop/headers/os/app/Application.h"
#include "/boot/system/develop/headers/os/app/Message.h"
#include "/boot/system/develop/headers/os/support/Archivable.h"


// 2. X11 Conflict Fix: Include X11 then UNDEF its macros immediately
#include <X11/Xlib.h>
#ifdef CurrentTime
  #undef CurrentTime
#endif

// 3. Media Kit & System
#include "/boot/system/develop/headers/os/media/SoundPlayer.h"
#include "/boot/system/develop/headers/os/media/MediaRoster.h"

#include "jack.h"
#include "global.h"

#include <unistd.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>


// 5. Globals (Only define these ONCE here)
extern float input_buffer_L[4096];
extern float input_buffer_R[4096];
extern float temp_buffer_L[4096];
extern float temp_buffer_R[4096];

static BSoundPlayer *outPlayer = NULL;
static BSoundPlayer *inPlayer = NULL;
extern pthread_mutex_t jmutex;
extern RKR *JackOUT;
extern float* current_haiku_buffer;

// Persistent handles for shutdown
media_node   gInputNode;
media_node   gPlayerNode;
media_output gInputOutput;
media_input  gPlayerInput;


// 6. Prototypes
void HaikuAudioCallback(void *cookie, void *buffer, size_t size, const media_raw_audio_format &format);
void HaikuRecordCallback(void *cookie, void *buffer, size_t size, const media_raw_audio_format &format);
int jackprocess (jack_nframes_t nframes, void *arg);

// 7. Helper Function (Must be ABOVE JACKstart)
status_t ConnectInputToPlayer(BSoundPlayer* player) {
    if (!player) return B_BAD_VALUE;
    BMediaRoster* roster = BMediaRoster::Roster();
    if (!roster) return B_ERROR;

    float dMin, dMax;
    int32 dParam;
    player->GetVolumeInfo(&gPlayerNode, &dParam, &dMin, &dMax);

    status_t err = roster->GetAudioInput(&gInputNode);
    if (err != B_OK) return err;

    int32 count = 0;
    err = roster->GetFreeOutputsFor(gInputNode, &gInputOutput, 1, &count, B_MEDIA_RAW_AUDIO);
    if (err != B_OK || count < 1) return B_BUSY;

    err = roster->GetFreeInputsFor(gPlayerNode, &gPlayerInput, 1, &count, B_MEDIA_RAW_AUDIO);
    if (err != B_OK || count < 1) return B_BUSY;

    // FIX: Set the format correctly through the union
    media_format format;
    memset(&format, 0, sizeof(format));
    format.type = B_MEDIA_RAW_AUDIO;
    
    format.u.raw_audio.frame_rate = 44100.0;
    format.u.raw_audio.channel_count = 2;
    format.u.raw_audio.format = media_raw_audio_format::B_AUDIO_FLOAT;
    format.u.raw_audio.byte_order = B_MEDIA_HOST_ENDIAN;
    format.u.raw_audio.buffer_size = 512 * sizeof(float) * 2; 
    
    // Use this format directly
    err = roster->Connect(gInputOutput.source, gPlayerInput.destination, &format, &gInputOutput, &gPlayerInput);
    
    if (err == B_OK) {
        roster->StartNode(gInputNode, 0); 
        roster->StartNode(gPlayerNode, 0); 
        printf("Nodes Started. Connection active.\n");
    }
    return err;
}




// The Record Hook: Grabs guitar from Haiku and puts it in Rakarrack's "In"
void HaikuRecordCallback(void *cookie, void *buffer, size_t size, const media_raw_audio_format &format) {
    if (buffer == NULL || (addr_t)buffer < 0x1000) return;
    float* haiku_in = (float*)buffer;
   // uint32_t nframes = size / 8; 
     uint32_t nframes = size / (sizeof(float) * 2); 
     /* 
    debug
        for (uint32_t i = 0; i < nframes; i++) {
        input_buffer_L[i] = 0.05f; 
        input_buffer_R[i] = 0.05f;
    }
     */
	
    for (uint32_t i = 0; i < nframes; i++) {
        input_buffer_L[i] = haiku_in[i * 2];
        input_buffer_R[i] = haiku_in[i * 2 + 1];
    }
   //	static int counter = 0;
   // if (counter++ % 100 == 0) printf("Record Callback is ALIVE\n");
}


// The Play Hook: Processes and then Interleaves to Speakers
void HaikuAudioCallback(void *cookie, void *buffer, size_t size, const media_raw_audio_format &format) {
    float* fbuf = (float*)buffer;
    uint32_t nframes = size / (sizeof(float) * 2); 

    memset(buffer, 0, size);
    current_haiku_buffer = fbuf;

    // 1. 

    // 2. Engine: Process the audio through the effects rack
    jackprocess(nframes, cookie);

    // 3. Output: Re-order the processed L/R data for Haiku speakers
    for (uint32_t i = 0; i < nframes; i++) {
        temp_buffer_L[i] = fbuf[i];
        temp_buffer_R[i] = fbuf[i + nframes];
    }
    for (uint32_t i = 0; i < nframes; i++) {
        fbuf[i * 2]     = temp_buffer_L[i];
        fbuf[i * 2 + 1] = temp_buffer_R[i];
    }

    current_haiku_buffer = NULL;
}


extern "C" {
    // ... your other stubs ...
    char** jack_get_ports(jack_client_t *, const char *, const char *, unsigned long);
    void jack_free(void *);
    
    jack_port_t* jack_port_register(jack_client_t*, const char*, const char*, unsigned long, unsigned long);
    
    // Make sure these are also here:
    char** jack_get_ports(jack_client_t *, const char *, const char *, unsigned long);
    void jack_free(void *);
}

jack_client_t *jackclient;
jack_port_t *outport_left, *outport_right;
jack_port_t *inputport_left, *inputport_right, *inputport_aux;
jack_port_t *jack_midi_in, *jack_midi_out;
void *dataout;

int jackprocess (jack_nframes_t nframes, void *arg);

int JACKstart(RKR * rkr_, jack_client_t * jackclient_) {
    JackOUT = rkr_;
    pthread_mutex_init(&jmutex, NULL);

    media_raw_audio_format format = media_raw_audio_format::wildcard;
    format.format = media_raw_audio_format::B_AUDIO_FLOAT;
    format.channel_count = 2;
    format.frame_rate = 44100.0; 
    format.buffer_size = 512 * sizeof(float) * 2; 

    // 1. Output Player (Speakers) - BSoundPlayer connects to Default Out automatically
    outPlayer = new BSoundPlayer(&format, "Rakarrack-Out", HaikuAudioCallback, NULL, (void*)rkr_);
    
    // 2. Input Player (Microphone)    
    inPlayer = new BSoundPlayer(&format, "Rakarrack-In", HaikuRecordCallback, NULL, (void*)rkr_);

    if (outPlayer->InitCheck() != B_OK || inPlayer->InitCheck() != B_OK) return 2;

    // START the players
    outPlayer->Start();
    inPlayer->Start();
    inPlayer->SetHasData(true); 

    // MANDATORY: Connect the physical mic to the inPlayer
    status_t connErr = ConnectInputToPlayer(inPlayer);
    if (connErr != B_OK) {
        printf("Warning: Could not connect Microphone (Error: %s). Check Media Prefs.\n", strerror(connErr));
    }

    outPlayer->SetHasData(true);
    return 3;
}



/*
int
JACKstart (RKR * rkr_, jack_client_t * jackclient_)
{


  JackOUT = rkr_;
  jackclient = jackclient_;

  jack_set_sync_callback(jackclient, timebase, NULL);
  jack_set_process_callback (jackclient, jackprocess, 0);
  jack_on_shutdown (jackclient, jackshutdown, 0);



  inputport_left =
    jack_port_register (jackclient, "in_1", JACK_DEFAULT_AUDIO_TYPE,
			JackPortIsInput, 0);
  inputport_right =
    jack_port_register (jackclient, "in_2", JACK_DEFAULT_AUDIO_TYPE,
			JackPortIsInput, 0);

  inputport_aux =
    jack_port_register (jackclient, "aux", JACK_DEFAULT_AUDIO_TYPE,
			JackPortIsInput, 0);

  outport_left =
    jack_port_register (jackclient, "out_1", JACK_DEFAULT_AUDIO_TYPE,
			JackPortIsOutput, 0);
  outport_right =
    jack_port_register (jackclient, "out_2", JACK_DEFAULT_AUDIO_TYPE,
			JackPortIsOutput, 0);

  jack_midi_in =  
    jack_port_register(jackclient, "in", JACK_DEFAULT_MIDI_TYPE, JackPortIsInput, 0);

  jack_midi_out = 
    jack_port_register(jackclient, "MC out", JACK_DEFAULT_MIDI_TYPE, JackPortIsOutput, 0);
  

  if (jack_activate (jackclient))
    {
      fprintf (stderr, "Cannot activate jack client.\n");
      return (2);
    };

  if (JackOUT->aconnect_JA)
    {

      for (int i = 0; i < JackOUT->cuan_jack; i += 2)
	{
	  jack_connect (jackclient, jack_port_name (outport_left),
			JackOUT->jack_po[i].name);
	  jack_connect (jackclient, jack_port_name (outport_right),
			JackOUT->jack_po[i + 1].name);
	}
    }

  if (JackOUT->aconnect_JIA)
    {

       if(JackOUT->cuan_ijack == 1)
        {
          jack_connect (jackclient,JackOUT->jack_poi[0].name,jack_port_name(inputport_left));
	  jack_connect (jackclient,JackOUT->jack_poi[0].name, jack_port_name(inputport_right));
	 }	
	
      else
       { 
      for (int i = 0; i < JackOUT->cuan_ijack; i += 2)
	 {
	  jack_connect (jackclient,JackOUT->jack_poi[i].name, jack_port_name (inputport_left));
	  jack_connect (jackclient,JackOUT->jack_poi[i + 1].name,jack_port_name (inputport_right));
	  }
       }

    }





  pthread_mutex_init (&jmutex, NULL);


  return 3;

};

*/

int
jackprocess (jack_nframes_t nframes, void *arg)
{

  int i,count;
  jack_midi_event_t midievent;
  jack_position_t pos;
  jack_transport_state_t astate;

  jack_default_audio_sample_t *outl = (jack_default_audio_sample_t *)
    jack_port_get_buffer (outport_left, nframes);
  jack_default_audio_sample_t *outr = (jack_default_audio_sample_t *)
    jack_port_get_buffer (outport_right, nframes);


  jack_default_audio_sample_t *inl = (jack_default_audio_sample_t *)
    jack_port_get_buffer (inputport_left, nframes);
  jack_default_audio_sample_t *inr = (jack_default_audio_sample_t *)
    jack_port_get_buffer (inputport_right, nframes);

  jack_default_audio_sample_t *aux = (jack_default_audio_sample_t *)
    jack_port_get_buffer (inputport_aux, nframes);

  JackOUT->cpuload = jack_cpu_load(jackclient);

  
  if((JackOUT->Tap_Bypass) && (JackOUT->Tap_Selection == 2))
  {
   astate = jack_transport_query(jackclient, &pos);
   if(astate >0)
   {
   if (JackOUT->jt_tempo != pos.beats_per_minute) 
   actualiza_tap(pos.beats_per_minute);
   }

  if(JackOUT->Looper_Bypass)
   {
    if((astate != JackOUT->jt_state) && (astate==0))
    {
    JackOUT->jt_state=astate;
    JackOUT->efx_Looper->changepar(2,1);
    stecla=5;
    }

    if((astate != JackOUT->jt_state) && (astate == 3))
    {
    JackOUT->jt_state=astate;
    JackOUT->efx_Looper->changepar(1,1);
    stecla=5;
    }

   }
  }  
  
  
  
  int jnumpi = jack_port_connected(inputport_left) + jack_port_connected(inputport_right );
  if(jnumpi != JackOUT->numpi) 
  {
  JackOUT->numpi=jnumpi;
  JackOUT->numpc = 1;
  }
  int jnumpo = jack_port_connected(outport_left) + jack_port_connected(outport_right );
  if(jnumpo != JackOUT->numpo)
  { 
  JackOUT->numpo = jnumpo;
  JackOUT->numpc = 1;
  }
  int jnumpa = jack_port_connected(inputport_aux);
  if(jnumpa != JackOUT->numpa)
  {
   JackOUT->numpa = jnumpa;
   JackOUT->numpc = 1;
  }
  
  int jnumpmi = jack_port_connected(jack_midi_in);
  if(jnumpmi != JackOUT->numpmi)
  {
   JackOUT->numpmi = jnumpmi;
   JackOUT->numpc = 1;
  }
  
  int jnumpmo = jack_port_connected(jack_midi_out);
  if(jnumpmo != JackOUT->numpmo)
  {
   JackOUT->numpmo = jnumpmo;
   JackOUT->numpc = 1;
  }
  



  pthread_mutex_lock (&jmutex);

  float *data = (float *)jack_port_get_buffer(jack_midi_in, nframes); 
  count = jack_midi_get_event_count(data);

  dataout = jack_port_get_buffer(jack_midi_out, nframes); 
  jack_midi_clear_buffer(dataout);   	


  for (i = 0; i < count; i++)
   {                  
   jack_midi_event_get(&midievent, data, i);
   JackOUT->jack_process_midievents(&midievent);
   }  

  for (i=0; i<=JackOUT->efx_MIDIConverter->ev_count; i++)
  { 
    jack_midi_event_write(dataout,
    JackOUT->efx_MIDIConverter->Midi_event[i].time,
    JackOUT->efx_MIDIConverter->Midi_event[i].dataloc,
    JackOUT->efx_MIDIConverter->Midi_event[i].len);
  }

  JackOUT->efx_MIDIConverter->moutdatasize = 0;
  JackOUT->efx_MIDIConverter->ev_count = 0;


   
  memcpy (JackOUT->efxoutl, inl,
	  sizeof (jack_default_audio_sample_t) * nframes);
  memcpy (JackOUT->efxoutr, inr,
	  sizeof (jack_default_audio_sample_t) * nframes);
  memcpy (JackOUT->auxdata, aux,
	  sizeof (jack_default_audio_sample_t) * nframes);
  



  JackOUT->Alg (JackOUT->efxoutl, JackOUT->efxoutr, inl, inr ,0);

  
  memcpy (outl, JackOUT->efxoutl,
	  sizeof (jack_default_audio_sample_t) * nframes);
  memcpy (outr, JackOUT->efxoutr,
	  sizeof (jack_default_audio_sample_t) * nframes);
  
  
  pthread_mutex_unlock (&jmutex);


  return 0;

};


void
JACKfinish ()
{
  jack_client_close (jackclient);
  pthread_mutex_destroy (&jmutex);
  usleep (100000);
};



void
jackshutdown (void *arg)
{
  if (gui == 0)
    printf ("Jack Shut Down, sorry.\n");
  else
    JackOUT->Message (1,JackOUT->jackcliname,
		      "Jack Shut Down, try to save your work");
};



int
timebase(jack_transport_state_t state, jack_position_t *pos, void *arg)
{

JackOUT->jt_state=state;


if((JackOUT->Tap_Bypass) && (JackOUT->Tap_Selection == 2))
 { 
  if((state > 0) && (pos->beats_per_minute > 0))
    {
      JackOUT->jt_tempo=pos->beats_per_minute;
      JackOUT->Tap_TempoSet = lrint(JackOUT->jt_tempo);
      JackOUT->Update_tempo();
      JackOUT->Tap_Display=1;
      if((JackOUT->Looper_Bypass) && (state==3))
       {
        JackOUT->efx_Looper->changepar(1,1);
        stecla=5;
       }
    }
 }

return(1);

}

void
actualiza_tap(double val)
{
JackOUT->jt_tempo=val;
JackOUT->Tap_TempoSet = lrint(JackOUT->jt_tempo);
JackOUT->Update_tempo();
JackOUT->Tap_Display=1;
}


// Added for Haiku 

extern "C" void HaikuAudioShutdown() {
    BMediaRoster* roster = BMediaRoster::Roster();
    
    if (inPlayer) {
        if (roster && gInputNode.node > 0) {
            // 1. Tell the hardware and player to stop the clock
            roster->StopNode(gInputNode, 0);
            roster->StopNode(gPlayerNode, 0);

            // 2. Now disconnect using the exact handles from your globals
            roster->Disconnect(gInputOutput.node.node, gInputOutput.source, 
                               gPlayerInput.node.node, gPlayerInput.destination);
            
            // 3. Release our claim on these nodes
            roster->ReleaseNode(gInputNode);
            roster->ReleaseNode(gPlayerNode);
        }
        inPlayer->Stop();
        delete inPlayer;
        inPlayer = NULL;
    }

    if (outPlayer) {
        outPlayer->Stop();
        delete outPlayer;
        outPlayer = NULL;
    }
    printf("Haiku Audio Cleaned Up Successfully.\n");
}



