/*
  rakarrack - a guitar efects software

  jack.C  -   jack I/O
  Copyright (C) 2008-2010 Josep Andreu
  Author: Josep Andreu

  This program is free software; you can redistribute it and/or modify
  it under the terms of version 2 of the GNU General Public License
  as published by the Free Software Foundation.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License (version 2) for more details.

  You should have received a copy of the GNU General Public License
(version2)
  along with this program; if not, write to the Free Software Foundation,
  Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
  
  
  Updated by Kris Beazley aka ablyss for Haiku OS with the help of AI
  Copyright 2026
*/
#include <app/Looper.h>
#include <app/Application.h>

#include <signal.h>
#include <unistd.h>

#include <getopt.h>
#include <sched.h>
#include <sys/mman.h>
#include <pthread.h>
#include "global.h"
#include "rakarrack.h"
#include "jack.h"
#include "rakarrack_haiku_bridge.h"


// Haiku Update
extern "C" void start_haiku_native_interface(void* rkr_ptr);
extern bool gDebugMode;
extern "C" void HaikuAudioShutdown();
extern "C" RKR *rk;
RKR *rk = nullptr; 
RKRGUI *rakgui = nullptr; 

extern bool haiku_mode;
bool haiku_mode = false;  


void
show_help ()
{
  fprintf (stderr, "Usage: rakarrack [OPTION]\n\n");
  fprintf (stderr,
	   "  -h ,     --help \t\t\t display command-line help and exit\n");
  fprintf (stderr, "  -n ,     --no-gui \t\t\t disable GUI\n");
  fprintf (stderr, "  -D ,     --debug \t\t\t enable verbose debug logging\n"); 
  fprintf (stderr, "  -H ,     --haiku \t\t\t enable testing haiku native code\n"); 
  fprintf (stderr, "  -l File, --load=File \t\t\t loads preset\n");
  fprintf (stderr, "  -b File, --bank=File \t\t\t loads bank\n");
  fprintf (stderr, "  -p #,    --preset=# \t\t\t set preset\n");
  fprintf (stderr, "  -x, --dump-preset-names \t\t prints bank of preset names and IDs\n\n");
  fprintf (stderr, "FLTK options are:\n\n");
  fprintf (stderr, "  -bg2 color\n");
  fprintf (stderr, "  -bg color\n");
  fprintf (stderr, "  -di[splay] host:n.n\n");
  fprintf (stderr, "  -dn[d]\n");
  fprintf (stderr, "  -fg color\n");
  fprintf (stderr, "  -g[eometry] WxH+X+Y\n");
  fprintf (stderr, "  -i[conic]\n");
  fprintf (stderr, "  -k[bd]\n");
  fprintf (stderr, "  -na[me] classname\n");
  fprintf (stderr, "  -nod[nd]\n");
  fprintf (stderr, "  -nok[bd]\n");
  fprintf (stderr, "  -not[ooltips]\n");
  fprintf (stderr, "  -s[cheme] scheme (plastic,none,gtk+)\n");
  fprintf (stderr, "  -ti[tle] windowtitle\n");
  fprintf (stderr, "  -to[oltips]\n");
  fprintf (stderr, "\n");

}

int
main (int argc, char *argv[])
{
	BApplication* myApp = nullptr; 
	RKR rkr;
    rk = &rkr; 
    int preset = 1000;
    bool exitwithhelp = false;
	int gui = 1; 
	
    // 1. Parse arguments FIRST
    struct option opts[] = {
        {"load", 1, NULL, 'l'},
        {"bank", 1, NULL, 'b'},
        {"preset",1,NULL, 'p'},
        {"no-gui", 0, NULL, 'n'},
        {"debug", 0, 0, 'D'},
        {"haiku", 0, 0, 'H'},
        {"dump-preset-names", 0, NULL, 'x'},
        {"help", 0, NULL, 'h'},
        {0, 0, 0, 0}
    };

    int option_index = 0, opt;
    while ((opt = getopt_long(argc, argv, "l:b:p:nDxhH", opts, &option_index)) != -1) {
        switch (opt) {
            case 'H': haiku_mode = true; break;
            case 'D': gDebugMode = true; break;
            case 'n': gui = 0; break;
            case 'h': exitwithhelp = 1; break;
            case 'l': if (optarg) rkr.loadfile(optarg); break;
            case 'b': if (optarg) rkr.loadbank(optarg); break;
            case 'p': if (optarg) preset = atoi(optarg); break;
            case 'x': rkr.dump_preset_names(); exit(0); break;
            }           
       
    }
    

    if (exitwithhelp) {
        show_help();
        return 0;
    }


    if (haiku_mode) {

        myApp = new BApplication("application/x-vnd.rakarrack-haiku");
        

  		JACKstart (&rkr, rkr.jackclient);
		rkr.InitMIDI ();
  		rkr.ConnectMIDI ();

  
        start_haiku_native_interface(rk);
        
		printf("Haiku Native Mode Started.\n");
        myApp->Run();

        
    } else {
        // --- FLTK / Standard Path ---
        if (be_app == nullptr) {
            myApp = new BApplication("application/x-vnd.rakarrack-haiku");
        }
        Fl::lock();
        if (gui) rakgui = new RKRGUI(argc, argv, &rkr);

        JACKstart (&rkr, rkr.jackclient);
        rkr.InitMIDI ();
        rkr.ConnectMIDI ();

  		// --- Haiku UI & Engine Sync ---
 		 if (gui != 0) {
      	// Access the specific Rakarrack-Haiku preferences
      	Fl_Preferences prefs(Fl_Preferences::USER, "rakarrack.sf.net", "rakarrack");
      
      	int fx_init = 0;
      	// Using the exact key string required for the Haiku build
      	prefs.get("Rakarrack-Haiku FX_init_state", fx_init, 0); 
      
      	if (fx_init == 1) {
          rakgui->INSTATE->value(1);           // Sync the Settings checkbox
          rakgui->ActivarGeneral->value(1);    // Set the LED button state
          rakgui->ActivarGeneral->do_callback(); // Trigger engine & lighting logic
          rakgui->ActivarGeneral->redraw();    // Force Haiku redraw
      	}
      
      	// Priming the audio engine
      	rkr.calculavol(1);
      	rkr.calculavol(2);
      	rkr.booster = 1.0f;
   }

  // --- Haiku Main Loop ---
  while (Pexitprogram == 0) {
      if (gui) {
          // Standard FLTK event handling for Haiku
          Fl::wait(0.1); 
          
          if (Fl::first_window() == NULL) {
              Pexitprogram = 1;
              break; 
          }
      } else {
          // Headless mode processing
          usleep(1500);
          if (preset != 1000) {
              if ((preset > 0) && (preset < 61)) rkr.Bank_to_Preset(preset);
              preset = 1000;
          }
        }
      }
    }

    printf("Rakarrack loop ended. Cleaning up audio...\n");
    HaikuAudioShutdown();
    fflush(stdout);
    if (myApp) {

        if (myApp->Lock()) {
            myApp->PostMessage(B_QUIT_REQUESTED);
            myApp->Unlock();
        }
    }
  _exit(0); 
}
  

