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


#include <app/Looper.h>
#include <media/BufferProducer.h>
#include <app/Application.h>
#include <app/Message.h>
#include <support/Archivable.h>
#include <media/TimeSource.h>
#include <media/MediaEventLooper.h>
#include <SupportDefs.h>
#include <OS.h>
#include <syslog.h>


// X11 Conflict Fix: Include X11 then UNDEF its macros immediately
#include <X11/Xlib.h>
#ifdef CurrentTime
  #undef CurrentTime
#endif

// Media Kit & System
#include <media/SoundPlayer.h>
#include <media/MediaRoster.h>

#include "jack.h"
#include "global.h"

#include <unistd.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>

#include <media/BufferConsumer.h>
#include <media/Buffer.h>
#include <media/MediaNode.h>

#include <media/BufferGroup.h>
#include <new>

// Prototypes
void HaikuAudioCallback(void *cookie, void *buffer, size_t size, const media_raw_audio_format &format);
void HaikuRecordCallback(void *cookie, void *buffer, size_t size, const media_raw_audio_format &format);
int jackprocess (jack_nframes_t nframes, void *arg);

// Globals (Only define these ONCE here)
extern float input_buffer_L[8192];
extern float input_buffer_R[8192];
extern float temp_buffer_L[8192];
extern float temp_buffer_R[8192];

float haiku_out_L[8192];
float haiku_out_R[8192];

float haiku_final_L[8192]; 
float haiku_final_R[8192];

float hardware_in_L[8192];
float hardware_in_R[8192];

class RakInputNode; // Forward declaration
static RakInputNode *inNode = NULL; 
static BSoundPlayer *outPlayer = NULL;
//static BSoundPlayer *inPlayer = NULL;
extern pthread_mutex_t jmutex;
extern RKR *JackOUT;
extern float* current_haiku_buffer;

// Persistent handles for shutdown
media_node   gInputNode;
media_node   gPlayerNode;
media_output gInputOutput;
media_input  gPlayerInput;


class SimpleRingBuffer {
public:
    float *buffer;
    int size;
    int writePos;
    int readPos;

    SimpleRingBuffer(int sz) {
        size = sz;
        buffer = new float[size];
        memset(buffer, 0, size * sizeof(float));
        writePos = 0;
        readPos = 0;
    }

    void Write(float* data, int frames) {
        for (int i = 0; i < frames; i++) {
            buffer[writePos] = data[i];
            writePos = (writePos + 1) % size;
        }
    }

    void Read(float* dest, int frames) {
        for (int i = 0; i < frames; i++) {
            dest[i] = buffer[readPos];
            readPos = (readPos + 1) % size;
        }
    }
    
    int Available() {
        int diff = writePos - readPos;
        if (diff < 0) diff += size;
        return diff;
    }
};

// Initialize GLOBAL Ring Buffers (allocate them in main or JACKstart)
SimpleRingBuffer* rbLeft = NULL;
SimpleRingBuffer* rbRight = NULL;


// Inherit ONLY from BBufferConsumer (which already includes BMediaNode)
class RakInputNode : public BBufferConsumer, public BMediaEventLooper {
public:
    RakInputNode() 
        : BBufferConsumer(B_MEDIA_RAW_AUDIO), 
          BMediaNode("Rakarrack-In"),
          BMediaEventLooper() // This handles the internal thread
    {
       AddNodeKind(B_BUFFER_CONSUMER); 
      //  AddNodeKind(B_RECORDING);
        
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
    virtual port_id ControlPort() const { 
    return BMediaEventLooper::ControlPort(); 
	}

    virtual status_t HandleMessage(int32 code, const void *data, size_t size) {
    // If the message is for the Consumer (like "Here is a buffer"), handle it immediately
    if (BBufferConsumer::HandleMessage(code, data, size) == B_OK) return B_OK;
    
    // Otherwise, let the Looper/Node handle it
    if (BMediaEventLooper::HandleMessage(code, data, size) == B_OK) return B_OK;
    
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
    if (!buffer || !rbLeft) return;

    float* incomingData = (float*)buffer->Data();
    size_t numFrames = buffer->SizeUsed() / (2 * sizeof(float)); 

    // De-interleave and write to Ring Buffers
    // We use a temp buffer to de-interleave first, or just write directly if we modify Write()
    // For simplicity, let's just write sample-by-sample in a locked block
    
    pthread_mutex_lock(&jmutex);
    for (size_t i = 0; i < numFrames; i++) {
        rbLeft->buffer[rbLeft->writePos] = incomingData[i * 2];
        rbLeft->writePos = (rbLeft->writePos + 1) % rbLeft->size;
        
        rbRight->buffer[rbRight->writePos] = incomingData[i * 2 + 1];
        rbRight->writePos = (rbRight->writePos + 1) % rbRight->size;
    }
    pthread_mutex_unlock(&jmutex);

    buffer->Recycle();
}









		
    // 4. BOILERPLATE: Mandatory to prevent "Abstract Class" errors
    virtual void Preroll() {}
    virtual BMediaAddOn* AddOn(int32* internalID) const { return NULL; }
    //virtual void NodeRegistered() {}
    virtual void Disconnected(const media_source&, const media_destination&) {}
	virtual status_t Connected(const media_source& source, 
                           const media_destination& dest, 
                           const media_format& format, 
                           media_input* out_input) 
	{
    // 1. Fill out the input info so the Media Server knows the link is active
    out_input->node = Node();
    out_input->source = source;
    out_input->destination = dest;
    out_input->format = format;
    sprintf(out_input->name, "Guitar In");

    // 2. Log that we are officially ready
    fprintf(stderr, "[DEBUG] Connection Finalized: %f Hz\n", format.u.raw_audio.frame_rate);
    fflush(stderr);

    return B_OK; 
	}
    virtual status_t FormatChanged(const media_source&, const media_destination&, int32, const media_format&) { return B_OK; }
	virtual status_t AcceptFormat(const media_destination& dest, media_format* format) {
    if (format->type != B_MEDIA_RAW_AUDIO) return B_MEDIA_BAD_FORMAT;
    
    // If the hardware hasn't picked a format yet, we suggest Float
    if (format->u.raw_audio.format == media_raw_audio_format::wildcard.format) {
        format->u.raw_audio.format = media_raw_audio_format::B_AUDIO_FLOAT;
    }
    
    return B_OK; // Say "Yes" to whatever else it wants (Rate, Channels, etc.)
	}



    virtual void DisposeInputCookie(int32) {}
    virtual void ProducerDataStatus(const media_destination& for_whom, int32 status, bigtime_t at_performance_time) {
    if (for_whom.id == 0) { // Your input pin ID
        fprintf(stderr, "[DEBUG] Producer Status: %s\n", 
                (status == B_DATA_AVAILABLE) ? "Sending Data" : "Stopped");
        fflush(stderr);
    }
}

    virtual status_t GetLatencyFor(const media_destination&, bigtime_t*, media_node_id*) { return B_OK; }
}; // Ensure this semicolon and bracket are here






// Helper Function (Must be ABOVE JACKstart)
status_t ConnectHardwareToRakarrack() {
    BMediaRoster* roster = BMediaRoster::Roster();
    if (!roster) {
        printf("[MediaKit] Error: Could not get Media Roster!\n");
        return B_ERROR;
    }
    if (!inNode) {
        printf("[MediaKit] Error: RakInputNode is NULL!\n");
        return B_ERROR;
    }

    // 1. Get Hardware
    media_node hardwareInput;
    status_t err = roster->GetAudioInput(&hardwareInput);
    if (err != B_OK) {
        printf("[MediaKit] Error: Could not find Physical Audio Input (0x%x)\n", (int)err);
        return err;
    }
    printf("[MediaKit] Found Hardware Input Node ID: %ld\n", hardwareInput.node);

    // 2. Find Hardware Output Pin
    media_output hardwareOut;
    int32 count = 0;
    err = roster->GetFreeOutputsFor(hardwareInput, &hardwareOut, 1, &count, B_MEDIA_RAW_AUDIO);
    if (err != B_OK || count < 1) {
        printf("[MediaKit] Error: Hardware has no free output pins (0x%x)\n", (int)err);
        return B_BUSY;
    }
    printf("[MediaKit] Using Hardware Output Pin: %s\n", hardwareOut.name);

    // 3. Find Rakarrack Input Pin
    media_input rakIn;
    err = roster->GetFreeInputsFor(inNode->Node(), &rakIn, 1, &count, B_MEDIA_RAW_AUDIO);
    if (err != B_OK || count < 1) {
        printf("[MediaKit] Error: RakInputNode has no free input pins (0x%x)\n", (int)err);
        return B_BUSY;
    }
    printf("[MediaKit] Using Rakarrack Input Pin: %s\n", rakIn.name);

    // 4. Set Format
    media_format format;
    format.type = B_MEDIA_RAW_AUDIO;
    format.u.raw_audio = media_raw_audio_format::wildcard;
	format.u.raw_audio.buffer_size = 4096; 
	
    // 5. Attempt Connection
    printf("[MediaKit] Attempting to connect %s -> %s...\n", hardwareOut.name, rakIn.name);
    err = roster->Connect(hardwareOut.source, rakIn.destination, &format, &hardwareOut, &rakIn);
    
    if (err != B_OK) {
        printf("[MediaKit] CONNECTION FAILED: 0x%x (%s)\n", (int)err, strerror(err));
        return err;
    }

    // Success! Print details of the negotiated format
    printf("[MediaKit] SUCCESS! Negotiated Format:\n");
    printf("           Rate: %.1f Hz\n", format.u.raw_audio.frame_rate);
    printf("           Channels: %d\n", (int)format.u.raw_audio.channel_count);
    printf("           Buffer Size: %d bytes\n", (int)format.u.raw_audio.buffer_size);

   
	// 1. Get the system's default time source
	media_node timeSourceNode;
	roster->GetTimeSource(&timeSourceNode);

	// 2. We need a BTimeSource object to call Now()
	BTimeSource* timeSource = roster->MakeTimeSourceFor(timeSourceNode);
	if (!timeSource) {
    	printf("[MediaKit] Error: Could not create BTimeSource object\n");
    	return B_ERROR;
	}

	// 3. Ensure the internal thread of your node is aware it should be 'Running'
	//inNode->SetRunMode(BMediaNode::B_RECORDING);

    // 4. Sync the nodes to the same TimeSource
    // Note: hardwareInput is already a media_node struct, 
    // and inNode->Node() returns a media_node struct.
    roster->SetTimeSourceFor(hardwareInput.node, timeSourceNode.node);
    roster->SetTimeSourceFor(inNode->Node().node, timeSourceNode.node);

	// 5. Calculate a start time (Now + 50ms buffer to ensure nodes are ready)
	bigtime_t startTime = timeSource->Now() + 50000; 

    // 6. Start EVERYTHING once
    roster->StartNode(timeSourceNode, 0); // Ensure clock is running
    roster->StartNode(hardwareInput, startTime); 
    roster->StartNode(inNode->Node(), startTime); 
    

	// 7. Clean up the temporary BTimeSource object
	timeSource->Release();

    
    return B_OK;
}



// The Record Hook: Grabs guitar from Haiku and puts it in Rakarrack's "In"
void HaikuRecordCallback(void *cookie, void *buffer, size_t size, const media_raw_audio_format &format) {
	
    if (buffer == NULL || (addr_t)buffer < 0x1000) return;
    float* haiku_in = (float*)buffer;
   // uint32_t nframes = size / 8; 
     uint32_t nframes = size / (sizeof(float) * 2); 
     if (nframes > 4096) nframes = 4096; 
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
    // Determine how many frames the speakers need
    int nframes = size / (2 * sizeof(float));
    
    // Direct call to your jackprocess function
    // This maintains your existing logic but runs it in the SoundPlayer thread
    jackprocess(nframes, cookie);
    
    // Copy the results from the JackOUT buffers to the hardware buffer
    float* out = (float*)buffer;
    for (int i = 0; i < nframes; i++) {
        out[i * 2]     = JackOUT->efxoutl[i];
        out[i * 2 + 1] = JackOUT->efxoutr[i];
    }
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

    // 1. Define Format
    media_raw_audio_format format;
    memset(&format, 0, sizeof(format)); 
    format.format = media_raw_audio_format::B_AUDIO_FLOAT;
    format.channel_count = 2; 
    format.frame_rate = 48000.0; 
    format.byte_order = B_MEDIA_HOST_ENDIAN;
    format.buffer_size = 512 * sizeof(float) * 2; 	

    BMediaRoster* roster = BMediaRoster::Roster();
    if (!roster) {
        printf("[DEBUG] Error: Could not access Media Roster.\n");
        return B_ERROR;
    }

    // 2. Initialize and Register Input Node
    inNode = new RakInputNode(); 
    status_t regErr = roster->RegisterNode(inNode);
    if (regErr != B_OK) {
        printf("[DEBUG] Node registration failed: %s\n", strerror(regErr));
        return regErr;
    }
    printf("[DEBUG] RakInputNode registered. Node ID: %ld\n", inNode->Node().node);

    // 3. Connect Hardware
    if (ConnectHardwareToRakarrack() != B_OK) {
        printf("[DEBUG] Auto-connect failed. Manual link in Cortex may be required.\n");
    }

    // 4. TimeSource Management
    // Get the preferred time source for this node's hardware/chain
    BTimeSource* timeSource = roster->MakeTimeSourceFor(inNode->Node());
    
    // Fallback to system master clock if needed
    if (timeSource == NULL) {
        media_node tsNode;
        roster->GetTimeSource(&tsNode);
        timeSource = roster->MakeTimeSourceFor(tsNode);
    }

    if (timeSource) {
        // Slaving the node to the clock: Essential for BMediaEventLooper stability
        roster->SetTimeSourceFor(inNode->Node().node, timeSource->Node().node);
        
        bigtime_t real = system_time(); 
        roster->StartTimeSource(timeSource->Node(), real);
        
        bigtime_t initLatency = 0;
        roster->GetInitialLatencyFor(inNode->Node(), &initLatency);
        
        // Start the node slightly in the future to allow buffers to fill
        bigtime_t start_at = timeSource->Now() + initLatency + 50000;
        
        printf("[DEBUG] Starting Input Node at: %lld\n", start_at);
        roster->StartNode(inNode->Node(), start_at);
        
        // The Roster now manages the node's relationship with the clock.
        // We release our local reference to the object.
        timeSource->Release();
    } else {
        printf("[DEBUG] Error: No valid TimeSource found. Node may stay in 'stopped' state.\n");
    }
// Allocate 4 seconds of buffer to be safe
if (!rbLeft) rbLeft = new SimpleRingBuffer(48000 * 4);
if (!rbRight) rbRight = new SimpleRingBuffer(48000 * 4);

    // 5. Output Player
    outPlayer = new BSoundPlayer(&format, "Rakarrack-Out", HaikuAudioCallback, NULL, (void*)rkr_);
    outPlayer->Start();
    outPlayer->SetHasData(true);
    printf("[DEBUG] Haiku BSoundPlayer started.\n");      
  
    return B_OK;
}


int jackprocess (jack_nframes_t nframes, void *arg)
{
    int i, count;
    jack_midi_event_t midievent;
    jack_position_t pos;
    jack_transport_state_t astate;

    // 1. OUTPUT buffers (These stay as they are)
    jack_default_audio_sample_t *outl = (jack_default_audio_sample_t *)jack_port_get_buffer (outport_left, nframes);
    jack_default_audio_sample_t *outr = (jack_default_audio_sample_t *)jack_port_get_buffer (outport_right, nframes);

    // 2. INPUT buffers (Point these to your GLOBAL hardware buffers instead of JACK ports)
    float *inl = hardware_in_L;
    float *inr = hardware_in_R;
    
    // Aux still needs a definition to avoid memcpy errors below
    jack_default_audio_sample_t *aux = (jack_default_audio_sample_t *)jack_port_get_buffer (inputport_aux, nframes);

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
  
      // Temporary local buffers for the engine
    float process_in_L[nframes];
    float process_in_R[nframes];

 // START LOCK
    pthread_mutex_lock (&jmutex);

    // READ from Ring Buffer
    if (rbLeft && rbLeft->Available() >= nframes) {
        rbLeft->Read(process_in_L, nframes);
        rbRight->Read(process_in_R, nframes);
    } else {
        // Not enough data? Silence input to avoid garbage noise
        memset(process_in_L, 0, sizeof(float) * nframes);
        memset(process_in_R, 0, sizeof(float) * nframes);
    }

    // 3. MIDI DATA - Needs to be defined for 'count' to work
    void *data = jack_port_get_buffer(jack_midi_in, nframes); 
    count = jack_midi_get_event_count(data);

    void *dataout = jack_port_get_buffer(jack_midi_out, nframes); 
    jack_midi_clear_buffer(dataout);   	

    for (i = 0; i < count; i++) {                  
        jack_midi_event_get(&midievent, data, i);
        JackOUT->jack_process_midievents(&midievent);
    }  

    for (i=0; i<=JackOUT->efx_MIDIConverter->ev_count; i++) { 
        jack_midi_event_write(dataout,
            JackOUT->efx_MIDIConverter->Midi_event[i].time,
            JackOUT->efx_MIDIConverter->Midi_event[i].dataloc,
            JackOUT->efx_MIDIConverter->Midi_event[i].len);
    }

    JackOUT->efx_MIDIConverter->moutdatasize = 0;
    JackOUT->efx_MIDIConverter->ev_count = 0;

    // 4. RUN THE EFFECTS ENGINE
    // Use the data currently in our global 'inl/inr' (filled by BufferReceived)
    // CORRECTED
	JackOUT->Alg (JackOUT->efxoutl, JackOUT->efxoutr, process_in_L, process_in_R, nframes);


    // 5. COPY TO OUTPUT
    memcpy (outl, JackOUT->efxoutl, sizeof (jack_default_audio_sample_t) * nframes);
    memcpy (outr, JackOUT->efxoutr, sizeof (jack_default_audio_sample_t) * nframes);
  
    // IMPORTANT: UNLOCK
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
    printf("DEBUG: Entering HaikuAudioShutdown...\n");

    // 1. Tell players to stop but DON'T block
    if (outPlayer) { 
        printf("DEBUG: Stopping outPlayer (non-blocking)...\n");
        outPlayer->Stop(false); // false = don't block
    }
   // if (inPlayer) { 
   //     printf("DEBUG: Stopping inPlayer (non-blocking)...\n");
   //     inPlayer->Stop(false); 
   // }

    BMediaRoster* roster = BMediaRoster::Roster();
    if (roster) {
        printf("DEBUG: MediaRoster found.\n");

        // 2. Disconnect and Stop Nodes ASYNCHRONOUSLY
        // If gInputNode is the problem, we skip the blocking StopNode
        if (gInputNode.node > 0) {
            printf("DEBUG: Disconnecting nodes...\n");
            // Disconnect first to break the audio loop
            roster->Disconnect(gInputOutput.node.node, gInputOutput.source, 
                               gPlayerInput.node.node, gPlayerInput.destination);
            
            printf("DEBUG: Stopping nodes (asynchronous)...\n");
            roster->StopNode(gInputNode, 0, false); // false = don't wait for reply
            roster->StopNode(gPlayerNode, 0, false);
        }

        // 3. Skip Unregistering if it's hanging
        // Many Haiku apps rely on the OS to clean up nodes on exit
        // to avoid this exact hang.
    }

    // 4. Force delete the players
    if (outPlayer) { 
        printf("DEBUG: Deleting outPlayer...\n");
        delete outPlayer; 
        outPlayer = NULL; 
    }
  //  if (inPlayer) { 
   //     delete inPlayer; 
   //     inPlayer = NULL; 
   // }

    printf("DEBUG: Cleanup phase complete.\n");
}





