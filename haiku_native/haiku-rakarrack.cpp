/*
 * Copyright 2026, Kris Beazley jb@epluribusunix.net
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

#include <InterfaceDefs.h>
#include <LayoutBuilder.h>
#include <Box.h>
#include <CheckBox.h>
#include <Font.h>
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
#include <vector>

// Pulls in the full RKR engine class (global.h) so we can call the real
// effect objects' public parameter APIs directly -- the same interface
// src/rakarrack.cxx itself uses. Nothing in this file touches a private
// member of any effect class.
#include "../src/global.h"

#include "../src/rakarrack_haiku_bridge.h"


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

#undef RKR_HAIKU_WEAK_CHANGEPAR

__attribute__((weak)) void Compressor::Compressor_Change(int, int) { }
__attribute__((weak)) int Compressor::getpar(int) { return 0; }
__attribute__((weak)) void Gate::Gate_Change(int, int) { }
__attribute__((weak)) int Gate::getpar(int) { return 0; }


extern pthread_mutex_t jmutex;

// Single message type for every control in the rack. "aidx" indexes into
// RakarrackView::fActions; the value comes from "be:value" for sliders and
// checkboxes, or from an explicit "val" field for menu items.
enum {
	MSG_ACTION = 'RKAx'
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
};

// A plain on/off parameter (e.g. Pan's "Auto Pan" / "Extra On" flags).
struct ToggleDef {
	const char* label;
	int32 npar;
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
		SetViewColor(180, 180, 180);

		fCpuDisplay = new BStringView("cpu", "CPU: 0.00%");
		fCpuDisplay->SetHighColor(100, 0, 0);

		fMasterFX = new BCheckBox("master_fx", "FX Engine",
			MakeMessage(Bind([rkr](int32 v) {
				rkr->Bypass = v ? 1 : 0;
				if (!v)
					rkr->cleanup_efx();
			})));
		fMasterFX->SetValue(rkr->Bypass ? B_CONTROL_ON : B_CONTROL_OFF);
		BFont boldFont(be_bold_font);
		fMasterFX->SetFont(&boldFont);

		BCheckBox* boost = new BCheckBox("boost", "Boost +10dB",
			MakeMessage(Bind([rkr](int32 v) {
				rkr->booster = v ? dB2rap(10.0f) : 1.0f;
			})));
		boost->SetValue(rkr->booster > 1.0f ? B_CONTROL_ON : B_CONTROL_OFF);

		BGroupView* master = new BGroupView(B_HORIZONTAL, 10);
		master->GroupLayout()->SetInsets(10);
		master->AddChild(fCpuDisplay);
		master->AddChild(fMasterFX);
		master->AddChild(boost);
		AddSlider(master, "in_gain", "Input Gain", -50, 50,
			(int32)(rkr->Input_Gain * 100.0f) - 50,
			[rkr](int32 v) {
				rkr->Input_Gain = (float)((v + 50) / 100.0);
				rkr->calculavol(1);
			});
		AddSlider(master, "out_gain", "Master Volume", -50, 50,
			(int32)(rkr->Master_Volume * 100.0f) - 50,
			[rkr](int32 v) {
				rkr->Master_Volume = (float)((v + 50) / 100.0);
				rkr->calculavol(2);
			});
		master->GroupLayout()->AddItem(BSpaceLayoutItem::CreateGlue());

		// Three scrollable columns of effect racks, mirroring the layout of
		// src/rakarrack.cxx without trying to reproduce its exact pixel
		// geometry.
		BGroupView* col1 = new BGroupView(B_VERTICAL, 8);
		BGroupView* col2 = new BGroupView(B_VERTICAL, 8);
		BGroupView* col3 = new BGroupView(B_VERTICAL, 8);
		col1->GroupLayout()->SetInsets(5);
		col2->GroupLayout()->SetInsets(5);
		col3->GroupLayout()->SetInsets(5);

		BuildColumn1(col1);
		BuildColumn2(col2);
		BuildColumn3(col3);

		col1->GroupLayout()->AddItem(BSpaceLayoutItem::CreateGlue());
		col2->GroupLayout()->AddItem(BSpaceLayoutItem::CreateGlue());
		col3->GroupLayout()->AddItem(BSpaceLayoutItem::CreateGlue());

		BGroupView* columns = new BGroupView(B_HORIZONTAL, 8);
		columns->GroupLayout()->SetInsets(10);
		columns->AddChild(col1);
		columns->AddChild(col2);
		columns->AddChild(col3);

		BLayoutBuilder::Group<>(this, B_VERTICAL, 0)
			.Add(master)
			.Add(columns)
			.End();
	}

	virtual void AttachedToWindow()
	{
		BView::AttachedToWindow();
		for (BMenu* menu : fMenus)
			menu->SetTargetForItems(Window());
	}

	virtual void Pulse()
	{
		if (!fRkr)
			return;
		char cpuBuf[32];
		sprintf(cpuBuf, "CPU: %5.2f%%", (float)fRkr->cpuload);
		fCpuDisplay->SetText(cpuBuf);
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

	BSlider* AddSlider(BView* parent, const char* name, const char* label,
		int32 min, int32 max, int32 initial, std::function<void(int32)> fn)
	{
		int32 idx = Bind(fn);
		BSlider* s = new BSlider(name, label, MakeMessage(idx), min, max,
			B_HORIZONTAL);
		s->SetValue(initial);
		s->SetHashMarks(B_HASH_MARKS_NONE);
		parent->AddChild(s);
		return s;
	}

	BCheckBox* AddToggle(BView* parent, const char* name, const char* label,
		bool initial, std::function<void(int32)> fn)
	{
		int32 idx = Bind(fn);
		BCheckBox* c = new BCheckBox(name, label, MakeMessage(idx));
		c->SetValue(initial ? B_CONTROL_ON : B_CONTROL_OFF);
		parent->AddChild(c);
		return c;
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
		parent->AddChild(field);
		return field;
	}

	// One "rack box" -- title, on/off, an optional type menu, optional plain
	// toggles, then every parameter slider. changeFn/getFn wrap whichever
	// method the effect actually exposes (changepar, Compressor_Change,
	// Gate_Change...) so the rest of this stays effect-agnostic.
	void BuildEffectBox(BView* column, const char* title, int* bypass,
		std::function<void(int32, int32)> changeFn,
		std::function<int32(int32)> getFn,
		const std::vector<ParamDef>& params,
		const std::vector<ToggleDef>& toggles = std::vector<ToggleDef>(),
		const std::vector<std::string>* typeItems = nullptr,
		int32 typeNpar = -1, const char* typeLabel = "Type")
	{
		BBox* box = new BBox(title);
		box->SetLabel(title);

		BGroupView* content = new BGroupView(B_VERTICAL, 4);
		content->GroupLayout()->SetInsets(8);

		AddToggle(content, "on", "On", *bypass != 0,
			[bypass](int32 v) { *bypass = v ? 1 : 0; });

		if (typeItems != nullptr && typeNpar >= 0) {
			AddTypeMenu(content, "type", typeLabel, *typeItems,
				getFn(typeNpar),
				[changeFn, typeNpar](int32 v) { changeFn(typeNpar, v); });
		}

		for (const ToggleDef& t : toggles) {
			AddToggle(content, t.label, t.label, getFn(t.npar) != 0,
				[changeFn, t](int32 v) { changeFn(t.npar, v); });
		}

		for (const ParamDef& p : params) {
			AddSlider(content, p.label, p.label, p.min, p.max,
				getFn(p.npar) - p.offset,
				[changeFn, p](int32 v) { changeFn(p.npar, v + p.offset); });
		}

		box->AddChild(content);
		column->AddChild(box);
	}

	void BuildColumn1(BView* col)
	{
		RKR* rkr = fRkr;

		BuildEffectBox(col, "Overdrive", &rkr->Overdrive_Bypass,
			[rkr](int32 n, int32 v) { rkr->efx_Overdrive->changepar(n, v); },
			[rkr](int32 n) { return rkr->efx_Overdrive->getpar(n); },
			{
				{"Drive", 0, 127, 3, 0},
				{"Level", 0, 127, 4, 0},
				{"LPF", 20, 26000, 7, 0},
				{"HPF", 20, 20000, 8, 0},
			},
			{}, &kDistTypeNames, 5, "Type");

		BuildEffectBox(col, "Distortion", &rkr->NewDist_Bypass,
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
			{}, &kDistTypeNames, 5, "Type");

		BuildEffectBox(col, "Echo", &rkr->Echo_Bypass,
			[rkr](int32 n, int32 v) { rkr->efx_Echo->changepar(n, v); },
			[rkr](int32 n) { return rkr->efx_Echo->getpar(n); },
			{
				{"Delay", 20, 2000, 2, 0},
				{"Feedback", 0, 127, 5, 0},
				{"Damp", 0, 127, 6, 0},
				{"L/R Cr.", -64, 63, 4, 64},
			});

		BuildEffectBox(col, "Compressor", &rkr->Compressor_Bypass,
			[rkr](int32 n, int32 v) { rkr->efx_Compressor->Compressor_Change(n, v); },
			[rkr](int32 n) { return rkr->efx_Compressor->getpar(n); },
			{
				{"A. Time", 10, 250, 4, 0},
				{"R. Time", 10, 500, 5, 0},
				{"Ratio", 2, 42, 2, 0},
				{"Knee", 0, 100, 7, 0},
				{"Threshold", -60, -3, 1, 0},
				{"Output", -40, 0, 3, 0},
			});

		BuildEffectBox(col, "Noise Gate", &rkr->Gate_Bypass,
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
			});

		BuildEffectBox(col, "Reverb", &rkr->Reverb_Bypass,
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
			});

		{
			static const char* kBandLabels[10] = {
				"31 Hz", "63 Hz", "125 Hz", "250 Hz", "500 Hz", "1 Khz",
				"2 Khz", "4 Khz", "8 Khz", "16 Khz"
			};
			std::vector<ParamDef> bands;
			for (int i = 0; i < 10; i++)
				bands.push_back({kBandLabels[i], -64, 63, 10 + i * 5 + 2, 64});
			BuildEffectBox(col, "Equalizer", &rkr->EQ1_Bypass,
				[rkr](int32 n, int32 v) { rkr->efx_EQ1->changepar(n, v); },
				[rkr](int32 n) { return rkr->efx_EQ1->getpar(n); },
				bands);
		}
	}

	void BuildColumn2(BView* col)
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

		BuildEffectBox(col, "Chorus", &rkr->Chorus_Bypass,
			[rkr](int32 n, int32 v) { rkr->efx_Chorus->changepar(n, v); },
			[rkr](int32 n) { return rkr->efx_Chorus->getpar(n); },
			chorusFlangerParams);

		BuildEffectBox(col, "Flanger", &rkr->Flanger_Bypass,
			[rkr](int32 n, int32 v) { rkr->efx_Flanger->changepar(n, v); },
			[rkr](int32 n) { return rkr->efx_Flanger->getpar(n); },
			chorusFlangerParams);

		BuildEffectBox(col, "Phaser", &rkr->Phaser_Bypass,
			[rkr](int32 n, int32 v) { rkr->efx_Phaser->changepar(n, v); },
			[rkr](int32 n) { return rkr->efx_Phaser->getpar(n); },
			{
				{"Tempo", 1, 600, 2, 0},
				{"Depth", 0, 127, 6, 0},
				{"Feedback", 0, 127, 7, 0},
				{"Phase", 0, 127, 11, 0},
				{"Stereo", 0, 127, 5, 0},
				{"L/R Cr.", -64, 63, 9, 64},
			});

		BuildEffectBox(col, "Analog Phaser", &rkr->APhaser_Bypass,
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
			});

		BuildEffectBox(col, "WhaWha", &rkr->WhaWha_Bypass,
			[rkr](int32 n, int32 v) { rkr->efx_WhaWha->changepar(n, v); },
			[rkr](int32 n) { return rkr->efx_WhaWha->getpar(n); },
			{
				{"Tempo", 1, 600, 2, 0},
				{"Depth", 0, 127, 6, 0},
				{"Amp.Sens", 0, 127, 7, 0},
				{"Smooth", 0, 127, 9, 0},
			});

		BuildEffectBox(col, "Alienwah", &rkr->Alienwah_Bypass,
			[rkr](int32 n, int32 v) { rkr->efx_Alienwah->changepar(n, v); },
			[rkr](int32 n) { return rkr->efx_Alienwah->getpar(n); },
			{
				{"Tempo", 1, 600, 2, 0},
				{"Depth", 0, 127, 6, 0},
				{"Feedback", 0, 127, 7, 0},
				{"Delay", 0, 127, 8, 0},
				{"Phase", 0, 127, 10, 0},
			});

		BuildEffectBox(col, "Valve", &rkr->Valve_Bypass,
			[rkr](int32 n, int32 v) { rkr->efx_Valve->changepar(n, v); },
			[rkr](int32 n) { return rkr->efx_Valve->getpar(n); },
			{
				{"Drive", 0, 127, 3, 0},
				{"Level", 0, 127, 4, 0},
				{"Dist.", 0, 127, 10, 0},
				{"Presence", 0, 100, 12, 0},
				{"LPF", 20, 26000, 6, 0},
				{"HPF", 20, 20000, 7, 0},
			});
	}

	void BuildColumn3(BView* col)
	{
		RKR* rkr = fRkr;

		BuildEffectBox(col, "Ring Modulator", &rkr->Ring_Bypass,
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
			});

		BuildEffectBox(col, "Sustainer", &rkr->Sustainer_Bypass,
			[rkr](int32 n, int32 v) { rkr->efx_Sustainer->changepar(n, v); },
			[rkr](int32 n) { return rkr->efx_Sustainer->getpar(n); },
			{
				{"Gain", 0, 127, 0, 0},
				{"Sustain", 1, 127, 1, 0},
			});

		BuildEffectBox(col, "StompBox", &rkr->StompBox_Bypass,
			[rkr](int32 n, int32 v) { rkr->efx_StompBox->changepar(n, v); },
			[rkr](int32 n) { return rkr->efx_StompBox->getpar(n); },
			{
				{"Level", 0, 127, 0, 0},
				{"Gain", 0, 127, 4, 0},
				{"Low", -64, 64, 3, 0},
				{"Mid", -64, 64, 2, 0},
				{"High", -64, 64, 1, 0},
			});

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
			BuildEffectBox(col, "Exciter", &rkr->Exciter_Bypass,
				[rkr](int32 n, int32 v) { rkr->efx_Exciter->changepar(n, v); },
				[rkr](int32 n) { return rkr->efx_Exciter->getpar(n); },
				exciterParams);
		}

		BuildEffectBox(col, "Vibe", &rkr->Vibe_Bypass,
			[rkr](int32 n, int32 v) { rkr->efx_Vibe->changepar(n, v); },
			[rkr](int32 n) { return rkr->efx_Vibe->getpar(n); },
			{
				{"Tempo", 1, 600, 1, 0},
				{"Width", 0, 127, 0, 0},
				{"Depth", 0, 127, 8, 0},
				{"Feedback", -64, 64, 7, 64},
				{"L/R Cr.", -64, 64, 9, 64},
			});

		BuildEffectBox(col, "Opticaltrem", &rkr->Opticaltrem_Bypass,
			[rkr](int32 n, int32 v) { rkr->efx_Opticaltrem->changepar(n, v); },
			[rkr](int32 n) { return rkr->efx_Opticaltrem->getpar(n); },
			{
				{"Depth", 0, 127, 0, 0},
				{"Tempo", 1, 600, 1, 0},
				{"Rnd", 0, 127, 2, 0},
				{"Stereo", 0, 127, 4, 0},
			});

		BuildEffectBox(col, "Auto Pan", &rkr->Pan_Bypass,
			[rkr](int32 n, int32 v) { rkr->efx_Pan->changepar(n, v); },
			[rkr](int32 n) { return rkr->efx_Pan->getpar(n); },
			{
				{"Tempo", 1, 600, 2, 0},
				{"Extra", 0, 127, 6, 0},
			},
			{
				{"Auto Pan", 7},
				{"Extra On", 8},
			});
	}

	RKR* fRkr;
	BStringView* fCpuDisplay;
	BCheckBox* fMasterFX;
	std::vector<std::function<void(int32)>> fActions;
	std::vector<BMenu*> fMenus;
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

		BScrollView* scroller = new BScrollView("rack_scroll", fMainView, 0,
			true, true);

		BLayoutBuilder::Group<>(this, B_VERTICAL, 0)
			.Add(scroller)
			.End();

		SetSizeLimits(200, 10000, 150, 10000);
		SetPulseRate(50000);
	}

	virtual void MessageReceived(BMessage* msg)
	{
		if (msg->what != MSG_ACTION) {
			BWindow::MessageReceived(msg);
			return;
		}

		pthread_mutex_lock(&jmutex);
		fMainView->Dispatch(msg);
		pthread_mutex_unlock(&jmutex);
	}

private:
	RKR* fRkr;
	RakarrackView* fMainView;
};

extern "C" void start_haiku_native_interface(void* rkr_ptr) {
    RKR* rkr = (RKR*)rkr_ptr;
    RakarrackWindow *win = new RakarrackWindow(BRect(80, 60, 1080, 760), rkr);
    win->Show();
}
