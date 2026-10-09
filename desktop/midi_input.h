#ifndef MIDI_INPUT_H
#define MIDI_INPUT_H

/* Minimal Windows MIDI-in wrapper, kept in its own translation unit
 * deliberately: windows.h and raylib.h both define symbols like
 * `Rectangle` and `CloseWindow`, so this header (and its .c file) must
 * never be included alongside raylib.h in the same file. */

/* note: MIDI note number 0-127. velocity: 1-127. */
typedef void (*MidiNoteOnCallback)(int note, int velocity);
typedef void (*MidiNoteOffCallback)(int note);

/* Opens the first available MIDI-in device, if any. Always returns a
 * non-NULL, short human-readable status string for display (device name,
 * "No MIDI device", or an error), valid until the next call or shutdown. */
const char *midi_input_init(MidiNoteOnCallback on_cb, MidiNoteOffCallback off_cb);
void midi_input_shutdown(void);

#endif /* MIDI_INPUT_H */
