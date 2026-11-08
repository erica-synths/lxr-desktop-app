// SPDX-License-Identifier: GPL-3.0-only
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "menu/menu.h"
#include "frontPanelParser_ui.h"
#include "Soundengine/MIDI/MidiParser.h"

uint8_t activeParameters[NUM_PARAMS];
uint8_t mainParameters[NUM_KIT_SLOTS][END_OF_SOUND_PARAMETERS];
uint8_t *morphParameters = mainParameters[0];
const ModTarg modTargets[] = {{0, 0}};
static uint8_t morph_state, morph_index;
static unsigned seen[END_OF_SOUND_PARAMETERS], dispatched, audio_calls;

void midiParser_ccHandler(MidiMsg msg, uint8_t update, uint8_t floating, float v)
{
    (void)v;
    unsigned param;
    assert(update == 1 && floating == 0);
    if(msg.status == MIDI_CC) {
        assert(msg.data1 != CC_BANK_CHANGE);
        assert(msg.data1 != NRPN_DATA_ENTRY_COARSE);
        assert(msg.data1 != NRPN_FINE && msg.data1 != NRPN_COARSE);
        param = msg.data1 - 1;
    } else {
        assert(msg.status == CC_2);
        param = msg.data1 + 128;
    }
    assert(param < END_OF_SOUND_PARAMETERS);
    assert(msg.data2 == activeParameters[param]); // identical kits must stay identical
    seen[param]++;
    dispatched++;
}
void frontPanel_sendData(uint8_t status, uint8_t data1, uint8_t data2)
{
    (void)data1; (void)data2;
    assert(status == CC_VELO_TARGET || status == CC_LFO_TARGET);
}
void soundengine_process(void) { audio_calls++; }

// The runner inserts unmodified production function bodies here.
#include "sweep_functions.inc"

static void check_sweep(void)
{
    assert(dispatched == END_OF_SOUND_PARAMETERS - 4);
    for(unsigned i = 0; i < END_OF_SOUND_PARAMETERS; i++)
        assert(seen[i] == (preset_isTransportParameter(i) ? 0u : 1u));
    memset(seen, 0, sizeof(seen));
    dispatched = 0;
}

int main(void)
{
    // Nonzero transport slots ensure their values cannot silently act as commands.
    memset(activeParameters, 73, END_OF_SOUND_PARAMETERS);
    for(unsigned i = 0; i < 6; i++) {
        activeParameters[PAR_VEL_DEST_1 + i] = 0;
        activeParameters[PAR_TARGET_LFO1 + i] = 0;
        activeParameters[PAR_VOICE_LFO1 + i] = 1;
    }
    memcpy(morphParameters, activeParameters, END_OF_SOUND_PARAMETERS);
    for(unsigned morph = 0; morph < 256; morph++) {
        activeParameters[PAR_MORPH] = morph;
        preset_morph(morph);
        for(unsigned tick = 0; tick < END_OF_SOUND_PARAMETERS; tick++) {
            assert(morph_state && morph_index == tick);
            preset_morph_tick();
        }
        assert(!morph_state);
        preset_morph_tick(); // idle must not dispatch
        check_sweep();
    }
    // Repeated events retain the existing restart behavior and final convergence.
    for(unsigned event = 0; event < 16; event++) {
        preset_morph(255);
        for(unsigned tick = 0; tick < 110; tick++) preset_morph_tick();
    }
    memset(seen, 0, sizeof(seen)); dispatched = 0;
    preset_morph(255);
    while(morph_state) preset_morph_tick();
    check_sweep();

    preset_sendDrumsetParametersImmediate();
    check_sweep();
    assert(audio_calls == END_OF_SOUND_PARAMETERS);
    puts("PASS: all 256 Morph positions, unchanged tick/restart behavior, immediate kit sweep and audio service cadence");
}
