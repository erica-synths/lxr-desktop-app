/*
 * frontPanelParser.c
 *
 * Created: 27.04.2012 12:04:00
 *  Author: Julian
 *
 * Copyright (C) 2012-2026 Erica Synths
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * This file is part of the LXR firmware. It is licensed under the GNU General
 * Public License version 3, with the additional permission and terms in
 * NOTICE.md. See LICENSE and NOTICE.md in the root of the repository.
 */ 
#include "frontPanelParser_ui.h"
#include "menu/menu.h"
#include <stdio.h>

#include "ledHandler.h"
#include "IO/uart.h"
#include "buttonHandler.h"
#include "front.h"
//debug
#include <stdlib.h>
#include "lcd.h"
//#include <util/atomic.h>
#include "Preset/PresetManager.h"
//--

static uint8_t frontParser_rxCnt=0;
volatile MidiMsg frontParser_midiMsg;
static uint16_t frontParser_nrpnNr = 0;

uint8_t frontPanel_sysexMode = 0;

volatile uint8_t frontParser_newSeqDataAvailable = 0;
volatile Step frontParser_stepData;
//sysex buffer used to store 7 byte sysex chunks
volatile static uint8_t frontParser_sysexBuffer[7];

uint8_t frontParser_nameIndex = 0;


//------------------------------------------------------------
#define NRPN_MUTE_1 1000
#define NRPN_MUTE_7 1006
void frontParser_parseNrpn(uint8_t value)
{
	uint16_t paramNr=frontParser_nrpnNr+128;

	if(paramNr < NUM_PARAMS) {
			activeParameters[paramNr] = value;
	}
	
	if( (paramNr >= PAR_TARGET_LFO1) && (paramNr <= PAR_TARGET_LFO6) )
	{
		//**LFO receive nrpn translate TODO this needs to be checked. I'm not sure what needs to happen here.
		// It seems like the assumption is that in this case, value represents an encoded menupage value (this code
		// used to call the now defunct getModTargetValue)

		//LFO
		uint8_t lfoNr = (uint8_t)(paramNr-PAR_TARGET_LFO1);
		if(lfoNr>5)lfoNr=5;

		// value (might) represents an actual parameter number, we need to convert to index into modTargets
		activeParameters[paramNr]=paramToModTarget[value];
		// this was the old code
		//since the LFO target calculation from the index number needs to know about the menu structure (menuPages)
		//we need to send back the right target param number to the cortex
		//value = getModTargetValue(parameter_values[frontParser_nrpnNr+128],
		//		(uint8_t)(parameter_values[PAR_VOICE_LFO1+lfoNr]-1));

		uint8_t upper = (uint8_t)(((value&0x80)>>7) | (((lfoNr)&0x3f)<<1));
		uint8_t lower = value&0x7f;
		frontPanel_sendData(CC_LFO_TARGET,upper,lower);
	}
	else if ( (paramNr >= PAR_VEL_DEST_1) && (paramNr <= PAR_VEL_DEST_6) )
	{
		//**VELO receive nrpn translate to parameter. TODO this needs to be checked as well

		// value (might) represents an actual parameter number, we need to convert to index into modTargets
		activeParameters[paramNr]=paramToModTarget[value];

		// old code
		//uint8_t param = parameter_values[frontParser_nrpnNr+128];
		//if(param > (NUM_SUB_PAGES * 8 -1))
		//param = (NUM_SUB_PAGES * 8 -1);
		//uint8_t value = getModTargetValue(param, (uint8_t)(frontParser_nrpnNr+128 - PAR_VEL_DEST_1));
				
		uint8_t upper,lower;
		upper = (uint8_t)((uint16_t)((value&0x80)>>7) | (((paramNr-PAR_VEL_DEST_1)&0x3f)<<1));
		lower = value&0x7f;
		frontPanel_sendData(CC_VELO_TARGET,upper,lower);

	} else if ( (frontParser_nrpnNr >= NRPN_MUTE_1) && (frontParser_nrpnNr <= NRPN_MUTE_7) )
	{
		const uint8_t voice = (uint8_t)(frontParser_nrpnNr - NRPN_MUTE_1);
		const uint8_t onOff = value;
		buttonHandler_muteVoice(voice,onOff);
		
	}
}
//------------------------------------------------------------
void frontPanel_ccHandler()
{
	//get the real parameter number from the cc number
	const uint8_t parNr =(uint8_t)( frontParser_midiMsg.data1 - 1);
	DISABLE_SIGN_WARNING
	if(parNr == 255/*PAR_BANK_CHANGE*/)
	{
		// A CC value is 0-127 but there are only NUM_KIT_SLOTS (64) kits in RAM,
		// so values past the last kit wrap around (64 = kit 0, 127 = kit 63).
		const uint8_t kitNr = frontParser_midiMsg.data2 % NUM_KIT_SLOTS;
		menu_selectedPresetNr[0] = kitNr;
		//preset_loadDrumset(kitNr, 0);
		preset_loadDrumsetRam(kitNr,0, 0);
		preset_touchTimer(PRESET_KIT_TIMER);
		menu_repaint();
		return;
	} else if(parNr == NRPN_DATA_ENTRY_COARSE-1) {
			frontParser_parseNrpn(frontParser_midiMsg.data2);
	} else if(parNr == NRPN_FINE-1) {
			frontParser_nrpnNr &= ~0x7f;
			frontParser_nrpnNr |= frontParser_midiMsg.data2;
	} else if(parNr == NRPN_COARSE-1) {
			frontParser_nrpnNr &= 0x7f;
			frontParser_nrpnNr |= frontParser_midiMsg.data2<<7;
	}
	END_DISABLE_WARNING
	
	//set the parameter value
	if(parNr >= END_OF_SOUND_PARAMETERS)
	{
		activeParameters[parNr] = frontParser_midiMsg.data2;
	}
	else
	{
		activeParameters[parNr] = frontParser_midiMsg.data2;
	}
	
	//repaint the LCD
	menu_repaint();
	
};
//------------------------------------------------------------
void frontPanel_parseData(uint8_t data)
{
	// if high byte set a new message starts
	if(data&0x80)
	{	
		//reset the byte counter
		frontParser_rxCnt = 0;
		frontParser_midiMsg.status = data;
		
		
		/*
		if(data==SYSEX_START  )
			{
				//we just entered the sysex mode - no data received yet
			//	lcd_setcursor(5,2);
			//	lcd_string("SYSSTART");
			
			}
			*/
			
		
	}
	else
	{
		if(frontParser_midiMsg.status == SYSEX_START)
		{
			/*
			//we are now in sysex mode
			if(data==SYSEX_START)
			{
				//we just entered the sysex mode - no data received yet
			}				
			else
			*/
			if(frontPanel_sysexMode == SYSEX_REQUEST_STEP_DATA)
			{
				//we are expecting step packages send from the sequencer
				//1st 7 lower nibble 7 bit messages
				//then an upper nibble 7 bit message containing the missing 7 upper bits
				//char text[5];
				if(frontParser_rxCnt<7)
				{
					frontParser_sysexBuffer[frontParser_rxCnt++] = data;
				//	lcd_setcursor(5,2);
				//	itoa(frontParser_rxCnt,text,10);
				//	lcd_string(text);
					
				}
				else
				{
					//now we have to distribute the MSBs to the sysex data
					for(int i=0;i<7;i++)
					{
						DISABLE_CONV_WARNING
						frontParser_sysexBuffer[i] |= ((data&(1<<i))<<(7-i));
						END_DISABLE_WARNING
						
					}
					frontParser_stepData.volume = frontParser_sysexBuffer[0];
					frontParser_stepData.prob = frontParser_sysexBuffer[1];
					frontParser_stepData.note = frontParser_sysexBuffer[2];
					frontParser_stepData.param1Nr = frontParser_sysexBuffer[3];
					frontParser_stepData.param1Val = frontParser_sysexBuffer[4];
					frontParser_stepData.param2Nr = frontParser_sysexBuffer[5];
					frontParser_stepData.param2Val = frontParser_sysexBuffer[6];
					
					//signal that a ne data chunk is available
					frontParser_newSeqDataAvailable = 1;
					//reset receive counter for next chunk
					frontParser_rxCnt = 0;
					
					//lcd_setcursor(5,2);
					//lcd_string("complete");
				}					
				
			}
			/*
			else if (frontPanel_sysexMode == SYSEX_REQUEST_PATTERN_DATA)
			{
				if(frontParser_rxCnt<1)
				{
					//1st byte
					frontParser_sysexBuffer[frontParser_rxCnt++] = data;
				} else {
					//2nd byte
					frontParser_sysexBuffer[frontParser_rxCnt++] = data;
					
					uint8_t next = frontParser_sysexBuffer[0];
					uint8_t repeat = frontParser_sysexBuffer[1];
					//we abuse the stepData struct to store the pattern data
					frontParser_stepData.volume = next;
					frontParser_stepData.prob = repeat;
					
					//signal that a ne data chunk is available
					frontParser_newSeqDataAvailable = 1;
					//reset receive counter for next chunk
					frontParser_rxCnt = 0;
				}
				
			}
			*/
			/*
			else if(frontPanel_sysexMode == SYSEX_REQUEST_MAIN_STEP_DATA)
			{
				if(frontParser_rxCnt<3)
				{
					//1st 2 nibbles + last 2 bit
					frontParser_sysexBuffer[frontParser_rxCnt++] = data;
				} else {
					// length information
					frontParser_sysexBuffer[frontParser_rxCnt++] = data;
					
					uint16_t mainStepData = frontParser_sysexBuffer[0] |
							(uint16_t)(frontParser_sysexBuffer[1]<<7) |
							(uint16_t)(frontParser_sysexBuffer[2]<<14);
					//we abuse the stepData struct to store the main step data and the length
					frontParser_stepData.volume = (uint8_t)(mainStepData>>8);
					frontParser_stepData.prob = (uint8_t)(mainStepData&0xff);
					frontParser_stepData.note = frontParser_sysexBuffer[3];
					
					//signal that a new data chunk is available
					frontParser_newSeqDataAvailable = 1;
					//reset receive counter for next chunk
					frontParser_rxCnt = 0;
				}
			}
			*/
		}
		else if(frontParser_rxCnt==0)
		{
			//parameter nr
			frontParser_midiMsg.data1 = data;
			frontParser_rxCnt++;
		}
		else
		{
			//parameter value
			frontParser_midiMsg.data2 = data;
			frontParser_rxCnt=0;
			//process the received data
			if((frontParser_midiMsg.status & 0xF0) == MIDI_CC) //sound cc data from cortex
			{
				frontPanel_ccHandler();
			}
			else
			{
				/*
				if(frontParser_midiMsg.status == PRESET_NAME)
				{
					
					if(frontParser_midiMsg.data2 & 0x40)
					{
						frontParser_nameIndex = 0;
					}
					
					presetNameBuffer[frontParser_nameIndex] =(char)(
							(frontParser_midiMsg.data1&0x7f) |
							((frontParser_midiMsg.data2&0x7f)<<7));
					frontParser_nameIndex++;
					frontParser_nameIndex &= 0x7; //wrap at 8
					if(frontParser_nameIndex==0)
					{
						menu_repaintAll();
					}						
									
				} 
				else
					*/
				if(frontParser_midiMsg.status == SET_P1_DEST)
				{
					//**AUTOM - translate cortex value to mod target index
					// a value of FF means no automation (on the back end)
					uint8_t dst=(uint8_t)((frontParser_midiMsg.data1<<7) | frontParser_midiMsg.data2);
					if(dst==0xFF)
						dst=0;
					activeParameters[PAR_P1_DEST] = paramToModTarget[dst];
					menu_repaintAll();
				}
				else if(frontParser_midiMsg.status == SET_P2_DEST)
				{
					//**AUTOM - translate cortex value to mod target index
					uint8_t dst=(uint8_t)((frontParser_midiMsg.data1<<7) | frontParser_midiMsg.data2);
					if(dst==0xFF)
						dst=0;
					activeParameters[PAR_P2_DEST] = paramToModTarget[dst];
					menu_repaintAll();
				}
				else if(frontParser_midiMsg.status == SET_P1_VAL)
				{
					activeParameters[PAR_P1_VAL] = (uint8_t)((frontParser_midiMsg.data1<<7) | frontParser_midiMsg.data2);
					menu_repaintAll();
				}
				else if(frontParser_midiMsg.status == SET_P2_VAL)
				{
					activeParameters[PAR_P2_VAL] = (uint8_t)((frontParser_midiMsg.data1<<7) | frontParser_midiMsg.data2);
					menu_repaintAll();
				}
				
				else if(frontParser_midiMsg.status == SEQ_CC)
				{
					switch(frontParser_midiMsg.data1)
					{
						/*
						case SEQ_SET_PAT_BEAT:
						parameter_values[PAR_PATTERN_BEAT] = frontParser_midiMsg.data2;
						menu_repaint();
						break;	

						case SEQ_SET_PAT_NEXT:
						parameter_values[PAR_PATTERN_NEXT] = frontParser_midiMsg.data2;
						menu_repaint();
						break;
						*/
						
						case SEQ_TRACK_LENGTH:
							activeParameters[PAR_TRACK_LENGTH] = frontParser_midiMsg.data2;
							menu_repaint();
							break;
						// **PATROT - receive rotation value from back for active track
							/*
						case SEQ_TRACK_ROTATION:
							parameter_values[PAR_TRACK_ROTATION] = frontParser_midiMsg.data2;
							menu_repaint(); // TODO we might not need this
							break;
							*/
						/*
						case SEQ_EUKLID_LENGTH:
							activeParameters[PAR_EUKLID_LENGTH] = frontParser_midiMsg.data2;
							menu_repaint();
							break;
						
						case SEQ_EUKLID_STEPS:
							activeParameters[PAR_EUKLID_STEPS] = frontParser_midiMsg.data2;
							menu_repaint();
							break;
						
						case SEQ_EUKLID_ROTATION:
							activeParameters[PAR_EUKLID_ROTATION] = frontParser_midiMsg.data2;
							menu_repaint();
						break;
						*/

						case SEQ_VOLUME:
							activeParameters[PAR_STEP_VOLUME] = frontParser_midiMsg.data2;
						menu_repaintAll();
						break;
						
						case SEQ_PROB:
							activeParameters[PAR_STEP_PROB] = frontParser_midiMsg.data2;
						menu_repaintAll();
						break;
						
						case SEQ_NOTE:
							activeParameters[PAR_STEP_NOTE] = frontParser_midiMsg.data2;
						menu_repaintAll();
						break;
						
						case SEQ_FLAM:
						activeParameters[PAR_STEP_FLAM] = frontParser_midiMsg.data2;
							menu_repaintAll();
							break;

						case SEQ_SHIFT:
						activeParameters[PAR_STEP_SHIFT] = frontParser_midiMsg.data2;
							menu_repaintAll();
							break;

						/*
						case SEQ_GET_ACTIVE_PAT:
						//only in mute mode relevant
						//led_clearSequencerLeds9_16();
						led_clearSelectLeds();
						// set led to show active pattern
						led_setValue(1,LED_PART_SELECT1+frontParser_midiMsg.data2);
						break;
						*/
						
						case SEQ_CHANGE_PAT:
						//pattern follow is always on
						if(1) {
						//if( parameter_values[PAR_FOLLOW] &= FOLLOW_PATTERN ) {
							
							// no more pattern settings page due to song mode
							//if( menu_activePage != PATTERN_SETTINGS_PAGE)
							{
								menu_setShownPattern(frontParser_midiMsg.data2);
								led_clearSequencerLeds();
#if 0
								//query current sequencer step states and light up the corresponding leds 
								uint8_t trackNr = menu_getActiveVoice(); //max 6 => 0x6 = 0b110
								uint8_t patternNr = menu_getViewedPattern(); //max 7 => 0x07 = 0b111
								uint8_t value = (uint8_t)((trackNr<<4) | (patternNr&0x7));
								frontPanel_sendData(LED_CC,LED_QUERY_SEQ_TRACK,value);
#else
								led_updateSeqLeds();
#endif
								//frontPanel_sendData(SEQ_CC,SEQ_REQUEST_PATTERN_PARAMS,frontParser_midiMsg.data2);

							}
							/*else {
								//store the pending pattern update for shift button release handler
								menu_shownPattern = frontParser_midiMsg.data2;
							}
							*/
						}	
						menu_playedPattern = frontParser_midiMsg.data2;						
						
						//if( (buttonHandler_getMode() == SELECT_MODE_PERF) || (buttonHandler_getMode() == SELECT_MODE_PAT_GEN) )
						if( (buttonHandler_getMode() == SELECT_MODE_PATTERN) )
						{
							//only show pattern changes when on Pattern select page
								
							led_clearSequencerLeds();
							//led_clearSelectLeds();
							led_clearAllBlinkLeds();
							// re init the LEDs shwoing active/viewed pattern
							led_initPatternSelectLeds();
							//led_setValue(1,LED_PART_SELECT1+frontParser_midiMsg.data2);
						}			
						
						
						break;
						case SEQ_RUN_STOP:
							// This tells the front that the sequencer has started/stopped due to MTC msg
							// we simply use this to turn on/off the led and cause the next press of start
							// button to act properly
							buttonHandler_setRunStopState(frontParser_midiMsg.data2);
							break;
						



					};						
				}
				else if(frontParser_midiMsg.status == SAMPLE_CC)
				{
					switch(frontParser_midiMsg.data1)
					{
						case SAMPLE_COUNT:
							menu_setNumSamples(frontParser_midiMsg.data2);
							break;

						default:
							break;
					}
				}
				else if(frontParser_midiMsg.status == LED_CC)
				{
					switch(frontParser_midiMsg.data1)
					{
						/*
						case LED_CURRENT_STEP_NR: {
							
							if(frontParser_midiMsg.data2 >=128) return;
							
							uint8_t shownPattern = menu_getViewedPattern();
							uint8_t playedPattern = menu_playedPattern;
							
							if(shownPattern == playedPattern) {
								//only update chaselight LED when it step edit mode
								if( (menu_activePage < MENU_MIDI_PAGE) || menu_activePage == PERFORMANCE_PAGE ||menu_activePage == SEQ_PAGE || menu_activePage == EUKLID_PAGE) {
									led_setActive_step(frontParser_midiMsg.data2);
								}							
							} else {
								led_clearActive_step();
							}								
															
						}							
						break;
						*/
						

						
						case LED_PULSE_BEAT:
						if(frontParser_midiMsg.data2!=0)
						{
							led_setValue(1,LED_START_STOP);
						}
						else
						{
							led_setValue(0,LED_START_STOP);
						}							
						break;
						
						case LED_SEQ_BUTTON:
						{
							if(menu_activePage != PERF_PAGE) //do not show active steps on perf. page
							{
								//limit to 16 steps
								uint8_t stepNr = (uint8_t)((frontParser_midiMsg.data2&0x7f)/8); //limit to 127
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
						
						break;
					}						
				}								
				else if(frontParser_midiMsg.status == NOTE_ON)
				{
					if(frontParser_midiMsg.data1 > 6) return;
					//only SELECT_MODE_VOICE and SELECT_MODE_MUTE
					led_pulseLed((uint8_t)(LED_VOICE1+frontParser_midiMsg.data1));
				}

			}				
		}
	}
};
//------------------------------------------------------------
void frontPanel_sendMidiMsg(MidiMsg msg)
{
#if AVR
	while(uart_putc(msg.status) == 0); 
	//data 1 - parameter number
	while(uart_putc(msg.data1) == 0);
	//data 2 - value	
	while(uart_putc(msg.data2) == 0);
#endif
};
//------------------------------------------------------------
#include <Soundengine/MIDI/CortexParser.h>
void frontPanel_sendData(uint8_t status, uint8_t data1, uint8_t data2)
{
#if AVR
	//we need atomic access, otherwise the LFO will interrupt message sending
	//ATOMIC_BLOCK(ATOMIC_RESTORESTATE)
	{
		while(uart_putc(status) == 0); 
		//data 1 - parameter number
		while(uart_putc(data1) == 0);
		//data 2 - value	
		while(uart_putc(data2) == 0);
	}
#else
	frontParser_parseUartData(status);
	frontParser_parseUartData(data1);
	frontParser_parseUartData(data2);
#endif
};
//------------------------------------------------------------
void frontPanel_sendByte(uint8_t data)
{
#if AVR
	while(uart_putc(data) == 0);
#endif
};
//------------------------------------------------------------
