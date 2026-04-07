### Rakarrack Haiku is a port in progress of the great [rakarrack project](https://rakarrack.sourceforge.net/)


To configure run ```make -f haiku.makefile config ```<br>
To build run ``` make -f haiku.makefile ``` <br>

If build goes all okay you will have ./rakarrack in the root directory <br>

If make -f haiku.makefile fails the first time just rerun ```make -f haiku.makefile config``` again.  That usually works for stale autoconf variables. <br>


To clean the build run ``` make -f haiku.makefile clean``` <br>



Things to keep in mind: <br>

Sample rate is hardcoded in at 48 kHz. <br>
Update Input/Output frequency in Haiku Media Prefernces to match.<br>

Known bugs: <br>
Upsampling will cause a crash. Maybe not worth looking into as the sound is already amazing on Haiku<br>
rakkarack media node sticks around after closing rakarrack. <br>
Restart media server to clear the zombie node else it won't connect again.<br>

