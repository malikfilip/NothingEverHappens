#include "SimulationView.hpp"
#include "ScenarioSwarm.hpp"

#include <QGraphicsPixmapItem>

#include <QGraphicsScene>
#include <QPainter>
#include <QPalette>

SimulationView::SimulationView(QWidget* parent)
    : QGraphicsView(parent)
{
    setObjectName("simulationCanvas");
    setScene(new QGraphicsScene(this));
    setRenderHint(QPainter::Antialiasing);
    setBackgroundBrush(palette().brush(QPalette::Base));
    setFrameShape(QFrame::StyledPanel);
    setMinimumSize(280, 180);
    setAccessibleName(tr("Simulation canvas"));
}

void SimulationView::showSwarm(const ScenarioSwarm* swarm)
{
    scene()->clear();
    scene()->setSceneRect(-160, -100, 320, 200);
    if (swarm) {
        const QPixmap icon(QStringLiteral(":/resources/nodes/tracker.png"));
        auto* tracker = scene()->addPixmap(icon.scaled(64, 64,
            Qt::KeepAspectRatio, Qt::SmoothTransformation));
        tracker->setOffset(-tracker->pixmap().width() / 2.0, -tracker->pixmap().height() / 2.0);
        tracker->setToolTip(tr("Tracker - %1").arg(swarm->name));
    }
    centerOn(0, 0);
}
