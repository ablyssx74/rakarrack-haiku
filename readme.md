### Rakarrack Haiku is a port in progress of the great [rakarrack project](https://rakarrack.sourceforge.net/)


To configure run ```make -f haiku.makefile config ```<br>
To build run ``` make -f haiku.makefile ``` <br>

If build goes all okay you will have ./rakarrack in the root directory <br>

To clean the build run ``` make -f haiku.makefile clean``` <br>



Things to keep in mind: <br>

Input/Output frequency & Sample rate can be configured after running ```make -f haiku.makefile config`` by adding the flags<br>
 ``` make -f haiku.makefile RATE=48000.0 FRAMES=16 ```. <br>
Update Input/Output frequency in Haiku Media Prefernces to match.<br>

Known bugs: <br>
rakkarack media node sticks around after closing rakarrack. <br>
Restart media server to clear the zombie node else it won't connect again.<br>

