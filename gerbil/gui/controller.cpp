#include "controller.h"
#include "dockcontroller.h"
#include <imginput.h>

#include "gerbil_gui_debug.h"

#include <QFileInfo>
#include <cstdlib> // for exit()

// for DEBUG
std::ostream &operator<<(std::ostream& os, const cv::Rect& r)
{
	os << boost::format("%1%x%2%+%3%+%4%") % r.x % r.y % r.width % r.height;
	return os;
}

Controller::Controller(const std::string &filename, bool limited_mode)
	: im(queue, limited_mode), illumm(this), fm(this, &queue), queuethread(0),
	  spectralRescaleInProgress(false)
{
	// load image
	cv::Rect dimensions = im.loadImage(filename);
	if (dimensions.width < 1) {
		exit(4); // Qt's exit does not work before calling exec();
	}

	// background task queue thread
	startQueue();

	// create gui (perform initUI before connecting signals!)
	window = new MainWindow(limited_mode);
	window->initUI(dimensions, im.getSize());

	// connect slots/signals
	window->initSignals(this);

	// initialize models
	initImage();
	initFalseColor(); // depends on ImageModel
	initIlluminant();
	initLabeling(dimensions);

#ifdef WITH_SEG_MEANSHIFT
	um.setMultiImage(im.getImage(representation::IMG));
#endif /* WITH_SEG_MEANSHIFT */

	// initialize docks (after initializing the models...)
	initDocks();

	// MODEL To be removed when refactored
	// into model classes.
	window->getViewerContainer()->setTaskQueue(&queue);

	// start with initial label (do this after signals, and before spawnROI())!
	lm.addLabel();

	/* Initial ROI images spawning. Do it before showing the window but after
	 * all signals were connected! */
	//GGDBGM("dimensions " << dimensions << endl);
	cv::Rect roi = dimensions; // initial ROI is image size, except:
	if (roi.area() > 262144) {
		// image is bigger than 512x512, start with a smaller ROI
		roi.width = std::min(roi.width, 512);
		roi.height = std::min(roi.height, 512);
	}
	//GGDBGM("roi " << roi << endl);
	spawnROI(roi);

	// set title and show window
	QFileInfo fi(QString::fromStdString(filename));
	window->setWindowTitle(QString("Gerbil - %1").arg(fi.completeBaseName()));
	window->show();
}

Controller::~Controller()
{
	delete window;
	// background task queue thread
	stopQueue();
}


/** Image management **/

// connect all signals between model and other parties
void Controller::initImage()
{
	/* gui requests */
	connect(window->getViewerContainer(),
			SIGNAL(bandSelected(representation::t, int)),
			&im, SLOT(computeBand(representation::t, int)));

	// TODO this belongs in DocksController
	/* im -> others */
	connect(&im, SIGNAL(bandUpdate(QPixmap, QString)),
			window, SLOT(changeBand(QPixmap, QString)));
	connect(&im, SIGNAL(imageUpdate(representation::t,SharedMultiImgPtr)),
			this, SLOT(docksUpdateImage(representation::t,SharedMultiImgPtr)));
}

// depends on ImageModel
void Controller::initFalseColor()
{
	fm.setMultiImg(representation::IMG, im.getImage(representation::IMG));

	connect(&im, SIGNAL(imageUpdate(representation::t,SharedMultiImgPtr)),
			&fm, SLOT(processImageUpdate(representation::t,SharedMultiImgPtr)));
}

void Controller::initIlluminant()
{
	illumm.setTaskQueue(&queue);
	illumm.setMultiImage(im.getImage(representation::IMG));

	// signals illumm <-> viewer container
	connect(&illumm, SIGNAL(newIlluminant(cv::Mat1f)),
			window->getViewerContainer(), SLOT(newIlluminant(cv::Mat1f)));
	connect(&illumm, SIGNAL(illuminantIsApplied(bool)),
			window->getViewerContainer(), SLOT(setIlluminantApplied(bool)));

	// TODO: signal does not exist right now.
	//connect(window, SIGNAL(roiChanged(cv::Rect)),
//			&illumm, SLOT(setRoi(cv::Rect)));

	// TODO: instead have a specific function in controller (like spectRescale)
//	connect(&illumm, SIGNAL(requestApplyROI(bool)),
//			window, SLOT(applyROI(bool)), Qt::DirectConnection);
	// TODO: don't know how RGB works right now.
//	connect(&illumm, SIGNAL(requestRebuildRGB()),
//			window, SLOT(rebuildRGB()), Qt::DirectConnection);

	connect(&illumm, SIGNAL(requestGUIEnabled(bool,TaskType)),
			window, SLOT(setGUIEnabled(bool, TaskType)), Qt::DirectConnection);
}

/** Labeling management **/

// connect all signals between model and other parties
void Controller::initLabeling(cv::Rect dimensions)
{
	// initialize label model
	lm.setDimensions(dimensions.width, dimensions.height);

	/* gui requests */
	connect(window, SIGNAL(clearLabelRequested(short)),
			&lm, SLOT(alterLabel(short)));
	connect(window, SIGNAL(alterLabelRequested(short,cv::Mat1b,bool)),
			&lm, SLOT(alterLabel(short,cv::Mat1b,bool)));
	connect(window, SIGNAL(alterLabelingRequested(cv::Mat1s,cv::Mat1b)),
			&lm, SLOT(alterPixels(cv::Mat1s,cv::Mat1b)));
	connect(window, SIGNAL(newLabelingRequested(cv::Mat1s)),
			&lm, SLOT(setLabels(cv::Mat1s)));

	/* lm -> others */
	connect(&lm,
			SIGNAL(newLabeling(const cv::Mat1s&, const QVector<QColor>&, bool)),
			this, SLOT(propagateLabelingChange(
					 const cv::Mat1s&, const QVector<QColor> &, bool)));
	connect(&lm, SIGNAL(partialLabelUpdate(const cv::Mat1s&,const cv::Mat1b&)),
			window->getViewerContainer(),
			SLOT(updateLabelsPartially(const cv::Mat1s&,const cv::Mat1b&)));
	connect(&lm, SIGNAL(partialLabelUpdate(const cv::Mat1s&,const cv::Mat1b&)),
			window,
			SLOT(processLabelingChange(const cv::Mat1s&,const cv::Mat1b&)));
}

void Controller::initDocks()
{
	dc = new DockController(this);
	dc->setImageModel(&im);
	dc->setIllumModel(&illumm);
	dc->setFalseColorModel(&fm);
#ifdef WITH_SEG_MEANSHIFT
	dc->setUsSegModel(&um);
#endif
	dc->setLabelingModel(&lm);
	dc->setMainWindow(window);
	dc->init();
}

void Controller::spawnROI(const cv::Rect &roi)
{
	bool reuse = true;
	// TODO: make a method cancelAllComputation that does following two steps
	/* TODO: this results in huge computation burden even if none of the
	 * running tasks would interfere. We could think of a more fine-grained
	 * method here. Previous was to keep track of "running task type"
	 *
	 * TODO: talk to petr about why we need the ROI state it seems to be only
	 * in use for this cancel and nothing else?
	 */
	if (!queue.isIdle()) {
		queue.cancelTasks(im.getROI());
		/* as we cancelled any tasks, we expect the image data not to reflect
		 * desired configuration, so we will recompute from scratch */
		reuse = false;
	}
	// also cancel CommandRunners
	fm.reset();

	disableGUI(TT_SELECT_ROI);
	// TODO: check
	window->getViewerContainer()->disconnectAllViewers();

	updateROI(reuse, roi);

	enableGUILater(true);
}

void Controller::rescaleSpectrum(size_t bands)
{
	queue.cancelTasks(im.getROI());
	disableGUI(TT_BAND_COUNT);
	// TODO: check
	window->getViewerContainer()->disconnectAllViewers();

	spectralRescaleInProgress = true;

	updateROI(false, cv::Rect(), bands);

	enableGUILater(true);
}

void Controller::updateROI(bool reuse, cv::Rect roi, size_t bands)
{
	// no new ROI provided
	if (roi == cv::Rect())
		roi = im.getROI();

	// prepare incremental update and test worthiness
	std::vector<cv::Rect> sub, add;
	if (reuse) {
		/* compute if it is profitable to add/sub pixels given old and new ROI,
		 * instead of full recomputation, and retrieve corresponding regions
		 */
		bool profitable = MultiImg::Auxiliary::rectTransform(im.getROI(), roi,
															 sub, add);
		if (!profitable)
			reuse = false;
	} else {
		// invalidate existing ROI information (to not re-use data)
		im.invalidateROI();
	}

	/** FIRST STEP: recycle existing payload **/
	QMap<representation::t, sets_ptr> sets;
	if (reuse) {
		foreach (representation::t i, representation::all()) {
			sets[i] = window->getViewerContainer()->subImage(i, sub, roi);
		}
	}

	/** SECOND STEP: update metadata */

	lm.updateROI(roi);

	/** THIRD STEP: update payload */
	/* this has to be done in the right order!
	 * IMG before all others, GRAD before GRADPCA
	 * it is implicit here but we would like this knowledge to be part of
	 * image model's logic
	 */
	foreach (representation::t i, representation::all()) {

		/* tasks to (incrementally) re-calculate image data */
		im.spawn(i, roi, bands);

		/* tasks to (incrementally) update distribution view */
		if (reuse) {
			window->getViewerContainer()->addImage(i, sets[i], add, roi);
		} else {
			window->getViewerContainer()->setImage(i, im.getImage(i), roi);
		}
	}

	// TODO: better method to make sure values in normalizationDock are correct
	// that means as soon as we have these values, report them directly to the
	// GUI.
	/*if (type == GRAD) {
		emit normTargetChanged(true);
	}*/
}

void Controller::propagateLabelingChange(const cv::Mat1s& labels,
										 const QVector<QColor> &colors,
										 bool colorsChanged)
{
	window->processLabelingChange(labels, colors, colorsChanged);

	bool grandUpdate = !labels.empty() || colorsChanged;

	if (grandUpdate)
		disableGUI();

	// TODO: we will talk to ViewerController directly
	window->getViewerContainer()->updateLabels(labels, colors, colorsChanged);

	if (grandUpdate)
		enableGUILater();
}

void Controller::addLabel()
{
	int index = lm.addLabel();

	// select our new label for convenience
	window->selectLabel(index);
}

/** DOCK stuff (to be moved to DockController */
void Controller::docksUpdateImage(representation::t type, SharedMultiImgPtr image)
{
	//GGDBGM(format("type=%1%  *image=%2% width=%3%") %type % (void*)(&**image) %(*image)->width << endl );
	/* conservative approach: do not initiate calculation tasks here,
	 * just invalidate data in the GUI (which may lead to initiating tasks)
	 */
	// TODO: if falsecolorDock is shown, trigger re-calculation of falsecolor image
}

/** Tasks and queue thread management */
void Controller::toggleLabels(bool toggle)
{
	// TODO: is this really legit? I doubt.
	// This is only to apply changes instantly,
	// instead of waiting for queue.
	queue.cancelTasks();
	disableGUI(TT_TOGGLE_LABELS);

	window->getViewerContainer()->toggleLabels(toggle);

	enableGUILater(false);
}

void Controller::enableGUILater(bool withROI)
{
	BackgroundTask *t = (withROI ? new BackgroundTask(im.getROI())
								 : new BackgroundTask());
	BackgroundTaskPtr taskEpilog(t);
	QObject::connect(taskEpilog.get(), SIGNAL(finished(bool)),
		this, SLOT(enableGUINow(bool)), Qt::QueuedConnection);
	queue.push(taskEpilog);
}

void Controller::enableGUINow(bool forreal)
{
	if(spectralRescaleInProgress) {
		// The number of spectral bands changed - let the GUI know about it.
		int nbands = im.getNumBandsROI();
		GGDBGM(format("emitting nSpectralBandsChanged(%1%)")%nbands << endl);
		emit nSpectralBandsChanged(nbands);
		spectralRescaleInProgress = false;
	}
	if (forreal)
		window->setGUIEnabled(true);
}

void Controller::disableGUI(TaskType tt)
{
	window->setGUIEnabled(false, tt);
}

/* container to allow passing an object reference to std::thread()
 * needed by initQueue(), without this std::thread() would run on a copy
 */
template<typename T>
struct holder {
	holder(T& payload) : payload(payload) {}
	void operator()() { payload(); }

	T& payload;
};

void Controller::startQueue()
{
	// start worker thread
	holder<BackgroundTaskQueue> h(queue);
	queuethread = new std::thread(h);
	// h is not needed anymore
}

void Controller::stopQueue()
{
	// cancel all jobs, then wait for thread to return
	queue.halt();
	queuethread->join();
	delete queuethread;
	queuethread = 0;
}
