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

#endif

