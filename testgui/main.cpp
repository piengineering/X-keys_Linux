/****************************************
 X-Keys Test GUI Main Window
 
 Alan Ott
 Signal 11 Software
 2011-08-15
****************************************/

#include <QtGui/QApplication>
#include "MainWindow.h"

int main(int argc, char **argv)
{
	QApplication app(argc, argv);
	
	MainWindow mw;
	mw.show();
	
	return app.exec();
}
