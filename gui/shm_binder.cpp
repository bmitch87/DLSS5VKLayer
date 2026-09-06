#include "shm_binder.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QWidget>

#include <cmath>

// Each Add* pulls its own value out of the header the moment the control exists, so a control is
// never showing something the header does not say. Relying on a single Reload() at the end of the
// panel worked only for as long as nobody added a control after it, which is the kind of ordering
// dependency that fails quietly and much later.
ShmBinder::ShmBinder(ShmHeader* header, QWidget* parent)
    : QObject(parent), _hdr(header), _parent(parent) {}

void ShmBinder::Write(Field field, uint32_t raw, Latch latch) {
    if (!_hdr || _reloading) return;
    (_hdr->*field).store(raw);
    _hdr->controlSeq.fetch_add(1);
    if (latch == AtCreate) _hdr->tuningSeq.fetch_add(1);
}

QCheckBox* ShmBinder::AddBool(QFormLayout* form, const QString& label, Field field, const QString& tip,
                              Latch latch, bool invert) {
    auto* w = new QCheckBox(label, _parent);
    w->setToolTip(tip);
    form->addRow(w);
    connect(w, &QCheckBox::toggled, this, [this, field, latch, invert](bool on) {
        const bool v = invert ? !on : on;
        Write(field, v ? 1u : 0u, latch);
    });
    _reloaders.push_back([this, w, field, invert] {
        if (!_hdr) return;
        QSignalBlocker block(w);
        const bool v = (_hdr->*field).load() != 0;
        w->setChecked(invert ? !v : v);
    });
    _reloaders.back()();
    return w;
}

QSpinBox* ShmBinder::AddInt(QFormLayout* form, const QString& label, Field field, int lo, int hi,
                            const QString& tip, Latch latch) {
    auto* w = new QSpinBox(_parent);
    w->setRange(lo, hi);
    w->setToolTip(tip);
    form->addRow(label, w);
    connect(w, QOverload<int>::of(&QSpinBox::valueChanged), this,
            [this, field, latch](int v) { Write(field, uint32_t(v < 0 ? 0 : v), latch); });
    _reloaders.push_back([this, w, field] {
        if (!_hdr) return;
        QSignalBlocker block(w);
        w->setValue(int((_hdr->*field).load()));
    });
    _reloaders.back()();
    return w;
}

QDoubleSpinBox* ShmBinder::AddFloat(QFormLayout* form, const QString& label, Field field, double lo,
                                    double hi, double step, const QString& tip, Latch latch) {
    auto* w = new QDoubleSpinBox(_parent);
    w->setRange(lo, hi);
    w->setSingleStep(step);
    w->setDecimals(step < 0.01 ? 3 : 2);
    w->setToolTip(tip);
    form->addRow(label, w);
    connect(w, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            [this, field, latch](double v) { Write(field, FloatToBits(float(v)), latch); });
    _reloaders.push_back([this, w, field] {
        if (!_hdr) return;
        QSignalBlocker block(w);
        w->setValue(double(BitsToFloat((_hdr->*field).load())));
    });
    _reloaders.back()();
    return w;
}

QComboBox* ShmBinder::AddChoice(QFormLayout* form, const QString& label, Field field,
                                const QStringList& options, const QString& tip, Latch latch) {
    auto* w = new QComboBox(_parent);
    w->addItems(options);
    w->setToolTip(tip);
    form->addRow(label, w);
    connect(w, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this, field, latch](int i) { Write(field, uint32_t(i < 0 ? 0 : i), latch); });
    _reloaders.push_back([this, w, field] {
        if (!_hdr) return;
        QSignalBlocker block(w);
        const int v = int((_hdr->*field).load());
        w->setCurrentIndex(v < w->count() ? v : 0);
    });
    _reloaders.back()();
    return w;
}

QSpinBox* ShmBinder::AddPercent(QFormLayout* form, const QString& label, Field field, int lo, int hi,
                                const QString& tip, Latch latch) {
    auto* w = new QSpinBox(_parent);
    w->setRange(lo, hi);
    w->setSuffix("%");
    w->setSingleStep(5);
    w->setToolTip(tip);
    form->addRow(label, w);
    connect(w, QOverload<int>::of(&QSpinBox::valueChanged), this,
            [this, field, latch](int v) { Write(field, FloatToBits(float(v) / 100.0f), latch); });
    _reloaders.push_back([this, w, field] {
        if (!_hdr) return;
        QSignalBlocker block(w);
        w->setValue(int(std::lround(double(BitsToFloat((_hdr->*field).load())) * 100.0)));
    });
    _reloaders.back()();
    return w;
}

void ShmBinder::Reload() {
    _reloading = true;
    for (auto& r : _reloaders) r();
    _reloading = false;
}
