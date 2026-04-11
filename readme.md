### Rakarrack Haiku is a port in progress of the great [rakarrack project](https://rakarrack.sourceforge.net/)

Requires: Haiku 64bit fltk_devel fontconfig_devel freetype_devel libxfont2_devel libsndfile_devel libsamplerate_devel libxpm_devel

To see help run ```make -f haiku.makefile help```

To configure run ```make -f haiku.makefile config ```

To build run ``` make -f haiku.makefile ``` 

If build goes all okay you will have ./rakarrack in the root directory 

To clean the build run ``` make -f haiku.makefile clean``` 

Notes: <br>

Input/Output frequency needs to match else rakarrack app will crash.  <br>
48kHz is ideal for Haiku Media setttings for rakarrack engine to sound the best in my experience but you can test others if you want.<br>


<img width="400" height="460" alt="screenshot" src="https://github.com/user-attachments/assets/1c5eb8c9-33a9-4d51-bb37-7387207e271f" />
