### Rakarrack Haiku is a port in progress of the great [rakarrack project](https://rakarrack.sourceforge.net/)


To configure run ```make -f haiku.makefile config ```<br>
To build run ``` make -f haiku.makefile ``` <br>

If build goes all okay you will have ./rakarrack in the root directory <br>

To clean the build run ``` make -f haiku.makefile clean``` <br>



Things to keep in mind: <br>
In my experience, 48kHz gives decent latency. however  the frames are locked in at 128 by the system private media kit code.<br>
Frequency  can be configured after running ```make -f haiku.makefile config``` by adding the flag ``` make -f haiku.makefile RATE=96000.0 ``` if you have to have 96kHz. <br>
Future plans are to add code to bypass the frames  so lower frames can be used if desired.<br>
Update Input/Output frequency in Haiku Media Preferences to match.<br>

Known bugs: <br>
rakkarack media node sticks around after closing rakarrack. <br>
Restart media server to clear the zombie node else it won't connect again.<br>

