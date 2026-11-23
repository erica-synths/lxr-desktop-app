/*
 * MidiParser.c
 *
 *  Created on: 02.04.2012
 *
 * Copyright (C) 2012-2026 Erica Synths
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * This file is part of the LXR firmware. It is licensed under the GNU General
 * Public License version 3, with the additional permission and terms in
 * NOTICE.md. See LICENSE and NOTICE.md in the root of the repository.
 */


#include <Soundengine/MIDI/CortexParser.h>
#include "Soundengine/MIDI/MidiParser.h"
#include "Soundengine/MIDI/MidiNoteNumbers.h"
#include "Soundengine/DSPAudio/DrumVoice.h"
#include "Soundengine/DSPAudio/Snare.h"
#include "Soundengine/config.h"
#include "Soundengine/DSPAudio/HiHat.h"
#include "Soundengine/DSPAudio/CymbalVoice.h"
#include "Soundengine/MIDI/Uart.h"
#include "Soundengine/Sequencer/sequencer.h"
#include "Soundengine/Sequencer/clockSync.h"
#include "Soundengine/DSPAudio/mixer.h"
#include "Soundengine/MIDI/valueShaper.h"
#include "Soundengine/DSPAudio/modulationNode.h"
#include "Soundengine/USB/usb_manager.h"
#include "menu/menu.h"
#include "Soundengine/FxBus.h"

#include "Soundengine/MIDI/MidiMessages.h"

static uint16_t midiParser_activeNrpnNumber = 0;

uint8_t midiParser_originalCcValues[0xff];

// this will be set to some value if we are ignoring all mtc messages until the next 0 message
static uint8_t midiParser_mtcIgnore=1;
static uint32_t volatile midiParser_lastMtcReceived=0x0;
static uint8_t midiParser_mtcIsRunning=0;

static union {
	uint8_t value;
	struct {
		unsigned usb2midi:1;
		unsigned usb2usb:1;   // not used
		unsigned midi2midi:1;
		unsigned midi2usb:1;
		unsigned :4;
	} route;
} midiParser_routing = {0};



//tx is high nibble
uint8_t midiParser_txRxFilter = 0xFF;

enum State
{
	MIDI_STATUS,  		// waiting for status byte
	MIDI_DATA1,  		// waiting for data byte1
	MIDI_DATA2,  		// waiting for data byte2
	SYSEX_DATA,  		// read sysex data byte
	IGNORE				// set when unknown status byte received, stays in ignore mode until next known status byte
  };

// 2^(1/12) factor for 1 semitone
#define SEMITONE_UP 1.0594630943592952645618252949463f
#define SEMITONE_DOWN 0.94387431268169349664191315666753f

#define NUM_LFO 6
uint8_t midiParser_selectedLfoVoice[NUM_LFO] = {0,0,0,0,0,0};

#if 0
// -- AS for debugging
void midiDebugSend(uint8_t b1, uint8_t b2)
{
	uart_sendMidiByte(0xF2);
	uart_sendMidiByte(b1&0x7F);
	uart_sendMidiByte(b2&0x7F);
}
#endif

//----------------------------------------------------------
#if 0
inline uint16_t calcSlopeEgTime(uint8_t data2)
{
	float val = (data2+1)/128.f;
	return data2>0?val*val*data2*128:1;
}
#endif
//-----------------------------------------------------------
static inline float calcPitchModAmount(uint8_t data2)
{
	const float val = data2/127.f;
	return val*val*PITCH_AMOUNT_FACTOR;
}

//-----------------------------------------------------------
// vars
//-----------------------------------------------------------
uint8_t midi_MidiChannels[8];	// the currently selected midi channel for each voice (element 7 is global channel)

// note overrides for each voice
//uint8_t midi_NoteOverride[7];
//uint8_t midi_mode; // not used anymore
MidiMsg midiMsg_tmp;				// buffer message where the incoming data is stored
// these two are used only when building up a midi message
//static uint8_t msgLength;					// number of following data bytes expected for current status
static uint8_t parserState = IGNORE;	// state of the parser state machine. Set to what it's expecting next
										// we set it to ignore initially so that any random data we get before
										// a valid msg header is ignored

//-----------------------------------------------------------
//takes a midi value from 0 to 127 and return +/- 50 cent detune factor
float midiParser_calcDetune(uint8_t value)
{
	//linear interpolation between 1(no change) and semitone up/down)
	float frac = (value/127.f -0.5f);
	float cent = 1;
	if(cent>=0)
	{
		cent += frac*(SEMITONE_UP - 1);
	}
	else
	{
		cent += frac*(SEMITONE_UP - 1);
	}
	return cent;
}
//-----------------------------------------------------------
static void midiParser_nrpnHandler(uint16_t value)
{
	MidiMsg msg2;
	msg2.status = MIDI_CC2;
	msg2.data1 = midiParser_activeNrpnNumber;
	msg2.data2 = value;
	midiParser_ccHandler(msg2,true, 0, 0.f);
}
//-----------------------------------------------------------
/** handle all incoming CCs and invoke action*/
void midiParser_ccHandler(MidiMsg msg, uint8_t updateOriginalValue, uint8_t isFloatModulator, float v)
{

	struct
	{
		uint8_t itg;
		float flt;
	} value;

	if(isFloatModulator)
	{
		value.itg = v*127;
		value.flt = v;
	}
	else
	{
		value.itg = msg.data2;;
		value.flt = value.itg/127.f;
	}

	if((msg.status &0xF0) == MIDI_CC)
	{

		const uint16_t paramNr = msg.data1-1;
		if(updateOriginalValue && !isFloatModulator) {
			midiParser_originalCcValues[paramNr+1] = value.itg;
		}

		switch(msg.data1)
		{

		case CC_MOD_WHEEL:

			break;

		case CC_BANK_CHANGE:
			// bank change (coarse) selects kit (sound)
			/*
			 * already send in parseMidiMessage()
			uart_sendFrontpanelByte(MIDI_CC);
			uart_sendFrontpanelByte(CC_BANK_CHANGE);
			uart_sendFrontpanelByte(value.itg);
			*/
			break;

		case NRPN_DATA_ENTRY_COARSE:
			midiParser_nrpnHandler(value.itg);
			return;
			break;

		case NRPN_FINE:
			midiParser_activeNrpnNumber &= ~(0x7f);	//clear lower 7 bit
			midiParser_activeNrpnNumber |= (value.itg&0x7f);
			break;

		case NRPN_COARSE:
			midiParser_activeNrpnNumber &= 0x7f;	//clear upper 7 bit
			midiParser_activeNrpnNumber |= (value.itg<<7);
			break;

		case VOL_SLOPE1:
			slopeEg2_setSlope(&voiceArray[0].oscVolEg,value.itg);

			break;
		case PITCH_SLOPE1:
			DecayEg_setSlope(&voiceArray[0].oscPitchEg,value.itg);
			break;

		case VOL_SLOPE2:
			slopeEg2_setSlope(&voiceArray[1].oscVolEg,value.itg);
			break;
		case PITCH_SLOPE2:
			DecayEg_setSlope(&voiceArray[1].oscPitchEg,value.itg);
			break;

		case VOL_SLOPE3:
			slopeEg2_setSlope(&voiceArray[2].oscVolEg,value.itg);
			break;
		case PITCH_SLOPE3:
			DecayEg_setSlope(&voiceArray[2].oscPitchEg,value.itg);
			break;

		case PITCH_SLOPE4:
			DecayEg_setSlope(&snareVoice.oscPitchEg,value.itg);
			break;
		case VOL_SLOPE6:
			slopeEg2_setSlope(&hatVoice.oscVolEg,value.itg);
			break;

		case FILTER_FREQ_DRUM1:
		case FILTER_FREQ_DRUM2:
		case FILTER_FREQ_DRUM3:
		{
			const float f = value.flt;//value.itg/127.f;
			//exponential full range freq
			SVF_directSetFilterValue(&voiceArray[msg.data1-FILTER_FREQ_DRUM1].filter,valueShaperF2F(f,FILTER_SHAPER) );
		}
			break;
		case RESO_DRUM1:
		case RESO_DRUM2:
		case RESO_DRUM3:
			SVF_setReso(&voiceArray[msg.data1-RESO_DRUM1].filter, value.flt);
			break;

		case F_OSC1_COARSE:
		{
			//clear upper nibble
			voiceArray[0].osc.midiFreq &= 0x00ff;
			//set upper nibble
			voiceArray[0].osc.midiFreq |= value.itg << 8;
			osc_recalcFreq(&voiceArray[0].osc);
		}
			break;
		case F_OSC2_COARSE:
		{
			//clear upper nibble
			voiceArray[1].osc.midiFreq &= 0x00ff;
			//set upper nibble
			voiceArray[1].osc.midiFreq |= value.itg << 8;
			osc_recalcFreq(&voiceArray[1].osc);
		}
				break;
		case F_OSC3_COARSE:
		{
			//clear upper nibble
			voiceArray[2].osc.midiFreq &= 0x00ff;
			//set upper nibble
			voiceArray[2].osc.midiFreq |= value.itg << 8;
			osc_recalcFreq(&voiceArray[2].osc);
		}
				break;
		case F_OSC4_COARSE:
		{
			//clear upper nibble
			snareVoice.osc.midiFreq &= 0x00ff;
			//set upper nibble
			snareVoice.osc.midiFreq |= value.itg << 8;
			osc_recalcFreq(&snareVoice.osc);
		}
				break;
		case F_OSC5_COARSE:
		{
			//clear upper nibble
			cymbalVoice.osc.midiFreq &= 0x00ff;
			//set upper nibble
			cymbalVoice.osc.midiFreq |= value.itg << 8;
			osc_recalcFreq(&cymbalVoice.osc);
		}
				break;

		case F_OSC1_FINE:
		{
			//clear lower nibble
			voiceArray[0].osc.midiFreq &= 0xff00;
			//set lower nibble
			voiceArray[0].osc.midiFreq |= value.itg;
			osc_recalcFreq(&voiceArray[0].osc);
		}
			break;
		case F_OSC2_FINE:
		{
			//clear lower nibble
			voiceArray[1].osc.midiFreq &= 0xff00;
			//set lower nibble
			voiceArray[1].osc.midiFreq |= value.itg;
			osc_recalcFreq(&voiceArray[1].osc);
		}
				break;
		case F_OSC3_FINE:
		{
			//clear lower nibble
			voiceArray[2].osc.midiFreq &= 0xff00;
			//set lower nibble
			voiceArray[2].osc.midiFreq |= value.itg;
			osc_recalcFreq(&voiceArray[2].osc);
		}
				break;
		case F_OSC4_FINE:
		{
			//clear lower nibble
			snareVoice.osc.midiFreq &= 0xff00;
			//set lower nibble
			snareVoice.osc.midiFreq |= value.itg;
			osc_recalcFreq(&snareVoice.osc);
		}
				break;

		case F_OSC5_FINE:
		{
			//clear lower nibble
			cymbalVoice.osc.midiFreq &= 0xff00;
			//set lower nibble
			cymbalVoice.osc.midiFreq |= value.itg;
			osc_recalcFreq(&cymbalVoice.osc);
		}
				break;

		case F_OSC6_FINE:
		{
			//clear lower nibble
			hatVoice.osc.midiFreq &= 0xff00;
			//set lower nibble
			hatVoice.osc.midiFreq |= value.itg;
			osc_recalcFreq(&hatVoice.osc);
		}
				break;

		case OSC_WAVE_DRUM1:
			if(isFloatModulator)
			{
				uint8_t wave = NUM_OSC_WAVEFORMS * value.flt;
				wave = wave >= NUM_OSC_WAVEFORMS ? NUM_OSC_WAVEFORMS-1 : wave;
				voiceArray[0].osc.waveform = wave;
			}
			else
			{
				voiceArray[0].osc.waveform = value.itg;
			}
			break;

		case MOD_WAVE_DRUM1:
			if(isFloatModulator)
			{
				uint8_t wave = NUM_OSC_WAVEFORMS * value.flt;
				wave = wave >= NUM_OSC_WAVEFORMS ? NUM_OSC_WAVEFORMS-1 : wave;
				voiceArray[0].modOsc.waveform = wave;
			}
			else
			{
				voiceArray[0].modOsc.waveform = value.itg;
			}
			break;

		case OSC_WAVE_DRUM2:
			if(isFloatModulator)
			{
				uint8_t wave = NUM_OSC_WAVEFORMS * value.flt;
				wave = wave >= NUM_OSC_WAVEFORMS ? NUM_OSC_WAVEFORMS-1 : wave;
				voiceArray[1].osc.waveform = wave;
			}
			else
			{
				voiceArray[1].osc.waveform = value.itg;
			}
			break;

		case MOD_WAVE_DRUM2:
			if(isFloatModulator)
			{
				uint8_t wave = NUM_OSC_WAVEFORMS * value.flt;
				wave = wave >= NUM_OSC_WAVEFORMS ? NUM_OSC_WAVEFORMS-1 : wave;
				voiceArray[1].modOsc.waveform = wave;
			}
			else
			{
				voiceArray[1].modOsc.waveform = value.itg;
			}
			break;

		case OSC_WAVE_DRUM3:
			if(isFloatModulator)
			{
				uint8_t wave = NUM_OSC_WAVEFORMS * value.flt;
				wave = wave >= NUM_OSC_WAVEFORMS ? NUM_OSC_WAVEFORMS-1 : wave;
				voiceArray[2].osc.waveform = wave;
			}
			else
			{
				voiceArray[2].osc.waveform = value.itg;
			}
			break;

		case MOD_WAVE_DRUM3:
			if(isFloatModulator)
			{
				uint8_t wave = NUM_OSC_WAVEFORMS * value.flt;
				wave = wave >= NUM_OSC_WAVEFORMS ? NUM_OSC_WAVEFORMS-1 : wave;
				voiceArray[2].modOsc.waveform = wave;
			}
			else
			{
				voiceArray[2].modOsc.waveform = value.itg;
			}
			break;

		case OSC_WAVE_SNARE:
			if(isFloatModulator)
			{
				uint8_t wave = NUM_OSC_WAVEFORMS * value.flt;
				wave = wave >= NUM_OSC_WAVEFORMS ? NUM_OSC_WAVEFORMS-1 : wave;
				snareVoice.osc.waveform = wave;
			}
			else
			{
				snareVoice.osc.waveform = value.itg;
			}
			break;


		case OSC1_DIST:
#if USE_FILTER_DRIVE
			voiceArray[0].filter.drive = 0.5f + (value.itg/127.f) *6;
#else
			setDistortionShape(&voiceArray[0].distortion,value.flt);
#endif
			break;
		case OSC2_DIST:
#if USE_FILTER_DRIVE
			voiceArray[1].filter.drive = 0.5f + (value.itg/127.f)*6;
#else
			setDistortionShape(&voiceArray[1].distortion,value.flt);
#endif
			break;
		case OSC3_DIST:
#if USE_FILTER_DRIVE
			voiceArray[3].filter.drive = 0.5f + (value.itg/127.f)*6;
#else
			setDistortionShape(&voiceArray[2].distortion,value.flt);
#endif
			break;

		case VELOA1:
			slopeEg2_setAttack(&voiceArray[0].oscVolEg,value.itg,AMP_EG_SYNC);
			break;

		case VELOD1:
		{
			slopeEg2_setDecay(&voiceArray[0].oscVolEg,value.itg,AMP_EG_SYNC);
		}
			break;

		case PITCHD1:
		{
			DecayEg_setDecay(&voiceArray[0].oscPitchEg,value.itg);
		}
			break;

		case MODAMNT1:
			voiceArray[0].egPitchModAmount = calcPitchModAmount(value.itg);
			break;

		case FMAMNT1:
			voiceArray[0].fmModAmount = value.flt;
			break;

		case FMDTN1:
			//clear upper nibble
			voiceArray[0].modOsc.midiFreq &= 0x00ff;
			//set upper nibble
			voiceArray[0].modOsc.midiFreq |= value.itg << 8;
			osc_recalcFreq(&voiceArray[0].modOsc);
			break;

		case VOL1:
			voiceArray[0].vol = value.flt;
			break;

		case PAN1:
			setPan(0,value.itg);
			break;

		case VELOA2:
			slopeEg2_setAttack(&voiceArray[1].oscVolEg,value.itg,AMP_EG_SYNC);
			break;

			case VELOD2:
			slopeEg2_setDecay(&voiceArray[1].oscVolEg,value.itg,AMP_EG_SYNC);
			break;

			case PITCHD2:
			DecayEg_setDecay(&voiceArray[1].oscPitchEg,value.itg);
			break;

			case MODAMNT2:
			voiceArray[1].egPitchModAmount = calcPitchModAmount(value.itg);
			break;

			case FMAMNT2:
			voiceArray[1].fmModAmount = value.flt;
				break;

			case FMDTN2:
				//clear upper nibble
				voiceArray[1].modOsc.midiFreq &= 0x00ff;
				//set upper nibble
				voiceArray[1].modOsc.midiFreq |= value.itg << 8;
				osc_recalcFreq(&voiceArray[1].modOsc);
				break;

			case VOL2:
				voiceArray[1].vol = value.flt;
				break;

			case PAN2:
				setPan(1,value.itg);
				break;


			case VELOA3:
				slopeEg2_setAttack(&voiceArray[2].oscVolEg,value.itg,AMP_EG_SYNC);
				break;

			case VELOD3:
				slopeEg2_setDecay(&voiceArray[2].oscVolEg,value.itg,AMP_EG_SYNC);
				break;

			case PITCHD3:
				DecayEg_setDecay(&voiceArray[2].oscPitchEg,value.itg);
				break;

			case MODAMNT3:
				voiceArray[2].egPitchModAmount = calcPitchModAmount(value.itg);
				break;

			case FMAMNT3:
				voiceArray[2].fmModAmount = value.flt;
				break;

			case FMDTN3:
				//clear upper nibble
				voiceArray[2].modOsc.midiFreq &= 0x00ff;
				//set upper nibble
				voiceArray[2].modOsc.midiFreq |= value.itg << 8;
				osc_recalcFreq(&voiceArray[2].modOsc);
				break;

			case VOL3:
				voiceArray[2].vol = value.flt;
				break;

			case PAN3:
				setPan(2,value.itg);
				break;

			//snare
			case VOL4:
				snareVoice.vol = value.flt;
				break;

			case PAN4:
				Snare_setPan(value.itg);

				break;

			case SNARE_NOISE_F:
				snareVoice.noiseOsc.freq = value.flt*22000;
				//TODO respond to midi note
				break;
			case VELOA4:
				slopeEg2_setAttack(&snareVoice.oscVolEg,value.itg,false);
				break;

			case VELOD4:
				slopeEg2_setDecay(&snareVoice.oscVolEg,value.itg,false);
				break;

			case PITCHD4:
				DecayEg_setDecay(&snareVoice.oscPitchEg,value.itg);
				break;

			case MODAMNT4:
				snareVoice.egPitchModAmount = calcPitchModAmount(value.itg);
				break;
			case SNARE_FILTER_F:
				{
					#if USE_PEAK
						peak_setFreq(&snareVoice.filter, value.itg/127.f*20000.f);
					#else
						const float f = value.flt;
						//exponential full range freq
						SVF_directSetFilterValue(&snareVoice.filter,valueShaperF2F(f,FILTER_SHAPER) );
					#endif
				}
					break;

			case SNARE_RESO:
#if USE_PEAK
				peak_setGain(&snareVoice.filter, value.itg/127.f);
#else
				SVF_setReso(&snareVoice.filter, value.flt);
#endif
				break;

			case SNARE_MIX:
				snareVoice.mix = value.flt;
				break;

			//snare 2
			case VOL5:
				cymbalVoice.vol = value.flt;
				break;

			case PAN5:
				Cymbal_setPan(value.itg);
				break;

			case VELOA5:
				slopeEg2_setAttack(&cymbalVoice.oscVolEg,value.itg,false);
				break;

			case VELOD5:
				slopeEg2_setDecay(&cymbalVoice.oscVolEg,value.itg,false);
				break;

			case CYM_WAVE1:
				if(isFloatModulator)
				{
					uint8_t wave = NUM_OSC_WAVEFORMS * value.flt;
					wave = wave >= NUM_OSC_WAVEFORMS ? NUM_OSC_WAVEFORMS-1 : wave;
					cymbalVoice.osc.waveform = wave;
				}
				else
				{
					cymbalVoice.osc.waveform = value.itg;
				}
				break;

			case CYM_WAVE2:
				if(isFloatModulator)
				{
					uint8_t wave = NUM_OSC_WAVEFORMS * value.flt;
					wave = wave >= NUM_OSC_WAVEFORMS ? NUM_OSC_WAVEFORMS-1 : wave;
					cymbalVoice.modOsc.waveform = wave;
				}
				else
				{
					cymbalVoice.modOsc.waveform = value.itg;
				}
				break;

			case CYM_WAVE3:
				if(isFloatModulator)
				{
					uint8_t wave = NUM_OSC_WAVEFORMS * value.flt;
					wave = wave >= NUM_OSC_WAVEFORMS ? NUM_OSC_WAVEFORMS-1 : wave;
					cymbalVoice.modOsc2.waveform = wave;
				}
				else
				{
					cymbalVoice.modOsc2.waveform = value.itg;
				}
				break;

			case CYM_MOD_OSC_F1:
				//cymbalVoice.modOsc.freq = MidiNoteFrequencies[value.itg];
				//clear upper nibble
				cymbalVoice.modOsc.midiFreq &= 0x00ff;
				//set upper nibble
				cymbalVoice.modOsc.midiFreq |= value.itg << 8;
				osc_recalcFreq(&cymbalVoice.modOsc);
				break;

			case CYM_MOD_OSC_F2:
				//clear upper nibble
				cymbalVoice.modOsc2.midiFreq &= 0x00ff;
				//set upper nibble
				cymbalVoice.modOsc2.midiFreq |= value.itg << 8;
				osc_recalcFreq(&cymbalVoice.modOsc2);
				break;

			case CYM_MOD_OSC_GAIN1:
				cymbalVoice.fmModAmount1 = value.flt;
				break;

			case CYM_MOD_OSC_GAIN2:
				cymbalVoice.fmModAmount2 = value.flt;
				break;

			case CYM_FIL_FREQ:
			{
				const float f = value.flt;
				//exponential full range freq
				SVF_directSetFilterValue(&cymbalVoice.filter,valueShaperF2F(f,FILTER_SHAPER) );
			}
				break;

			case CYM_RESO:
				SVF_setReso(&cymbalVoice.filter, value.flt);
				break;

			case CYM_REPEAT:
				cymbalVoice.oscVolEg.repeat = value.itg;
				break;
			case CYM_SLOPE:
				slopeEg2_setSlope(&cymbalVoice.oscVolEg,value.itg);
				break;

			// hat
			case WAVE1_HH:
				if(isFloatModulator)
				{
					uint8_t wave = NUM_OSC_WAVEFORMS * value.flt;
					wave = wave >= NUM_OSC_WAVEFORMS ? NUM_OSC_WAVEFORMS-1 : wave;
					hatVoice.osc.waveform = wave;
				}
				else
				{
					hatVoice.osc.waveform = value.itg;
				}
				break;

			case WAVE2_HH:
				if(isFloatModulator)
				{
					uint8_t wave = NUM_OSC_WAVEFORMS * value.flt;
					wave = wave >= NUM_OSC_WAVEFORMS ? NUM_OSC_WAVEFORMS-1 : wave;
					hatVoice.modOsc.waveform = wave;
				}
				else
				{
					hatVoice.modOsc.waveform = value.itg;
				}
				break;
			case WAVE3_HH:
				if(isFloatModulator)
				{
					uint8_t wave = NUM_OSC_WAVEFORMS * value.flt;
					wave = wave >= NUM_OSC_WAVEFORMS ? NUM_OSC_WAVEFORMS-1 : wave;
					hatVoice.modOsc2.waveform = wave;
				}
				else
				{
					hatVoice.modOsc2.waveform = value.itg;
				}
				break;

			case VELOD6:
				hatVoice.decayClosed = slopeEg2_calcDecay(value.itg);
				break;

			case VELOD6_OPEN:
				hatVoice.decayOpen = slopeEg2_calcDecay(value.itg);
				break;

			case HAT_FILTER_F:
				{
					const float f = value.flt;
					//exponential full range freq
					SVF_directSetFilterValue(&hatVoice.filter,valueShaperF2F(f,FILTER_SHAPER) );
				}
				break;

			case HAT_RESO:
				SVF_setReso(&hatVoice.filter, value.flt);
				break;

			case VOL6:
				hatVoice.vol = value.flt;
				break;

			case PAN6:
				HiHat_setPan(value.itg);
				break;

			case F_OSC6_COARSE:
				{
					//clear upper nibble
					hatVoice.osc.midiFreq &= 0x00ff;
					//set upper nibble
					hatVoice.osc.midiFreq |= value.itg << 8;
					osc_recalcFreq(&hatVoice.osc);
				}
				break;

			case MOD_OSC_F1:
				//clear upper nibble
				hatVoice.modOsc.midiFreq &= 0x00ff;
				//set upper nibble
				hatVoice.modOsc.midiFreq |= value.itg << 8;
				osc_recalcFreq(&hatVoice.modOsc);
				break;

			case MOD_OSC_F2:
				//clear upper nibble
				hatVoice.modOsc2.midiFreq &= 0x00ff;
				//set upper nibble
				hatVoice.modOsc2.midiFreq |= value.itg << 8;
				osc_recalcFreq(&hatVoice.modOsc2);
				break;

			case MOD_OSC_GAIN1:
				hatVoice.fmModAmount1 = value.flt;
				break;

			case MOD_OSC_GAIN2:
				hatVoice.fmModAmount2 = value.flt;
				break;

			case VELOA6:
				slopeEg2_setAttack(&hatVoice.oscVolEg,value.itg,false);
				break;

			case REPEAT1:
				snareVoice.oscVolEg.repeat = value.itg;
				break;


			case EG_SNARE1_SLOPE:
				slopeEg2_setSlope(&snareVoice.oscVolEg,value.itg);
				break;

			case SNARE_DISTORTION:
				setDistortionShape(&snareVoice.distortion,value.flt);
				break;

			case CYMBAL_DISTORTION:
				setDistortionShape(&cymbalVoice.distortion,value.flt);
				break;

			case HAT_DISTORTION:
				setDistortionShape(&hatVoice.distortion,value.flt);
				break;

			case FREQ_LFO1:
			case FREQ_LFO2:
			case FREQ_LFO3:
				lfo_setFreq(&voiceArray[msg.data1-FREQ_LFO1].lfo,value.flt);
								break;
			case FREQ_LFO4:
				lfo_setFreq(&snareVoice.lfo,value.flt);
								break;
			case FREQ_LFO5:
				lfo_setFreq(&cymbalVoice.lfo,value.flt);
								break;
			case FREQ_LFO6:
				lfo_setFreq(&hatVoice.lfo,value.flt);
				break;

			case AMOUNT_LFO1:
			case AMOUNT_LFO2:
			case AMOUNT_LFO3:
				voiceArray[msg.data1-AMOUNT_LFO1].lfo.modTarget.amount = value.flt;
				break;
			case AMOUNT_LFO4:
				snareVoice.lfo.modTarget.amount = value.flt;
				break;
			case AMOUNT_LFO5:
				cymbalVoice.lfo.modTarget.amount = value.flt;
				break;
			case AMOUNT_LFO6:
				hatVoice.lfo.modTarget.amount = value.flt;
				break;

			case VOICE_DECIMATION1:
			case VOICE_DECIMATION2:
			case VOICE_DECIMATION3:
			case VOICE_DECIMATION4:
			case VOICE_DECIMATION5:
			case VOICE_DECIMATION6:
			case VOICE_DECIMATION_ALL:
				mixer_decimation_rate[msg.data1-VOICE_DECIMATION1] = valueShaperF2F(value.flt,-0.7f); ;//valueShaperI2F(value.itg,-0.7f);
				break;

			default:
				break;
		}
		if(!isFloatModulator)
		{
			modNode_originalValueChanged(paramNr);
		}
	} //msg.status == MIDI_CC

	else //MIDI_CC2
	{
		const uint16_t paramNr = msg.data1 + 1 + 127;

		if(updateOriginalValue && !isFloatModulator) {
			midiParser_originalCcValues[paramNr] = value.itg;
		}
		switch(msg.data1)
		{

			case CC2_TRANS1_WAVE:
				if(isFloatModulator)
				{
					uint8_t wave = NUM_TRANSIENTS * value.flt;
					wave = wave >= NUM_TRANSIENTS ? NUM_TRANSIENTS-1 : wave;
					transient_setWaveform(&voiceArray[0].transGen, wave);
				}
				else
				{
					transient_setWaveform(&voiceArray[0].transGen, value.itg);
				}
				break;

			case CC2_TRANS1_VOL:
				voiceArray[0].transGen.volume = value.flt;
				break;

			case CC2_TRANS1_FREQ:
				voiceArray[0].transGen.pitch = 1.f + ((value.itg/33.9f)-0.75f) ;// range about  0.25 to 4 => 1/4 to 1*4
				break;

			case CC2_TRANS2_WAVE:
				if(isFloatModulator)
				{
					uint8_t wave = NUM_TRANSIENTS * value.flt;
					wave = wave >= NUM_TRANSIENTS ? NUM_TRANSIENTS-1 : wave;
					transient_setWaveform(&voiceArray[1].transGen, wave);
				}
				else
				{
					transient_setWaveform(&voiceArray[1].transGen, value.itg);
				}
				break;

			case CC2_TRANS2_VOL:
				voiceArray[1].transGen.volume = value.flt;
				break;

			case CC2_TRANS2_FREQ:
				voiceArray[1].transGen.pitch = 1.f + ((value.itg/33.9f)-0.75f) ;
				break;

			case CC2_TRANS3_WAVE:
				if(isFloatModulator)
				{
					uint8_t wave = NUM_TRANSIENTS * value.flt;
					wave = wave >= NUM_TRANSIENTS ? NUM_TRANSIENTS-1 : wave;
					transient_setWaveform(&voiceArray[2].transGen, wave);
				}
				else
				{
					transient_setWaveform(&voiceArray[2].transGen, value.itg);
				}
				break;

			case CC2_TRANS3_VOL:
				voiceArray[2].transGen.volume = value.flt;
				break;

			case CC2_TRANS3_FREQ:
				voiceArray[2].transGen.pitch = 1.f + ((value.itg/33.9f)-0.75f) ;
				break;

			case CC2_TRANS4_WAVE:
				if(isFloatModulator)
				{
					uint8_t wave = NUM_TRANSIENTS * value.flt;
					wave = wave >= NUM_TRANSIENTS ? NUM_TRANSIENTS-1 : wave;
					transient_setWaveform(&snareVoice.transGen, wave);
				}
				else
				{
					transient_setWaveform(&snareVoice.transGen, value.itg);
				}
				break;

			case CC2_TRANS4_VOL:
				snareVoice.transGen.volume = value.flt;
				break;
			case CC2_TRANS4_FREQ:
				snareVoice.transGen.pitch = 1.f + ((value.itg/33.9f)-0.75f) ;
				break;

			case CC2_TRANS5_WAVE:
				if(isFloatModulator)
				{
					uint8_t wave = NUM_TRANSIENTS * value.flt;
					wave = wave >= NUM_TRANSIENTS ? NUM_TRANSIENTS-1 : wave;
					transient_setWaveform(&cymbalVoice.transGen, wave);
				}
				else
				{
					transient_setWaveform(&cymbalVoice.transGen, value.itg);
				}
				break;

			case CC2_TRANS5_VOL:
				cymbalVoice.transGen.volume = value.flt;
				break;
			case CC2_TRANS5_FREQ:
				cymbalVoice.transGen.pitch = 1.f + ((value.itg/33.9f)-0.75f) ;
				break;

			case CC2_TRANS6_WAVE:
				if(isFloatModulator)
				{
					uint8_t wave = NUM_TRANSIENTS * value.flt;
					wave = wave >= NUM_TRANSIENTS ? NUM_TRANSIENTS-1 : wave;
					transient_setWaveform(&hatVoice.transGen, wave);
				}
				else
				{
					transient_setWaveform(&hatVoice.transGen, value.itg);
				}
				break;

			case CC2_TRANS6_VOL:
				hatVoice.transGen.volume = value.flt;
				break;
			case CC2_TRANS6_FREQ:
				hatVoice.transGen.pitch = 1.f + ((value.itg/33.9f)-0.75f) ;
				break;

			case CC2_FILTER_TYPE_1:
			case CC2_FILTER_TYPE_2:
			case CC2_FILTER_TYPE_3:
				if(isFloatModulator)
				{
					uint8_t type = FILTER_NAIVE_2_POLE * value.flt + 1;
					type = type >= FILTER_OFF ? FILTER_OFF-1 : type;
					voiceArray[msg.data1-CC2_FILTER_TYPE_1].filterType = type;
				}
				else
				{
					voiceArray[msg.data1-CC2_FILTER_TYPE_1].filterType = value.itg+1;
				}
				break;

			case CC2_FILTER_TYPE_4:
				if(isFloatModulator)
				{
					uint8_t type = FILTER_NAIVE_2_POLE * value.flt + 1;
					type = type >= FILTER_OFF ? FILTER_OFF-1 : type;
					snareVoice.filterType = type;
				}
				else
				{
					snareVoice.filterType = value.itg + 1; // +1 because 0 is filter off which results in silence
				}
				break;

			case CC2_FILTER_TYPE_5:
				//cymbal filter
				if(isFloatModulator)
				{
					uint8_t type = FILTER_NAIVE_2_POLE * value.flt + 1;
					type = type >= FILTER_OFF ? FILTER_OFF-1 : type;
					cymbalVoice.filterType = type;
				}
				else
				{
					cymbalVoice.filterType = value.itg+1;
				}
				break;

			case CC2_FILTER_TYPE_6:
				//Hihat filter
				if(isFloatModulator)
				{
					uint8_t type = FILTER_NAIVE_2_POLE * value.flt + 1;
					type = type >= FILTER_OFF ? FILTER_OFF-1 : type;
					hatVoice.filterType = type;
				}
				else
				{
					hatVoice.filterType = value.itg+1;
				}
				//SVF_reset(&hatVoice.filter);
				break;

			case CC2_FILTER_DRIVE_1:
			case CC2_FILTER_DRIVE_2:
			case CC2_FILTER_DRIVE_3:
#if UNIT_GAIN_DRIVE
				voiceArray[msg.data1-CC2_FILTER_DRIVE_1].filter.drive = (value.itg/127.f);
#else
				SVF_setDrive(&voiceArray[msg.data1-CC2_FILTER_DRIVE_1].filter, value.flt);
#endif
				break;
			case CC2_FILTER_DRIVE_4:
#if UNIT_GAIN_DRIVE
				snareVoice.filter.drive = (value.itg/127.f);
#else
				SVF_setDrive(&snareVoice.filter, value.flt);
#endif

				break;
			case CC2_FILTER_DRIVE_5:
#if UNIT_GAIN_DRIVE
				cymbalVoice.filter.drive = (value.itg/127.f);
#else
				SVF_setDrive(&cymbalVoice.filter, value.flt);
#endif

				break;
			case CC2_FILTER_DRIVE_6:
#if UNIT_GAIN_DRIVE
				hatVoice.filter.drive = (value.itg/127.f);
#else
				SVF_setDrive(&hatVoice.filter, value.flt);
#endif

				break;

			case CC2_MIX_MOD_1:
			case CC2_MIX_MOD_2:
			case CC2_MIX_MOD_3:
				voiceArray[msg.data1-CC2_MIX_MOD_1].mixOscs = value.itg != 0;
				break;

			case CC2_VOLUME_MOD_ON_OFF1:
			case CC2_VOLUME_MOD_ON_OFF2:
			case CC2_VOLUME_MOD_ON_OFF3:
				voiceArray[msg.data1-CC2_VOLUME_MOD_ON_OFF1].volumeMod = value.itg != 0;
				break;
			case CC2_VOLUME_MOD_ON_OFF4:
				snareVoice.volumeMod = value.itg != 0;
				break;
			case CC2_VOLUME_MOD_ON_OFF5:
				cymbalVoice.volumeMod = value.itg != 0;
				break;
			case CC2_VOLUME_MOD_ON_OFF6:
				hatVoice.volumeMod = value.itg != 0;
				break;

			case CC2_VELO_MOD_AMT_1:
			case CC2_VELO_MOD_AMT_2:
			case CC2_VELO_MOD_AMT_3:
			case CC2_VELO_MOD_AMT_4:
			case CC2_VELO_MOD_AMT_5:
			case CC2_VELO_MOD_AMT_6:
				velocityModulators[msg.data1-CC2_VELO_MOD_AMT_1].amount = value.flt;
				break;

			case CC2_WAVE_LFO1:
			case CC2_WAVE_LFO2:
			case CC2_WAVE_LFO3:
				if(isFloatModulator)
				{
					uint8_t wave = NUM_LFO_WAVEFORMS * value.flt;
					wave = wave >= NUM_LFO_WAVEFORMS ? NUM_LFO_WAVEFORMS-1 : wave;
					voiceArray[msg.data1-CC2_WAVE_LFO1].lfo.waveform = wave;
				}
				else
				{
					voiceArray[msg.data1-CC2_WAVE_LFO1].lfo.waveform = value.itg;
				}
				break;

			case CC2_WAVE_LFO4:
				if(isFloatModulator)
				{
					uint8_t wave = NUM_LFO_WAVEFORMS * value.flt;
					wave = wave >= NUM_LFO_WAVEFORMS ? NUM_LFO_WAVEFORMS-1 : wave;
					snareVoice.lfo.waveform = wave;
				}
				else
				{
					snareVoice.lfo.waveform = value.itg;
				}
				break;

			case CC2_WAVE_LFO5:
				if(isFloatModulator)
				{
					uint8_t wave = NUM_LFO_WAVEFORMS * value.flt;
					wave = wave >= NUM_LFO_WAVEFORMS ? NUM_LFO_WAVEFORMS-1 : wave;
					cymbalVoice.lfo.waveform = wave;
				}
				else
				{
					cymbalVoice.lfo.waveform = value.itg;
				}
				break;

			case CC2_WAVE_LFO6:
				if(isFloatModulator)
				{
					uint8_t wave = NUM_LFO_WAVEFORMS * value.flt;
					wave = wave >= NUM_LFO_WAVEFORMS ? NUM_LFO_WAVEFORMS-1 : wave;
					hatVoice.lfo.waveform = wave;
				}
				else
				{
					hatVoice.lfo.waveform = value.itg;
				}
				break;

			case CC2_RETRIGGER_LFO1:
			case CC2_RETRIGGER_LFO2:
			case CC2_RETRIGGER_LFO3:
				if(isFloatModulator)
				{
					uint8_t voice = 7 * value.flt;
					voiceArray[msg.data1-CC2_RETRIGGER_LFO1].lfo.retrigger = voice;
				}
				else
				{
					voiceArray[msg.data1-CC2_RETRIGGER_LFO1].lfo.retrigger = value.itg;
				}
				break;
			case CC2_RETRIGGER_LFO4:
				if(isFloatModulator)
				{
					uint8_t voice = 7 * value.flt;
					snareVoice.lfo.retrigger = voice;
				}
				else
				{
					snareVoice.lfo.retrigger = value.itg;
				}
				break;

			case CC2_RETRIGGER_LFO5:
				if(isFloatModulator)
				{
					uint8_t voice = 7 * value.flt;
					cymbalVoice.lfo.retrigger = voice;
				}
				else
				{
					cymbalVoice.lfo.retrigger = value.itg;
				}
				break;

			case CC2_RETRIGGER_LFO6:
				if(isFloatModulator)
				{
					uint8_t voice = 7 * value.flt;
					hatVoice.lfo.retrigger = voice;
				}
				else
				{
					hatVoice.lfo.retrigger = value.itg;
				}
				break;

			case CC2_SYNC_LFO1:
			case CC2_SYNC_LFO2:
			case CC2_SYNC_LFO3:
				lfo_setSync(&voiceArray[msg.data1-CC2_SYNC_LFO1].lfo, value.itg);
				break;
			case CC2_SYNC_LFO4:
				lfo_setSync(&snareVoice.lfo, value.itg);
				break;
			case CC2_SYNC_LFO5:
				lfo_setSync(&cymbalVoice.lfo, value.itg);
				break;
			case CC2_SYNC_LFO6:
				lfo_setSync(&hatVoice.lfo, value.itg);
				break;

			case CC2_VOICE_LFO1:
			case CC2_VOICE_LFO2:
			case CC2_VOICE_LFO3:
			case CC2_VOICE_LFO4:
			case CC2_VOICE_LFO5:
			case CC2_VOICE_LFO6:
				break;

			case CC2_TARGET_LFO1:
			case CC2_TARGET_LFO2:
			case CC2_TARGET_LFO3:
			case CC2_TARGET_LFO4:
			case CC2_TARGET_LFO5:
			case CC2_TARGET_LFO6:
				break;



			case CC2_OFFSET_LFO1:
			case CC2_OFFSET_LFO2:
			case CC2_OFFSET_LFO3:
				voiceArray[msg.data1-CC2_OFFSET_LFO1].lfo.phaseOffset = value.flt * 0xffffffff;
				break;
			case CC2_OFFSET_LFO4:
				snareVoice.lfo.phaseOffset = value.flt * 0xffffffff;
				break;
			case CC2_OFFSET_LFO5:
				cymbalVoice.lfo.phaseOffset = value.flt * 0xffffffff;
				break;
			case CC2_OFFSET_LFO6:
				hatVoice.lfo.phaseOffset = value.flt * 0xffffffff;
				break;

			case CC2_AUDIO_OUT1:
			case CC2_AUDIO_OUT2:
			case CC2_AUDIO_OUT3:
			case CC2_AUDIO_OUT4:
			case CC2_AUDIO_OUT5:
			case CC2_AUDIO_OUT6:
				mixer_audioRouting[msg.data1-CC2_AUDIO_OUT1] = value.itg;
				break;

				/*
			case CC2_MIDI_NOTE1:
			case CC2_MIDI_NOTE2:
			case CC2_MIDI_NOTE3:
			case CC2_MIDI_NOTE4:
			case CC2_MIDI_NOTE5:
			case CC2_MIDI_NOTE6:
			case CC2_MIDI_NOTE7:
				// set the note override for the voice. 0 means use the note value, anything else means
				// that the note will always play with that note
				//midi_NoteOverride[msg.data1-CC2_MIDI_NOTE1] = value.itg;
				break;
				*/

			case CC2_MUTE_1:
			case CC2_MUTE_2:
			case CC2_MUTE_3:
			case CC2_MUTE_4:
			case CC2_MUTE_5:
			case CC2_MUTE_6:
			case CC2_MUTE_7:
			{
				const uint8_t voiceNr = msg.data1 - CC2_MUTE_1;
				if(value.itg == 0)
				{
					seq_setMute(voiceNr,0);
				}
				else
				{
					seq_setMute(voiceNr,1);
				}

			}
				break;

			case CC2_PWM_1:
			case CC2_PWM_2:
			case CC2_PWM_3:
				voiceArray[msg.data1-CC2_PWM_1].pwm = value.flt;
				break;
			case CC2_PWM_4:
				snareVoice.pwm = value.flt;
				break;
			case CC2_PWM_5:
				cymbalVoice.pwm = value.flt;
				break;
			case CC2_PWM_6:
				hatVoice.pwm = value.flt;
				break;

			case CC2_FX_TYPE:
#if 1
#if USE_REVERB
				if((value.itg == FX_REVERB) && (fxBus.fxType != value.itg)) FxBus_prepareFxChange();
#else
				if( fxBus.fxType != value.itg) FxBus_prepareFxChange();
#endif
				fxBus.fxType = value.itg;
				FxBus_updateParameter();
				//change visible parameters to selected FX type
				if(menu_activePage == FX_MODE_PAGE)
				{
					if(menu_getSubPage() != fxBus.fxType)
						menu_switchSubPage(fxBus.fxType, 1 );
				}
#endif
				break;
			case CC2_FX_ROUTING:
				fxBus.audioRouting = value.itg;
				break;
			case CC2_FX_AMOUNT:
				fxBus.dryWet = value.itg/100.f;
				break;

			case CC2_FX_DIST_TYPE:
				fxBus.distortion.distortionType = value.itg;
				break;

			case CC2_FX_P1:
				FxBus_setParameter(0, value.flt);
				break;

			case CC2_FX_P2:
				FxBus_setParameter(1, value.flt);
				//fxBus.distortion.p1 = value.itg/127.f;
				break;

			case CC2_FX_P3:
				FxBus_setParameter(2, value.flt);
				//fxBus.distortion.p1 = value.itg/127.f;
				break;

			case CC2_FX_P4:
				FxBus_setParameter(3, value.flt);
				//fxBus.distortion.p1 = value.itg/127.f;
				break;

			case CC2_FX_RATIO:
				Compressor_setRatio(&fxBus.compressor, value.itg);
				break;

			case CC2_FX_WAVE:
				Ringmod_setWave(&fxBus.ringmod, value.itg);
				break;

			case CC2_FX_REVERB_TYPE:

				break;

			case CC2_FX_DELAY_TYPE:
				fxBus.delay.delayType = value.itg;
			break;

			default:
				break;
		}
		if(!isFloatModulator)
		{
			modNode_originalValueChanged(paramNr);
		}
	}
}

//-----------------------------------------------------------
// this will check whether mtc is running, and if so will
// check to see whether we need to stop the sequencer due to
// lack of recent mtc activity. It will also reset our running indicator
// if the sequencer has stopped for some other reason
void midiParser_checkMtc()
{
	if(!midiParser_mtcIsRunning)
		return;

	if(!seq_isRunning()) {
		// inform mtc that his services are no longer needed
		midiParser_mtcIsRunning=0;
		return;
	}

	// at a 24fps framerate (the lowest) we should receive a completed message (we receive one every 2 frames)
	// every 83 ms. our tick counter is .25 ms resolution
	if(systick_ticks-midiParser_lastMtcReceived > 100 * 4) { // overestimate, just in case something untoward should happen
		// too much time has elapsed since our last message. mtc has gone away.
		midiParser_mtcIsRunning=0;

		uart_sendFrontpanelByte(FRONT_SEQ_CC);
		uart_sendFrontpanelByte(FRONT_SEQ_RUN_STOP);
		uart_sendFrontpanelByte(0);
		// stop the sequencer
		seq_setRunning(0);
	}
}

//------------------------------------------------------
// this fn assumes a valid voice is sent
// do_rec - whether we want to allow it to be recorded and echoed - we will only record
//          to the active track for notes received from the global channel
static void midiParser_noteOn(uint8_t voice, uint8_t note, uint8_t vel, uint8_t do_rec)
{

	if(menu_activePage == SEQ_PAGE)
	{
		//use incoming MIDI notes to set sequencer/step note
		const uint8_t step = activeParameters[PAR_ACTIVE_STEP];
		const uint8_t trackNr = menu_getActiveVoice(); //max 6 => 0x6 = 0b110
		const uint8_t patternNr = menu_getViewedPattern();


		seq_patternSet.seq_subStepPattern[patternNr][trackNr][step].note = note;
		activeParameters[PAR_STEP_NOTE] = note;
		menu_repaint();

	}
	else
	{

		if(seq_isTrackMuted(voice))
			return;

		// check the midi mode for the voice. If it's "any", trigger that note.
		// if it's some other note, trigger the drum voice only if it matches, and in this case
		// trigger the default note

		//if(midi_NoteOverride[voice] != 0) {
		if(activeParameters[PAR_MIDI_NOTE1 + voice] != 0) {
			// looking for specific note
			//if(note==midi_NoteOverride[voice])
			if(note==activeParameters[PAR_MIDI_NOTE1 + voice])
				note=SEQ_DEFAULT_NOTE; // match found. Play default note
			else
				return; // note does not match. Do nothing
		}

		voiceControl_noteOn(voice,note,vel,1);

		//Recording Mode - record the note to sequencer and echo to channel of that voice
		// (we'd want to hear what's being recorded)
		if(do_rec) {
			const uint8_t chan=midi_MidiChannels[voice];

			// record note if rec is on
			seq_addNote(voice,vel, note);

			//if a note is on for that channel send note-off first
			seq_midiNoteOff(chan);
			//send the new note to midi/usb out
			// todo a user played note will end up being turned off if a pattern switch happens.
			//           to fix this we'd have to differentiate between a user played note and a
			//           note triggered from sequencer, and also recognize note off on user played note and send it.
			seq_sendMidiNoteOn(chan, note, vel);
		}

	}

}
//------------------------------------------------------
//static void midiParser_noteOff(uint8_t voice, uint8_t note, uint8_t vel)
//{
	// in case we ever need it
//}


//check if midi channel is selected for a voice or as global channel
uint8_t midiParser_isValidChannel(uint8_t chan)
{
	for(int i=0;i<8;i++)
	{
		if(chan == midi_MidiChannels[i]) return 1;
	}

	return 0;
}

uint8_t midiParser_cc2voiceNr(uint8_t cc)
{
	switch(cc)
	{

	//No voice
	default:
		case 0:
		case 1:
		case 6:
		case 98:
		case 99:
			return 0;

	//voice 1
		case 2:
		case 9:
		case 10:
		case 21:
		case 38:
		case 44:
		case 50:
		case 51:
		case 63:
		case 71:
		case 75:
		case 79:
		case 83:
		case 84:
		case 89:
		case 95:
		case 103:
		case 109:
		case 116:
		case 122:
			return 1;

	//voice 2
		case 3:
		case 11:
		case 12:
		case 22:
		case 39:
		case 45:
		case 52:
		case 53:
		case 64:
		case 72:
		case 76:
		case 80:
		case 85:
		case 86:
		case 90:
		case 96:
		case 104:
		case 110:
		case 117:
		case 123:
			return 2;

	//voice 3
		case 4:
		case 13:
		case 14:
		case 23:
		case 40:
		case 46:
		case 54:
		case 55:
		case 65:
		case 73:
		case 77:
		case 81:
		case 87:
		case 88:
		case 91:
		case 97:
		case 105:
		case 111:
		case 118:
		case 124:
			return 3;

	//voice 4
		case 5:
		case 15:
		case 16:
		case 28:
		case 29:
		case 41:
		case 47:
		case 56:
		case 57:
		case 66:
		case 69:
		case 74:
		case 78:
		case 82:
		case 92:
		case 100:
		case 106:
		case 112:
		case 119:
		case 125:
			return 4;

	//voice 5
		case 7:
		case 17:
		case 18:
		case 24:
		case 25:
		case 30:
		case 31:
		case 32:
		case 33:
		case 42:
		case 48:
		case 58:
		case 59:
		case 67:
		case 70:
		case 93:
		case 101:
		case 107:
		case 113:
		case 120:
		case 126:
			return 5;

	//voice 6
		case 8:
		case 19:
		case 20:
		case 26:
		case 27:
		case 34:
		case 35:
		case 36:
		case 37:
		case 43:
		case 49:
		case 60:
		case 61:
		case 62:
		case 68:
		case 94:
		case 102:
		case 108:
		case 114:
		case 121:
		case 127:
			return 6;

	//all voices
		case 115:
			return 7;

	}
}

//-----------------------------------------------------------
/** Parse incoming midi messages and invoke corresponding action
 */
void midiParser_parseMidiMessage(MidiMsg msg)
{
	// route message if needed
	if(midiParser_routing.value) {
		if(msg.bits.source==midiSourceUSB) {
			if(midiParser_routing.route.usb2midi){
				// route to midi out port
				uart_sendMidi(msg);
			}
		} else if(msg.bits.source==midiSourceMIDI) {
			if(midiParser_routing.route.midi2midi) {
				// route to midi out port
				uart_sendMidi(msg);
			}
			if(midiParser_routing.route.midi2usb) {
				// route to usb out port
				usb_sendMidi(msg);
			}
		}
	}

	if(msg.bits.sysxbyte)
		return; // no further action needed. we don't interpret sysex data right now

	// FILT filter messages here. Filter inline below to be more optimal
	// we are interested in the low nibble since we are Rx here


	if((msg.status & 0xF0) == 0xF0)
  {
		// non-channel specific messages (system messages)
		switch(msg.status)
    {
      case MIDI_CLOCK:
        if((midiParser_txRxFilter & MIDI_FILTER_SYSTEM) && seq_getExtSync())
          seq_sync();
        break;
      case MIDI_START:
        if((midiParser_txRxFilter & MIDI_FILTER_SYSTEM) && seq_getExtSync())
        {
          sync_midiResetPosition();
          sync_midiStartStop(1);
        }
        break;
      case MIDI_CONTINUE:
        if((midiParser_txRxFilter & MIDI_FILTER_SYSTEM) && seq_getExtSync())
          sync_midiStartStop(1);
        break;
      case MIDI_STOP:
        if((midiParser_txRxFilter & MIDI_FILTER_SYSTEM) && seq_getExtSync())
          sync_midiStartStop(0);
        break;
      case MIDI_SONG_POSITION_POINTER:
        {
          /* Two data bytes follow the status. Just like with the Pitch Wheel,
          * these two bytes are combined into a 14-bit value. (See Pitch Wheel).
          * This 14-bit value is the MIDI Beat upon which to start the song.
          * Songs are always assumed to start on a MIDI Beat of 0. Each MIDI
          * Beat spans 6 MIDI Clocks. In other words, each MIDI Beat is a 16th
          * note (since there are 24 MIDI Clocks in a quarter note).
          *
          */
          // TODO song position pointer is not implemented yet:
          // position = msg.data1 | (msg.data2<<7)  (in MIDI beats = 16th notes)
        }
        break;
      case MIDI_MTC_QFRAME:
        /* Strategy:
        * If we get through all 8 of the mtc messages with a value of 0, it means that
        * the song has just started playing. That is the only time we will start the
        * sequencer. so we will not start it if they start the tape recorder playing half way
        * through the song. Also, if the sequencer is running, or we are in external sync mode
        * we will ignore mtc messages as well
        */
        if ((midiParser_txRxFilter & MIDI_FILTER_SYSTEM) == 0)
          break; // ignore filtered

        // keep track of when we got the last mtc. only do this for the 0 msg to save time
        if((msg.data1 & 0x70)==0) {
          midiParser_lastMtcReceived=systick_ticks;
        }

        if(seq_isRunning()) {
          break; //already running, so we don't care to figure out where we are
        }
        if(seq_getExtSync())
          break; // bypass the lot. we are using external midi clock sync

        // figure out whether we are at 0:0:0:0
        if((msg.data1 & 0x7F)==0) { // this is the first mtc message of the set AND it's value is 0
          midiParser_mtcIgnore=0; // reset our level of ignorance
        } else if(midiParser_mtcIgnore) { // not the first msg and we are ignoring
          break; // get out of here fast
        } else if((msg.data1 & 0x70)!=0x70) { // messages 0 to 6 and we are not ignoring yet
          if((msg.data1 & 0x0F) !=0) {
            midiParser_mtcIgnore=1;
            break; // get out fast
          }
        } else { // message 7 and we are not ignoring yet
          if((msg.data1 & 0x01)==0) { // hour high nibble is 0
            // well, we got all the way thru all 8 messages with 0, so the song has just begun
            // tell the front that we've started running on our own
            uart_sendFrontpanelByte(FRONT_SEQ_CC);
            uart_sendFrontpanelByte(FRONT_SEQ_RUN_STOP);
            uart_sendFrontpanelByte(1);
            midiParser_mtcIgnore=1; // in case we happen to miss a 0 message. probably wouldn't happen, but...
            midiParser_mtcIsRunning=1;
            midiParser_lastMtcReceived=systick_ticks; // also might not be needed, but...
            // start the sequencer
            seq_setRunning(1);
          }
        }
        break;

      default:
        break;
		}
    // msg.status = msg.lastNonSysStatus;
	}
  else
  { // channel specific message
		const uint8_t msgonly = msg.status & 0xF0;
		const uint8_t chanonly= msg.status & 0x0F;

    msg.lastNonSysStatus = msg.status;

		if((msgonly & 0xE0) == 0x80) {
			// note on or note off message (one of these two only)
			if(midiParser_txRxFilter & MIDI_FILTER_NOTES) {
				int8_t v, q=-1;
				// if a note message comes in on global channel, then send that note to
				// the voice that is currently active on the front.
				if(midi_MidiChannels[7]==chanonly) {
					q=frontParser_activeTrack; // avoid sending it again later if the active track happens to have the same channel set
											   // (this might not really matter. does sending a note on twice do any harm?)
					if(msgonly==NOTE_ON && msg.data2) {
						midiParser_noteOn(frontParser_activeTrack, msg.data1, msg.data2, 1);
					} else { // NOTE_OFF or zero velocity note
						//midiParser_noteOff(frontParser_activeTrack, msg.data1, msg.data2);
					}
				}

				// check each voice to see if it cares about this message
				for(v=0;v<7;v++) {
					if(midi_MidiChannels[v]==chanonly && v != q) { // if channel match and we haven't sent it already for the voice
						if(msgonly==NOTE_ON && msg.data2) {
							midiParser_noteOn(v, msg.data1, msg.data2, 1);
							//Also used in sequencer trigger note function
						} else { // NOTE_OFF or zero velocity note
						//	midiParser_noteOff(v, msg.data1, msg.data2);
						}
					} // if channel matches
				} // for each voice
			} // check midi filter

		} else if(msgonly==PROG_CHANGE) {
			// respond to prog change and change patterns. This responds only when global channel matches the PC message's channel.
			if((midiParser_txRxFilter & MIDI_FILTER_PROG_C) && (chanonly == midi_MidiChannels[7]))
				seq_setNextPattern(msg.data1);

		} else if(msgonly==MIDI_CC){


			uint8_t voiceNr = midiParser_cc2voiceNr(msg.data1);

			if(voiceNr==0 || voiceNr >= 7) //any set midi channel
			{
				if((midiParser_txRxFilter & MIDI_FILTER_CC) && (midiParser_isValidChannel(chanonly) ))
				{
					//record automation if record is turned on
					seq_recordAutomation(frontParser_activeTrack, msg.data1, msg.data2);

					//handle midi data
					midiParser_ccHandler(msg,1, 0, 0.f);
					//we received a midi cc message forward it to the front panel
					uart_sendFrontpanelByte(msg.status);
					uart_sendFrontpanelByte(msg.data1);
					uart_sendFrontpanelByte(msg.data2);
				}
			}
			else
			{

				// respond to CC message. This responds only when global channel or voice channel matches the cc message's channel
				if((midiParser_txRxFilter & MIDI_FILTER_CC) && ((chanonly == midi_MidiChannels[voiceNr]) || (chanonly == midi_MidiChannels[7]) )) {
					//record automation if record is turned on
					seq_recordAutomation(frontParser_activeTrack, msg.data1, msg.data2);

					//handle midi data
					midiParser_ccHandler(msg,1, 0, 0.f);
					//we received a midi cc message forward it to the front panel
					uart_sendFrontpanelByte(msg.status);
					uart_sendFrontpanelByte(msg.data1);
					uart_sendFrontpanelByte(msg.data2);
				}
			}
		} else {
			// anything else
			// TODO MIDI_PITCH_WHEEL ?
		}
	} // channel specific vs non channel specific

}

//-----------------------------------------------------------

void midiParser_handleStatusByte(unsigned char data)
{
	// we received a channel voice/mode byte. set the status as appropriate
	switch(data&0xF0)
  {
    // 2 databyte messages
    case NOTE_OFF:
    case NOTE_ON:
    case MIDI_CC:
    case MIDI_PITCH_WHEEL:
    case MIDI_AT:
      midiMsg_tmp.status = data;	// store the new status byte
      parserState = MIDI_DATA1;	//status received, next should be data byte 1
      midiMsg_tmp.bits.length = 2;// status is followed by 2 data bytes
      break;

    // 1 databyte messages
    case PROG_CHANGE:
    case CHANNEL_PRESSURE:
      midiMsg_tmp.status = data;	// store the new status byte
      parserState = MIDI_DATA1;	//status received, next should be data byte 1
      midiMsg_tmp.bits.length = 1;// status is followed by 1 data bytes
      break;

    // messages we don't care about right now, and don't know how to handle or passthru (Are there any?).
    default:
      parserState = IGNORE;	// throw away any data bytes until next message
      midiMsg_tmp.bits.length = 0;
      break;
	}

  midiMsg_tmp.lastNonSysStatus = data;
}
//-----------------------------------------------------------
// This will build up the midi message and hand it off to
// parseMidiMessage when it's complete
volatile static uint8_t checkMIDIprocPAR_t = 0;
volatile static int8_t checkMIDIprocPAR = 0;

void midiParser_parseUartData(unsigned char data)
{
  checkMIDIprocPAR_t = ((checkMIDIprocPAR_t^1) & 0x01);
  checkMIDIprocPAR = (int8_t)checkMIDIprocPAR_t - 6;

  uint8_t parseMsg = 0;

  // if (data == 0xF8)
  // {
  //   return;
  // }

	if(data & 0x80)
  {
  /*  High bit is set -  its either a status or a system message.
      regardless of current state, we blindly start a new message without questioning it */
		midiMsg_tmp.bits.sysxbyte = 0;

		if((data&0xf0) == 0xF0) /* System message */
    {
			midiMsg_tmp.status = data;
			switch(data)
      {
        case MIDI_CLOCK:
          parseMsg = 1;
          break;  
			  case SYSEX_START: /*  Get into sysex receive mode. any more bytes received until this status changes are considered
							                to be sysex data. we still need to parse this sysex start, in case we are routing it */
          parserState = SYSEX_DATA;
          midiMsg_tmp.bits.length = 0;
          parseMsg = 1; /* we will still parse it in case we are doing a passthru */
          break;
        case SYSEX_END:	  /* Get out of sysex mode */
          if(parserState == SYSEX_DATA)
          {
            parserState = MIDI_STATUS;
            midiMsg_tmp.bits.length = 0;
            parseMsg = 1; // We will still parse it in case we are doing a passthru
            break;
          }
          else
          {
            // spurious sysex end msg received. ignore it
          }
          break;
        /* 1 byte payload messages */
        case MIDI_SONG_SEL:		  // passthru only
          parseMsg = 1;
          break; 
        case MIDI_MTC_QFRAME: 	// mtc chunk
          parserState = MIDI_DATA1; 	  // we expect the nugget of mtc frame info
          midiMsg_tmp.bits.length = 1;  // we expect 1 data byte
          break;
        case MIDI_SONG_POSITION_POINTER:
          parserState = MIDI_DATA1; 	// we expect 14-bit value for song position
          midiMsg_tmp.bits.length = 2;// we expect 1 data byte
          break;
        default: /* 0 byte payload messages (we will assume that any system message
                    other than those above has 0 byte payload) */
          // midiMsg_tmp.bits.length = 0;
          parseMsg = 1;
          break;
			}
		}
    else
    { /* Status byte (channel specific message containing a channel number) */
			midiParser_handleStatusByte(data);
		}
	}
  else
  { /* High bit is not set - it's a data byte */
		switch(parserState)
    {
      case MIDI_STATUS: /* We are expecting status msg, but got data, so running status may be in effect */
        midiMsg_tmp.status = midiMsg_tmp.lastNonSysStatus;
        if(midiMsg_tmp.bits.length)
        {
          midiMsg_tmp.data1 = data;
          if(midiMsg_tmp.bits.length == 2)
          {
            parserState = MIDI_DATA2;
          }
          else
          {
            midiMsg_tmp.data2 = 0;
            parseMsg = 1;
            break;
          }
        }
        else
        {
          break; // last msg had 0 payload, so wtf is this? just ignore it
        }
        break;
      case MIDI_DATA1:
        midiMsg_tmp.data1 = data;
        if(midiMsg_tmp.bits.length == 2)
        {
          parserState = MIDI_DATA2; /* We need another byte before we can do anything meaningful */
        }
        else /* It must be 1 */
        {
          parseMsg = 1;
          break;
        }
        break;
      case MIDI_DATA2:
        midiMsg_tmp.data2 = data;
        parseMsg = 1; /* Message complete */
        break;
      case SYSEX_DATA: /* We are in sysex mode */
        midiMsg_tmp.bits.sysxbyte = 1;
        midiMsg_tmp.status = data; // Status will contain the sysex byte
        midiMsg_tmp.bits.length = 0;
        parseMsg = 1;
        break;
      default: /* We are expecting no data byte, but we got one */
			  break;
		}
	}

  if (parseMsg == 1)
  {
    /* We get here if we have proudly received a message that we want to do something with */
    if(parserState != SYSEX_DATA) // we are still in sysex receive mode
      parserState = MIDI_STATUS;  // next byte should be a new message, or we don't care about it
    midiMsg_tmp.bits.source = midiSourceMIDI;
    midiParser_parseMidiMessage(midiMsg_tmp);
    parseMsg = 0;
  }

	return;
}

// 0 - Off - nothing to nothing
// 1 - U2M - usb in to midi out
// 2 - M2M - midi in to midi out
// 3 - A2M - usb in and midi in to midi out
// 4 - M2U - midi in to usb out
// 5 - M2A - midi in to usb out and midi out

void midiParser_setRouting(uint8_t value)
{
	midiParser_routing.value=0;

	switch(value) {
	case 1:
		midiParser_routing.route.usb2midi=1;
		break;
	case 2:
		midiParser_routing.route.midi2midi=1;
		break;
	case 3:
		midiParser_routing.route.usb2midi=1;
		midiParser_routing.route.midi2midi=1;
		break;
	case 4:
		midiParser_routing.route.midi2usb=1;
		break;
	case 5:
		midiParser_routing.route.midi2midi=1;
		midiParser_routing.route.midi2usb=1;
		break;
	default:
	case 0:

		break;
	}

}

void midiParser_setFilter(uint8_t is_tx, uint8_t value)
{

	if(is_tx) // set the high nibble to value
		midiParser_txRxFilter = (value << 4) | (midiParser_txRxFilter & 0x0F);
	else // set the low nibble to value
		midiParser_txRxFilter = (value & 0x0F) | (midiParser_txRxFilter & 0xF0);

}
