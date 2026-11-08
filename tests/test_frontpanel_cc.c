/*
 * test_frontpanel_cc.c - MIDI CCs forwarded to the front-panel parser
 * (frontPanelParser.c), in particular bank select (CC 0) kit changes
 *
 * Copyright (C) 2026 Erica Synths
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * This file is part of the LXR firmware. It is licensed under the GNU General
 * Public License version 3, with the additional permission and terms in
 * NOTICE.md. See LICENSE and NOTICE.md in the root of the repository.
 */

#include "lxr_test.h"
#include "frontPanelParser_ui.h"
#include "menu/menu.h"
#include "Preset/PresetManager.h"

// --- Stubs for what frontPanelParser.c calls ------------------------------

uint8_t activeParameters[NUM_PARAMS];
uint8_t menu_activePage;
uint8_t menu_playedPattern;
uint8_t menu_selectedPresetNr[NUM_PRESET_LOCATIONS];
uint8_t paramToModTarget[END_OF_SOUND_PARAMETERS];

static int loadCalls, loadedKit, kitTimerTouched, repaints;

void preset_loadDrumsetRam(uint8_t presetNr, uint8_t isMorph, uint8_t sendImmediate)
{
	loadCalls++;
	loadedKit = presetNr;
	CHECK_EQ(isMorph, 0);
	// The real function copies mainParameters[presetNr]: must be a valid slot.
	CHECK(presetNr < NUM_KIT_SLOTS);
}
void preset_touchTimer(PresetTimer type)
{
	if (type == PRESET_KIT_TIMER)
		kitTimerTouched++;
}
void menu_repaint() { repaints++; }

uint8_t buttonHandler_getMode() { return 0; }
void buttonHandler_muteVoice(uint8_t voice, uint8_t isMuted) {}
void buttonHandler_setRunStopState(uint8_t running) {}
void frontParser_parseUartData(unsigned char data) {}
void led_clearAllBlinkLeds() {}
void led_clearSequencerLeds() {}
void led_initPatternSelectLeds() {}
void led_pulseLed(uint8_t ledNr) {}
void led_setValue(uint8_t val, uint8_t ledNr) {}
void led_updateSeqLeds() {}
void menu_repaintAll() {}
void menu_setNumSamples(uint8_t num) {}
void menu_setShownPattern(uint8_t patternNr) {}

// --- Helpers -------------------------------------------------------------------

static void reset(void)
{
	loadCalls = loadedKit = kitTimerTouched = repaints = 0;
	menu_selectedPresetNr[0] = 0xee;
	memset(activeParameters, 0, sizeof activeParameters);
}

// What MidiParser.c does with a received CC: forward its three bytes.
static void receiveCc(uint8_t channel, uint8_t cc, uint8_t value)
{
	frontPanel_parseData((uint8_t)(0xb0 | channel));
	frontPanel_parseData(cc);
	frontPanel_parseData(value);
}

// --- Tests -----------------------------------------------------------------------

static void test_bank_select_loads_the_kit(void)
{
	reset();
	receiveCc(0, 0, 5);
	CHECK_EQ(loadCalls, 1);
	CHECK_EQ(loadedKit, 5);
	CHECK_EQ(menu_selectedPresetNr[0], 5);
	CHECK_EQ(kitTimerTouched, 1);
	CHECK(repaints > 0);
}

static void test_bank_select_accepts_every_kit_slot(void)
{
	for (int kit = 0; kit < NUM_KIT_SLOTS; kit++) {
		reset();
		receiveCc(3, 0, (uint8_t)kit);
		CHECK_EQ(loadCalls, 1);
		CHECK_EQ(loadedKit, kit);
	}
}

// There are 64 kits but CC values go up to 127: values past the last kit wrap
// around (64 = kit 0, ..., 127 = kit 63).
static void test_bank_select_wraps_values_past_the_last_kit(void)
{
	for (int value = NUM_KIT_SLOTS; value <= 127; value++) {
		reset();
		receiveCc(0, 0, (uint8_t)value);
		CHECK_EQ(loadCalls, 1);
		CHECK_EQ(loadedKit, value - NUM_KIT_SLOTS);
		CHECK_EQ(menu_selectedPresetNr[0], value - NUM_KIT_SLOTS);
		CHECK_EQ(kitTimerTouched, 1);
	}
}

// Whatever the value, the kit passed on and the selected kit are valid slots.
static void test_bank_select_never_leaves_the_kit_range(void)
{
	for (int value = 0; value <= 127; value++) {
		reset();
		receiveCc(2, 0, (uint8_t)value);
		CHECK_EQ(loadCalls, 1);
		CHECK(loadedKit >= 0 && loadedKit < NUM_KIT_SLOTS);
		CHECK(menu_selectedPresetNr[0] < NUM_KIT_SLOTS);
	}
}

// Bank select must not be treated as a parameter.
static void test_bank_select_doesnt_change_parameters(void)
{
	reset();
	receiveCc(0, 0, 7);
	for (int i = 0; i < NUM_PARAMS; i++)
		CHECK_EQ(activeParameters[i], 0);
}

// Other CCs still set front-panel parameter CC-1 (CC 2 = parameter 1, ...).
static void test_other_ccs_set_parameters(void)
{
	reset();
	receiveCc(0, 2, 99);
	CHECK_EQ(activeParameters[1], 99);
	CHECK_EQ(loadCalls, 0);
}

int main(void)
{
	RUN_TEST(test_bank_select_loads_the_kit);
	RUN_TEST(test_bank_select_accepts_every_kit_slot);
	RUN_TEST(test_bank_select_wraps_values_past_the_last_kit);
	RUN_TEST(test_bank_select_never_leaves_the_kit_range);
	RUN_TEST(test_bank_select_doesnt_change_parameters);
	RUN_TEST(test_other_ccs_set_parameters);
	return TEST_REPORT();
}
