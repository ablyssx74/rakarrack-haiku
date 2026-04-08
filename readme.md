### Rakarrack Haiku is a port in progress of the great [rakarrack project](https://rakarrack.sourceforge.net/)

Requires: Haiku 64bit fltk_devel libsamplerate_devel libsndfile_devel libxfont_devel libxfont2_devel libxpm_devel

To configure run ```make -f haiku.makefile config ```

To build run ``` make -f haiku.makefile ``` 

If build goes all okay you will have ./rakarrack in the root directory 

To clean the build run ``` make -f haiku.makefile clean``` 



Notes: <br>

Update Input/Output frequency in Haiku Media Preferences to match 48kHz if engine is built to use 48kHz<br><br>


