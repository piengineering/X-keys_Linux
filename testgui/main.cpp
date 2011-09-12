/***************************************************
 X-Keys Test GUI Main Window

 The code may be used by anyone for any purpose,
 and can serve as a starting point for developing
 applications using the X-Keys libary.
 
 Alan Ott
 Signal 11 Software
 2011-08-15
***************************************************/

#include <QtGui/QApplication>
#include "MainWindow.h"

int main(int argc, char **argv)
{
	QApplication app(argc, argv);
	
	MainWindow mw;
	mw.show();
	
	return app.exec();
}
