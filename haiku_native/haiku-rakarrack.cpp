/*
 * Copyright 2026, Kris Beazley jb@epluribusunix.net
 * All rights reserved. Distributed under the terms of the MIT license.
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

//#include "../src/global.h"
//#include "../src/process.C"

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
#include <ScrollView.h>
#include <Size.h>
#include <StringView.h>
#include <Slider.h>
#include <StringView.h>
#include <StatusBar.h>
#include <stdio.h>
#include <SupportDefs.h>
#include <Window.h>
#include <View.h>
#include <pthread.h>



#include "../src/rakarrack_haiku_bridge.h"


// This "weak" function satisfies the linker for small utilities 
// like rakverb, but gets overridden by the real one in the main app.
__attribute__((weak)) void RKR::calculavol(int i) { }


extern pthread_mutex_t jmutex;

enum {
    MSG_OVRD_TOGGLE = 'ovtg',
    MSG_OVRD_DRIVE  = 'ovdr',
    MSG_OVRD_LEVEL  = 'ovlv',
    MSG_OVRD_LPF    = 'ovlp',
    MSG_OVRD_HPF    = 'ovhp',
    MSG_OVRD_TYPE   = 'ovtp',
    MSG_ECHO_TOGGLE = 'ectg', 
    MSG_ECHO_DELAY  = 'ecdl',
    MSG_ECHO_FB     = 'ecfb', 
    MSG_MASTER_FX   = 'mfxs',
    MSG_MASTER_IN_GAIN  = 'mign',
    MSG_MASTER_OUT_GAIN = 'mogn'
};

// Main View
class RakarrackView : public BView {
public:
    RakarrackView(RKR* rkr) : BView("MainView", B_WILL_DRAW | B_PULSE_NEEDED) {
        fRkr = rkr;
        SetViewColor(180, 180, 180);

        fCpuDisplay = new BStringView("cpu", "CPU: 0.00%");
        fCpuDisplay->SetHighColor(100, 0, 0);

        // --- Master FX Toggle ---
        fMasterFX = new BCheckBox("master_fx", "FX Engine", new BMessage(MSG_MASTER_FX));
        if (fRkr->Bypass == 1) {
    		fMasterFX->SetValue(B_CONTROL_ON);
		} else {
    		fMasterFX->SetValue(B_CONTROL_OFF);
		}
        BFont boldFont(be_bold_font);
        fMasterFX->SetFont(&boldFont);
        
        // Sync initial state: 0 = On (Checked), 1 = Bypass (Unchecked)
        if (fRkr->Bypass == 0)
            fMasterFX->SetValue(B_CONTROL_ON);

        // --- Overdrive Section ---
        BPopUpMenu* typeMenu = new BPopUpMenu("Select Type");
        BMessage* msg1 = new BMessage(MSG_OVRD_TYPE);
        msg1->AddInt32("index", 0);
        typeMenu->AddItem(new BMenuItem("Overdrive 1", msg1));

        BMessage* msg2 = new BMessage(MSG_OVRD_TYPE);
        msg2->AddInt32("index", 1);
        typeMenu->AddItem(new BMenuItem("Overdrive 2", msg2));

        BMenuField* typeSelector = new BMenuField("Type", "Type:", typeMenu);

        BBox* ovrdBox = new BBox("Overdrive");
        ovrdBox->SetLabel("Overdrive");
        
        BGroupView* ovrdCnt = new BGroupView(B_VERTICAL, 5);
        ovrdCnt->GroupLayout()->SetInsets(10);
        ovrdCnt->AddChild(new BCheckBox("On", "On", new BMessage(MSG_OVRD_TOGGLE)));
        ovrdCnt->AddChild(typeSelector);
        ovrdCnt->AddChild(new BSlider("drive", "Drive", new BMessage(MSG_OVRD_DRIVE), 0, 127, B_HORIZONTAL));
        ovrdCnt->AddChild(new BSlider("level", "Level", new BMessage(MSG_OVRD_LEVEL), 0, 127, B_HORIZONTAL));
        ovrdCnt->AddChild(new BSlider("lpf", "LPF", new BMessage(MSG_OVRD_LPF), 0, 127, B_HORIZONTAL));
        ovrdCnt->AddChild(new BSlider("hpf", "HPF", new BMessage(MSG_OVRD_HPF), 0, 127, B_HORIZONTAL));
        ovrdBox->AddChild(ovrdCnt);

        // --- Echo Section ---
        BBox* echoBox = new BBox("Echo");
        echoBox->SetLabel("Echo");
        
        BGroupView* echoCnt = new BGroupView(B_VERTICAL, 5);
        echoCnt->GroupLayout()->SetInsets(10);
        echoCnt->AddChild(new BCheckBox("On", "On", new BMessage(MSG_ECHO_TOGGLE)));
        echoCnt->AddChild(new BSlider("delay", "Delay", new BMessage(MSG_ECHO_DELAY), 0, 127, B_HORIZONTAL));
        echoCnt->AddChild(new BSlider("fb", "Feedback", new BMessage(MSG_ECHO_FB), 0, 127, B_HORIZONTAL));
        echoBox->AddChild(echoCnt);


        // --- Layout: Horizontal Rack Style ---
        BLayoutBuilder::Group<>(this, B_VERTICAL, 0)
            .SetInsets(10)
            .AddGroup(B_HORIZONTAL, 20)
                .Add(fCpuDisplay)
                .Add(fMasterFX)
                .AddGlue()
            .End()
            .AddStrut(10)
            .AddGroup(B_HORIZONTAL, 10) 
                .Add(ovrdBox)
                .Add(echoBox)
                .AddGlue() 
            .End()
            .AddGlue(); 
    }

    virtual void Pulse() {
        if (!fRkr) return;
        char cpuBuf[32];
        sprintf(cpuBuf, "CPU: %5.2f%%", (float)fRkr->cpuload);
        fCpuDisplay->SetText(cpuBuf);
    }

private:
    RKR* fRkr;
    BStringView* fCpuDisplay;
    BCheckBox* fMasterFX;
};

class RakarrackWindow : public BWindow {
public:
    RakarrackWindow(BRect frame, RKR* rkr) 
        : BWindow(frame, "Rakarrack Native", B_DOCUMENT_WINDOW, 
          B_ASYNCHRONOUS_CONTROLS | B_QUIT_ON_WINDOW_CLOSE) {
        
        fRkr = rkr;
        SetLayout(new BGroupLayout(B_VERTICAL));

        RakarrackView* mainView = new RakarrackView(rkr);
        mainView->SetExplicitMinSize(BSize(300, 200));

        BScrollView* scroller = new BScrollView("rack_scroll", mainView, 0, true, true);

        BLayoutBuilder::Group<>(this, B_VERTICAL, 0)
            .Add(scroller)
            .End();

        this->SetSizeLimits(200, 10000, 150, 10000);
        SetPulseRate(50000);
    }

    virtual void MessageReceived(BMessage* msg) {
        pthread_mutex_lock(&jmutex);
        
        switch (msg->what) {
            case MSG_MASTER_FX:
            {
                int32 value = msg->GetInt32("be:value", B_CONTROL_OFF);
    if (value == B_CONTROL_ON) {
        fRkr->Bypass = 1; 
        fRkr->calculavol(1);
        fRkr->calculavol(2);
        fRkr->booster = 1.0f;
    } else {
        fRkr->Bypass = 0;
    }
                break;
            }
            case MSG_OVRD_TOGGLE:
                fRkr->Overdrive_Bypass = !fRkr->Overdrive_Bypass;
                break;
            case MSG_OVRD_TYPE:
                if (fRkr->efx_Overdrive)
                    fRkr->efx_Overdrive->Ptype = msg->FindInt32("index");
                break;
            case MSG_OVRD_DRIVE:
                if (fRkr->efx_Overdrive)
                    fRkr->efx_Overdrive->Pdrive = (int)msg->GetInt32("be:value", 90);
                break;
            case MSG_OVRD_LEVEL:
                if (fRkr->efx_Overdrive)
                    fRkr->efx_Overdrive->Plevel = (int)msg->GetInt32("be:value", 64);
                break;
            case MSG_OVRD_LPF:
                if (fRkr->efx_Overdrive)
                    fRkr->efx_Overdrive->Plpf = (int)msg->GetInt32("be:value", 127);
                break;
            case MSG_OVRD_HPF:
                if (fRkr->efx_Overdrive)
                    fRkr->efx_Overdrive->Phpf = (int)msg->GetInt32("be:value", 0);
                break;
            case MSG_ECHO_TOGGLE:
                fRkr->Echo_Bypass = !fRkr->Echo_Bypass;
                break;
            case MSG_ECHO_DELAY:
                if (fRkr->efx_Echo)
                    fRkr->efx_Echo->Pdelay = (int)msg->GetInt32("be:value", 0);
                break;
            case MSG_ECHO_FB:
                if (fRkr->efx_Echo)
                    fRkr->efx_Echo->Pfb = (int)msg->GetInt32("be:value", 0);
                break;
            default:
                BWindow::MessageReceived(msg);
        }
        
        pthread_mutex_unlock(&jmutex);
    }

private:
    RKR* fRkr;
};

extern "C" void start_haiku_native_interface(void* rkr_ptr) {
    RKR* rkr = (RKR*)rkr_ptr;
    RakarrackWindow *win = new RakarrackWindow(BRect(100, 100, 550, 400), rkr);
    win->Show();
}

