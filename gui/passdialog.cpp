#include "passdialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTabWidget>
#include <QVBoxLayout>

PassDialog::PassDialog(ShmHeader* header, QWidget* parent) : QDialog(parent), hdr(header) {
    setWindowTitle("Per-pass model settings");
    resize(460, 460);

    auto* root = new QVBoxLayout(this);
    auto* note = new QLabel(
        "A pass follows the global settings for anything it does not tick. These are read when that "
        "pass's feature is built, so a change takes a moment to appear.",
        this);
    note->setWordWrap(true);
    root->addWidget(note);

    tabs = new QTabWidget(this);
    // Only the passes that can actually run are offered; the ceiling is a real limit, not a hint.
    const uint32_t shown = hdr ? ShmPassCeiling(hdr) : kDefaultMaxPasses;
    for (uint32_t i = 0; i < shown; ++i) {
        auto* page = new QWidget(tabs);
        buildPage(i, page);
        tabs->addTab(page, QString("Pass %1").arg(i + 1));
    }
    root->addWidget(tabs);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::accept);
    root->addWidget(buttons);
}

void PassDialog::buildPage(uint32_t pass, QWidget* page) {
    auto* form = new QFormLayout(page);
    if (!hdr) return;

    const uint32_t mask = hdr->pass[pass].overrideMask.load();
    PassControl& pc = hdr->pass[pass];

    const auto addRow = [&](const QString& label, uint32_t bit, QWidget* value,
                            std::function<uint32_t()> read, std::function<void(uint32_t)> write) {
        auto* on = new QCheckBox(page);
        on->setChecked((mask & bit) != 0);
        value->setEnabled(on->isChecked());

        auto* row = new QHBoxLayout;
        row->addWidget(on);
        row->addWidget(value, 1);
        form->addRow(label, row);

        Row r{ on, value, bit, std::move(read), std::move(write) };
        rows[pass].push_back(r);

        connect(on, &QCheckBox::toggled, this, [this, pass, value](bool checked) {
            value->setEnabled(checked);
            writePass(pass);
        });
    };

    const auto makeFloat = [&](double lo, double hi, double step, uint32_t bits) {
        auto* w = new QDoubleSpinBox(page);
        w->setRange(lo, hi);
        w->setSingleStep(step);
        w->setValue(double(BitsToFloat(bits)));
        connect(w, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
                [this, pass](double) { writePass(pass); });
        return w;
    };

    auto* intensity = makeFloat(0.0, 4.0, 0.05, pc.intensityBits.load());
    addRow("Intensity", kOverrideIntensity, intensity,
           [intensity] { return FloatToBits(float(intensity->value())); }, {});

    auto* structure = makeFloat(0.0, 4.0, 0.05, pc.localStructureBits.load());
    addRow("Local structure", kOverrideLocalStructure, structure,
           [structure] { return FloatToBits(float(structure->value())); }, {});

    auto* tone = makeFloat(0.0, 4.0, 0.05, pc.localToneBits.load());
    addRow("Local tone", kOverrideLocalTone, tone,
           [tone] { return FloatToBits(float(tone->value())); }, {});

    auto* skin = makeFloat(-1.0, 4.0, 0.05, pc.skinStructureBits.load());
    addRow("Skin structure", kOverrideSkinStructure, skin,
           [skin] { return FloatToBits(float(skin->value())); }, {});

    auto* sharp = makeFloat(0.0, 1.0, 0.05, pc.sharpnessBits.load());
    addRow("Sharpness", kOverrideSharpness, sharp,
           [sharp] { return FloatToBits(float(sharp->value())); }, {});

    auto* style = new QComboBox(page);
    style->addItems({ "Default", "Natural", "Cinematic" });
    style->setCurrentIndex(int(pc.style.load()));
    connect(style, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this, pass](int) { writePass(pass); });
    addRow("Style", kOverrideStyle, style, [style] { return uint32_t(style->currentIndex()); }, {});

    auto* preset = new QSpinBox(page);
    preset->setRange(0, 15);
    preset->setValue(int(pc.preset.load()));
    connect(preset, QOverload<int>::of(&QSpinBox::valueChanged), this,
            [this, pass](int) { writePass(pass); });
    addRow("Preset", kOverridePreset, preset, [preset] { return uint32_t(preset->value()); }, {});

    auto* automask = new QCheckBox("on", page);
    automask->setChecked(pc.autoMask.load() != 0);
    connect(automask, &QCheckBox::toggled, this, [this, pass](bool) { writePass(pass); });
    addRow("Auto skin mask", kOverrideAutoMask, automask,
           [automask] { return automask->isChecked() ? 1u : 0u; }, {});
}

void PassDialog::writePass(uint32_t pass) {
    if (!hdr || pass >= kMaxPasses) return;
    PassControl& pc = hdr->pass[pass];

    uint32_t mask = 0;
    for (const Row& r : rows[pass]) {
        if (!r.on->isChecked()) continue;
        mask |= r.bit;
        const uint32_t v = r.read();
        switch (r.bit) {
            case kOverrideIntensity: pc.intensityBits.store(v); break;
            case kOverrideLocalStructure: pc.localStructureBits.store(v); break;
            case kOverrideLocalTone: pc.localToneBits.store(v); break;
            case kOverrideSkinStructure: pc.skinStructureBits.store(v); break;
            case kOverrideSharpness: pc.sharpnessBits.store(v); break;
            case kOverrideStyle: pc.style.store(v); break;
            case kOverridePreset: pc.preset.store(v); break;
            case kOverrideAutoMask: pc.autoMask.store(v); break;
            default: break;
        }
    }
    pc.overrideMask.store(mask);
    hdr->controlSeq.fetch_add(1);
    // Every one of these is latched when the pass's feature is built, so the helper has to know to
    // rebuild rather than carry on with a feature made from the old values.
    hdr->tuningSeq.fetch_add(1);
}
