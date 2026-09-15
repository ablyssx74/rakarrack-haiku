/*
 * Copyright 2026, Kris Beazley hakarrack@epluribusunix.net
 * All rights reserved. Distributed under the terms of the MIT license.
 *
 * Native Haiku GUI for Rakarrack.
 *
 * This mirrors the layout of the FLTK GUI (src/rakarrack.cxx) using native
 * BeAPI/Haiku widgets: one "rack box" per effect, each with an On/Off
 * checkbox and the effect's own sliders, laid out in a few scrollable
 * columns. All effect parameters are read/written exclusively through each
 * effect's public changepar()/getpar() API (or the equivalent
 * *_Change()/getpar() pair used by Compressor and Gate) -- exactly how
 * src/rakarrack.cxx itself talks to the engine. No engine header had to be
 * modified to make this work.
 */

#include <app/Looper.h>
#include <BufferProducer.h>
#include <Application.h>
#include <Message.h>
#include <Archivable.h>
#include <TimeSource.h>
#include <MediaEventLooper.h>

#include <OS.h>
#include <syslog.h>
#include <math.h>
#include <Alert.h>

#include <Alignment.h>
#include <InterfaceDefs.h>
#include <LayoutBuilder.h>
#include <Box.h>
#include <Button.h>
#include <CheckBox.h>
#include <Entry.h>
#include <FilePanel.h>
#include <Font.h>
#include <ListItem.h>
#include <ListView.h>
#include <Messenger.h>
#include <Path.h>
#include <String.h>
#include <GroupView.h>
#include <MenuBar.h>
#include <MenuField.h>
#include <MenuItem.h>
#include <Message.h>
#include <PopUpMenu.h>
#include <Menu.h>
#include <ScrollView.h>
#include <Size.h>
#include <SpaceLayoutItem.h>
#include <StringView.h>
#include <Slider.h>
#include <StringView.h>
#include <StatusBar.h>
#include <stdio.h>
#include <SupportDefs.h>
#include <Window.h>
#include <View.h>
#include <pthread.h>

#include <functional>
#include <string>
#include <deque>
#include <vector>

// Pulls in the full RKR engine class (global.h) so we can call the real
// effect objects' public parameter APIs directly -- the same interface
// src/rakarrack.cxx itself uses. Nothing in this file touches a private
// member of any effect class.
#include "../src/global.h"

#include "../src/rakarrack_haiku_bridge.h"

// See rakarrack_haiku_bridge.h's own comment on the declaration -- defined
// here (not main.C) purely so every target that links this file, extra/'s
// tiny utilities included, has a real definition without needing a weak
// fallback for it too.
bool gAppQuitting = false;

// This "weak" function satisfies the linker for small utilities
// like rakverb, but gets overridden by the real one in the main app.
__attribute__((weak)) void RKR::calculavol(int i) { }

// haiku_native/haiku-rakarrack.o gets linked into more than just the main
// "rakarrack" binary: the automake build also links it into the small
// standalone utilities in extra/ (rakverb, rakverb2, rakconvert,
// rakgit2new) via the shared $(LIBS) variable in haiku.makefile. Those
// utilities only build their own tiny .C file plus this one -- none of
// src/*.o (Distorsion.o, Echo.o, ...) is part of their link. Since this
// file now drives every effect through its real changepar()/getpar() (or
// Compressor_Change()/Gate_Change()) API instead of poking members
// directly, the linker needs *something* to resolve those symbols to when
// building those utilities. These weak fallbacks are never actually
// reached at runtime there (those tools never call
// start_haiku_native_interface()); the strong definitions in src/*.C
// silently take over whenever this is linked into the real app, exactly
// like the RKR::calculavol() stub above already does.
__attribute__((weak)) void RKR::cleanup_efx() { }

// Same story for loadfile()/savefile(): the real implementations live in
// src/fileio.C (never linked into the small extra/ utilities), but this
// file now calls them directly from the Save Preset/Load Preset buttons.
__attribute__((weak)) void RKR::loadfile(char *filename) { }
__attribute__((weak)) void RKR::savefile(char *filename) { }

// MIDIConverter (the guitar-to-MIDI "MIDI" panel in the header, see
// BuildHeader) isn't part of the changepar()/getpar() effect chain either --
// same reasoning, same fix.
__attribute__((weak)) void MIDIConverter::setmidichannel(int) { }
__attribute__((weak)) void MIDIConverter::panic() { }
__attribute__((weak)) void MIDIConverter::setTriggerAdjust(int) { }
__attribute__((weak)) void MIDIConverter::setVelAdjust(int) { }

#define RKR_HAIKU_WEAK_CHANGEPAR(EffectClass) \
	__attribute__((weak)) void EffectClass::changepar(int, int) { } \
	__attribute__((weak)) int EffectClass::getpar(int) { return 0; }

RKR_HAIKU_WEAK_CHANGEPAR(Distorsion)
RKR_HAIKU_WEAK_CHANGEPAR(NewDist)
RKR_HAIKU_WEAK_CHANGEPAR(Echo)
RKR_HAIKU_WEAK_CHANGEPAR(Reverb)
RKR_HAIKU_WEAK_CHANGEPAR(EQ)
RKR_HAIKU_WEAK_CHANGEPAR(Chorus)
RKR_HAIKU_WEAK_CHANGEPAR(Phaser)
RKR_HAIKU_WEAK_CHANGEPAR(Analog_Phaser)
RKR_HAIKU_WEAK_CHANGEPAR(DynamicFilter)
RKR_HAIKU_WEAK_CHANGEPAR(Alienwah)
RKR_HAIKU_WEAK_CHANGEPAR(Valve)
RKR_HAIKU_WEAK_CHANGEPAR(Ring)
RKR_HAIKU_WEAK_CHANGEPAR(Sustainer)
RKR_HAIKU_WEAK_CHANGEPAR(StompBox)
RKR_HAIKU_WEAK_CHANGEPAR(Exciter)
RKR_HAIKU_WEAK_CHANGEPAR(Vibe)
RKR_HAIKU_WEAK_CHANGEPAR(Opticaltrem)
RKR_HAIKU_WEAK_CHANGEPAR(Pan)

// Added when the remaining 25 effects were wired into native mode.
// (Cabinet and EQ2 are both actually "class EQ *" in global.h -- see
// efx_Cabinet/efx_EQ2 -- so they already resolve via RKR_HAIKU_WEAK_CHANGEPAR(EQ)
// above and don't need their own entry here.)
RKR_HAIKU_WEAK_CHANGEPAR(Convolotron)
RKR_HAIKU_WEAK_CHANGEPAR(Looper)
RKR_HAIKU_WEAK_CHANGEPAR(Sequence)
RKR_HAIKU_WEAK_CHANGEPAR(StereoHarm)
RKR_HAIKU_WEAK_CHANGEPAR(MBVvol)
RKR_HAIKU_WEAK_CHANGEPAR(CoilCrafter)
RKR_HAIKU_WEAK_CHANGEPAR(Reverbtron)
RKR_HAIKU_WEAK_CHANGEPAR(MusicDelay)
RKR_HAIKU_WEAK_CHANGEPAR(CompBand)
RKR_HAIKU_WEAK_CHANGEPAR(Arpie)
RKR_HAIKU_WEAK_CHANGEPAR(Vocoder)
RKR_HAIKU_WEAK_CHANGEPAR(MBDist)
RKR_HAIKU_WEAK_CHANGEPAR(Echotron)
RKR_HAIKU_WEAK_CHANGEPAR(Harmonizer)
RKR_HAIKU_WEAK_CHANGEPAR(Shifter)
RKR_HAIKU_WEAK_CHANGEPAR(RyanWah)
RKR_HAIKU_WEAK_CHANGEPAR(RBEcho)
RKR_HAIKU_WEAK_CHANGEPAR(Synthfilter)
RKR_HAIKU_WEAK_CHANGEPAR(ShelfBoost)
RKR_HAIKU_WEAK_CHANGEPAR(Shuffle)
RKR_HAIKU_WEAK_CHANGEPAR(Dflange)

#undef RKR_HAIKU_WEAK_CHANGEPAR

__attribute__((weak)) void Compressor::Compressor_Change(int, int) { }
__attribute__((weak)) int Compressor::getpar(int) { return 0; }
__attribute__((weak)) void Gate::Gate_Change(int, int) { }
__attribute__((weak)) int Gate::getpar(int) { return 0; }

// Expander uses Expander_Change() instead of changepar(), same pattern as
// Compressor/Gate above.
__attribute__((weak)) void Expander::Expander_Change(int, int) { }
__attribute__((weak)) int Expander::getpar(int) { return 0; }

// Same story again, this time for every effect's Preset dropdown (see
// PresetMenuDef) -- setpreset() (and the Compressor/Gate/Expander
// equivalents that don't use that name) are called directly from this file
// now, so the small extra/ utilities need weak fallbacks for these too, on
// top of changepar()/getpar() above.
#define RKR_HAIKU_WEAK_SETPRESET(EffectClass) \
	__attribute__((weak)) void EffectClass::setpreset(int) { }

RKR_HAIKU_WEAK_SETPRESET(Exciter)
RKR_HAIKU_WEAK_SETPRESET(Valve)
RKR_HAIKU_WEAK_SETPRESET(Vibe)
RKR_HAIKU_WEAK_SETPRESET(Pan)
RKR_HAIKU_WEAK_SETPRESET(Reverbtron)
RKR_HAIKU_WEAK_SETPRESET(MusicDelay)
RKR_HAIKU_WEAK_SETPRESET(CompBand)
RKR_HAIKU_WEAK_SETPRESET(Arpie)
RKR_HAIKU_WEAK_SETPRESET(Vocoder)
RKR_HAIKU_WEAK_SETPRESET(Analog_Phaser)
RKR_HAIKU_WEAK_SETPRESET(Phaser)
RKR_HAIKU_WEAK_SETPRESET(Opticaltrem)
RKR_HAIKU_WEAK_SETPRESET(MBDist)
RKR_HAIKU_WEAK_SETPRESET(Echotron)
RKR_HAIKU_WEAK_SETPRESET(Harmonizer)
RKR_HAIKU_WEAK_SETPRESET(Shifter)
RKR_HAIKU_WEAK_SETPRESET(ShelfBoost)
RKR_HAIKU_WEAK_SETPRESET(Ring)
RKR_HAIKU_WEAK_SETPRESET(Reverb)
RKR_HAIKU_WEAK_SETPRESET(Alienwah)
RKR_HAIKU_WEAK_SETPRESET(Echo)
RKR_HAIKU_WEAK_SETPRESET(Sustainer)
RKR_HAIKU_WEAK_SETPRESET(Synthfilter)
RKR_HAIKU_WEAK_SETPRESET(Dflange)
RKR_HAIKU_WEAK_SETPRESET(RyanWah)
RKR_HAIKU_WEAK_SETPRESET(Shuffle)
RKR_HAIKU_WEAK_SETPRESET(RBEcho)
RKR_HAIKU_WEAK_SETPRESET(Convolotron)
RKR_HAIKU_WEAK_SETPRESET(NewDist)
RKR_HAIKU_WEAK_SETPRESET(StompBox)
RKR_HAIKU_WEAK_SETPRESET(DynamicFilter)
RKR_HAIKU_WEAK_SETPRESET(Looper)
RKR_HAIKU_WEAK_SETPRESET(Sequence)
RKR_HAIKU_WEAK_SETPRESET(StereoHarm)
RKR_HAIKU_WEAK_SETPRESET(MBVvol)
RKR_HAIKU_WEAK_SETPRESET(CoilCrafter)

#undef RKR_HAIKU_WEAK_SETPRESET

// Distorsion::setpreset() and Chorus::setpreset() take an extra "dgui"
// selector arg -- Overdrive (also a Distorsion instance, see efx_Overdrive
// in global.h) and Flanger (also a Chorus instance, see efx_Flanger) share
// these same two stubs, not separate ones.
__attribute__((weak)) void Distorsion::setpreset(int, int) { }
__attribute__((weak)) void Chorus::setpreset(int, int) { }

// Compressor/Gate/Expander's preset selector uses their own bespoke
// *_Change_Preset() name instead of setpreset() -- same as their regular
// parameter-change methods above.
__attribute__((weak)) void Compressor::Compressor_Change_Preset(int, int) { }
__attribute__((weak)) void Gate::Gate_Change_Preset(int) { }
__attribute__((weak)) void Expander::Expander_Change_Preset(int) { }

// EQ1/EQ2/Cabinet's presets are RKR-level free functions (see the
// RKR::loadfile/savefile stubs above), not methods on the effect object.
__attribute__((weak)) void RKR::EQ1_setpreset(int) { }
__attribute__((weak)) void RKR::EQ2_setpreset(int) { }
__attribute__((weak)) int RKR::Cabinet_setpreset(int) { return 0; }

// PERIOD (src/process.C) is a plain global, not a function -- same linking
// problem, same fix: a weak fallback definition that the strong one in
// process.C overrides whenever this file is linked into the real
// "rakarrack" binary. ScopeView::Draw() is the only thing here that reads
// it.
extern int PERIOD;
__attribute__((weak)) int PERIOD = 0;


extern pthread_mutex_t jmutex;

// Single message type for every control in the rack. "aidx" indexes into
// RakarrackView::fActions; the value comes from "be:value" for sliders and
// checkboxes, or from an explicit "val" field for menu items.
//
// MSG_OPEN_ORDER, MSG_SAVE_PRESET and MSG_LOAD_PRESET are deliberately NOT
// part of this table: RakarrackWindow wraps every MSG_ACTION dispatch in
// jmutex (the shared engine state most actions touch needs that), but
// showing a window or a BFilePanel for the first time is real work (layout,
// app_server round-trips) that has nothing to do with engine state --
// holding jmutex for however long that takes was blocking the real-time
// audio callback from acquiring it every ~2.7ms, starving playback (heard
// as a flood of "SoundPlayNode::FillNextBuffer: RequestBuffer failed").
// Routing these through their own messages keeps them out of that lock
// entirely; the actual savefile()/loadfile() calls those panels lead to
// (see RakarrackWindow) still take the lock explicitly, since those do
// touch engine state the audio thread reads.
enum {
	MSG_ACTION = 'RKAx',
	MSG_OPEN_ORDER = 'RKOo',
	MSG_SAVE_PRESET = 'RKSp',
	MSG_LOAD_PRESET = 'RKLp'
};

static const std::vector<std::string> kStompBoxModeNames = {
	"Amp", "Grunge", "Rat", "Fat Cat", "Dist+", "Death", "Mid Elves Own", "Fuzz"
};

// The 30 waveshaper types shared by the Overdrive and Distortion effects
// (both are instances of the Distorsion class) -- taken verbatim from
// RKRGUI::menu_dist_tipo in src/rakarrack.cxx.
static const std::vector<std::string> kDistTypeNames = {
	"Atan", "Asym1", "Pow", "Sine", "Qnts", "Zigzg", "Lmt", "LmtU", "LmtL",
	"ILmt", "Clip", "Asym2", "Pow2", "Sgm", "Crunch", "Hard Crunch",
	"Dirty Octave+", "M.Square", "M.Saw", "Compress", "Overdrive", "Soft",
	"Super Soft", "Hard Compress", "Lmt-NoGain", "FET", "DynoFET",
	"Valve 1", "Valve 2", "Diode clipper"
};

// Shared dropdown lists for the effects added after the initial 21 -- each
// taken verbatim from the matching RKRGUI::menu_* array in rakarrack.cxx,
// reused across effects the same way it is there.
static const std::vector<std::string> kLfoTypeNames = { // menu_chorus_lfotype
	"Sine", "Tri", "Ramp Up", "Ramp Down", "ZigZag", "M. Sqare", "M.Saw",
	"L. Fractal", "L. Fractal XY", "S/H Random"
};
static const std::vector<std::string> kSubDivNames = { // menu_arpie_subdiv
	"1", "1/2", "1/3", "1/4", "1/5", "1/6"
};
static const std::vector<std::string> kMusDelayDiv7Names = { // menu_musdelay_delay3
	"1", "1/2", "1/3", "1/4", "1/5", "1/6", "0"
};
static const std::vector<std::string> kArpiePatternNames = { // menu_arpie_pattern
	"Ascending", "Descending", "UpDown", "Stutter", "Interrupted Descent",
	"Double Descend"
};
static const std::vector<std::string> kCoilOriginNames = { // menu_coil_origin
	"Off", "Fender Strat (old)", "Fender Strat (new)", "Squire Strat",
	"Fender Hambucker", "Gibson P90", "Gibson Standard", "Gibson Mini",
	"Gibson Super L6S"
};
static const std::vector<std::string> kMBVvolCombiNames = { // menu_mbvvol_combi
	"1122", "1221", "1212", "o11o", "o12o", "x11x", "x12x", "1oo1", "1oo2",
	"1xx1", "1xx2"
};
static const std::vector<std::string> kLooperBarNames = { // menu_looper_bar
	"2/4", "3/4", "4/4", "5/4", "6/8", "7/8", "9/8", "11/8"
};
static const std::vector<std::string> kLooperMsNames = { "N", "H", "L" }; // menu_looper_ms
static const std::vector<std::string> kSeqModeNames = { // menu_seq_mode
	"Lineal", "UpDown", "Stepper", "Shifter", "Tremor", "Arpegiator", "Chorus"
};
static const std::vector<std::string> kShifterModeNames = { "Trigger", "Whammy" }; // menu_shifter_mode
static const std::vector<std::string> kMIDIOctaveNames = { "-2", "-1", "0", "1", "2" }; // menu_MIDIOctave
static const std::vector<std::string> kCCMapNames = { "Default", "MIDI Learn" }; // Mw0/Mw1

// Built-in impulse-response/cabinet selectors for the three effects that can
// also load a *custom* file via a Browse button in the FLTK GUI (Convol,
// Reverbtron, Echotron) -- that file-chooser path isn't wired up here, but
// these dropdowns (a plain changepar(8, ...) each) work standalone and give
// full use of the built-in library.
static const std::vector<std::string> kConvolIRNames = { // menu_convo_fnum
	"Marshall JCM200", "Fender Superchamp", "Mesa Boogie", "Mesa Boogie 2",
	"Marshall Plexi", "Bassman", "JCM2000", "Ampeg", "Marshall2"
};
static const std::vector<std::string> kReverbtronIRNames = { // menu_revtron_fnum
	"Chamber", "Conc. Stair", "Hall", "Med Hall", "Large Room", "Large Hall",
	"Guitar Ambience", "Studio", "Twilight", "Santa Lucia"
};
static const std::vector<std::string> kEchotronIRNames = { // menu_echotron_fnum
	"SwingPong", "Short Delays", "Flange + Echo", "Comb", "EchoFlange",
	"Filtered Echo", "Notch-Wah", "Multi-Chorus", "PingPong", "90-Shifter",
	"Basic LR Delay"
};

// Built-in factory presets, one list per effect -- each taken verbatim from
// the matching RKRGUI::menu_*_preset array in rakarrack.cxx (same order,
// same labels) so "Preset N" here always means the same thing it does in
// the FLTK GUI. See PresetMenuDef / BuildEffectBox's "preset" argument for
// how these get wired up: unlike every other dropdown in this file, a
// preset selection doesn't drive a single changepar() index -- it calls
// the effect's own setpreset()-family method, which writes many parameters
// at once from a table built into the effect class itself.
static const std::vector<std::string> kExciterPresetNames = { // menu_exciter_preset
	"Plain", "Loudness", "Exciter 1", "Exciter 2", "Exciter 3"
};
static const std::vector<std::string> kCompressorPresetNames = { // menu_compress_preset
	"2:1", "4:1", "8:1"
};
static const std::vector<std::string> kValvePresetNames = { // menu_valve_preset
	"Valve 1", "Valve 2", "Valve 3"
};
static const std::vector<std::string> kVibePresetNames = { // menu_vibe_preset
	"Classic", "Stereo Classic", "Wide Vibe", "Classic Chorus",
	"Vibe Chorus", "Lush Chorus", "Sick Phaser", "Warble"
};
static const std::vector<std::string> kPanPresetNames = { // menu_pan_preset
	"AutoPan", "Extra Stereo"
};
static const std::vector<std::string> kReverbtronPresetNames = { // menu_revtron_preset
	"Chamber", "Concrete Stairwell", "Hall", "Med Hall", "Room", "Hall",
	"Guitar", "Studio", "Cathedral"
};
static const std::vector<std::string> kMusDelayPresetNames = { // menu_musdelay_preset
	"Echo 1", "Echo 2"
};
static const std::vector<std::string> kCompBandPresetNames = { // menu_cband_preset
	"Good Start", "Loudness", "Loudness 2"
};
static const std::vector<std::string> kEQ2PresetNames = { // menu_eqp_preset
	"Plain", "Pop", "Jazz"
};
static const std::vector<std::string> kArpiePresetNames = { // menu_arpie_preset
	"Arpie 1", "Arpie 2", "Arpie 3", "Simple Arpie", "Canyon",
	"Panning Arpie 1", "Panning Arpie 2", "Panning Arpie 3",
	"Feedback Arpie"
};
static const std::vector<std::string> kVocoderPresetNames = { // menu_vo_preset
	"Vocoder 1", "Vocoder 2", "Vocoder 3", "Vocoder 4"
};
static const std::vector<std::string> kEQ1PresetNames = { // menu_eq_preset
	"Plain", "Pop", "Jazz"
};
static const std::vector<std::string> kAPhaserPresetNames = { // menu_aphaser_preset
	"Phaser 1", "Phaser 2", "Phaser 3", "Phaser 4", "Phaser 5", "Phaser 6"
};
static const std::vector<std::string> kPhaserPresetNames = { // menu_phaser_preset
	"Phaser 1", "Phaser 2", "Phaser 3", "Phaser 4", "Phaser 5", "Phaser 6"
};
static const std::vector<std::string> kOverdrivePresetNames = { // menu_ovrd_preset
	"Overdrive 1", "Overdrive 2"
};
static const std::vector<std::string> kOpticaltremPresetNames = { // menu_otrem_preset
	"Fast", "Trem 2", "Hard Pan", "Soft Pan", "Ramp Down", "Hard Ramp"
};
static const std::vector<std::string> kMBDistPresetNames = { // menu_mbdist_preset
	"Saturation", "Distorsion 1", "Soft", "Modulated", "Crunch",
	"Distortion 2", "Distortion 3", "Distortion 4"
};
static const std::vector<std::string> kEchotronPresetNames = { // menu_echotron_preset
	"Summer", "Ambience", "Arranjer", "Suction", "SuctionFlange"
};
static const std::vector<std::string> kDistorsionPresetNames = { // menu_dist_preset
	"Distorsion 1", "Distorsion 2", "Distorsion 3", "Guitar Amp"
};
static const std::vector<std::string> kHarmonizerPresetNames = { // menu_har_preset
	"Plain", "Octavator", "3m Down"
};
static const std::vector<std::string> kShifterPresetNames = { // menu_shifter_preset
	"Fast", "Slow Up", "Slow Down", "Chorus", "Trig. Chorus"
};
static const std::vector<std::string> kShelfBoostPresetNames = { // menu_shelf_preset
	"Trebble", "Mid", "Low", "Distortion 1"
};
static const std::vector<std::string> kCabinetPresetNames = { // menu_Cabinet_preset
	"Marshall-4-12", "Celestion G12M", "Jensen Alnico P12N",
	"Jensen Alnico P15N", "Delta Demon", "Celestion-EVH12",
	"Eminence Copperhead", "Mesa Boogie", "Jazz-Chorus", "Vox-Bright",
	"Marshall-I"
};
static const std::vector<std::string> kRingPresetNames = { // menu_ring_preset
	"Saw_Sin", "E string", "A string", "Dissonance", "Fast Beat", "Ring Amp"
};
static const std::vector<std::string> kReverbPresetNames = { // menu_reverb_preset
	"Cathedral 1", "Cathedral 2", "Cathedral 3", "Hall 1", "Hall 2",
	"Room 1", "Room 2", "Basement", "Tunnel", "Echoed 1", "Echoed 2",
	"Very Long 1", "Very Long 2"
};
static const std::vector<std::string> kFlangerPresetNames = { // menu_flanger_preset
	"Flange 1", "Flange 2", "Flange 3", "Flange 4", "Flange 5"
};
static const std::vector<std::string> kAlienwahPresetNames = { // menu_Alienwah_preset
	"AlienWah1", "AlienWah2", "AlienWah3", "AlienWah4"
};
static const std::vector<std::string> kEchoPresetNames = { // menu_echo_preset
	"Echo 1", "Echo 2", "Echo 3", "Simple Echo", "Canyon", "Panning Echo 1",
	"Panning Echo 2", "Panning Echo 3", "Feedback Echo"
};
static const std::vector<std::string> kSustainerPresetNames = { // menu_sus_preset
	"Sustain 1", "Sustain 2", "Sustain 3"
};
static const std::vector<std::string> kSynthfilterPresetNames = { // menu_synthfilter_preset
	"Low Pass", "High Pass", "Band Pass", "Lead Synth", "Water",
	"Pan Filter", "Multi"
};
static const std::vector<std::string> kDFlangePresetNames = { // menu_dflange_preset
	"Dual Flange 1", "Flange-Wah", "FbFlange", "SoftFlange", "Flanger",
	"Deep Chorus", "Bright Chorus"
};
static const std::vector<std::string> kRyanWahPresetNames = { // menu_ryanwah_preset
	"WahWah", "Mutron", "Phase Wah", "Phaser", "Quack Quack"
};
static const std::vector<std::string> kShufflePresetNames = { // menu_shuffle_preset
	"Shuffle 1", "Shuffle 2", "Shuffle 3", "Remover"
};
static const std::vector<std::string> kRBEchoPresetNames = { // menu_rbecho_preset
	"Echo 1", "Echo 2", "Echo 3"
};
static const std::vector<std::string> kConvolotronPresetNames = { // menu_convo_preset
	"Marshall JCM200", "Fender Superchamp", "Mesa Boogie", "Mesa Boogie 2"
};
static const std::vector<std::string> kNewDistPresetNames = { // menu_newdist_preset
	"New Dist 1", "New Dist 2", "New Dist 3"
};
static const std::vector<std::string> kGatePresetNames = { // menu_gate_preset
	"0dB", "-10dB", "-20dB"
};
static const std::vector<std::string> kChorusPresetNames = { // menu_chorus_preset
	"Chorus 1", "Chorus 2", "Chorus 3", "Celeste 1", "Celeste 2"
};
static const std::vector<std::string> kStompBoxPresetNames = { // menu_stomp_preset
	"Odie", "Grunger", "Hard Dist.", "Ratula", "Classic Dist",
	"Morbid Impalement", "Sharp Metal", "Classic Fuzz"
};
static const std::vector<std::string> kWhaWhaPresetNames = { // menu_WhaWha_preset
	"WahWah", "AutoWah", "Sweep", "VocalMorph1", "VocalMorph2"
};
static const std::vector<std::string> kLooperPresetNames = { // menu_looper_preset
	"Looper", "Reverse"
};
static const std::vector<std::string> kSequencePresetNames = { // menu_seq_preset
	"Jumpy", "Stair Step", "Mild", "Wah Wah", "Filter Pan", "Stepper",
	"Shifter", "Zeke Trem", "Boogie", "Chorus"
};
static const std::vector<std::string> kStereoHarmPresetNames = { // menu_shar_preset
	"Plain", "Octavator", "Chorus", "Hard Chorus"
};
static const std::vector<std::string> kMBVvolPresetNames = { // menu_mbvvol_preset
	"VaryVol 1", "VaryVol 2", "VaryVol 3"
};
static const std::vector<std::string> kCoilCrafterPresetNames = { // menu_coil_preset
	"H to S", "S to H"
};
static const std::vector<std::string> kExpanderPresetNames = { // menu_expander_preset
	"Noise Gate", "Boost Gate", "Treble swell"
};

// One slider: on-screen label/range plus which changepar() index it drives.
// "offset" is added to the raw UI value before it is sent to changepar(),
// and subtracted back out when priming the slider from getpar() -- this is
// how rakarrack.cxx itself maps its centered (-64..63 style) knobs onto the
// 0..127-centered-on-64 values the engine expects.
struct ParamDef {
	const char* label;
	int32 min;
	int32 max;
	int32 npar;
	int32 offset;
	// Set true only for a changepar() that's known to be genuinely
	// expensive (e.g. Convolotron/Reverbtron/Echotron's "Length" -- these
	// reprocess a whole impulse-response buffer synchronously). Such a
	// slider is wired through AddDebouncedSlider() instead of AddSlider():
	// the drag still moves and shows a value live, but the actual
	// changepar() call is held until ~150ms after the last change instead
	// of firing on every step, since each call blocks the real-time audio
	// thread (which needs the same lock) for as long as it takes.
	bool debounced = false;
};

// State for one AddDebouncedSlider() -- see that method and ParamDef's
// "debounced" field.
struct DebouncedSlider {
	bool hasPending = false;
	int32 pendingValue = 0;
	bigtime_t lastChangeTime = 0;
	std::function<void(int32)> apply;
};

// A plain on/off parameter (e.g. Pan's "Auto Pan" / "Extra On" flags).
struct ToggleDef {
	const char* label;
	int32 npar;
};

// A dropdown that drives a single changepar() index (an algorithm/mode/LFO
// type). See PresetMenuDef below for the other kind of dropdown -- a
// "Preset" selector that loads many params at once via setpreset().
struct TypeMenuDef {
	const char* label;
	const std::vector<std::string>* items;
	int32 npar;
	int32 offset = 0; // added to the menu index before changepar(), subtracted
	                  // back out when priming the initial selection from
	                  // getFn() -- most of these dropdowns store the raw
	                  // 0-based menu index, but a few (MusDelay's) store
	                  // index+1 instead.
};

// The "Preset" dropdown -- distinct from TypeMenuDef because a preset
// selection doesn't map onto a single changepar() index/value pair the way
// every other dropdown in this file does. It calls the effect's own
// setpreset()-family method instead (setpreset(), Compressor_Change_Preset(),
// Gate_Change_Preset(), Expander_Change_Preset()...), which writes many
// parameters at once from a table built into the effect class itself --
// see src/fileio.C-adjacent *.C files' own setpreset() bodies. There's no
// matching getpreset() to prime the dropdown's initial selection from (the
// FLTK GUI doesn't try either -- its preset Fl_Choice always just opens on
// its first item), so BuildEffectBox always marks index 0 initially,
// regardless of what's actually loaded.
struct PresetMenuDef {
	const std::vector<std::string>* items = nullptr;
	std::function<void(int32)> apply = nullptr;
};

// Dark theme, chosen to read close to src/rakarrack.cxx's own black
// background / gold titles / cyan labels look. Native BControls (BSlider,
// BCheckBox, BMenuField) render their frames, knobs and native text with
// the system's current UI theme rather than fully custom drawing the way
// FLTK's SliderW does, so this covers what Haiku's API actually exposes:
// view/panel backgrounds and the text we draw ourselves (titles, labels,
// values) -- not a pixel-exact reproduction of rakarrack.cxx's skin.
// Same value as kPanelColor on purpose -- the empty space around the effect
// boxes (columns, master bar, scroll area) is meant to read as one
// continuous surface with the boxes themselves, not a separate shade.
static const rgb_color kBgColor = { 30, 30, 30, 255 };      // window/column background
static const rgb_color kPanelColor = { 30, 30, 30, 255 };   // effect box / slider row background
static const rgb_color kTitleColor = { 224, 196, 132, 255 };// effect box titles (gold)
static const rgb_color kLabelColor = { 140, 200, 224, 255 }; // parameter labels (cyan)
static const rgb_color kValueColor = { 235, 235, 235, 255 };// numeric readouts (near-white)
static const rgb_color kAccentColor = { 90, 170, 200, 255 };// slider fill

// The shared audio engine (process.C) only ever walks the first 10 slots
// of rkr->efx_order[] per callback -- an effect's own Bypass flag is
// necessary but not sufficient for it to actually run: its effect-type ID
// (the same numbers as the "case N:" labels in that switch) also has to be
// sitting in one of those 10 slots. The FLTK GUI manages that through its
// own "Effects Order" window (drag effects in/out of the active chain);
// native mode has no such window, so each effect box claims its own slot
// directly from its "On" toggle instead. This mirrors the same
// 10-active-effects ceiling the FLTK GUI has always had -- it does not
// remove it, just gives native mode its own way to work within it.
//
// Every slot ALWAYS holds a valid effect-type ID (0-45), the same way
// FLTK's own Order window always names a real effect for each of its 10
// positions (an inactive one just has its own Bypass off) -- there is no
// "empty" sentinel. This isn't a style choice: src/fileio.C's savefile()
// unconditionally writes one getbuf() line per slot, and getbuf()'s
// switch has no case for an invalid ID -- for one it silently writes zero
// bytes, not even a newline, which desyncs every line loadfile() reads
// after that and corrupts the rest of the save file (confirmed against a
// save made from here: a couple of real-looking lines, then cascading
// garbage). An earlier version of this file used -1 for "no effect here"
// and hit exactly that. So "inactive" here means only one thing: a slot
// whose current occupant happens to have its own Bypass off, exactly
// like FLTK.
static const int kOrderSlotCount = 10;

// Address of the RKR member that gates whether effectId's out()/changepar()
// actually run -- one of 46 separately-named *_Bypass fields, since RKR has
// no array indexable by effect-type ID. Only the ones ActivateEffectSlot()
// and OrderWindow need to check generically are covered; everywhere else
// each effect box already has its own bypass pointer passed to it directly
// at construction. NULL for an ID this switch doesn't recognize.
static int* BypassPtrForId(RKR* rkr, int effectId) {
	switch (effectId) {
	case 0: return &rkr->EQ1_Bypass;
	case 1: return &rkr->Compressor_Bypass;
	case 2: return &rkr->Distorsion_Bypass;
	case 3: return &rkr->Overdrive_Bypass;
	case 4: return &rkr->Echo_Bypass;
	case 5: return &rkr->Chorus_Bypass;
	case 6: return &rkr->Phaser_Bypass;
	case 7: return &rkr->Flanger_Bypass;
	case 8: return &rkr->Reverb_Bypass;
	case 9: return &rkr->EQ2_Bypass;
	case 10: return &rkr->WhaWha_Bypass;
	case 11: return &rkr->Alienwah_Bypass;
	case 12: return &rkr->Cabinet_Bypass;
	case 13: return &rkr->Pan_Bypass;
	case 14: return &rkr->Harmonizer_Bypass;
	case 15: return &rkr->MusDelay_Bypass;
	case 16: return &rkr->Gate_Bypass;
	case 17: return &rkr->NewDist_Bypass;
	case 18: return &rkr->APhaser_Bypass;
	case 19: return &rkr->Valve_Bypass;
	case 20: return &rkr->DFlange_Bypass;
	case 21: return &rkr->Ring_Bypass;
	case 22: return &rkr->Exciter_Bypass;
	case 23: return &rkr->MBDist_Bypass;
	case 24: return &rkr->Arpie_Bypass;
	case 25: return &rkr->Expander_Bypass;
	case 26: return &rkr->Shuffle_Bypass;
	case 27: return &rkr->Synthfilter_Bypass;
	case 28: return &rkr->MBVvol_Bypass;
	case 29: return &rkr->Convol_Bypass;
	case 30: return &rkr->Looper_Bypass;
	case 31: return &rkr->RyanWah_Bypass;
	case 32: return &rkr->RBEcho_Bypass;
	case 33: return &rkr->CoilCrafter_Bypass;
	case 34: return &rkr->ShelfBoost_Bypass;
	case 35: return &rkr->Vocoder_Bypass;
	case 36: return &rkr->Sustainer_Bypass;
	case 37: return &rkr->Sequence_Bypass;
	case 38: return &rkr->Shifter_Bypass;
	case 39: return &rkr->StompBox_Bypass;
	case 40: return &rkr->Reverbtron_Bypass;
	case 41: return &rkr->Echotron_Bypass;
	case 42: return &rkr->StereoHarm_Bypass;
	case 43: return &rkr->CompBand_Bypass;
	case 44: return &rkr->Opticaltrem_Bypass;
	case 45: return &rkr->Vibe_Bypass;
	default: return nullptr;
	}
}

// Returns true if effectId is now (or already was) occupying a slot.
// False means all 10 slots are held by other currently-active effects.
static bool ActivateEffectSlot(RKR* rkr, int effectId) {
	for (int i = 0; i < kOrderSlotCount; i++) {
		if (rkr->efx_order[i] == effectId)
			return true;
	}
	// Steal a slot from whichever occupant is currently bypassed -- its
	// own state doesn't change, it just stops being one of the 10 in the
	// chain. If that effect is turned on again later, it competes for a
	// slot the same way any other effect does.
	for (int i = 0; i < kOrderSlotCount; i++) {
		int* occupantBypass = BypassPtrForId(rkr, rkr->efx_order[i]);
		if (occupantBypass && *occupantBypass == 0) {
			rkr->efx_order[i] = effectId;
			return true;
		}
	}
	return false;
}

// Deliberately does nothing: with no "empty" sentinel, turning an effect
// off doesn't free its slot -- the slot keeps naming this effect (now
// just bypassed, like any inactive FLTK Order-window entry) until some
// other effect actually needs the slot and steals it in
// ActivateEffectSlot(). The caller has already flipped *bypass to 0,
// which is what actually silences it (see process.C's "if (X_Bypass)").
static void DeactivateEffectSlot(RKR*, int) { }

// Display name for each effect-type ID the native UI exposes -- every
// effect BuildColumn1-4 wires up can end up in rkr->efx_order[], so this
// needs to cover all 46, not just the original 21.
static const char* EffectName(int effectId) {
	switch (effectId) {
	case 0: return "Equalizer";
	case 1: return "Compressor";
	case 2: return "Distorsion";
	case 3: return "Overdrive";
	case 4: return "Echo";
	case 5: return "Chorus";
	case 6: return "Phaser";
	case 7: return "Flanger";
	case 8: return "Reverb";
	case 9: return "EQ2";
	case 10: return "WhaWha";
	case 11: return "Alienwah";
	case 12: return "Cabinet";
	case 13: return "Auto Pan";
	case 14: return "Harmonizer";
	case 15: return "MusDelay";
	case 16: return "Noise Gate";
	case 17: return "Distortion";
	case 18: return "Analog Phaser";
	case 19: return "Valve";
	case 20: return "DFlange";
	case 21: return "Ring Modulator";
	case 22: return "Exciter";
	case 23: return "MBDist";
	case 24: return "Arpie";
	case 25: return "Expander";
	case 26: return "Shuffle";
	case 27: return "Synthfilter";
	case 28: return "MBVvol";
	case 29: return "Convolotron";
	case 30: return "Looper";
	case 31: return "RyanWah";
	case 32: return "RBEcho";
	case 33: return "CoilCrafter";
	case 34: return "ShelfBoost";
	case 35: return "Vocoder";
	case 36: return "Sustainer";
	case 37: return "Sequence";
	case 38: return "Shifter";
	case 39: return "StompBox";
	case 40: return "Reverbtron";
	case 41: return "Echotron";
	case 42: return "StereoHarm";
	case 43: return "CompBand";
	case 44: return "Opticaltrem";
	case 45: return "Vibe";
	default: return "(unknown)";
	}
}

// A small window listing the currently-active effects (those with a claimed
// chain slot -- see ActivateEffectSlot()) in their actual processing order,
// top to bottom, with Move Up/Move Down buttons to reorder them. This is
// native mode's answer to rakarrack.cxx's drag-and-drop "Effects Order"
// window: a plain BListView instead of a real drag target, since order
// only ever needs a swap between two slots and buttons get that exactly
// right without any native drag-and-drop code to get subtly wrong.
enum {
	MSG_ORDER_UP = 'RKOu',
	MSG_ORDER_DOWN = 'RKOd',
	MSG_ORDER_CLOSE = 'RKOc'
};

class OrderWindow : public BWindow {
public:
	OrderWindow(RKR* rkr)
		:
		BWindow(BRect(160, 160, 460, 480), "Effects Order", B_TITLED_WINDOW,
			B_ASYNCHRONOUS_CONTROLS | B_NOT_ZOOMABLE),
		fRkr(rkr)
	{
		// BWindow has no SetViewColor() of its own (unlike BView) -- give it
		// a single real BView child to carry the background color instead,
		// same as RakarrackWindow's scroller/content already do. This has
		// to be a plain BView, not another BGroupView: a BGroupView already
		// sets up and owns its own BGroupLayout, and running
		// BLayoutBuilder::Group<> on it too (below) fights that existing
		// layout instead of using it -- nothing ends up attached to
		// anything that actually draws, which is why this showed up as a
		// blank white window instead of the rack's dark theme.
		SetLayout(new BGroupLayout(B_VERTICAL));
		BView* background = new BView("bg", B_WILL_DRAW);
		background->SetViewColor(kBgColor);
		AddChild(background);

		fList = new BListView("order_list", B_SINGLE_SELECTION_LIST);
		fList->SetViewColor(kPanelColor);
		fList->SetLowColor(kPanelColor);
		fList->SetHighColor(kValueColor);
		BScrollView* listScroll = new BScrollView("order_scroll", fList, 0,
			false, true);

		BStringView* hint = new BStringView("hint",
			"Only effects you've turned on appear here. This is the order "
			"they process your signal in, top to bottom.");
		hint->SetHighColor(kLabelColor);
		hint->SetLowColor(kBgColor);
		hint->SetFontSize(10.5f);

		BButton* upBtn = new BButton("up", "Move Up",
			new BMessage(MSG_ORDER_UP));
		BButton* downBtn = new BButton("down", "Move Down",
			new BMessage(MSG_ORDER_DOWN));
		BButton* closeBtn = new BButton("close", "Close",
			new BMessage(MSG_ORDER_CLOSE));

		BLayoutBuilder::Group<>(background, B_VERTICAL, 8)
			.SetInsets(10)
			.Add(hint)
			.Add(listScroll)
			.AddGroup(B_HORIZONTAL, 6)
				.Add(upBtn)
				.Add(downBtn)
				.AddGlue()
				.Add(closeBtn)
			.End()
		.End();

		SetSizeLimits(260, 6000, 260, 6000);
		RefreshList();
	}

	virtual void MessageReceived(BMessage* msg)
	{
		switch (msg->what) {
		case MSG_ORDER_UP:
			MoveSelected(-1);
			break;
		case MSG_ORDER_DOWN:
			MoveSelected(1);
			break;
		case MSG_ORDER_CLOSE:
			// Hide rather than Quit()/destroy -- see QuitRequested() below
			// for why.
			Hide();
			break;
		default:
			BWindow::MessageReceived(msg);
			break;
		}
	}

	// This window is never actually destroyed while the app runs -- it is
	// created once, lazily, on the first "Effects Order..." click (see
	// RakarrackView) and hidden/shown after that. A BApplication's default
	// behavior is to quit once its window count reaches zero; that default
	// has nothing to do with which window closed, so destroying even this
	// one secondary window (via Quit()) while the main rack window was
	// still open was enough to trip it and take the whole app down.
	// Hiding instead of quitting means this window is never subtracted
	// from that count in the first place.
	//
	// BUT that "hide, refuse" answer must not be unconditional: it's also
	// exactly what BApplication::QuitRequested()'s default implementation
	// asks every window (this one included) when the app is asked to quit
	// as a whole -- Deskbar/ProcessController's "Quit Application", a
	// session shutdown, B_QUIT_REQUESTED sent straight to the app. Once
	// this window has been created at all (a single "Effects Order..."
	// click, any time in the session), answering "no" there every single
	// time would permanently veto the whole app's shutdown from then on --
	// which is the exact bug this fixes. gAppQuitting (see
	// rakarrack_haiku_bridge.h) is set by RakarrackApp::QuitRequested() in
	// main.C before that cascade runs, so this only refuses the narrower,
	// original case: the user closing just this one window on its own.
	virtual bool QuitRequested()
	{
		if (gAppQuitting)
			return true;
		Hide();
		return false;
	}

	// Called by RakarrackView every time it shows this window, since the
	// active-effects list can have changed since it was last hidden.
	void Refresh() { RefreshList(); }

private:
	// Rebuilds the visible list from rkr->efx_order[], skipping empty
	// slots -- fSlotIndices[row] records which actual efx_order[] index
	// that visible row came from, since gaps from effects turned off in
	// between mean visible rows are not the same as raw slot indices.
	void RefreshList()
	{
		int32 selRow = fList->CurrentSelection();
		int selectedSlot = (selRow >= 0 && selRow < (int32)fSlotIndices.size())
			? fSlotIndices[selRow] : -1;

		while (fList->CountItems() > 0)
			delete fList->RemoveItem((int32)0);
		fSlotIndices.clear();

		for (int i = 0; i < kOrderSlotCount; i++) {
			int id = fRkr->efx_order[i];
			// Every slot always names a real effect now (see the big
			// comment above ActivateEffectSlot()) -- only show the ones
			// actually turned on, matching the hint text above and what
			// used to be conveyed by an empty slot.
			int* bypass = BypassPtrForId(fRkr, id);
			if (!bypass || *bypass == 0)
				continue;
			fList->AddItem(new BStringItem(EffectName(id)));
			fSlotIndices.push_back(i);
		}

		for (size_t row = 0; row < fSlotIndices.size(); row++) {
			if (fSlotIndices[row] == selectedSlot) {
				fList->Select((int32)row);
				break;
			}
		}
	}

	// Swaps the selected effect's actual efx_order[] slot with its
	// immediate visible neighbor's -- locked the same way every other
	// write to efx_order[] is (see ActivateEffectSlot/DeactivateEffectSlot)
	// since jackprocess() reads this array on the audio thread every
	// callback with no lock of its own around that read.
	void MoveSelected(int direction)
	{
		int32 row = fList->CurrentSelection();
		if (row < 0)
			return;
		int32 otherRow = row + direction;
		if (otherRow < 0 || otherRow >= (int32)fSlotIndices.size())
			return;

		pthread_mutex_lock(&jmutex);
		int a = fSlotIndices[row];
		int b = fSlotIndices[otherRow];
		int tmp = fRkr->efx_order[a];
		fRkr->efx_order[a] = fRkr->efx_order[b];
		fRkr->efx_order[b] = tmp;
		pthread_mutex_unlock(&jmutex);

		RefreshList();
		fList->Select(otherRow);
	}

	RKR* fRkr;
	BListView* fList;
	std::vector<int> fSlotIndices;
};

// A stereo waveform view for the header -- a native port of src/rakarrack.cxx's
// "Sco" (Scope) widget, the one that appears over the Tuner box there when its
// title is clicked. It draws straight from the same two buffers the FLTK GUI
// itself uses (rkr->anall/rkr->analr -- see Scope::init()'s call site in
// rakarrack.cxx): PERIOD samples of the final, post-FX output, refreshed by
// the real-time audio callback every buffer. Unlike rakarrack.cxx, this is
// always visible here (no click-to-reveal toggle) rather than sharing screen
// space with a Tuner box native mode doesn't have.
//
// Reads anall/analr with no lock, same as rakarrack.cxx's own Scope::draw()
// -- the audio thread is free to be mid-memcpy into them on any given frame,
// but a plain float read/write races only into torn *values* here (never a
// crash, and self-correcting one frame later), the same tradeoff every VU
// meter and scope in this codebase already makes for real-time-safe display
// code.
class ScopeView : public BView {
public:
	ScopeView(RKR* rkr)
		:
		BView("scope", B_WILL_DRAW | B_PULSE_NEEDED),
		fRkr(rkr)
	{
		SetViewColor(kPanelColor);
		SetExplicitMinSize(BSize(220, 60));
		SetExplicitMaxSize(BSize(220, 60));
	}

	virtual void Pulse()
	{
		Invalidate();
	}

	virtual void Draw(BRect updateRect)
	{
		BRect b = Bounds();
		SetHighColor(kPanelColor);
		FillRect(b);
		SetHighColor(70, 70, 70, 255);
		StrokeRect(b);
		SetHighColor(50, 50, 50, 255);
		StrokeLine(BPoint(b.Width() / 2.0f, b.top + 1),
			BPoint(b.Width() / 2.0f, b.bottom - 1));

		if (!fRkr || !fRkr->anall || !fRkr->analr || PERIOD <= 1)
			return;

		float gutter = 4.0f;
		float halfW = (b.Width() - 3.0f * gutter) / 2.0f;
		BRect left(b.left + gutter, b.top + 2, b.left + gutter + halfW, b.bottom - 2);
		BRect right(left.right + gutter, left.top, left.right + gutter + halfW, left.bottom);
		DrawChannel(fRkr->anall, left);
		DrawChannel(fRkr->analr, right);
	}

private:
	void DrawChannel(float* samples, BRect area)
	{
		SetHighColor(kAccentColor);
		float midY = (area.top + area.bottom) / 2.0f;
		float halfH = area.Height() / 2.0f;
		float stepX = area.Width() / (float)PERIOD;
		BPoint prev(area.left, midY);
		// PERIOD can be a few thousand at low sample rates/large buffers --
		// no need to plot every single sample when several land on the same
		// pixel column, so stride through in ~1px steps instead of all of
		// them.
		int stride = (int)(1.0f / stepX);
		if (stride < 1)
			stride = 1;
		for (int i = 0; i < PERIOD; i += stride) {
			float v = samples[i];
			if (v > 1.0f)
				v = 1.0f;
			else if (v < -1.0f)
				v = -1.0f;
			BPoint pt(area.left + i * stepX, midY - v * halfH);
			if (i > 0)
				StrokeLine(prev, pt);
			prev = pt;
		}
	}

	RKR* fRkr;
};

// Main rack content view: builds every effect box and owns the table of
// callbacks ("actions") that the controls' messages are dispatched through.
class RakarrackView : public BView {
public:
	RakarrackView(RKR* rkr)
		:
		BView("MainView", B_WILL_DRAW | B_PULSE_NEEDED),
		fRkr(rkr)
	{
		// No setup needed for efx_order[] here -- the factory-default bank
		// (loaded earlier, before this view exists) already leaves it at
		// {0,1,...,9}, all with their own Bypass off, which is exactly
		// what ActivateEffectSlot() needs: every slot names a real,
		// currently-inactive effect, so the first thing turned on (in
		// native mode, any of the 46, not just 0-9) immediately has a
		// slot to steal. See the comment above ActivateEffectSlot() for
		// why there's no "clear to empty" step the way there used to be.
		SetViewColor(kBgColor);

		// Four scrollable columns of effect racks (was five -- with each
		// slider now ~2x as wide, four fits comfortably on more screens),
		// mirroring the layout of src/rakarrack.cxx without trying to
		// reproduce its exact pixel geometry.
		BGroupView* col1 = new BGroupView(B_VERTICAL, 8);
		BGroupView* col2 = new BGroupView(B_VERTICAL, 8);
		BGroupView* col3 = new BGroupView(B_VERTICAL, 8);
		BGroupView* col4 = new BGroupView(B_VERTICAL, 8);
		col1->GroupLayout()->SetInsets(5);
		col2->GroupLayout()->SetInsets(5);
		col3->GroupLayout()->SetInsets(5);
		col4->GroupLayout()->SetInsets(5);
		col1->SetViewColor(kBgColor);
		col2->SetViewColor(kBgColor);
		col3->SetViewColor(kBgColor);
		col4->SetViewColor(kBgColor);

		BuildColumn1(col1);
		BuildColumn2(col2);
		BuildColumn3(col3);
		BuildColumn4(col4);

		col1->GroupLayout()->AddItem(BSpaceLayoutItem::CreateGlue());
		col2->GroupLayout()->AddItem(BSpaceLayoutItem::CreateGlue());
		col3->GroupLayout()->AddItem(BSpaceLayoutItem::CreateGlue());
		col4->GroupLayout()->AddItem(BSpaceLayoutItem::CreateGlue());

		BGroupView* columns = new BGroupView(B_HORIZONTAL, 8);
		columns->GroupLayout()->SetInsets(10);
		columns->SetViewColor(kBgColor);
		columns->AddChild(col1);
		columns->AddChild(col2);
		columns->AddChild(col3);
		columns->AddChild(col4);

		BLayoutBuilder::Group<>(this, B_VERTICAL, 0)
			.Add(columns)
			.End();
	}

	// Builds the always-visible header (logo, CPU/FX Engine/Boost, Input
	// Gain/Master Volume, Effects Order, Save/Load Preset) into
	// headerParent, a view RakarrackWindow keeps outside the scrolled area
	// so none of this scrolls away with the effect racks. Lives here (not
	// in RakarrackWindow) purely so it can call this view's own private
	// Bind()/MakeMessage() -- the resulting widgets are added to
	// headerParent, not to this view, and that's fine: a BControl's
	// message still resolves to Window() regardless of which view in the
	// window's hierarchy it's actually a child of, so Dispatch() (called
	// from RakarrackWindow::MessageReceived on every MSG_ACTION) still
	// reaches them the same way it reaches every slider in the scrolled
	// columns below.
	void BuildHeader(BView* headerParent)
	{
		RKR* rkr = fRkr;

		BStringView* logo = new BStringView("logo", "Haikurack");
		logo->SetHighColor(kTitleColor);
		logo->SetLowColor(kBgColor);
		BFont logoFont(be_bold_font);
		logoFont.SetSize(28.0f);
		logoFont.SetFace(B_ITALIC_FACE | B_BOLD_FACE);
		logo->SetFont(&logoFont);
		logo->SetExplicitAlignment(BAlignment(B_ALIGN_LEFT, B_ALIGN_MIDDLE));

		fCpuDisplay = new BStringView("cpu", "CPU: 0.00%");
		fCpuDisplay->SetHighColor(kValueColor);
		fCpuDisplay->SetLowColor(kBgColor);
		// Fixed width so the header doesn't reflow (a visible bounce/jitter
		// in everything to its right) every time the text changes length as
		// the percentage itself changes -- e.g. "0.37%" vs. "12.34%" are
		// different widths, and Pulse() updates this via SetText() many
		// times a second.
		fCpuDisplay->SetExplicitMinSize(BSize(80, B_SIZE_UNSET));
		fCpuDisplay->SetExplicitMaxSize(BSize(80, B_SIZE_UNSET));

		fMasterFX = new BCheckBox("master_fx", "FX Engine",
			MakeMessage(Bind([rkr](int32 v) {
				rkr->Bypass = v ? 1 : 0;
				if (!v)
					rkr->cleanup_efx();
			})));
		fMasterFX->SetValue(rkr->Bypass ? B_CONTROL_ON : B_CONTROL_OFF);
		fMasterFX->SetViewColor(kBgColor);
		BFont boldFont(be_bold_font);
		fMasterFX->SetFont(&boldFont);

		BCheckBox* boost = new BCheckBox("boost", "Boost +10dB",
			MakeMessage(Bind([rkr](int32 v) {
				rkr->booster = v ? dB2rap(10.0f) : 1.0f;
			})));
		boost->SetValue(rkr->booster > 1.0f ? B_CONTROL_ON : B_CONTROL_OFF);
		boost->SetViewColor(kBgColor);

		// Opens (or, if already built, just refreshes, un-hides and raises)
		// the Effects Order window -- see OrderWindow above. This window is
		// created once and then only ever hidden, never destroyed, for the
		// life of the app (see OrderWindow::QuitRequested()).
		//
		// This button (and Save/Load Preset below) deliberately does NOT go
		// through Bind()/MakeMessage() (MSG_ACTION) like every other control
		// here -- RakarrackWindow wraps every MSG_ACTION dispatch in jmutex,
		// and OpenOrderWindow()/the save-and-load panels are real,
		// possibly-slow work (building a window, opening a BFilePanel) that
		// has nothing to do with the engine state jmutex actually protects.
		// See MSG_OPEN_ORDER's comment.
		BButton* orderBtn = new BButton("order", "Effects Order...",
			new BMessage(MSG_OPEN_ORDER));

		BButton* savePresetBtn = new BButton("save_preset", "Save Preset",
			new BMessage(MSG_SAVE_PRESET));
		savePresetBtn->SetViewColor(kPanelColor);

		BButton* loadPresetBtn = new BButton("load_preset", "Load Preset",
			new BMessage(MSG_LOAD_PRESET));
		loadPresetBtn->SetViewColor(kPanelColor);

		BGroupView* controls = new BGroupView(B_HORIZONTAL, 10);
		controls->GroupLayout()->SetInsets(10, 0, 10, 4);
		controls->SetViewColor(kBgColor);
		controls->AddChild(fCpuDisplay);
		controls->AddChild(fMasterFX);
		controls->AddChild(boost);
		controls->AddChild(orderBtn);
		controls->AddChild(savePresetBtn);
		controls->AddChild(loadPresetBtn);
		controls->GroupLayout()->AddItem(BSpaceLayoutItem::CreateGlue());

		// Third row: Input Gain/Master Volume (wrapped down from the controls
		// row above so it has room to breathe) plus the waveform view, left
		// to right. Half the usual 190px track width (see AddSlider's
		// "width" parameter) -- full-width here made this row noticeably
		// wider than it needed to be next to the compact scope/MIDI panel.
		static const int32 kHeaderSliderWidth = 95;
		BGroupView* volumeGroup = new BGroupView(B_HORIZONTAL, 10);
		volumeGroup->SetViewColor(kBgColor);
		AddSlider(volumeGroup, "in_gain", "Input Gain", -50, 50,
			(int32)(rkr->Input_Gain * 100.0f) - 50,
			[rkr](int32 v) {
				rkr->Input_Gain = (float)((v + 50) / 100.0);
				rkr->calculavol(1);
			}, kHeaderSliderWidth);
		AddSlider(volumeGroup, "out_gain", "Master Volume", -50, 50,
			(int32)(rkr->Master_Volume * 100.0f) - 50,
			[rkr](int32 v) {
				rkr->Master_Volume = (float)((v + 50) / 100.0);
				rkr->calculavol(2);
			}, kHeaderSliderWidth);

		// The sliders above only set their on-screen position from
		// Input_Gain/Master_Volume -- they never fire their own callback, so
		// without this, Log_I_Gain/Log_M_Volume (the actual gain multipliers
		// used in the audio path, see process.C's calculavol()) stay
		// uninitialized until the user manually touches a slider. Mirrors
		// rakarrack.cxx's own startup priming (RKRGUI's constructor).
		rkr->calculavol(1);
		rkr->calculavol(2);
		rkr->booster = 1.0f;

		ScopeView* scope = new ScopeView(rkr);

		// The MIDI (guitar-to-MIDI) converter panel -- a native port of
		// rakarrack.cxx's "Midi" group (nidi_activar/MIDIOctave/
		// Midi_out_Counter/Trig_Adj). Velocity (Vel_Adj there) is left out,
		// per instructions -- it doesn't work. This effect sits outside the
		// changepar()/getpar() rack chain entirely (no effect-type ID, never
		// occupies an efx_order[] slot), so it's built directly here instead
		// of through BuildEffectBox/BuildColumnN. Its own row, below Input
		// Gain/Master Volume/the scope, rather than crowding them.
		BBox* midiBox = new BBox("midi_box");
		midiBox->SetViewColor(kPanelColor);
		BStringView* midiTitle = new BStringView("midi_title", "MIDI");
		midiTitle->SetHighColor(kTitleColor);
		midiTitle->SetLowColor(kBgColor);
		BFont midiTitleFont(be_bold_font);
		midiTitle->SetFont(&midiTitleFont);
		midiBox->SetLabel(midiTitle);

		BGroupView* midiContent = new BGroupView(B_VERTICAL, 6);
		midiContent->GroupLayout()->SetInsets(8);
		midiContent->SetViewColor(kPanelColor);

		// Everything below the "On" checkbox -- hidden whenever MIDI is off,
		// the same way each effect box's body collapses to just its title
		// bar in BuildEffectBox. Built before midiOn since its callback
		// below needs to reach it.
		BGroupView* midiBody = new BGroupView(B_VERTICAL, 4);
		midiBody->SetViewColor(kPanelColor);

		BCheckBox* midiOn = new BCheckBox("midi_on", "On",
			MakeMessage(Bind([rkr, midiBody](int32 v) {
				// Mirrors cb_nidi_activar_i: silence any note the converter
				// currently thinks is held before switching it off, so
				// nothing gets stuck on.
				if (!v)
					rkr->efx_MIDIConverter->panic();
				rkr->MIDIConverter_Bypass = v ? 1 : 0;
				if (v)
					midiBody->Show();
				else
					midiBody->Hide();
			})));
		midiOn->SetValue(rkr->MIDIConverter_Bypass ? B_CONTROL_ON : B_CONTROL_OFF);
		midiOn->SetViewColor(kPanelColor);
		midiContent->AddChild(midiOn);
		if (rkr->MIDIConverter_Bypass == 0)
			midiBody->Hide();
		midiContent->AddChild(midiBody);

		// Like the Input Gain/Master Volume sliders above, AddSlider() only
		// sets a slider's on-screen position from whatever field it's
		// given as "initial" -- it never fires the slider's own callback,
		// so anything that needs the *engine* to actually know that value
		// (not just the widget to display it) has to be primed here,
		// before any of the sliders below are built (several of them read
		// these same fields for their own initial display).
		//
		// efx_MIDIConverter->channel/TrigVal happen to already be fine
		// without this -- MIDIConverter's own constructor defaults
		// (channel 0, TrigVal .25f) already match what's shown below --
		// but VelVal (which MIDI_Send_Note_On() uses to compute the
		// velocity byte sent to an external synth) has NO constructor
		// default at all, so it was whatever garbage happened to be on
		// the heap. rakarrack.cxx always primes this at startup too (see
		// its "Velocity Adjust" pref, default 50) even though no Velocity
		// slider is exposed here (per instructions -- it doesn't work).
		// Without this, notes could reach an external synth (e.g.
		// MidiSynth) and visibly trigger there -- schmittFloat()'s note
		// detection doesn't depend on VelVal -- while playing at whatever
		// garbage velocity resulted, typically silent once clamped to the
		// 1-127 range.
		rkr->efx_MIDIConverter->setmidichannel(rkr->efx_MIDIConverter->channel);
		rkr->efx_MIDIConverter->setTriggerAdjust(
			rkr->efx_MIDIConverter->TrigVal > 0.0f
				? (int32)(1.0f / rkr->efx_MIDIConverter->TrigVal + 0.5f)
				: 4);
		rkr->efx_MIDIConverter->setVelAdjust(50);

		// Same story again, this time for incoming MIDI ("Rakarrack IN" --
		// see src/rkrMIDI.C's RKR::Conecta()/jack_process_midievents()):
		// rkr->MidiCh and rkr->RControl are given real defaults by
		// RKR::RKR() itself (src/process.C), shared by every frontend, so
		// they're already fine here. rkr->HarCh (the channel the
		// Harmonizer/StereoHarm "MIDI" toggle listens on for chord input
		// via RecChord::MiraChord()) and rkr->MIDIway (which CC-mapping
		// table process_midi_controller_events() uses) are NOT -- like
		// VelVal above, rakarrack.cxx only ever sets them from its own
		// "MIDI IN Harmonizer"/"MIDI Implementation" prefs at startup, so
		// without this they'd be whatever garbage was on the heap here.
		// Defaults match rakarrack.cxx's own fallbacks when no saved
		// preference exists (channel 1, i.e. HarCh 0; MIDIway 0, the
		// built-in CC map rather than the empty custom XUserMIDI table).
		rkr->HarCh = 0;
		rkr->MIDIway = 0;

		// Wide gap between controls (vs. AddSlider's own tight 6px
		// label/value/slider spacing within each one) so each slider
		// clearly reads as trailing its own preceding label+value instead
		// of blending into the next control's label -- with only 8px
		// either side that trailing slider was easy to misread as
		// belonging to whichever label came right after it instead.
		BGroupView* midiRow1 = new BGroupView(B_HORIZONTAL, 24);
		midiRow1->SetViewColor(kPanelColor);
		midiBody->AddChild(midiRow1);

		// "Out Ch" -- named to distinguish it from "In Ch" below; this is
		// the channel efx_MIDIConverter sends the guitar's converted
		// notes out on (the "Rakarrack OUT" MIDI endpoint), not anything
		// to do with incoming MIDI.
		AddSlider(midiRow1, "midi_channel", "Out Ch", 1, 16,
			rkr->efx_MIDIConverter->channel + 1,
			[rkr](int32 v) { rkr->efx_MIDIConverter->setmidichannel(v - 1); },
			kHeaderSliderWidth);

		AddSlider(midiRow1, "midi_trigger", "Trigger", 2, 60,
			rkr->efx_MIDIConverter->TrigVal > 0.0f
				? (int32)(1.0f / rkr->efx_MIDIConverter->TrigVal + 0.5f)
				: 4,
			[rkr](int32 v) { rkr->efx_MIDIConverter->setTriggerAdjust(v); },
			kHeaderSliderWidth);

		AddTypeMenu(midiRow1, "midi_octave", "Octave", kMIDIOctaveNames,
			rkr->efx_MIDIConverter->Moctave + 2,
			[rkr](int32 v) { rkr->efx_MIDIConverter->Moctave = v - 2; });

		// Second row: the guitar-to-MIDI pitch tracker's own fine-tune
		// knobs (Conv_Trig_Counter/Conv_Stable_Counter/Conv_Off_Counter/
		// Conv_Freq_Ceiling_Counter/Conv_Freq_Floor_Counter in
		// rakarrack.cxx). Unlike Out Ch/Trigger/Octave above, these five
		// don't need explicit startup priming -- p_trigfact/
		// p_stable_threshold/p_off_count_max/p_freq_ceiling/p_freq_floor
		// all already have constructor defaults in MIDIConverter.C that
		// match rakarrack.cxx's own widget defaults exactly (0.5, 2, 5,
		// 320, 20).
		//
		// Trigger Sensitivity is the one float among these with a
		// fractional step (0.1-1.0 by 0.05) -- shown as a plain integer
		// slider like everything else here (10-100) rather than adding a
		// decimal-display slider variant just for this one control, with
		// the /100 conversion happening in the callback.
		BGroupView* midiRow2 = new BGroupView(B_HORIZONTAL, 24);
		midiRow2->SetViewColor(kPanelColor);
		midiBody->AddChild(midiRow2);

		AddSlider(midiRow2, "midi_trigsens", "Trig Sens", 10, 100,
			(int32)(rkr->efx_MIDIConverter->p_trigfact * 100.0f + 0.5f),
			[rkr](int32 v) { rkr->efx_MIDIConverter->p_trigfact = (float)v / 100.0f; },
			kHeaderSliderWidth);

		AddSlider(midiRow2, "midi_stability", "Stability", 1, 10,
			rkr->efx_MIDIConverter->p_stable_threshold,
			[rkr](int32 v) { rkr->efx_MIDIConverter->p_stable_threshold = v; },
			kHeaderSliderWidth);

		AddSlider(midiRow2, "midi_offgrace", "Off Grace", 1, 30,
			rkr->efx_MIDIConverter->p_off_count_max,
			[rkr](int32 v) { rkr->efx_MIDIConverter->p_off_count_max = v; },
			kHeaderSliderWidth);

		AddSlider(midiRow2, "midi_freqceil", "Freq Ceil", 50, 5000,
			(int32)rkr->efx_MIDIConverter->p_freq_ceiling,
			[rkr](int32 v) { rkr->efx_MIDIConverter->p_freq_ceiling = (float)v; },
			kHeaderSliderWidth);

		AddSlider(midiRow2, "midi_freqfloor", "Freq Floor", 20, 300,
			(int32)rkr->efx_MIDIConverter->p_freq_floor,
			[rkr](int32 v) { rkr->efx_MIDIConverter->p_freq_floor = (float)v; },
			kHeaderSliderWidth);

		// Third row: incoming MIDI ("Rakarrack IN") settings -- a native
		// port of rakarrack.cxx's Midi_In_Counter/Har_In_Counter/Mw0+Mw1
		// (Fl_Preferences "MIDI IN Channel"/"MIDI IN Harmonizer"/"MIDI
		// Implementation"). "In Ch" gates Program Change (preset
		// switching) and Control Change (parameter control, see
		// RKR::process_midi_controller_events() in src/rkrMIDI.C); "Har
		// Ch" is the separate channel the Harmonizer/StereoHarm "MIDI"
		// toggle listens on for chord input (RecChord::MiraChord()); "CC
		// Map" (labels match Mw0/Mw1 exactly) picks which table Control
		// Change messages are looked up in. "MIDI Learn" isn't much use
		// without a way to fill in XUserMIDI's per-CC assignments --
		// rakarrack.cxx's own MIDI-learn UI for that ("ML_Menu") isn't
		// ported here -- so it's included for parity/no-surprises rather
		// than because it's fully usable yet; "Default" (the default
		// either way) is the built-in CC map real rakarrack ships with,
		// and needs no extra setup.
		BGroupView* midiRow3 = new BGroupView(B_HORIZONTAL, 24);
		midiRow3->SetViewColor(kPanelColor);
		midiBody->AddChild(midiRow3);

		AddSlider(midiRow3, "midi_in_ch", "In Ch", 1, 16,
			rkr->MidiCh + 1,
			[rkr](int32 v) { rkr->MidiCh = v - 1; },
			kHeaderSliderWidth);

		AddSlider(midiRow3, "midi_har_ch", "Har Ch", 1, 16,
			rkr->HarCh + 1,
			[rkr](int32 v) { rkr->HarCh = v - 1; },
			kHeaderSliderWidth);

		AddTypeMenu(midiRow3, "midi_ccmap", "CC Map", kCCMapNames,
			rkr->MIDIway,
			[rkr](int32 v) { rkr->MIDIway = v; });

		midiBox->AddChild(midiContent);

		// Row 3: Input Gain/Master Volume next to the scope.
		BGroupView* meterRow = new BGroupView(B_HORIZONTAL, 10);
		meterRow->GroupLayout()->SetInsets(10, 0, 10, 6);
		meterRow->SetViewColor(kBgColor);
		meterRow->AddChild(volumeGroup);
		meterRow->AddChild(scope);
		meterRow->GroupLayout()->AddItem(BSpaceLayoutItem::CreateGlue());

		// Row 4: the MIDI panel, wrapped below row 3 instead of crowding it
		// on one line.
		BGroupView* midiRow = new BGroupView(B_HORIZONTAL, 10);
		midiRow->GroupLayout()->SetInsets(10, 0, 10, 10);
		midiRow->SetViewColor(kBgColor);
		midiRow->AddChild(midiBox);
		midiRow->GroupLayout()->AddItem(BSpaceLayoutItem::CreateGlue());

		BGroupView* logoRow = new BGroupView(B_HORIZONTAL, 10);
		logoRow->GroupLayout()->SetInsets(10, 10, 10, 4);
		logoRow->SetViewColor(kBgColor);
		logoRow->AddChild(logo);
		logoRow->GroupLayout()->AddItem(BSpaceLayoutItem::CreateGlue());

		BLayoutBuilder::Group<>(headerParent, B_VERTICAL, 0)
			.Add(logoRow)
			.Add(controls)
			.Add(meterRow)
			.Add(midiRow)
			.End();
	}

	virtual void AttachedToWindow()
	{
		BView::AttachedToWindow();
		for (BMenu* menu : fMenus)
			menu->SetTargetForItems(Window());
	}

	// RakarrackWindow's scroll bars are hidden (see start_haiku_native_
	// interface()) so the rack reads as one continuous dark surface
	// instead of being framed by chrome -- this is the only remaining way
	// to move around when the content is taller/wider than the window.
	// A B_MOUSE_WHEEL_CHANGED with no scroll bar to handle it bubbles up
	// from whichever child view the mouse is over (sliders, checkboxes,
	// menus, the column groups, the effect boxes -- none of those have
	// scroll bars either) until something consumes it; this view wraps
	// all of that, so it is always eventually reached. Plain wheel moves
	// vertically; the common desktop convention (Shift+wheel, which many
	// mice/trackpads also report directly as a horizontal delta) moves
	// horizontally.
	virtual void MessageReceived(BMessage* msg)
	{
		if (msg->what == B_MOUSE_WHEEL_CHANGED) {
			float dx = 0.0f, dy = 0.0f;
			msg->FindFloat("be:wheel_delta_x", &dx);
			msg->FindFloat("be:wheel_delta_y", &dy);
			const float kStep = 32.0f;
			if (dx != 0.0f)
				ScrollBy(dx * kStep, 0.0f);
			if (dy != 0.0f)
				ScrollBy(0.0f, dy * kStep);
			return;
		}
		BView::MessageReceived(msg);
	}

	virtual void Pulse()
	{
		if (!fRkr)
			return;
		char cpuBuf[32];
		sprintf(cpuBuf, "CPU: %5.2f%%", (float)fRkr->cpuload);
		fCpuDisplay->SetText(cpuBuf);

		// Apply any AddDebouncedSlider() value that's settled -- no further
		// drag movement for kDebounceUsec -- since the last check. Locked
		// like every other real engine mutation: the real-time audio
		// callback needs this same lock, which is exactly why these calls
		// are being held back from firing on every drag step in the first
		// place.
		static const bigtime_t kDebounceUsec = 150000; // 150ms
		bigtime_t now = system_time();
		for (DebouncedSlider& d : fDebounced) {
			if (d.hasPending && (now - d.lastChangeTime) >= kDebounceUsec) {
				d.hasPending = false;
				pthread_mutex_lock(&jmutex);
				d.apply(d.pendingValue);
				pthread_mutex_unlock(&jmutex);
			}
		}
	}

	// Called by RakarrackWindow::MessageReceived (already holding jmutex)
	// for every MSG_ACTION.
	void Dispatch(BMessage* msg)
	{
		int32 aidx;
		if (msg->FindInt32("aidx", &aidx) != B_OK)
			return;
		if (aidx < 0 || (size_t)aidx >= fActions.size())
			return;

		int32 value;
		if (msg->FindInt32("val", &value) != B_OK)
			value = msg->GetInt32("be:value", 0);

		fActions[aidx](value);
	}

	// Called by RakarrackWindow::MessageReceived for MSG_OPEN_ORDER --
	// deliberately NOT under jmutex (see that message's declaration for
	// why). Opens the Effects Order window, building it the first time and
	// just refreshing/un-hiding/raising it after that.
	void OpenOrderWindow()
	{
		if (fOrderWindow == nullptr) {
			// A just-constructed BWindow is locked to the constructing
			// thread until its first Unlock() (which Show() takes care of)
			// -- safe without an explicit Lock() here.
			fOrderWindow = new OrderWindow(fRkr);
			fOrderWindow->Show();
			return;
		}
		// Once shown, a BWindow runs its own message loop on its own
		// thread -- reaching into it from here (a different thread) needs
		// its lock held first, unlike the just-constructed case above.
		if (fOrderWindow->Lock()) {
			fOrderWindow->Refresh();
			if (fOrderWindow->IsHidden())
				fOrderWindow->Show();
			fOrderWindow->Activate();
			fOrderWindow->Unlock();
		}
	}

private:
	// Registers a callback and returns its slot; used by every control.
	int32 Bind(std::function<void(int32)> fn)
	{
		fActions.push_back(fn);
		return (int32)(fActions.size() - 1);
	}

	BMessage* MakeMessage(int32 actionIndex, int32 explicitValue = -0x7fffffff)
	{
		BMessage* msg = new BMessage(MSG_ACTION);
		msg->AddInt32("aidx", actionIndex);
		if (explicitValue != -0x7fffffff)
			msg->AddInt32("val", explicitValue);
		return msg;
	}

	// label ... value ... bar, one row, mirroring rakarrack.cxx's SliderW
	// (which draws its own live numeric readout next to the track --
	// BSlider has no such thing built in, so this adds a plain BStringView
	// next to it and keeps it in sync on every value change). Label/value
	// widths are fixed so sliders line up across a whole effect box.
	BSlider* AddSlider(BView* parent, const char* name, const char* label,
		int32 min, int32 max, int32 initial, std::function<void(int32)> fn,
		// Every effect box slider relies on the 190px default; only the
		// header's Input Gain/Master Volume/MIDI Channel/MIDI Trigger pass
		// something narrower (see BuildHeader) to keep that row compact.
		int32 width = 190)
	{
		BGroupView* row = new BGroupView(B_HORIZONTAL, 6);
		row->SetViewColor(kPanelColor);

		BStringView* labelView = new BStringView("lbl", label);
		labelView->SetHighColor(kLabelColor);
		labelView->SetLowColor(kPanelColor);
		labelView->SetExplicitMinSize(BSize(72, B_SIZE_UNSET));
		labelView->SetExplicitMaxSize(BSize(72, B_SIZE_UNSET));
		labelView->SetFontSize(10.5f);

		BStringView* valueView = new BStringView("val", "");
		valueView->SetHighColor(kValueColor);
		valueView->SetLowColor(kPanelColor);
		valueView->SetExplicitMinSize(BSize(32, B_SIZE_UNSET));
		valueView->SetExplicitMaxSize(BSize(32, B_SIZE_UNSET));
		valueView->SetExplicitAlignment(
			BAlignment(B_ALIGN_RIGHT, B_ALIGN_VERTICAL_CENTER));
		valueView->SetFontSize(10.5f);
		char buf[16];
		snprintf(buf, sizeof(buf), "%d", (int)initial);
		valueView->SetText(buf);

		int32 idx = Bind([fn, valueView](int32 v) {
			char buf[16];
			snprintf(buf, sizeof(buf), "%d", (int)v);
			valueView->SetText(buf);
			fn(v);
		});

		BSlider* s = new BSlider(name, NULL, MakeMessage(idx), min, max,
			B_HORIZONTAL);
		s->SetValue(initial);
		s->SetHashMarks(B_HASH_MARKS_NONE);
		s->SetViewColor(kPanelColor);
		s->SetBarColor(kAccentColor);
		// Roughly doubles the effect boxes' width over the default track
		// size -- cramped sliders were hard to drag precisely.
		s->SetExplicitMinSize(BSize(width, B_SIZE_UNSET));

		row->AddChild(labelView);
		row->AddChild(valueView);
		row->AddChild(s);
		parent->AddChild(row);
		return s;
	}

	// Same look and feel as AddSlider, but the actual fn(v) call -- the
	// real changepar(), which for a handful of parameters (see ParamDef's
	// "debounced" comment) does real, possibly-slow DSP work -- is held
	// until ~150ms after the value stops changing, applied from Pulse()
	// below, rather than firing on every single drag step.
	BSlider* AddDebouncedSlider(BView* parent, const char* name,
		const char* label, int32 min, int32 max, int32 initial,
		std::function<void(int32)> fn)
	{
		BGroupView* row = new BGroupView(B_HORIZONTAL, 6);
		row->SetViewColor(kPanelColor);

		BStringView* labelView = new BStringView("lbl", label);
		labelView->SetHighColor(kLabelColor);
		labelView->SetLowColor(kPanelColor);
		labelView->SetExplicitMinSize(BSize(72, B_SIZE_UNSET));
		labelView->SetExplicitMaxSize(BSize(72, B_SIZE_UNSET));
		labelView->SetFontSize(10.5f);

		BStringView* valueView = new BStringView("val", "");
		valueView->SetHighColor(kValueColor);
		valueView->SetLowColor(kPanelColor);
		valueView->SetExplicitMinSize(BSize(32, B_SIZE_UNSET));
		valueView->SetExplicitMaxSize(BSize(32, B_SIZE_UNSET));
		valueView->SetExplicitAlignment(
			BAlignment(B_ALIGN_RIGHT, B_ALIGN_VERTICAL_CENTER));
		valueView->SetFontSize(10.5f);
		char buf[16];
		snprintf(buf, sizeof(buf), "%d", (int)initial);
		valueView->SetText(buf);

		// fDebounced is a deque specifically because it never invalidates
		// references to existing elements when it grows (unlike a vector,
		// which can reallocate) -- entry's address has to stay valid for
		// the lifetime of the lambda below, which outlives this call.
		fDebounced.emplace_back();
		DebouncedSlider* entry = &fDebounced.back();
		entry->apply = fn;

		int32 idx = Bind([entry, valueView](int32 v) {
			char buf[16];
			snprintf(buf, sizeof(buf), "%d", (int)v);
			valueView->SetText(buf);
			entry->hasPending = true;
			entry->pendingValue = v;
			entry->lastChangeTime = system_time();
		});

		BSlider* s = new BSlider(name, NULL, MakeMessage(idx), min, max,
			B_HORIZONTAL);
		s->SetValue(initial);
		s->SetHashMarks(B_HASH_MARKS_NONE);
		s->SetViewColor(kPanelColor);
		s->SetBarColor(kAccentColor);
		s->SetExplicitMinSize(BSize(190, B_SIZE_UNSET));

		row->AddChild(labelView);
		row->AddChild(valueView);
		row->AddChild(s);
		parent->AddChild(row);
		return s;
	}

	BCheckBox* AddToggle(BView* parent, const char* name, const char* label,
		bool initial, std::function<void(int32)> fn)
	{
		int32 idx = Bind(fn);
		BCheckBox* c = new BCheckBox(name, label, MakeMessage(idx));
		c->SetValue(initial ? B_CONTROL_ON : B_CONTROL_OFF);
		c->SetViewColor(kPanelColor);
		parent->AddChild(c);
		return c;
	}

	// A momentary press that sends a fixed value -- Looper's transport
	// controls (Play/Stop/Record/...) are plain Fl_Button presses in
	// rakarrack.cxx, each just a changepar(npar, 1) call, not a toggle with
	// its own on/off state.
	BButton* AddButton(BView* parent, const char* name, const char* label,
		int32 pressValue, std::function<void(int32)> fn)
	{
		int32 idx = Bind(fn);
		BButton* b = new BButton(name, label, MakeMessage(idx, pressValue));
		b->SetViewColor(kPanelColor);
		parent->AddChild(b);
		return b;
	}

	BMenuField* AddTypeMenu(BView* parent, const char* name, const char* label,
		const std::vector<std::string>& items, int32 initial,
		std::function<void(int32)> fn)
	{
		int32 idx = Bind(fn);
		BPopUpMenu* menu = new BPopUpMenu(label);
		for (size_t i = 0; i < items.size(); i++) {
			BMenuItem* item = new BMenuItem(items[i].c_str(),
				MakeMessage(idx, (int32)i));
			menu->AddItem(item);
			if ((int32)i == initial)
				item->SetMarked(true);
		}
		fMenus.push_back(menu);
		BMenuField* field = new BMenuField(name, label, menu);
		field->SetViewColor(kPanelColor);
		parent->AddChild(field);
		return field;
	}

	// One "rack box" -- title, on/off, an optional type menu, optional plain
	// toggles, then every parameter slider. changeFn/getFn wrap whichever
	// method the effect actually exposes (changepar, Compressor_Change,
	// Gate_Change...) so the rest of this stays effect-agnostic.
	void BuildEffectBox(BView* column, const char* title, RKR* rkr,
		int effectId, int* bypass,
		std::function<void(int32, int32)> changeFn,
		std::function<int32(int32)> getFn,
		const std::vector<ParamDef>& params,
		const std::vector<ToggleDef>& toggles = std::vector<ToggleDef>(),
		const std::vector<TypeMenuDef>& typeMenus = std::vector<TypeMenuDef>(),
		// Escape hatch for controls that don't fit the slider/toggle/dropdown
		// model -- Looper's transport buttons are the only user of this so
		// far. Called with the effect's body BGroupView after every param
		// above has already been added to it.
		std::function<void(BView*)> extraWidgets = nullptr,
		// Added at the very end, after every other optional parameter, so
		// every existing call site above (with whatever mix of toggles/
		// typeMenus/extraWidgets it already passes) keeps compiling
		// unchanged -- see PresetMenuDef.
		const PresetMenuDef& preset = PresetMenuDef())
	{
		BBox* box = new BBox(title);
		box->SetViewColor(kPanelColor);
		// A plain SetLabel(title) draws the title in the system theme's
		// label color; a BStringView label lets it use the gold accent
		// color instead, matching rakarrack.cxx's titles.
		BStringView* titleView = new BStringView("title", title);
		titleView->SetHighColor(kTitleColor);
		titleView->SetLowColor(kBgColor);
		BFont titleFont(be_bold_font);
		titleView->SetFont(&titleFont);
		box->SetLabel(titleView);

		BGroupView* content = new BGroupView(B_VERTICAL, 4);
		content->GroupLayout()->SetInsets(8);
		content->SetViewColor(kPanelColor);

		// Everything but the "On" checkbox itself -- hidden whenever the
		// effect is off, so an inactive effect collapses to just its title
		// bar instead of eating vertical space in the rack.
		BGroupView* body = new BGroupView(B_VERTICAL, 4);
		body->SetViewColor(kPanelColor);

		// If a loaded bank/preset already has this effect's Bypass flag on
		// by the time this box is built, claim its chain slot right away so
		// the checkbox's initial "on" state matches what's actually
		// audible. If the chain is somehow already full at startup, fall
		// back to showing it off rather than lying in the UI.
		if (*bypass != 0 && !ActivateEffectSlot(rkr, effectId))
			*bypass = 0;

		// Built directly (not through AddToggle) because the callback below
		// needs to reach back into the checkbox itself to revert it when
		// ActivateEffectSlot() fails -- AddToggle hands back its BCheckBox*
		// only after the callback that would need it is already built.
		BCheckBox* onToggle = new BCheckBox("on", "On", nullptr);
		onToggle->SetValue(*bypass != 0 ? B_CONTROL_ON : B_CONTROL_OFF);
		onToggle->SetViewColor(kPanelColor);
		int32 onIdx = Bind([rkr, effectId, bypass, body, onToggle](int32 v) {
			if (v) {
				if (!ActivateEffectSlot(rkr, effectId)) {
					// All 10 chain slots are taken by other active effects
					// -- refuse rather than silently turn on a pedal that
					// will never actually process audio. Turn off one of
					// the other active effects first.
					onToggle->SetValue(B_CONTROL_OFF);
					return;
				}
			} else {
				DeactivateEffectSlot(rkr, effectId);
			}
			*bypass = v ? 1 : 0;
			if (v)
				body->Show();
			else
				body->Hide();
		});
		onToggle->SetMessage(MakeMessage(onIdx));
		content->AddChild(onToggle);
		if (*bypass == 0)
			body->Hide();
		content->AddChild(body);

		// Rendered first, same as rakarrack.cxx's own effect panels (the
		// preset Fl_Choice always sits above every slider/toggle/type menu).
		if (preset.items && preset.apply)
			AddTypeMenu(body, "Preset", "Preset", *preset.items, 0, preset.apply);

		for (const TypeMenuDef& t : typeMenus) {
			AddTypeMenu(body, t.label, t.label, *t.items,
				getFn(t.npar) - t.offset,
				[changeFn, t](int32 v) { changeFn(t.npar, v + t.offset); });
		}

		for (const ToggleDef& t : toggles) {
			AddToggle(body, t.label, t.label, getFn(t.npar) != 0,
				[changeFn, t](int32 v) { changeFn(t.npar, v); });
		}

		for (const ParamDef& p : params) {
			if (p.debounced) {
				AddDebouncedSlider(body, p.label, p.label, p.min, p.max,
					getFn(p.npar) - p.offset,
					[changeFn, p](int32 v) { changeFn(p.npar, v + p.offset); });
			} else {
				AddSlider(body, p.label, p.label, p.min, p.max,
					getFn(p.npar) - p.offset,
					[changeFn, p](int32 v) { changeFn(p.npar, v + p.offset); });
			}
		}

		if (extraWidgets)
			extraWidgets(body);

		box->AddChild(content);
		column->AddChild(box);
	}

	// Four columns instead of five -- with each slider now ~2x as wide (see
	// AddSlider), five columns needed more width than fits comfortably on
	// smaller screens. Effects are grouped by roughly how many slider rows
	// each box needs (not just effect count) so the four columns end up
	// close to the same height rather than one column towering over the
	// rest -- Exciter alone (13 rows: gain + 10 harmonics + 2 filters) is
	// close to as tall as three or four small boxes put together.
	void BuildColumn1(BView* col)
	{
		RKR* rkr = fRkr;

		{
			std::vector<ParamDef> exciterParams = {
				{"Gain", 0, 127, 0, 0},
			};
			for (int h = 1; h <= 10; h++) {
				static char labels[10][8];
				snprintf(labels[h - 1], sizeof(labels[h - 1]), "Har %d", h);
				exciterParams.push_back({labels[h - 1], -64, 64, h, 0});
			}
			exciterParams.push_back({"LPF", 20, 26000, 11, 0});
			exciterParams.push_back({"HPF", 20, 20000, 12, 0});
			BuildEffectBox(col, "Exciter", rkr, 22, &rkr->Exciter_Bypass,
				[rkr](int32 n, int32 v) { rkr->efx_Exciter->changepar(n, v); },
				[rkr](int32 n) { return rkr->efx_Exciter->getpar(n); },
				exciterParams, {}, {}, nullptr,
				{&kExciterPresetNames, [rkr](int32 v) { rkr->efx_Exciter->setpreset(v); }});
		}

		BuildEffectBox(col, "Compressor", rkr, 1, &rkr->Compressor_Bypass,
			[rkr](int32 n, int32 v) { rkr->efx_Compressor->Compressor_Change(n, v); },
			[rkr](int32 n) { return rkr->efx_Compressor->getpar(n); },
			{
				{"A. Time", 10, 250, 4, 0},
				{"R. Time", 10, 500, 5, 0},
				{"Ratio", 2, 42, 2, 0},
				{"Knee", 0, 100, 7, 0},
				{"Threshold", -60, -3, 1, 0},
				{"Output", -40, 0, 3, 0},
			}, {}, {}, nullptr,
			{&kCompressorPresetNames, [rkr](int32 v) { rkr->efx_Compressor->Compressor_Change_Preset(1, v); }});

		BuildEffectBox(col, "Valve", rkr, 19, &rkr->Valve_Bypass,
			[rkr](int32 n, int32 v) { rkr->efx_Valve->changepar(n, v); },
			[rkr](int32 n) { return rkr->efx_Valve->getpar(n); },
			{
				{"Drive", 0, 127, 3, 0},
				{"Level", 0, 127, 4, 0},
				{"Dist.", 0, 127, 10, 0},
				{"Presence", 0, 100, 12, 0},
				{"LPF", 20, 26000, 6, 0},
				{"HPF", 20, 20000, 7, 0},
			}, {}, {}, nullptr,
			{&kValvePresetNames, [rkr](int32 v) { rkr->efx_Valve->setpreset(v); }});

		BuildEffectBox(col, "Vibe", rkr, 45, &rkr->Vibe_Bypass,
			[rkr](int32 n, int32 v) { rkr->efx_Vibe->changepar(n, v); },
			[rkr](int32 n) { return rkr->efx_Vibe->getpar(n); },
			{
				{"Tempo", 1, 600, 1, 0},
				{"Width", 0, 127, 0, 0},
				{"Depth", 0, 127, 8, 0},
				{"Feedback", -64, 64, 7, 64},
				{"L/R Cr.", -64, 64, 9, 64},
			}, {}, {}, nullptr,
			{&kVibePresetNames, [rkr](int32 v) { rkr->efx_Vibe->setpreset(v); }});

		BuildEffectBox(col, "Auto Pan", rkr, 13, &rkr->Pan_Bypass,
			[rkr](int32 n, int32 v) { rkr->efx_Pan->changepar(n, v); },
			[rkr](int32 n) { return rkr->efx_Pan->getpar(n); },
			{
				{"Tempo", 1, 600, 2, 0},
				{"Extra", 0, 127, 6, 0},
			},
			{
				{"Auto Pan", 7},
				{"Extra On", 8},
			}, {}, nullptr,
			{&kPanPresetNames, [rkr](int32 v) { rkr->efx_Pan->setpreset(v); }});

		BuildEffectBox(col, "Reverbtron", rkr, 40, &rkr->Reverbtron_Bypass,
			[rkr](int32 n, int32 v) { rkr->efx_Reverbtron->changepar(n, v); },
			[rkr](int32 n) { return rkr->efx_Reverbtron->getpar(n); },
			{
				{"Wet/Dry", -64, 64, 0, 64},
				{"Pan", -64, 63, 11, 64},
				{"Level", 0, 127, 7, 0},
				{"Damp", 0, 127, 6, 0},
				{"Fb", -64, 64, 10, 0},
				{"Length", 20, 1500, 3, 0, true},
				{"Stretch", -64, 64, 9, 0},
				{"I.Del", 0, 500, 5, 0},
				{"Fade", 0, 127, 1, 0},
				{"Diffusion", 0, 127, 15, 0},
				{"LPF", 20, 26000, 14, 0},
			},
			{
				{"Sh", 13},
				{"ES", 12},
				{"Safe", 2},
			},
			{{"IR", &kReverbtronIRNames, 8}}, nullptr,
			{&kReverbtronPresetNames, [rkr](int32 v) { rkr->efx_Reverbtron->setpreset(v); }});

		BuildEffectBox(col, "MusDelay", rkr, 15, &rkr->MusDelay_Bypass,
			[rkr](int32 n, int32 v) { rkr->efx_MusDelay->changepar(n, v); },
			[rkr](int32 n) { return rkr->efx_MusDelay->getpar(n); },
			{
				{"Wet/Dry", -64, 63, 0, 64},
				{"L/R Cr.", -64, 63, 4, 64},
				{"Pan1", -64, 63, 1, 64},
				{"Pan2", -64, 63, 7, 64},
				{"Tempo", 10, 480, 10, 0},
				{"Gain1", -64, 63, 11, 64},
				{"Gain2", -64, 63, 12, 64},
				{"Fb1.", 0, 127, 5, 0},
				{"Fb2.", 0, 127, 9, 0},
				{"Damp", 0, 127, 6, 0},
			},
			{},
			{
				{"Div 1", &kSubDivNames, 2, 1},
				{"Div 2", &kSubDivNames, 8, 1},
				{"Div 3", &kMusDelayDiv7Names, 3, 1},
			}, nullptr,
			{&kMusDelayPresetNames, [rkr](int32 v) { rkr->efx_MusDelay->setpreset(v); }});

		BuildEffectBox(col, "CompBand", rkr, 43, &rkr->CompBand_Bypass,
			[rkr](int32 n, int32 v) { rkr->efx_CompBand->changepar(n, v); },
			[rkr](int32 n) { return rkr->efx_CompBand->getpar(n); },
			{
				{"Wet/Dry", -64, 64, 0, 64},
				{"Gain", 0, 127, 12, 0},
				{"L Ratio", 2, 42, 1, 0},
				{"ML Ratio", 2, 42, 2, 0},
				{"MH Ratio", 2, 42, 3, 0},
				{"H Ratio", 2, 42, 4, 0},
				{"L Thres", -70, 24, 5, 0},
				{"ML Thres", -70, 24, 6, 0},
				{"MH Thres", -70, 24, 7, 0},
				{"H Thres", -70, 24, 8, 0},
				{"Cross1", 20, 1000, 9, 0},
				{"Cross2", 1000, 8000, 10, 0},
				{"Cross3", 2000, 26000, 11, 0},
			}, {}, {}, nullptr,
			{&kCompBandPresetNames, [rkr](int32 v) { rkr->efx_CompBand->setpreset(v); }});

		BuildEffectBox(col, "EQ2", rkr, 9, &rkr->EQ2_Bypass,
			[rkr](int32 n, int32 v) { rkr->efx_EQ2->changepar(n, v); },
			[rkr](int32 n) { return rkr->efx_EQ2->getpar(n); },
			{
				{"Gain", -64, 63, 0, 64},
				{"Low F.", 20, 1000, 11, 0},
				{"Low G.", -64, 63, 12, 64},
				{"Low Q", -64, 63, 13, 64},
				{"Mid F.", 800, 8000, 16, 0},
				{"Mid G.", -64, 63, 17, 64},
				{"Mid Q", -64, 63, 18, 64},
				{"High F.", 6000, 26000, 21, 0},
				{"High G.", -64, 63, 22, 64},
				{"High Q", -64, 63, 23, 64},
			}, {}, {}, nullptr,
			{&kEQ2PresetNames, [rkr](int32 v) { rkr->EQ2_setpreset(v); }});

		BuildEffectBox(col, "Arpie", rkr, 24, &rkr->Arpie_Bypass,
			[rkr](int32 n, int32 v) { rkr->efx_Arpie->changepar(n, v); },
			[rkr](int32 n) { return rkr->efx_Arpie->getpar(n); },
			{
				{"Wet/Dry", -64, 63, 0, 64},
				{"Arpe's", 0, 127, 7, 0},
				{"Pan", -64, 63, 1, 64},
				{"Tempo", 1, 600, 2, 0},
				{"LRdl.", 0, 127, 3, 0},
				{"L/R Cr.", -64, 63, 4, 64},
				{"Fb.", 0, 127, 5, 0},
				{"Damp", 0, 127, 6, 0},
				{"Steps", 1, 8, 8, 0},
			},
			{},
			{
				{"SubDiv", &kSubDivNames, 12, 0},
				{"Pattern", &kArpiePatternNames, 9, 0},
			}, nullptr,
			{&kArpiePresetNames, [rkr](int32 v) { rkr->efx_Arpie->setpreset(v); }});

		BuildEffectBox(col, "Vocoder", rkr, 35, &rkr->Vocoder_Bypass,
			[rkr](int32 n, int32 v) { rkr->efx_Vocoder->changepar(n, v); },
			[rkr](int32 n) { return rkr->efx_Vocoder->getpar(n); },
			{
				{"Wet/Dry", -64, 64, 0, 64},
				{"Pan", -64, 64, 1, 64},
				{"Input", 0, 127, 4, 0},
				{"Muf.", 1, 127, 2, 0},
				{"Q", 40, 170, 3, 0},
				{"Ring", 0, 127, 6, 0},
				{"Level", 0, 127, 5, 0},
			}, {}, {}, nullptr,
			{&kVocoderPresetNames, [rkr](int32 v) { rkr->efx_Vocoder->setpreset(v); }});
	}

	void BuildColumn2(BView* col)
	{
		RKR* rkr = fRkr;

		{
			static const char* kBandLabels[10] = {
				"31 Hz", "63 Hz", "125 Hz", "250 Hz", "500 Hz", "1 Khz",
				"2 Khz", "4 Khz", "8 Khz", "16 Khz"
			};
			std::vector<ParamDef> bands;
			for (int i = 0; i < 10; i++)
				bands.push_back({kBandLabels[i], -64, 63, 10 + i * 5 + 2, 64});
			BuildEffectBox(col, "Equalizer", rkr, 0, &rkr->EQ1_Bypass,
				[rkr](int32 n, int32 v) { rkr->efx_EQ1->changepar(n, v); },
				[rkr](int32 n) { return rkr->efx_EQ1->getpar(n); },
				bands, {}, {}, nullptr,
				{&kEQ1PresetNames, [rkr](int32 v) { rkr->EQ1_setpreset(v); }});
		}

		BuildEffectBox(col, "Analog Phaser", rkr, 18, &rkr->APhaser_Bypass,
			[rkr](int32 n, int32 v) { rkr->efx_APhaser->changepar(n, v); },
			[rkr](int32 n) { return rkr->efx_APhaser->getpar(n); },
			{
				{"Tempo", 1, 600, 2, 0},
				{"Width", 0, 127, 6, 0},
				{"Depth", 0, 127, 11, 0},
				{"Feedback", -64, 64, 7, 64},
				{"Distort", 0, 100, 1, 0},
				{"Mismatch", 0, 100, 9, 0},
				{"Stereo", 0, 127, 5, 0},
			}, {}, {}, nullptr,
			{&kAPhaserPresetNames, [rkr](int32 v) { rkr->efx_APhaser->setpreset(v); }});

		BuildEffectBox(col, "Phaser", rkr, 6, &rkr->Phaser_Bypass,
			[rkr](int32 n, int32 v) { rkr->efx_Phaser->changepar(n, v); },
			[rkr](int32 n) { return rkr->efx_Phaser->getpar(n); },
			{
				{"Tempo", 1, 600, 2, 0},
				{"Depth", 0, 127, 6, 0},
				{"Feedback", 0, 127, 7, 0},
				{"Phase", 0, 127, 11, 0},
				{"Stereo", 0, 127, 5, 0},
				{"L/R Cr.", -64, 63, 9, 64},
			}, {}, {}, nullptr,
			{&kPhaserPresetNames, [rkr](int32 v) { rkr->efx_Phaser->setpreset(v); }});

		BuildEffectBox(col, "Overdrive", rkr, 3, &rkr->Overdrive_Bypass,
			[rkr](int32 n, int32 v) { rkr->efx_Overdrive->changepar(n, v); },
			[rkr](int32 n) { return rkr->efx_Overdrive->getpar(n); },
			{
				{"Drive", 0, 127, 3, 0},
				{"Level", 0, 127, 4, 0},
				{"LPF", 20, 26000, 7, 0},
				{"HPF", 20, 20000, 8, 0},
			},
			{}, {{"Type", &kDistTypeNames, 5}}, nullptr,
			{&kOverdrivePresetNames, [rkr](int32 v) { rkr->efx_Overdrive->setpreset(1, v); }});

		BuildEffectBox(col, "Opticaltrem", rkr, 44, &rkr->Opticaltrem_Bypass,
			[rkr](int32 n, int32 v) { rkr->efx_Opticaltrem->changepar(n, v); },
			[rkr](int32 n) { return rkr->efx_Opticaltrem->getpar(n); },
			{
				{"Depth", 0, 127, 0, 0},
				{"Tempo", 1, 600, 1, 0},
				{"Rnd", 0, 127, 2, 0},
				{"Stereo", 0, 127, 4, 0},
			}, {}, {}, nullptr,
			{&kOpticaltremPresetNames, [rkr](int32 v) { rkr->efx_Opticaltrem->setpreset(v); }});

		BuildEffectBox(col, "MBDist", rkr, 23, &rkr->MBDist_Bypass,
			[rkr](int32 n, int32 v) { rkr->efx_MBDist->changepar(n, v); },
			[rkr](int32 n) { return rkr->efx_MBDist->getpar(n); },
			{
				{"Wet/Dry", -64, 64, 0, 64},
				{"L/R Cr.", -64, 64, 2, 64},
				{"Drive", 0, 127, 3, 0},
				{"Level", 0, 127, 4, 0},
				{"L.Gain", 0, 100, 8, 0},
				{"M.Gain", 0, 100, 9, 0},
				{"H.Gain", 0, 100, 10, 0},
				{"Cross1", 20, 1000, 12, 0},
				{"Cross2", 800, 12000, 13, 0},
				{"Pan", -64, 64, 1, 64},
			},
			{
				{"Stereo", 14},
				{"Neg.", 11},
			},
			{
				{"Type L", &kDistTypeNames, 5},
				{"Type M", &kDistTypeNames, 6},
				{"Type H", &kDistTypeNames, 7},
			}, nullptr,
			{&kMBDistPresetNames, [rkr](int32 v) { rkr->efx_MBDist->setpreset(v); }});

		BuildEffectBox(col, "Echotron", rkr, 41, &rkr->Echotron_Bypass,
			[rkr](int32 n, int32 v) { rkr->efx_Echotron->changepar(n, v); },
			[rkr](int32 n) { return rkr->efx_Echotron->getpar(n); },
			{
				{"Wet/Dry", -64, 64, 0, 64},
				{"Pan", -64, 63, 11, 64},
				{"Tempo", 1, 600, 5, 0},
				{"Damp", 0, 127, 6, 0},
				{"Fb", -64, 64, 10, 0},
				{"L/R Cr.", -64, 64, 7, 64},
				{"Width", 0, 127, 2, 0},
				{"Depth", -64, 64, 1, 64},
				{"St.df", 0, 127, 9, 0},
				{"#", 1, 127, 3, 0, true},
			},
			{
				{"AF", 15},
				{"MF", 13},
				{"MD", 12},
			},
			{
				{"LFO Type", &kLfoTypeNames, 14},
				{"IR", &kEchotronIRNames, 8},
			}, nullptr,
			{&kEchotronPresetNames, [rkr](int32 v) { rkr->efx_Echotron->setpreset(v); }});

		BuildEffectBox(col, "Distorsion", rkr, 2, &rkr->Distorsion_Bypass,
			[rkr](int32 n, int32 v) { rkr->efx_Distorsion->changepar(n, v); },
			[rkr](int32 n) { return rkr->efx_Distorsion->getpar(n); },
			{
				{"Wet/Dry", -64, 63, 0, 64},
				{"L/R Cr.", -64, 63, 2, 64},
				{"Drive", 0, 127, 3, 0},
				{"Level", 0, 127, 4, 0},
				{"Pan", -64, 63, 1, 64},
				{"Sub Octv", 0, 127, 12, 0},
				{"LPF", 20, 26000, 7, 0},
				{"HPF", 20, 20000, 8, 0},
			},
			{
				{"Neg.", 6},
				{"Pre Filter", 10},
				{"Stereo", 9},
			},
			{{"Type", &kDistTypeNames, 5}}, nullptr,
			{&kDistorsionPresetNames, [rkr](int32 v) { rkr->efx_Distorsion->setpreset(0, v + 2); }});

		BuildEffectBox(col, "Harmonizer", rkr, 14, &rkr->Harmonizer_Bypass,
			[rkr](int32 n, int32 v) { rkr->efx_Har->changepar(n, v); },
			[rkr](int32 n) { return rkr->efx_Har->getpar(n); },
			{
				{"Wet/Dry", -64, 63, 0, 64},
				{"Int.", -12, 12, 3, 12},
				{"Gain", -64, 63, 2, 64},
				{"Pan", -64, 63, 1, 64},
				{"Freq", 20, 26000, 4, 0},
				{"Gain 2", -64, 63, 8, 64},
				{"Q", -64, 63, 9, 64},
				{"Note", 0, 23, 6, 0},
				{"Chord", 0, 33, 7, 0},
			},
			{
				{"MIDI", 10},
				{"SEL", 5},
			}, {}, nullptr,
			{&kHarmonizerPresetNames, [rkr](int32 v) { rkr->efx_Har->setpreset(v); }});

		BuildEffectBox(col, "Shifter", rkr, 38, &rkr->Shifter_Bypass,
			[rkr](int32 n, int32 v) { rkr->efx_Shifter->changepar(n, v); },
			[rkr](int32 n) { return rkr->efx_Shifter->getpar(n); },
			{
				{"Wet/Dry", -64, 63, 0, 64},
				{"Int.", 0, 12, 6, 0},
				{"Gain", -64, 63, 2, 64},
				{"Pan", -64, 63, 1, 64},
				{"Attack", 1, 2000, 3, 0},
				{"Decay", 1, 2000, 4, 0},
				{"Threshold", -70, 20, 5, 0},
				{"Whamy", 0, 127, 9, 0},
			},
			{
				{"Down", 7},
			},
			{{"Mode", &kShifterModeNames, 8}}, nullptr,
			{&kShifterPresetNames, [rkr](int32 v) { rkr->efx_Shifter->setpreset(v); }});

		BuildEffectBox(col, "ShelfBoost", rkr, 34, &rkr->ShelfBoost_Bypass,
			[rkr](int32 n, int32 v) { rkr->efx_ShelfBoost->changepar(n, v); },
			[rkr](int32 n) { return rkr->efx_ShelfBoost->getpar(n); },
			{
				{"Gain", 0, 127, 0, 0},
				{"Level", 1, 127, 4, 0},
				{"Tone", 220, 16000, 2, 0},
				{"Pres.", -64, 64, 1, 0},
			},
			{
				{"Stereo", 3},
			}, {}, nullptr,
			{&kShelfBoostPresetNames, [rkr](int32 v) { rkr->efx_ShelfBoost->setpreset(v); }});

		BuildEffectBox(col, "Cabinet", rkr, 12, &rkr->Cabinet_Bypass,
			[rkr](int32 n, int32 v) { rkr->efx_Cabinet->changepar(n, v); },
			[rkr](int32 n) { return rkr->efx_Cabinet->getpar(n); },
			{
				{"Gain", -64, 63, 0, 64},
			}, {}, {}, nullptr,
			{&kCabinetPresetNames, [rkr](int32 v) { rkr->Cabinet_setpreset(v); }});
	}

	void BuildColumn3(BView* col)
	{
		RKR* rkr = fRkr;

		BuildEffectBox(col, "Ring Modulator", rkr, 21, &rkr->Ring_Bypass,
			[rkr](int32 n, int32 v) { rkr->efx_Ring->changepar(n, v); },
			[rkr](int32 n) { return rkr->efx_Ring->getpar(n); },
			{
				{"Input", 1, 127, 11, 0},
				{"Level", 0, 127, 3, 0},
				{"Depth", 0, 100, 4, 0},
				{"Freq", 1, 20000, 5, 0},
				{"Sin", 0, 100, 7, 0},
				{"Tri", 0, 100, 8, 0},
				{"Saw", 0, 100, 9, 0},
				{"Squ", 0, 100, 10, 0},
			}, {}, {}, nullptr,
			{&kRingPresetNames, [rkr](int32 v) { rkr->efx_Ring->setpreset(v); }});

		BuildEffectBox(col, "Reverb", rkr, 8, &rkr->Reverb_Bypass,
			[rkr](int32 n, int32 v) { rkr->efx_Rev->changepar(n, v); },
			[rkr](int32 n) { return rkr->efx_Rev->getpar(n); },
			{
				{"Time", 0, 127, 2, 0},
				{"I.Del", 0, 127, 3, 0},
				{"Del.E/R", 0, 127, 4, 0},
				{"LPF", 20, 26000, 7, 0},
				{"HPF", 20, 20000, 8, 0},
				{"Damp", 64, 127, 9, 0},
				{"R.Size", 1, 127, 11, 0},
			}, {}, {}, nullptr,
			{&kReverbPresetNames, [rkr](int32 v) { rkr->efx_Rev->setpreset(v); }});

		std::vector<ParamDef> chorusFlangerParams = {
			{"Tempo", 1, 600, 2, 0},
			{"Depth", 0, 127, 6, 0},
			{"Delay", 0, 127, 7, 0},
			{"Feedback", 0, 127, 8, 0},
			{"Stereo", 0, 127, 5, 0},
			{"L/R Cr.", -64, 63, 9, 64},
		};

		BuildEffectBox(col, "Flanger", rkr, 7, &rkr->Flanger_Bypass,
			[rkr](int32 n, int32 v) { rkr->efx_Flanger->changepar(n, v); },
			[rkr](int32 n) { return rkr->efx_Flanger->getpar(n); },
			chorusFlangerParams, {}, {}, nullptr,
			{&kFlangerPresetNames, [rkr](int32 v) { rkr->efx_Flanger->setpreset(1, v + 5); }});

		BuildEffectBox(col, "Alienwah", rkr, 11, &rkr->Alienwah_Bypass,
			[rkr](int32 n, int32 v) { rkr->efx_Alienwah->changepar(n, v); },
			[rkr](int32 n) { return rkr->efx_Alienwah->getpar(n); },
			{
				{"Tempo", 1, 600, 2, 0},
				{"Depth", 0, 127, 6, 0},
				{"Feedback", 0, 127, 7, 0},
				{"Delay", 0, 127, 8, 0},
				{"Phase", 0, 127, 10, 0},
			}, {}, {}, nullptr,
			{&kAlienwahPresetNames, [rkr](int32 v) { rkr->efx_Alienwah->setpreset(v); }});

		BuildEffectBox(col, "Echo", rkr, 4, &rkr->Echo_Bypass,
			[rkr](int32 n, int32 v) { rkr->efx_Echo->changepar(n, v); },
			[rkr](int32 n) { return rkr->efx_Echo->getpar(n); },
			{
				{"Delay", 20, 2000, 2, 0},
				{"Feedback", 0, 127, 5, 0},
				{"Damp", 0, 127, 6, 0},
				{"L/R Cr.", -64, 63, 4, 64},
			}, {}, {}, nullptr,
			{&kEchoPresetNames, [rkr](int32 v) { rkr->efx_Echo->setpreset(v); }});

		BuildEffectBox(col, "Sustainer", rkr, 36, &rkr->Sustainer_Bypass,
			[rkr](int32 n, int32 v) { rkr->efx_Sustainer->changepar(n, v); },
			[rkr](int32 n) { return rkr->efx_Sustainer->getpar(n); },
			{
				{"Gain", 0, 127, 0, 0},
				{"Sustain", 1, 127, 1, 0},
			}, {}, {}, nullptr,
			{&kSustainerPresetNames, [rkr](int32 v) { rkr->efx_Sustainer->setpreset(v); }});

		BuildEffectBox(col, "Synthfilter", rkr, 27, &rkr->Synthfilter_Bypass,
			[rkr](int32 n, int32 v) { rkr->efx_Synthfilter->changepar(n, v); },
			[rkr](int32 n) { return rkr->efx_Synthfilter->getpar(n); },
			{
				{"Wet/Dry", -64, 63, 0, 64},
				{"Distort", 0, 127, 1, 0},
				{"Tempo", 1, 600, 2, 0},
				{"St.df", 0, 127, 5, 0},
				{"Width", 0, 127, 6, 0},
				{"Fb", -64, 64, 7, 0},
				{"Depth", 0, 127, 11, 0},
				{"E.Sens", -64, 64, 12, 0},
				{"A.Time", 5, 1000, 13, 0},
				{"R.Time", 5, 500, 14, 0},
				{"Offset", 0, 127, 15, 0},
				{"LPF Stg.", 0, 12, 8, 0},
				{"HPF Stg.", 0, 12, 9, 0},
			},
			{
				{"Subtr.", 10},
			},
			{{"LFO Type", &kLfoTypeNames, 4}}, nullptr,
			{&kSynthfilterPresetNames, [rkr](int32 v) { rkr->efx_Synthfilter->setpreset(v); }});

		BuildEffectBox(col, "DFlange", rkr, 20, &rkr->DFlange_Bypass,
			[rkr](int32 n, int32 v) { rkr->efx_DFlange->changepar(n, v); },
			[rkr](int32 n) { return rkr->efx_DFlange->getpar(n); },
			{
				{"Wet/Dry", -64, 64, 0, 0},
				{"Pan", -64, 64, 1, 0},
				{"L/R Cr.", 0, 127, 2, 0},
				{"Depth", 20, 500, 3, 0},
				{"Width", 0, 3000, 4, 0},
				{"Offset", 0, 100, 5, 0},
				{"Fb", -64, 64, 6, 0},
				{"LPF", 20, 20000, 7, 0},
				{"Tempo", 1, 600, 10, 0},
				{"St.df", 0, 127, 11, 0},
				{"Rnd", 0, 127, 13, 0},
			},
			{
				{"Subtract", 8},
				{"Th. zero", 9},
			},
			{{"LFO Type", &kLfoTypeNames, 12}}, nullptr,
			{&kDFlangePresetNames, [rkr](int32 v) { rkr->efx_DFlange->setpreset(v); }});

		BuildEffectBox(col, "RyanWah", rkr, 31, &rkr->RyanWah_Bypass,
			[rkr](int32 n, int32 v) { rkr->efx_RyanWah->changepar(n, v); },
			[rkr](int32 n) { return rkr->efx_RyanWah->getpar(n); },
			{
				{"Wet/Dry", -64, 63, 0, 64},
				{"LP", -64, 64, 10, 0},
				{"BP", -64, 64, 11, 0},
				{"HP", -64, 64, 12, 0},
				{"Width", 0, 127, 6, 0},
				{"Tempo", 1, 600, 2, 0},
				{"Res.", 1, 127, 1, 0},
				{"Range", 10, 6000, 14, 0},
				{"Wah", 0, 127, 8, 0},
				{"E.Sens", -64, 64, 7, 0},
				{"Smooth", 0, 127, 9, 0},
				{"Stg", 1, 12, 13, 0},
			},
			{
				{"Mode", 17},
			},
			{{"LFO", &kLfoTypeNames, 4}}, nullptr,
			{&kRyanWahPresetNames, [rkr](int32 v) { rkr->efx_RyanWah->setpreset(v); }});

		BuildEffectBox(col, "Shuffle", rkr, 26, &rkr->Shuffle_Bypass,
			[rkr](int32 n, int32 v) { rkr->efx_Shuffle->changepar(n, v); },
			[rkr](int32 n) { return rkr->efx_Shuffle->getpar(n); },
			{
				{"Wet/Dry", -64, 64, 0, 64},
				{"Low Freq", 20, 1000, 5, 0},
				{"Low Gain", -64, 64, 1, 0},
				{"M.L Freq", 400, 4000, 6, 0},
				{"M.L Gain", -64, 64, 2, 0},
				{"M.H Freq", 1200, 8000, 7, 0},
				{"M.H Gain", -64, 64, 3, 0},
				{"High Freq", 6000, 26000, 8, 0},
				{"High Gain", -64, 64, 4, 0},
				{"Q", -64, 64, 9, 0},
			},
			{
				{"Rev", 10},
			}, {}, nullptr,
			{&kShufflePresetNames, [rkr](int32 v) { rkr->efx_Shuffle->setpreset(v); }});

		BuildEffectBox(col, "RBEcho", rkr, 32, &rkr->RBEcho_Bypass,
			[rkr](int32 n, int32 v) { rkr->efx_RBEcho->changepar(n, v); },
			[rkr](int32 n) { return rkr->efx_RBEcho->getpar(n); },
			{
				{"Wet/Dry", -64, 63, 0, 64},
				{"Reverse", 0, 127, 7, 0},
				{"Pan", -64, 63, 1, 64},
				{"Tempo", 1, 600, 2, 0},
				{"LRdl.", 0, 127, 3, 0},
				{"Fb.", 0, 127, 5, 0},
				{"Damp", 0, 127, 6, 0},
				{"E.S.", 0, 127, 9, 0},
				{"Angle", -64, 64, 4, 64},
			},
			{},
			{{"SubDiv", &kSubDivNames, 8}}, nullptr,
			{&kRBEchoPresetNames, [rkr](int32 v) { rkr->efx_RBEcho->setpreset(v); }});

		BuildEffectBox(col, "Convolotron", rkr, 29, &rkr->Convol_Bypass,
			[rkr](int32 n, int32 v) { rkr->efx_Convol->changepar(n, v); },
			[rkr](int32 n) { return rkr->efx_Convol->getpar(n); },
			{
				{"Wet/Dry", -64, 64, 0, 64},
				{"Pan", -64, 63, 1, 64},
				{"Level", 0, 127, 7, 0},
				{"Damp", 0, 127, 6, 0},
				{"Fb", -64, 64, 10, 0},
				{"Length", 5, 250, 3, 0, true},
			},
			{
				{"Safe Mode", 2},
			},
			{{"IR", &kConvolIRNames, 8}}, nullptr,
			{&kConvolotronPresetNames, [rkr](int32 v) { rkr->efx_Convol->setpreset(v); }});
	}

	void BuildColumn4(BView* col)
	{
		RKR* rkr = fRkr;

		std::vector<ParamDef> chorusFlangerParams = {
			{"Tempo", 1, 600, 2, 0},
			{"Depth", 0, 127, 6, 0},
			{"Delay", 0, 127, 7, 0},
			{"Feedback", 0, 127, 8, 0},
			{"Stereo", 0, 127, 5, 0},
			{"L/R Cr.", -64, 63, 9, 64},
		};

		BuildEffectBox(col, "Distortion", rkr, 17, &rkr->NewDist_Bypass,
			[rkr](int32 n, int32 v) { rkr->efx_NewDist->changepar(n, v); },
			[rkr](int32 n) { return rkr->efx_NewDist->getpar(n); },
			{
				{"Drive", 1, 127, 3, 0},
				{"Level", 0, 127, 4, 0},
				{"Color", 0, 127, 9, 0},
				{"Sub Octv", 0, 127, 11, 0},
				{"LPF", 20, 26000, 7, 0},
				{"HPF", 20, 20000, 8, 0},
			},
			{}, {{"Type", &kDistTypeNames, 5}}, nullptr,
			{&kNewDistPresetNames, [rkr](int32 v) { rkr->efx_NewDist->setpreset(v); }});

		BuildEffectBox(col, "Noise Gate", rkr, 16, &rkr->Gate_Bypass,
			[rkr](int32 n, int32 v) { rkr->efx_Gate->Gate_Change(n, v); },
			[rkr](int32 n) { return rkr->efx_Gate->getpar(n); },
			{
				{"A. Time", 1, 250, 3, 0},
				{"R. Time", 2, 250, 4, 0},
				{"Range", -90, 0, 2, 0},
				{"Threshold", -70, 20, 1, 0},
				{"Hold", 2, 500, 7, 0},
				{"LPF", 20, 26000, 5, 0},
				{"HPF", 20, 20000, 6, 0},
			}, {}, {}, nullptr,
			{&kGatePresetNames, [rkr](int32 v) { rkr->efx_Gate->Gate_Change_Preset(v); }});

		BuildEffectBox(col, "Chorus", rkr, 5, &rkr->Chorus_Bypass,
			[rkr](int32 n, int32 v) { rkr->efx_Chorus->changepar(n, v); },
			[rkr](int32 n) { return rkr->efx_Chorus->getpar(n); },
			chorusFlangerParams, {}, {}, nullptr,
			{&kChorusPresetNames, [rkr](int32 v) { rkr->efx_Chorus->setpreset(0, v); }});

		BuildEffectBox(col, "StompBox", rkr, 39, &rkr->StompBox_Bypass,
			[rkr](int32 n, int32 v) { rkr->efx_StompBox->changepar(n, v); },
			[rkr](int32 n) { return rkr->efx_StompBox->getpar(n); },
			{
				{"Level", 0, 127, 0, 0},
				{"Gain", 0, 127, 4, 0},
				{"Low", -64, 64, 3, 0},
				{"Mid", -64, 64, 2, 0},
				{"High", -64, 64, 1, 0},
			},
			{}, {{"Mode", &kStompBoxModeNames, 5}}, nullptr,
			{&kStompBoxPresetNames, [rkr](int32 v) { rkr->efx_StompBox->setpreset(v); }});

		BuildEffectBox(col, "WhaWha", rkr, 10, &rkr->WhaWha_Bypass,
			[rkr](int32 n, int32 v) { rkr->efx_WhaWha->changepar(n, v); },
			[rkr](int32 n) { return rkr->efx_WhaWha->getpar(n); },
			{
				{"Tempo", 1, 600, 2, 0},
				{"Depth", 0, 127, 6, 0},
				{"Amp.Sens", 0, 127, 7, 0},
				{"Smooth", 0, 127, 9, 0},
			}, {}, {}, nullptr,
			{&kWhaWhaPresetNames, [rkr](int32 v) { rkr->efx_WhaWha->setpreset(v); }});

		BuildEffectBox(col, "Looper", rkr, 30, &rkr->Looper_Bypass,
			[rkr](int32 n, int32 v) { rkr->efx_Looper->changepar(n, v); },
			[rkr](int32 n) { return rkr->efx_Looper->getpar(n); },
			{
				{"Wet/Dry", -64, 63, 0, 64},
				{"Level 1", 0, 127, 6, 0},
				{"Level 2", 0, 127, 10, 0},
				{"Tempo", 20, 380, 14, 0},
			},
			{
				{"Reverse", 5},
				{"Auto Play", 9},
				{"R1", 11},
				{"R2", 12},
				{"Lnk", 13},
				{"M", 16},
			},
			{
				{"Time Sig.", &kLooperBarNames, 15},
				{"MS", &kLooperMsNames, 17},
			},
			[this, rkr](BView* body) {
				// Plain momentary presses (Fl_Button in rakarrack.cxx, each
				// just a changepar(npar, 1) call) rather than sliders,
				// toggles, or dropdowns -- see AddButton().
				BGroupView* transport = new BGroupView(B_HORIZONTAL, 4);
				transport->SetViewColor(kPanelColor);
				body->AddChild(transport);
				AddButton(transport, "play", "Play", 1,
					[rkr](int32) { rkr->efx_Looper->changepar(1, 1); });
				AddButton(transport, "stop", "Stop", 1,
					[rkr](int32) { rkr->efx_Looper->changepar(2, 1); });
				AddButton(transport, "record", "Rec", 1,
					[rkr](int32) { rkr->efx_Looper->changepar(3, 1); });
				AddButton(transport, "clear", "Clear", 1,
					[rkr](int32) { rkr->efx_Looper->changepar(4, 1); });
				AddButton(transport, "t1", "Trk 1", 1,
					[rkr](int32) { rkr->efx_Looper->changepar(7, 1); });
				AddButton(transport, "t2", "Trk 2", 1,
					[rkr](int32) { rkr->efx_Looper->changepar(8, 1); });
			},
			{&kLooperPresetNames, [rkr](int32 v) { rkr->efx_Looper->setpreset(v); }});

		BuildEffectBox(col, "Sequence", rkr, 37, &rkr->Sequence_Bypass,
			[rkr](int32 n, int32 v) { rkr->efx_Sequence->changepar(n, v); },
			[rkr](int32 n) { return rkr->efx_Sequence->getpar(n); },
			{
				{"Wet/Dry", -64, 64, 8, 64},
				{"1", 0, 127, 0, 0},
				{"2", 0, 127, 1, 0},
				{"3", 0, 127, 2, 0},
				{"4", 0, 127, 3, 0},
				{"5", 0, 127, 4, 0},
				{"6", 0, 127, 5, 0},
				{"7", 0, 127, 6, 0},
				{"8", 0, 127, 7, 0},
				{"Tempo", 1, 600, 9, 0},
				{"Q", -64, 64, 10, 64},
				{"St.df", 0, 7, 12, 0},
				{"Range", 1, 8, 14, 0},
			},
			{
				{"Amp.", 11},
			},
			{{"Mode", &kSeqModeNames, 13}}, nullptr,
			{&kSequencePresetNames, [rkr](int32 v) { rkr->efx_Sequence->setpreset(v); }});

		BuildEffectBox(col, "StereoHarm", rkr, 42, &rkr->StereoHarm_Bypass,
			[rkr](int32 n, int32 v) { rkr->efx_StereoHarm->changepar(n, v); },
			[rkr](int32 n) { return rkr->efx_StereoHarm->getpar(n); },
			{
				{"Wet/Dry", -64, 63, 0, 64},
				{"Int L", -12, 12, 2, 12},
				{"Chrm L", -2000, 2000, 3, 0},
				{"Gain L", -64, 64, 1, 64},
				{"Int R", -12, 12, 5, 12},
				{"Chrm R", -2000, 2000, 6, 0},
				{"Gain R", -64, 64, 4, 64},
				{"L/R Cr.", -64, 64, 11, 64},
				{"Note", 0, 23, 8, 0},
				{"Chord", 0, 33, 9, 0},
			},
			{
				{"MIDI", 10},
				{"SEL", 7},
			}, {}, nullptr,
			{&kStereoHarmPresetNames, [rkr](int32 v) { rkr->efx_StereoHarm->setpreset(v); }});

		BuildEffectBox(col, "MBVvol", rkr, 28, &rkr->MBVvol_Bypass,
			[rkr](int32 n, int32 v) { rkr->efx_MBVvol->changepar(n, v); },
			[rkr](int32 n) { return rkr->efx_MBVvol->getpar(n); },
			{
				{"Wet/Dry", -64, 63, 0, 64},
				{"Tempo 1", 1, 600, 1, 0},
				{"St.df 1", 0, 127, 3, 0},
				{"Tempo 2", 1, 600, 4, 0},
				{"St.df 2", 0, 127, 6, 0},
				{"Cross1", 20, 1000, 7, 0},
				{"Cross2", 1000, 8000, 8, 0},
				{"Cross3", 2000, 26000, 9, 0},
			},
			{},
			{
				{"LFO 1", &kLfoTypeNames, 2},
				{"LFO 2", &kLfoTypeNames, 5},
				{"Combi", &kMBVvolCombiNames, 10},
			}, nullptr,
			{&kMBVvolPresetNames, [rkr](int32 v) { rkr->efx_MBVvol->setpreset(v); }});

		BuildEffectBox(col, "CoilCrafter", rkr, 33, &rkr->CoilCrafter_Bypass,
			[rkr](int32 n, int32 v) { rkr->efx_CoilCrafter->changepar(n, v); },
			[rkr](int32 n) { return rkr->efx_CoilCrafter->getpar(n); },
			{
				{"Gain", 0, 127, 0, 0},
				{"Tone", 20, 4400, 7, 0},
				{"Freq1", 2600, 4500, 3, 0},
				{"Q1", 10, 65, 4, 0},
				{"Freq2", 2600, 4500, 5, 0},
				{"Q2", 10, 65, 6, 0},
			},
			{
				{"Pos.", 8},
			},
			{
				{"Origin", &kCoilOriginNames, 1},
				{"Destiny", &kCoilOriginNames, 2},
			}, nullptr,
			{&kCoilCrafterPresetNames, [rkr](int32 v) { rkr->efx_CoilCrafter->setpreset(v); }});

		BuildEffectBox(col, "Expander", rkr, 25, &rkr->Expander_Bypass,
			[rkr](int32 n, int32 v) { rkr->efx_Expander->Expander_Change(n, v); },
			[rkr](int32 n) { return rkr->efx_Expander->getpar(n); },
			{
				{"A. Time", 10, 2000, 3, 0},
				{"R. Time", 10, 500, 4, 0},
				{"Shape", 1, 50, 2, 0},
				{"Thrhold", -80, 0, 1, 0},
				{"Level", 1, 127, 7, 0},
				{"LPF", 20, 26000, 5, 0},
				{"HPF", 20, 20000, 6, 0},
			}, {}, {}, nullptr,
			{&kExpanderPresetNames, [rkr](int32 v) { rkr->efx_Expander->Expander_Change_Preset(v); }});
	}

	RKR* fRkr;
	BStringView* fCpuDisplay;
	BCheckBox* fMasterFX;
	OrderWindow* fOrderWindow = nullptr;
	std::vector<std::function<void(int32)>> fActions;
	std::vector<BMenu*> fMenus;
	std::deque<DebouncedSlider> fDebounced;
};

class RakarrackWindow : public BWindow {
public:
	RakarrackWindow(BRect frame, RKR* rkr)
		:
		BWindow(frame, "Haikurack", B_DOCUMENT_WINDOW,
			B_ASYNCHRONOUS_CONTROLS | B_QUIT_ON_WINDOW_CLOSE),
		fRkr(rkr)
	{
		SetLayout(new BGroupLayout(B_VERTICAL));

		fMainView = new RakarrackView(rkr);
		fMainView->SetExplicitMinSize(BSize(300, 200));

		// Header (logo, CPU/FX Engine/Boost, Input Gain/Master Volume,
		// Effects Order, Save/Load Preset) lives outside the BScrollView
		// entirely, as its own sibling view, so none of it scrolls away
		// with the effect racks -- see RakarrackView::BuildHeader().
		BGroupView* header = new BGroupView(B_VERTICAL, 0);
		header->SetViewColor(kBgColor);
		fMainView->BuildHeader(header);

		// No visible scroll bars -- see RakarrackView::MessageReceived()
		// for how scrolling still works (mouse wheel) without them.
		BScrollView* scroller = new BScrollView("rack_scroll", fMainView, 0,
			false, false);
		scroller->SetViewColor(kBgColor);

		BLayoutBuilder::Group<>(this, B_VERTICAL, 0)
			.Add(header)
			.Add(scroller)
			.End();

		SetSizeLimits(200, 10000, 150, 10000);
		SetPulseRate(50000);
	}

	virtual void MessageReceived(BMessage* msg)
	{
		if (msg->what == MSG_OPEN_ORDER) {
			// Deliberately outside jmutex -- see MSG_OPEN_ORDER's
			// declaration comment.
			fMainView->OpenOrderWindow();
			return;
		}

		if (msg->what == MSG_SAVE_PRESET) {
			ShowSavePanel();
			return;
		}

		if (msg->what == MSG_LOAD_PRESET) {
			ShowLoadPanel();
			return;
		}

		if (msg->what == B_SAVE_REQUESTED) {
			HandleSaveRequested(msg);
			return;
		}

		if (msg->what == B_REFS_RECEIVED) {
			HandleRefsReceived(msg);
			return;
		}

		if (msg->what != MSG_ACTION) {
			BWindow::MessageReceived(msg);
			return;
		}

		pthread_mutex_lock(&jmutex);
		fMainView->Dispatch(msg);
		pthread_mutex_unlock(&jmutex);
	}

private:
	// Lazily built, kept for the life of the window (BFilePanel is heavy
	// enough to build once and reuse, same reasoning as OrderWindow).
	// Targeted at "this" explicitly rather than left at BFilePanel's
	// default (be_app), so the resulting B_SAVE_REQUESTED/B_REFS_RECEIVED
	// land in this window's own MessageReceived() above, not wherever
	// BApplication's default handling of those would send them.
	void ShowSavePanel()
	{
		if (!fSavePanel)
			fSavePanel = new BFilePanel(B_SAVE_PANEL, new BMessenger(this));
		fSavePanel->Show();
	}

	void ShowLoadPanel()
	{
		if (!fOpenPanel)
			fOpenPanel = new BFilePanel(B_OPEN_PANEL, new BMessenger(this));
		fOpenPanel->Show();
	}

	// rkr->savefile()/loadfile() (declared in src/global.h) are exactly
	// what src/rakarrack.cxx's own File > Save/Load menu items call --
	// same file format, same engine entry points, nothing native-mode-
	// specific about the save/load mechanism itself, just how it's
	// triggered. Both mutate/read a large amount of engine state the
	// audio thread also touches every callback, so both need jmutex held,
	// unlike the panel-opening steps above.
	void HandleSaveRequested(BMessage* msg)
	{
		entry_ref dirRef;
		BString name;
		if (msg->FindRef("directory", &dirRef) != B_OK
			|| msg->FindString("name", &name) != B_OK) {
			return;
		}
		BPath dirPath(&dirRef);
		BPath filePath(dirPath.Path(), name.String());

		pthread_mutex_lock(&jmutex);
		fRkr->savefile((char*)filePath.Path());
		pthread_mutex_unlock(&jmutex);
	}

	void HandleRefsReceived(BMessage* msg)
	{
		entry_ref ref;
		if (msg->FindRef("refs", &ref) != B_OK)
			return;
		BPath filePath(&ref);

		pthread_mutex_lock(&jmutex);
		fRkr->loadfile((char*)filePath.Path());
		pthread_mutex_unlock(&jmutex);

		// The engine and audio output switch to the loaded preset
		// immediately; the on-screen slider/toggle/dropdown positions
		// don't, since nothing here re-primes those widgets from the
		// engine's new state after the fact (every effect box reads its
		// initial values once, at construction). Rebuilding all of them
		// live is a real project of its own -- flagging it plainly rather
		// than leaving the mismatch to look like a bug.
		BAlert* alert = new BAlert("Preset Loaded",
			"The preset was loaded and is already playing -- audio reflects "
			"it now. The on-screen slider and toggle positions won't catch "
			"up until Rakarrack is restarted, though.",
			"OK", NULL, NULL, B_WIDTH_AS_USUAL, B_INFO_ALERT);
		alert->Go(NULL);
	}

	RKR* fRkr;
	RakarrackView* fMainView;
	BFilePanel* fSavePanel = nullptr;
	BFilePanel* fOpenPanel = nullptr;
};

extern "C" void start_haiku_native_interface(void* rkr_ptr) {
    RKR* rkr = (RKR*)rkr_ptr;
    RakarrackWindow *win = new RakarrackWindow(BRect(80, 60, 1080, 760), rkr);
    win->Show();
}
