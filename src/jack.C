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
#include <math.h>
#include <Alert.h>

// Global Debug Flag (Default to OFF)
bool gDebugMode = false;

// X11 Conflict Fix: Include X11 then UNDEF its macros immediately
#include <X11/Xlib.h>
#ifdef CurrentTime
  #undef CurrentTime
#endif

#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)

// Media Kit & System
#include <media/SoundPlayer.h>
#include <media/MediaRoster.h>

#include "jack.h"
#include "global.h"
#include "config.h"

#include <unistd.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <OS.h>

#include <media/BufferConsumer.h>
#include <media/Buffer.h>
#include <media/MediaNode.h>

#include <media/BufferGroup.h>
#include <MidiProducer.h>
#include <MidiConsumer.h>
#include <xmmintrin.h>

// Forward Declarations
status_t ConnectHardwareToRakarrack();

// Prototypes
void HaikuAudioCallback(void *cookie, void *buffer, size_t size, const media_raw_audio_format &format);
void HaikuRecordCallback(void *cookie, void *buffer, size_t size, const media_raw_audio_format &format);
int jackprocess (jack_nframes_t nframes, void *arg);

// Globals (Only define these ONCE here)
extern float input_buffer_L[8192];
extern float input_buffer_R[8192];
extern float temp_buffer_L[8192];
extern float temp_buffer_R[8192];

class RakInputNode; // Forward declaration
static RakInputNode *inNode = NULL; 
static BSoundPlayer *outPlayer = NULL;
//static BSoundPlayer *inPlayer = NULL;
extern pthread_mutex_t jmutex;
extern RKR *JackOUT;
extern "C" RKR *rk;





//extern float* current_haiku_buffer;

// Persistent handles for shutdown
media_node   gInputNode;
media_node   gPlayerNode;
media_output gInputOutput;
media_input  gPlayerInput;


class SimpleRingBuffer {
public:
    float *buffer;
    int size;
    // Use atomic-style access for positions to prevent race conditions
    volatile int writePos;
    volatile int readPos;

    SimpleRingBuffer(int sz) {
        size = sz;
        buffer = new float[size];
        memset(buffer, 0, size * sizeof(float));
        writePos = 0;
        readPos = 0;
        if (gDebugMode) {
        printf("[DEBUG] RingBuffer Initialized. Size: %d frames\n", size);
        }
    }

    ~SimpleRingBuffer() {
        delete[] buffer;
    }

    // Standard Write (for non-interleaved data)
    void Write(float* data, int frames) {
   	 int wp = writePos; 
   	 for (int i = 0; i < frames; i++) {
        buffer[wp] = data[i];
        wp++;
        if (wp >= size) wp = 0;
  	    }
            __sync_synchronize(); // Change to this
            writePos = wp;         
    }

    // Specialized Interleaved Write (for [L, R, L, R] hardware)
    void WriteInterleaved(float* data, int frames, int channelOffset) {
   		int wp = writePos;
   		for (int i = 0; i < frames; i++) {
        buffer[wp] = data[i * 2 + channelOffset];
        wp++;
        if (wp >= size) wp = 0;
 		   }
            __sync_synchronize(); // Change to this
            writePos = wp; 
      }


    // Read data and ensure we don't read past the writePos
    void Read(float* dest, int frames) {
        for (int i = 0; i < frames; i++) {
            dest[i] = buffer[readPos];
            int next = (readPos + 1) % size;
            readPos = next;
        }
    }
    
    // Returns how many frames are ready to be read
    int Available() {
        int w = writePos;
        int r = readPos;
        int diff = w - r;
        if (diff < 0) diff += size;
        return diff;
    }

    // New: Check how much space is left to write (to prevent overruns)
    int FreeSpace() {
        return (size - 1) - Available();
    }
  
    // Clear the buffer (useful for stopping/starting effects)
    void Clear() {
        writePos = 0;
        readPos = 0;
        memset(buffer, 0, size * sizeof(float));
    }
};

// 1. Raw Guitar Input (Filled by RakInputNode)
SimpleRingBuffer* rbInputLeft = NULL;
SimpleRingBuffer* rbInputRight = NULL;

// 2. Processed Audio (Filled by jackprocess, read by HaikuAudioCallback)
SimpleRingBuffer* rbOutputLeft = NULL;
SimpleRingBuffer* rbOutputRight = NULL;

// Inherit ONLY from BBufferConsumer (which already includes BMediaNode)
class RakInputNode : public BBufferConsumer, public BMediaEventLooper {
public:
    RakInputNode() 
        : BBufferConsumer(B_MEDIA_RAW_AUDIO), 
          BMediaNode("Rakarrack-In"),
          BMediaEventLooper() 
    {
        AddNodeKind(B_BUFFER_CONSUMER); 
    }

    uint32 fInputFormat; 

    virtual ~RakInputNode() {
        // This stops the internal thread safely.
        // Without this, the Media Server thinks we are still running!
        BMediaEventLooper::Quit(); 
    }


    // --- 1. MESSAGE PUMP ---
    virtual status_t HandleMessage(int32 code, const void *data, size_t size) {
        if (BBufferConsumer::HandleMessage(code, data, size) == B_OK) return B_OK;
        if (BMediaEventLooper::HandleMessage(code, data, size) == B_OK) return B_OK;
        return BMediaNode::HandleMessage(code, data, size);
    }

    // --- 2. LATENCY & RUN MODE ---
   // Inside class RakInputNode : public ...
    virtual status_t GetLatencyFor(const media_destination&, bigtime_t* out_latency, media_node_id* out_timesource) {
        // Report 1ms (1000 microseconds) for snappy response
    	 *out_latency = 1000; 
    
    	// Always provide a valid ID
  		  if (TimeSource()) 
        	*out_timesource = TimeSource()->ID();
    	else 
       		 *out_timesource = 0;
        
    	return B_OK;
    }

    virtual BMediaNode::run_mode RunMode() { return B_RECORDING; }

    // --- 3. LIFECYCLE ---
    virtual void NodeRegistered() {
        Run(); 
    }

    virtual void HandleEvent(const media_timed_event* event, bigtime_t lateness, bool realTimeEvent = false) {}
    virtual port_id ControlPort() const { return BMediaEventLooper::ControlPort(); }
    virtual void Preroll() {}
    virtual BMediaAddOn* AddOn(int32* internalID) const { return NULL; }

    // --- 4. CONNECTION LOGIC ---
    virtual status_t GetNextInput(int32* cookie, media_input* out_input) {
        if (*cookie != 0) return B_ENTRY_NOT_FOUND; 
        out_input->node = Node();
        out_input->destination.port = ControlPort();
        out_input->destination.id = 0; 
        out_input->source = media_source::null;
        sprintf(out_input->name, "Guitar In");
        out_input->format.type = B_MEDIA_RAW_AUDIO;
        out_input->format.u.raw_audio = media_raw_audio_format::wildcard;
        *cookie = 1; 
        return B_OK;
    }

    virtual status_t AcceptFormat(const media_destination& dest, media_format* format) {
        if (format->type != B_MEDIA_RAW_AUDIO) return B_MEDIA_BAD_FORMAT;
        return B_OK; 
    }

    virtual status_t Connected(const media_source& source, const media_destination& dest, const media_format& format, media_input* out_input) {
        out_input->node = Node();
        out_input->source = source;
        out_input->destination = dest;
        out_input->format = format;
        fInputFormat = format.u.raw_audio.format; 
         if (gDebugMode) {
      		  printf("[Rakarrack] Connected! Format ID: %u\n", fInputFormat);
         }
        return B_OK; 
    }

    virtual void Disconnected(const media_source&, const media_destination&) {}
    virtual status_t FormatChanged(const media_source&, const media_destination&, int32, const media_format&) { return B_OK; }

    // --- 5. AUDIO CAPTURE (UPDATED TO rbInput) ---
	virtual void BufferReceived(BBuffer *buffer) {
    if (!buffer || !rbInputLeft || !rbInputRight) return;

    size_t bytes = buffer->SizeUsed();
    void* rawData = buffer->Data();
    
    // --- 32-bit INT SECTION ---
    if (fInputFormat == 0x4) { 
        int32* data = (int32*)rawData;
        int frames = bytes / (2 * sizeof(int32));            
        
        const float kReciprocal = 1.0f / 2147483648.0f;
        float totalGain = kReciprocal * 1.0f; // Force gain to 1.0f for testing

        for (int i = 0; i < frames; i++) {
            float valL = (float)data[i * 2] * totalGain;
            float valR = (float)data[i * 2 + 1] * totalGain;

            rbInputLeft->buffer[rbInputLeft->writePos] = valL;
            rbInputRight->buffer[rbInputRight->writePos] = valR;
            
            // Sync before moving the pointer
            __sync_synchronize(); 

            int next = (rbInputLeft->writePos + 1) % rbInputLeft->size;
            rbInputLeft->writePos = next;
            rbInputRight->writePos = next;
        }
    }
    // --- 32-bit FLOAT SECTION ---
    else if (fInputFormat == 0x24) { 
        float* data = (float*)rawData;
        int frames = bytes / (2 * sizeof(float));
        
        rbInputLeft->WriteInterleaved(data, frames, 0); 
        rbInputRight->WriteInterleaved(data, frames, 1); 
    }
    // --- 16-bit FALLBACK ---
    else { 
        int16* data = (int16*)rawData;
        int frames = bytes / (2 * sizeof(int16));
        const float k16Recip = 1.0f / 32768.0f;

        for (int i = 0; i < frames; i++) {
            rbInputLeft->buffer[rbInputLeft->writePos] = (float)data[i * 2] * k16Recip;
            rbInputRight->buffer[rbInputRight->writePos] = (float)data[i * 2 + 1] * k16Recip;
            
            __sync_synchronize();

            int next = (rbInputLeft->writePos + 1) % rbInputLeft->size;
            rbInputLeft->writePos = next;
            rbInputRight->writePos = next;
        }
    }

    // 2. DEBUG PROBE (Only when needed)
    if (gDebugMode) {
        static int input_log = 0;
        if (input_log++ % 500 == 0) { // Increased to 500 to reduce console lag
            printf("[INPUT] Available: %d frames\n", rbInputLeft->Available());
        }
    }

    buffer->Recycle();
}

    virtual void ProducerDataStatus(const media_destination& for_whom, int32 status, bigtime_t at_performance_time) {}
    virtual void DisposeInputCookie(int32) {}
    virtual status_t GetNodeAttributes(media_node_attribute* out_attributes, size_t in_max_count, size_t* out_count) {
        if (out_count) *out_count = 0;
        return B_OK;
    }
};

	// The Record Hook: Grabs guitar from Haiku and puts it in Rakarrack's "In"
	void HaikuRecordCallback(void *cookie, void *buffer, size_t size, const media_raw_audio_format &format) {
    if (buffer == NULL || rbInputLeft == NULL || rbInputRight == NULL) return;

    // Use the incoming buffer from the hardware
    	float* incoming = (float*)buffer;
   	 	uint32_t nframes = size / (sizeof(float) * 2); 
   		 if (nframes > 8192) nframes = 8192; // Match your global array size

   	 // 1. Copy from hardware into your existing Globals
    	for (uint32_t i = 0; i < nframes; i++) {
        input_buffer_L[i] = incoming[i * 2];
        input_buffer_R[i] = incoming[i * 2 + 1];
    	}

    // 2. Push from Globals into the Ring Buffers for the engine to use
   	 rbInputLeft->Write(input_buffer_L, nframes);
   	 rbInputRight->Write(input_buffer_R, nframes);
	}


	// The Play Hook: Processes and then Interleaves to Speakers
	void HaikuAudioCallback(void *cookie, void *buffer, size_t size, const media_raw_audio_format &format) {
  	  if (!rbOutputLeft || !rbOutputRight) {
    	    memset(buffer, 0, size);
   	     return;
   	 }

    uint32 type = format.format; 
    size_t sampleSize = (type == 0x24) ? sizeof(float) : sizeof(int16);
    size_t frames = size / (2 * sampleSize); // <--- This defines 'frames'
    
        /* Uncomment if needed  // Needs update - Reading from empty frames.
        if (gDebugMode) {
        static int out_log = 0;
        if (out_log++ % 50 == 0) {
        printf("[Debug-Output] Reading from Engine... Available Frames: %d\n", rbOutputLeft->Available());
    		}
        }
        */
        if (gDebugMode) {
   		 uint32_t realFrames = size / (format.channel_count * sampleSize);
        static int size_check_count = 0;
        if (size_check_count++ % 500 == 0) {
            printf("[Rakarrack] Buffer Size: %zu bytes | Frames: %u\n", size, realFrames);
          }
       }
       
    // 1. ONE-TIME LATENCY KILLER
    static bool firstRun = true;
    if (firstRun && rbInputLeft->Available() > 512) {
        // Just once, jump the read pointer to the most recent data
        int lookback = frames * 2; // Keep a tiny safety margin of 2 buffers
        rbInputLeft->readPos = (rbInputLeft->writePos - lookback + rbInputLeft->size) % rbInputLeft->size;
        rbInputRight->readPos = (rbInputRight->writePos - lookback + rbInputRight->size) % rbInputRight->size;
        
        firstRun = false; // Never run this logic again!
        printf("[Rakarrack] Initial backlog cleared. Now at live edge.\n");
    }


    jackprocess(frames, NULL);

    pthread_mutex_lock(&jmutex);
    
    if (type == 0x24) { // Float
        float* outBuffer = (float*)buffer;
        for (size_t i = 0; i < frames; i++) {
            outBuffer[i * 2]     = JackOUT->efxoutl[i];
            outBuffer[i * 2 + 1] = JackOUT->efxoutr[i];
        }
    } else { // Short
        int16* outBuffer = (int16*)buffer;
        for (size_t i = 0; i < frames; i++) {
            outBuffer[i * 2]     = (int16)(JackOUT->efxoutl[i] * 32767.0f);
            outBuffer[i * 2 + 1] = (int16)(JackOUT->efxoutr[i] * 32767.0f);
        }
    }
    // Probe the FIRST sample of the Left channel
	float probeSample = 0.0f;
	if (type == 0x24) probeSample = ((float*)buffer)[0];
	else probeSample = ((int16*)buffer)[0] / 32767.0f;
		/*  Uncomment if needed
	 	if (gDebugMode) {
			static int out_log_count = 0;
				if (out_log_count++ % 50 == 0) {
    				printf("[Debug-Out-Check] Sending to Speaker: %.6f\n", probeSample);
					}
 				}
 				*/
    pthread_mutex_unlock(&jmutex);
}


extern "C" {

    char** jack_get_ports(jack_client_t *, const char *, const char *, unsigned long);
    void jack_free(void *);    
    jack_port_t* jack_port_register(jack_client_t*, const char*, const char*, unsigned long, unsigned long); 
    char** jack_get_ports(jack_client_t *, const char *, const char *, unsigned long);
    void jack_free(void *);
}

jack_client_t *jackclient;
jack_port_t *outport_left, *outport_right;
jack_port_t *inputport_left, *inputport_right, *inputport_aux;
jack_port_t *jack_midi_in, *jack_midi_out;
//void *dataout; // delete ?

int jackprocess (jack_nframes_t nframes, void *arg);

extern "C" bigtime_t estimate_max_scheduling_latency();

int JACKstart(RKR * rkr_, jack_client_t * jackclient_) {
    JackOUT = rkr_;
    pthread_mutex_init(&jmutex, NULL);
 
     // --- LOAD SAVED HAIKU SETTINGS ---
    char saved_rate[32], saved_frames[32];
    Fl_Preferences rkr_prefs(Fl_Preferences::USER, "rakarrack.sf.net", "rakarrack");
    
    // Default to your haiku.make values if nothing is saved yet
	rkr_prefs.get("Haiku_SampleRate", saved_rate, STR(DEFAULT_FRAME_RATE), 32);
	rkr_prefs.get("Haiku_BufferSize", saved_frames, STR(DEFAULT_BUFFER_FRAMES), 32);

    int final_rate = atoi(saved_rate);
    int final_frames = atoi(saved_frames);
    
    // 1. Initialize Ring Buffers using the loaded rate
    uint32_t bufferSize = (uint32_t)final_rate * 2;

	if (!rbInputLeft)   rbInputLeft   = new SimpleRingBuffer(bufferSize);
	if (!rbInputRight)  rbInputRight  = new SimpleRingBuffer(bufferSize);
	if (!rbOutputLeft)  rbOutputLeft  = new SimpleRingBuffer(bufferSize);
	if (!rbOutputRight) rbOutputRight = new SimpleRingBuffer(bufferSize);


    // 2. Setup Output Format
    media_raw_audio_format format;
    memset(&format, 0, sizeof(format)); 
    format.format = media_raw_audio_format::B_AUDIO_FLOAT;
    format.channel_count = 2; 
    format.frame_rate = (float)final_rate; 
    format.byte_order = B_MEDIA_HOST_ENDIAN;
    format.buffer_size = final_frames * sizeof(float) * 2; 		
    
    // Latency sniffer
  
	float latency_ms = ((float)final_frames / (float)final_rate) * 1000.0f;
	


    // 3. Register Input Node
    BMediaRoster* roster = BMediaRoster::Roster();
    inNode = new RakInputNode(); 
    roster->RegisterNode(inNode);

    // Connect hardware
    ConnectHardwareToRakarrack();

    // --- "START ASAP" TIMING FIX ---
    BTimeSource* timeSource = roster->MakeTimeSourceFor(inNode->Node());
    if (timeSource) {
        roster->SetTimeSourceFor(inNode->Node().node, timeSource->Node().node);
        
        // Ensure Time Source is running
        if (!timeSource->IsRunning()) {
             roster->StartTimeSource(timeSource->Node(), system_time());
             snooze(5000); // Small breath to let it tick
        }
        
        // STRATEGY: Pass 0 ("Start Now") to avoid "Performance Time Too Large" crashes.
        // The MediaRoster will calculate the best time automatically.
        roster->StartNode(inNode->Node(), 0);
        
        // Only start the Hardware Node if it's NOT the Time Source 
        // (If it is the Time Source, it's already running!)
        if (gInputNode.node > 0 && gInputNode.node != timeSource->Node().node) {
            roster->StartNode(gInputNode, 0);
            printf("[Rakarrack] Started Hardware Node (Asynchronous)\n");
        } else {
            printf("[Rakarrack] Hardware Node is the Time Source (Already Running)\n");
        }

        timeSource->Release();
    }
    // -------------------------------

    // 4. Start Output Player
    outPlayer = new BSoundPlayer(&format, "Rakarrack-Out", HaikuAudioCallback, NULL, (void*)rkr_);
    outPlayer->Start();
    outPlayer->SetHasData(true);
    
    printf("[Rakarrack] Audio Engine started.\n");
    return B_OK;
}


int jackprocess (jack_nframes_t nframes, void *arg)
{
	
	// Start the clock
    bigtime_t start_time = system_time();
    
    
	_mm_setcsr(_mm_getcsr() | 0x8040); // Sets DAZ (Denormals-Are-Zero) and FTZ (Flush-To-Zero)
	
    static float process_in_L[8192];
    static float process_in_R[8192];

    // Safety Clamp
    if (nframes > 8192) nframes = 8192;

    // 1. READ INPUT
    pthread_mutex_lock(&jmutex);
    if (rbInputLeft && rbInputRight && rbInputLeft->Available() >= (int)nframes) {
        rbInputLeft->Read(process_in_L, nframes);
        rbInputRight->Read(process_in_R, nframes);
    } else {
        memset(process_in_L, 0, sizeof(float) * nframes);
        memset(process_in_R, 0, sizeof(float) * nframes);
    }
    pthread_mutex_unlock(&jmutex);

    // 2. NAN GUARD
    for (int i = 0; i < (int)nframes; i++) {
        if (isnan(process_in_L[i])) process_in_L[i] = 0.0f;
        if (isnan(process_in_R[i])) process_in_R[i] = 0.0f;
    }

    // 3. MIDI PROCESSING
    void *mididata_in = jack_port_get_buffer(jack_midi_in, nframes); 
    void *mididata_out = jack_port_get_buffer(jack_midi_out, nframes); 
    int count = jack_midi_get_event_count(mididata_in);
    jack_midi_event_t midievent;
    jack_midi_clear_buffer(mididata_out);
    for (int i = 0; i < count; i++) {                  
        jack_midi_event_get(&midievent, mididata_in, i);
        JackOUT->jack_process_midievents(&midievent);
    }  

    // 4. RUN EFFECTS ENGINE 
    // We use 'nframes' so the engine actually loops!
	for (int i = 0; i < (int)nframes; i++) {
    // Clean up "ghost" signals (Denormals) that cause static
    if (fabs(process_in_L[i]) < 1e-15f) process_in_L[i] = 0.0f;
    if (fabs(process_in_R[i]) < 1e-15f) process_in_R[i] = 0.0f;

    JackOUT->efxoutl[i] = process_in_L[i];
    JackOUT->efxoutr[i] = process_in_R[i];
	}   

    JackOUT->Alg(JackOUT->efxoutl, JackOUT->efxoutr, process_in_L, process_in_R, nframes); 
    
    // Stop the clock
    bigtime_t end_time = system_time();

    // 5. CALCULATE LOAD
    // Available time for this block in microseconds
    uint32_t current_rate = jack_get_sample_rate(NULL);
    
    // Safety fallback in case the function returns 0 (prevents divide-by-zero crash)
    if (current_rate == 0) {
        current_rate = DEFAULT_FRAME_RATE;
    }

    double max_us = ((double)nframes / (double)current_rate) * 1000000.0;
   
    float instant_load = ((float)(end_time - start_time) / max_us) * 100.0f;

    // Smooth the result so the UI doesn't flicker (90% old, 10% new)
    JackOUT->cpuload = (JackOUT->cpuload * 0.9f) + (instant_load * 0.1f);

    return 0;
}


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

status_t ConnectHardwareToRakarrack() {
    BMediaRoster* roster = BMediaRoster::Roster();
    if (!roster || !inNode) return B_ERROR;

    // 1. Find Hardware
    status_t err = roster->GetAudioInput(&gInputNode);
    if (err != B_OK) return err;
    printf("[Rakarrack] Hardware Input Node: %d\n", (int)gInputNode.node);

    // 2. Check for Busy Pins (Basic Recovery)
    media_output hardwareOut;
    int32 count = 0;
    err = roster->GetFreeOutputsFor(gInputNode, &hardwareOut, 1, &count, B_MEDIA_RAW_AUDIO);

    if (err != B_OK || count == 0) {
        printf("[Rakarrack] WARNING: Pins busy. Attempting to find free pin...\n");
        // Try to find ANY free pin, even if the default one is taken
        media_output outputs[16];
        int32 numOut = 0;
        if (roster->GetAllOutputsFor(gInputNode, outputs, 16, &numOut) == B_OK) {
            for (int i = 0; i < numOut; i++) {
                if (outputs[i].destination.port == 0) { // Found a free one!
                    hardwareOut = outputs[i];
                    count = 1;
                    err = B_OK;
                    break;
                }
            }
        }
    }

    if (count == 0) {
        printf("[Rakarrack] ERROR: All hardware pins are blocked. Please restart Media Server.\n");
               // Trigger Haiku Alert
        BAlert* alert = new BAlert("Media Server Busy", 
            "All hardware audio pins are currently blocked by the Media Server.\n\n"
            "Would you like to attempt a restart of the Media Services?\n\n"
            "If so, please allow ~5 seconds for the Media Service to restart before starting the app.\n\n",
            "Cancel", "Restart Media Server", NULL, B_WIDTH_AS_USUAL, B_STOP_ALERT);

        // alert->Go() returns the index of the button clicked (0 for Cancel, 1 for Restart)
			if (alert->Go() == 1) {
    printf("[Rakarrack] Cycling Media Services...\n");
    
    // 1. Send the quit signal
    system("hey media_server quit > /dev/null 2>&1");
    
    // 2. Brutally kill the addon server (usually the one holding the pins)
    system("kill media_addon_server > /dev/null 2>&1");

    // 3. Start the server back up in the background
    // We use 'nohup' or '&' to make sure it outlives Rakarrack
    system("/boot/system/servers/media_server &");
    
    // 4. EXIT IMMEDIATELY
    // Do not show another BAlert here, because the BApplication/Roster 
    // connection is already broken and will cause "Bad port ID" errors.
    printf("[Rakarrack] Media server restarting... Please allow ~5 seconds before relaunching the app.\n");
    exit(0); 
		}

        return B_BUSY;
    }

    // 3. Find Rakarrack Input
    media_input rakIn;
    err = roster->GetFreeInputsFor(inNode->Node(), &rakIn, 1, &count, B_MEDIA_RAW_AUDIO);
    if (err != B_OK) return err;

    // 4. Connect
    media_format format;
    format.type = B_MEDIA_RAW_AUDIO;
    format.u.raw_audio = media_raw_audio_format::wildcard;
    
    err = roster->Connect(hardwareOut.source, rakIn.destination, &format, &hardwareOut, &rakIn);
    if (err != B_OK) {
        printf("[Rakarrack] Connect failed: %s\n", strerror(err));
        return err;
    }

    // 5. Start Hardware
    BTimeSource* ts = roster->MakeTimeSourceFor(gInputNode);
    if (ts) {
        roster->StartTimeSource(ts->Node(), system_time());
        roster->StartNode(gInputNode, ts->Now());
        ts->Release();
    }

    return B_OK;
}


// Shutdown

extern "C" void HaikuAudioShutdown() {
    printf("Rakarrack: Entering HaikuAudioShutdown...\n");
    BMediaRoster* roster = BMediaRoster::Roster();
    if (!roster) return;

    // 1. FORCE DISCONNECT FIRST
    if (inNode && gInputNode.node > 0) {
        media_input connectedInput;
        int32 inputCount = 0;
        media_node myNode = inNode->Node();

        // Attempt to find the link from our side
        if (roster->GetConnectedInputsFor(myNode, &connectedInput, 1, &inputCount) == B_OK && inputCount > 0) {
            printf("Rakarrack: Breaking active link via our input...\n");
            roster->Disconnect(gInputNode.node, connectedInput.source, myNode.node, connectedInput.destination);
        } else {
            // Fallback: Search from the Hardware side
            printf("Rakarrack: Searching via Hardware Node %d...\n", (int)gInputNode.node);
            media_output hwOutputs[8]; // Array for multiple outputs
            int32 hwOutCount = 0;
            if (roster->GetConnectedOutputsFor(gInputNode, hwOutputs, 8, &hwOutCount) == B_OK && hwOutCount > 0) {
                for (int i = 0; i < hwOutCount; i++) {
                    // Match by the 'port' ID which is the unique destination identifier
                    if (hwOutputs[i].destination.port == inNode->ControlPort()) {
                         printf("Rakarrack: Found hardware output! Severing link...\n");
                         roster->Disconnect(hwOutputs[i].node.node, hwOutputs[i].source, myNode.node, hwOutputs[i].destination);
                    }
                }
            }
        }
    }
    
    printf("Rakarrack: Cleaning up MIDI resources...\n");


// 1. MIDI DISCONNECT
    if (rk) {
        rk->MidiShutdown();
    }


    // 2. STOP AND RELEASE
    if (outPlayer) {
        printf("Rakarrack: Stopping outPlayer (Sync)...\n");
        outPlayer->Stop(true, true); // Synchronous wait
        delete outPlayer;
        outPlayer = NULL;
    }
	
    if (inNode) {
        printf("Rakarrack: Stopping and Releasing inNode...\n");
        roster->StopNode(inNode->Node(), 0, true);
        inNode->Release(); // Incremental cleanup
        inNode = NULL;
    }
    
    printf("Rakarrack: Shutdown complete.\n");
}

