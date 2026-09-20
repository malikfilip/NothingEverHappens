#include "PieceBitmapDialog.hpp"
#include <QAbstractScrollArea>
#include <QDialogButtonBox>
#include <QHelpEvent>
#include <QLabel>
#include <QPainter>
#include <QResizeEvent>
#include <QScrollBar>
#include <QToolTip>
#include <QVBoxLayout>
#include <algorithm>
#include <limits>

class PieceGrid : public QAbstractScrollArea {
public:
    explicit PieceGrid(QWidget* parent) : QAbstractScrollArea(parent) {
        setAccessibleName(tr("Piece ownership grid"));
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
        connect(verticalScrollBar(), &QScrollBar::valueChanged, viewport(), qOverload<>(&QWidget::update));
    }
    void setPieces(quint64 count, const std::vector<std::uint8_t>& bits) {
        if (count_ == count && bits_ == bits) return;
        count_ = count;
        bits_ = bits;
        updateRange();
        viewport()->update();
    }
protected:
    void resizeEvent(QResizeEvent* event) override {
        QAbstractScrollArea::resizeEvent(event);
        updateRange();
    }
    void scrollContentsBy(int, int) override { viewport()->update(); }
    void paintEvent(QPaintEvent*) override {
        QPainter painter(viewport());
        painter.fillRect(viewport()->rect(), palette().base());
        const auto firstRow = static_cast<quint64>(verticalScrollBar()->value());
        const int visibleRows = viewport()->height() / cell + 1;
        for (int row = 0; row < visibleRows; ++row) {
            for (int column = 0; column < columns(); ++column) {
                const auto index = (firstRow + row) * columns() + column;
                if (index >= count_) return;
                const QRect rect(column * cell + 2, row * cell + 2, cell - 4, cell - 4);
                painter.fillRect(rect, owns(index) ? QColor("#32965a") : palette().base().color());
                painter.setPen(palette().text().color());
                painter.drawRect(rect);
            }
        }
    }
    bool viewportEvent(QEvent* event) override {
        if (event->type() == QEvent::ToolTip) {
            auto* help = static_cast<QHelpEvent*>(event);
            const int column = help->pos().x() / cell;
            const auto row = static_cast<quint64>(verticalScrollBar()->value()) + help->pos().y() / cell;
            const auto index = row * columns() + column;
            if (column < columns() && index < count_)
                QToolTip::showText(help->globalPos(), tr("Piece %1 - %2")
                    .arg(index).arg(owns(index) ? tr("Owned") : tr("Missing")), viewport());
            else QToolTip::hideText();
            return true;
        }
        return QAbstractScrollArea::viewportEvent(event);
    }
private:
    static constexpr int cell = 20;
    int columns() const { return std::max(4, viewport()->width() / cell); }
    bool owns(quint64 index) const {
        return index / 8 < bits_.size() && (bits_[index / 8] & (0x80u >> (index % 8))) != 0;
    }
    void updateRange() {
        // Scroll in rows rather than pixels, avoiding huge widget dimensions.
        const quint64 rows = count_ / columns() + (count_ % columns() != 0);
        const auto visible = static_cast<quint64>(std::max(1, viewport()->height() / cell));
        verticalScrollBar()->setRange(0, static_cast<int>(std::min<quint64>(
            rows > visible ? rows - visible : 0, std::numeric_limits<int>::max())));
        verticalScrollBar()->setPageStep(static_cast<int>(visible));
        verticalScrollBar()->setSingleStep(1);
    }
    quint64 count_ = 0;
    std::vector<std::uint8_t> bits_;
};

PieceBitmapDialog::PieceBitmapDialog(QWidget* parent) : QDialog(parent)
{
    setWindowTitle(tr("Peer Pieces"));
    setModal(false);
    resize(600, 420);
    setMinimumSize(300, 240);
    auto* layout = new QVBoxLayout(this);
    summary_ = new QLabel(this);
    summary_->setTextFormat(Qt::PlainText);
    summary_->setWordWrap(true);
    layout->addWidget(summary_);
    grid_ = new PieceGrid(this);
    layout->addWidget(grid_, 1);
    auto* legend = new QLabel(tr("Filled green: owned    Empty: missing\nHover a cell for its zero-based piece index."), this);
    legend->setWordWrap(true);
    layout->addWidget(legend);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}
void PieceBitmapDialog::setPieces(const QString& peerName, quint64 count, quint64 owned,
                                const std::vector<std::uint8_t>& bits)
{
    summary_->setText(tr("%1 ? %2 / %3 pieces").arg(peerName).arg(owned).arg(count));
    grid_->setPieces(count, bits);
}
