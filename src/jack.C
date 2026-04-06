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

// Forward Declarations
status_t ConnectHardwareToRakarrack();
void HaikuAudioCallback(void *cookie, void *buffer, size_t size, const media_raw_audio_format &format);



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
    // Use atomic-style access for positions to prevent race conditions
    volatile int writePos;
    volatile int readPos;

    SimpleRingBuffer(int sz) {
        size = sz;
        buffer = new float[size];
        memset(buffer, 0, size * sizeof(float));
        writePos = 0;
        readPos = 0;
        printf("[DEBUG] RingBuffer Initialized. Size: %d frames\n", size);
    }

    ~SimpleRingBuffer() {
        delete[] buffer;
    }

    // Write data and ensure the writePos is updated only after data is in memory
    void Write(float* data, int frames) {
        for (int i = 0; i < frames; i++) {
            buffer[writePos] = data[i];
            // Atomic-style wrap around
            int next = (writePos + 1) % size;
            writePos = next;
        }
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

    // --- 1. MESSAGE PUMP ---
    virtual status_t HandleMessage(int32 code, const void *data, size_t size) {
        if (BBufferConsumer::HandleMessage(code, data, size) == B_OK) return B_OK;
        if (BMediaEventLooper::HandleMessage(code, data, size) == B_OK) return B_OK;
        return BMediaNode::HandleMessage(code, data, size);
    }

    // --- 2. LATENCY & RUN MODE ---
// Inside class RakInputNode : public ...
virtual status_t GetLatencyFor(const media_destination&, bigtime_t* out_latency, media_node_id* out_timesource) {
    // Report a small, valid latency (10ms) so the Media Server allows the connection
    *out_latency = 10000; 
    
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
        printf("[DEBUG] Connected! Format ID: %u\n", fInputFormat);
        return B_OK; 
    }

    virtual void Disconnected(const media_source&, const media_destination&) {}
    virtual status_t FormatChanged(const media_source&, const media_destination&, int32, const media_format&) { return B_OK; }

    // --- 5. AUDIO CAPTURE (UPDATED TO rbInput) ---
    virtual void BufferReceived(BBuffer *buffer) {
        if (!buffer || !rbInputLeft || !rbInputRight) return;

        size_t bytes = buffer->SizeUsed();
        void* rawData = buffer->Data();
        
        // 1. LOCK and WRITE to Input Buffer
        pthread_mutex_lock(&jmutex);

        if (fInputFormat == 0x4) { // 32-bit Int
            int32* data = (int32*)rawData;
            int frames = bytes / (2 * sizeof(int32));
            for (int i = 0; i < frames; i++) {
                rbInputLeft->buffer[rbInputLeft->writePos] = (float)data[i * 2] / 2147483648.0f;
                rbInputLeft->writePos = (rbInputLeft->writePos + 1) % rbInputLeft->size;
                rbInputRight->buffer[rbInputRight->writePos] = (float)data[i * 2 + 1] / 2147483648.0f;
                rbInputRight->writePos = (rbInputRight->writePos + 1) % rbInputRight->size;
            }
        } 
        else if (fInputFormat == 0x24) { // 32-bit Float
            float* data = (float*)rawData;
            int frames = bytes / (2 * sizeof(float));
            for (int i = 0; i < frames; i++) {
                rbInputLeft->buffer[rbInputLeft->writePos] = data[i * 2];
                rbInputLeft->writePos = (rbInputLeft->writePos + 1) % rbInputLeft->size;
                rbInputRight->buffer[rbInputRight->writePos] = data[i * 2 + 1];
                rbInputRight->writePos = (rbInputRight->writePos + 1) % rbInputRight->size;
            }
        }
        else { // Fallback 16-bit
            int16* data = (int16*)rawData;
            int frames = bytes / (2 * sizeof(int16));
            for (int i = 0; i < frames; i++) {
                rbInputLeft->buffer[rbInputLeft->writePos] = (float)data[i * 2] / 32768.0f;
                rbInputLeft->writePos = (rbInputLeft->writePos + 1) % rbInputLeft->size;
                rbInputRight->buffer[rbInputRight->writePos] = (float)data[i * 2 + 1] / 32768.0f;
                rbInputRight->writePos = (rbInputRight->writePos + 1) % rbInputRight->size;
            }
        }

		 pthread_mutex_unlock(&jmutex); // UNLOCK before processing!
        
        // 2. TRIGGER THE ENGINE
        // If we have enough data (e.g., 256 frames), run the effects!
        // jackprocess handles its own locking, so we call it outside the lock.
        int block_size = 256; 
        while (rbInputLeft->Available() >= block_size) {
            jackprocess(block_size, NULL);
        }

        // Debug logging
        static int input_log = 0;
        if (input_log++ % 100 == 0) {
             // If this stays low, it means the engine is eating the data correctly!
            printf("[INPUT] WritePos: %d | Available: %d\n", rbInputLeft->writePos, rbInputLeft->Available());
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
    if (!rbOutputLeft || !rbOutputRight) {
        memset(buffer, 0, size);
        return;
    }

    uint32 type = format.format; 
    size_t sampleSize = (type == 0x24) ? sizeof(float) : sizeof(int16);
    size_t frames = size / (2 * sampleSize); // <--- This defines 'frames'
    
        static int out_log = 0;
    if (out_log++ % 50 == 0) {
        printf("[OUTPUT] Reading from Engine... Available Frames: %d\n", rbOutputLeft->Available());
    }

    if (rbOutputLeft->Available() < (int)frames) {
        memset(buffer, 0, size);
        return;
    }

    pthread_mutex_lock(&jmutex);

    if (type == 0x24) { // Float
        float* outBuffer = (float*)buffer;
        rbOutputLeft->Read(temp_buffer_L, frames);
        rbOutputRight->Read(temp_buffer_R, frames);

        for (size_t i = 0; i < frames; i++) {
            outBuffer[i * 2]     = temp_buffer_L[i];
            outBuffer[i * 2 + 1] = temp_buffer_R[i];
        }
    } else { // Short
        int16* outBuffer = (int16*)buffer;
        rbOutputLeft->Read(temp_buffer_L, frames);
        rbOutputRight->Read(temp_buffer_R, frames);

        for (size_t i = 0; i < frames; i++) {
            outBuffer[i * 2]     = (int16)(temp_buffer_L[i] * 32767.0f);
            outBuffer[i * 2 + 1] = (int16)(temp_buffer_R[i] * 32767.0f);
        }
    }

    pthread_mutex_unlock(&jmutex);
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
    
    // 1. Initialize Ring Buffers
    if (!rbInputLeft)   rbInputLeft   = new SimpleRingBuffer(48000);
    if (!rbInputRight)  rbInputRight  = new SimpleRingBuffer(48000);
    if (!rbOutputLeft)  rbOutputLeft  = new SimpleRingBuffer(48000);
    if (!rbOutputRight) rbOutputRight = new SimpleRingBuffer(48000);

    // 2. Setup Output Format
    media_raw_audio_format format;
    memset(&format, 0, sizeof(format)); 
    format.format = media_raw_audio_format::B_AUDIO_FLOAT;
    format.channel_count = 2; 
    format.frame_rate = 48000.0; 
    format.byte_order = B_MEDIA_HOST_ENDIAN;
    format.buffer_size = 512 * sizeof(float) * 2; 	

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
            printf("[DEBUG] Started Hardware Node (Asynchronous)\n");
        } else {
            printf("[DEBUG] Hardware Node is the Time Source (Already Running)\n");
        }

        timeSource->Release();
    }
    // -------------------------------

    // 4. Start Output Player
    outPlayer = new BSoundPlayer(&format, "Rakarrack-Out", HaikuAudioCallback, NULL, (void*)rkr_);
    outPlayer->Start();
    outPlayer->SetHasData(true);
    
    printf("[DEBUG] Rakarrack Audio Engine started.\n");
    return B_OK;
}




#include <math.h> // Ensure this is at the top for isnan()

int jackprocess (jack_nframes_t nframes, void *arg)
{
    // 1. Setup Local Buffers
    // Use static buffers to avoid stack overflow on large block sizes
    static float process_in_L[8192];
    static float process_in_R[8192];

    // Safety: If frames exceed our static buffer, clamp it (or skip)
    if (nframes > 8192) nframes = 8192;

    // 2. Read from Input Ring Buffer (Protected)
    pthread_mutex_lock(&jmutex);
    
    if (rbInputLeft && rbInputRight && rbInputLeft->Available() >= (int)nframes) {
        rbInputLeft->Read(process_in_L, nframes);
        rbInputRight->Read(process_in_R, nframes);
    } else {
        // Not enough data? Silence.
        memset(process_in_L, 0, sizeof(float) * nframes);
        memset(process_in_R, 0, sizeof(float) * nframes);
    }
    
    pthread_mutex_unlock(&jmutex);

    // 3. THE FIX: NaN / DC Guard
    // Scan the input. If we find garbage, silence it to save the engine.
    for (int i = 0; i < (int)nframes; i++) {
        // Fix Left
        if (isnan(process_in_L[i]) || isinf(process_in_L[i])) {
            process_in_L[i] = 0.0f;
        }
        // Fix Right
        if (isnan(process_in_R[i]) || isinf(process_in_R[i])) {
            process_in_R[i] = 0.0f;
        }
    }

    // 4. MIDI Processing (Keep your existing logic)
    void *mididata_in = jack_port_get_buffer(jack_midi_in, nframes); 
    void *mididata_out = jack_port_get_buffer(jack_midi_out, nframes); 
    int count = jack_midi_get_event_count(mididata_in);
    jack_midi_event_t midievent;

    jack_midi_clear_buffer(mididata_out);
    for (int i = 0; i < count; i++) {                  
        jack_midi_event_get(&midievent, mididata_in, i);
        JackOUT->jack_process_midievents(&midievent);
    }  
    // ... (Write back MIDI events from engine if needed) ...

    // 5. RUN THE EFFECTS ENGINE
    // Now safe to call because inputs are sanitized
    JackOUT->Alg(JackOUT->efxoutl, JackOUT->efxoutr, process_in_L, process_in_R, NULL);

    // 6. Write to Output Ring Buffer
    pthread_mutex_lock(&jmutex);
    
    if (rbOutputLeft && rbOutputRight) {
        // Sanitize Output too (Just in case the engine exploded internally)
        for (int i = 0; i < (int)nframes; i++) {
            if (isnan(JackOUT->efxoutl[i])) JackOUT->efxoutl[i] = 0.0f;
            if (isnan(JackOUT->efxoutr[i])) JackOUT->efxoutr[i] = 0.0f;
        }
        
        rbOutputLeft->Write(JackOUT->efxoutl, nframes);
        rbOutputRight->Write(JackOUT->efxoutr, nframes);
    }

    pthread_mutex_unlock(&jmutex);

    return 0;
}



void
JACKfinish ()
{
  jack_client_close (jackclient);
  pthread_mutex_destroy (&jmutex);
  usleep (100000);
};


/*
void
jackshutdown (void *arg)
{
  if (gui == 0)
    printf ("Jack Shut Down, sorry.\n");
  else
    JackOUT->Message (1,JackOUT->jackcliname,
		      "Jack Shut Down, try to save your work");
};

*/

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
    if (!roster) {
        printf("[MediaKit] Error: Could not get Media Roster!\n");
        return B_ERROR;
    }
    if (!inNode) {
        printf("[MediaKit] Error: RakInputNode is NULL!\n");
        return B_ERROR;
    }

    // 1. Get Hardware (Store in GLOBAL gInputNode)
    // Note: gInputNode must be declared as 'media_node gInputNode;' in your globals
    extern media_node gInputNode; 
    
    status_t err = roster->GetAudioInput(&gInputNode);
    if (err != B_OK) {
        printf("[MediaKit] Error: Could not find Physical Audio Input (0x%x)\n", (int)err);
        return err;
    }
    printf("[MediaKit] Found Hardware Input Node ID: %d\n", (int)gInputNode.node);

    // 2. Find Hardware Output Pin
    media_output hardwareOut;
    int32 count = 0;
    err = roster->GetFreeOutputsFor(gInputNode, &hardwareOut, 1, &count, B_MEDIA_RAW_AUDIO);
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
    
    // 5. Attempt Connection
    printf("[MediaKit] Attempting to connect %s -> %s...\n", hardwareOut.name, rakIn.name);
    err = roster->Connect(hardwareOut.source, rakIn.destination, &format, &hardwareOut, &rakIn);
    
    if (err != B_OK) {
        printf("[MediaKit] CONNECTION FAILED: 0x%x (%s)\n", (int)err, strerror(err));
        return err;
    }

    // Success! Print details
    printf("[MediaKit] SUCCESS! Negotiated Format:\n");
    printf("           Rate: %.1f Hz\n", format.u.raw_audio.frame_rate);
    printf("           Channels: %d\n", (int)format.u.raw_audio.channel_count);
    
    // CRITICAL: Start the hardware node here if it isn't running
    BTimeSource* ts = roster->MakeTimeSourceFor(gInputNode);
    if (ts) {
        roster->StartTimeSource(ts->Node(), system_time());
        roster->StartNode(gInputNode, ts->Now());
        ts->Release();
    }

    return B_OK;
}


// Added for Haiku 

extern "C" void HaikuAudioShutdown() {
    printf("DEBUG: Entering HaikuAudioShutdown...\n");

    // 1. Stop the Player (Output)
    if (outPlayer) { 
        printf("DEBUG: Stopping outPlayer...\n");
        outPlayer->Stop(); 
        // Deleting outPlayer AUTOMATICALLY disconnects the output node
        delete outPlayer; 
        outPlayer = NULL; 
    }

    // 2. Disconnect Input Nodes manually (Safe Check)
    BMediaRoster* roster = BMediaRoster::Roster();
    if (roster && gInputNode.node > 0) {
        // Check if the hardware node is still actually connected
        media_output connectedOutput;
        media_input connectedInput;
        int32 count = 0;
        
        // Only disconnect if the Roster says it's connected
        if (roster->GetConnectedOutputsFor(gInputNode, &connectedOutput, 1, &count) == B_OK && count > 0) {
             printf("DEBUG: Disconnecting Input Hardware...\n");
             roster->Disconnect(connectedOutput.node.node, connectedOutput.source, 
                                connectedInput.node.node, connectedInput.destination);
        }

        // 3. Stop the Hardware Node
        printf("DEBUG: Stopping Hardware Node...\n");
        roster->StopNode(gInputNode, 0, true); // true = synchronous wait
    }

    // 4. Cleanup Ring Buffers
    delete rbInputLeft; rbInputLeft = NULL;
    delete rbInputRight; rbInputRight = NULL;
    delete rbOutputLeft; rbOutputLeft = NULL;
    delete rbOutputRight; rbOutputRight = NULL;

    printf("DEBUG: Cleanup phase complete.\n");
}






