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
#include "../src/rakarrack_haiku_bridge.h"

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
    MSG_ECHO_FB     = 'ecfb' 
};

class RakarrackView : public BView {
public:
    RakarrackView(RKR* rkr) : BView("MainView", B_WILL_DRAW | B_PULSE_NEEDED) {
        fRkr = rkr;
        SetViewColor(180, 180, 180);

        fCpuDisplay = new BStringView("cpu", "CPU: 0.00%");
        fCpuDisplay->SetHighColor(100, 0, 0);

        // 1. Create the Preset Menu
        BPopUpMenu* typeMenu = new BPopUpMenu("Select Type");
        
        // We add items and attach a message to each. 
        // We can put the type index (0, 1, 2...) directly in the message.
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
        ovrdCnt->AddChild(typeSelector); // Add the dropdown here
        ovrdCnt->AddChild(new BSlider("drive", "Drive", new BMessage(MSG_OVRD_DRIVE), 0, 127, B_HORIZONTAL));
        ovrdCnt->AddChild(new BSlider("level", "Level", new BMessage(MSG_OVRD_LEVEL), 0, 127, B_HORIZONTAL));
        ovrdCnt->AddChild(new BSlider("lpf", "LPF", new BMessage(MSG_OVRD_LPF), 0, 127, B_HORIZONTAL));
        ovrdCnt->AddChild(new BSlider("hpf", "HPF", new BMessage(MSG_OVRD_HPF), 0, 127, B_HORIZONTAL));
        
        ovrdBox->AddChild(ovrdCnt);

        BLayoutBuilder::Group<>(this, B_VERTICAL, 10)
            .SetInsets(10)
            .Add(fCpuDisplay)
            .Add(ovrdBox)
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
};

class RakarrackWindow : public BWindow {
public:
    RakarrackWindow(BRect frame, RKR* rkr) 
        : BWindow(frame, "Rakarrack Native", B_TITLED_WINDOW, 
          B_ASYNCHRONOUS_CONTROLS | B_QUIT_ON_WINDOW_CLOSE | B_AUTO_UPDATE_SIZE_LIMITS) {
        
        fRkr = rkr;
        SetLayout(new BGroupLayout(B_VERTICAL));
        AddChild(new RakarrackView(rkr));
    }


virtual void MessageReceived(BMessage* msg) {
    pthread_mutex_lock(&jmutex);
    
    switch (msg->what) {
        // --- Distortion Dashboard ---
        case MSG_OVRD_DRIVE:
            if (fRkr->efx_Overdrive)
                fRkr->efx_Overdrive->Pdrive = (int)msg->GetInt32("be:value", 90);
            break;
        case MSG_OVRD_LEVEL:
            if (fRkr->efx_Overdrive)
                fRkr->efx_Overdrive->Plevel = (int)msg->GetInt32("be:value", 64);
            break;

case MSG_ECHO_DELAY:
    if (fRkr->efx_Echo)
        fRkr->efx_Echo->Pdelay = (int)msg->GetInt32("be:value", 0);
    break;

case MSG_ECHO_FB:
    if (fRkr->efx_Echo)
        fRkr->efx_Echo->Pfb = (int)msg->GetInt32("be:value", 0); // Changed to Pfb
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
    RakarrackWindow *win = new RakarrackWindow(BRect(100, 100, 500, 500), rkr);
    win->Show();
}

