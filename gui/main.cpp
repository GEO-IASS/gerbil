#include "viewerwindow.h"
#include <QApplication>
#include <QFileDialog>
#include <iostream>
#include <string>

int main(int argc, char **argv)
{

	// start gui
	QApplication app(argc, argv);

	// get input file name
	std::string filename;
	if (argc < 2) {
#ifdef __unix__
		std::cerr << "Usage: " << argv[0] << " <filename> [labeling file]\n\n"
					 "Filename may point to a RGB image or "
					 "a multispectral image descriptor file." << std::endl;
#endif
		filename = QFileDialog::getOpenFileName
		           	(0, "Open Descriptor or Image File").toStdString();
	} else {
		filename = argv[1];
	}

	QString labelfile;
	if (argc >= 3)
		labelfile = argv[2];

	// load image   
	multi_img* image = new multi_img(filename);
	if (image->empty())
		return 2;
	
	// regular viewer
	ViewerWindow window(image);
	window.show();
	
	// load labels
	if (!labelfile.isEmpty())
		window.loadLabeling(labelfile);
	
/*	// fancy 3d viewer
	View3D window3d(image);
	window3d.show();
*/	
	return app.exec();
}

