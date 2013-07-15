#include "model/image.h"
#include "tasks/normrangecuda.h"
#include "tasks/normrangetbb.h"
#include "tasks/rgbtbb.h"

#include "../gerbil_gui_debug.h"

#include <multi_img_offloaded.h>
#include <multi_img_tasks.h>
#include <imginput.h>
#include <boost/make_shared.hpp>

#include <opencv2/gpu/gpu.hpp>

#define USE_CUDA_GRADIENT       1
#define USE_CUDA_DATARANGE      0
#define USE_CUDA_CLAMP          0

ImageModel::ImageModel(BackgroundTaskQueue &queue, bool lm)
	: limitedMode(lm), queue(queue),
	  image_lim(new SharedMultiImgBase(new multi_img())),
	  nBands(-1)
{
	foreach (representation::t i, representation::all()) {
		map.insert(i, new payload(i));
	}

	foreach (payload *p, map) {
		connect(p, SIGNAL(newImageData(representation::t,SharedMultiImgPtr)),
				this, SLOT(processNewImageData(representation::t,SharedMultiImgPtr)));
		connect(p, SIGNAL(dataRangeUpdate(representation::t,ImageDataRange)),
				this, SIGNAL(dataRangeUdpate(representation::t,ImageDataRange)));
	}
}

ImageModel::~ImageModel()
{
	foreach (payload *p, map)
		delete p;

	if (!limitedMode) {
		// release image_lim as imgHolder has still ownership and will delete
		image_lim->swap((multi_img_base*)0);
	}
}

size_t ImageModel::getNumBandsFull()
{
	SharedMultiImgBaseGuard guard(*image_lim);
	return (*image_lim)->size();
}

int ImageModel::getNumBandsROI()
{
	return nBands;
}

cv::Rect ImageModel::loadImage(const std::string &filename)
{
	if (limitedMode) {
		// create offloaded image
		std::pair<std::vector<std::string>, std::vector<multi_img::BandDesc> >
				filelist = multi_img::parse_filelist(filename);
		image_lim = boost::make_shared<SharedMultiImgBase>
				(new multi_img_offloaded(filelist.first, filelist.second));
	} else {
		// create using ImgInput
		vole::ImgInputConfig inputConfig;
		inputConfig.file = filename;
		imgHolder = vole::ImgInput(inputConfig).execute();
		image_lim = boost::make_shared<SharedMultiImgBase>(imgHolder.get());
	}

	if ((*image_lim)->empty()) {
		return cv::Rect();
	} else {
		nBands = (*image_lim)->size();
		return cv::Rect(0, 0, (*image_lim)->width, (*image_lim)->height);
	}
}

void ImageModel::invalidateROI()
{
	// set roi to empty rect
	roi = cv::Rect();
	foreach (payload *p, map) {
		if (!p->image)
			continue;

		SharedDataLock lock(p->image->mutex);
		(*(p->image))->roi = roi;
	}
}

void ImageModelPayload::processImageDataTaskFinished(bool success)
{
	if (!success)
		return;

	// signal new image data
	emit newImageData(type, image);
	emit dataRangeUpdate(type, **normRange);
}

void ImageModel::spawn(representation::t type, const cv::Rect &newROI, size_t bands)
{
	// one ROI for all, effectively
	roi = newROI;

	// invalidate band caches
	map[type]->bands.clear();

	// shortcuts for convenience
	SharedMultiImgPtr image = map[representation::IMG]->image,
			gradient = map[representation::GRAD]->image,
			imagepca = map[representation::IMGPCA]->image,
			gradpca = map[representation::IMGPCA]->image;

	// scoping and spectral rescaling done for IMG
	if (type == representation::IMG) {
		// scope image to new ROI
		SharedMultiImgPtr scoped_image(new SharedMultiImgBase(NULL));
		BackgroundTaskPtr taskScope(new MultiImg::ScopeImage(
			image_lim, scoped_image, roi));
		queue.push(taskScope);

		// sanitize spectral rescaling parameters
		assert(-1 != getNumBandsFull());
		if (bands == -1 || bands > getNumBandsFull())
			bands = getNumBandsFull();
		if (bands <= 2)
			bands = 3;
		assert(-1 != bands);

		// perform spectral rescaling
		BackgroundTaskPtr taskRescale(new MultiImg::RescaleTbb(
			scoped_image, image, bands, roi));
		queue.push(taskRescale);
	}

	if (type == representation::GRAD) {
		if (cv::gpu::getCudaEnabledDeviceCount() > 0 && USE_CUDA_GRADIENT) {
			BackgroundTaskPtr taskGradient(new MultiImg::GradientCuda(
				image, gradient, roi));
			queue.push(taskGradient);
		} else {
			BackgroundTaskPtr taskGradient(new MultiImg::GradientTbb(
				image, gradient, roi));
			queue.push(taskGradient);
		}
	}

	// user-customizable norm range calculation, sets minval/maxval of the image
	if (type == representation::IMG || type == representation::GRAD)
	{
		SharedMultiImgPtr target = map[type]->image;
		SharedDataRangePtr range = map[type]->normRange;
		MultiImg::NormMode mode =  map[type]->normMode;
		// TODO: a small hack in NormRangeTBB to determine theoretical range
		int isGRAD = (type == representation::GRAD ? 1 : 0);

		SharedDataLock hlock(range->mutex);
		//GGDBGM(format("%1% range %2%") %type %**range << endl);
		double min = (*range)->min;
		double max = (*range)->max;
		hlock.unlock();

		if (cv::gpu::getCudaEnabledDeviceCount() > 0 && USE_CUDA_DATARANGE) {
			BackgroundTaskPtr taskNormRange(new NormRangeCuda(
				target, range, mode, isGRAD, min, max, true, roi));
			queue.push(taskNormRange);
		} else {
			BackgroundTaskPtr taskNormRange(new NormRangeTbb(
				target, range, mode, isGRAD, min, max, true, roi));
			queue.push(taskNormRange);
		}
	}

	if (type == representation::IMGPCA && imagepca.get()) {
		BackgroundTaskPtr taskPca(new MultiImg::PcaTbb(
			image, imagepca, 0, roi));
		queue.push(taskPca);
	}

	if (type == representation::GRADPCA && gradpca.get()) {
		BackgroundTaskPtr taskPca(new MultiImg::PcaTbb(
			gradient, gradpca, 0, roi));
		queue.push(taskPca);
	}

	// emit signal after all tasks are finished and fully updated data available
	BackgroundTaskPtr taskEpilog(new BackgroundTask(roi));
	QObject::connect(taskEpilog.get(), SIGNAL(finished(bool)),
					 map[type], SLOT(processImageDataTaskFinished(bool)));
	queue.push(taskEpilog);
}

void ImageModel::computeBand(representation::t type, int dim)
{
	QMap<int, QPixmap> &m = map[type]->bands;

	if (!m.contains(dim)) {
		SharedMultiImgPtr src = map[type]->image;
		qimage_ptr dest(new SharedData<QImage>(new QImage()));

		SharedDataLock hlock(src->mutex);
		BackgroundTaskPtr taskConvert(
					new MultiImg::Band2QImageTbb(src, dest, dim));
		taskConvert->run();
		hlock.unlock();

		m[dim] = QPixmap::fromImage(**dest);
	}

	// retrieve wavelength information
	SharedMultiImgPtr src = map[type]->image;
	SharedDataLock hlock(src->mutex);
	std::string banddesc = (*src)->meta[dim].str();
	hlock.unlock();
	QString desc;
	const char * const str[] =
		{ "Image", "Gradient", "Image PCA", "Gradient PCA" };
	if (banddesc.empty())
		desc = QString("%1 Band #%2").arg(str[type]).arg(dim+1);
	else
		desc = QString("%1 Band %2").arg(str[type]).arg(banddesc.c_str());

	emit bandUpdate(m[dim], desc);
}

void ImageModel::computeFullRgb()
{
	qimage_ptr fullRgb(new SharedData<QImage>(NULL));
	/* we do it instantly as this is typically what the user wants to see first,
	 * and not wait for it while the queue processes other things */
	BackgroundTaskPtr taskRgb(new RgbTbb(
		image_lim, mat3f_ptr(new SharedData<cv::Mat3f>(new cv::Mat3f)),
								  fullRgb));
	taskRgb->run();

	QPixmap p = QPixmap::fromImage(**fullRgb);
	emit fullRgbUpdate(p);
}

void ImageModel::setNormalizationParameters(
		representation::t type,
		MultiImg::NormMode normMode,
		ImageDataRange targetRange)
{
	//GGDBGM(type << " " << targetRange << endl);
	map[type]->normMode = normMode;
	SharedDataLock lock(map[type]->normRange->mutex);
	**(map[type]->normRange) = targetRange;
}


void ImageModel::processNewImageData(representation::t type, SharedMultiImgPtr image)
{
	if(representation::IMG == type) {
		SharedDataLock lock(image->mutex);
		nBands = (*image)->size();
	}
	emit imageUpdate(type, image);
}
