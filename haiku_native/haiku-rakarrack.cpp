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

#include <Alignment.h>
#include <InterfaceDefs.h>
#include <LayoutBuilder.h>
#include <Box.h>
#include <Button.h>
#include <CheckBox.h>
#include <Font.h>
#include <ListItem.h>
#include <ListView.h>
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
// native mode has no such window, so each effect box claims and releases
// its own slot directly from its "On" toggle instead. This mirrors the
// same 10-active-effects ceiling the FLTK GUI has always had -- it does
// not remove it, just gives native mode its own way to work within it.
static const int kOrderSlotCount = 10;
static const int kEmptySlot = -1; // matches no "case N:" label in that switch

static void ClearOrderSlots(RKR* rkr) {
	for (int i = 0; i < kOrderSlotCount; i++)
		rkr->efx_order[i] = kEmptySlot;
}

// Returns true if effectId is now (or already was) occupying a slot.
// False means all 10 slots are claimed by other effects.
static bool ActivateEffectSlot(RKR* rkr, int effectId) {
	int freeSlot = -1;
	for (int i = 0; i < kOrderSlotCount; i++) {
		if (rkr->efx_order[i] == effectId)
			return true;
		if (freeSlot < 0 && rkr->efx_order[i] == kEmptySlot)
			freeSlot = i;
	}
	if (freeSlot < 0)
		return false;
	rkr->efx_order[freeSlot] = effectId;
	return true;
}

static void DeactivateEffectSlot(RKR* rkr, int effectId) {
	for (int i = 0; i < kOrderSlotCount; i++) {
		if (rkr->efx_order[i] == effectId) {
			rkr->efx_order[i] = kEmptySlot;
			return;
		}
	}
}

// Display name for each effect-type ID the native UI actually exposes --
// only the 21 effects BuildColumn1-4 wire up can ever end up in
// rkr->efx_order[], so that is the only set this needs to name.
static const char* EffectName(int effectId) {
	switch (effectId) {
	case 0: return "Equalizer";
	case 1: return "Compressor";
	case 3: return "Overdrive";
	case 4: return "Echo";
	case 5: return "Chorus";
	case 6: return "Phaser";
	case 7: return "Flanger";
	case 8: return "Reverb";
	case 10: return "WhaWha";
	case 11: return "Alienwah";
	case 13: return "Auto Pan";
	case 16: return "Noise Gate";
	case 17: return "Distortion";
	case 18: return "Analog Phaser";
	case 19: return "Valve";
	case 21: return "Ring Modulator";
	case 22: return "Exciter";
	case 36: return "Sustainer";
	case 39: return "StompBox";
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
	virtual bool QuitRequested()
	{
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
			if (id == kEmptySlot)
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


// Main rack content view: builds every effect box and owns the table of
// callbacks ("actions") that the controls' messages are dispatched through.
class RakarrackView : public BView {
public:
	RakarrackView(RKR* rkr)
		:
		BView("MainView", B_WILL_DRAW | B_PULSE_NEEDED),
		fRkr(rkr)
	{
		// Start with an empty active-effects chain -- see ActivateEffectSlot()
		// above for why this matters. The factory-default bank (loaded
		// earlier, before this view exists, in both FLTK and native mode)
		// leaves effect-type IDs 0-9 pre-occupying all 10 slots regardless
		// of whether those effects are actually on; native mode has no
		// Effects Order window to ever change that, so without this, the
		// first 10 effect boxes built below would find the chain already
		// full of IDs 0-9 and every other effect (StompBox included) would
		// never get a slot no matter how many are turned off.
		ClearOrderSlots(rkr);

		SetViewColor(kBgColor);

		fCpuDisplay = new BStringView("cpu", "CPU: 0.00%");
		fCpuDisplay->SetHighColor(kValueColor);
		fCpuDisplay->SetLowColor(kBgColor);

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
		BButton* orderBtn = new BButton("order", "Effects Order...",
			MakeMessage(Bind([this, rkr](int32) {
				if (fOrderWindow == nullptr) {
					// A just-constructed BWindow is locked to the
					// constructing thread until its first Unlock() (which
					// Show() takes care of) -- safe without an explicit
					// Lock() here.
					fOrderWindow = new OrderWindow(rkr);
					fOrderWindow->Show();
					return;
				}
				// Once shown, a BWindow runs its own message loop on its
				// own thread -- reaching into it from here (a different
				// thread) needs its lock held first, unlike the
				// just-constructed case above.
				if (fOrderWindow->Lock()) {
					fOrderWindow->Refresh();
					if (fOrderWindow->IsHidden())
						fOrderWindow->Show();
					fOrderWindow->Activate();
					fOrderWindow->Unlock();
				}
			})));

		BGroupView* master = new BGroupView(B_HORIZONTAL, 10);
		master->GroupLayout()->SetInsets(10);
		master->SetViewColor(kBgColor);
		master->AddChild(fCpuDisplay);
		master->AddChild(fMasterFX);
		master->AddChild(boost);
		master->AddChild(orderBtn);
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

		// The sliders above only set their on-screen position from
		// Input_Gain/Master_Volume -- they never fire their own callback, so
		// without this, Log_I_Gain/Log_M_Volume (the actual gain multipliers
		// used in the audio path, see process.C's calculavol()) stay
		// uninitialized until the user manually touches a slider. Mirrors
		// rakarrack.cxx's own startup priming (RKRGUI's constructor).
		rkr->calculavol(1);
		rkr->calculavol(2);
		rkr->booster = 1.0f;
		
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

	// label ... value ... bar, one row, mirroring rakarrack.cxx's SliderW
	// (which draws its own live numeric readout next to the track --
	// BSlider has no such thing built in, so this adds a plain BStringView
	// next to it and keeps it in sync on every value change). Label/value
	// widths are fixed so sliders line up across a whole effect box.
	BSlider* AddSlider(BView* parent, const char* name, const char* label,
		int32 min, int32 max, int32 initial, std::function<void(int32)> fn)
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
		const std::vector<std::string>* typeItems = nullptr,
		int32 typeNpar = -1, const char* typeLabel = "Type")
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

		if (typeItems != nullptr && typeNpar >= 0) {
			AddTypeMenu(body, "type", typeLabel, *typeItems,
				getFn(typeNpar),
				[changeFn, typeNpar](int32 v) { changeFn(typeNpar, v); });
		}

		for (const ToggleDef& t : toggles) {
			AddToggle(body, t.label, t.label, getFn(t.npar) != 0,
				[changeFn, t](int32 v) { changeFn(t.npar, v); });
		}

		for (const ParamDef& p : params) {
			AddSlider(body, p.label, p.label, p.min, p.max,
				getFn(p.npar) - p.offset,
				[changeFn, p](int32 v) { changeFn(p.npar, v + p.offset); });
		}

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
				exciterParams);
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
			});

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
			});

		BuildEffectBox(col, "Vibe", rkr, 45, &rkr->Vibe_Bypass,
			[rkr](int32 n, int32 v) { rkr->efx_Vibe->changepar(n, v); },
			[rkr](int32 n) { return rkr->efx_Vibe->getpar(n); },
			{
				{"Tempo", 1, 600, 1, 0},
				{"Width", 0, 127, 0, 0},
				{"Depth", 0, 127, 8, 0},
				{"Feedback", -64, 64, 7, 64},
				{"L/R Cr.", -64, 64, 9, 64},
			});

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
			});
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
				bands);
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
			});

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
			});

		BuildEffectBox(col, "Overdrive", rkr, 3, &rkr->Overdrive_Bypass,
			[rkr](int32 n, int32 v) { rkr->efx_Overdrive->changepar(n, v); },
			[rkr](int32 n) { return rkr->efx_Overdrive->getpar(n); },
			{
				{"Drive", 0, 127, 3, 0},
				{"Level", 0, 127, 4, 0},
				{"LPF", 20, 26000, 7, 0},
				{"HPF", 20, 20000, 8, 0},
			},
			{}, &kDistTypeNames, 5, "Type");

		BuildEffectBox(col, "Opticaltrem", rkr, 44, &rkr->Opticaltrem_Bypass,
			[rkr](int32 n, int32 v) { rkr->efx_Opticaltrem->changepar(n, v); },
			[rkr](int32 n) { return rkr->efx_Opticaltrem->getpar(n); },
			{
				{"Depth", 0, 127, 0, 0},
				{"Tempo", 1, 600, 1, 0},
				{"Rnd", 0, 127, 2, 0},
				{"Stereo", 0, 127, 4, 0},
			});
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
			});

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
			});

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
			chorusFlangerParams);

		BuildEffectBox(col, "Alienwah", rkr, 11, &rkr->Alienwah_Bypass,
			[rkr](int32 n, int32 v) { rkr->efx_Alienwah->changepar(n, v); },
			[rkr](int32 n) { return rkr->efx_Alienwah->getpar(n); },
			{
				{"Tempo", 1, 600, 2, 0},
				{"Depth", 0, 127, 6, 0},
				{"Feedback", 0, 127, 7, 0},
				{"Delay", 0, 127, 8, 0},
				{"Phase", 0, 127, 10, 0},
			});

		BuildEffectBox(col, "Echo", rkr, 4, &rkr->Echo_Bypass,
			[rkr](int32 n, int32 v) { rkr->efx_Echo->changepar(n, v); },
			[rkr](int32 n) { return rkr->efx_Echo->getpar(n); },
			{
				{"Delay", 20, 2000, 2, 0},
				{"Feedback", 0, 127, 5, 0},
				{"Damp", 0, 127, 6, 0},
				{"L/R Cr.", -64, 63, 4, 64},
			});

		BuildEffectBox(col, "Sustainer", rkr, 36, &rkr->Sustainer_Bypass,
			[rkr](int32 n, int32 v) { rkr->efx_Sustainer->changepar(n, v); },
			[rkr](int32 n) { return rkr->efx_Sustainer->getpar(n); },
			{
				{"Gain", 0, 127, 0, 0},
				{"Sustain", 1, 127, 1, 0},
			});
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
			{}, &kDistTypeNames, 5, "Type");

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
			});

		BuildEffectBox(col, "Chorus", rkr, 5, &rkr->Chorus_Bypass,
			[rkr](int32 n, int32 v) { rkr->efx_Chorus->changepar(n, v); },
			[rkr](int32 n) { return rkr->efx_Chorus->getpar(n); },
			chorusFlangerParams);

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
			{}, &kStompBoxModeNames, 5, "Mode");

		BuildEffectBox(col, "WhaWha", rkr, 10, &rkr->WhaWha_Bypass,
			[rkr](int32 n, int32 v) { rkr->efx_WhaWha->changepar(n, v); },
			[rkr](int32 n) { return rkr->efx_WhaWha->getpar(n); },
			{
				{"Tempo", 1, 600, 2, 0},
				{"Depth", 0, 127, 6, 0},
				{"Amp.Sens", 0, 127, 7, 0},
				{"Smooth", 0, 127, 9, 0},
			});
	}

	RKR* fRkr;
	BStringView* fCpuDisplay;
	BCheckBox* fMasterFX;
	OrderWindow* fOrderWindow = nullptr;
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

		// No visible scroll bars -- see RakarrackView::MessageReceived()
		// for how scrolling still works (mouse wheel) without them.
		BScrollView* scroller = new BScrollView("rack_scroll", fMainView, 0,
			false, false);
		scroller->SetViewColor(kBgColor);

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
