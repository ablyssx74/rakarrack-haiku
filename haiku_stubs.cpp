#include "jack.h"
#include "alsa/asoundlib.h"
#include "global.h"
#include "config.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <SupportDefs.h>
#include <OS.h>

// Planar buffers for Rakarrack to write into
float input_buffer_L[8192];
float input_buffer_R[8192];
float temp_buffer_L[8192];
float temp_buffer_R[8192];

pthread_mutex_t jmutex;
RKR *JackOUT = NULL;
float* current_haiku_buffer = NULL;

extern float* current_haiku_buffer; 

extern "C" {
	int snd_seq_event_output_direct(snd_seq_t *seq, snd_seq_event_t *ev) { 
        return 0; 
	}
    int snd_seq_connect_from(snd_seq_t *seq, int my_port, int src_client, int src_port) { return 0; }
    int snd_seq_connect_to(snd_seq_t *seq, int my_port, int dest_client, int dest_port) { return 0; }        
    int jack_port_connected(const jack_port_t port) { 
        return 1; 
   		 }
	int jack_port_unregister(jack_client_t*, jack_port_t*) { return 0; }
	const char* jack_port_type(const jack_port_t*) { return "32 bit float mono audio"; }	
	jack_port_t* jack_port_by_name(jack_client_t* client, const char* name) {
    	// Return the name itself as the "handle"
   		 return (jack_port_t*)strdup(name);
		}

	jack_port_t* jack_port_register(jack_client_t*, const char* name, const char*, unsigned long, unsigned long) {
   		 // Also return the name as the "handle"
   		 return (jack_port_t*)strdup(name);
		}

	 uint32_t jack_get_sample_rate(jack_client_t) { 
    	return (uint32_t)DEFAULT_FRAME_RATE; 
		}

	 jack_nframes_t jack_get_buffer_size(jack_client_t) { 
    	return (jack_nframes_t)DEFAULT_BUFFER_FRAMES; 
		}

	void* jack_port_get_buffer(jack_port_t port, jack_nframes_t nframes) {
    const char* name = jack_port_name(port);
    // Use the dummy buffers if the engine isn't fully alive yet
    if (!name || !JackOUT || !JackOUT->efxoutl) return (void*)temp_buffer_L;

    // INPUTS: Where Rakarrack reads guitar data
    if (strstr(name, "in")) {
        if (strstr(name, "1")) return (void*)JackOUT->efxoutl;
        if (strstr(name, "2")) return (void*)JackOUT->efxoutr;
    	}
    
    // OUTPUTS: Where Rakarrack writes processed data
    // We point these to the same efxoutl/r so BSoundPlayer can see the result
    if (strstr(name, "out")) {
        if (strstr(name, "1")) return (void*)JackOUT->efxoutl;
        if (strstr(name, "2")) return (void*)JackOUT->efxoutr;
   		 }

    return (void*)JackOUT->efxoutl; 
	}

    // --- JACK Stubs ---
    jack_client_t jack_client_open(const char* name, jack_options_t, jack_status_t*, ...) { return (jack_client_t)0x12345; }
    const char* jack_get_client_name(jack_client_t) { return "Rakarrack-Haiku"; }
    void jack_set_process_callback(jack_client_t, int (*)(jack_nframes_t, void*), void*) {}
    int jack_activate(jack_client_t) { return 0; }
    void jack_on_shutdown(jack_client_t, void (*)(void*), void*) {}
    int jack_connect(jack_client_t, const char*, const char*) { return 0; }    
	const char* jack_port_name(jack_port_t port) {
    if (port == NULL) return "unknown";
    return (const char*)port; // We assume the port handle is the string pointer
	}    
    float jack_cpu_load(jack_client_t) { return 0.0f; }
    void jack_client_close(jack_client_t) {}
    int jack_transport_query(jack_client_t, jack_position_t*) { return 0; }
	const char** jack_get_ports(jack_client_t client, const char* name_pattern, const char* type_pattern, unsigned long flags) {
    char** ports = (char**)malloc(3 * sizeof(char*));
    
    // Check if Rakarrack is looking for its Inputs or Outputs
    // 0x1 is usually JackPortIsInput
    if (flags & 0x1) { 
        ports[0] = strdup("rakarrack:in_1");
        ports[1] = strdup("rakarrack:in_2");
    } else {
        ports[0] = strdup("rakarrack:out_1");
        ports[1] = strdup("rakarrack:out_2");
    }
    ports[2] = NULL;
    return (const char**)ports;
	}

    //void jack_free(void*) {}
    void jack_free(void* p) { if (p) free(p); }

    // --- MIDI Stubs ---
    int jack_midi_get_event_count(void* b) { return 0; }
    void jack_midi_clear_buffer(void* b) {}
    int jack_midi_event_get(jack_midi_event_t* e, void* b, uint32_t i) { return 0; }
    void jack_midi_event_write(void* b, jack_nframes_t t, const jack_midi_data_t* d, size_t s) {}

    // --- ALSA Stubs ---
    int snd_seq_open(snd_seq_t** s, const char* n, int st, int m) { *s = (snd_seq_t*)0x5678; return 0; }
    int snd_seq_set_client_name(snd_seq_t* s, const char* n) { return 0; }
    void snd_config_update_free_global() {}
    int snd_seq_create_simple_port(snd_seq_t* s, const char* n, unsigned int c, unsigned int t) { return 0; }
    int snd_seq_event_input_pending(snd_seq_t* s, int f) { return 0; }
    int snd_seq_event_input(snd_seq_t* s, snd_seq_event_t** ev) { *ev = NULL; return 0; }
    int snd_seq_close(snd_seq_t* s) { return 0; }
    void snd_seq_ev_clear(snd_seq_event_t* ev) {}
    void snd_seq_ev_set_noteon(snd_seq_event_t* ev, int c, int k, int v) {}
    void snd_seq_ev_set_noteoff(snd_seq_event_t* ev, int c, int k, int v) {}
    void snd_seq_ev_set_subs(snd_seq_event_t* ev) {}
    void snd_seq_ev_set_direct(snd_seq_event_t* ev) {}
}

// --- Helper Functions ---
char *strsep(char **stringp, const char *delim) {
    char *s; const char *spanp; int c, sc; char *tok;
    if ((s = *stringp) == NULL) return NULL;
    for (tok = s; ; ) {
        c = *s++; spanp = delim;
        do {
            if ((sc = *spanp++) == c) {
                if (c == 0) s = NULL; else s[-1] = 0;
                *stringp = s; return tok;
            }
        } while (sc != 0);
    }
}
