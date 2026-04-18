### Rakarrack Haiku is a port in progress of the great [rakarrack project](https://rakarrack.sourceforge.net/)

### Current Status:
- Pretty much everthing works like effects and even midi with the exception of a few things here there.
-  Midi velocity - not really needed but may look into it eventually. Low priority.
-  Background images.  Not really needed but had to remove them because the files caused a crash in 32bit builds likey due to pixel 4 byte misalignment. May add custom bg images another day. Low priority.
-  ALC - Not really sure even how to use this correctly.  Perhaps requires AUX input for secondary source.  Low priority.
- Midi mapping and other original midi settings - Not really sure how to do this either. Low priority.

Requires: Haiku 64bit or 32bit 

To see help run ```make -f haiku.makefile help```

To configure run ```make -f haiku.makefile config ```

To build run ``` make -f haiku.makefile ``` 

To package run ```make -f haiku.makefile package ```

To clean the build run ``` make -f haiku.makefile clean``` 

<br>
GTK Theme<br>
<img width="510" height="460" alt="screenshot" src="https://github.com/user-attachments/assets/1c5eb8c9-33a9-4d51-bb37-7387207e271f" /><br>
Plastic Theme<br>
<img width="510" height="460" alt="Image" src="https://github.com/user-attachments/assets/04c3984a-30de-4aba-96fb-82ead7a4def3" /><br>
Preferences Menu<br>
<img width="510" height="460" alt="Image" src="https://github.com/user-attachments/assets/f3ba92ce-0ade-4020-8840-ff964cc9cc47" />
