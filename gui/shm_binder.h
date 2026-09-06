#pragma once
// Binds widgets to fields of the shared header.
//
// Table-driven for the same reason dlssnr-shmctl is: there are around thirty settings now, and
// hand-writing a widget, a default, a read-back and a connect() for each is four places to forget
// something in. Here a control is one line, and a header field with no line is visibly absent rather
// than quietly unreachable.
//
// Two things the binder knows that the widgets do not. Settings the model latches when its feature is
// built bump tuningSeq as well as controlSeq, which is what tells the helper to rebuild rather than
// carry on with a feature made from the old values. And reloading from the header blocks signals
// while it writes, so refreshing the interface cannot be mistaken for the user changing something.
#include "../common/shm_protocol.h"

#include <QObject>
#include <QStringList>
#include <functional>
#include <vector>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QFormLayout;
class QSpinBox;
class QWidget;

class ShmBinder : public QObject {
    Q_OBJECT
  public:
    using Field = std::atomic<uint32_t> ShmHeader::*;

    // Whether the model reads this when its feature is built. Changing one of these costs a rebuild,
    // so the helper has to be told; changing anything else takes effect on the next frame.
    enum Latch { Live, AtCreate };

    ShmBinder(ShmHeader* header, QWidget* parent);

    void SetHeader(ShmHeader* header) { _hdr = header; }

    // invert: the checkbox reads as the opposite of the stored bit -- a box labelled "Enabled" over
    // a field that stores the negative, like the composition switch over compositionBypass.
    QCheckBox* AddBool(QFormLayout* form, const QString& label, Field field, const QString& tip,
                       Latch latch = Live, bool invert = false);
    QSpinBox* AddInt(QFormLayout* form, const QString& label, Field field, int lo, int hi,
                     const QString& tip, Latch latch = Live);
    QDoubleSpinBox* AddFloat(QFormLayout* form, const QString& label, Field field, double lo, double hi,
                             double step, const QString& tip, Latch latch = Live);
    QComboBox* AddChoice(QFormLayout* form, const QString& label, Field field, const QStringList& options,
                         const QString& tip, Latch latch = Live);

    // A float stored as a fraction but shown as a percentage, which is how model resolution reads.
    QSpinBox* AddPercent(QFormLayout* form, const QString& label, Field field, int lo, int hi,
                         const QString& tip, Latch latch = Live);

    // Pull every bound control's value out of the header. Safe to call at any time.
    void Reload();

  private:
    void Write(Field field, uint32_t raw, Latch latch);

    ShmHeader* _hdr = nullptr;
    QWidget* _parent = nullptr;
    std::vector<std::function<void()>> _reloaders;
    bool _reloading = false;
};
