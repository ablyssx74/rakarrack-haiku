#ifndef ALSA_DUMMY_H
#define ALSA_DUMMY_H

#include <stdint.h>

// Types
typedef void snd_seq_t;
typedef void snd_seq_addr_t;

// This provides the .data.note and .data.control members
typedef struct {
    uint8_t type;
    union {
        struct { uint8_t channel; uint8_t note; uint8_t velocity; } note;
        struct { uint8_t channel; uint8_t param; int32_t value; } control;
    } data;
} snd_seq_event_t;

// Constants (Keep these from before)
#define SND_SEQ_OPEN_INPUT 1
#define SND_SEQ_OPEN_OUTPUT 2
#define SND_SEQ_PORT_CAP_WRITE 1
#define SND_SEQ_PORT_CAP_SUBS_WRITE 2
#define SND_SEQ_PORT_CAP_READ 4
#define SND_SEQ_PORT_CAP_SUBS_READ 8
#define SND_SEQ_PORT_TYPE_SYNTH 1
#define SND_SEQ_PORT_TYPE_APPLICATION 2

#define SND_SEQ_EVENT_CLOCK 0
#define SND_SEQ_EVENT_START 1
#define SND_SEQ_EVENT_STOP 2
#define SND_SEQ_EVENT_NOTEON 3
#define SND_SEQ_EVENT_NOTEOFF 4
#define SND_SEQ_EVENT_PGMCHANGE 5
#define SND_SEQ_EVENT_CONTROLLER 6

extern "C" {
    int snd_seq_open(snd_seq_t**, const char*, int, int);
    int snd_seq_set_client_name(snd_seq_t*, const char*);
    void snd_config_update_free_global();
    int snd_seq_create_simple_port(snd_seq_t*, const char*, unsigned int, unsigned int);
    int snd_seq_event_input_pending(snd_seq_t*, int);
    int snd_seq_event_input(snd_seq_t*, snd_seq_event_t**);
    int snd_seq_close(snd_seq_t*);
    
    // MIDI Output macros/functions
    void snd_seq_ev_clear(snd_seq_event_t*);
    void snd_seq_ev_set_noteon(snd_seq_event_t*, int, int, int);
    void snd_seq_ev_set_noteoff(snd_seq_event_t*, int, int, int);
    void snd_seq_ev_set_subs(snd_seq_event_t*);
    void snd_seq_ev_set_direct(snd_seq_event_t*);
    int snd_seq_event_output_direct(snd_seq_t*, snd_seq_event_t*);
}

#endif
