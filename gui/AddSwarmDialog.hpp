#pragma once

#include "ScenarioSwarm.hpp"
#include <QDialog>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QRadioButton;

class AddSwarmDialog : public QDialog {
public:
    explicit AddSwarmDialog(const QString& defaultName, QWidget* parent = nullptr);
    ScenarioSwarm swarm() const;

private:
    void updateValues();
    void updateFileLabel();
    bool eventFilter(QObject* watched, QEvent* event) override;

    QLineEdit* name_;
    QRadioButton* fileMode_;
    QPushButton* browse_;
    QLabel* filename_;
    QLineEdit* virtualSize_;
    QComboBox* virtualUnit_;
    QLineEdit* pieceSize_;
    QComboBox* pieceUnit_;
    QLineEdit* pieces_;
    QPushButton* create_;
    QString filePath_;
    quint64 fileSizeBytes_ = 0;
    quint64 totalSizeBytes_ = 0;
    quint64 pieceSizeBytes_ = 0;
    quint64 pieceCount_ = 0;
};
