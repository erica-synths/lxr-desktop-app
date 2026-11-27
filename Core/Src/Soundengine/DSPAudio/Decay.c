/*
 * Decay.c
 *
 *  Created on: 16.04.2012
 *
 * Copyright (C) 2012-2026 Erica Synths
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * This file is part of the LXR firmware. It is licensed under the GNU General
 * Public License version 3, with the additional permission and terms in
 * NOTICE.md. See LICENSE and NOTICE.md in the root of the repository.
 */



#include "Soundengine/DSPAudio/Decay.h"
#include "Soundengine/config.h"
#include <math.h>

//-------------------------------------------------
float DecayEg_calcSlopeValue(float val, float slope)
{
	return (1+slope)*val/(1+slope*fabsf(val));
}
//-------------------------------------------------
void DecayEg_init(DecayEg* eg)
{
	eg->decay 	= 0.01f;
	eg->value 	= 0;
};
//-------------------------------------------------
#define TIME_K (2*0.99f/(1.f-0.99f))
float DecayEg_calcTime(uint8_t data2)
{
	const float val = (data2)/127.f;
#if USE_BIG_SAMPLE_BUFFER
	//routine is called only half as often -> *2
	const float ret = (1.f-((1.f+TIME_K)*val/(1.f+TIME_K*fabsf(val)))) * 2.f;
	return ret > 1.f ? 1.f : ret;


#else
	return 1.f-((1.f+TIME_K)*val/(1.f+TIME_K*fabsf(val)));
#endif
}
//-------------------------------------------------
void DecayEg_trigger(DecayEg* eg)
{
	eg->value = 1.f;
};
//-------------------------------------------------
IN_ITCM_RAM float DecayEg_calc(DecayEg* eg)
{
	float val = eg->value;

	if(val>0)
	{
		val -= eg->decay;

	} else
	{
		val = 0;
	}
	eg->value = val;
	return DecayEg_calcSlopeValue(val,eg->slope);

};
//-------------------------------------------------
void DecayEg_setDecay(DecayEg* eg, uint8_t data2)
{
	eg->decay = DecayEg_calcTime(data2);
}
//--------------------------------------------------
IN_ITCM_RAM void DecayEg_setSlope(DecayEg* eg, uint8_t data2)
{
	float amount 		= ((data2/127.f)-0.5f)*2;
	// At 127 the amount is 1 and the slope would divide by zero.
	// Use the same top value as slopeEg2_setSlope() (0.5f*1.999f).
	if(amount > 0.9995f) amount = 0.9995f;
	eg->slope 			= 2*amount/(1-amount);
}
//--------------------------------------------------
