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


extern bool gDebugMode;

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


#include <Path.h>          // BPath -- settings-dir resolution
#include <FindDirectory.h> // find_directory()/B_USER_SETTINGS_DIRECTORY
#include <dirent.h>        // opendir()/readdir() -- /dev/audio/hmulti scan
#include <string>

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
        AddNodeKind(B_BUFFER_CONSUMER | B_PHYSICAL_INPUT); 
    }

    uint32 fInputFormat; 

    virtual ~RakInputNode() {
        BMediaEventLooper::Quit(); 
    }


    // --- 1. MESSAGE PUMP ---
    virtual status_t HandleMessage(int32 code, const void *data, size_t size) {
        if (BBufferConsumer::HandleMessage(code, data, size) == B_OK) return B_OK;
        if (BMediaEventLooper::HandleMessage(code, data, size) == B_OK) return B_OK;
        return BMediaNode::HandleMessage(code, data, size);
    }

    // --- 2. LATENCY & RUN MODE ---
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
    	thread_id looperThread = ControlThread();
    	set_thread_priority(looperThread, B_REAL_TIME_PRIORITY);
    	fprintf(stderr, "[Rakarrack] Input Node Thread %d Realtime Priority Set.\n", (int)looperThread);
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
       
	// Catch-up: snap to the live edge whenever backlog builds past ~2 buffer periods,
	// not just on the very first callback. A backlog can build any time this output
	// callback stalls while the input side keeps writing (e.g. while something
	// external disconnects/reconnects our Mixer connection).
	int backlogThreshold = frames * 3;
	if (rbInputLeft->Available() > backlogThreshold) {
	    int lookback = frames * 2;
	    rbInputLeft->readPos = (rbInputLeft->writePos - lookback + rbInputLeft->size) % rbInputLeft->size;
	    rbInputRight->readPos = (rbInputRight->writePos - lookback + rbInputRight->size) % rbInputRight->size;
	    printf("[Rakarrack] Backlog cleared (%d frames). Now at live edge.\n", rbInputLeft->Available());
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


// ----------------------------------------------------------------------------
// Real-time audio auto-detection, ported from hrecord's own
// DetectRealtimeAudioSettings() (github.com/ablyssx74/hrecord) and
// RealTimeGUI's DetectCurrentSampleRate() (github.com/ablyssx74/RealTimeGUI).
// Same driver catalog, same active-line-only settings-file matching.
// ----------------------------------------------------------------------------
struct RealtimeDriverProfile {
    const char* devfsSegment;
    const char* settingsFileName;
    const char* framesKey;
};

static const RealtimeDriverProfile kRealtimeDriverProfiles[] = {
    { "hda", "hda.settings", "play_buffer_frames" },
    { "auich", "auich.settings", "buffer_frames" },
    { "es1370", "es1370.settings", "buffer_frames" },
    { "echo", "echo.settings", "buffer_frames" },
    { "emuxki", "emuxki.settings", "buffer_frames" },
    { "ice1712", "ice1712.settings", "buffer_size" },
};
static const int32 kRealtimeDriverProfileCount =
    sizeof(kRealtimeDriverProfiles) / sizeof(kRealtimeDriverProfiles[0]);

static std::string ReadDriverSettingsFile(const std::string& path) {
    std::string result;
    FILE* f = fopen(path.c_str(), "r");
    if (f == nullptr)
        return result;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        result.append(buf, n);
    fclose(f);
    return result;
}

// Only ever matches a genuinely active (non-'#') line -- a commented line
// may just be a driver's own worked example in its header comment.
static bool FindActiveSettingValue(const std::string& content, const char* key,
        int32* outValue) {
    std::string keyStr(key);
    size_t lineStart = 0;
    while (lineStart < content.size()) {
        size_t lineEnd = content.find('\n', lineStart);
        if (lineEnd == std::string::npos)
            lineEnd = content.size();

        size_t firstNonSpace = lineStart;
        while (firstNonSpace < lineEnd
                && (content[firstNonSpace] == ' ' || content[firstNonSpace] == '\t'))
            firstNonSpace++;

        if (firstNonSpace < lineEnd && content[firstNonSpace] != '#'
                && lineEnd - firstNonSpace >= keyStr.size()
                && content.compare(firstNonSpace, keyStr.size(), keyStr) == 0) {
            size_t afterKey = firstNonSpace + keyStr.size();
            if (afterKey >= lineEnd || content[afterKey] == ' ' || content[afterKey] == '\t') {
                *outValue = atol(content.c_str() + afterKey);
                return true;
            }
        }
        lineStart = lineEnd + 1;
    }
    return false;
}


static bool DetectRealtimeBufferFrames(int32* outFrames) {
    DIR* dir = opendir("/dev/audio/hmulti");
    if (dir == nullptr)
        return false;

    const RealtimeDriverProfile* profile = nullptr;
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        for (int32 i = 0; i < kRealtimeDriverProfileCount; i++) {
            if (strcmp(entry->d_name, kRealtimeDriverProfiles[i].devfsSegment) == 0) {
                profile = &kRealtimeDriverProfiles[i];
                break;
            }
        }
        if (profile != nullptr)
            break;
    }
    closedir(dir);
    if (profile == nullptr)
        return false;

    BPath settingsDirPath;
    std::string settingsDir;
    if (find_directory(B_USER_SETTINGS_DIRECTORY, &settingsDirPath) == B_OK) {
        settingsDirPath.Append("kernel/drivers");
        settingsDir = settingsDirPath.Path();
    } else {
        settingsDir = "/boot/home/config/settings/kernel/drivers";
    }

    std::string content = ReadDriverSettingsFile(settingsDir + "/" + profile->settingsFileName);
    if (content.empty())
        return false;

    return FindActiveSettingValue(content, profile->framesKey, outFrames);
}

static bool DetectCurrentSampleRate(uint32_t* outRate) {
    BMediaRoster* roster = BMediaRoster::Roster();
    if (roster == nullptr)
        return false;

    media_node audioOutputNode;
    if (roster->GetAudioOutput(&audioOutputNode) != B_OK)
        return false;

    media_output outputs[8];
    int32 outputCount = 0;
    bool found = false;
    if (roster->GetAllOutputsFor(audioOutputNode, outputs, 8, &outputCount) == B_OK) {
        for (int32 i = 0; i < outputCount && !found; i++) {
            media_format format;
            if (roster->GetFormatFor(outputs[i], &format) == B_OK
                    && format.type == B_MEDIA_RAW_AUDIO) {
                *outRate = (uint32_t)format.u.raw_audio.frame_rate;
                found = true;
            }
        }
    }
    roster->ReleaseNode(audioOutputNode);
    return found;
}

// Fills *outRate always (falls back to DEFAULT_FRAME_RATE), *outFrames only
// when real-time buffers were actually detected (falls back to
// DEFAULT_BUFFER_FRAMES otherwise). Returns whether real-time was detected.
static bool DetectHaikuRealtimeAudio(uint32_t* outRate, int32* outFrames) {
    uint32_t rate = 0;
    *outRate = DetectCurrentSampleRate(&rate) ? rate : (uint32_t)DEFAULT_FRAME_RATE;

    int32 frames = 0;
    if (DetectRealtimeBufferFrames(&frames)) {
        *outFrames = frames;
        return true;
    }
    *outFrames = DEFAULT_BUFFER_FRAMES;
    return false;
}

// Scales a frame count to a different sample rate, preserving the same
// buffer DURATION, snapped to the nearest of a handful of common buffer
// sizes. Used only for the non-real-time fallback: a flat frame count
// regardless of rate is backwards for a *safety* fallback -- a flat 1024
// frames is ~21ms at 48000 Hz but only ~10ms at 96000 Hz, i.e. LESS
// safety margin at the higher rate, when a fallback should keep more.
static int32 ScaleFramesForRate(int32 baseFrames, uint32_t baseRate, uint32_t actualRate) {
    static const int32 kCommonSizes[] = { 256, 512, 1024, 2048, 4096 };
    if (actualRate == 0 || baseRate == 0 || actualRate == baseRate)
        return baseFrames;

    double target = (double)baseFrames * ((double)actualRate / (double)baseRate);

    int32 best = kCommonSizes[0];
    double bestDiff = fabs(target - best);
    for (size_t i = 1; i < sizeof(kCommonSizes) / sizeof(kCommonSizes[0]); i++) {
        double diff = fabs(target - kCommonSizes[i]);
        if (diff < bestDiff) {
            bestDiff = diff;
            best = kCommonSizes[i];
        }
    }
    return best;
}



// Runs full detection (rate + frames) and stores the result in
// g_HaikuRealtimeDetected/g_HaikuDetectedRate/g_HaikuDetectedFrames.
// Must run before "RKR rkr;" is constructed in main() -- RKR's
// constructor builds every effect object, and each one permanently
// sizes its own internal buffers from SAMPLE_RATE *at construction
// time*, never revisiting that allocation. Changing SAMPLE_RATE
// afterward (an earlier version of this fix did exactly that, inside
// JACKstart()) leaves those buffers sized for the OLD rate while
// per-sample math uses the NEW one -- an out-of-bounds write once real
// audio flows, confirmed as the cause of the StompBox::out() crash at
// 96kHz/192kHz. Detecting here means RKR::RKR()'s own
// jack_get_sample_rate()/jack_get_buffer_size() calls see the final
// correct values the first and only time.
//
// Needs a running BApplication (BMediaRoster's reply round-trip depends
// on one) -- see the call site in main.C.
void HaikuDetectAudioSettingsEarly() {
    g_HaikuRealtimeDetected = DetectHaikuRealtimeAudio(&g_HaikuDetectedRate, &g_HaikuDetectedFrames);
    if (!g_HaikuRealtimeDetected) {
        g_HaikuDetectedFrames = ScaleFramesForRate(g_HaikuDetectedFrames,
            (uint32_t)DEFAULT_FRAME_RATE, g_HaikuDetectedRate);
    }
}





int JACKstart(RKR * rkr_, jack_client_t * jackclient_) {
    JackOUT = rkr_;
    pthread_mutex_init(&jmutex, NULL);

    // Detection already ran in main(), before RKR was constructed (see
    // HaikuDetectAudioSettingsEarly()) -- this just reports the result.
    // Do NOT re-latch J_SAMPLE_RATE/J_PERIOD or call Adjust_Upsample()
    // here: every effect object already exists by this point, sized for
    // whatever SAMPLE_RATE was in effect at construction -- changing it
    // now is what caused the crash.
    if (g_HaikuRealtimeDetected) {
        printf("[Rakarrack] Real-time audio settings detected (%u Hz / %d-frame buffers) -- "
            "using automatically.\n", g_HaikuDetectedRate, g_HaikuDetectedFrames);
    } else {
        printf("[Rakarrack] Real-time audio not detected -- using %u Hz / %d frames "
            "(buffer scaled to your detected rate).\n", g_HaikuDetectedRate, g_HaikuDetectedFrames);

        BAlert* rtAlert = new BAlert("Real-Time Audio",
            "Rakarrack works best when real-time configuration is enabled. Please enable "
            "real-time audio.", "OK", NULL, NULL, B_WIDTH_AS_USUAL, B_INFO_ALERT);
        rtAlert->Go();
    }

    // Several effects (Reverb in particular) have shown instability --
    // runaway/exploding output, clicking/popping -- at sample rates above
    // 48kHz on Haiku that we have not been able to root-cause (it is not
    // the SliderW/changepar() data race already fixed elsewhere in this
    // file and rakarrack.cxx; it reproduces from simply enabling Reverb,
    // no parameter touched). Rather than let users hit that blind, warn
    // up front when the detected device rate isn't 48kHz so they know to
    // either switch Haiku's Media preferences to 48kHz or expect trouble
    // from rate-sensitive effects.
    if (g_HaikuDetectedRate != 48000) {
        char rateMsg[256];
        snprintf(rateMsg, sizeof(rateMsg),
            "Your audio device is running at %u Hz instead of 48000 Hz. Some effects "
            "(especially Reverb) are known to become unstable -- runaway volume, clicking "
            "and popping -- at rates other than 48kHz. If you hear this, set Haiku's Media "
            "preferences to 48000 Hz.", g_HaikuDetectedRate);
        BAlert* rateAlert = new BAlert("Audio Sample Rate", rateMsg, "OK", NULL, NULL,
            B_WIDTH_AS_USUAL, B_WARNING_ALERT);
        rateAlert->Go();
    }

    int final_rate = (int)g_HaikuDetectedRate;
    int final_frames = (int)g_HaikuDetectedFrames;

    // 1. Initialize Ring Buffers using the detected rate
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
	
	static bool priority_set = false;
	if (!priority_set) {    
    	thread_id thisThread = find_thread(NULL); 
  	  	set_thread_priority(thisThread, B_REAL_TIME_PRIORITY);    
  	    fprintf(stderr, "[Rakarrack] Output Node Thread %d Realtime Priority Set.\n", (int)thisThread); 
    	priority_set = true;
	}

	
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

    // 4b. RUN THE EFFECTS CHAIN -- under jmutex, matching
    // RakarrackWindow::MessageReceived (haiku_native/haiku-rakarrack.cpp),
    // which holds this same mutex for the entire duration of every
    // changepar() call a slider triggers. Without this, a GUI-thread
    // changepar() -> settype()/setroomsize() (Reverb.C; delete comb[i];
    // comb[i] = new float[comblen[i]];) can run concurrently with this
    // thread reading/writing that exact buffer inside Alg() -- a data
    // race, confirmed in practice as the cause of Reverb's output
    // exploding exponentially over hundreds of calls (with completely
    // normal input) whenever a Reverb parameter was touched while
    // playing, worse at higher sample rates where comblen[] -- and so
    // the reallocation's own duration and processmono()'s per-callback
    // memory footprint -- is proportionally larger, widening the race
    // window. The 1. READ INPUT lock above is a separate, narrower
    // critical section (just the ring-buffer read) and is fully released
    // before this one begins, so there's no nesting/deadlock risk.
    pthread_mutex_lock(&jmutex);
    JackOUT->Alg(JackOUT->efxoutl, JackOUT->efxoutr, process_in_L, process_in_R, nframes);
    pthread_mutex_unlock(&jmutex);

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
    printf("[Rakarrack] Entering HaikuAudioShutdown...\n");
    BMediaRoster* roster = BMediaRoster::Roster();
    if (!roster) return;

    // 1. FORCE DISCONNECT FIRST
    if (inNode && gInputNode.node > 0) {
        media_input connectedInput;
        int32 inputCount = 0;
        media_node myNode = inNode->Node();

        // Attempt to find the link from our side
        if (roster->GetConnectedInputsFor(myNode, &connectedInput, 1, &inputCount) == B_OK && inputCount > 0) {
            printf("[Rakarrack] Breaking active link via our input...\n");
            roster->Disconnect(gInputNode.node, connectedInput.source, myNode.node, connectedInput.destination);
        } else {
            // Fallback: Search from the Hardware side
            printf("[Rakarrack] Searching via Hardware Node %d...\n", (int)gInputNode.node);
            media_output hwOutputs[8]; // Array for multiple outputs
            int32 hwOutCount = 0;
            if (roster->GetConnectedOutputsFor(gInputNode, hwOutputs, 8, &hwOutCount) == B_OK && hwOutCount > 0) {
                for (int i = 0; i < hwOutCount; i++) {
                    // Match by the 'port' ID which is the unique destination identifier
                    if (hwOutputs[i].destination.port == inNode->ControlPort()) {
                         printf("[Rakarrack] Found hardware output! Severing link...\n");
                         roster->Disconnect(hwOutputs[i].node.node, hwOutputs[i].source, myNode.node, hwOutputs[i].destination);
                    }
                }
            }
        }
    }
    
    printf("[Rakarrack] Cleaning up MIDI resources...\n");


// 1. MIDI DISCONNECT
    if (rk) {
        rk->MidiShutdown();
    }


    // 2. STOP AND RELEASE
    if (outPlayer) {
        printf("[Rakarrack] Stopping outPlayer (Sync)...\n");
        outPlayer->Stop(true, true); // Synchronous wait
        delete outPlayer;
        outPlayer = NULL;
    }
	
    if (inNode) {
        printf("[Rakarrack] Stopping and Releasing inNode...\n");
        roster->StopNode(inNode->Node(), 0, true);
        inNode->Release(); // Incremental cleanup
        inNode = NULL;
    }
    
    printf("[Rakarrack] Shutdown complete.\n");
}

