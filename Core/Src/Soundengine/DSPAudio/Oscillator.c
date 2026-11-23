/*
 * Oscillator.c
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


#include "Soundengine/DSPAudio/Oscillator.h"
#include "Soundengine/DSPAudio/random.h"
#include "Soundengine/DSPAudio/Samples.h"
#include "Soundengine/MIDI/MidiParser.h"
#include "Soundengine/MIDI/MidiNoteNumbers.h"
#include "Soundengine/Sequencer/sequencer.h"

static __inline uint32_t freq2PhaseIncr(const float f) //4096
{
	return (((TABLESIZE*f)/REAL_FS))*1048576 ; // 1048576 <-> (<<20)
}
//TODO die phaseInc berechnung kann man doch sicher per LUT machen!
//-----------------------------------------------------------
static __inline uint8_t fast_log2 (const uint32_t val)
{
	return 31-__CLZ(val);
}
//-----------------------------------------------------------
static __inline int freqToMidiNote(const float f)
{
	return (12*fast_log2((uint32_t)(f/440.f)))+70;
}
//-----------------------------------------------------------
static __inline uint8_t freqToTableIndex(const float f)
{
	return freqToMidiNote(f)/12;
}
//-----------------------------------------------------------

//-----------------------------------------------------------
static __inline uint32_t freq2PhaseIncr1024(const float f)
{
	return (((1024*f)/REAL_FS))*4194304 ; //(<<22)
}
//-----------------------------------------------------------
static __inline uint32_t freq2PhaseIncr32767(const float f)
{
	return (((1024*f)/REAL_FS))*131072 ; //(<<17)
}
//-----------------------------------------------------------
IN_ITCM_RAM void calcSineBlock(OscInfo* osc, int16_t* buf, const uint8_t size ,const float gain)
{
	//tablesize 4096 => 0b111111111111 -> 12 bits
	uint8_t i;
	for(i=0;i<size;i++)
	{
		const uint32_t oscPhase = osc->phase;
		int16_t oscOut;

		const uint32_t index =  oscPhase;
		uint32_t  itg	= index>>20; // 12 msb as itg index

		#if INTERPOLATE_OSC
			oscOut = sine_table[itg++];

			//const float frac = (index&0x7ffff)*0.0000019073486328125f;  // 1/0x7ffff = 0.0000019073486328125f
			//oscOut += frac*(sine_table[itg] - oscOut);
			//a + t * (b - a);
			uint32_t frac = index&0xFFFFF;
			oscOut = oscOut + (int16_t)(((int64_t)(sine_table[itg] - oscOut) * frac)   >>20);
		#else
			oscOut = sine_table[itg];
		#endif

		osc->phase = oscPhase + osc->phaseInc;

		buf[i] = oscOut * gain;
	}
}
//-----------------------------------------------------------
int16_t calcSine(OscInfo* osc)
{
	const uint32_t oscPhase = osc->phase;
	int16_t oscOut;

	const uint32_t index =  oscPhase;
	uint32_t  itg	= index>>20;

#if INTERPOLATE_OSC
	oscOut = sine_table[itg++];
	//float frac	= (index&0x7ffff)*0.0000019073486328125f;
	//oscOut += frac*(sine_table[itg] - oscOut);
	uint32_t frac = index&0xFFFFF;
	oscOut = oscOut + (int16_t)(((int64_t)(sine_table[itg] - oscOut) * frac)   >>20);
#else
	oscOut = sine_table[itg];
#endif

	osc->phase = oscPhase + osc->phaseInc;
	osc->output = oscOut;
	return oscOut;
}

//-----------------------------------------------------------
IN_ITCM_RAM void calcFmSineBlock(OscInfo* osc, int16_t* modBuffer, int16_t* buf, uint8_t size,const float gain)
{
	uint8_t i;
	for(i=0;i<size;i++)
	{
		const uint32_t oscPhase = osc->phase;
		int16_t oscOut ;

		const uint32_t index =  oscPhase + (((uint32_t)(modBuffer[i]*osc->fmMod))<<17);
		uint32_t  itg	= index>>20;

	#if INTERPOLATE_FM_OSC
		oscOut = sine_table[itg++];
		//const float frac	= (index&0x7ffff)*0.0000019073486328125f;
		//oscOut += frac*(sine_table[itg] - oscOut);
		uint32_t frac = index&0xFFFFF;
		oscOut = oscOut + (int16_t)(((int64_t)(sine_table[itg] - oscOut) * frac)   >>20);
	#else
		oscOut = sine_table[itg];
	#endif

		osc->phase = oscPhase + osc->phaseInc;
		buf[i] =  oscOut * gain;
	}
}
//-----------------------------------------------------------
int16_t calcFmSine(OscInfo* osc, OscInfo* modOsc)
{
	const uint32_t oscPhase 	= osc->phase;
	int16_t oscOut ;

	const uint32_t index =  oscPhase + (((uint32_t)(modOsc->output*osc->fmMod))<<17);
	uint32_t  itg	= index>>20;

#if INTERPOLATE_FM_OSC
	oscOut = sine_table[itg++];
	//const float frac	= (index&0x7ffff)*0.0000019073486328125f;
	//oscOut += frac*(sine_table[itg] - oscOut);
	uint32_t frac = index&0xFFFFF;
	oscOut = oscOut + (int16_t)(((int64_t)(sine_table[itg] - oscOut) * frac)   >>20);
#else
	oscOut = sine_table[itg];
#endif

	osc->phase = oscPhase + osc->phaseInc;
	osc->output = oscOut;
	return oscOut;
};
//---------------------------------------------------------------
IN_ITCM_RAM void calcFmBlock(OscInfo* osc, const int16_t table[][1024], int16_t* modBuffer, int16_t* buf, uint8_t size ,const float gain)
{
	uint8_t i;
	for(i=0;i<size;i++)
	{
		const uint32_t oscPhase = osc->phase;
		int16_t oscOut ;
		const uint32_t index =  oscPhase + (((uint32_t)(modBuffer[i]*osc->fmMod))<<19);

		uint32_t  itg	= index>>22; //10 bit index, 22 bit fractional part

	#if INTERPOLATE_FM_OSC
		oscOut = table[osc->tableOffset][itg];
		/* The row is 1024 long and the phase wraps, so the sample after 1023 is 0
		   of the same row, not index 1024 of the next one. */
		itg = (itg + 1u) & 0x3FFu;
		//const float frac	= (index&0x003FFFFF)*2.38418579101562e-07f;
		//oscOut += frac*(table[osc->tableOffset][itg] - oscOut);
		uint32_t frac = index & 0x3FFFFF;
		oscOut = oscOut + (int16_t)(((int64_t)(table[osc->tableOffset][itg] - oscOut) * frac)   >>22);

	#else
		oscOut = table[overtoneIndex][itg];
	#endif

		osc->phase = oscPhase + osc->phaseInc;
		osc->output = oscOut;
		buf[i] = oscOut * gain;
	}
}
//-----------------------------------------------------------
void calcPwmFmBlock(OscInfo* osc, int16_t* modBuffer, int16_t* buf, uint8_t size ,const float gain, const float pwm)
{
	uint8_t i;
	for(i=0;i<size;i++)
	{
		const uint32_t oscPhase = osc->phase;
		int16_t oscOut ;
		const uint32_t index =  oscPhase + (((uint32_t)(modBuffer[i]*osc->fmMod))<<19);
		const uint32_t index2 = index + pwm * (UINT32_MAX/2);

		 uint32_t  itg	 = index>>22;
		 uint32_t  itg2 = index2>>22;

	#if INTERPOLATE_FM_OSC
		oscOut = sawTable[osc->tableOffset][itg];
		/* The row is 1024 long and the phase wraps, so the sample after 1023 is 0
		   of the same row, not index 1024 of the next one. */
		itg = (itg + 1u) & 0x3FFu;
		//const float frac	= (index&0x003FFFFF)*2.38418579101562e-07f;
		//oscOut += frac*(sawTable[osc->tableOffset][itg] - oscOut);
		uint32_t frac = index & 0x3FFFFF;
		oscOut = oscOut + (int16_t)(((int64_t)(sawTable[osc->tableOffset][itg] - oscOut) * frac)   >>22);

		int16_t osc2Out = sawTable[osc->tableOffset][itg2];// * -1;
		/* The row is 1024 long and the phase wraps, so the sample after 1023 is 0
		   of the same row, not index 1024 of the next one. */
		itg2 = (itg2 + 1u) & 0x3FFu;
		//const float frac	= (index&0x003FFFFF)*2.38418579101562e-07f;
		frac = index2 & 0x3FFFFF;
		//osc2Out += frac*(sawTable[osc->tableOffset][itg2] * -1 - osc2Out);
		osc2Out = osc2Out + (int16_t)(((int64_t)(sawTable[osc->tableOffset][itg2] - osc2Out) * frac)   >>22);

		oscOut =  __QSUB16(oscOut,osc2Out);


	#else
		oscOut = sawTable[overtoneIndex][itg];
	#endif

		osc->phase = oscPhase + osc->phaseInc;
		osc->output = oscOut;
		buf[i] = oscOut * gain;
	}
}
//-----------------------------------------------------------
int16_t calcFm(OscInfo* osc, OscInfo* modOsc, const int16_t table[][1024])
{

	const uint32_t oscPhase = osc->phase;
	int16_t oscOut ;

	const uint32_t index =  oscPhase + (((uint32_t)(modOsc->output*osc->fmMod))<<19);

	uint32_t  itg	= index>>22;

#if INTERPOLATE_FM_OSC
	oscOut = table[osc->tableOffset][itg];
	/* The row is 1024 long and the phase wraps, so the sample after 1023 is 0
	   of the same row, not index 1024 of the next one. */
	itg = (itg + 1u) & 0x3FFu;
	//const float frac	= (index&0x003FFFFF)*2.38418579101562e-07f;
	//oscOut += frac*(table[osc->tableOffset][itg] - oscOut);
	uint32_t frac = index & 0x3FFFFF;
	oscOut = oscOut + (int16_t)(((int64_t)(table[osc->tableOffset][itg] - oscOut) * frac)   >>22);
#else
	oscOut = table[overtoneIndex][itg];
#endif

	osc->phase = oscPhase + osc->phaseInc;
	osc->output = oscOut;
	return oscOut;
}
//-----------------------------------------------------------
IN_ITCM_RAM void calcNextOscSampleFmBlock(OscInfo* osc, int16_t* modBuffer, int16_t* buf, uint8_t size ,const float gain, const float pwm)
{
	switch(osc->waveform)
		{
		case SINE:
			calcFmSineBlock(osc,modBuffer,buf,size,gain);
			break;

		case SAW:
			calcFmBlock(osc,sawTable,modBuffer,buf,size,gain);
			break;

		case TRI:
			calcFmBlock(osc, triTable, modBuffer,buf,size,gain);
			break;

		case REC:
			calcFmBlock(osc, recTable, modBuffer,buf,size,gain);
			break;

		case NOISE:
			//TODO FM noise - needed since its tuned digi noise!
			calcNoiseBlock(osc,buf,size,gain);
			break;

		case PWM:
			calcPwmFmBlock(osc,modBuffer,buf,size,gain, pwm);//calcSampleOscFmBlock(osc,modBuffer,buf,size,gain);
			break;

		default:
			calcUserSampleOscFmBlock(osc,modBuffer,buf,size,gain);
			break;

		}
}
//-----------------------------------------------------------
/*
int16_t calcNextOscSampleFm(OscInfo* osc,OscInfo* modOsc)
{
	switch(osc->waveform)
	{
	case SINE:
		return calcFmSine(osc,modOsc);
		break;

	case SAW:
		return calcFm(osc,modOsc,sawTable);
		break;

	case TRI:
		return calcFm(osc,modOsc, triTable);
		break;

	case REC:
		return calcFm(osc,modOsc,recTable);
		break;

	case NOISE:
		return calcNoise(osc);
		break;

	case PWM:
		return calcSampleOscFm(osc,modOsc);
		break;

	default:

		return 0;
		break;

	}
	return 0;
}
*/
//-----------------------------------------------------------
IN_ITCM_RAM void calcNoiseBlock(OscInfo* osc, int16_t* buf, const uint8_t size ,const float gain)
{
	int i;
	for(i=0;i<size;i++)
	{
		const uint32_t lastPhase = osc->phase;
		osc->phase += osc->phaseInc;

		if(lastPhase > osc->phase)
		{
			//overflow happened -> phaseWrapped
			osc->output = GetRngValue(); //normal pitched white noise
		}

		buf[i] = osc->output * gain;
	}
}
//-----------------------------------------------------------
int16_t calcNoise(OscInfo* osc)
{
	osc->phase += osc->phaseInc;

	//check overflow flag, ( if osc->phase == osc->phaseInc a reset occured ==> retrigger, too
	APSR_Type apsr;
	apsr.w = __get_APSR();
	if(apsr.b.V ||  (osc->phase == osc->phaseInc))
	{
		//overflow happened -> phaseWrapped

		uint16_t rnd = GetRngValue();
		if( rnd > 0x00ff)
		{
			if( rnd > 0x000f)
			{
				osc->output = 32767;
			}
			else
			{
				osc->output = -32768;
			}

		}
	}
	else
	{
			osc->output = 0;
	}
	return osc->output;
}
//-----------------------------------------------------------
IN_ITCM_RAM void calcNextOscSampleBlock(OscInfo* osc, int16_t* buf, const uint8_t size, const float gain, const float pwm)
{
	switch(osc->waveform)
		{
		case SINE:
			calcSineBlock(osc,buf,size, gain);
			break;

		case SAW:
			calcWavetableOscBlock(osc,sawTable,buf,size, gain);
			break;

		case TRI:
			calcWavetableOscBlock(osc,triTable,buf,size, gain);
			break;

		case REC:
			calcWavetableOscBlock(osc,recTable,buf,size, gain);
			break;

		case NOISE:
			calcNoiseBlock(osc,buf,size, gain);
			break;

		case PWM:
			calcPwmBlock(osc,buf,size, gain, pwm);
			break;

		default://sample playback
		//	if( osc->waveform - OSC_SAMPLE_START > sampleMemory_getNumSamples() ) return; // return if out of range

		//	calcUserSampleOscBlock(osc,buf,size, gain);
			break;
		}
}
//-----------------------------------------------------------
/*
int16_t calcNextOscSample(OscInfo* osc)
{
	switch(osc->waveform)
	{
	case SINE:
		return calcSine(osc);
		break;

	case SAW:
		return calcWavetableOsc(osc,sawTable);
		break;

	case TRI:
		return calcWavetableOsc(osc,triTable);
		break;

	case REC:
		return calcWavetableOsc(osc,recTable);
		break;

	case NOISE:
		return calcNoise(osc);
		break;

	case CRASH:
		return calcSampleOsc(osc);
		break;

	default:

		return 0;
		break;

	}
	return 0;
};
*/
//---------------------------------------------------------------
IN_ITCM_RAM void calcWavetableOscBlock(OscInfo* osc, const int16_t table[][1024], int16_t* buf, const uint8_t size ,const float gain)
{
	uint8_t i;
	for(i=0;i<size;i++)
	{
		const uint32_t oscPhase = osc->phase;
			int16_t oscOut ;
			uint32_t  itg	= oscPhase>>22;
		#if INTERPOLATE_OSC
			//todo use ldm instead of ldr to load multiple values from memory
			oscOut = table[osc->tableOffset][itg];
			//const float frac = (itg&0x003FFFFF)*2.38418579101562e-07f; //2.38... => 1.f/0x3fffff
			//oscOut += frac*(table[osc->tableOffset][itg] - oscOut);
			/* The phase wraps, so the sample after 1023 is 0 of the same row.
			   The post-increment this replaces read index 1024 of a
			   1024-element row: for every row but the last that is the first
			   sample of the *next* waveform, and for the last it is past the
			   table. */
			itg = (itg + 1u) & 0x3FFu;
			uint32_t frac = oscPhase & 0x3FFFFF;
			oscOut = oscOut + (int16_t)(((int64_t)(table[osc->tableOffset][itg] - oscOut) * frac)   >>22);
		#else
			oscOut = table[osc->tableOffset][itg];
		#endif

			osc->phase = oscPhase + osc->phaseInc;
			buf[i] = oscOut * gain;
	}
}

//---------------------------------------------------------------
int16_t calcWavetableOsc(OscInfo* osc,  const int16_t table[][1024])
{
	const uint32_t oscPhase = osc->phase;
	int16_t oscOut ;

	uint32_t  itg	= oscPhase>>22;

#if INTERPOLATE_OSC
	//todo use ldm instead of ldr to laod multiple values from memory
	oscOut = table[osc->tableOffset][itg];
	/* The row is 1024 long and the phase wraps, so the sample after 1023 is 0
	   of the same row, not index 1024 of the next one. */
	itg = (itg + 1u) & 0x3FFu;
	//const float frac = (itg&0x003FFFFF)*2.38418579101562e-07f; //2.38... => 1.f/0x3fffff
	//oscOut += frac*(table[osc->tableOffset][itg] - oscOut);
	uint32_t frac = oscPhase & 0x3FFFFF;
	oscOut = oscOut + (int16_t)(((int64_t)(table[osc->tableOffset][itg] - oscOut) * frac)   >>22);
#else
	oscOut = table[osc->tableOffset][itg];
#endif

	osc->phase = oscPhase + osc->phaseInc;
	osc->output = oscOut;
	return oscOut;

};

//------------------------------------------------------------------
/*
void calcSampleOscFmBlock(OscInfo* osc,int16_t* modBuffer, int16_t* buf, uint8_t size ,const float gain)
{
	uint8_t i;
	for(i=0;i<size;i++)
	{
		const uint32_t oscPhase 	= osc->phase;
		int16_t oscOut ;

		const uint32_t index =  oscPhase + (((uint32_t)(modBuffer[i]*osc->fmMod))<<14);
		uint32_t  itg	= index>>17;

	#if INTERPOLATE_FM_OSC
		oscOut = crashSample[itg++];
		const float frac	= (index&20000)*0.00000762939453125f;//=> * 1/0x20000
		oscOut += frac*(crashSample[itg] - oscOut);
	#else
		oscOut = crashSample[itg];
	#endif

		oscOut = (oscOut - 127)*256;

		osc->phase = oscPhase + osc->phaseInc;
		osc->output = oscOut;
		buf[i] = oscOut * gain;
	}
}
*/
//------------------------------------------------------------------
/*
int16_t calcSampleOscFm(OscInfo* osc, OscInfo* modOsc)
{

	const uint32_t oscPhase 	= osc->phase;
	int16_t oscOut ;

	const uint32_t index =  oscPhase + (((uint32_t)(modOsc->output*osc->fmMod))<<14);
	uint32_t  itg	= index>>17;

#if INTERPOLATE_FM_OSC
	oscOut = crashSample[itg++];
	const float frac	= (index&20000)*0.00000762939453125f;//=> * 1/0x20000
	oscOut += frac*(crashSample[itg] - oscOut);
#else
	oscOut = crashSample[itg];
#endif

	oscOut = (oscOut - 127)*256;

	osc->phase = oscPhase + osc->phaseInc;
	osc->output = oscOut;
	return oscOut;
};
*/
//------------------------------------------------------------------
void calcUserSampleOscFmBlock(OscInfo* osc,int16_t* modBuffer, int16_t* buf, uint8_t size ,const float gain)
{
#if 0
	//TODO optimize performance by moving sampleInfo to oscillator struct
	SampleInfo info = sampleMemory_getSampleInfo(osc->waveform - OSC_SAMPLE_START);

	//cast sample data to signed int16_t array
	int16_t* sampleData = (int16_t*)((int8_t*)(info.offset));

	uint8_t i;
	for(i=0;i<size;i++)
	{
		const uint32_t oscPhase 	= osc->phase;
		int16_t oscOut ;

		const uint32_t index =  oscPhase + (((uint32_t)(modBuffer[i]*osc->fmMod))<<14);
		uint32_t  itg	= index>>17;

	#if INTERPOLATE_FM_OSC
		oscOut = sampleData[itg++];
		const float frac	= (index&20000)*0.00000762939453125f;//=> * 1/0x20000
		oscOut += frac*(sampleData[itg] - oscOut);
	#else
		oscOut = sampleData[itg];
	#endif

		//one shot
		if(itg < info.size)
		{
			osc->phase = oscPhase + osc->phaseInc;
		}
		osc->output = oscOut;
		buf[i] = oscOut * gain;
	}
#endif
}
//---------------------------------------------------------------
void calcUserSampleOscBlock(OscInfo* osc, int16_t* buf, const uint8_t size ,const float gain)
{
#if 0
	//TODO optimize performance by moving sampleInfo to oscillator struct
	SampleInfo info = sampleMemory_getSampleInfo(osc->waveform - OSC_SAMPLE_START);

	//cast sample data to signed int16_t array
	int16_t* sampleData = (int16_t*)((int8_t*)(info.offset));

	uint8_t i;
	for(i=0;i<size;i++)
	{
		const uint32_t oscPhase = osc->phase;
		int16_t oscOut ;
		uint32_t  itg	= oscPhase>>17;


	#if INTERPOLATE_OSC

		oscOut = sampleData[itg++];
		const float frac = (oscPhase&20000)*0.00000762939453125f;//=> * 1/0x20000
		oscOut += frac*(sampleData[itg] - oscOut);
	#else
		oscOut = sampleData[itg];
	#endif
	//	oscOut = (oscOut - 127)*256;

		//one shot
		if(itg < info.size)
		{
			osc->phase = oscPhase + osc->phaseInc;
		}
		buf[i] = oscOut * gain;
	}
#endif
}
//---------------------------------------------------------------
/*
void calcSampleOscBlock(OscInfo* osc, int16_t* buf, const uint8_t size ,const float gain)
{
	uint8_t i;
	for(i=0;i<size;i++)
	{
		const uint32_t oscPhase = osc->phase;
		int16_t oscOut ;
		uint32_t  itg	= oscPhase>>17;


	#if INTERPOLATE_OSC
		//todo use ldm instead of ldr to laod multiple values from memory
		oscOut = crashSample[itg++];
		const float frac = (oscPhase&20000)*0.00000762939453125f;//=> * 1/0x20000
		oscOut += frac*(crashSample[itg] - oscOut);
	#else
		oscOut = crashSample[itg];
	#endif
		oscOut = (oscOut - 127)*256;

		osc->phase = oscPhase + osc->phaseInc;
		buf[i] = oscOut * gain;
	}
}
*/
//-----------------------------------------------------------
IN_ITCM_RAM void calcPwmBlock(OscInfo* osc, int16_t* buf, uint8_t size ,const float gain, const float pwm)
{



	uint8_t i;
	for(i=0;i<size;i++)
	{
		const uint32_t oscPhase = osc->phase;
		const uint32_t oscPhase2 = osc->phase + pwm * (UINT32_MAX/2);
		int16_t oscOut ;
		uint32_t  itg	= oscPhase>>22;
		uint32_t  itg2	= oscPhase2>>22;


		#if INTERPOLATE_FM_OSC
			oscOut = sawTable[osc->tableOffset][itg];
			/* The row is 1024 long and the phase wraps, so the sample after 1023 is 0
			   of the same row, not index 1024 of the next one. */
			itg = (itg + 1u) & 0x3FFu;
			//const float frac	= (oscPhase&0x003FFFFF)*2.38418579101562e-07f;
			//const float frac = (itg&0x003FFFFF)*2.38418579101562e-07f;
			//oscOut += frac*(sawTable[osc->tableOffset][itg] - oscOut);
			uint32_t frac = oscPhase & 0x3FFFFF;
			oscOut = oscOut + (int16_t)(((int64_t)(sawTable[osc->tableOffset][itg] - oscOut) * frac)   >>22);

			int16_t osc2Out = sawTable[osc->tableOffset][itg2] ;//* -1;
			/* The row is 1024 long and the phase wraps, so the sample after 1023 is 0
			   of the same row, not index 1024 of the next one. */
			itg2 = (itg2 + 1u) & 0x3FFu;
			//const float frac	= (index&0x003FFFFF)*2.38418579101562e-07f;
			//osc2Out += frac*(sawTable[osc->tableOffset][itg2] * -1 - osc2Out);
			frac = oscPhase2 & 0x3FFFFF;
			osc2Out = osc2Out + (int16_t)(((int64_t)(sawTable[osc->tableOffset][itg2] - osc2Out) * frac)   >>22);

			oscOut =  __QSUB16(oscOut,osc2Out);

		#else
			oscOut = sawTable[overtoneIndex][itg];
		#endif

			osc->phase = oscPhase + osc->phaseInc;
			osc->output = oscOut;
			buf[i] = oscOut * gain;
	}
}
//---------------------------------------------------------------
/*
int16_t calcSampleOsc(OscInfo* osc)
{
	const uint32_t oscPhase = osc->phase;
	int16_t oscOut ;

	uint32_t  itg	= oscPhase>>17;

#if INTERPOLATE_OSC
	//todo use ldm instead of ldr to laod multiple values from memory
	oscOut = crashSample[itg++];
	const float frac = (oscPhase&20000)*0.00000762939453125f;//=> * 1/0x20000
	oscOut += frac*(crashSample[itg] - oscOut);
#else
	oscOut = crashSample[itg];
#endif
	oscOut = (oscOut - 127)*256;

	osc->phase = oscPhase + osc->phaseInc;
	osc->output = oscOut;
	return oscOut;

};
*/
 //-----------------------------------------------------------
void osc_calcSineFreq(OscInfo* osc)
{
	osc->phaseInc = freq2PhaseIncr(osc->freq* osc->pitchMod*osc->modNodeValue);
}
//-----------------------------------------------------------
void osc_calcNoiseFreq(OscInfo* osc)
{
	osc_calcSineFreq(osc);
}
//-----------------------------------------------------------
void osc_calcWavetableFreq(OscInfo* osc)
{
	const float currentFreq = osc->freq*osc->pitchMod*osc->modNodeValue;
	osc->phaseInc = freq2PhaseIncr1024(currentFreq);

	const uint8_t overtoneIndex = freqToTableIndex(currentFreq);
	osc->tableOffset = overtoneIndex>10?10:overtoneIndex;
}
//-----------------------------------------------------------
void osc_calcSampleFreq(OscInfo* osc)
{
	const float currentFreq = osc->freq*osc->pitchMod*osc->modNodeValue;
	osc->phaseInc = freq2PhaseIncr32767(currentFreq);
}
//-----------------------------------------------------------
void osc_calcUserSampleFreq(OscInfo* osc)
{
	const float currentFreq = osc->freq*osc->pitchMod*osc->modNodeValue;
	osc->phaseInc = freq2PhaseIncr32767(currentFreq);
}
//-----------------------------------------------------------
 void osc_setFreq(OscInfo* osc)
 {
		switch(osc->waveform)
		{
		case SINE:
			osc_calcSineFreq(osc);
			break;

		case SAW:
			osc_calcWavetableFreq(osc);
			break;

		case TRI:
			osc_calcWavetableFreq(osc);
			break;

		case REC:
			osc_calcWavetableFreq(osc);
			break;

		case NOISE:
			osc_calcNoiseFreq(osc);
			break;


		case PWM:
			osc_calcWavetableFreq(osc);
			break;

		//samples
		default:
			osc_calcUserSampleFreq(osc);
			break;

		}
 }
 //-----------------------------------------------------------
 void osc_setBaseNote(OscInfo* osc, uint8_t baseNote)
 {

	 //get fine tune
	 const float cent = midiParser_calcDetune(osc->midiFreq&0xff);
	 //calc coarse tune
	 int16_t note =  (osc->midiFreq>>8) + (baseNote-SEQ_DEFAULT_NOTE);
	 if(note>127)note=127;
	 if(note<0)note=0;

	 osc->freq = MidiNoteFrequencies[note]*cent;
	 osc->baseNote = baseNote;
 };

 //-----------------------------------------------------------
 IN_ITCM_RAM void osc_recalcFreq(OscInfo* osc)
 {
	 //get fine tune
	 const float cent = midiParser_calcDetune(osc->midiFreq&0xff);
	 //calc coarse tune
	 int16_t note =  (osc->midiFreq>>8) + (osc->baseNote-SEQ_DEFAULT_NOTE);

	 if(note>127)note=127;
 	 if(note<0)note=0;

	 osc->freq = MidiNoteFrequencies[note]*cent;
 }
 //-----------------------------------------------------------
