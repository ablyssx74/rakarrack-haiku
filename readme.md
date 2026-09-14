### Rakarrack Haiku is a port-in-progress of the great [rakarrack project](https://rakarrack.sourceforge.net/)

### Current Status:
- Pretty much everthing works like effects and even midi with the exception of a few things here there.
-  Midi velocity - not really needed but may look into it eventually. Low priority.
-  Background images.  Not really needed but had to remove them because the files caused a crash in 32bit builds likey due to pixel 4 byte misalignment. May add custom bg images another day. Low priority.
-  ACI - Not really sure even how to use this correctly.  Perhaps requires AUX input for secondary source.  Low priority.
- Midi mapping and other original midi settings - Not really sure how to do this either. Low priority.

Requires: Haiku 64bit or 32bit 

To see help:  ```make -f haiku.makefile help```

1. To configure:  ```make -f haiku.makefile config ```

2. To build:  ``` make -f haiku.makefile ``` 

3. To package:  ```make -f haiku.makefile package ```

Steps 1,2,3 all together ``` make -f haiku.makefile release ```

To clean: ``` make -f haiku.makefile clean``` 

<br>
GTK Theme<br>
<img width="510" height="460" alt="screenshot" src="https://github.com/user-attachments/assets/1c5eb8c9-33a9-4d51-bb37-7387207e271f" /><br>
Plastic Theme<br>
<img width="510" height="460" alt="Image" src="https://github.com/user-attachments/assets/04c3984a-30de-4aba-96fb-82ead7a4def3" /><br>
From Terminal rakarrack -H ( Haiku mode )<br>
<img width="510" height="460" alt="Image" src="https://github.com/user-attachments/assets/ccd84f1f-571b-4689-a1c8-f4b649d4cdee" />
