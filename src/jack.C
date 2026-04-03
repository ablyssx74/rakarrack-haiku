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
#include "/boot/system/develop/headers/os/media/TimeSource.h"
#include "/boot/system/develop/headers/os/media/MediaEventLooper.h"



// X11 Conflict Fix: Include X11 then UNDEF its macros immediately
#include <X11/Xlib.h>
#ifdef CurrentTime
  #undef CurrentTime
#endif

// Media Kit & System
#include "/boot/system/develop/headers/os/media/SoundPlayer.h"
#include "/boot/system/develop/headers/os/media/MediaRoster.h"

#include "jack.h"
#include "global.h"

#include <unistd.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>

#include "/boot/system/develop/headers/os/media/BufferConsumer.h"
#include "/boot/system/develop/headers/os/media/Buffer.h"
#include "/boot/system/develop/headers/os/media/MediaNode.h"






void HaikuRecordCallback(void *cookie, void *buffer, size_t size, const media_raw_audio_format &format);


// Inherit ONLY from BBufferConsumer (which already includes BMediaNode)
class RakInputNode : public BBufferConsumer, public BMediaEventLooper {
public:
    RakInputNode() 
        : BBufferConsumer(B_MEDIA_RAW_AUDIO), 
          BMediaNode("Rakarrack-In"),
          BMediaEventLooper() // This handles the internal thread
    {
        AddNodeKind(B_PHYSICAL_INPUT);
    }
    
	// This MUST match the header exactly to stop the "Abstract Class" error
	virtual void HandleEvent(const media_timed_event* event, 
                         bigtime_t lateness, 
                         bool realTimeEvent = false) 
	{
    // Leave this empty for now; the looper just needs it to exist
	}

    // This replaces the need for "Run()"
    virtual void NodeRegistered() {
        // Start the internal BMediaEventLooper thread
        Run(); // Note: BMediaEventLooper::Run() is PUBLIC!
    }
    
    virtual ~RakInputNode() {}
	virtual BMediaNode::run_mode RunMode() { return B_RECORDING; }

    // 1. COMMUNICATE: Fixes the Cortex hang
    virtual port_id ControlPort() const { return BMediaNode::ControlPort(); }

    virtual status_t HandleMessage(int32 code, const void *data, size_t size) {
        if (BBufferConsumer::HandleMessage(code, data, size) == B_OK) return B_OK;
        return BMediaNode::HandleMessage(code, data, size);
    }

	virtual status_t GetNodeAttributes(media_node_attribute* out_attributes, size_t in_max_count, size_t* out_count) {
    	// Correct logic: Set count to 0 and return B_OK to tell Cortex we have no special tags.
    	// If you return an error or leave out_count uninitialized, Cortex hangs.
    	if (out_count == NULL) return B_BAD_VALUE;
    	*out_count = 0;
    	return B_OK;
	}
    virtual status_t GetConfiguration(BMessage* message) { return B_OK; }

    // 2. PINS: This makes the "Guitar In" hole appear on the left in Cortex
    virtual status_t GetNextInput(int32* cookie, media_input* out_input) {
    // 1. Check the cookie. On the first call, it's 0.
    if (*cookie != 0) {
        // This is the "Stop" signal. Returning B_ENTRY_NOT_FOUND or B_ERROR 
        // tells Cortex there are no more pins to draw.
        return B_ENTRY_NOT_FOUND; 
    }

    // 2. Describe the pin
    out_input->node = Node();
    out_input->destination.port = ControlPort();
    out_input->destination.id = 0; // The first pin ID
    out_input->source = media_source::null;
    sprintf(out_input->name, "Guitar In");
    
    // 3. Set the format (use wildcard to allow the mixer/HDA to connect)
    out_input->format.type = B_MEDIA_RAW_AUDIO;
    out_input->format.u.raw_audio = media_raw_audio_format::wildcard;

    // 4. IMPORTANT: Increment the cookie so the NEXT call hits the check at the top.
    *cookie = 1; 

    return B_OK;
}


    // 3. AUDIO: The actual data capture
    virtual void BufferReceived(BBuffer *buffer) {
        HaikuRecordCallback(NULL, buffer->Data(), buffer->SizeUsed(), media_raw_audio_format::wildcard);
        buffer->Recycle();
    }

    // 4. BOILERPLATE: Mandatory to prevent "Abstract Class" errors
    virtual void Preroll() {}
    virtual BMediaAddOn* AddOn(int32* internalID) const { return NULL; }
    //virtual void NodeRegistered() {}
    virtual void Disconnected(const media_source&, const media_destination&) {}
    virtual status_t Connected(const media_source&, const media_destination&, const media_format&, media_input*) { return B_OK; }
    virtual status_t FormatChanged(const media_source&, const media_destination&, int32, const media_format&) { return B_OK; }
    virtual status_t AcceptFormat(const media_destination&, media_format*) { return B_OK; }
    virtual void DisposeInputCookie(int32) {}
    virtual void ProducerDataStatus(const media_destination&, int32, bigtime_t) {}
    virtual status_t GetLatencyFor(const media_destination&, bigtime_t*, media_node_id*) { return B_OK; }
}; // Ensure this semicolon and bracket are here

// Globals (Only define these ONCE here)
extern float input_buffer_L[4096];
extern float input_buffer_R[4096];
extern float temp_buffer_L[4096];
extern float temp_buffer_R[4096];

class RakInputNode; // Forward declaration
static RakInputNode *inNode = NULL; 
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


// Prototypes
void HaikuAudioCallback(void *cookie, void *buffer, size_t size, const media_raw_audio_format &format);
void HaikuRecordCallback(void *cookie, void *buffer, size_t size, const media_raw_audio_format &format);
int jackprocess (jack_nframes_t nframes, void *arg);

// Helper Function (Must be ABOVE JACKstart)
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
  //  media_format format;
 //   memset(&format, 0, sizeof(format));
  //  format.type = B_MEDIA_RAW_AUDIO;
 
 
    media_format format;
    format.type = B_MEDIA_RAW_AUDIO;
    format.u.raw_audio = media_raw_audio_format::wildcard; 
    
    format.u.raw_audio.frame_rate = 96000.0;
    format.u.raw_audio.channel_count = 2;
   // format.u.raw_audio.format = media_raw_audio_format::B_AUDIO_FLOAT;
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

    // 3. Write to Haiku Output (Interleaved)
    // Use the engine's output buffers (temp_buffer_L/R) directly
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


extern "C" bigtime_t estimate_max_scheduling_latency();

int JACKstart(RKR * rkr_, jack_client_t * jackclient_) {
    JackOUT = rkr_;
    pthread_mutex_init(&jmutex, NULL);

    // DEFINE FORMAT HERE AT THE TOP
    media_raw_audio_format format;
    memset(&format, 0, sizeof(format)); 
    format.format = media_raw_audio_format::B_AUDIO_FLOAT;
    format.channel_count = 2;
    format.frame_rate = 96000.0; 
    format.byte_order = B_MEDIA_HOST_ENDIAN;
    format.buffer_size = 512 * sizeof(float) * 2; 	

    BMediaRoster* roster = BMediaRoster::Roster();
    RakInputNode* inNode = new RakInputNode();
    roster->RegisterNode(inNode);

    // Safe TimeSource Logic
    BTimeSource* timeSource = roster->MakeTimeSourceFor(inNode->Node());
    if (timeSource == NULL) {
        media_node tsNode;
        roster->GetTimeSource(&tsNode);
        timeSource = roster->MakeTimeSourceFor(tsNode);
    }

    if (timeSource) {
        if (!timeSource->IsRunning()) {
            bigtime_t real = system_time(); 
            roster->StartTimeSource(timeSource->Node(), real);
            roster->SeekTimeSource(timeSource->Node(), 0, real);
        }
        
        bigtime_t initLatency = 0;
        roster->GetInitialLatencyFor(inNode->Node(), &initLatency);
        bigtime_t startTime = timeSource->Now() + initLatency + 25000; 
        roster->StartNode(inNode->Node(), startTime);
        timeSource->Release();
    }

    // Now 'format' is safely in scope for the output player
    outPlayer = new BSoundPlayer(&format, "Rakarrack-Out", HaikuAudioCallback, NULL, (void*)rkr_);
    
    outPlayer->Start();
    outPlayer->SetHasData(true);

    return 0;
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
    if (!roster) return;

    // 1. SHUT DOWN THE LOOPER (The "Magic Bullet" for the hang)
    if (inNode != NULL) {
        // Tell the roster we are done with this node
        roster->UnregisterNode(inNode); 
        
        // This is the "Public" way to trigger the looper's Quit() and delete
        inNode->Release(); 
        
        inNode = NULL; 
    }

    // 2. Tear down the BSoundPlayer connections
    if (gInputNode.node > 0) {
        roster->StopNode(gInputNode, 0);
        roster->StopNode(gPlayerNode, 0);

        roster->Disconnect(gInputOutput.node.node, gInputOutput.source, 
                           gPlayerInput.node.node, gPlayerInput.destination);
        
        roster->ReleaseNode(gInputNode);
        roster->ReleaseNode(gPlayerNode);
    }

    // 3. Clean up the players
    if (inPlayer) { inPlayer->Stop(); delete inPlayer; inPlayer = NULL; }
    if (outPlayer) { outPlayer->Stop(); delete outPlayer; outPlayer = NULL; }

    printf("Haiku Audio Cleaned Up Successfully.\n");
}



