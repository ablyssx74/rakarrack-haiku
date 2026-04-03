#ifndef JACK_DUMMY_H
#define JACK_DUMMY_H

#include <stdint.h>
#include <stddef.h>

// Core Types
typedef uint32_t jack_nframes_t;
typedef uint64_t jack_time_t;
typedef float jack_default_audio_sample_t;
typedef unsigned char jack_midi_data_t;

#define JACK_DEFAULT_AUDIO_TYPE "32 bit float mono audio"
#define JACK_DEFAULT_MIDI_TYPE  "8 bit raw midi"
#define JackPortIsInput  0x1
#define JackPortIsOutput 0x2

// Pointer Handles
typedef void* jack_client_t;
typedef void* jack_port_t;

// Enums and Status
typedef int jack_options_t;
typedef int jack_status_t;
typedef int jack_transport_state_t;

// Structs
typedef struct {
    uint32_t frame;
    double beats_per_minute;
} jack_position_t;

// This replaces the old 'typedef void' version
typedef struct {
    jack_nframes_t time;
    size_t size;
    jack_midi_data_t *buffer;
} jack_midi_event_t;

// Function Prototypes for Linker satisfaction
extern "C" {
    void jack_set_process_callback(jack_client_t, int (*)(jack_nframes_t, void*), void*);
    void* jack_port_get_buffer(jack_port_t, jack_nframes_t);
    int jack_activate(jack_client_t);
    void jack_on_shutdown(jack_client_t, void (*)(void*), void*);
    int jack_connect(jack_client_t, const char*, const char*);
    const char* jack_port_name(const jack_port_t);
    float jack_cpu_load(jack_client_t);
    int jack_port_connected(const jack_port_t);
    void jack_client_close(jack_client_t);
    int jack_transport_query(jack_client_t, jack_position_t*);

    jack_client_t jack_client_open(const char*, jack_options_t, jack_status_t*, ...);
    const char* jack_get_client_name(jack_client_t);
    uint32_t jack_get_sample_rate(jack_client_t);
    jack_nframes_t jack_get_buffer_size(jack_client_t);
    
    // MIDI
    int jack_midi_get_event_count(void*);
    void jack_midi_clear_buffer(void*);
    int jack_midi_event_get(jack_midi_event_t*, void*, uint32_t);
    void jack_midi_event_write(void*, jack_nframes_t, const jack_midi_data_t*, size_t);
}

#endif
