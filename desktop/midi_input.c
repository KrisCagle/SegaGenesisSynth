#include "midi_input.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>

#include <math.h>
#include <stdio.h>

static HMIDIIN g_midi_in = NULL;
static char g_status[64] = "No MIDI device";
static MidiNoteOnCallback g_on_cb = NULL;
static MidiNoteOffCallback g_off_cb = NULL;

/* Runs on a Windows multimedia thread, not the UI or audio thread. Calls
 * straight into the app's note_on/note_off, same "no lock, plain integer
 * writes" simplification already used for the raylib audio callback. */
static void CALLBACK MidiInProc(HMIDIIN h, UINT wMsg, DWORD_PTR inst, DWORD_PTR param1, DWORD_PTR param2) {
    (void)h;
    (void)inst;
    (void)param2;

    if (wMsg == MIM_DATA) {
        DWORD msg = (DWORD)param1;
        unsigned char status = (unsigned char)(msg & 0xFF);
        unsigned char data1 = (unsigned char)((msg >> 8) & 0xFF);
        unsigned char data2 = (unsigned char)((msg >> 16) & 0xFF);
        unsigned char type = (unsigned char)(status & 0xF0);

        /* MIDI note-id space is 0-127; offset by 1000 so it never collides
         * with the app's QWERTY/mouse piano ids (0-12). */
        if (type == 0x90 && data2 > 0) {
            if (g_on_cb) {
                double freq = 440.0 * pow(2.0, (data1 - 69) / 12.0);
                g_on_cb(1000 + data1, freq);
            }
        } else if (type == 0x80 || (type == 0x90 && data2 == 0)) {
            if (g_off_cb) g_off_cb(1000 + data1);
        }
    }
}

const char *midi_input_init(MidiNoteOnCallback on_cb, MidiNoteOffCallback off_cb) {
    UINT num_devs = midiInGetNumDevs();
    g_on_cb = on_cb;
    g_off_cb = off_cb;

    if (num_devs > 0) {
        if (midiInOpen(&g_midi_in, 0, (DWORD_PTR)MidiInProc, 0, CALLBACK_FUNCTION) == MMSYSERR_NOERROR) {
            MIDIINCAPSA caps;
            if (midiInGetDevCapsA(0, &caps, sizeof(caps)) == MMSYSERR_NOERROR) {
                snprintf(g_status, sizeof(g_status), "MIDI: %s", caps.szPname);
            } else {
                snprintf(g_status, sizeof(g_status), "MIDI: connected");
            }
            midiInStart(g_midi_in);
        } else {
            snprintf(g_status, sizeof(g_status), "MIDI: open failed");
        }
    }

    return g_status;
}

void midi_input_shutdown(void) {
    if (g_midi_in) {
        midiInStop(g_midi_in);
        midiInClose(g_midi_in);
        g_midi_in = NULL;
    }
}
