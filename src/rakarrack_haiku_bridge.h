/*
 * Copyright 2026, Kris Beazley jb@epluribusunix.net
 * All rights reserved. Distributed under the terms of the MIT license.
 */



#ifndef RAKARRACK_HAIKU_BRIDGE_H
#define RAKARRACK_HAIKU_BRIDGE_H

#ifdef __cplusplus
extern "C" {
#endif

// Use a simple void pointer to pass the engine state
// This prevents including the heavy rakarrack.h in our clean Haiku code
void start_haiku_native_interface(void* rkr_ptr);

#ifdef __cplusplus
}
#endif

// Set (by RakarrackApp::QuitRequested() in main.C) before the app-wide
// quit cascade asks every window -- including native mode's OrderWindow,
// see its own QuitRequested() override in haiku_native/haiku-rakarrack.cpp
// -- whether it agrees to quit. Distinguishes an external/whole-app quit
// (Deskbar/ProcessController's "Quit Application", a session shutdown,
// B_QUIT_REQUESTED sent straight to the app) from the user just closing
// that one window on its own, which should still only hide it. Defined in
// haiku-rakarrack.cpp (linked into every target, including the small
// extra/ utilities main.C itself never reaches), declared here so both
// translation units agree on its type without needing a weak-symbol
// fallback for it.
extern bool gAppQuitting;

#endif

