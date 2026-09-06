#pragma once
#include <QDialog>
#include <array>
#include <functional>
#include <vector>
#include "../common/shm_protocol.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QSpinBox;
class QComboBox;
class QTabWidget;
class QWidget;

// Per-pass model settings, sparse.
//
// A pass names only the fields it wants to differ; anything it does not name follows the global
// setting. That is what makes "pass 3 is gentler" expressible without restating everything else about
// pass 3, and it is why each field has its own tick rather than the whole pass having one.
class PassDialog : public QDialog {
    Q_OBJECT
public:
    explicit PassDialog(ShmHeader* hdr, QWidget* parent = nullptr);

private:
    // One overridable field: the tick that says whether this pass names it, and the control holding
    // the value it names.
    struct Row {
        QCheckBox* on = nullptr;
        QWidget* value = nullptr;
        uint32_t bit = 0;
        std::function<uint32_t()> read;
        std::function<void(uint32_t)> write;
    };

    void writePass(uint32_t pass);
    void buildPage(uint32_t pass, QWidget* page);

    ShmHeader* hdr = nullptr;
    QTabWidget* tabs = nullptr;
    std::array<std::vector<Row>, kMaxPasses> rows;
};
