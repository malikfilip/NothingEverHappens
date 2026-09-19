#pragma once

#include "ScenarioPeer.hpp"
#include <QDialog>

class QLineEdit;
class QRadioButton;
class QSpinBox;
class QComboBox;
class QPushButton;
class PieceCountSpinBox;

class AddPeerDialog : public QDialog {
public:
    AddPeerDialog(const QString& defaultName, quint64 swarmPieceCount, QWidget* parent = nullptr);
    ScenarioPeer peer() const;

private:
    void updateValidity();
    quint64 swarmPieceCount_;
    QLineEdit* name_;
    QRadioButton* seeder_;
    PieceCountSpinBox* pieces_;
    QSpinBox* upload_;
    QSpinBox* download_;
    QComboBox* uploadUnit_;
    QComboBox* downloadUnit_;
    QPushButton* add_;
};
