#pragma once
#include <QDialog>
#include <cstdint>
#include <vector>
class QLabel;
class PieceGrid;

class PieceBitmapDialog : public QDialog {
public:
    explicit PieceBitmapDialog(QWidget* parent = nullptr);
    void setPieces(const QString& peerName, quint64 count, quint64 owned,
                   const std::vector<std::uint8_t>& bits);
private:
    QLabel* summary_;
    PieceGrid* grid_;
};
