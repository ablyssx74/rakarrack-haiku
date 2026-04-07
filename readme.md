### Rakarrack Haiku is a port in progress of the great [rakarrack project](https://rakarrack.sourceforge.net/)


To configure run ```make -f haiku.makefile config ```<br>
To build run ``` make -f haiku.makefile ``` <br>
If build goes all okay you will have ./rakarrack in the root directory <br>
To clean the build run ``` make -f haiku.makefile clean``` <br>

Things to keep in mind: <br>

Sample rate is hardcoded in at 48 kHz. <br>
Update Input/Output frequency in Haiku Media Prefernces to match.<br>

Known bug: <br>

rakkarack media node sticks around after closing rakarrack. <br>
Restart media server to clear the zombie node else it won't connect again.<br>

