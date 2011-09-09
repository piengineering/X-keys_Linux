/****************************************
 X-Keys Test GUI Main Window
 
 Alan Ott
 Signal 11 Software
 2011-08-15
****************************************/

#ifndef MAIN_WINDOW_H__
#define MAIN_WINDOW_H__

#include <QtGui/QDialog>
#include "ui_MainWindow.h"

#include "PieHid32.h"

class MainWindow : public QDialog, private Ui::MainWindow {
	Q_OBJECT
	
public:
        MainWindow(QWidget *parent = 0);

signals:
	void append(const QString &);
	void messageBox(const QString &title, const QString &message);
	
private slots:
	void timeout();
	void showMessageBox(const QString &title, const QString &message);
	
	// Master Controls
	void startButtonClicked(bool);
	void stopButtonClicked();
	void quitButtonClicked();
	void clearClicked();
	
	// LED controls
	void greenCheckClicked(bool);
	void redCheckClicked(bool);
	void flashGreenCheckClicked(bool);
	void flashRedCheckClicked(bool);
	void flashFrequencyChanged(const QString &);
	
	// Backlight controls
	void backlightOnOffClicked(bool);
	void backlightFlashClicked(bool);
	void backlightBank1AllClicked(bool);
	void backlightBank2AllClicked(bool);
	void backlightScrollLockClicked(bool);
	void backlightToggle();
	void backlightSetIntensity();
	void backlightSave();

	// PID controls
	void convertPID1Clicked();
	void convertPID2Clicked();
	void showDescriptorClicked();
	void generateReportClicked();
	
	// Timestamp
	void timestampOnClicked();
	void tiemstampOffClicked();
	
	// Reflector Controls
	void keyboardReflectorClicked();
	void joystickReflectorClicked();
	void mouseReflectorClicked();

private:
	long handle;
	
	void setLED(int number, int mode/*0=off,1=on,2=blink*/);
	bool checkHandle();

	static unsigned int HandleDataEvent(unsigned char *pData, unsigned int deviceID, unsigned int error);
	unsigned int handleDataEvent(unsigned char *pData, unsigned int deviceID, unsigned int error);
	
	bool keyboard; //for keyboard reflect sample
	bool joystick;
	bool mouseon;
	bool lastprogsw;
};

#endif // MAIN_WINDOW_H__

