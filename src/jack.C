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
#include <xmmintrin.h>

// Forward Declarations
status_t ConnectHardwareToRakarrack();

// Prototypes
//void HaikuAudioCallback(void *cookie, void *buffer, size_t size, const media_raw_audio_format &format);
void HaikuRecordCallback(void *cookie, void *buffer, size_t size, const media_raw_audio_format &format);
int jackprocess (jack_nframes_t nframes, void *arg);

// Globals (Only define these ONCE here)
extern float input_buffer_L[8192];
extern float input_buffer_R[8192];
extern float temp_buffer_L[8192];
extern float temp_buffer_R[8192];

class RakInputNode; // Forward declaration
static RakInputNode *inNode = NULL; 
extern pthread_mutex_t jmutex;
extern RKR *JackOUT;



float get_system_sample_rate() {
    BMediaRoster* roster = BMediaRoster::Roster();
    if (!roster) return 48000.0f;

    media_node mixer;
    media_format format;
    float rate = 48000.0f;

    // Get the default audio mixer node
    if (roster->GetAudioMixer(&mixer) == B_OK) {
        // Initialize format to look for raw audio
        format.type = B_MEDIA_RAW_AUDIO;
        
        // Get the current format for the mixer
        // We use B_MEDIA_UNKNOWN_TYPE as the third argument to let the 
        // Media Roster return the actual active format
        if (roster->GetFormatFor(mixer, &format) == B_OK) {
            rate = format.u.raw_audio.frame_rate;
        }
        
        // Release the node reference
        roster->ReleaseNode(mixer);
    }
    return rate;
}

extern "C" {
    extern float G_FRAME_RATE;
    extern uint32 G_BUFFER_SIZE_BYTES;
    extern float* current_haiku_buffer;  
}
    
// External Rakarrack globals
extern unsigned int SAMPLE_RATE;
extern float fSAMPLE_RATE;
extern float cSAMPLE_RATE;
extern float fPERIOD;
extern int PERIOD;

float    fPendingRate = 48000.0f;
uint32   fPendingBufferSize = 16384;
bool     fNeedsFrequencySync = false;


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

// 2. Processed Audio (Filled by jackprocess, read by THE HEARTBEAT THREAD)
SimpleRingBuffer* rbOutputLeft = NULL;
SimpleRingBuffer* rbOutputRight = NULL;

// This template trick bypasses the 'private' access modifier for SendBuffer
struct SendBufferTag {
    typedef status_t (BBufferProducer::*type)(BBuffer*, const media_destination&);
};

template <typename Tag, typename Tag::type M>
struct Rob {
    friend typename Tag::type get_send_ptr(Tag) { return M; }
};

// This line "steals" the pointer to the private SendBuffer method
template struct Rob<SendBufferTag, &BBufferProducer::SendBuffer>;

// Forward declaration of the thief function
status_t (BBufferProducer::*get_send_ptr(SendBufferTag))(BBuffer*, const media_destination&);


// Inherit ONLY from BBufferConsumer (which already includes BMediaNode)
class RakInputNode : public BBufferConsumer, public BBufferProducer, public BMediaEventLooper {
public:

    virtual void Start(bigtime_t performance_time);
    
    RakInputNode() 
        : BBufferConsumer(B_MEDIA_RAW_AUDIO), 
          BBufferProducer(B_MEDIA_RAW_AUDIO),
          BMediaNode("Rakarrack-In-Out"),
          BMediaEventLooper() 
    {
        AddNodeKind(B_BUFFER_CONSUMER); 
        AddNodeKind(B_BUFFER_PRODUCER); 
        fBufferGroup = NULL;
        fOutputEnabled = false;
    }

    uint32 fInputFormat; 


    
    virtual ~RakInputNode() {
        BMediaEventLooper::Quit(); 
        delete fBufferGroup;
    }
     // 1. Required for BBufferProducer - Proposal of format
	virtual status_t FormatProposal(const media_source& output, media_format* format) {
    	  // If the hardware suggests something, we MUST take it to connect
   		 if (format->type == B_MEDIA_UNKNOWN_TYPE) format->type = B_MEDIA_RAW_AUDIO;
    	 if (format->type != B_MEDIA_RAW_AUDIO) return B_MEDIA_BAD_FORMAT;

    	  // Fill in our defaults only for wildcards
   		 if (format->u.raw_audio.format == media_raw_audio_format::wildcard.format)
    	    format->u.raw_audio.format = media_raw_audio_format::B_AUDIO_FLOAT;       


    	return B_OK;
	}

    // 2. Required for BBufferProducer - Handling downstream change requests
    virtual status_t FormatChangeRequested(const media_source& source, const media_destination& destination, 
                                            media_format* io_format, int32* _deprecated_) {
        return B_ERROR; // We don't support dynamic format changes yet
    }

    // 3. Required for BBufferProducer - Allowing downstream to provide buffers
    virtual status_t SetBufferGroup(const media_source& for_source, BBufferGroup* group) {
        if (for_source.id != 0) return B_MEDIA_BAD_SOURCE;
        // If the downstream (mixer/hardware) wants us to use their buffers, we can
        if (group != fBufferGroup) {
            delete fBufferGroup;
            fBufferGroup = group;
        }
        return B_OK;
    }



	virtual status_t HandleMessage(int32 code, const void *data, size_t size);
    virtual status_t DisposeOutputCookie(int32 cookie) { return B_OK; }
    
    virtual status_t FormatSuggestionRequested(media_type type, int32 quality, media_format* format) {
    if (type != B_MEDIA_RAW_AUDIO && type != B_MEDIA_UNKNOWN_TYPE) 
        return B_MEDIA_BAD_FORMAT;

    	format->type = B_MEDIA_RAW_AUDIO;

    	// Explicitly define what Rakarrack outputs
    	format->u.raw_audio.format = media_raw_audio_format::B_AUDIO_FLOAT;
   		// format->u.raw_audio.frame_rate = DEFAULT_FRAME_RATE;
    	media_raw_audio_format wc = media_raw_audio_format::wildcard;
    	format->u.raw_audio.frame_rate = wc.frame_rate; 
    	format->u.raw_audio.channel_count = 2;
   	 	format->u.raw_audio.byte_order = B_MEDIA_HOST_ENDIAN;

    	return B_OK;
	}


	virtual status_t PrepareToConnect(const media_source& source, const media_destination& dest,
                                  media_format* format, media_source* out_source, char* out_name) {
    	if (source.id != 1) return B_MEDIA_BAD_SOURCE; 
    
    	format->u.raw_audio.format = media_raw_audio_format::B_AUDIO_FLOAT;
    
    	*out_source = source;
    	strcpy(out_name, "ROt (Output)");
    	return B_OK;
	}


	// --- 2. CONNECTION ---
	virtual void Connect(status_t error, const media_source& source, const media_destination& dest,
                     const media_format& format, char* io_name) {
    	if (error != B_OK) return;
    	fOutput.source = source;
    	fOutput.destination = dest;
    	fOutput.format = format;
    
    	delete fBufferGroup;
    	// Allocate buffers based on negotiated size
    	fBufferGroup = new BBufferGroup(format.u.raw_audio.buffer_size, 4);
    	fOutputEnabled = true;
	}

    virtual void Disconnect(const media_source& source, const media_destination& dest) {
        fOutputEnabled = false;
        delete fBufferGroup;
        fBufferGroup = NULL;
    }

    virtual void LateNoticeReceived(const media_source&, bigtime_t, bigtime_t) {}
    virtual void EnableOutput(const media_source&, bool enabled, int32*) { fOutputEnabled = enabled; }
    virtual status_t SetPlayRate(int32, int32, int32*) { return B_ERROR; }
    virtual void AdditionalBufferRequested(const media_source&, media_buffer_id, bigtime_t, const media_seek_tag*) {}

	virtual status_t GetLatencyFor(const media_destination&, bigtime_t* out_latency, media_node_id* out_timesource) {
    	if (G_FRAME_RATE > 0 && G_BUFFER_SIZE_BYTES > 0) {
        	*out_latency = (bigtime_t)((G_BUFFER_SIZE_BYTES / 4.0) * 1000000LL / G_FRAME_RATE);
    	} else {
        	*out_latency = 10000; // 10ms safe default
    	}

    	if (TimeSource()) *out_timesource = TimeSource()->ID();
    	else *out_timesource = 0;
    
    	return B_OK;
	}


    virtual BMediaNode::run_mode RunMode() { return B_RECORDING; }

    virtual void NodeRegistered() { Run(); }
    virtual void HandleEvent(const media_timed_event* event, bigtime_t lateness, bool realTimeEvent = false) {}
    virtual port_id ControlPort() const { return BMediaEventLooper::ControlPort(); }
    virtual void Preroll() {}
    virtual BMediaAddOn* AddOn(int32* internalID) const { return NULL; }

	// --- CONSUMER (Input for Guitar) ---
	virtual status_t GetNextInput(int32* cookie, media_input* out_input) {
    	if (*cookie != 0) return B_ENTRY_NOT_FOUND; 
    	out_input->node = Node();
    	out_input->destination.port = ControlPort();
    	out_input->destination.id = 0; 
    	sprintf(out_input->name, "GIn"); 
    	out_input->format.type = B_MEDIA_RAW_AUDIO;
    	*cookie = 1; 
    	return B_OK;
	}

	// --- PRODUCER (Output for Speakers) ---
	virtual status_t GetNextOutput(int32* cookie, media_output* out_output) {
    	if (*cookie != 0) return B_ENTRY_NOT_FOUND;
    	out_output->node = Node();
    	out_output->source.port = ControlPort();
    	out_output->source.id = 1; 
    	sprintf(out_output->name, "ROt");
    	out_output->format.type = B_MEDIA_RAW_AUDIO;
    	*cookie = 1;
    	return B_OK;
	}

	virtual status_t AcceptFormat(const media_destination& dest, media_format* format) {
    	if (format->type == B_MEDIA_UNKNOWN_TYPE) format->type = B_MEDIA_RAW_AUDIO;
    
    	media_raw_audio_format wc = media_raw_audio_format::wildcard;
    	if (format->u.raw_audio.frame_rate == wc.frame_rate) {
        format->u.raw_audio.frame_rate = 48000.0f; 
    }

    fInputFormat = format->u.raw_audio.format;

    // Capture the new values for the audio thread to pick up
    if (format->u.raw_audio.buffer_size != wc.buffer_size && format->u.raw_audio.buffer_size > 0) {
        fPendingRate = format->u.raw_audio.frame_rate;
        fPendingBufferSize = format->u.raw_audio.buffer_size;
        fNeedsFrequencySync = true; 
    }

    return B_OK; 
}


virtual status_t Connected(const media_source& source, const media_destination& dest, 
                           const media_format& format, media_input* out_input) {
    
    // 1. Sync our global audio parameters
    G_FRAME_RATE = format.u.raw_audio.frame_rate;
    G_BUFFER_SIZE_BYTES = format.u.raw_audio.buffer_size;
    
    if (JackOUT) JackOUT->Update_tempo();

    return B_OK;
}


    virtual void Disconnected(const media_source&, const media_destination&) {}
    virtual status_t FormatChanged(const media_source&, const media_destination&, int32, const media_format&) { return B_OK; }
    

    // This static helper bypasses the visibility scope by operating on the pointer directly
	status_t FinalSendBuffer(BBuffer* b, const media_destination& d) {
    // Retrieve the stolen pointer
    auto send_func = get_send_ptr(SendBufferTag());
    // Execute the pointer against the current instance
    return (this->*send_func)(b, d);
}


// Helper to prevent "Static Explosion" (Integer Overflow)
inline float fClamp(float val) {
    if (val > 0.99f) return 0.99f;
    if (val < -0.99f) return -0.99f;
    return val;
}

// --- AUDIO BRIDGE ---
virtual void BufferReceived(BBuffer *buffer) {
    if (!buffer || !rbInputLeft || !rbInputRight) {
        if (buffer) buffer->Recycle();
        return;
    }

    pthread_mutex_lock(&jmutex); 

    // --- A. INPUT STAGE ---
    size_t inputBytes = buffer->SizeUsed();
    void* rawInput = buffer->Data();
    int frames = 0; // We use the same frame count for In and Out
    
    // Determine True Hardware Format from Input (The source of truth)
    bool isInt32 = (fInputFormat == 0x4);
    bool isInt16 = (fInputFormat == 0x2);
    bool isFloat = (fInputFormat == 0x24);

    if (isInt32) frames = inputBytes / (2 * sizeof(int32));
    else if (isInt16) frames = inputBytes / (2 * sizeof(int16));
    else frames = inputBytes / (2 * sizeof(float));

    if (frames > 0) {
        if (isInt32) { 
            int32* data = (int32*)rawInput;
            const float kRecip = 1.0f / 2147483648.0f;
            for (int i = 0; i < frames; i++) {
                rbInputLeft->buffer[rbInputLeft->writePos] = (float)data[i * 2] * kRecip;
                rbInputRight->buffer[rbInputRight->writePos] = (float)data[i * 2 + 1] * kRecip;
                int next = (rbInputLeft->writePos + 1) % rbInputLeft->size;
                rbInputLeft->writePos = next;
                rbInputRight->writePos = next;
            }
        }
        else if (isInt16) {
            int16* data = (int16*)rawInput;
            const float kRecip = 1.0f / 32768.0f;
            for (int i = 0; i < frames; i++) {
                rbInputLeft->buffer[rbInputLeft->writePos] = (float)data[i * 2] * kRecip;
                rbInputRight->buffer[rbInputRight->writePos] = (float)data[i * 2 + 1] * kRecip;
                int next = (rbInputLeft->writePos + 1) % rbInputLeft->size;
                rbInputLeft->writePos = next;
                rbInputRight->writePos = next;
            }
        }
        else { // FLOAT
            float* data = (float*)rawInput;
            for (int i = 0; i < frames; i++) {
                rbInputLeft->buffer[rbInputLeft->writePos] = data[i * 2];
                rbInputRight->buffer[rbInputRight->writePos] = data[i * 2 + 1];
                int next = (rbInputLeft->writePos + 1) % rbInputLeft->size;
                rbInputLeft->writePos = next;
                rbInputRight->writePos = next;
            }
        }
    }

    // --- B. OUTPUT STAGE (Mirrored) ---
    if (fOutputEnabled && fBufferGroup) {
        size_t outputSizeBytes = fOutput.format.u.raw_audio.buffer_size;
        
        // Check availability
        size_t avail = rbOutputLeft->Available();
        if (avail < (size_t)frames) {
            pthread_mutex_unlock(&jmutex);
            buffer->Recycle(); 
            return; 
        }

        if (!TimeSource()) {
            pthread_mutex_unlock(&jmutex);
            buffer->Recycle();
            return;
        }

        BBuffer* outBuffer = fBufferGroup->RequestBuffer(outputSizeBytes);
        if (outBuffer) {
            void* rawDest = outBuffer->Data();
            memset(rawDest, 0, outputSizeBytes);
            
            // --- THE FIX: IGNORE fOutput.format, USE fInputFormat ---
            // We assume the hardware input and output are locked to the same type.
            
            if (isInt32) { // Convert to INT32
                int32* dest = (int32*)rawDest;
                const float kScale = 2147483647.0f;
                for (int i = 0; i < frames; i++) {
                    float L = rbOutputLeft->buffer[rbOutputLeft->readPos];
                    float R = rbOutputRight->buffer[rbOutputRight->readPos];
                    
                    // Clamp to prevent overflow static
                    if (L > 0.99f) L = 0.99f; else if (L < -0.99f) L = -0.99f;
                    if (R > 0.99f) R = 0.99f; else if (R < -0.99f) R = -0.99f;

                    dest[i*2]   = (int32)(L * kScale);
                    dest[i*2+1] = (int32)(R * kScale);
                    
                    rbOutputLeft->readPos = (rbOutputLeft->readPos + 1) % rbOutputLeft->size;
                    rbOutputRight->readPos = (rbOutputRight->readPos + 1) % rbOutputRight->size;
                }
            }
            else if (isInt16) { // Convert to INT16
                int16* dest = (int16*)rawDest;
                const float kScale = 32767.0f;
                for (int i = 0; i < frames; i++) {
                    float L = rbOutputLeft->buffer[rbOutputLeft->readPos];
                    float R = rbOutputRight->buffer[rbOutputRight->readPos];
                    
                    if (L > 0.99f) L = 0.99f; else if (L < -0.99f) L = -0.99f;
                    if (R > 0.99f) R = 0.99f; else if (R < -0.99f) R = -0.99f;

                    dest[i*2]   = (int16)(L * kScale);
                    dest[i*2+1] = (int16)(R * kScale);

                    rbOutputLeft->readPos = (rbOutputLeft->readPos + 1) % rbOutputLeft->size;
                    rbOutputRight->readPos = (rbOutputRight->readPos + 1) % rbOutputRight->size;
                }
            }
            else { // FLOAT (Direct)
                float* dest = (float*)rawDest;
                for (int i = 0; i < frames; i++) {
                    dest[i*2]   = rbOutputLeft->buffer[rbOutputLeft->readPos];
                    dest[i*2+1] = rbOutputRight->buffer[rbOutputRight->readPos];
                    rbOutputLeft->readPos = (rbOutputLeft->readPos + 1) % rbOutputLeft->size;
                    rbOutputRight->readPos = (rbOutputRight->readPos + 1) % rbOutputRight->size;
                }
            }
            
            outBuffer->Header()->type = B_MEDIA_RAW_AUDIO;
            outBuffer->Header()->size_used = outputSizeBytes; 
            outBuffer->Header()->start_time = 0;

            if (FinalSendBuffer(outBuffer, fOutput.destination) != B_OK) {
                outBuffer->Recycle();
            }
        }
    }

    pthread_mutex_unlock(&jmutex); 
    buffer->Recycle(); 
}

    virtual void ProducerDataStatus(const media_destination& for_whom, int32 status, bigtime_t at_performance_time) {}
    virtual void DisposeInputCookie(int32) {}
    virtual status_t GetNodeAttributes(media_node_attribute* out_attributes, size_t in_max_count, size_t* out_count) {
        if (out_count) *out_count = 0;
        return B_OK;
    }

private:
    bool fOutputEnabled;
    media_output fOutput;
    BBufferGroup* fBufferGroup;
    float    fPendingRate;
    uint32   fPendingBufferSize;
    bool     fNeedsFrequencySync;

};



status_t RakInputNode::HandleMessage(int32 code, const void* data, size_t size) {
    if (code == B_MEDIA_FORMAT_CHANGED) {
        BMediaRoster* roster = BMediaRoster::Roster();
        media_node mixer;
        media_format format;

        if (roster && roster->GetAudioMixer(&mixer) == B_OK) {
            // Set type to raw audio so GetFormatFor knows what to look for
            format.type = B_MEDIA_RAW_AUDIO;
            
            if (roster->GetFormatFor(mixer, &format) == B_OK) {
                // 1. Update Sample Rate
                float new_rate = format.u.raw_audio.frame_rate;
                if (new_rate > 0 && new_rate != G_FRAME_RATE) {
                    fprintf(stderr, "[MEDIA] Rate change: %.1f Hz\n", new_rate);
                    G_FRAME_RATE = new_rate;
                }

                // 2. Update Buffer Size 
                uint32 new_buffer_size = format.u.raw_audio.buffer_size;
                if (new_buffer_size > 0 && new_buffer_size != G_BUFFER_SIZE_BYTES) {
                    fprintf(stderr, "[MEDIA] Buffer change: %u bytes\n", new_buffer_size);
                    G_BUFFER_SIZE_BYTES = new_buffer_size;
                }

                // 3. Notify Rakarrack Engine to recalculate internal math
                if (JackOUT) {
                    JackOUT->Update_tempo();
                }
            }
            // Clean up the node reference
            roster->ReleaseNode(mixer);
        }
    }
    
    // Crucial: We must call the base class of the PRIMARY looper type.
    // Since RakInputNode is a Consumer, call BBufferConsumer::HandleMessage.
    return BBufferConsumer::HandleMessage(code, data, size);
}

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
//void *dataout; 

int jackprocess (jack_nframes_t nframes, void *arg);

extern "C" bigtime_t estimate_max_scheduling_latency();


// --- GLOBAL THREAD CONTROL ---
thread_id gRakThread = -1;
volatile bool gEngineRunning = true;

// --- THE HEARTBEAT THREAD ---
int32 RakarrackEngineThread(void* data) {
    while (gEngineRunning) {
        int quantum = PERIOD;
        if (quantum <= 0) quantum = 2048;
        // 1. How many microseconds does it take to fill 'quantum' samples?
        // Formula: (Samples / SampleRate) * 1,000,000
        bigtime_t bufferDuration = (bigtime_t)((quantum / (float)SAMPLE_RATE) * 1000000LL);
        
        // 2. Snooze for half that duration (Safety margin)
        // We cap it at 100 minimum so we don't accidentally spin-lock.
        bigtime_t napTime = bufferDuration / 2;
        if (napTime < 100) napTime = 100; 

        if (rbInputLeft) {
            size_t avail = rbInputLeft->Available();
            while (avail >= (size_t)quantum) {
                pthread_mutex_lock(&jmutex);
                jackprocess(quantum, NULL);
                pthread_mutex_unlock(&jmutex);
                avail = rbInputLeft->Available();
            }
        }

        // Snooze based on the calculated timing
        snooze(napTime); 
    }
    return 0;
}




// --- AUDIO ENGINE ---
int jackprocess(jack_nframes_t nframes, void *arg) {
	
    if (fNeedsFrequencySync) {
        fNeedsFrequencySync = false;
        
        G_FRAME_RATE = fPendingRate;
        SAMPLE_RATE  = (unsigned int)G_FRAME_RATE;
        fSAMPLE_RATE = G_FRAME_RATE;
        cSAMPLE_RATE = G_FRAME_RATE;
        
        
         PERIOD = nframes; 

        if (JackOUT) {
            JackOUT->Update_tempo();
            // Clear buffers to stop the "nan" and crashes
            memset(JackOUT->efxoutl, 0, sizeof(float) * 8192);
            memset(JackOUT->efxoutr, 0, sizeof(float) * 8192);
        }
        fprintf(stderr, "[JACK] Engine synced to %u Hz in audio thread.\n", SAMPLE_RATE);
    }

 if (JackOUT && nframes > 0) {

	
	
    bigtime_t start_time = system_time();
    _mm_setcsr(_mm_getcsr() | 0x8040); // DAZ/FTZ - Flush denormals to zero

    // Static buffers prevent stack overflow and memory reallocation
    static float process_in_L[8192];
    static float process_in_R[8192];

	if (nframes != (G_BUFFER_SIZE_BYTES / 8)) {
    	G_BUFFER_SIZE_BYTES = nframes * 8;
	}

	if (isnan(JackOUT->efxoutl[0])) {
    	memset(JackOUT->efxoutl, 0, sizeof(float) * nframes);
    	memset(JackOUT->efxoutr, 0, sizeof(float) * nframes);
	}

    // Safety cap for buffer size
    if (nframes > 8192) {
        fprintf(stderr, "[DEBUG] nframes (%d) exceeds static buffer size! Truncating.\n", nframes);
        nframes = 8192;
    }
    
    

    // --- 1. READ INPUT FROM RINGBUFFER (THREAD SAFE) ---
    pthread_mutex_lock(&jmutex);
    if (rbInputLeft && rbInputRight) {
        size_t avail = rbInputLeft->Available();
        if (avail < nframes) {
             // If we don't have enough data, fill the rest with silence
             // This prevents "jumbled" stuttering from playing old garbage data
             memset(process_in_L, 0, sizeof(float) * nframes);
             memset(process_in_R, 0, sizeof(float) * nframes);
        }
        // Read what we can (or silence if empty/partial)
        rbInputLeft->Read(process_in_L, nframes);
        rbInputRight->Read(process_in_R, nframes);
    } else {
        // If buffers don't exist, play absolute silence
        memset(process_in_L, 0, sizeof(float) * nframes);
        memset(process_in_R, 0, sizeof(float) * nframes);
    }
    pthread_mutex_unlock(&jmutex);

    // --- 2. SIGNAL TRACE: INPUT CHECK ---
    // Use this to see if hardware is actually sending audio
    float max_in = 0.0f;
    for (int i = 0; i < (int)nframes; i++) {
        float abs_L = fabs(process_in_L[i]);
        if (abs_L > max_in) max_in = abs_L;
    }

    // --- 3. MIDI PROCESSING ---
    if (jack_midi_in && jack_midi_out) {
        void *mididata_in = jack_port_get_buffer(jack_midi_in, nframes);
        void *mididata_out = jack_port_get_buffer(jack_midi_out, nframes);
        int count = jack_midi_get_event_count(mididata_in);
        jack_midi_event_t midievent;
        jack_midi_clear_buffer(mididata_out);
        for (int i = 0; i < count; i++) {
            jack_midi_event_get(&midievent, mididata_in, i);
            JackOUT->jack_process_midievents(&midievent);
        }
    }

    // --- 4. PREPARE BUFFERS & DENORMAL PREVENTION ---
    for (int i = 0; i < (int)nframes; i++) {
        // Kill denormals (numbers too small for CPU to handle quickly)
        if (fabs(process_in_L[i]) < 1e-15f) process_in_L[i] = 0.0f;
        if (fabs(process_in_R[i]) < 1e-15f) process_in_R[i] = 0.0f;

        JackOUT->efxoutl[i] = process_in_L[i];
        JackOUT->efxoutr[i] = process_in_R[i];
    }

    // --- 5. RUN EFFECTS ENGINE ---
    JackOUT->Alg(JackOUT->efxoutl, JackOUT->efxoutr, process_in_L, process_in_R, nframes);

    // --- 6. SIGNAL TRACE: OUTPUT CHECK ---
    float max_out = 0.0f;
    for (int i = 0; i < (int)nframes; i++) {
        float abs_out_L = fabs(JackOUT->efxoutl[i]);
        if (abs_out_L > max_out) max_out = abs_out_L;

        // Safety: Catch NAN/INF which causes "No Sound" or "Loud Pops"
        if (isnan(JackOUT->efxoutl[i]) || isinf(JackOUT->efxoutl[i])) {
            JackOUT->efxoutl[i] = 0.0f;
        }
        if (isnan(JackOUT->efxoutr[i]) || isinf(JackOUT->efxoutr[i])) {
            JackOUT->efxoutr[i] = 0.0f;
        }
    }

    // --- 7. WRITE TO OUTPUT RINGBUFFER (THREAD SAFE) ---
    if (rbOutputLeft && rbOutputRight) {
        pthread_mutex_lock(&jmutex);
        size_t free_space = rbOutputLeft->FreeSpace();
        if (free_space >= (int)nframes) {
            rbOutputLeft->Write(JackOUT->efxoutl, nframes);
            rbOutputRight->Write(JackOUT->efxoutr, nframes);
            if (gDebugMode) {
            static int engine_push_count = 0;
			if (++engine_push_count % 100 == 0) {
   				 fprintf(stderr, "[DEBUG] Engine wrote to RB. New Avail: %zu\n", rbOutputLeft->Available());
				}
			}
        } else {
            // DEBUG: If the engine processes faster than hardware consumes, we get "jumbled" audio.
            static int overflow_log = 0;
            if (++overflow_log % 50 == 0) {
                fprintf(stderr, "[OVERFLOW] Engine too fast! RB_Out_Free: %zu, Need: %d\n", free_space, (int)nframes);
            }
        }
        pthread_mutex_unlock(&jmutex);
    }

    // --- 8. LOGGING & LOAD CALCULATION ---
	if (gDebugMode) {
		static int debug_timer = 0;
		if (++debug_timer % 100 == 0) { // Print roughly every 2 seconds
    		fprintf(stderr, "[DEBUG] Engine Stats | Rate: %.1f | BufFrames: %u | CPU: %.1f%% | Quantum: %d\n",
            G_FRAME_RATE, nframes, JackOUT->cpuload, (int)G_BUFFER_SIZE_BYTES / 8);   
        	fprintf(stderr, "[DEBUG] G_FRAME_RATE is %.1f\n", G_FRAME_RATE);
			}
	}


    bigtime_t end_time = system_time();

    double max_us = ((double)nframes / G_FRAME_RATE) * 1000000.0; 
    float instant_load = ((float)(end_time - start_time) / max_us) * 100.0f;
    JackOUT->cpuload = (JackOUT->cpuload * 0.9f) + (instant_load * 0.1f);
	
	if (gDebugMode) {
    // Diagnostic Print (Prints ~once per second)
    static int log_timer = 0;
    if (++log_timer % 100 == 0) {
        fprintf(stderr, "[SIGNAL] In Max: %.4f | Out Max: %.4f\n",
                max_in, max_out, JackOUT->cpuload);
    }
        if (max_in < 0.00001f) fprintf(stderr, "   -> Warning: Input is Silent (Check Hardware/Connection).\n");
        if (max_in > 0.01f && max_out < 0.00001f) fprintf(stderr, "   -> Warning: Engine is muting output (Check Volume/Bypass).\n");
    }
    
 }

    return 0;
}

// --- 3. STARTUP SEQUENCE ---
int JACKstart(RKR * rkr_, jack_client_t * jackclient_) {
    JackOUT = rkr_;
    pthread_mutex_init(&jmutex, NULL);
    media_format directFormat;
	status_t err;
    // 1. Buffers
    uint32_t bufferSize = 65536; 
    if (!rbInputLeft)   rbInputLeft   = new SimpleRingBuffer(bufferSize);
    if (!rbInputRight)  rbInputRight  = new SimpleRingBuffer(bufferSize);
    if (!rbOutputLeft)  rbOutputLeft  = new SimpleRingBuffer(bufferSize);
    if (!rbOutputRight) rbOutputRight = new SimpleRingBuffer(bufferSize);

    uint32 prefillFrames = G_BUFFER_SIZE_BYTES; 
    float* silence = new float[prefillFrames];
    memset(silence, 0, prefillFrames * sizeof(float));
    
    pthread_mutex_lock(&jmutex);
    rbOutputLeft->Write(silence, prefillFrames);
    rbOutputRight->Write(silence, prefillFrames);
    pthread_mutex_unlock(&jmutex);
    
    delete[] silence;

    // 3. Register Node
    BMediaRoster* roster = BMediaRoster::Roster();
    inNode = new RakInputNode(); 
    roster->RegisterNode(inNode);    

	if (inNode) {
    	BMediaRoster::Roster()->StartWatching(be_app_messenger, B_MEDIA_WILDCARD);
	}

    // 4. Connect Input
    ConnectHardwareToRakarrack();

    // 5. Spawn Engine Thread
    if (gRakThread == -1) {
        gRakThread = spawn_thread(RakarrackEngineThread, "RakarrackProcessing", B_REAL_TIME_PRIORITY, NULL);
        if (gRakThread >= B_OK) {
            resume_thread(gRakThread);
        }
    }

    // 6. Time Source
    BTimeSource* timeSource = roster->MakeTimeSourceFor(inNode->Node());
    if (timeSource) {
        roster->SetTimeSourceFor(inNode->Node().node, timeSource->Node().node);
        if (!timeSource->IsRunning()) {
             roster->StartTimeSource(timeSource->Node(), system_time());
             snooze(5000); 
        }
        roster->StartNode(inNode->Node(), 0);
        if (gInputNode.node > 0 && gInputNode.node != timeSource->Node().node) {
            roster->StartNode(gInputNode, 0);
        }
        timeSource->Release();
    }

    // 7. Connect Output
    media_node hardwareOutput;
    if (roster->GetAudioOutput(&hardwareOutput) == B_OK) {
        media_input hwInput;   
        media_output rakOutput; 
        int32 count = 0;

        if (roster->GetAllInputsFor(hardwareOutput, &hwInput, 1, &count) == B_OK && count > 0) {
            if (roster->GetAllOutputsFor(inNode->Node(), &rakOutput, 1, &count) == B_OK && count > 0) {
                
                if (hwInput.source != media_source::null) {
                    media_node mixerNode;
                    if (roster->GetNodeFor(hwInput.source.port, &mixerNode) == B_OK) {
                        roster->Disconnect(mixerNode.node, hwInput.source, 
                                           hardwareOutput.node, hwInput.destination);
                        roster->ReleaseNode(mixerNode);
                    }
                }

                media_format directFormat;
                directFormat.type = B_MEDIA_RAW_AUDIO;
                directFormat.u.raw_audio = media_raw_audio_format::wildcard;
                roster->GetFormatFor(hwInput, &directFormat); 
                
                if (directFormat.u.raw_audio.format == media_raw_audio_format::wildcard.format)
                     directFormat.u.raw_audio.format = media_raw_audio_format::B_AUDIO_FLOAT;              
                rakOutput.source.id = 1; 
                status_t err = roster->Connect(rakOutput.source, hwInput.destination, 
                                              &directFormat, &rakOutput, &hwInput);
                
                if (err == B_OK) {
                    roster->StartNode(hardwareOutput, 0);
                }
            }
        }
    }
  
    roster->StartNode(inNode->Node(), system_time());
    return B_OK;
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

    // 1. Find the Physical Input Node (Microphone/Line-In)
    status_t err = roster->GetAudioInput(&gInputNode);
    if (err != B_OK) return err;
    if (gDebugMode) {
    printf("[Rakarrack] Hardware Input Node: %ld\n", gInputNode.node);
    }

    // 2. Find the Hardware's Output port (The Source of the guitar signal)
    media_output hardwareOut;
    int32 count = 0;
    
    // We use GetAllOutputsFor to find the hardware's "Mic/Line Out" port
    err = roster->GetAllOutputsFor(gInputNode, &hardwareOut, 1, &count);

    if (err != B_OK || count == 0) {
        printf("[Rakarrack] WARNING: Capture pins busy. Checking all outputs...\n");
        // Fallback: search for any available output on the capture node
        media_output outputs[16];
        int32 numOut = 0;
        if (roster->GetAllOutputsFor(gInputNode, outputs, 16, &numOut) == B_OK) {
            for (int i = 0; i < numOut; i++) {
                if (outputs[i].destination.port == 0) { // Found a free one
                    hardwareOut = outputs[i];
                    count = 1;
                    break;
                }
            }
        }
    }

    // If still blocked, trigger the Media Server Restart logic
    if (count == 0) {
        BAlert* alert = new BAlert("Media Server Busy", 
            "All hardware capture pins are blocked.\n\n"
            "Would you like to restart the Media Server?",
            "Cancel", "Restart", NULL, B_WIDTH_AS_USUAL, B_STOP_ALERT);

        if (alert->Go() == 1) {
            system("hey media_server quit > /dev/null 2>&1");
            system("kill media_addon_server > /dev/null 2>&1");
            system("/boot/system/servers/media_server &");
            exit(0); 
        }
        return B_BUSY;
    }

    // 3. Find Rakarrack's GIn (Your Consumer Input - ID 0)
    media_input rakIn;
    err = roster->GetAllInputsFor(inNode->Node(), &rakIn, 1, &count);
    if (err != B_OK || count == 0) return B_NAME_NOT_FOUND;

    // 4. HIJACK: Sever existing link if the Mic is already plugged into something
    if (hardwareOut.destination != media_destination::null) {
        printf("[Rakarrack] Severing existing capture link for direct path...\n");
        roster->Disconnect(hardwareOut.node.node, hardwareOut.source, 
                           hardwareOut.destination.port, hardwareOut.destination);
    }

    // 5. Connect: Hardware Source (Mic) -> Rakarrack Destination (GIn)
    media_format format;
    memset(&format, 0, sizeof(format));
    format.type = B_MEDIA_RAW_AUDIO;
    media_raw_audio_format wc = media_raw_audio_format::wildcard;
    
    format.u.raw_audio = media_raw_audio_format::wildcard;
    format.u.raw_audio.frame_rate = wc.frame_rate;
    format.u.raw_audio.buffer_size = wc.buffer_size; 
    
    // Get hardware info first
    roster->GetFormatFor(hardwareOut, &format);

    // NOW enforce your engine's limits so they don't get overwritten
    format.u.raw_audio.format = media_raw_audio_format::B_AUDIO_FLOAT;
    format.u.raw_audio.channel_count = 2;    

    format.u.raw_audio.buffer_size = G_BUFFER_SIZE_BYTES * sizeof(float) * 2; 

    // Try to connect with YOUR enforced format
    err = roster->Connect(hardwareOut.source, rakIn.destination, &format, &hardwareOut, &rakIn);

    if (err == B_OK) {
    // 3. Store it! The 'format' object was specialized by the system.
    G_FRAME_RATE = format.u.raw_audio.frame_rate; 
    G_BUFFER_SIZE_BYTES = format.u.raw_audio.buffer_size;
	}

    // 6. Start Hardware Node
    roster->StartNode(gInputNode, 0);

    return B_OK;
}

// --- CORTEX CRASH FIX ---
// This intercepts the "Start" command from Cortex/Media Server.
// If they send a timestamp that is "Too Large" (System Time), we rewrite it to 0.

void RakInputNode::Start(bigtime_t performance_time) {
    if (performance_time > 10000000000LL) { 
        if (TimeSource() && TimeSource()->IsRunning()) {
            performance_time = TimeSource()->Now();
        } else {
            performance_time = 0;
        }
    }
    BMediaEventLooper::Start(performance_time);
    fOutputEnabled = true;
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

    if (inNode) {
        printf("Rakarrack: Stopping and Releasing inNode...\n");
        roster->StopNode(inNode->Node(), 0, true);
        inNode->Release(); // Incremental cleanup
        inNode = NULL;
    }
    
    printf("Rakarrack: Shutdown complete.\n");
}

