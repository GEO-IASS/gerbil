#include "viewport.h"
#include "qpainter.h"
#include <iostream>
#include <QtCore>
#include <QPaintEvent>
#include <QRect>

using namespace std;

Viewport::Viewport(QWidget *parent)
	: QGLWidget(QGLFormat(QGL::SampleBuffers), parent)
{}

void Viewport::paintEvent(QPaintEvent *event)
{
	QBrush background (QColor(64, 32, 64));
	QPainter painter;
	QTransform modelview;
	modelview.translate(10, 10);
	modelview.scale((width() - 20)/(qreal)(dimensionality-1),
					(height() - 20)/(qreal)nbins);
	painter.begin(this);

	painter.fillRect(event->rect(), background);
	painter.setWorldTransform(modelview);
	painter.setRenderHint(QPainter::Antialiasing);
	cerr << sets.size() << endl;
	for (int i = 0; i < sets.size(); ++i) {
		const BinSet *s = sets[i];
		QColor color = s->label;
		QHash<qlonglong, Bin>::const_iterator it;
		for (it = s->bins.constBegin(); it != s->bins.constEnd(); ++it) {
			const Bin &b = it.value();
			color.setAlphaF(b.weight / s->totalweight);
			painter.setPen(color);
			painter.drawLines(b.connections);
		}
	}
	painter.end();
}