/*
 * MidiVoiceControl.c
 *
 *  Created on: 03.04.2012
 *
 * Copyright (C) 2012-2026 Erica Synths
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * This file is part of the LXR firmware. It is licensed under the GNU General
 * Public License version 3, with the additional permission and terms in
 * NOTICE.md. See LICENSE and NOTICE.md in the root of the repository.
 */


#include "Soundengine/MIDI/MidiVoiceControl.h"
#include "Soundengine/DSPAudio/DrumVoice.h"
#include "Soundengine/DSPAudio/Snare.h"
#include "Soundengine/DSPAudio/HiHat.h"
#include "Soundengine/MIDI/MidiMessages.h"
#include "Soundengine/DSPAudio/CymbalVoice.h"
#include "Soundengine/Sequencer/sequencer.h"
#include "Soundengine/TriggerOut.h"
#include "Soundengine/MIDI/Uart.h"
#include "Soundengine/MIDI/MidiParser.h"
//#include "LCD_driver.h"
uint8_t voiceStatus[NUM_DRUM_VOICES];
static uint8_t active_voices=0;	// which voices are currently playing a note
//----------------------------------------------------------------
// this fn assumes a valid voice is sent
// sendMidi: also send the note to MIDI/USB out. The sequencer passes 0 because
// it sends its own note, which takes the voice's MIDI note setting into account.
void voiceControl_noteOn(uint8_t voice, uint8_t note, uint8_t vel, uint8_t sendMidi)
{
	active_voices |= (1<<voice);

	if(voice < 3)
		Drum_trigger(voice, vel, note);
	else if(voice < 4)
		Snare_trigger(vel, note);
	else if(voice < 5)
		Cymbal_trigger(vel, note);
	else
		HiHat_trigger(vel,voice-5,note);
	
	//Send trigger out signal
	/*
	if(trigger_isGateModeOn())
	{
		if(vel)
			trigger_triggerVoice(voice, TRIGGER_ON);
	} else {
		trigger_triggerVoice(voice, TRIGGER_PULSE);
	}
	*/

	if (sendMidi)
	{
    uint8_t midiChan = midi_MidiChannels[voice];
		seq_sendMidiNoteOn(midiChan, note, vel);
	}

	// Send to front panel so it can pulse the LED
	uart_sendFrontpanelByte(NOTE_ON);
	uart_sendFrontpanelByte(voice);
	uart_sendFrontpanelByte(0);
}
//----------------------------------------------------------------
void voiceControl_noteOff(uint8_t voice)
{
	uint8_t midiChan; // which midi channel to send a note on

	if(voice==0xff)
	{
		active_voices = 0;
		seq_midiNoteOff(0xff);
		return;
	}

	//only set voice inactive and send MIDI off when voice is currently playing
	if(active_voices & (1<<voice))
	{
		active_voices &= (~(1<<voice));

		//send midi note off
		midiChan = midi_MidiChannels[voice];
		seq_midiNoteOff(midiChan);
	}
}
//----------------------------------------------------------------
uint8_t voiceControl_isVoicePlaying(uint8_t voice)
{
	return (active_voices & (1<<voice));
}
