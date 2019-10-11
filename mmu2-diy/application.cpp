/*********************************************************************************************************
* MMU2 Clone Controller Version
**********************************************************************************************************
*
* Actual Code developed by Jeremy Briffaut
* Initial Code developed by Chuck Kozlowski
*/

#include <Arduino.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#include "application.h"

#include "config.h"

/*************** */
char cstr[16];
#include "print.h"
IOPrint ioprint;
/*************** */

int command = 0;

// absolute position of bearing stepper motor
int bearingAbsPos[5] = {0 + IDLEROFFSET[0], IDLERSTEPSIZE + IDLEROFFSET[1], IDLERSTEPSIZE * 2 + IDLEROFFSET[2], IDLERSTEPSIZE * 3 + IDLEROFFSET[3], IDLERSTEPSIZE * 4 + IDLEROFFSET[4]};
// absolute position of selector stepper motor
int selectorAbsPos[5] = {0 + CSOFFSET[0], CSSTEPS * 1 + CSOFFSET[1], CSSTEPS * 2 + CSOFFSET[2], CSSTEPS * 3 + CSOFFSET[3], CSSTEPS * 4 + CSOFFSET[4]};

//stepper direction
#define CW 0
#define CCW 1

// used for 3 states of the idler stepper motor (
#define INACTIVE 0	// parked
#define ACTIVE 1	  // not parked
#define QUICKPARKED 2 // quick parked

#define STOP_AT_EXTRUDER 1
#define IGNORE_STOP_AT_EXTRUDER 0

int trackToolChanges = 0;
int extruderMotorStatus = INACTIVE;

int currentCSPosition = 0; // color selector position
int currentPosition = 0;

int repeatTCmdFlag = INACTIVE; // used by the 'C' command processor to avoid processing multiple 'C' commands

int oldBearingPosition = 0; // this tracks the roller bearing position (top motor on the MMU)
int filamentSelection = 0;  // keep track of filament selection (0,1,2,3,4))
int dummy[100];
char currentExtruder = '0';

int firstTimeFlag = 0;
int earlyCommands = 0; // forcing communications with the mk3 at startup

int toolChangeCount = 0;

char receivedChar;
boolean newData = false;
int idlerStatus = INACTIVE;
int colorSelectorStatus = INACTIVE;

/*****************************************************
 *
 *
 *****************************************************/
void Application::setup()
{
	int waitCount;
	/************/
	ioprint.setup();
	/************/

	println_log(MMU2_VERSION);
	delay(200);

	Serial1.begin(115200); // Hardware serial interface (mmu<->printer board)
	delay(100);

	println_log(F("Sending START command to mk3 controller board"));
	// ***************************************
	// THIS NEXT COMMAND IS CRITICAL ... IT TELLS THE MK3 controller that an MMU is present
	// ***************************************
	Serial1.print(F("start\n")); // attempt to tell the mk3 that the mmu is present

	//***************************
	//  check the serial interface to see if it is active
	//***************************
	waitCount = 0;
	while (!Serial1.available())
	{

		//delay(100);
		println_log(F("Waiting for message from mk3"));
		delay(1000);
		++waitCount;
		if (waitCount >= S1_WAIT_TIME)
		{
			println_log(F("X seconds have passed, aborting wait for printer board (Marlin) to respond"));
			goto continue_processing;
		}
	}
	println_log(F("inbound message from Marlin"));

continue_processing:

	pinMode(idlerDirPin, OUTPUT);
	pinMode(idlerStepPin, OUTPUT);
	pinMode(idlerEnablePin, OUTPUT);

	pinMode(findaPin, INPUT); // MMU pinda Filament sensor
	pinMode(filamentSwitch, INPUT); // extruder Filament sensor
	pinMode(colorSelectorEnstop, INPUT_PULLUP); // enstop switch sensor

	pinMode(extruderEnablePin, OUTPUT);
	pinMode(extruderDirPin, OUTPUT);
	pinMode(extruderStepPin, OUTPUT);

	pinMode(colorSelectorEnablePin, OUTPUT);
	pinMode(colorSelectorDirPin, OUTPUT);
	pinMode(colorSelectorStepPin, OUTPUT);

	pinMode(greenLED, OUTPUT); // green LED used for debug purposes

	println_log(F("finished setting up input and output pins"));

	// Turn OFF all three stepper motors (heat protection)
	digitalWrite(idlerEnablePin, DISABLE);		   // DISABLE the roller bearing motor (motor #1)
	digitalWrite(extruderEnablePin, DISABLE);	  //  DISABLE the extruder motor  (motor #2)
	digitalWrite(colorSelectorEnablePin, DISABLE); // DISABLE the color selector motor  (motor #3)

    // Initialize stepper
	println_log(F("Syncing the Idler Selector Assembly")); // do this before moving the selector motor
	initIdlerPosition();								   // reset the roller bearing position

	println_log(F("Syncing the Filament Selector Assembly"));
	if (!isFilamentLoadedPinda())
	{
		initColorSelector(); // reset the color selector if there is NO filament present
	}
	else
	{
		println_log(F("Unable to clear the Color Selector, please remove filament"));
	}

	println_log(F("Inialialization Complete, let's multicolor print ...."));

} // end of init() routine

/**
 * Serial read until new line
 */
String ReadSerialStrUntilNewLine()
{
	String str = "";
	char c = -1;
	while ((c != '\n') && (c != '\r'))
	{
		if (Serial.available())
		{
			c = char(Serial.read());
			if (c != -1)
			{
				str += c;
			}
		}
	}
	return str;
}

/*****************************************************
 * infinite loop - core of the program
 *
 *****************************************************/
void Application::loop()
{
	String kbString;

	// println_log(F("looping"));
	delay(100);				// wait for 100 milliseconds
	checkSerialInterface(); // check the serial interface for input commands from the mk3

#ifdef SERIAL_DEBUG
	// check for keyboard input

	if (Serial.available())
	{
		print_log(F("Key was hit "));

		kbString = ReadSerialStrUntilNewLine();

		if (kbString[0] == 'C')
		{
			println_log(F("Processing 'C' Command"));
			filamentLoadWithBondTechGear();
		}
		if (kbString[0] == 'T')
		{
			println_log(F("Processing 'T' Command"));
			if ((kbString[1] >= '0') && (kbString[1] <= '4'))
			{
				toolChange(kbString[1]); // invoke the tool change command
										 //toolChange(c2);
										 // processKeyboardInput();
			}
			else
			{
				println_log(F("T: Invalid filament Selection"));
			}
		}
		if (kbString[0] == 'U')
		{
			println_log(F("Processing 'U' Command"));

			// parkIdler();                      // reset the idler               // added on 10.7.18 ... get in known state

			if (idlerStatus == QUICKPARKED)
			{
				quickUnParkIdler(); // un-park the idler from a quick park
			}
			if (idlerStatus == INACTIVE)
			{
				unParkIdler(); // turn on the idler motor
			}
			unloadFilamentToFinda(); //unload the filament

			parkIdler(); // park the idler motor and turn it off
		}
#ifdef DEBUGMODE
		if (kbString[0] == 'D')
		{
			println_log(F("Processing 'D' Command"));
			println_log(F("initColorSelector"));
			initColorSelector();
			println_log(F("initIdlerPosition"));
			initIdlerPosition();
			println_log(F("feedFilament"));
			feedFilament(STEPSPERMM * 100, IGNORE_STOP_AT_EXTRUDER);
			println_log(F("Color 0"));
			idlerSelector('0');
			colorSelector('0');
			delay(5000);
			println_log(F("Color 1"));
			idlerSelector('1');
			colorSelector('1');
			delay(5000);
			println_log(F("Color 2"));
			idlerSelector('2');
			colorSelector('2');
			delay(5000);
			println_log(F("Color 3"));
			idlerSelector('3');
			colorSelector('3');
			delay(5000);
			println_log(F("Color 4"));
			idlerSelector('4');
			colorSelector('4');
			delay(5000);
		}
		else if (kbString[0] == 'Z')
		{
			print_log(F("FINDA status: "));
			int fstatus = digitalRead(findaPin);
			println_log(fstatus);
			print_log(F("colorSelectorEnstop status: "));
			int cdenstatus = digitalRead(colorSelectorEnstop);
			println_log(cdenstatus);
			print_log(F("Extruder endstop status: "));
			fstatus = digitalRead(filamentSwitch);
			Serial.println(fstatus);
		}
		else if (kbString[0] == 'A')
		{
			println_log(F("Processing 'D' Command"));
			println_log(F("initColorSelector"));
			initColorSelector();
			println_log(F("initIdlerPosition"));
			initIdlerPosition();
			println_log(F("T0"));
			toolChange('0');
			delay(2000);
			println_log(F("T1"));
			toolChange('1');
			delay(2000);
			println_log(F("T2"));
			toolChange('2');
			delay(2000);
			println_log(F("T3"));
			toolChange('3');
			delay(2000);
			println_log(F("T4"));
			toolChange('4');
			delay(2000);
			println_log(F("T0"));
			toolChange('0');
			delay(2000);

			if (idlerStatus == QUICKPARKED)
			{
				quickUnParkIdler(); // un-park the idler from a quick park
			}
			if (idlerStatus == INACTIVE)
			{
				unParkIdler(); // turn on the idler motor
			}
			unloadFilamentToFinda(); //unload the filament
			parkIdler();			 // park the idler motor and turn it off
		}
#endif
	}
#endif

} // end of infinite loop

/*****************************************************
 *
 * Handle command from the Printer
 * 
 *****************************************************/
void checkSerialInterface()
{
	int cnt;
	String inputLine;
	int index;

	index = 0;
	if ((cnt = Serial1.available()) > 0)
	{

		inputLine = Serial1.readString(); // fetch the command from the mmu2 serial input interface

		if (inputLine[0] != 'P')
		{
			print_log(F("MMU Command: "));
			println_log(inputLine);
		}
	process_more_commands: // parse the inbound command
		unsigned char c1, c2;

		c1 = inputLine[index++]; // fetch single characer from the input line
		c2 = inputLine[index++]; // fetch 2nd character from the input line
		inputLine[index++];		 // carriage return

		// process commands coming from the mk3 controller
		//***********************************************************************************
		// Commands still to be implemented:  
		// X0 (MMU Reset)
		// F0 (Filament type select),
		// E0->E4 (Eject Filament)
		// R0 (recover from eject)
		//***********************************************************************************
		switch (c1)
		{
		case 'T':
			// request for idler and selector based on filament number
			if ((c2 >= '0') && (c2 <= '4'))
			{
				toolChange(c2);
			}
			else
			{
				println_log(F("T: Invalid filament Selection"));
			}

			Serial1.print(F("ok\n")); // send command acknowledge back to mk3 controller
			break;
		case 'C':
			// move filament from selector ALL the way to printhead
			if (filamentLoadWithBondTechGear())
				Serial1.print(F("ok\n"));
			break;

		case 'U':
			// request for filament unload

			println_log(F("U: Filament Unload Selected"));
			if (idlerStatus == QUICKPARKED)
			{
				quickUnParkIdler(); // un-park the idler from a quick park
			}
			if (idlerStatus == INACTIVE)
			{
				unParkIdler(); // turn on the idler motor
			}

			if ((c2 >= '0') && (c2 <= '4'))
			{
				unloadFilamentToFinda();
				parkIdler();
				println_log(F("U: Sending Filament Unload Acknowledge to MK3"));
				delay(200);
				Serial1.print(F("ok\n"));
			}
			else
			{
				println_log(F("U: Invalid filament Unload Requested"));
				delay(200);
				Serial1.print(F("ok\n"));
			}
			break;
		case 'L':
			// request for filament load
			println_log(F("L: Filament Load Selected"));
			if (idlerStatus == QUICKPARKED)
			{
				quickUnParkIdler(); // un-park the idler from a quick park
			}
			if (idlerStatus == INACTIVE)
			{
				unParkIdler(); // turn on the idler motor
			}

			if (colorSelectorStatus == INACTIVE)
				activateColorSelector(); // turn on the color selector motor

			if ((c2 >= '0') && (c2 <= '4'))
			{
				println_log(F("L: Moving the bearing idler"));
				idlerSelector(c2); // move the filament selector stepper motor to the right spot
				println_log(F("L: Moving the color selector"));
				colorSelector(c2); // move the color Selector stepper Motor to the right spot
				println_log(F("L: Loading the Filament"));
				// loadFilament(CCW);
				loadFilamentToFinda();
				parkIdler(); // turn off the idler roller

				println_log(F("L: Sending Filament Load Acknowledge to MK3"));

				delay(200);

				Serial1.print(F("ok\n"));
			}
			else
			{
				println_log(F("Error: Invalid Filament Number Selected"));
			}
			break;

		case 'S':
			// request for firmware version
			switch (c2)
			{
			case '0':
				println_log(F("S: Sending back OK to MK3"));
				Serial1.print(F("ok\n"));
				break;
			case '1':
				println_log(F("S: FW Version Request"));
				Serial1.print(FW_VERSION);
				Serial1.print(F("ok\n"));
				break;
			case '2':
				println_log(F("S: Build Number Request"));
				println_log(F("Initial Communication with MK3 Controller: Successful"));
				Serial1.print(FW_BUILDNR);
				Serial1.print(F("ok\n"));
				break;
			default:
				println_log(F("S: Unable to process S Command"));
				break;
			}
			break;
		case 'P':
			// check FINDA status
			if (!isFilamentLoadedPinda())
			{
				Serial1.print(F("0"));
			}
			else
			{
				Serial1.print(F("1"));
			}
			Serial1.print(F("ok\n"));

			break;
		case 'F': 
		    // 'F' command is acknowledged but no processing goes on at the moment
			// will be useful for flexible material down the road
			println_log(F("Filament Type Selected: "));
			println_log(c2);
			Serial1.print(F("ok\n")); // send back OK to the mk3
			break;
		default:
			print_log(F("ERROR: unrecognized command from the MK3 controller"));
			Serial1.print(F("ok\n"));

		} // end of switch statement

	} // end of cnt > 0 check

	if (index < cnt)
	{
		goto process_more_commands;
	}
	// }  // check for early commands
}

/*****************************************************
 *
 * Select the color : selection (0..4)
 * 
 *****************************************************/
void colorSelector(char selection)
{
	if ((selection < '0') || (selection > '4'))
	{
		println_log(F("colorSelector():  Error, invalid filament selection"));
		return;
	}
loop:
	if (isFilamentLoadedPinda())
	{
		fixTheProblem("colorSelector(): Error, filament is present between the MMU2 and the MK3 Extruder:  UNLOAD FILAMENT!!");
		goto loop;
	}

	switch (selection)
	{
	case '0': 
	    // position '0' is always just a move to the left
		// the '+CS_RIGHT_FORCE_SELECTOR_0' is an attempt to move the selector ALL the way left (puts the selector into known position)
		csTurnAmount(currentPosition + CS_RIGHT_FORCE_SELECTOR_0, CCW); 
		// Apply CSOFFSET
		csTurnAmount((selectorAbsPos[0]), CW);
		currentPosition = selectorAbsPos[0];
		break;
	case '1':
		if (currentPosition <= selectorAbsPos[1])
		{
			csTurnAmount((selectorAbsPos[1] - currentPosition), CW);
		}
		else
		{
			csTurnAmount((currentPosition - selectorAbsPos[1]), CCW);
		}
		currentPosition = selectorAbsPos[1];
		break;
	case '2':
		if (currentPosition <= selectorAbsPos[2])
		{
			csTurnAmount((selectorAbsPos[2] - currentPosition), CW);
		}
		else
		{
			csTurnAmount((currentPosition - selectorAbsPos[2]), CCW);
		}
		currentPosition = selectorAbsPos[2];
		break;
	case '3':
		if (currentPosition <= selectorAbsPos[3])
		{
			csTurnAmount((selectorAbsPos[3] - currentPosition), CW);
		}
		else
		{
			csTurnAmount((currentPosition - selectorAbsPos[3]), CCW);
		}
		currentPosition = selectorAbsPos[3];
		break;
	case '4':
		if (currentPosition <= selectorAbsPos[4])
		{
			csTurnAmount((selectorAbsPos[4] - currentPosition), CW);
		}
		else
		{
			csTurnAmount((currentPosition - selectorAbsPos[4]), CCW);
		}
		currentPosition = selectorAbsPos[4];
		break;
	}

} // end of colorSelector routine()

/*****************************************************
 *
 * this routine is the common routine called for fixing the filament issues (loading or unloading)
 *
 *****************************************************/
void fixTheProblem(String statement)
{
	println_log(F(""));
	println_log(F("********************* ERROR ************************"));
	println_log(statement); // report the error to the user
	println_log(F("********************* ERROR ************************"));
	println_log(F("Clear the problem and then hit any key to continue "));
	println_log(F(""));

	parkIdler();								   // park the idler stepper motor
	digitalWrite(colorSelectorEnablePin, DISABLE); // turn off the selector stepper motor

#ifdef SERIAL_DEBUG
	while (!Serial.available())
	{
		//  wait until key is entered to proceed  (this is to allow for operator intervention)
	}
	Serial.readString(); // clear the keyboard buffer
#endif

	unParkIdler();								  // put the idler stepper motor back to its' original position
	digitalWrite(colorSelectorEnablePin, ENABLE); // turn ON the selector stepper motor
	delay(1);									  // wait for 1 millisecond
}

/*****************************************************
 *
 * this is the selector motor with the lead screw (final stage of the MMU2 unit)
 * 
 *****************************************************/
void csTurnAmount(int steps, int direction)
{

	digitalWrite(colorSelectorEnablePin, ENABLE); // turn on the color selector motor
	if (direction == CW)
		digitalWrite(colorSelectorDirPin, LOW); // set the direction for the Color Extruder Stepper Motor
	else
		digitalWrite(colorSelectorDirPin, HIGH);
    // FIXME ??? NEEDED ???
	// wait 1 milliseconds
	delayMicroseconds(1500); // changed from 500 to 1000 microseconds on 10.6.18, changed to 1500 on 10.7.18)

#ifdef DEBUG
	int scount;
	print_log(F("raw steps: "));
	println_log(steps);

	scount = steps * STEPSIZE;
	print_log(F("total number of steps: "));
	println_log(scount);
#endif

	for (uint16_t i = 0; i <= (steps * STEPSIZE); i++)
	{ 
		digitalWrite(colorSelectorStepPin, HIGH);
		delayMicroseconds(PINHIGH); // delay for 10 useconds
		digitalWrite(colorSelectorStepPin, LOW);
		delayMicroseconds(PINLOW);					// delay for 10 useconds 
		delayMicroseconds(COLORSELECTORMOTORDELAY); // wait for 60 useconds
		//add enstop
		if ((digitalRead(colorSelectorEnstop) == LOW) && (direction == CW))
			break;
	}

#ifdef TURNOFFSELECTORMOTOR						   
	digitalWrite(colorSelectorEnablePin, DISABLE); // turn off the color selector motor
#endif
}


/*****************************************************
 *
 * turn the idler stepper motor
 * 
 *****************************************************/
void idlerturnamount(int steps, int dir)
{
	digitalWrite(idlerEnablePin, ENABLE); // turn on motor
	digitalWrite(idlerDirPin, dir);
	delay(1); // wait for 1 millisecond

	// these command actually move the IDLER stepper motor
	for (uint16_t i = 0; i < steps * STEPSIZE; i++)
	{
		digitalWrite(idlerStepPin, HIGH);
		delayMicroseconds(PINHIGH); // delay for 10 useconds
		digitalWrite(idlerStepPin, LOW);
		//delayMicroseconds(PINLOW);               // delay for 10 useconds 
		delayMicroseconds(IDLERMOTORDELAY);
	}
} // end of idlerturnamount() routine

/*****************************************************
 *
 * Load the Filament using the FINDA and go back to MMU
 * 
 *****************************************************/
void loadFilamentToFinda()
{
	unsigned long startTime, currentTime;

	digitalWrite(extruderEnablePin, ENABLE); 
	digitalWrite(extruderDirPin, CCW);		 // set the direction of the MMU2 extruder motor
	delay(1);

	startTime = millis();

loop:
	currentTime = millis();
	if ((currentTime - startTime) > 10000)
	{   // 10 seconds worth of trying to load the filament
		fixTheProblem("UNLOAD FILAMENT ERROR:   timeout error, filament is not loaded to the FINDA sensor");
		startTime = millis(); // reset the start time clock
	}

    // go 144 steps (1 mm) and then check the finda status
	feedFilament(STEPSPERMM, STOP_AT_EXTRUDER);

    // keep feeding the filament until the pinda sensor triggers
	if (!isFilamentLoadedPinda())
		goto loop;
	//
	// for a filament load ... need to get the filament out of the selector head !!
	//
	digitalWrite(extruderDirPin, CW); // back the filament away from the selector
	// after hitting the FINDA sensor, back away by UNLOAD_LENGTH_BACK_COLORSELECTOR mm
	feedFilament(STEPSPERMM * UNLOAD_LENGTH_BACK_COLORSELECTOR, IGNORE_STOP_AT_EXTRUDER); 
}

/*****************************************************
 *
 * unload Filament using the FINDA sensor and push it in the MMU
 * 
 *****************************************************/
void unloadFilamentToFinda()
{
	unsigned long startTime, currentTime, startTime1;
	// if the filament is already unloaded, do nothing
	if (!isFilamentLoadedPinda())
	{ 
		println_log(F("unloadFilamentToFinda():  filament already unloaded"));
		return;
	}

	digitalWrite(extruderEnablePin, ENABLE); // turn on the extruder motor
	digitalWrite(extruderDirPin, CW);		 // set the direction of the MMU2 extruder motor
	delay(1);

	startTime = millis();
	startTime1 = millis();

loop:

	currentTime = millis();

    // read the filament switch (on the top of the mk3 extruder)
	if (isFilamentLoadedtoExtruder())
	{ 
		// filament Switch is still ON, check for timeout condition
		if ((currentTime - startTime1) > 2000)
		{ // has 2 seconds gone by ?
			fixTheProblem("UNLOAD FILAMENT ERROR: filament not unloading properly, stuck in mk3 head");
			startTime1 = millis();
		}
	}else{ 
		// check for timeout waiting for FINDA sensor to trigger
		if ((currentTime - startTime) > TIMEOUT_LOAD_UNLOAD)
		{ 
			// 10 seconds worth of trying to unload the filament
			fixTheProblem("UNLOAD FILAMENT ERROR: filament is not unloading properly, stuck between mk3 and mmu2");
			startTime = millis(); // reset the start time
		}
	}

	feedFilament(STEPSPERMM, IGNORE_STOP_AT_EXTRUDER); // 1mm and then check the pinda status

	// keep unloading until we hit the FINDA sensor
	if (isFilamentLoadedPinda())
	{ 
		goto loop;
	}

    // back the filament away from the selector by UNLOAD_LENGTH_BACK_COLORSELECTOR mm
	digitalWrite(extruderDirPin, CW); 
	feedFilament(STEPSPERMM * UNLOAD_LENGTH_BACK_COLORSELECTOR, IGNORE_STOP_AT_EXTRUDER);
}

/*****************************************************
 *
 * this routine feeds filament by the amount of steps provided
 * stoptoextruder when mk3 switch detect it (only if switch is before mk3 gear)
 * 144 steps = 1mm of filament (using the current mk8 gears in the MMU2)
 *
 *****************************************************/
void feedFilament(unsigned int steps, int stoptoextruder)
{
	for (unsigned int i = 0; i <= steps; i++)
	{
		digitalWrite(extruderStepPin, HIGH);
		delayMicroseconds(PINHIGH); // delay for 10 useconds
		digitalWrite(extruderStepPin, LOW);
		delayMicroseconds(PINLOW); // delay for 10 useconds

		delayMicroseconds(EXTRUDERMOTORDELAY); // wait for 400 useconds
		//delay(delayValue);           // wait for 30 milliseconds
		if ((stoptoextruder) && isFilamentLoadedtoExtruder())
			break;
	}
}

/*****************************************************
 *
 * Home the idler
 * perform this function only at power up/reset
 *
 *****************************************************/
void initIdlerPosition()
{

	digitalWrite(idlerEnablePin, ENABLE); // turn on the roller bearing motor
	delay(1);
	oldBearingPosition = 125; // points to position #1
	idlerturnamount(MAXROLLERTRAVEL, CW);
	idlerturnamount(MAXROLLERTRAVEL, CCW); // move the bearings out of the way
	digitalWrite(idlerEnablePin, DISABLE); // turn off the idler roller bearing motor

	filamentSelection = 0; // keep track of filament selection (0,1,2,3,4))
	currentExtruder = '0';
}

/*****************************************************
 *
 * this routine drives the 5 position bearings (aka idler, on the top of the MMU2 carriage)
 * filament 0..4 -> the position
 *
 *****************************************************/
void idlerSelector(char filament)
{
	int newBearingPosition;
	int newSetting;

#ifdef DEBUG
	print_log(F("idlerSelector(): Filament Selected: "));
	println_log(filament);
#endif

	digitalWrite(extruderEnablePin, ENABLE);
	if ((filament < '0') || (filament > '4'))
	{
		println_log(F("idlerSelector() ERROR, invalid filament selection"));
		print_log(F("idlerSelector() filament: "));
		println_log(filament);
		return;
	}

#ifdef DEBUG
	print_log(F("Old Idler Roller Bearing Position:"));
	println_log(oldBearingPosition);
	println_log(F("Moving filament selector"));
#endif

	switch (filament)
	{
	case '0':
		newBearingPosition = bearingAbsPos[0]; // idler set to 1st position
		filamentSelection = 0;
		currentExtruder = '0';
		break;
	case '1':
		newBearingPosition = bearingAbsPos[1];
		filamentSelection = 1;
		currentExtruder = '1';
		break;
	case '2':
		newBearingPosition = bearingAbsPos[2];
		filamentSelection = 2;
		currentExtruder = '2';
		break;
	case '3':
		newBearingPosition = bearingAbsPos[3];
		filamentSelection = 3;
		currentExtruder = '3';
		break;
	case '4':
		newBearingPosition = bearingAbsPos[4];
		filamentSelection = 4;
		currentExtruder = '4';
		break;
	default:
		println_log(F("idlerSelector(): ERROR, Invalid Idler Bearing Position"));
		break;
	}

	newSetting = newBearingPosition - oldBearingPosition;
	if (newSetting < 0)
		idlerturnamount(-newSetting, CW); // turn idler to appropriate position
	else
		idlerturnamount(newSetting, CCW); // turn idler to appropriate position
	oldBearingPosition = newBearingPosition;
}


/*****************************************************
 *
 * Home the Color Selector
 * perform this function only at power up/reset
 * 
 *****************************************************/
void initColorSelector()
{

	digitalWrite(colorSelectorEnablePin, ENABLE); // turn on the stepper motor
	delay(1);									  // wait for 1 millisecond
	csTurnAmount(MAXSELECTOR_STEPS, CW);				   // move to the right
	csTurnAmount(MAXSELECTOR_STEPS + CS_RIGHT_FORCE, CCW); // move all the way to the left
	digitalWrite(colorSelectorEnablePin, DISABLE); // turn off the stepper motor
}

/*****************************************************
 *
 * Re-Sync Color Selector
 * this function is performed by the 'T' command after so many moves to make sure the colorselector is synchronized
 *
 *****************************************************/
void syncColorSelector()
{
	int moveSteps;

	digitalWrite(colorSelectorEnablePin, ENABLE); // turn on the selector stepper motor
	delay(1);									  // wait for 1 millecond

	print_log(F("syncColorSelelector()   current Filament selection: "));
	println_log(filamentSelection);

	moveSteps = MAXSELECTOR_STEPS - selectorAbsPos[filamentSelection];

	print_log(F("syncColorSelector()   moveSteps: "));
	println_log(moveSteps);

	csTurnAmount(moveSteps, CW);			   // move all the way to the right
	csTurnAmount(MAXSELECTOR_STEPS + CS_RIGHT_FORCE, CCW); // move all the way to the left
	//FIXME : turn of motor ???
	//digitalWrite(colorSelectorEnablePin, DISABLE); // turn off the stepper motor
}

/*****************************************************
 *
 * move the filament Roller pulleys away from the filament
 *
 *****************************************************/
void parkIdler()
{
	int newSetting;

	digitalWrite(idlerEnablePin, ENABLE);
	delay(1);

	newSetting = MAXROLLERTRAVEL - oldBearingPosition;
	oldBearingPosition = MAXROLLERTRAVEL; // record the current roller status  (CSK)

	idlerturnamount(newSetting, CCW);	 // move the bearing roller out of the way
	idlerStatus = INACTIVE;

	digitalWrite(idlerEnablePin, DISABLE); // turn off the roller bearing stepper motor  (nice to do, cuts down on CURRENT utilization)
	digitalWrite(extruderEnablePin, DISABLE); // turn off the extruder stepper motor as well
}


/*****************************************************
 *
 *
 *****************************************************/
void unParkIdler()
{
	int rollerSetting;

	digitalWrite(idlerEnablePin, ENABLE); // turn on (enable) the roller bearing motor
	delay(1); // wait for 10 useconds

	rollerSetting = MAXROLLERTRAVEL - bearingAbsPos[filamentSelection];
	oldBearingPosition = bearingAbsPos[filamentSelection]; // update the idler bearing position

	idlerturnamount(rollerSetting, CW); // restore the old position
	idlerStatus = ACTIVE;				// mark the idler as active

	digitalWrite(extruderEnablePin, ENABLE); // turn on (enable) the extruder stepper motor as well
}


/*****************************************************
 *
 * attempt to disengage the idler bearing after a 'T' command instead of parking the idler
 * this is trying to save significant time on re-engaging the idler when the 'C' command is activated
 *
 *****************************************************/
void quickParkIdler()
{

	digitalWrite(idlerEnablePin, ENABLE); // turn on the idler stepper motor
	delay(1);

	idlerturnamount(IDLERSTEPSIZE, CCW);

	oldBearingPosition = oldBearingPosition + IDLERSTEPSIZE; // record the current position of the IDLER bearing
	idlerStatus = QUICKPARKED; // use this new state to show the idler is pending the 'C0' command

    //FIXME : Turn off idler ?
	//digitalWrite(idlerEnablePin, DISABLE);    // turn off the roller bearing stepper motor  (nice to do, cuts down on CURRENT utilization)
	digitalWrite(extruderEnablePin, DISABLE); // turn off the extruder stepper motor as well
}

/*****************************************************
 *
 * this routine is called by the 'C' command to re-engage the idler bearing
 *
 * FIXME: needed ?
 *****************************************************/
void quickUnParkIdler()
{
	int rollerSetting;

	rollerSetting = oldBearingPosition - IDLERSTEPSIZE; // go back IDLERSTEPSIZE units (hopefully re-enages the bearing
	
	idlerturnamount(IDLERSTEPSIZE, CW);					// restore old position

	print_log(F("quickunparkidler(): oldBearingPosition"));
	println_log(oldBearingPosition);

	oldBearingPosition = rollerSetting - IDLERSTEPSIZE; // keep track of the idler position

	idlerStatus = ACTIVE; // mark the idler as active
}


/*****************************************************
 *
 * called by 'C' command to park the idler
 *
 * FIXME: needed ?
 *****************************************************/
void specialParkIdler()
{
	int idlerSteps;

	digitalWrite(idlerEnablePin, ENABLE); // turn on the idler stepper motor
	delay(1);

	//*************************************************************************************************
	//*  this is a new approach to moving the idler just a little bit (off the filament)
	//*  in preparation for the 'C' Command
	//*************************************************************************************************
	if (IDLERSTEPSIZE % 2)
		idlerSteps = IDLERSTEPSIZE / 2 + 1; // odd number processing, need to round up
	else
		idlerSteps = IDLERSTEPSIZE / 2;

	idlerturnamount(idlerSteps, CCW);

	//************************************************************************************************
	//* record the idler position  (get back to where we were)
	//***********************************************************************************************
	oldBearingPosition = oldBearingPosition + idlerSteps; // record the current position of the IDLER bearingT
	idlerStatus = QUICKPARKED; // use this new state to show the idler is pending the 'C0' command

	//FIXME : stop the idler ????
	// digitalWrite(idlerEnablePin, DISABLE);    // turn off the roller bearing stepper motor  (nice to do, cuts down on CURRENT utilization)
}

/*****************************************************
 *
 * this routine is called by the 'C' command to re-engage the idler bearing
 *
 * FIXME: needed ?
 *****************************************************/
void specialUnParkIdler()
{
	int idlerSteps;

	// re-enage the idler bearing that was only moved 1 position (for quicker re-engagement)
	//
	if (IDLERSTEPSIZE % 2)
		idlerSteps = IDLERSTEPSIZE / 2 + 1; // odd number processing, need to round up
	else
		idlerSteps = IDLERSTEPSIZE / 2;

	idlerturnamount(idlerSteps, CW); // restore old position

	// MIGHT BE A BAD IDEA
	oldBearingPosition = oldBearingPosition - idlerSteps; // keep track of the idler position

	idlerStatus = ACTIVE; // mark the idler as active
}

/*****************************************************
 *
 *
 *****************************************************/
void deActivateColorSelector()
{
//FIXME : activate it by default
#ifdef TURNOFFSELECTORMOTOR
	digitalWrite(colorSelectorEnablePin, DISABLE); // turn off the color selector stepper motor  (nice to do, cuts down on CURRENT utilization)
	delay(1);
	colorSelectorStatus = INACTIVE;
#endif
}

/*****************************************************
 *
 *
 *****************************************************/
void activateColorSelector()
{
	digitalWrite(colorSelectorEnablePin, ENABLE);
	delay(1);
	colorSelectorStatus = ACTIVE;
}

/*****************************************************
 *
 * this routine is executed as part of the 'T' Command (Load Filament)
 *
 *****************************************************/
void filamentLoadToMK3()
{
	int flag;
	int filamentDistance;
	int fStatus;
	int startTime, currentTime;

	if ((currentExtruder < '0') || (currentExtruder > '4'))
	{
		println_log(F("filamentLoadToMK3(): fixing current extruder variable"));
		currentExtruder = '0';
	}
#ifdef DEBUG
	println_log(F("Attempting to move Filament to Print Head Extruder Bondtech Gears"));
	//unParkIdler();
	print_log(F("filamentLoadToMK3():  currentExtruder: "));
	println_log(currentExtruder);
#endif

	// idlerSelector(currentExtruder);        // active the idler before the filament load

	deActivateColorSelector();

	digitalWrite(extruderEnablePin, ENABLE); // turn on the extruder stepper motor (10.14.18)
	digitalWrite(extruderDirPin, CCW);		 // set extruder stepper motor to push filament towards the mk3
	delay(1);								 // wait 1 millisecond

	startTime = millis();

loop:
	// feedFilament(1);        // 1 step and then check the pinda status
	feedFilament(STEPSPERMM, IGNORE_STOP_AT_EXTRUDER); // feed 1 mm of filament into the bowden tube

	currentTime = millis();

	// added this timeout feature on 10.4.18 (2 second timeout)
	if ((currentTime - startTime) > 2000)
	{
		fixTheProblem("FILAMENT LOAD ERROR:  Filament not detected by FINDA sensor, check the selector head in the MMU2");

		startTime = millis();
	}
	if (!isFilamentLoadedPinda()) // keep feeding the filament until the pinda sensor triggers
		goto loop;
	//***************************************************************************************************
	//* added additional check (10.10.18) - if the filament switch is already set this might mean there is a switch error or a clog
	//*       this error condition can result in 'air printing'
	//***************************************************************************************************************************
loop1:
	if (isFilamentLoadedtoExtruder())
	{ // switch is active (this is not a good condition)
		fixTheProblem("FILAMENT LOAD ERROR: Filament Switch in the MK3 is active (see the RED LED), it is either stuck open or there is debris");
		goto loop1;
	}

	feedFilament(STEPSPERMM * DIST_MMU_EXTRUDER, STOP_AT_EXTRUDER); // go DIST_MMU_EXTRUDER mm then look for the 2nd filament sensor
	filamentDistance = DIST_MMU_EXTRUDER;

#ifdef FILAMENTSWITCH_BEFORE_EXTRUDER
	startTime = millis();
	flag = 0;
	//filamentDistance = 0;

	// wait until the filament sensor on the mk3 extruder head (microswitch) triggers
	while (flag == 0)
	{

		currentTime = millis();
		if ((currentTime - startTime) > TIMEOUT_LOAD_UNLOAD)
		{ // only wait for 8 seconds
			fixTheProblem("FILAMENT LOAD ERROR: Filament not detected by the MK3 filament sensor, check the bowden tube for clogging/binding");
			startTime = millis(); // reset the start Time
		}
		feedFilament(STEPSPERMM, STOP_AT_EXTRUDER); // step forward 1 mm
		filamentDistance++;
		// read the filament switch on the mk3 extruder
		if (isFilamentLoadedtoExtruder())
		{
			// println_log(F("filament switch triggered"));
			flag = 1;

			print_log(F("Filament distance traveled (mm): "));
			println_log(filamentDistance);
		}
	}

	// feed filament an additional DIST_EXTRUDER_BTGEAR mm to hit the middle of the bondtech gear
	// go an additional 3DIST_EXTRUDER_BTGEAR2mm (increased to DIST_EXTRUDER_BTGEAR on 10.4.18)
	feedFilament(STEPSPERMM * DIST_EXTRUDER_BTGEAR, IGNORE_STOP_AT_EXTRUDER);
#endif
}

/*****************************************************
 *
 *
 *****************************************************/
int isFilamentLoadedPinda()
{
	int findaStatus;

	findaStatus = digitalRead(findaPin);
	return (findaStatus);
}

/*****************************************************
 *
 *
 *****************************************************/
bool isFilamentLoadedtoExtruder()
{
	int fStatus;

	fStatus = digitalRead(filamentSwitch);
	return (fStatus == filamentSwitchON);
}

//
// (T) Tool Change Command - this command is the core command used my the mk3 to drive the mmu2 filament selection
//
/*****************************************************
 *
 *
 *****************************************************/
void toolChange(char selection)
{
	int newExtruder;

	++toolChangeCount; // count the number of tool changes
	++trackToolChanges;

	//**********************************************************************************
	// * 10.10.18 added an automatic reset of the tracktoolchange counter since going to
	//            filament position '0' move the color selection ALL the way to the left
	//*********************************************************************************
	if (selection == '0')
	{
		// println_log(F("toolChange()  filament '0' selected: resetting tracktoolchanges counter"));
		trackToolChanges = 0;
	}

	print_log(F("Tool Change Count: "));
	println_log(toolChangeCount);

	newExtruder = selection - 0x30; // convert ASCII to a number (0-4)

	//***********************************************************************************************
	// code snippet added on 10.8.18 to help the 'C' command processing (happens after 'T' command
	//***********************************************************************************************
	if (newExtruder == filamentSelection)
	{ // already at the correct filament selection

		if (!isFilamentLoadedPinda())
		{ // no filament loaded

			println_log(F("toolChange: filament not currently loaded, loading ..."));

			idlerSelector(selection); // move the filament selector stepper motor to the right spot
			colorSelector(selection); // move the color Selector stepper Motor to the right spot
			filamentLoadToMK3();
			quickParkIdler(); // command moved here on 10.13.18
			//****************************************************************************************
			//*  added on 10.8.18 to help the 'C' command
			//***************************************************************************************
			repeatTCmdFlag = INACTIVE; // used to help the 'C' command
									   //loadFilamentToFinda();
		}
		// #ifndef FILAMENTSWITCH_BEFORE_EXTRUDER
		// 		else if (!isFilamentLoadedtoExtruder())
		// 		{
		// 			//  filament loaded in pinda but not mk3

		// 			println_log(F("toolChange: filament not currently loaded to mk3, loading ..."));

		// 			filamentLoadToMK3();
		// 			quickParkIdler(); // command moved here on 10.13.18
		// 			//****************************************************************************************
		// 			//*  added on 10.8.18 to help the 'C' command
		// 			//***************************************************************************************
		// 			repeatTCmdFlag = INACTIVE; // used to help the 'C' command
		// 									   //loadFilamentToFinda();
		// 		}
		// #endif
		else
		{
			println_log(F("toolChange:  filament already loaded to mk3 extruder"));
			//*********************************************************************************************
			//* added on 10.8.18 to help the 'C' Command
			//*********************************************************************************************
			repeatTCmdFlag = ACTIVE; // used to help the 'C' command to not feed the filament again
		}

		//                               else {                           // added on 9.24.18 to
		//                                     println_log(F("Filament already loaded, unloading the filament"));
		//                                     idlerSelector(selection);
		//                                     unloadFilamentToFinda();
		//                               }
	}
	else
	{ // different filament position
		//********************************************************************************************
		//* added on 19.8.18 to help the 'C' Command
		//************************************************************************************************
		repeatTCmdFlag = INACTIVE; // turn off the repeat Commmand Flag (used by 'C' Command)
		if (isFilamentLoadedPinda())
		{
			//**************************************************************
			// added on 10.5.18 to get the idler into the correct state
			// idlerSelector(currentExtruder);
			//**************************************************************
#ifdef DEBUG
			println_log(F("Unloading filament"));
#endif

			idlerSelector(currentExtruder); // point to the current extruder

			unloadFilamentToFinda(); // have to unload the filament first
		}

		if (trackToolChanges > TOOLSYNC)
		{ // reset the color selector stepper motor (gets out of alignment)
			println_log(F("Synchronizing the Filament Selector Head"));
			//*******************************
			// NOW HAVE A MORE ELEGANT APPROACH - syncColorSelector (and it works)
			// *******************************
			syncColorSelector();
			//initColorSelector();              // reset the color selector

			activateColorSelector(); // turn the color selector motor back on
			currentPosition = 0;	 // reset the color selector

			// colorSelector('0');                       // move selector head to position 0

			trackToolChanges = 0;
		}
#ifdef DEBUG
		println_log(F("Selecting the proper Idler Location"));
#endif
		idlerSelector(selection);
#ifdef DEBUG
		println_log(F("Selecting the proper Selector Location"));
#endif
		colorSelector(selection);
#ifdef DEBUG
		println_log(F("Loading Filament: loading the new filament to the mk3"));
#endif

		filamentLoadToMK3(); // moves the idler and loads the filament

		filamentSelection = newExtruder;
		currentExtruder = selection;
		quickParkIdler(); // command moved here on 10.13.18
	}

	//******************************************************************************************
	//* barely move the idler out of the way
	//* WARNING:  THIS MAY NOT WORK PROPERLY ... NEEDS TO BE DEBUGGED (10.7.18)
	//******************************************************************************************
	// quickParkIdler();                       // 10.7.2018 ... attempt to speed up idler for the follow-on 'C' command

	//******************************************************************************************
	//* this was how it was normally done until the above command was attempted
	//******************************************************************************************
	//parkIdler();                            // move the idler away

} // end of ToolChange processing

// part of the 'C' command,  does the last little bit to load into the past the extruder gear
/*****************************************************
 *
 *
 *****************************************************/
bool filamentLoadWithBondTechGear()
{
	int i;
	int delayFactor; // delay factor (in microseconds) for the filament load loop
	int stepCount;
	int tSteps;

	//*****************************************************************************************************************
	//*  added this code snippet to not process a 'C' command that is essentially a repeat command

	if (repeatTCmdFlag == ACTIVE)
	{
		println_log(F("filamentLoadWithBondTechGear(): filament already loaded and 'C' command already processed"));
		repeatTCmdFlag = INACTIVE;
		return false;
	}

	if (!isFilamentLoadedPinda())
	{
		println_log(F("filamentLoadWithBondTechGear()  Error, filament sensor thinks there is no filament"));
		return false;
	}

	if ((currentExtruder < '0') || (currentExtruder > '4'))
	{
		println_log(F("filamentLoadWithBondTechGear(): fixing current extruder variable"));
		currentExtruder = '0';
	}

	//*************************************************************************************************
	//* change of approach to speed up the IDLER engagement 10.7.18
	//*  WARNING: THIS APPROACH MAY NOT WORK ... NEEDS TO BE DEBUGGED
	//*  C command assumes there is always a T command right before it
	//*  (IF 2 'C' commands are issued by the MK3 in a row the code below might be an issue)
	//*
	//*************************************************************************************************

	if (idlerStatus == QUICKPARKED)
	{ // make sure idler is  in the pending state (set by quickparkidler() routine)
		// println_log(F("'C' Command: quickUnParking the Idler"));
		// quickUnParkIdler();
		specialUnParkIdler(); // PLACEHOLDER attempt to speed up the idler engagement a little more 10.13.18
	}
	if (idlerStatus == INACTIVE)
	{
		unParkIdler();
	}

	//*************************************************************************************************
	//* following line of code is currently disabled (in order to test out the code above
	//*  NOTE: I don't understand why the unParkIdler() command is not used instead ???
	//************************************************************************************************
	// idlerSelector(currentExtruder);        // move the idler back into position

	stepCount = 0;
	digitalWrite(greenLED, HIGH); // turn on the green LED (for debug purposes)
	//*******************************************************************************************
	// feed the filament from the MMU2 into the bondtech gear for 2 seconds at 10 mm/sec
	// STEPPERMM : 144, 1: duration in seconds,  21: feed rate (in mm/sec)
	// delay: 674 (for 10 mm/sec)
	// delay: 350 (for 21 mm/sec)
	// LOAD_DURATION:  1 second (time to spend with the mmu2 extruder active)
	// LOAD_SPEED: 21 mm/sec  (determined by Slic3r settings
	// INSTRUCTION_DELAY:  25 useconds  (time to do the instructions in the loop below, excluding the delayFactor)
	// #define LOAD_DURATION 1000       (load duration in milliseconds, currently set to 1 second)
	// #define LOAD_SPEED 21    // load speed (in mm/sec) during the 'C' command (determined by Slic3r setting)
	// #define INSTRUCTION_DELAY 25  // delay (in microseconds) of the loop

	// *******************************************************************************************
	// compute the loop delay factor (eventually this will replace the '350' entry in the loop)
	//       this computed value is in microseconds of time
	//********************************************************************************************
	// delayFactor = ((LOAD_DURATION * 1000.0) / (LOAD_SPEED * STEPSPERMM)) - INSTRUCTION_DELAY;   // compute the delay factor (in microseconds)

	// for (i = 0; i < (STEPSPERMM * 1 * 21); i++) {

	tSteps = STEPSPERMM * ((float)LOAD_DURATION / 1000.0) * LOAD_SPEED;			// compute the number of steps to take for the given load duration
	delayFactor = (float(LOAD_DURATION * 1000.0) / tSteps) - INSTRUCTION_DELAY; // 2nd attempt at delayFactor algorithm

	digitalWrite(extruderEnablePin, ENABLE); // turn on the extruder stepper motor (10.14.18)
	digitalWrite(extruderDirPin, CCW);		 // set extruder stepper motor to push filament towards the mk3

	for (i = 0; i < tSteps; i++)
	{
		digitalWrite(extruderStepPin, HIGH); // step the extruder stepper in the MMU2 unit
		delayMicroseconds(PINHIGH);
		digitalWrite(extruderStepPin, LOW);
		//*****************************************************************************************************
		// replace '350' with delayFactor once testing of variable is complete
		//*****************************************************************************************************
		// after further testing, the '350' can be replaced by delayFactor
		delayMicroseconds(delayFactor); // this was calculated in order to arrive at a 10mm/sec feed rate
		++stepCount;
	}
	digitalWrite(greenLED, LOW); // turn off the green LED (for debug purposes)

#ifdef DEBUG
	println_log(F("C Command: parking the idler"));
#endif
	//***************************************************************************************************************************
	//*  this disengags the idler pulley after the 'C' command has been exectuted
	//***************************************************************************************************************************
	// quickParkIdler();                           // changed to quickparkidler on 10.12.18 (speed things up a bit)

	specialParkIdler(); // PLACEHOLDER (experiment attempted on 10.13.18)

	//parkIdler();                               // turn OFF the idler rollers when filament is loaded

	//*********************************************************************************************
	//* going back to the fundamental approach with the idler
	//*********************************************************************************************
	parkIdler(); // cleanest way to deal with the idler


#ifndef FILAMENTSWITCH_BEFORE_EXTRUDER
	//Wait for MMU code in Marlin to load the filament and activate the filament switch
	delay(FILAMENT_TO_MK3_C0_WAIT_TIME);
	if (isFilamentLoadedtoExtruder())
	{
		println_log(F("filamentLoadToMK3(): Loading Filament to Print Head Complete"));
		return true;
	}
	println_log(F("FILAMENT LOAD ERROR:  Filament not detected by EXTRUDER sensor, check the EXTRUDER"));
	// Force mmu to load 5cm ???

	return false;
#endif

#ifdef DEBUG
	println_log(F("filamentLoadToMK3(): Loading Filament to Print Head Complete"));
#endif
	return true;
}

Application::Application()
{
	// nothing to do in the constructor
}
