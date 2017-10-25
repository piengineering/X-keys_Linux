/****************************************
 X-Keys Test GUI Main Window
 
 Alan Ott
 Signal 11 Software
 2011-08-15
****************************************/


#include <Qt>
#include <QtGui/QMessageBox>

#include "MainWindow.h"
#include <stdio.h>

MainWindow *g_main_window; // HACK

MainWindow::MainWindow(QWidget *parent)
	: QDialog(parent),
	handle(-1),
	thispid(-1),
	//joystick(false),
	mouseon(false),
	lastprogsw(false)

{
	g_main_window = this; // HACK

	setupUi(this);

	connect(start_button, SIGNAL(clicked(bool)), this, SLOT(startButtonClicked(bool)));
	connect(stop_button, SIGNAL(clicked()), this, SLOT(stopButtonClicked()));
	connect(quit_button, SIGNAL(clicked()), this, SLOT(quitButtonClicked()));
	connect(clear_button, SIGNAL(clicked()), this, SLOT(clearClicked()));

	// LED controls
	connect(green_check, SIGNAL(clicked(bool)), this, SLOT(greenCheckClicked(bool)));
	connect(red_check, SIGNAL(clicked(bool)), this, SLOT(redCheckClicked(bool)));
	connect(flash_green_check, SIGNAL(clicked(bool)), this, SLOT(flashGreenCheckClicked(bool)));
	connect(flash_red_check, SIGNAL(clicked(bool)), this, SLOT(flashRedCheckClicked(bool)));
	connect(flash_freq, SIGNAL(textChanged(const QString &)), this, SLOT(flashFrequencyChanged(const QString &)));
	connect(unit_id, SIGNAL(clicked()), this, SLOT(unitIDClicked()));
	connect(individual_led, SIGNAL(clicked()), this, SLOT(individualLEDClicked()));

	// PID controls
	connect(convert_pid1, SIGNAL(clicked()), this, SLOT(convertPID1Clicked()));
	connect(show_descriptor, SIGNAL(clicked()), this, SLOT(showDescriptorClicked()));
	connect(generate_report, SIGNAL(clicked()), this, SLOT(generateReportClicked()));
	
	// Timestamp
	connect(timestamp_on, SIGNAL(clicked()), this, SLOT(timestampOnClicked()));
	connect(timestamp_off, SIGNAL(clicked()), this, SLOT(tiemstampOffClicked()));

	// Reflector Controls
	connect(keyboard_reflector, SIGNAL(clicked()), this, SLOT(keyboardReflectorClicked()));
	connect(joystick_reflector, SIGNAL(clicked()), this, SLOT(joystickReflectorClicked()));
	connect(mouse_reflector, SIGNAL(clicked()), this, SLOT(mouseReflectorClicked()));
	
	// Connect our append() signal to the output box's append() slot.
	// This is so we can append to the output box from the callback
	// thread.
	connect(this, SIGNAL(append(const QString &)), output_box, SLOT(append(const QString &)));

	connect(this, SIGNAL(messageBox(const QString &, const QString &)),
	        this, SLOT(showMessageBox(const QString &, const QString&)));

}

void
MainWindow::startButtonClicked(bool)
{
	TEnumHIDInfo info[128];
	long count;
	int i;
	
	unsigned res = EnumeratePIE(PI_VID, info, &count);
	
	if (res != 0) {
		output_box->append("Error Finding PI Engineering Devices.\n");
		return;
	}
	if (count == 0) {
		output_box->append("No PI Engineering Devices Found.\n");
		return;
	}
	
	handle = -1;
	
	/* Open the first X-Keys Device found. */
	for (i = 0; i < count; i++) {
		TEnumHIDInfo *dev = &info[i];
		printf("Found XKeys Device:\n");
		printf("\tPID: %04x\n", dev->PID);
		printf("\tUsage Page: %04x\n", dev->UP);
		printf("\tUsage:      %04x\n", dev->Usage);
		printf("\tVersion: %d\n\n", dev->Version);

		//if (dev->UP == 0x000c && dev->Usage == 0x0001) {
		if (dev->UP == 0x000c && dev->writeSize > 0) {
			/* This is the splat interface, */

			handle = dev->Handle;
			thispid=dev->PID;
			
			unsigned int res = SetupInterfaceEx(handle);
			if (res != 0) {
				output_box->append("Error Setting up PI Engineering Device");
			}
			else {
				output_box->append(dev->ProductString + QString(", PID:") + QString::number(dev->PID));
				
				/* Set up the Data Callback. */
				unsigned int result;
				result = SetDataCallback(handle, HandleDataEvent);
				if (result != 0) {
					QMessageBox::critical(this, "Error", "Error setting event callback");
				}

				SuppressDuplicateReports(handle, false); //if dealing with SE or MWII firmware may want this to be true
				break;
			}
		}
	}
	
	if (handle < 0) {
		output_box->append("Could not open any of the PI Engineering devices connected.");
	}
}

void
MainWindow::stopButtonClicked()
{
	if (handle >= 0) {
		CloseInterface(handle);




		output_box->append("Disconnected from device");	
	}
	handle = -1;
}

void
MainWindow::quitButtonClicked()
{
	stopButtonClicked();
	qApp->quit();
}

void
MainWindow::clearClicked()
{
	output_box->clear();
}

void
MainWindow::unitIDClicked()
{
	if (!checkHandle())
		return;
	//Set the unit ID

	bool ok;
	int unit_id = unitid_number->text().toInt(&ok, 10);

	unsigned int result;
	unsigned char buffer[80];
	memset(buffer, 0, sizeof(buffer));
	buffer[1]= 189;
	buffer[2]= unit_id;
	result = WriteData(handle, buffer);

	if (result != 0) {
			QMessageBox::critical(this, "Write Error", "Unable to write to Device.");
	}
}

void
MainWindow::greenCheckClicked(bool state)
{
	setLED(6, (state)? 1: 0); // 0=off, 1=on, 2=flash)
}

void
MainWindow::redCheckClicked(bool state)
{
	setLED(7, (state)? 1: 0); // 0=off, 1=on, 2=flash)
}

void
MainWindow::flashGreenCheckClicked(bool state)
{
	if (state)
		setLED(6, 2);
	else {
		setLED(6, (green_check->isChecked())? 1: 0); // 0=off, 1=on, 2=flash)
	}
	
}

void
MainWindow::flashRedCheckClicked(bool state)
{
	if (state)
		setLED(7, 2);
	else {
		setLED(7, (red_check->isChecked())? 1: 0); // 0=off, 1=on, 2=flash)
	}
}

void
MainWindow::flashFrequencyChanged(const QString &str)
{
	if (!checkHandle())
		return;

	unsigned int result;
	bool ok;
	int freq = str.toInt(&ok, 10);
	
	if (ok) {
		//Set the frequency of the flashing, same one for LEDs and backlights
		unsigned char buffer[80];
		memset(buffer, 0, sizeof(buffer));
		buffer[1]= 180;
		buffer[2]= (char) freq;
		result = WriteData(handle, buffer);

		if (result != 0) {
			QMessageBox::critical(this, "Write Error", "Unable to write to Device.");
		}
	}
}

void
MainWindow::convertPID1Clicked()
{
	//applicable only to products with multiple PIDs
	if (!checkHandle())
		return;

	bool ok;
	int pid_num = pid_number->text().toInt(&ok, 10);

	unsigned int result;
	unsigned char buffer[80];
	memset(buffer, 0, sizeof(buffer));

	buffer[0]=0;
	buffer[1]=204;
	buffer[2]=pid_num; //0, 1, 2, 3, etc. see SDK documentation for endpoints available on each pid

	result = WriteData(handle, buffer);

	if (result != 0) {
		QMessageBox::critical(this, "Write Error", "Unable to write to Device.");
	}

	
}

void
MainWindow::individualLEDClicked()
{
	//applicable only to products with backlight LEDs
	if (!checkHandle())
		return;

	bool ok;
	int led_index2 = led_index->text().toInt(&ok, 10);
	int led_state2 = led_state->text().toInt(&ok, 10);

	unsigned int result;
	unsigned char buffer[80];
	memset(buffer, 0, sizeof(buffer));

	buffer[0]=0;
	buffer[1]=181;
	buffer[2]=led_index2;
	buffer[3]=led_state2;

	result = WriteData(handle, buffer);

	if (result != 0) {
		QMessageBox::critical(this, "Write Error", "Unable to write to Device.");
	}

	
}

void
MainWindow::showDescriptorClicked()
{
	unsigned int result;

	if (!checkHandle())
		return;


	// Turn off the data callback.
	DisableDataCallback(handle, true);

	unsigned char buffer[80];
	memset(buffer, 0, sizeof(buffer));

	buffer[0]=0;
	buffer[1]=214;

	result = WriteData(handle, buffer);

	if (result != 0) {
		QMessageBox::critical(this, "Write Error", "Unable to write to Device.");
	}

	// After this write the next read 3rd byte=214 will
	// give descriptor information.
	memset(buffer, 0, sizeof(buffer));
	
	result = BlockingReadData(handle, buffer, 100 /*timeout*/);
	
	int countout = 0;
	while (result == PIE_HID_READ_INSUFFICIENT_DATA ||
	       (result == 0 && buffer[2] != 214))
	{
		if (result == PIE_HID_READ_INSUFFICIENT_DATA)
		{
			// No data received after 100ms, so increment countout extra
			countout += 99;
		}
		countout++;
		if (countout > 1000) //increase this if have to check more than once
			break;
		result = BlockingReadData(handle, buffer, 100);
	}
	
	if (result ==0 && buffer[2]==214)
	{
		//clear out listbox
		output_box->clear();

		if (buffer[3] == 0)
			output_box->append("PID 1027");
		else if (buffer[3]==2)
			output_box->append("PID 1029");
		
		output_box->append(QString("Keymapstart ") + QString::number(buffer[4]));
		output_box->append(QString("Layer2offset  ") + QString::number(buffer[5]));
		output_box->append(QString("OutSize ") + QString::number(buffer[6]));
		output_box->append(QString("ReportSize ") + QString::number(buffer[7]));
		output_box->append(QString("MaxCol ") + QString::number(buffer[8]));
		output_box->append(QString("MaxRow ") + QString::number(buffer[9]));

		bool has_led = false;
		if (buffer[10] & 64) {
			output_box->append("Green LED ");
			has_led = true;
		}
		if (buffer[10] & 128) {
			output_box->append("Red LED ");
			has_led = true;
		}
		if (!has_led)
			output_box->append("None ");

		//output_box->append("\n");
		
		output_box->append(QString("Version ") + QString::number(buffer[11]));
		output_box->append(QString("PID ") + QString::number(buffer[13]*256+buffer[12]));
	}

	// Enable data callback.
	DisableDataCallback(handle, false);
}

void
MainWindow::generateReportClicked()
{
	// After sending this command a general incoming data report will be
	// given with the 3rd byte (Data Type) 2nd bit set.  If program switch
	// is up byte 3 will be 2 and if it is pressed byte 3 will be 3.  This
	// is useful for getting the initial state or unit id of the device
	// before it sends any data.

	if (!checkHandle())
		return;

	unsigned int result;
	unsigned char buffer[80];
	memset(buffer, 0, sizeof(buffer));

	buffer[0]=0;
	buffer[1]=177;

	result = WriteData(handle, buffer);

	if (result != 0) {
		QMessageBox::critical(this, "Write Error", "Unable to write to Device.");
	}
}


void
MainWindow::timestampOnClicked()
{
	// Sending this command will turn on the 4 bytes of data which
	// assembled give the time in ms from the start of the computer
	// for XK-24 bytes 8th-11th give the time stamp data with 8th
	// byte being the MSB and 11th the LSB. 

	if (!checkHandle())
		return;

	unsigned int result;
	unsigned char buffer[80];
	memset(buffer, 0, sizeof(buffer));

	buffer[0]=0;
	buffer[1]=210;
	buffer[2]=1; // 1 to turn on time stamp, 0 to turn off time stamp

	result = WriteData(handle, buffer);

	if (result != 0) {
		QMessageBox::critical(this, "Write Error", "Unable to write to Device.");
	}

}

void
MainWindow::tiemstampOffClicked()
{
	// Sending this command will turn off the 4 bytes of data which
	// assembled give the time in ms from the start of the computer
	// for XK-24 bytes 8th-11th give the time stamp data with 8th
	// byte being the MSB and 11th the LSB. 

	if (!checkHandle())
		return;

	unsigned int result;
	unsigned char buffer[80];
	memset(buffer, 0, sizeof(buffer));

	buffer[0]=0;
	buffer[1]=210;
	buffer[2]=0; // 1 to turn on time stamp, 0 to turn off time stamp

	result = WriteData(handle, buffer);

	if (result != 0) {
		QMessageBox::critical(this, "Write Error", "Unable to write to Device.");
	}
}

void
MainWindow::keyboardReflectorClicked()
{
	if (!checkHandle())
		return;

	kbd_input->setFocus();

	//Sends keyboard commands as a native keyboard
		//results: Abcd in kbd_input
		unsigned int result;
		unsigned char buffer[80];
		memset(buffer, 0, sizeof(buffer));

		buffer[0]=0;
		buffer[1]=201;
		buffer[2]=2; //modifiers Bit 1=Left Ctrl, bit 2=Left Shift, bit 3=Left Alt, bit 4=Left Gui, bit 5=Right Ctrl, bit 6=Right Shift, bit 7=Right Alt, bit 8=Right Gui.
		buffer[3]=0; //always 0
		buffer[4]=0x04; //1st hid code a, down
		buffer[5]=0; //2nd hid code
		buffer[6]=0; //3rd hid code
		buffer[7]=0; //4th hid code
		buffer[8]=0; //5th hid code
		buffer[9]=0; //6th hid code
		result = WriteData(handle, buffer);
		if (result != 0) {
			emit messageBox("Write Error", "Unable to write to Device.");
		}

		buffer[0]=0;
		buffer[1]=201;
		buffer[2]=0; //modifiers Bit 1=Left Ctrl, bit 2=Left Shift, bit 3=Left Alt, bit 4=Left Gui, bit 5=Right Ctrl, bit 6=Right Shift, bit 7=Right Alt, bit 8=Right Gui.
		buffer[3]=0; //always 0
		buffer[4]=0; //1st hid code a up
		buffer[5]=0x05; //2nd hid code b down
		buffer[6]=0x06; //3rd hid code c down
		buffer[7]=0x07; //4th hid code d down
		buffer[8]=0; //5th hid code
		buffer[9]=0; //6th hid code
		result = WriteData(handle, buffer);
		if (result != 0) {
			emit messageBox("Write Error", "Unable to write to Device.");
		}

		buffer[0]=0;
		buffer[1]=201;
		buffer[2]=0; //modifiers Bit 1=Left Ctrl, bit 2=Left Shift, bit 3=Left Alt, bit 4=Left Gui, bit 5=Right Ctrl, bit 6=Right Shift, bit 7=Right Alt, bit 8=Right Gui.
		buffer[3]=0; //always 0
		buffer[4]=0; //1st hid code 
		buffer[5]=0; //2nd hid code b up
		buffer[6]=0; //3rd hid code c up
		buffer[7]=0; //4th hid code d up
		buffer[8]=0; //5th hid code
		buffer[9]=0; //6th hid code
		result = WriteData(handle, buffer);
		if (result != 0) {
			emit messageBox("Write Error", "Unable to write to Device.");
		}	
}

void
MainWindow::joystickReflectorClicked()
{
	if (!checkHandle())
		return;

	

		// Open up the game controller control panel to test these features,
		// after clicking this button go and make active the control panel
		// properties and change will occur.
		                      
		unsigned char buttons = 15; //turn on first 4 game buttons
		
		printf("Buttons %02hhx\n", buttons);

		unsigned int result;
		unsigned char buffer[80];
		memset(buffer, 0, sizeof(buffer));
		
		buffer[0]=0;
		buffer[1]=202;
		buffer[2]=128;  // X, 0 to 127 from center to right, 255 to 128 from center to left
		buffer[3]=128;  // Y, 0 to 127 from center down, 255 to 128 from center up
		buffer[4]=128;  // Z rot, 0 to 127 from center down, 255 to 128 from center up
		buffer[5]=128;  // Z, 0 to 127 from center down, 255 to 128 from center up
		buffer[6]=128;  // Slider, 0 to 127 from center down, 255 to 128 from center up
		
		buffer[7]=buttons;  // Game buttons as bitmap, bit 1= game button 1, bit 2=game button 2, etc., buttons 1-8
		buffer[8]=0;  // Game buttons as bitmap, bit 1= game button 9, bit 2=game button 10, etc., buttons 9-16
		buffer[9]=0;  // Game buttons as bitmap, bit 1= game button 17, bit 2=game button 18, etc., buttons 17-24
		buffer[10]=0; // Game buttons as bitmap, bit 1= game button 25, bit 2=game button 26, etc., buttons 25-32
		
		buffer[11]=0;

		buffer[12]=8; // Hat, 0 to 7 clockwise, 8=no hat

		result = WriteData(handle, buffer);

		if (result != 0) {
			QMessageBox::critical(this, "Write Error", "Unable to write to Device.");
		}
	
	
}

void
MainWindow::mouseReflectorClicked()
{
	if (!checkHandle())
		return;

	// After turning mouseon=true, pressing 1st key will behave as left
	// mouse button only available in PID #1.
	if (!mouseon) {
		mouseon = true;
		mouse_label->setText("Mouse On");
	}
	else {
		mouseon = false;
		mouse_label->setText("Mouse Off");
	}
}

/*static*/
unsigned int
MainWindow::HandleDataEvent(unsigned char *pData, unsigned int deviceID, unsigned int error)
{
	g_main_window->handleDataEvent(pData, deviceID, error);
	return true;
}


unsigned int
MainWindow::handleDataEvent(unsigned char *pData, unsigned int deviceID, unsigned int error)
{
	unsigned int result;
	char dataStr[256];

	sprintf(dataStr, "%02x %02x %02x %02x -- %02x %02x %02x %02x -- %02x %02x %02x %02x -- %02x %02x %02x %02x -- %02x %02x %02x %02x -- %02x %02x %02x %02x -- %02x %02x %02x %02x -- %02x %02x %02x %02x -- %02x %02x %02x %02x", 
		pData[0], pData[1], pData[2], pData[3], pData[4], pData[5], pData[6], pData[7], pData[8], pData[9], pData[10], pData[11], pData[12], pData[13], pData[14], pData[15], pData[16], pData[17], pData[18], pData[19], pData[20], pData[21], pData[22], pData[23], pData[24], pData[25], pData[26], pData[27], pData[28], pData[29], pData[30], pData[31], pData[32], pData[33], pData[34], pData[35]);

	// Append it to the end of the output box. This is done using a signal
	// which is connected to the output box because this function is
	// called from a different thread than the GUI is on.
	emit append(dataStr);

	// The output buffer
	unsigned char buffer[80];
	memset(buffer, 0, sizeof(buffer));
	
	
	//for Mouse Reflect
	if (mouseon) {
		bool progsw = pData[3] & 1;
		if (progsw && !lastprogsw) //key down
		{
			memset(buffer, 0, sizeof(buffer));
			buffer[0]=0;
			buffer[1]=203;
			//play with these values to get various mouse functions; clicks, motion, scroll
			buffer[2]=1; //buttons 1=left, 2=right, 4=center, 8=XButton1 (browser back on my mouse), 16=XButton2
			buffer[3]=0; //X 128=0 no motion, 1-127 is right, 255-129=left, finest inc (1 and 255) to coarsest (127 and 129)
			buffer[4]=0; //Y 128=0 no motion, 1-127 is down, 255-129=up, finest inc (1 and 255) to coarsest (127 and 129)
			buffer[5]=0; //wheel X
			buffer[6]=0; //wheel Y 128=0 no motion, 1-127 is up, 255-129=down, finest inc (1 and 255) to coarsest (127 and 129)
			result = WriteData(handle, buffer);
			if (result != 0) {
				emit messageBox("Write Error", "Unable to write to Device.");
			}
		}
		else if (!progsw && lastprogsw==true) //key up
		{
			//send ups
			buffer[0]=0;
			buffer[1]=203;
			//play with these values to get various mouse functions; clicks, motion, scroll
			buffer[2]=0; //buttons 1=left, 2=right, 4=center, 8=XButton1 (browser back on my mouse), 16=XButton2
			buffer[3]=0; //X 128=0 no motion, 1-127 is right, 255-129=left, finest inc (1 and 255) to coarsest (127 and 129)
			buffer[4]=0; //Y 128=0 no motion, 1-127 is down, 255-129=up, finest inc (1 and 255) to coarsest (127 and 129)
			buffer[5]=0; //wheel X
			buffer[6]=0; //wheel Y 128=0 no motion, 1-127 is up, 255-129=down, finest inc (1 and 255) to coarsest (127 and 129)
			result = WriteData(handle, buffer);
			if (result != 0) {
				emit messageBox("Write Error", "Unable to write to Device.");
			}
		}
		lastprogsw=progsw;
	}

	
	
	// Error handling
	if (error == PIE_HID_READ_INVALID_HANDLE)
	{
		CleanupInterface(handle);
		output_box->append("Device Disconnected");
	}

	return true;
}

void
MainWindow::timeout()
{

}

void
MainWindow::showMessageBox(const QString &caption, const QString &message)
{
	QMessageBox::critical(this, caption, message);
}

void
MainWindow::setLED(int number, int mode/*0=off,1=on,2=blink*/)
{
	if (!checkHandle())
		return;

	//applicable for pi3 firmwares and later, ie PID>0x0400
	if (thispid>0x03FF)
	{
		unsigned int result;
		unsigned char buffer[80];
		memset(buffer, 0, sizeof(buffer));
		buffer[1]=179; // 0xb3
		buffer[2]=number;  // 6 for green, 7 for red
		buffer[3]= mode; // 0=off, 1=on, 2=flash
	
		result = WriteData(handle, buffer);

		if (result != 0) {
			QMessageBox::critical(this, "Write Error", "Unable to write to Device.");
		}
	}
	//obsolete MWII Products-LEDs are bitmapped, make a global variable to keep track of each LED and set or set bit
	//this sample is just turning only the red or green on or off.
	else if (thispid>0x02A4 && thispid<0x400)
	{
		unsigned int result;
		unsigned char buffer[80];
		memset(buffer, 0, sizeof(buffer));
		if (number==6)number=64;
		else if (number==7)number=128;
		if (mode==0)number=0; 
		buffer[0] = 2;
		buffer[1] = 186;
		buffer[7]=number;  // 64 for green only, 128 for red only, 0 for none, 192 (64+128) for both
	
		result = WriteData(handle, buffer);

		if (result != 0) {
			QMessageBox::critical(this, "Write Error", "Unable to write to Device.");
		}
	}
	//obsolete SE Products-LEDs are bitmapped, make a global variable to keep track of each LED and set or set bit
	//this sample is just turning only the red or green on or off.
	else if (thispid<0x02A5)
	{
		unsigned int result;
		unsigned char buffer[80];
		memset(buffer, 0, sizeof(buffer));
		if (number==6)number=64;
		else if (number==7)number=128;
		if (mode==0)number=0;
		buffer[8]=number;  // 64 for green only, 128 for red only, 0 for none, 192 (64+128) for both
	
		result = WriteData(handle, buffer);

		if (result != 0) {
			QMessageBox::critical(this, "Write Error", "Unable to write to Device.");
		}
	}
	
	
}

bool
MainWindow::checkHandle()
{
	if (handle < 0) {
		QMessageBox::critical(this, "Connection Error", "Not Connected to Device (press Start button to connect).");
		return false;
	}
	
	return true;
}
