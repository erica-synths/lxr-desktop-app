/*
 * sequencer.c
 *
 *  Created on: 11.04.2012
 *
 * Copyright (C) 2012-2026 Erica Synths
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * This file is part of the LXR firmware. It is licensed under the GNU General
 * Public License version 3, with the additional permission and terms in
 * NOTICE.md. See LICENSE and NOTICE.md in the root of the repository.
 */


#include "stm32f7xx_hal.h"
#include "Soundengine/globals.h"
#include "Soundengine/DSPAudio/DrumVoice.h"
#include "Soundengine/DSPAudio/Snare.h"
#include "Soundengine/DSPAudio/HiHat.h"
#include "Soundengine/DSPAudio/random.h"
#include "Soundengine/MIDI/Uart.h"
#include "Soundengine/MIDI/MidiMessages.h"
#include "Soundengine/MIDI/MidiVoiceControl.h"
#include "Soundengine/DSPAudio/CymbalVoice.h"
#include "Soundengine/Sequencer/sequencer.h"
#include <string.h>
#include <Soundengine/MIDI/CortexParser.h>
#include "Soundengine/USB/usb_manager.h"
#include "Soundengine/Sequencer/clockSync.h"
#include "Soundengine/MIDI/MidiParser.h"
#include "Soundengine/DSPAudio/automationNode.h"
//#include "SomGenerator.h"
#include "Soundengine/TriggerOut.h"
#include "Soundengine/FxBus.h"

#include "menu/menu.h"
#include "ledHandler.h"
#include "buttonHandler.h"
#include "Preset/PresetManager.h"

void seq_setPendingPatternActive();



#define SEQ_PRESCALER_MASK 	0x03
#define MIDI_PRESCALER_MASK	0x04
uint8_t seq_prescaleCounter = 0;

uint8_t seq_masterStepCnt=0;				/** keeps track of the played steps between 0 and 127 independent from the track counters*/
uint8_t seq_rollRate = 1;				//start with roll rate = 1/16
uint8_t seq_rollState = 0;					/**< each bit represents a voice. if bit is set, roll is active*/

static int8_t 	seq_stepIndex[NUM_TRACKS];	/**< we have 16 steps con 4 bars = 64 steps.
											     each track has its own counter to allow different pattern lengths */

static uint16_t seq_tempo = 120;			/**< seq speed in bpm*/

static uint32_t	seq_lastTick = 0;			/**< stores the time the last step change occured*/
uint8_t isStepShuffled=0;
static float	seq_deltaT;					/**< time in [ms] until the next step
 	 	 	 	 	 	 	 	 	 	 	 1000ms = 1 sec
 	 	 	 	 	 	 	 	 	 	 	 1 min = 60 sec*/
uint8_t seq_activeAutomTrack=0;

uint8_t seq_delayedSyncStepFlag = 0;		//normally sync steps will only be advanced by external midi clocks in ext. sync mode
											//if the shuffle needs a delayed sync step, it is indicated here.

uint8_t seq_isSyncExternal = 0;
uint8_t seq_lastMasterStep[NUM_TRACKS];		//keeps track of the last triggered master sync step of each track


float seq_shuffle = 0;

int8_t seq_stepToSkip[NUM_TRACKS];



static int8_t seq_armedArmedAutomationStep = -1;
static int8_t seq_armedArmedAutomationTrack = -1;

static uint8_t seq_mutedTracks=0;			/**< indicate which tracks are muted */
uint8_t seq_songModeEnabled=0;
uint8_t seq_running = 0;					/**<  1 running, 0 if stopped*/

uint8_t seq_activePattern = 0;				/**< the currently playing pattern*/
uint8_t seq_pendingPattern = 0;				/**< next pattern to play*/



uint8_t seq_selectedStep = 0;

uint8_t seq_recordActive = 0;				/**< set to 1 to activate the reording mode*/

uint8_t seq_eraseActive=0;					/**RECORD will be 1 if live erasing the active voice  */

uint8_t seq_quantisation = QUANT_16;

/**< random value for probability function*/
struct
{
	uint8_t prev;
	uint8_t current;
	uint8_t next;
}seq_rndValue[NUM_TRACKS];


//uint8_t seq_barCounter;						/**< counts the absolute position in bars since the seq was started */

static uint8_t seq_loadPendigFlag = 0;

// Allow it to be configured whether it keeps track of bar position in the song for
// the purpose of pattern changes
uint8_t seq_resetBarOnPatternChange=0;

// keep track of which midi notes are playing
static uint8_t midi_chan_notes[16];		    /**< what note is playing on each channel */
static uint16_t midi_notes_on=0;		    /**< which channels have a note currently playing */

const float seq_shuffleTable[16] =
{
		0.f,
		0.015625f,
		0.0625f,
		0.140625f,
		0.25f,
		0.390625f,
		0.5625f,
		0.765625f,
		1.f,
		0.984375f,
		0.9375f,
		0.859375f,
		0.75f,
		0.609375f,
		0.4375f,
		0.234375f,
};

float seq_lastShuffle = 0;
#if USE_PATTERN_EDIT_BUFFER
	PatternSet seq_patterns;
#else
	PatternSet seq_patternSet;
#endif

PatternBackupBuffer patternBackupBuffer;

//TempPattern seq_tmpPattern;

//uint8_t seq_newPatternAvailable = 0; 	//indicate that a new pattern has loaded in the background and we should switch
uint8_t seq_songData[NUM_KIT_SLOTS][MAX_SONG_LENGTH];
uint8_t *seq_activeSong;
uint8_t seq_songPositionPointer = 0;	//for song playback, current song position
//uint8_t seq_bankPreloadPendingFlag = NO_PRELOAD_PENDING;

//for the automation tracks each track needs 2 modNodes
static AutomationNode seq_automationNodes[NUM_TRACKS][2];

static void seq_sendMidi(MidiMsg msg);
static void seq_sendRealtime(const uint8_t status);
static void seq_sendProgChg(const uint8_t ptn);
//static void seq_eraseStepAndSubSteps(const uint8_t voice, const uint8_t mainStep);
//static void seq_activateTmpPattern();



//static uint8_t seq_isNextStepSyncStep();
static uint8_t seq_intIsStepActive(uint8_t voice, uint8_t stepNr, uint8_t patternNr);
static uint8_t seq_intIsMainStepActive(uint8_t voice, uint8_t mainStepNr, uint8_t pattern);
static void seq_resetNote(Step *step);
static void seq_setStepIndexToStart();


uint8_t lastWasNoteOn[NUM_TRACKS];
//------------------------------------------------------------------------------
uint8_t seq_getMasterStepCnt()
{
	return seq_masterStepCnt;
}
//------------------------------------------------------------------------------
void seq_init()
{
	int i;

	//give active song pointer a proper value
	seq_activeSong = seq_songData[0];

	memset(seq_songData, SONG_END, sizeof(uint8_t) * MAX_SONG_LENGTH * NUM_KIT_SLOTS);
	seq_songData[0][0] = 0;

	for(i=0;i<NUM_TRACKS;i++) {
		autoNode_init(&seq_automationNodes[i][0]);
		autoNode_init(&seq_automationNodes[i][1]);

		seq_stepToSkip[i] = -1;
	}

	memset(seq_stepIndex,0,NUM_TRACKS);
	memset(seq_lastMasterStep,0,NUM_TRACKS);


	for(i=0;i<NUM_PATTERN;i++)
	{
		//seq_patternSet.seq_patternSettings[i].changeBar 	= 0;	//default setting: zero repeats (play once then change)
		//seq_patternSet.seq_patternSettings[i].nextPattern 	= i;	//default setting: repeat same pattern
		seq_clearPattern(i); // will clear all tracks in the pattern
	}

}
//------------------------------------------------------------------------------
/*
static void seq_activateTmpPattern()
{
	memcpy(&seq_patternSet.seq_subStepPattern[seq_activePattern],&seq_tmpPattern.seq_subStepPattern,sizeof(Step)*NUM_TRACKS*NUM_STEPS);
	for(uint8_t i=0; i<NUM_BARS; i++)
	{
		memcpy(&seq_patternSet.seq_mainSteps[i][seq_activePattern],&seq_tmpPattern.seq_mainSteps[i],sizeof(uint16_t)*NUM_TRACKS);
	}
	//memcpy(&seq_patternSet.seq_patternSettings[seq_activePattern],&seq_tmpPattern.seq_patternSettings,sizeof(PatternSetting));
	memcpy(&seq_patternSet.length[seq_activePattern],&seq_tmpPattern.length,sizeof(uint8_t)*NUM_TRACKS);
}
*/
//------------------------------------------------------------------------------

void seq_copyPatternToBackupBuffer(uint8_t patternNr)
{
	memcpy(&patternBackupBuffer.seq_subStepPattern, &seq_patternSet.seq_subStepPattern[patternNr],  sizeof(Step)*NUM_TRACKS*NUM_STEPS);
	memcpy(&patternBackupBuffer.seq_stepTiming, &seq_patternSet.seq_stepTiming[patternNr],  sizeof(uint8_t)*NUM_TRACKS*NUM_STEPS);
	for(uint8_t i=0; i<NUM_BARS; i++)
	{
		memcpy(&patternBackupBuffer.seq_mainSteps[i], &seq_patternSet.seq_mainSteps[i][patternNr],  sizeof(uint16_t)*NUM_TRACKS);
	}
	memcpy(&patternBackupBuffer.length, &seq_patternSet.length[patternNr],  sizeof(uint8_t)*NUM_TRACKS);



	patternBackupBuffer.shuffle = seq_patternSet.shuffle[patternNr];;
	patternBackupBuffer.kitNr = seq_patternSet.kitNr[patternNr];

}
//------------------------------------------------------------------------------
void seq_copyBackupBufferToPattern(uint8_t patternNr)
{
	memcpy(&seq_patternSet.seq_subStepPattern[patternNr], &patternBackupBuffer.seq_subStepPattern,  sizeof(Step)*NUM_TRACKS*NUM_STEPS);
	memcpy(&seq_patternSet.seq_stepTiming[patternNr]    , &patternBackupBuffer.seq_stepTiming,      sizeof(uint8_t)*NUM_TRACKS*NUM_STEPS);
	for(uint8_t i=0; i<NUM_BARS; i++)
	{
		memcpy(&seq_patternSet.seq_mainSteps[i][patternNr], &patternBackupBuffer.seq_mainSteps[i],  sizeof(uint16_t)*NUM_TRACKS);
	}
	memcpy(&seq_patternSet.length[patternNr], &patternBackupBuffer.length,  sizeof(uint8_t)*NUM_TRACKS);

	seq_patternSet.shuffle[patternNr] = patternBackupBuffer.shuffle;
	seq_patternSet.kitNr[patternNr] = patternBackupBuffer.kitNr;
}
/*
//------------------------------------------------------------------------------
void seq_copyEditBufferToRam(uint8_t patternNr)
{
	memcpy(&seq_patternSet.seq_subStepPattern[patternNr],&seq_tmpPattern.seq_subStepPattern, sizeof(Step)*NUM_TRACKS*NUM_STEPS);
	for(uint8_t i=0; i<NUM_BARS; i++)
	{
		memcpy(&seq_patternSet.seq_mainSteps[i][patternNr],&seq_tmpPattern.seq_mainSteps[i],sizeof(uint16_t)*NUM_TRACKS);
	}
	//memcpy(&seq_patternSet.seq_patternSettings[seq_activePattern],&seq_tmpPattern.seq_patternSettings,sizeof(PatternSetting));
	memcpy(&seq_patternSet.length[patternNr],&seq_tmpPattern.length,sizeof(uint8_t)*NUM_TRACKS);
}
*/
//------------------------------------------------------------------------------
void seq_setShuffle(float shuffle)
{
	seq_shuffle = shuffle;
}
//------------------------------------------------------------------------------
void seq_setTrackLength(uint8_t trackNr, uint8_t length)
{
	//set new end marker
	/*
	if(length == 16)
		length=0;
	*/
	// **PATROT this was changed from setting length on seq_activePattern to shown (or edited) pattern

#if USE_PATTERN_EDIT_BUFFER

	patternBackupBuffer.length[trackNr]=length;

#else
	seq_patternSet.length[frontParser_shownPattern][trackNr]=length;
#endif
}

//------------------------------------------------------------------------------
uint8_t seq_getTrackLength(uint8_t trackNr)
{
	// **PATROT this was changed from getting seq_activePattern to shown (or edited) pattern
#if USE_PATTERN_EDIT_BUFFER
	uint8_t r=patternBackupBuffer.length[trackNr];
#else
	uint8_t r=seq_patternSet.length[frontParser_shownPattern][trackNr];
#endif
	/*
	if(r==0)
		return 16;
	*/
	return r;
}
//------------------------------------------------------------------------------
// **PATROT
//TODO check if rotation is working with new 4 bar layout
/*
void seq_setTrackRotation(uint8_t trackNr, const uint8_t newRot)
{
	// frontParser_shownPattern contains the pattern that is shown (being edited) on the front at the time this is called
	// seq_activePattern is the pattern that is now playing
	LengthRotate *lr=&seq_patternSet.seq_patternLengthRotate[frontParser_shownPattern][trackNr];

	if(newRot == lr->rotate)
		return;

	// if sequencer is running, move the current step position to compensate for the rotation
	if(seq_running) {
		int8_t len = lr->length;
		if(len==0)
			len=16;

		// this is how many main steps we need to move in one direction, negative means
		// move back positive is move forward
		int8_t offset=(((int8_t)newRot) % len) - (((int8_t)lr->rotate) % len);

        // if adding the offset to si would push it over the edge either way we need to wrap it
		// offset will always be less than len
        int16_t si = seq_stepIndex[trackNr] + (offset*8);
        if(si < 0 )
            si += (len*8);
        else if(si >= (len*8))
            si -= (len*8);
        seq_stepIndex[trackNr]=(int8_t)si;
	}

	//set new rotation value
	lr->rotate=newRot;

}
*/
//------------------------------------------------------------------------------
// **PATROT
/*
uint8_t seq_getTrackRotation(uint8_t trackNr)
{
	return seq_patternSet.seq_patternLengthRotate[frontParser_shownPattern][trackNr].rotate;
}
*/
//------------------------------------------------------------------------------
float seq_getDeltaTShuffleDelay(uint16_t bpm)
{
	if(bpm==0) return 1;

	float dT;
	/* without substeps notes
	 *
	 * internal clock is 96 PPQ
	 * wit 16 steps per beat
	 * we have 4 ppq (16/4) so we get 96ppq/4ppq= prescaler 24
	 *
	 *
	 * stuff below were substep calculations
	 */
	//--- calc deltaT ----
	// for 4/4tel takt -> 1 beat = 4 main steps = 4*8 = 32 sub steps
	// 120 bpm 4/4tel = 120 * 1 beat / 60sec = 120 * 32 in 60 sec;
	dT 	= (1000*60)/bpm; 	//bei 12 = 500ms = time for one beat
	dT /= 96.f; //we run the internal clock at 96ppq -> seq clock 32 ppq == prescaler 3, midi clock 24 ppq == prescale 4
	dT *= 4;
	//32.f;					// time for one beat / number of steps per beat



	const float shuffleFactor = seq_shuffle;
	const float originalDeltaT = dT;

	return shuffleFactor * originalDeltaT * 16.f;


}
//------------------------------------------------------------------------------
static void seq_calcDeltaT(uint16_t bpm, uint8_t isShuffled)
{

	/* without substeps notes
	 *
	 * internal clock is 96 PPQ
	 * wit 16 steps per beat
	 * we have 4 ppq (16/4) so we get 96ppq/4ppq= prescaler 24
	 *
	 *
	 * stuff below were substep calculations
	 */
	//--- calc deltaT ----
	// for 4/4tel takt -> 1 beat = 4 main steps = 4*8 = 32 sub steps
	// 120 bpm 4/4tel = 120 * 1 beat / 60sec = 120 * 32 in 60 sec;
	seq_deltaT 	= (1000*60)/bpm; 	//bei 12 = 500ms = time for one beat
	seq_deltaT /= 96.f; //we run the internal clock at 96ppq -> seq clock 32 ppq == prescaler 3, midi clock 24 ppq == prescale 4
	seq_deltaT *= 4;
	//32.f;					// time for one beat / number of steps per beat

	//--- calc shuffle ---
	if(seq_shuffle != 0)
	{
#if 0
		//every 2nd and 4th 16th note in a beat is shifted full
		//=> step 8 and step 24
		//every 2nd 16th note in half a beat
		//every beat has 32 steps => half = 16
		uint8_t stepInHalfBeat = seq_masterStepCnt&0xf;
		const float shuffleFactor = seq_shuffleTable[stepInHalfBeat] * seq_shuffle;
		const float originalDeltaT = seq_deltaT;

		seq_deltaT += shuffleFactor * originalDeltaT * 16.f;
		seq_deltaT -= seq_lastShuffle * originalDeltaT * 16.f;

		seq_lastShuffle = shuffleFactor;
#endif


		uint8_t isStepShuffled = isShuffled;//seq_masterStepCnt%2 == 1;
		const float shuffleFactor = isStepShuffled * seq_shuffle;
		const float originalDeltaT = seq_deltaT;

		seq_deltaT += shuffleFactor * originalDeltaT * 16.f;
		seq_deltaT -= seq_lastShuffle * originalDeltaT * 16.f;

		seq_lastShuffle = shuffleFactor;

	}
}
//------------------------------------------------------------------------------
void seq_setBpm(uint16_t bpm)
{
	seq_tempo 	= bpm;
	//seq_calcDeltaT(bpm);
	lfo_recalcSync();
}
//------------------------------------------------------------------------------
uint16_t seq_getBpm()
{
	return seq_tempo;
}
//------------------------------------------------------------------------------
void seq_sync()
{
  if (seq_getExtSync() == 2)
  {
    sync_tick();
  }
}
//------------------------------------------------------------------------------
void seq_setNextPattern(const uint8_t patNr)
{
	if(patNr >= MAX_PATTERN) return;
	seq_pendingPattern = patNr;
	seq_loadPendigFlag = 1;

	if(!seq_running)
	{
		//immediately change pattern
		seq_setPendingPatternActive();
	}
}
//------------------------------------------------------------------------------
static void seq_sendMidi(MidiMsg msg)
{
	//send to usb midi
	usb_sendMidi(msg);

	//send to hardware midi out jack
	uart_sendMidi(msg);

}


//------------------------------------------------------------------------------
static void seq_parseAutomationNodes(uint8_t track, Step* stepData)
{
	//set new destination
	autoNode_setDestination(&seq_automationNodes[track][0], stepData->param1Nr);
	autoNode_setDestination(&seq_automationNodes[track][1], stepData->param2Nr);
	//set new mod value
	autoNode_updateValue(&seq_automationNodes[track][0], stepData->param1Val);
	autoNode_updateValue(&seq_automationNodes[track][1], stepData->param2Val);
}
//------------------------------------------------------------------------------
void seq_triggerVoice(uint8_t voiceNr, uint8_t vol, uint8_t note)
{
	uint8_t midiChan; // which midi channel to send a note on
	uint8_t midiNote; // which midi note to send

	if(voiceNr > 6) return;


#if USE_PATTERN_EDIT_BUFFER
	seq_parseAutomationNodes(voiceNr, &patternBackupBuffer.seq_subStepPattern[voiceNr][seq_stepIndex[voiceNr]]);
#else
	seq_parseAutomationNodes(voiceNr, &seq_patternSet.seq_subStepPattern[seq_activePattern][voiceNr][seq_stepIndex[voiceNr]]);
#endif

	//turn the trigger off before sending the next one
	/*
	if(voiceNr>=5)
	{
		//hihat channels choke each other
		trigger_triggerVoice(5, TRIGGER_OFF);
		trigger_triggerVoice(6, TRIGGER_OFF);
	} else {
		trigger_triggerVoice(voiceNr, TRIGGER_OFF);
	}
	*/

	// if a note is on for that channel send note-off first
	voiceControl_noteOff(voiceNr);

	//Trigger internal synth voice
	voiceControl_noteOn(voiceNr, note, vol, 0);

	midiChan = midi_MidiChannels[voiceNr];

	// the note that is played will be whatever is received unless we have a note override set
	// A note override is any non-zero value for this parameter
	//if(midi_NoteOverride[voiceNr] == 0)
	if(activeParameters[PAR_MIDI_NOTE1 + voiceNr] == 0)
		midiNote = note;
	else
		//midiNote = midi_NoteOverride[voiceNr];
		midiNote = activeParameters[PAR_MIDI_NOTE1 + voiceNr];

	//send the new note to midi/usb out
#if USE_PATTERN_EDIT_BUFFER
	seq_sendMidiNoteOn(midiChan, midiNote, patternBackupBuffer.seq_subStepPattern[voiceNr][seq_stepIndex[voiceNr]].volume&STEP_VOLUME_MASK);
#else
	seq_sendMidiNoteOn(midiChan, midiNote, seq_patternSet.seq_subStepPattern[seq_activePattern][voiceNr][seq_stepIndex[voiceNr]].volume&STEP_VOLUME_MASK);
#endif
}
//------------------------------------------------------------------------------
//has to be called AFTER the last pattern switch is finished, so the playing pattern is properly put into the temp buffer
#if 0
static void seq_CheckIfNextPatternNeedsBankSwitch()
{
	if(seq_songPositionPointer+1 < MAX_SONG_LENGTH)
	{
		//determine next bank to load
		const uint8_t nextPattern = seq_activeSong[seq_songPositionPointer+1];
		if(nextPattern==SONG_END || nextPattern == SONG_LOOP) return;
		const uint8_t nextBank = nextPattern/16;

		/*
		const uint8_t lowestPatternInMemory = menu_activePatternBank * 16;
		const uint8_t highestPatternInMemory = lowestPatternInMemory + 15;

		if( (nextPattern > lowestPatternInMemory) && (nextPattern  < highestPatternInMemory) )
		*/
		menu_activePatternBank = nextBank ;
	}
}
#endif
//------------------------------------------------------------------------------
static uint8_t seq_determineNextPattern(uint8_t autoIncrement)
{

	/*
	  extern uint8_t seq_songData[MAX_SONG_LENGTH];
	  extern uint8_t seq_songPositionPointer;
	 */
	if(seq_songModeEnabled)
	{
		if(autoIncrement)
		{
			seq_songPositionPointer++;
			//TODO: this will loop when all 64 pattern slots are used... maybe a stop would be better?
			if(seq_songPositionPointer == MAX_SONG_LENGTH) seq_songPositionPointer = 0;
			return seq_activeSong[seq_songPositionPointer];
		}
		else
		{
			uint8_t pos = seq_songPositionPointer +1;
			if(pos == MAX_SONG_LENGTH) pos = 0;
			return seq_activeSong[pos];
		}
	}
	else
		return seq_activePattern;

	/*
	const PatternSetting * const p=&seq_patternSet.seq_patternSettings[seq_activePattern];
	if(seq_barCounter % (p->changeBar+1) == 0)
		return p->nextPattern;
	else
		*/

}

#include "Soundengine/MIDI/Uart.h"

IN_ITCM_RAM void seq_setPendingPatternActive()
{
  midi_process_block = 1;
	if((seq_activePattern != seq_pendingPattern) || seq_loadPendigFlag)
	{
    seq_setStepIndexToStart();
		// if this setting is active and the user has manually changed patterns,
		// reset the bar counter. uncommenting the below will cause it to only reset
		// when a manual pattern change is invoked. to me the whole auto pattern change modulo stuff
		// above is a bit broken.

		/*
		if( seq_resetBarOnPatternChange)
			seq_barCounter=0;
			*/

		// TODO we need to also reset barCounter to 0 when the end of a repetition set of a pattern plays
		// EVEN IF the pattern is set to play itself again, this will facilitate having the bits that play
		// certain steps only on certain intervals of bar counter

		seq_loadPendigFlag = 0;

		/*//background loading removed
		//first check if 2 new pattern is available
		if(seq_newPatternAvailable)
		{
			seq_newPatternAvailable = 0;
			seq_activateTmpPattern();
		}
		*/
		//copy current backup to current slot (discarding unsaved changes)
		seq_copyBackupBufferToPattern(seq_activePattern);
		//copy pending pattern to backup buffer
		seq_copyPatternToBackupBuffer(seq_pendingPattern);

		seq_activePattern = seq_pendingPattern;

		activeParameters[PAR_SHUFFLE] = seq_patternSet.shuffle[seq_activePattern];
		seq_setShuffle(activeParameters[PAR_SHUFFLE]/127.f);

		//A new pattern is activated here -> Load corresponding drumkit
		if(activeParameters[PAR_PATTERN_MODE] == SKIP_NEXT_KIT_AUTOLOAD)
		{
			activeParameters[PAR_PATTERN_MODE] = 1;
		}
		else if(activeParameters[PAR_PATTERN_MODE] == 1)
		{
			//turn FX off to save cycles
			fxBus.fxType = FX_OFF;
			preset_loadDrumsetRam(seq_patternSet.kitNr[seq_activePattern], 0, 1);
			menu_selectedPresetNr[SAVE_TYPE_KIT] = seq_patternSet.kitNr[seq_activePattern];
			menu_loadedPresetNr[CURRENT_KIT] = seq_patternSet.kitNr[seq_activePattern];
			preset_touchTimer(PRESET_KIT_TIMER);
		}

		//use shuffle value from pattern
		//activeParameters[PAR_SHUFFLE] = seq_patternSet.shuffle[seq_activePattern];
		//seq_setShuffle(activeParameters[PAR_SHUFFLE]/127.f);

		//reset pattern position to pattern rotate starting position for the active pattern **PATROT
		// seq_setStepIndexToStart();

		//send the ack message to tell the front that a new pattern starts playing
		/*
		uart_sendFrontpanelByte(FRONT_SEQ_CC);
		uart_sendFrontpanelByte(FRONT_SEQ_CHANGE_PAT);
		uart_sendFrontpanelByte(seq_activePattern);
		*/

		//if(frontParser_midiMsg.data2 > 7) return;
		//ack message that the sequencer changed to the requested pattern

		//stop blinking pattern led
		led_setBlinkLed((uint8_t)(led_idx2StepLed(seq_activePattern)),0);
		//clear last pattern led



		menu_playedPattern = seq_activePattern;

		/*
		//step display follows pattern changes
		//if(parameter_values[PAR_FOLLOW] & FOLLOW_PATTERN ) {
		//Playing pattern is always the shown pattern. Makes more sense!
		if(1) {

			//update for song screen to follow patterns
			menu_songMenuPosition = seq_songPositionPointer;

			if( menu_activePage < PATTERN_LOAD_PAGE || menu_activePage > SONG_MODE_PAGE)
			{

				menu_setShownPattern(seq_activePattern);
				led_clearSequencerLeds();
				//query current sequencer step states and light up the corresponding leds
				const uint8_t trackNr = menu_getActiveVoice(); //max 6 => 0x6 = 0b110
				//uint8_t patternNr = menu_getViewedPattern(); //max 7 => 0x07 = 0b111
				//uint8_t value = (uint8_t)((trackNr<<4) | (patternNr&0x7));


				//frontPanel_sendData(LED_CC,LED_QUERY_SEQ_TRACK,value); muss gemacht werden
				//frontParser_updateTrackLeds(trackNr, patternNr);
				led_updateSeqLeds();
				activeParameters[PAR_TRACK_LENGTH] = seq_getTrackLength(trackNr);
				//parameter_values[PAR_TRACK_ROTATION] = seq_getTrackRotation(trackNr);

				//frontPanel_sendData(SEQ_CC,SEQ_REQUEST_PATTERN_PARAMS,frontParser_midiMsg.data2); muss auch gem,acht werdne!
				//parameter_values[PAR_PATTERN_BEAT] = seq_patternSet.seq_patternSettings[frontParser_shownPattern].changeBar;
				//parameter_values[PAR_PATTERN_NEXT] = seq_patternSet.seq_patternSettings[frontParser_shownPattern].nextPattern;
				menu_repaint();


			} else {
				//store the pending pattern update for shift button release handler
				menu_shownPattern = seq_activePattern;


			}
		}




		//if( (buttonHandler_getMode() == SELECT_MODE_PERF) || (buttonHandler_getMode() == SELECT_MODE_PAT_GEN) )
		if( (buttonHandler_getMode() == SELECT_MODE_PATTERN))
		{
			//only show pattern changes when in select pattern  mode

			led_clearSequencerLeds();
			//led_clearSelectLeds();
			led_clearAllBlinkLeds();
			// re init the LEDs showing active/viewed pattern
			led_initPatternSelectLeds();
			//led_setValue(1,LED_PART_SELECT1+frontParser_midiMsg.data2);
			menu_repaint();

		}
		else if(( (buttonHandler_getMode() == SELECT_MODE_SONG)))
		{
			//update song mode screen
			menu_repaintAll();
		}
		*/


		// send a pattern change message to midi/usb out
		seq_sendProgChg(seq_activePattern);


		// all notes off here since we are switching patterns
		voiceControl_noteOff(0xFF);

		//song mode - check for preload pattern bank
		/*
		if(seq_songModeEnabled)
		{
			seq_CheckIfNextPatternNeedsBankSwitch();
		}
		*/

	}
	else if(activeParameters[PAR_PATTERN_MODE] == SKIP_NEXT_KIT_AUTOLOAD)
	{
		activeParameters[PAR_PATTERN_MODE] = 1;
	}
	//moved song list display here to prevent skipping list on same consecutive patterns
	if(1) {

				//update for song screen to follow patterns
				menu_songMenuPosition = seq_songPositionPointer;

				if( menu_activePage < PATTERN_LOAD_PAGE || menu_activePage > SONG_MODE_PAGE)
				{

					menu_setShownPattern(seq_activePattern);
					led_clearSequencerLeds();
					//query current sequencer step states and light up the corresponding leds
					const uint8_t trackNr = menu_getActiveVoice(); //max 6 => 0x6 = 0b110
					//uint8_t patternNr = menu_getViewedPattern(); //max 7 => 0x07 = 0b111
					//uint8_t value = (uint8_t)((trackNr<<4) | (patternNr&0x7));


					//frontPanel_sendData(LED_CC,LED_QUERY_SEQ_TRACK,value); muss gemacht werden
					//frontParser_updateTrackLeds(trackNr, patternNr);
					led_updateSeqLeds();
					activeParameters[PAR_TRACK_LENGTH] = seq_getTrackLength(trackNr);
					//parameter_values[PAR_TRACK_ROTATION] = seq_getTrackRotation(trackNr);

					//frontPanel_sendData(SEQ_CC,SEQ_REQUEST_PATTERN_PARAMS,frontParser_midiMsg.data2); muss auch gem,acht werdne!
					//parameter_values[PAR_PATTERN_BEAT] = seq_patternSet.seq_patternSettings[frontParser_shownPattern].changeBar;
					//parameter_values[PAR_PATTERN_NEXT] = seq_patternSet.seq_patternSettings[frontParser_shownPattern].nextPattern;
					menu_repaint();


				} else {
					//store the pending pattern update for shift button release handler
					menu_shownPattern = seq_activePattern;


				}
			}




			//if( (buttonHandler_getMode() == SELECT_MODE_PERF) || (buttonHandler_getMode() == SELECT_MODE_PAT_GEN) )
			if( (buttonHandler_getMode() == SELECT_MODE_PATTERN))
			{
				//only show pattern changes when in select pattern  mode

				led_clearSequencerLeds();
				//led_clearSelectLeds();
				led_clearAllBlinkLeds();
				// re init the LEDs showing active/viewed pattern
				led_initPatternSelectLeds();
				//led_setValue(1,LED_PART_SELECT1+frontParser_midiMsg.data2);
				menu_repaint();

			}
			else if(( (buttonHandler_getMode() == SELECT_MODE_SONG)))
			{
				//update song mode screen
				menu_repaintAll();
			}

  midi_process_block = 0;
}


uint8_t seq_getLongestTrack()
{
	uint8_t trackId = 0;

	for(int i=1;i<NUM_TRACKS;i++)
	{
		if(seq_patternSet.length[seq_activePattern][i] > seq_patternSet.length[seq_activePattern][trackId])
		{
			trackId = i;
		}
	}
	return trackId;
}

int8_t getShiftValue(uint8_t in)
{
	switch(in)
	{
	case 0:
		return -7;
		break;

	case 1:
		return -6;
		break;

	case 2:
		return -5;
		break;

	case 3:
		return -4;
		break;

	case 4:
		return -3;
		break;

	case 5:
		return -2;
		break;

	case 6:
		return -1;
		break;

	default:
	case 7:
		return 0;
		break;

	case 8:
		return 1;
		break;

	case 9:
		return 2;
		break;

	case 10:
		return 3;
		break;

	case 11:
		return 4;
		break;

	case 12:
		return 5;
		break;

	case 13:
		return 6;
		break;

	case 14:
		return 7;
		break;
	}
}
uint32_t flamIdxToPattern(uint8_t flamIdx)
{
	switch(flamIdx)
	{
	default:
	case 0: //off
		return 0b100000000000000000000000;
		break;

	case 1: //x2
		return 0b100000000000100000000000;
		break;

	case 2: //x3
		return 0b100000001000000010000000;
		break;

	case 3: //x4
		return 0b100000100000100000100000;
		break;

	case 4: //x6
		return 0b100010001000100010001000;
		break;

	case 5: //x8
		return 0b100100100100100100100100;
		break;
	}
}
//return pattern as 8 bit binary value (steps 0-7)
uint32_t seq_getShiftFlamPattern(const uint8_t track, uint8_t step, StepPosition stepPos)
{
#if 0
	const uint8_t length = seq_patternSet.length[seq_activePattern][track];
	const uint8_t nextStep = (step+1)%length;
	const uint8_t prevStep = step != 0 ? step - 1 : length-1;

	/* we have 3 possible step sources:
	 * - flammed AND positive shifted notes from the previous step
	 * - current step data
	 * - negative shifted data from the next step
	 */

	uint8_t prevStepActive = seq_intIsMainStepActive(track,prevStep,seq_activePattern);
	uint8_t currentStepActive = seq_intIsMainStepActive(track,step,seq_activePattern);
	uint8_t nextStepActive = seq_intIsMainStepActive(track,nextStep,seq_activePattern);


	const uint8_t flamCurrentStep = seq_patternSet.seq_stepTiming[seq_activePattern][track][step].flam;
	const uint8_t flamNextStep = seq_patternSet.seq_stepTiming[seq_activePattern][track][nextStep].flam;
	const uint8_t flamPrevStep = seq_patternSet.seq_stepTiming[seq_activePattern][track][prevStep].flam;
	const uint8_t shiftCurrentStep = seq_patternSet.seq_stepTiming[seq_activePattern][track][step].shift;
	const uint8_t shiftNextStep = seq_patternSet.seq_stepTiming[seq_activePattern][track][nextStep].shift;
	const uint8_t shiftPrevStep = seq_patternSet.seq_stepTiming[seq_activePattern][track][prevStep].shift;

	//calc flam step pattern
	StepPattern prev, current, next;

	//check probability
	if( (seq_rndValue[track].prev) > seq_patternSet.seq_subStepPattern[seq_activePattern][track][prevStep].prob ) prevStepActive = 0;
	if( (seq_rndValue[track].current) > seq_patternSet.seq_subStepPattern[seq_activePattern][track][step].prob ) currentStepActive = 0;
	if( (seq_rndValue[track].next) > seq_patternSet.seq_subStepPattern[seq_activePattern][track][nextStep].prob ) nextStepActive = 0;

	//add step pattern for active steps
	prev.StepData.prevStep = prevStepActive ? flamIdxToPattern(flamPrevStep) : 0;
	current.StepData.prevStep = currentStepActive ? flamIdxToPattern(flamCurrentStep) : 0;
	next.StepData.prevStep = nextStepActive ? flamIdxToPattern(flamNextStep) : 0;

	//add shift
	prev.completeData = shiftPrevStep >= 0 ? prev.completeData >> shiftPrevStep : prev.completeData << shiftPrevStep;
	current.completeData = shiftCurrentStep >= 0 ? current.completeData >> shiftCurrentStep : current.completeData << shiftCurrentStep;
	next.completeData = shiftNextStep >= 0 ? next.completeData >> shiftNextStep : next.completeData << shiftNextStep;

	//combine
	uint8_t pattern =  prev.StepData.currentStep | current.StepData.currentStep | next.StepData.currentStep;

	return pattern;
#else
		uint8_t currentStepActive = seq_intIsMainStepActive(track,step,seq_activePattern);
		if(!currentStepActive) return 0;


		const  uint8_t flamCurrentStep = seq_patternSet.seq_stepTiming[seq_activePattern][track][step].flam;
		int8_t shiftCurrentStep = getShiftValue(seq_patternSet.seq_stepTiming[seq_activePattern][track][step].shift);

		uint32_t stepPattern;

		//check probability
		switch(stepPos)
		{
		case STEP_PREV:
			if( (seq_rndValue[track].prev) > seq_patternSet.seq_subStepPattern[seq_activePattern][track][step].prob ) currentStepActive = 0;
			break;

		default:
		case STEP_CURRENT:
			if( (seq_rndValue[track].current) > seq_patternSet.seq_subStepPattern[seq_activePattern][track][step].prob ) currentStepActive = 0;
			break;

		case STEP_NEXT:
			if( (seq_rndValue[track].next) > seq_patternSet.seq_subStepPattern[seq_activePattern][track][step].prob ) currentStepActive = 0;
			break;
		}
		if(!currentStepActive) return 0;

		//add step pattern for active steps
		stepPattern = flamIdxToPattern(flamCurrentStep);

		//we have 24 pulses per step (96ppq) but only 8 possible shift values (0-7)
		shiftCurrentStep *= 3;

		//since we use this function to determine what to play at the current step, we have to look at different shifted sections depending of which step we calculate
		//i.e. we only play the data from the previous step, that was shifted into the current(next) step
		if(shiftCurrentStep == 0)
		{
			switch(stepPos)
			{
				case STEP_PREV:
					return 0;	//nothing from prev step is shifted forward to this one
					break;

				default:
				case STEP_CURRENT:
					return (stepPattern) & 0xFFFFFF;
					break;

				case STEP_NEXT:
					return 0;	//nothing from next step is shifted backwards to this one
					break;
			}
		}
		else if(shiftCurrentStep > 0)
		{
			switch(stepPos)
			{
				case STEP_PREV:
					//data from prev step is shifted forward to this one
					return (stepPattern << (24-shiftCurrentStep)) & 0xFFFFFF;
					break;

				default:
				case STEP_CURRENT:
					//data is shifted forward, but a rest remains in this step
					return (stepPattern >> shiftCurrentStep) & 0xFFFFFF;
					break;

				case STEP_NEXT:
					return 0;	//nothing from next step is shifted backwards to this one
					break;
			}
		}
		else
		{
			//make shift value positive
			shiftCurrentStep *= -1;
			switch(stepPos)
			{
				case STEP_PREV:
					//no data from prev step is shifted forward to this one
					return 0;
					break;

				default:
				case STEP_CURRENT:
					//data is shifted backwards, but a rest remains in this step
					return (stepPattern << shiftCurrentStep) & 0xFFFFFF;
					break;

				case STEP_NEXT:
					//data from next step is shifted backwards to this one
					return (stepPattern >> (24-shiftCurrentStep)) & 0xFFFFFF;
					break;
			}
		}
#endif
}

uint8_t chokeCurrent=0;
uint8_t chokeNext=0;
void seq_processFlam(uint8_t idx)
{
	if(!seq_running)
		return;

	if(idx==0)
	{
		chokeCurrent=0;
		chokeNext=0;
	}

	for(uint8_t track=0;track<NUM_TRACKS;track++)
	{
		//if track is not muted
		if(!(seq_mutedTracks & (1<<track) ) )
		{
			//pattern takes main step active and probability into account
#if 1
			const uint8_t length = seq_patternSet.length[seq_activePattern][track];
			const uint8_t currentStep = seq_stepIndex[track];
			const uint8_t nextStep = (seq_stepIndex[track]+1)%length;
			const uint8_t prevStep = seq_stepIndex[track] != 0 ? seq_stepIndex[track] - 1 : length-1;

			/* we have 3 possible step sources:
			 * - flammed AND positive shifted notes from the previous step
			 * - current step data
			 * - negative shifted data from the next step
			 */
			uint32_t currentStepPattern = seq_getShiftFlamPattern(track, currentStep, STEP_CURRENT);
			uint32_t nextStepPattern 	= seq_getShiftFlamPattern(track, nextStep, STEP_NEXT);
			uint32_t prevStepPattern 	= seq_getShiftFlamPattern(track, prevStep, STEP_PREV);

			//calc priotity (next > current > prev)
			uint8_t prevActive 		= (prevStepPattern 		& (1UL<<(23-idx)) ) != 0;
			uint8_t currentActive 	= (currentStepPattern 	& (1UL<<(23-idx)) ) != 0;
			uint8_t nextActive 		= (nextStepPattern 		& (1UL<<(23-idx)) ) != 0;

			if(nextActive)
			{
				lastWasNoteOn[track] = 1;
				chokeNext = 1;
				const uint8_t vol = seq_patternSet.seq_subStepPattern[seq_activePattern][track][nextStep].volume&STEP_VOLUME_MASK;
				const uint8_t note = seq_patternSet.seq_subStepPattern[seq_activePattern][track][nextStep].note;
				seq_triggerVoice(track,vol,note);
			}
			else if((currentActive) && (chokeNext == 0))
			{
				lastWasNoteOn[track] = 1;
				chokeCurrent = 1;
				const uint8_t vol = seq_patternSet.seq_subStepPattern[seq_activePattern][track][currentStep].volume&STEP_VOLUME_MASK;
				const uint8_t note = seq_patternSet.seq_subStepPattern[seq_activePattern][track][currentStep].note;
				seq_triggerVoice(track,vol,note);
			}
			else if((prevActive) && (chokeNext == 0) && (chokeCurrent == 0))
			{
				lastWasNoteOn[track] = 1;
				const uint8_t vol = seq_patternSet.seq_subStepPattern[seq_activePattern][track][prevStep].volume&STEP_VOLUME_MASK;
				const uint8_t note = seq_patternSet.seq_subStepPattern[seq_activePattern][track][prevStep].note;
				seq_triggerVoice(track,vol,note);
			}
			else if (lastWasNoteOn[track])
			{
				lastWasNoteOn[track] = 0;
				voiceControl_noteOff(track);
			}
#else

			//if main step is active
			if(seq_intIsMainStepActive(track,seq_stepIndex[track],seq_activePattern))
			{


				if( (seq_rndValue[track]) <= seq_patternSet.seq_subStepPattern[seq_activePattern][track][seq_stepIndex[track]].prob )
				{
					/* to be able to process flam and shift simultaneously
					   we have to look at the current and the next step
					   as a negative shifted step will be played at the previous step
					   (including a wrap around at the track length end)
					   */



					uint8_t flamCurrentStep = seq_patternSet.seq_stepTiming[seq_activePattern][track][seq_stepIndex[track]]+1;
					uint8_t length = patternSet->length[seq_activePattern][track];
					uint8_t flamNextStep = seq_patternSet.seq_stepTiming[seq_activePattern][track][(seq_stepIndex[track]+1)%length]+1;
					uint8_t shift = 0;
					switch(flamCurrentStep)
					{
					default:
						break;

					case 5:
						flamCurrentStep = 6;
						break;

					case 6:
						flamCurrentStep=8;
						break;

					case 7:
					case 8:
					case 9:
					case 10:
					case 11:
					case 12:
					case 13:
					case 14:
						shift = flamCurrentStep - 6;
						break;
					}
					if(flamCurrentStep<=1) continue;


					if(shift != 0)
					{
						// ######## SHIFT #######
						if(idx == shift*3)
						{
							const uint8_t vol = seq_patternSet.seq_subStepPattern[seq_activePattern][i][seq_stepIndex[i]].volume&STEP_VOLUME_MASK;
							const uint8_t note = seq_patternSet.seq_subStepPattern[seq_activePattern][track][seq_stepIndex[track]].note;
							seq_triggerVoice(track,vol,note);
						}

					}
					else
					{
						// ######## FLAM ########
						// idx value 0 to 24
						// 24 /3 = 8
						uint8_t div = 24/flam;
						//if(idx%(8/flamCurrentStep)==0)
						if(idx%(div)==0)
						{
							const uint8_t vol = seq_patternSet.seq_subStepPattern[seq_activePattern][i][seq_stepIndex[i]].volume&STEP_VOLUME_MASK;
							const uint8_t note = seq_patternSet.seq_subStepPattern[seq_activePattern][i][seq_stepIndex[i]].note;
							seq_triggerVoice(i,vol,note);
						}
					}
				}
			}
#endif
		}
	}
}

void seq_MidiSyncQueue(uint8_t cnt)
{

	seq_prescaleCounter = cnt;
	trigger_reset(1);
	seq_lastTick = systick_ticks;



	//seq_calcDeltaT(seq_tempo, isStepShuffled);
	if(seq_tempo !=0)
	{
		seq_calcDeltaT(seq_tempo, 0);
	}

}


IN_ITCM_RAM void seq_nextStep()
{
  midi_process_block = 1;
	led_blinkHandler(); //blink leds in sync to tempo/seq

	if(!seq_running)
		return;

	seq_masterStepCnt++;

	//---- calc master step position. max value is 64. also take in regard the pattern length -----
	// track 0 determines the master step position
	uint8_t masterStepPos;
	uint8_t seqlen;
	uint8_t longestTrack;

	longestTrack = seq_getLongestTrack();
	seqlen=seq_patternSet.length[seq_activePattern][longestTrack];
	if(!seqlen)
		seqlen=16;

	if( (((seq_stepIndex[longestTrack]+1) & 0x3f) == 0) || ((seq_stepIndex[longestTrack]+1) >= seqlen))
			//(((seq_patternSet.seq_subStepPattern[seq_activePattern][0][seq_stepIndex[0]+1]).note & PATTERN_END)) )
	{
		masterStepPos = 0;
		//a bar has passed
		//seq_barCounter++;
	}
	else
	{
		//determine the longest sequencer track and use it as master track (make sure all bars play befor epattern changes)
		masterStepPos = seq_stepIndex[longestTrack]+1;
	}

	//-------- check if the master track has ended and check if a pattern switch is necessary --------
	if(masterStepPos == 0)
	{
		if(seq_activePattern == seq_pendingPattern)
		{
			//check pattern settings if we have to auto change patterns
			seq_pendingPattern = seq_determineNextPattern(1);
			/*
			if(seq_pendingPattern >= SEQ_NEXT_RANDOM)
			{
				uint8_t limit = seq_pendingPattern - SEQ_NEXT_RANDOM +2;
				uint8_t rnd = GetRngValue() % limit;
				seq_pendingPattern = rnd;
			}
			*/
			if(seq_pendingPattern == SONG_END)
			{
				//TODO: check if this is enough? does GUI needs info?
				seq_setRunning(0);
				//make sure pending pattern does not remain at 64 resulting in a seg fault.
				seq_pendingPattern = seq_activeSong[0];
				if(seq_pendingPattern >= 64) seq_pendingPattern  = 0;
				seq_songPositionPointer = 0;
				return;
			}
			else if(seq_pendingPattern == SONG_LOOP)
			{
				//reset to song start
				seq_pendingPattern = seq_activeSong[0];
				seq_songPositionPointer = 0;
			}

		}

		// a new pattern is about to start
		// set pendingPattern active
		seq_setPendingPatternActive();

    midi_process_block = 0;
	}

	//---------- now check if the master track is at a full beat position to flash the start/stop button --------
	if((masterStepPos&3) == 0)
	{
		//&32 <=> %32
		//a quarter beat occured (multiple of 4 steps in the 16 step pattern)
		//turn on the start/stop led (beat indicator)
		led_setValue(1,LED_START_STOP);

	}
	else if ((masterStepPos&3) == 1)
	{
		//TODO datenmenge zur front reduzieren
		//turn it of again on the next step
		led_setValue(0,LED_START_STOP);
	}

	//--------- Time to process the single tracks -------------------------
	//trigger_clockTick(seq_stepIndex[0]+1);
//	trigger_clockTick(1, 0);

	int i;
	for(i=0;i<NUM_TRACKS;i++)
	{
		//increment the step index
		seq_stepIndex[i]++;
		//check if track end is reached

		// **PATROT we now use this for length
		seqlen=seq_patternSet.length[seq_activePattern][i];
		if(!seqlen)
			seqlen=16;

		if((seq_stepIndex[i]) >= seqlen || (seq_stepIndex[i] & 0x3f) == 0)
		{
			//if end is reached reset track to step 0
			seq_stepIndex[i] = 0;
		}



		//if track is not muted
		if(!(seq_mutedTracks & (1<<i) ) )
		{
/*
			//if main step (associated with current substep) is active
			if(seq_intIsMainStepActive(i,seq_stepIndex[i]/8,seq_activePattern)) {
*/
			//we only have mainsteps now
			if(seq_intIsMainStepActive(i,seq_stepIndex[i],seq_activePattern)) {



				// **RECORD if we are in erase mode (shift clear while record and playing)
				// and this is the active track on the front, we erase the note value
				if(seq_eraseActive && i==frontParser_activeTrack) {
					//sub steps are ditched, so only remove main step
					seq_setMainStep(seq_activePattern, frontParser_activeTrack, seq_stepIndex[i],0);
					//if steps are shown, (active bar) turn off LED
					if( (seq_stepIndex[i] >= (menu_activeBar*16)) && (seq_stepIndex[i] < (menu_activeBar+1)*16))
					{
						led_setValue(0,led_idx2StepLed(seq_stepIndex[i]%16));
					}
				} else
				// if sub-step is active
				//if(seq_intIsStepActive(i,seq_stepIndex[i],seq_activePattern))
				{
					//PROBABILITY

#if 0
					//every 8th step a new random value is generated
					//thus every sub step block has only one random value to compare against
					//allows randomisation of rolls by chance

					if((seq_stepIndex[i] & 0x07) == 0x00) //every 8th step
					{
						seq_rndValue[i] = GetRngValue()&0x7f;
					}
#else
					//every step gets a random value
					//GetRngValue()&0x7f;
					seq_rndValue[i].prev = seq_rndValue[i].current;
					seq_rndValue[i].current = seq_rndValue[i].next;
					seq_rndValue[i].next = GetRngValue()% 100 + 1; //value between 0 and 100
#endif

#if 0 //moved to flam/shift processing routine

					if( (seq_rndValue[i]) <= seq_patternSet.seq_subStepPattern[seq_activePattern][i][seq_stepIndex[i]].prob )
					{
						//only play if timeshift is not active
						if(seq_patternSet.seq_stepTiming[seq_activePattern][i][seq_stepIndex[i]] < 6)
						{
							if(seq_stepToSkip[i] == seq_stepIndex[i])
							{
								//freshly recorded step, skip and reset seq_stepToSkip[]
								seq_stepToSkip[i] = -1;
							}
							else
							{
								const uint8_t vol = seq_patternSet.seq_subStepPattern[seq_activePattern][i][seq_stepIndex[i]].volume&STEP_VOLUME_MASK;
								const uint8_t note = seq_patternSet.seq_subStepPattern[seq_activePattern][i][seq_stepIndex[i]].note;
								seq_triggerVoice(i,vol,note);
							}
						}
					}
#endif
				} // if sub step is active
			} // if main step is active
		} // if this track is not muted


	//---- check if the roll mode has to trigger the voice
	if(seq_rollRate!=0xff) //not in oneshot mode
	{
		if(seq_rollState & (1<<i))
		{
			//check if roll is active
			{
				if((seq_stepIndex[i]%seq_rollRate)==0)
				{
					const uint8_t vol = ROLL_VOLUME;

					const uint8_t note = seq_patternSet.seq_subStepPattern[seq_activePattern][i][seq_stepIndex[i]].note;
					seq_triggerVoice(i,vol,note);

					seq_addNote(i,vol, note); // todo should this be note or should it be SEQ_DEFAULT_NOTE (before my change it would have been SEQ_DEFAULT_NOTE)
				}
			}
		}
	}//end oneshot

}
	//TODO can maybe be combined with pattern change follow above?!
	//step display follows bars
	if(activeParameters[PAR_FOLLOW] & FOLLOW_BAR ) {

		//only update if bar changed
		if(menu_activeBar != seq_stepIndex[menu_getActiveVoice()]/16)
		{
			if( menu_activePage < PATTERN_LOAD_PAGE || menu_activePage > SONG_MODE_PAGE)
			{



				menu_setShownPattern(seq_activePattern);
				led_clearSequencerLeds();
				//query current sequencer step states and light up the corresponding leds
				const uint8_t trackNr = menu_getActiveVoice(); //max 6 => 0x6 = 0b110
				//uint8_t patternNr = menu_getViewedPattern(); //max 7 => 0x07 = 0b111
				//uint8_t value = (uint8_t)((trackNr<<4) | (patternNr&0x7));


				//update menu_activeBar and Bar LEDs
				menu_activeBar = seq_stepIndex[trackNr]/16;

				led_updateSeqLeds();

			}
		}
	}

	//send message to frontpanel
	//to display the current step
	/*
	uart_sendFrontpanelByte(FRONT_STEP_LED_STATUS_BYTE);
	uart_sendFrontpanelByte(FRONT_CURRENT_STEP_NUMBER_CC);
	uart_sendFrontpanelByte(seq_stepIndex[frontParser_activeTrack]);
	*/
	const uint8_t shownPattern = menu_getViewedPattern();
	const uint8_t playedPattern = menu_playedPattern;

	const uint8_t stepIdx = seq_stepIndex[frontParser_activeTrack];

	const uint8_t isOnActivePage = (stepIdx >= menu_activeBar*16) && (stepIdx < (menu_activeBar*16+16));

	if( (shownPattern == playedPattern) && (isOnActivePage)) {
		//only update chaselight LED when in step edit mode
		if( (menu_activePage < MENU_MIDI_PAGE) || menu_activePage == PERF_PAGE ||menu_activePage == SEQ_PAGE || menu_activePage == EUKLID_PAGE) {
			led_setActive_step(stepIdx%16);
		}
	} else {
		led_clearActive_step();
	}

	// check mtc, which might stop the sequencer if we haven't seen one in a while
	midiParser_checkMtc();

}
//------------------------------------------------------------------------------
uint8_t seq_getExtSync()
{
	return seq_isSyncExternal;
}
//------------------------------------------------------------------------------
void seq_setExtSync(uint8_t isExt)
{
	seq_isSyncExternal = isExt;
}
//------------------------------------------------------------------------------
void seq_armAutomationStep(uint8_t stepNr, uint8_t track,uint8_t isArmed)
{
	if(isArmed) {
		seq_armedArmedAutomationStep 	= stepNr;
		seq_armedArmedAutomationTrack 	= track;
	} else {
		seq_armedArmedAutomationStep 	= -1;
		seq_armedArmedAutomationTrack 	= -1;
	}
}
//------------------------------------------------------------------------------
void seq_setDeltaT(float delta)
{
	seq_deltaT = delta;
}
//------------------------------------------------------------------------------

/*This is called from IRQ handler when an external clock tick is received
 * master steps are used to keep the sync with the external clocks
 * a master step is a step that is directly triggered by the external clock signal.
 * non master steps are derived from the internaly calculated phase accumulator.
 * spacing is defined by the prescaler value
 * - with 32ppq every step is a master step
 * - with 4ppq only every 8th step is a master step
 * We set the next step index to a value - 1 because seq_nextStep() will
 * increment the value itself
 */
void seq_triggerNextMasterStep(uint8_t stepSize)
{
	uint8_t i, sn, len;
	for(i=0;i<NUM_TRACKS;i++) {
		len = seq_patternSet.length[seq_activePattern][i];
		if(!len) // length of 0 means length of 16 (since we are using 4 bits)
			len=16;
		len *= 8; // need length in steps

		if(seq_lastMasterStep[i] == 0) // need to set it so next step will inc it to 0
			sn=len-1; // set to last step before wrap around, effectively 0
		else
			sn=seq_lastMasterStep[i]-1; // adjust for seq_nextStep

		// establish the next step for this track for the sequencer
		seq_stepIndex[i] = sn;

		// save the position where we will trigger on the next external clock tick.
		// wrap around if we would exceed our track length
		seq_lastMasterStep[i] += stepSize;
		if(seq_lastMasterStep[i] >= len)
			seq_lastMasterStep[i] -= len;

		//set time to next step to zero, forcing the sequencer to process the next step now
		seq_setDeltaT(-1);
	}
}
//------------------------------------------------------------------------------
/*
void seq_resetDeltaAndTick()
{
	//if there are unplayed steps jump over them
	while(!seq_isNextStepSyncStep())
	{
		seq_nextStep();
	}

	//is shuffle delay necessary?

	if(seq_shuffle != 0)
	{
		//seq_deltaT = 0;^

		seq_deltaT 	= (1000*60)/seq_tempo; 	//bei 12 = 500ms = time for one beat
		seq_deltaT /= 96.f; //we run the internal clock at 96ppq -> seq clock 32 ppq == prescaler 3, midi clock 24 ppq == prescale 4
		seq_deltaT *= 4;

		seq_lastTick = systick_ticks;

		uint8_t stepInHalfBeat = seq_masterStepCnt&0xf;
		const float shuffleFactor = seq_shuffleTable[stepInHalfBeat] * seq_shuffle;
		const float originalDeltaT = seq_deltaT;

		seq_deltaT = shuffleFactor * originalDeltaT * 16.f;
		seq_lastShuffle = shuffleFactor;

		if(seq_deltaT <= 0)
		{
			seq_nextStep();
			seq_lastTick = systick_ticks;
			seq_calcDeltaT(seq_tempo);
		} else
		{
			seq_delayedSyncStepFlag =1;
		}

		seq_prescaleCounter = 0;

	}
	else
	{
		//play next sync step
		seq_nextStep();

		seq_lastTick = systick_ticks;
		seq_calcDeltaT(seq_tempo);

		seq_prescaleCounter = 0;
	}


}
*/
//------------------------------------------------------------------------------
//returns how many % [0:99] of the current step have elapsed (time to next step)
// used for MIDI recording quantisation.
// if current step is elapsed more than 50%, record step to next step
float seq_getElapsedStepTimePercent()
{
		// not really exact, but close enough for the midi record purpose
		return seq_prescaleCounter / 23.f * 100.f;
}
//------------------------------------------------------------------------------
/** call periodically to check if the next step has to be processed */

/*IN_ITCM_RAM*/ void seq_tick()
{



	if(seq_getExtSync())
	{

		if(sync_delayedStepTimer != 0)
		{
			if(systick_ticks >= sync_delayedStepTimer)
			{
				sync_delayedStepTimer = 0;
				seq_nextStep();
				seq_prescaleCounter=0;

				seq_MidiSyncQueue(0);
			}
		}
#if !HANDLE_SEQ_IN_CLOCK_INTERRUPT
			while(trigger_clockReceived != 0) //analogue clock
			{
				trigger_clockReceived--;

				//trigger_prescalerClockInput
				//seq_nextStep();

					uint8_t isStepShuffled = seq_getMasterStepCnt()%2 == 1;

					if(isStepShuffled)
					{
						float dt = seq_getDeltaTShuffleDelay(seq_getBpm());
						sync_delayedStepTimer = systick_ticks + dt;
					}
					else
					{
						seq_nextStep();
						//seq_MidiSyncQueue(0);
						seq_prescaleCounter=0;
						seq_MidiSyncQueue(0);
					}


				//reset internal clock cycle for next step
				//seq_prescaleCounter=0;
			}

			//midi clock flam stuff
			if(systick_ticks-seq_lastTick >= seq_deltaT)
			{


				float rest = systick_ticks-seq_lastTick - seq_deltaT;
				seq_lastTick = systick_ticks;



				//seq_calcDeltaT(seq_tempo, isStepShuffled);
				if(seq_tempo !=0)
				{
					seq_calcDeltaT(seq_tempo, 0);
				}
				//isStepShuffled = 0;
				seq_deltaT = seq_deltaT - rest;

				//do not autorun, internal clock used to upscale 24ppq MIDI clock for flam processing
				if(seq_prescaleCounter!=24)
				{


					//if(seq_prescaleCounter==12)
					if(seq_isRunning()==1)
					{
							trigger_clockTick(0, seq_prescaleCounter);
					}

					//flam

					// if((seq_prescaleCounter%3) == 0)
					 {
							seq_processFlam(seq_prescaleCounter);
					 }

					//seq_processFlam(seq_prescaleCounter);

					seq_prescaleCounter++;
					/*
					if(seq_prescaleCounter==23)
					{
						isStepShuffled = seq_masterStepCnt%2 == 1;
					}*/
					if(seq_prescaleCounter>=24)
					{
						//reset is done by midi or analogue clock
						//seq_prescaleCounter=0;


					}
				}
			}

			//TODO this has to be properly implemented once the clock in ppq prescaler is implemented as well
			//should work the same way as the midi implementation with uscaling the clock accordingly
			/*
			if(seq_prescaleCounter==12)
			{
					trigger_clockTick(0);
			}

			//flam
			 if((seq_prescaleCounter%3) == 0)
			 {
					seq_processFlam(seq_prescaleCounter/3);
			 }
			 */


#endif
	}
	else
	{
		if(seq_deltaT == -1)
		{
			seq_deltaT = 32000;
			seq_nextStep();

			return;
		}

		if(systick_ticks-seq_lastTick >= seq_deltaT)
		{

			float rest = systick_ticks-seq_lastTick - seq_deltaT;
			seq_lastTick = systick_ticks;



			seq_calcDeltaT(seq_tempo, isStepShuffled);
			isStepShuffled = 0;
			seq_deltaT = seq_deltaT - rest;



			//if((seq_prescaleCounter%SEQ_PRESCALER_MASK) == 0)
			if(seq_prescaleCounter == 0)
			{


				if(seq_getExtSync()) {

					//MIDI sync is handled directly from clockSync.c functions

	#if 0
					//for external sync we have a ratio of 3/4 ppq/steps
					//so when the 3rd ppq is received we have to activate the 4th step etc
					//advance only 2 steps automatically, then wait for sync message
					if(seq_isNextStepSyncStep()==0) {
						seq_delayedSyncStepFlag = 0;

						seq_nextStep();

					}
	#endif
				} else {
				//	seq_flamCnt = 0;
					seq_nextStep();
				}
			}



			//else if(seq_prescaleCounter==12)
			if(seq_isRunning())
			{
					trigger_clockTick(0, seq_prescaleCounter);
			}

			//flam

			// if((seq_prescaleCounter%3) == 0)
			 {
					seq_processFlam(seq_prescaleCounter);
			 }

			//seq_processFlam(seq_prescaleCounter);

			if(!seq_getExtSync()) //only send internal MIDI clock to output when external sync is off
			{
				if((seq_prescaleCounter%MIDI_PRESCALER_MASK) == 0)
				{
					seq_sendRealtime(MIDI_CLOCK);
				}
			}
			seq_prescaleCounter++;
			if(seq_prescaleCounter==23)
			{
				isStepShuffled = seq_masterStepCnt%2 == 1;
			}
			if(seq_prescaleCounter>=24)
			{
				seq_prescaleCounter=0;


			}
		}
	}

}

//------------------------------------------------------------------------------
void seq_setQuantisation(uint8_t value)
{
	seq_quantisation = value;
}
//------------------------------------------------------------------------------
void seq_toggleStep(uint8_t voice, uint8_t stepNr, uint8_t patternNr)
{
	if((seq_patternSet.seq_subStepPattern[patternNr][voice][stepNr].volume&STEP_ACTIVE_MASK)==0)
	{
		seq_patternSet.seq_subStepPattern[patternNr][voice][stepNr].volume |= STEP_ACTIVE_MASK;
	} else {
		seq_patternSet.seq_subStepPattern[patternNr][voice][stepNr].volume &= ~STEP_ACTIVE_MASK;
	}
}
//------------------------------------------------------------------------------
void seq_toggleMainStep(uint8_t voice, uint8_t stepNr, uint8_t patternNr)
{
	const uint8_t bar 	= stepNr / 16;
	const uint8_t shift = stepNr - bar*16;
	seq_patternSet.seq_mainSteps[bar][patternNr][voice] ^= (1<<shift);
}
//------------------------------------------------------------------------------
void seq_setMainStep(uint8_t patternNr, uint8_t voice, uint8_t stepNr, uint8_t onOff)
{
	const uint8_t bar 	= stepNr / 16;
	const uint8_t shift = stepNr - bar*16;

	if(onOff)
	{
		seq_patternSet.seq_mainSteps[bar][patternNr][voice] |= (1<<shift);
	}
	else
	{
		seq_patternSet.seq_mainSteps[bar][patternNr][voice] &= ~(1<<shift);
	}
}
//------------------------------------------------------------------------------
// this appears unused
//void seq_setStep(uint8_t voice, uint8_t stepNr, uint8_t onOff)
//{
//	if(onOff)
//	{
//		seq_patternSet.seq_subStepPattern[seq_activePattern][voice][stepNr].volume |= STEP_ACTIVE_MASK;
//	}
//	else
//	{
//		seq_patternSet.seq_subStepPattern[seq_activePattern][voice][stepNr].volume &= ~STEP_ACTIVE_MASK;
//	}
//}
//------------------------------------------------------------------------------
uint8_t seq_isRunning() {
	return seq_running;
}
//------------------------------------------------------------------------------
void seq_resetPosition()
{
	led_resetBlinkTimer();

	// reset all track rotations to 0. We are not saving rotated value. it's a performance tool.
	/*
	uint8_t i;
	for(i=0;i<NUM_TRACKS;i++) {
		seq_patternSet.seq_patternLengthRotate[seq_activePattern][i].rotate=0;
		// let the front know this is happening


		parameter_values[PAR_TRACK_ROTATION] = seq_getTrackRotation(i);
		menu_repaint(); // TODO we might not need this
	}
	*/

	//reset song position bar counter
	seq_lastShuffle = 0;
//	seq_barCounter = 0;
	seq_masterStepCnt = 0;
	seq_songPositionPointer = -1;
	//so the next seq_tick call will trigger the next step immediately
	seq_deltaT = 0;
	seq_sendRealtime(MIDI_STOP);

	// send notes off on all channels that have notes playing and reset our bitmap to reflect that
	voiceControl_noteOff(0xFF);

	trigger_reset(0);
	//trigger_allOff();


	// if mtc was doing it's thing, tell it to stop it.
	midiParser_checkMtc();


	// set start points back to default (happens on start and stop. needs to happen on start
		// in case the user has entered a rotate value while stopped)
		seq_setStepIndexToStart();
}
//------------------------------------------------------------------------------
void seq_setRunning(uint8_t isRunning)
{
	seq_running = isRunning;
	//jump to 1st step if sequencer is stopped
	if(!seq_running)
	{
		preset_touchTimer(PRESET_PAT_TIMER);
		//moved to seq_resetPosition()
	} else {
		//abort pattern save timer, since seq is running again...
		preset_touchTimer(PRESET_STOP_PAT_TIMER);

		trigger_clockReceived = 0;
		seq_prescaleCounter = 0;
		seq_sendRealtime(MIDI_START);
		trigger_reset(1);
	}

	/*
	// set start points back to default (happens on start and stop. needs to happen on start
	// in case the user has entered a rotate value while stopped)
	seq_setStepIndexToStart();
	*/
	seq_setStepIndexToStart();

}
//------------------------------------------------------------------------------
void seq_setSongModeEnabled(uint8_t onOff)
{
	seq_songModeEnabled = onOff;
	if(onOff)
	{
		seq_songPositionPointer = -1; //because nextStep() will first increase it
	}
}
//------------------------------------------------------------------------------
void seq_addPatternToSong(uint8_t patternNr)
{
	seq_activeSong[menu_songMenuPosition] = patternNr;
	if(menu_songMenuPosition < (MAX_SONG_LENGTH-1))
	{
		menu_songMenuPosition++;
	}
}
//------------------------------------------------------------------------------
void seq_deleteRow()
{
	 //seq_songData[NUM_KIT_SLOTS][MAX_SONG_LENGTH];
	for(int i=menu_songMenuPosition; i < MAX_SONG_LENGTH-1 ;i++)
	{
		seq_activeSong[i] = seq_activeSong[i+1];
	}

}
//------------------------------------------------------------------------------
void seq_insertRow()
{
	//for(int i=menu_songMenuPosition; i < MAX_SONG_LENGTH-1 ;i++)
	for(int i=MAX_SONG_LENGTH-1; i > menu_songMenuPosition ;i--)
	{
		seq_activeSong[i] = seq_activeSong[i-1];
	}
}
//------------------------------------------------------------------------------
static uint8_t seq_intIsStepActive(uint8_t voice, uint8_t stepNr, uint8_t patternNr)
{
	//return ((seq_patternSet.seq_subStepPattern[patternNr][voice][stepNr].volume & STEP_ACTIVE_MASK) > 0);

	//TODO since we don't have substeps anymore, we can probably ditch the whole active flag/mask thing
	return 1;
}
// above will be inlined, below is for ext linkage
uint8_t seq_isStepActive(uint8_t voice, uint8_t stepNr, uint8_t patternNr)
{
	return seq_intIsStepActive(voice,stepNr,patternNr);
}

//------------------------------------------------------------------------------
static uint8_t seq_intIsMainStepActive(uint8_t voice, uint8_t mainStepNr, uint8_t pattern)
{
	const uint8_t bar = mainStepNr / 16;
	const uint8_t shift = mainStepNr - bar*16;
	return (seq_patternSet.seq_mainSteps[bar][pattern][voice] & (1<<shift)) > 0;
}
// above is inlined below for ext linkage
uint8_t seq_isMainStepActive(uint8_t voice, uint8_t mainStepNr, uint8_t pattern)
{
	return seq_intIsMainStepActive(voice, mainStepNr, pattern);
}

//------------------------------------------------------------------------------
IN_ITCM_RAM void seq_setMute(uint8_t trackNr, uint8_t isMuted)
{
	if(trackNr==7)
	{
		//unmute all
		seq_mutedTracks = 0;
	} else {
		//mute/unmute tracks
		if(isMuted) {
			//mute track
			seq_mutedTracks |= (1<<trackNr);
			// turn off the midi note that may be playing on that track
			voiceControl_noteOff(midi_MidiChannels[trackNr]);
		} else {
			//unmute track
			seq_mutedTracks &= ~(1<<trackNr);
		}
	}
};
//------------------------------------------------------------------------------
uint8_t seq_isTrackMuted(uint8_t trackNr)
{
	if(seq_mutedTracks & (1<<trackNr) )
	{
		return 1;
	}
	return 0;
}
//------------------------------------------------------------------------------
// given a pattern and a track:
// this sends the main step info (which main steps are on/off) in addition
// to the length of the track
/*
void seq_sendMainStepInfoToFront(uint16_t stepNr)
{
	//the absolute number of patterns
	const uint8_t currentPattern	= stepNr / 7;
	const uint8_t currentTrack  	= stepNr - currentPattern*7;

	uint16_t dataToSend = seq_patternSet.seq_mainSteps[currentPattern][currentTrack];

	uart_sendFrontpanelSysExByte(  dataToSend	  & 0x7f); //1st 7 bit
	uart_sendFrontpanelSysExByte( (dataToSend>>7) & 0x7f); //2nd 7 bit
	uart_sendFrontpanelSysExByte( (dataToSend>>14)& 0x7f); //last 2 bit

	// send the track length
	uart_sendFrontpanelSysExByte( seq_patternSet.seq_patternLengthRotate[currentPattern][currentTrack].length);

}
*/
//--------------------------------------------------------------------
/** this one is more complicated than the 14 bit upper/lower nibble when transmitting the requested step number via SysEx.
 * because we have to transmit seven 8-bit values that make up the StepStruct we have to pack the data clever into the 7-bit
 * SysEx packets.
 *
 * first we send the lower 7-bit of all 7 values
 * then we transmit an additional 7-bit value containing all the MSBs from the previous seven values.
 *
 * 1st - (value1 & 0x7f)
 * 2nd - (value2 & 0x7f)
 * 3rd - (value3 & 0x7f)
 * 4th - (value4 & 0x7f)
 * 5th - (value5 & 0x7f)
 * 6th - (value6 & 0x7f)
 * 7th - (value7 & 0x7f)
 *
 * 8th - (0 MSB7 MSB6 MSB5 MSB4 MSB3 MSB2 MSB1)
 */
void seq_sendStepInfoToFront(uint16_t stepNr)
{

	//decode the step number
	//the step number is between 0 and NUMBER_STEPS*NUM_TRACKS*NUM_PATTERN = 64*7*8 = 7168 steps
	// stepNr / 64 = absolute number of patterns (each track has 8 patterns * 7 tracks)
	// numPatterns / 8 = the current track

	//the absolute number of patterns
	const uint8_t absPat 			= stepNr/NUM_STEPS;
	const uint8_t currentTrack 		= absPat / 8;
	const uint8_t currentPattern 	= absPat - currentTrack*8;
	const uint8_t currentStep		= stepNr - absPat*NUM_STEPS;

	//encode the data and send it back
	Step *dataToSend = &seq_patternSet.seq_subStepPattern[currentPattern][currentTrack][currentStep];

	uart_sendFrontpanelSysExByte(dataToSend->volume	& 0x7f);
	uart_sendFrontpanelSysExByte(dataToSend->prob	& 0x7f);
	uart_sendFrontpanelSysExByte(dataToSend->note	& 0x7f);

	uart_sendFrontpanelSysExByte(dataToSend->param1Nr	& 0x7f);
	uart_sendFrontpanelSysExByte(dataToSend->param1Val	& 0x7f);

	uart_sendFrontpanelSysExByte(dataToSend->param2Nr	& 0x7f);
	uart_sendFrontpanelSysExByte(dataToSend->param2Val	& 0x7f);

	//now the MSBs from all 7 values
	uart_sendFrontpanelSysExByte( 	(((dataToSend->volume 	& 0x80)>>7) |
									((dataToSend->prob	 	& 0x80)>>6) |
									((dataToSend->note	 	& 0x80)>>5) |
									((dataToSend->param1Nr	& 0x80)>>4) |
									((dataToSend->param1Val	& 0x80)>>3) |
									((dataToSend->param2Nr	& 0x80)>>2) |
									((dataToSend->param2Val	& 0x80)>>1))&0x7f
									);
}
//-------------------------------------------------------------------------------
void seq_setRoll(uint8_t voice, uint8_t onOff)
{
	if(voice >= 7) return;

	if(onOff) {
		seq_rollState |= (1<<voice);
		//if seq is stopped, always use oneshot mode
		if((seq_rollRate == 0xff) || (seq_isRunning() == 0) ) {
			//trigger one shot
			seq_triggerVoice(voice,ROLL_VOLUME,SEQ_DEFAULT_NOTE);
			//record roll notes
			seq_addNote(voice,ROLL_VOLUME,SEQ_DEFAULT_NOTE);
		}
	} else {
		seq_rollState &= ~(1<<voice);
	}
};
//--------------------------------------------------------------------------------
void seq_setRollRate(uint8_t rate)
{
	/*
	0 - one shot immediate trigger

	{"1/1"},
	{"1/2"},
	{"1/4"},
	{"1/8"},
	{"16"},
	{"24"},
	{"32"},
	{"48"},
	{"64"},
	{"128"},
	{"1/3"},
	{"1/6"},
	{"12"},
				*/

	switch(rate)
	{
	case 0:
		seq_rollRate = 0xfe;
		break;
	case 1: // 1/1
		seq_rollRate = 15;
		break;

	case 2: // 1/2
		seq_rollRate = 7;
		break;



	case 3:// 1/4
		seq_rollRate = 3;
			break;



	case 4:// 1/8
			seq_rollRate = 1;
			break;


default:
	case 5:// 1/16
		seq_rollRate = 0;
			break;
/*
	case 6: // 1/24
		seq_rollRate = 0x05;
		break;

	case 7:// 1/32
		seq_rollRate = 0x03;
		break;

	case 8:// 1/48
		seq_rollRate = 0x02;
		break;

	case 9://1/64
		seq_rollRate = 0x01;
		break;

	case 10://1/128
		seq_rollRate = 0x00;
		break;

	case 11:// 1/3
		seq_rollRate = 0x6;
			break;
	case 12:// 1/6
		seq_rollRate = 0x31;
			break;
	case 13:// 1/12
		seq_rollRate = 0x0a;
			break;
			*/
	}
	seq_rollRate +=1; //is there a reason for this offset here? seems the value could be assigned directly!?!

}
//------------------------------------------------------------------------
/** quantize a step to the seq_quantisation value*/
/*
#define QUANT(x) (NUM_STEPS/x)
static int8_t seq_quantize(int8_t step)
{
	uint8_t quantisationMultiplier=1;
	switch(seq_quantisation)
	{
	case QUANT_8:
		quantisationMultiplier = QUANT(8);
		break;

	case QUANT_16:
		quantisationMultiplier = QUANT(16);
		break;

	case QUANT_32:
		quantisationMultiplier = QUANT(32);
		break;

	case QUANT_64:
		quantisationMultiplier = QUANT(64);
		break;

	case NO_QUANTISATION:
	default:
		return step;
		break;
	}

	//now calc the quantisation
	float frac = step/(float)quantisationMultiplier;
	int8_t itg = (int8_t)frac;
	frac = frac - itg;

	if(frac>=0.5f)
	{
		return ((itg + 1)*quantisationMultiplier)&0x7f;
	}
	return itg*quantisationMultiplier;
}
*/

//------------------------------------------------------------------------
uint8_t seq_isAutomationStepArmed()
{
	if(seq_armedArmedAutomationStep	!= -1) return 1;
	return 0;
}
//------------------------------------------------------------------------
void seq_recordAutomation(uint8_t voice, uint8_t dest, uint8_t value)
{
	if(seq_recordActive)
	{
		//uint8_t quantizedStep = seq_quantize(seq_stepIndex[voice]);
		const uint8_t step = seq_stepIndex[voice];

		//only record to active steps
		/*

		if( seq_intIsMainStepActive(voice,quantizedStep/8,seq_activePattern) &&
				seq_intIsStepActive(voice,quantizedStep,seq_activePattern))
			*/
		if( seq_intIsMainStepActive(voice,step,seq_activePattern) )
		{
      if(seq_activeAutomTrack == 0) {
        seq_patternSet.seq_subStepPattern[seq_activePattern][voice][step].param1Nr = dest;
        seq_patternSet.seq_subStepPattern[seq_activePattern][voice][step].param1Val = value;
      } else {
        seq_patternSet.seq_subStepPattern[seq_activePattern][voice][step].param2Nr = dest;
        seq_patternSet.seq_subStepPattern[seq_activePattern][voice][step].param2Val = value;
      }
      
      // seq_patternSet.seq_subStepPattern[seq_activePattern][voice][step].paramNr[seq_activeAutomTrack] = dest;
      // seq_patternSet.seq_subStepPattern[seq_activePattern][voice][step].paramVal[seq_activeAutomTrack] = value;
      
		}
	}

	if( (seq_armedArmedAutomationStep	!= -1) && (seq_armedArmedAutomationTrack != -1) )
	{
		//step button is held down
		//-> set step automation parameters
		if(seq_activeAutomTrack == 0) {
			seq_patternSet.seq_subStepPattern[seq_activePattern]
			                                  [seq_armedArmedAutomationTrack]
			                                   [seq_armedArmedAutomationStep].param1Nr = dest;
			seq_patternSet.seq_subStepPattern[seq_activePattern]
			                                  [seq_armedArmedAutomationTrack]
			                                   [seq_armedArmedAutomationStep].param1Val = value;
		} else {
			seq_patternSet.seq_subStepPattern[seq_activePattern]
			                                  [seq_armedArmedAutomationTrack]
			                                   [seq_armedArmedAutomationStep].param2Nr = dest;
			seq_patternSet.seq_subStepPattern[seq_activePattern]
			                                  [seq_armedArmedAutomationTrack]
			                                   [seq_armedArmedAutomationStep].param2Val = value;
		}
	}
}

PatternSet* get_seq_patternSet_p(void)
{
  return &seq_patternSet;
}

//------------------------------------------------------------------------
void seq_addNote(uint8_t trackNr,uint8_t vel, uint8_t note)
{
	uint8_t targetPattern;
	Step *stepPtr;
	//only record notes when seq is running and recording
	if(seq_running && seq_recordActive)
	{
		//const int8_t quantizedStep = seq_quantize(seq_stepIndex[trackNr]);

		uint8_t step = seq_stepIndex[trackNr];
		int8_t quantizedStep = (seq_getElapsedStepTimePercent() >= 50.f) ? seq_stepIndex[trackNr] + 1 : seq_stepIndex[trackNr];



		// **RECORD fix for recording across patterns
		if(step==0 && seq_stepIndex[trackNr] > (NUM_STEPS/2)) {
			// this means that we hit a note in 2nd half of the bar and quantization pushed
			// the note to position 0 of the next bar.
			// need to see if there is about to be a pattern change so that the note
			// ends up on 0 of the next pattern
			targetPattern=seq_determineNextPattern(0);

		} else
			targetPattern=seq_activePattern;

		//special care must be taken when recording midi notes!
		//since per default the 1st substep of a mainstep cluster is always active
		//we will get double notes when a substep other than ss1 is recorded
		/*
		if(!seq_intIsMainStepActive(trackNr, step, targetPattern))
		{
			//if the mainstep is not active, we clear the 1st substep
			//to prevent double notes while recording
			seq_patternSet.seq_subStepPattern[targetPattern][trackNr][(quantizedStep/8)*8].volume 	&= ~STEP_ACTIVE_MASK;
		}
		*/

		//limit to track length
		quantizedStep = quantizedStep%seq_patternSet.length[targetPattern][trackNr];
		if(step != quantizedStep)
		{
			step = quantizedStep;
			seq_stepToSkip[trackNr] = step;
		}

		//set the current step in the requested track active
		stepPtr=&seq_patternSet.seq_subStepPattern[targetPattern][trackNr][step];
		stepPtr->note 		= note;				// note (was SEQ_DEFAULT_NOTE)
		stepPtr->volume		= vel;				// new velocity
		stepPtr->prob		= 100;				// 100% probability
		stepPtr->volume 	|= STEP_ACTIVE_MASK;

		//activate corresponding main step
		seq_setMainStep(targetPattern, trackNr, step,1);

		if( (frontParser_shownPattern == targetPattern) && ( frontParser_activeTrack == trackNr) && (menu_activeBar == (step/16)) )
		{

			if(menu_activePage != PERF_PAGE) //do not show active steps on perf. page
			{
				//limit to 16 steps
				uint8_t stepNr = (uint8_t)(step%16);
				uint8_t led = 0;
				switch(stepNr)
				{
					case 0:
						led = LED_STEP1;
						break;
					case 1:
						led = LED_STEP2;
						break;
					case 2:
						led = LED_STEP3;
						break;
					case 3:
						led = LED_STEP4;
						break;
					case 4:
						led = LED_STEP5;
						break;
					case 5:
						led = LED_STEP6;
						break;
					case 6:
						led = LED_STEP7;
						break;
					case 7:
						led = LED_STEP8;
						break;
					case 8:
						led = LED_STEP9;
						break;
					case 9:
						led = LED_STEP10;
						break;
					case 10:
						led = LED_STEP11;
						break;
					case 11:
						led = LED_STEP12;
						break;
					case 12:
						led = LED_STEP13;
						break;
					case 13:
						led = LED_STEP14;
						break;
					case 14:
						led = LED_STEP15;
						break;
					case 15:
						led = LED_STEP16;
						break;


				}
				led_setValue(1,led);
			}
		}
	}
}

//------------------------------------------------------------------------
// **RECORD erase a main step and all it's sub steps on the active pattern
// for the specified voice
/*
static void seq_eraseStepAndSubSteps(const uint8_t voice, const uint8_t mainStep)
{
	uint8_t i;
	// turn off the main step
	seq_setMainStep(seq_activePattern, voice, mainStep,0);

	// turn off all substeps
	for(i=(uint8_t)(mainStep*8);i<(uint8_t)((mainStep+1)*8);i++) {
		seq_resetNote(&seq_patternSet.seq_subStepPattern[seq_activePattern][voice][i]);
	}

	// first substep needs to be made active
	seq_patternSet.seq_subStepPattern[seq_activePattern][voice][(uint8_t)(mainStep*8)].volume |= STEP_ACTIVE_MASK;

	//if( (frontParser_shownPattern == seq_activePattern) && ( frontParser_activeTrack == voice) )
	//{
		// todo if the pattern is shown on the front, update the leds
		// 		figure out. Right now it clears the LED's after shift is released... which is fine for now.
	//}

}
*/

//------------------------------------------------------------------------
void seq_setRecordingMode(uint8_t active)
{
	seq_recordActive = active;
}

void seq_setErasingMode(uint8_t active)
{
	seq_eraseActive = active;
}

//------------------------------------------------------------------------------
// reset a step to it's default state
static void seq_resetNote(Step *step)
{
	step->note 		= SEQ_DEFAULT_NOTE;
	step->param1Nr 	= NO_AUTOMATION;
	step->param1Val = 0;
	step->param2Nr	= NO_AUTOMATION;
	step->param2Val	= 0;
	step->prob		= 100;
	step->volume	= 127; // clears active bit as well
}
//------------------------------------------------------------------------------
void seq_clearTrack(uint8_t trackNr, uint8_t pattern)
{
	int k;
	for(k=0;k<NUM_STEPS;k++)
	{
		seq_resetNote(&seq_patternSet.seq_subStepPattern[pattern][trackNr][k]);

		seq_patternSet.seq_stepTiming[pattern][trackNr][k].flam = 0;
		seq_patternSet.seq_stepTiming[pattern][trackNr][k].shift = 7;   // 7 = no shift

		//every 1st step in a substep pattern active
		if( (k%8) == 0)
			seq_patternSet.seq_subStepPattern[pattern][trackNr][k].volume |= STEP_ACTIVE_MASK ;
	}

	// all main steps off for this track
	for(uint8_t i=0;i<NUM_BARS;i++)	seq_patternSet.seq_mainSteps[i][pattern][trackNr] = 0;

	//**PATROT all pattern rotations off and all patterns set to length 16
	seq_patternSet.length[pattern][trackNr]=16;

}
//------------------------------------------------------------------------------
void seq_clearPattern(uint8_t pattern)
{
	int i;
	for(i=0;i<NUM_TRACKS;i++)
		seq_clearTrack(i, pattern);
}

//------------------------------------------------------------------------------
void seq_clearAutomation(uint8_t trackNr, uint8_t pattern, uint8_t automTrack)
{
	int k;

	if(automTrack==0)
	{
		for(k=0;k<NUM_STEPS;k++)
		{
			seq_patternSet.seq_subStepPattern[pattern][trackNr][k].param1Nr 	= NO_AUTOMATION;
			seq_patternSet.seq_subStepPattern[pattern][trackNr][k].param1Val 	= 0;
		}
	} else {
		for(k=0;k<NUM_STEPS;k++)
		{
			seq_patternSet.seq_subStepPattern[pattern][trackNr][k].param2Nr		= NO_AUTOMATION;
			seq_patternSet.seq_subStepPattern[pattern][trackNr][k].param2Val	= 0;
		}
	}
}
//------------------------------------------------------------------------------
void seq_copyTrack(uint8_t srcNr, uint8_t dstNr, uint8_t pattern)
{
	// todo this fn and the next are very similar. Add a new fn that takes src, dest track and src, dest pat
	// and then have these two fn's call it instead.
	int k;
	Step *src, *dst;
	for(k=0;k<NUM_STEPS;k++)
	{
		dst=&seq_patternSet.seq_subStepPattern[pattern][dstNr][k];
		src=&seq_patternSet.seq_subStepPattern[pattern][srcNr][k];
		dst->note		= src->note;
		dst->param1Nr 	= src->param1Nr;
		dst->param1Val 	= src->param1Val;
		dst->param2Nr	= src->param2Nr;
		dst->param2Val	= src->param2Val;
		dst->prob		= src->prob;
		dst->volume		= src->volume;

		seq_patternSet.seq_stepTiming[pattern][dstNr][k] = seq_patternSet.seq_stepTiming[pattern][srcNr][k];
	}

	// copy which main steps are on/off
	for(uint8_t i=0;i<NUM_BARS;i++)	seq_patternSet.seq_mainSteps[i][pattern][dstNr] = seq_patternSet.seq_mainSteps[i][pattern][srcNr];

	// copy length and rotation offset from source track
	seq_patternSet.length[pattern][dstNr] = seq_patternSet.length[pattern][srcNr];


}
//------------------------------------------------------------------------------
void seq_copyPattern(uint8_t src, uint8_t dst)
{
	int k,j;
	Step *psrc, *pdst;
	for(j=0;j<NUM_TRACKS;j++)
	{
		for(k=0;k<NUM_STEPS;k++)
		{
			pdst=&seq_patternSet.seq_subStepPattern[dst][j][k];
			psrc=&seq_patternSet.seq_subStepPattern[src][j][k];
			pdst->note			= psrc->note;
			pdst->param1Nr 		= psrc->param1Nr;
			pdst->param1Val 	= psrc->param1Val;
			pdst->param2Nr		= psrc->param2Nr;
			pdst->param2Val		= psrc->param2Val;
			pdst->prob			= psrc->prob;
			pdst->volume		= psrc->volume;

			seq_patternSet.seq_stepTiming[dst][j][k] = seq_patternSet.seq_stepTiming[src][j][k];
		}

		for( uint8_t i=0;i<NUM_BARS;i++)
		{
			seq_patternSet.seq_mainSteps[i][dst][j] = seq_patternSet.seq_mainSteps[i][src][j];
		}

		// copy length and rotation offset from source pattern for the track
		seq_patternSet.length[dst][j] = seq_patternSet.length[src][j];

		seq_patternSet.shuffle[dst] = seq_patternSet.shuffle[src];
		seq_patternSet.kitNr[dst] = seq_patternSet.kitNr[src];
	}
}
//------------------------------------------------------------------------------
void seq_setActiveAutomationTrack(uint8_t trackNr)
{
	seq_activeAutomTrack = trackNr;
}

uint8_t seq_getActiveAutomationTrack(void)
{
  return seq_activeAutomTrack;
}
//------------------------------------------------------------------------------
/*
static uint8_t seq_isNextStepSyncStep()
{
	if(seq_delayedSyncStepFlag)
	{
		seq_delayedSyncStepFlag = 0;
		seq_prescaleCounter = 0;
		return 0;
	}
	if( ((seq_stepIndex[0] & 0x3) % 4) == 3) {
		return 1;
	}
	return 0;
}
*/
//------------------------------------------------------------------------------

void seq_midiNoteOff(uint8_t chan)
{
	uint8_t i;
	MidiMsg msg;

	// we are not filtering according to tx filter because they might have turned that
	// setting on while a note was sustaining

	msg.bits.length=2;
	msg.data2=0;

	if(chan==0xff) { // all notes off
		for(i=0; i<16; i++)
			if((1<<i) & midi_notes_on) {
				msg.status=	NOTE_OFF | i;
				msg.data1=midi_chan_notes[i];
				seq_sendMidi(msg);
			}
		// reset all
		midi_notes_on=0;
		return;
	}
	// The proper way to do a note off is with 0x80. 0x90 with velocity 0 is also used, however I think there is still
	// synth gear out there that doesn't recognize that properly.
	if((1<<chan) & midi_notes_on) {
		msg.status=	NOTE_OFF | chan;
		msg.data1=midi_chan_notes[chan];
		seq_sendMidi(msg);
		// turn off our knowledge of that note playing
		midi_notes_on &= (~(1<<chan));
	}
}

static void seq_sendRealtime(const uint8_t status)
{
	static MidiMsg msg = {0};
	// FILT filter out realtime msgs if appropriate
	if((midiParser_txRxFilter & (MIDI_FILTER_SYSTEM<<4))==0)
		return;
	msg.status=status;
	seq_sendMidi(msg);

}

/* Send a note on message. This will filter out these messages if appropriate
 */
void seq_sendMidiNoteOn(const uint8_t channel, const uint8_t note, const uint8_t veloc)
{
	static MidiMsg msg = { .bits = { .length = 2 } };  // status + 2 data bytes
	// FILT filter out note msgs if appropriate
	if((midiParser_txRxFilter & (MIDI_FILTER_NOTES<<4))==0)
		return;

	msg.status=NOTE_ON | channel;
	msg.data1=note;
	msg.data2=veloc;
	seq_sendMidi(msg);


	// keep track of which notes are on so we can turn them off later
	midi_chan_notes[channel]=note;
	midi_notes_on |= (1 << channel);

}


uint8_t seq_getStepIndex(uint8_t trackNr)
{
	return seq_stepIndex[trackNr];
}

/* This will send a prog change on the global channel and will filter
 * out the message if appropriate
 */
static void seq_sendProgChg(const uint8_t ptn)
{
	static MidiMsg msg = { .bits = { .length = 1 } };  // status + 1 data byte

	// FILT filter out PC msgs if appropriate
	if((midiParser_txRxFilter & (MIDI_FILTER_PROG_C<<4))==0)
		return;

	msg.status = PROG_CHANGE | midi_MidiChannels[7];
	msg.data1=ptn;
	msg.bits.length=1;
	seq_sendMidi(msg);
}

/* **PATROT set the step starting index to the position where the pattern rotation would have it start.
 *  A pattern rotation of 0 means start at the beginning of the pattern. max value is 15.
 *  Each value represents a main step interval (which contains 8 substeps)
 *
 *  This is called when the sequencer starts/stops running, also when a pattern change takes place
 */

static void seq_setStepIndexToStart()
{
	/*
	uint8_t len, rot, i;
	for(i=0;i<NUM_TRACKS;i++) {
		// adjust rot in case the pattern length is less than the rotated amount
		// len is 0-15 where a value of 0 means 16
		rot=seq_patternSet.seq_patternLengthRotate[seq_activePattern][i].rotate;
		len=seq_patternSet.seq_patternLengthRotate[seq_activePattern][i].length;
		if(len && (rot > len))
			rot = rot % len;

		// this is for external clock sync via trigger expansion kit (the ext tick will adjust this -1)
		seq_lastMasterStep[i] = (8 * rot);

		// -1 here because we increment it first thing when we start
		seq_stepIndex[i] = ( 8 * rot) - 1;

	}
	*/
	for(uint8_t i=0;i<NUM_TRACKS;i++) {
		// this is for external clock sync via trigger expansion kit (the ext tick will adjust this -1)
		seq_lastMasterStep[i] = 0;

		// -1 here because we increment it first thing when we start
		seq_stepIndex[i] =  - 1;

	}

}

