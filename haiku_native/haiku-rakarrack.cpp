/*
 * Copyright 2026, Kris Beazley jb@epluribusunix.net
 * All rights reserved. Distributed under the terms of the MIT license.
 */

#include <app/Looper.h>
#include <media/BufferProducer.h>
#include <app/Application.h>
#include <app/Message.h>
#include <support/Archivable.h>
#include <media/TimeSource.h>
#include <media/MediaEventLooper.h>
#include <SupportDefs.h>
#include <OS.h>
#include <syslog.h>
#include <math.h>
#include <Alert.h>
#include "../src/rakarrack_haiku_bridge.h"
#include <interface/Window.h>
#include <interface/View.h>
#include <interface/InterfaceDefs.h>
#include <StatusBar.h> 



class RakarrackView : public BView {
public:
    // This is the Constructor
    RakarrackView(BRect frame) 
        : BView(frame, "MainView", B_FOLLOW_ALL, B_WILL_DRAW) {
        
        SetViewColor(30, 30, 30); // Dark gray background

// Inside RakarrackView constructor
fLeftMeter = new BStatusBar(BRect(20, 50, 300, 65), "left_vu", NULL, NULL);
fRightMeter = new BStatusBar(BRect(20, 75, 300, 90), "right_vu", NULL, NULL);

fLeftMeter->SetBarHeight(12.0);
fRightMeter->SetBarHeight(12.0);

// Use a darker background for the "empty" part of the bar
fLeftMeter->SetViewColor(20, 20, 20); 
fRightMeter->SetViewColor(20, 20, 20);


        AddChild(fLeftMeter);
        AddChild(fRightMeter);
    }

    virtual void Draw(BRect updateRect) {
        SetHighColor(100, 100, 100); 
        StrokeRect(Bounds());
    }

private:
    // Store pointers to the meters so we can update them later
    BStatusBar* fLeftMeter;
    BStatusBar* fRightMeter;
};

// Main Native Window
class RakarrackWindow : public BWindow {
public:
    RakarrackWindow(BRect frame) 
        : BWindow(frame, "Rakarrack Native", B_TITLED_WINDOW, 
                  B_ASYNCHRONOUS_CONTROLS | B_QUIT_ON_WINDOW_CLOSE) {
        AddChild(new RakarrackView(Bounds()));
    }
};

extern "C" void start_haiku_native_interface(void* rkr_ptr) {
    // Only Haiku code here
	RakarrackWindow *win = new RakarrackWindow(BRect(100, 100, 600, 500));
	win->Show();

}
