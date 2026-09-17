#include "passdialog.h"

#include "shm_binder.h"  // FormatTip

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
    const uint32_t shown = hdr ? ShmPassCeiling(hdr) : kMaxPasses;
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
                            std::function<uint32_t()> read,
                            std::function<void(uint32_t)> write) -> QCheckBox* {
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
        return on;
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
    skin->setToolTip(FormatTip(
        "Needs the auto skin mask below. With the mask off, strengths of 0, 2 and 4 were measured "
        "to give byte-identical output -- the mask is what tells the model where skin is."));
    skinBox[pass] = skin;
    addRow("Skin structure", kOverrideSkinStructure, skin,
           [skin] { return FloatToBits(float(skin->value())); }, {});

    auto* sharp = makeFloat(0.0, 1.0, 0.05, pc.sharpnessBits.load());
    addRow("Sharpness", kOverrideSharpness, sharp,
           [sharp] { return FloatToBits(float(sharp->value())); }, {});

    auto* style = new QComboBox(page);
    style->addItems({ "Default", "Natural", "Cinematic" });
    style->setToolTip(FormatTip(
        "Three, and only three: styles above Cinematic were swept through this model and produced "
        "output identical to Cinematic."));
    // Clamped on the way in. A stored value above the last item -- from an older profile, or from
    // dlssnr-shmctl, which writes the header directly -- left this combo showing index -1, an empty
    // box that wrote 0 the moment it was touched.
    {
        const int st = int(pc.style.load());
        style->setCurrentIndex(st < style->count() ? st : style->count() - 1);
    }
    connect(style, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this, pass](int) { writePass(pass); });
    addRow("Style", kOverrideStyle, style, [style] { return uint32_t(style->currentIndex()); }, {});

    auto* preset = new QSpinBox(page);
    preset->setToolTip(FormatTip(
        "Read by the model every time this pass is built, and on this model build no value from 0 "
        "to 15 changed the picture."));
    preset->setRange(0, 15);
    preset->setValue(int(pc.preset.load()));
    connect(preset, QOverload<int>::of(&QSpinBox::valueChanged), this,
            [this, pass](int) { writePass(pass); });
    addRow("Preset", kOverridePreset, preset, [preset] { return uint32_t(preset->value()); }, {});

    auto* automask = new QCheckBox("on", page);
    automask->setChecked(pc.autoMask.load() != 0);
    automask->setToolTip(FormatTip(
        "The model's automatic skin mask, and the switch that makes Skin structure above mean "
        "anything."));
    maskBox[pass] = automask;
    connect(automask, &QCheckBox::toggled, this, [this, pass](bool) { writePass(pass); });
    maskOverride[pass] = addRow("Auto skin mask", kOverrideAutoMask, automask,
                                [automask] { return automask->isChecked() ? 1u : 0u; }, {});

    auto* uicorr = new QCheckBox("on", page);
    uicorr->setChecked(pc.uiCorrection.load() != 0);
    uicorr->setToolTip(FormatTip(
        "Tell the model this pass's frame already has the game's interface drawn on it. Read every "
        "frame, so it costs no rebuild."));
    connect(uicorr, &QCheckBox::toggled, this, [this, pass](bool) { writePass(pass); });
    addRow("UI correction", kOverrideUiCorrection, uicorr,
           [uicorr] { return uicorr->isChecked() ? 1u : 0u; }, {});

    // Both halves of the resolution move the skin row: the pass's own mask value, and whether the
    // pass names the mask at all -- untick it and the pass follows the global setting instead.
    connect(automask, &QCheckBox::toggled, this, [this, pass] { updateSkinEnabled(pass); });
    if (maskOverride[pass])
        connect(maskOverride[pass], &QCheckBox::toggled, this, [this, pass] { updateSkinEnabled(pass); });
    updateSkinEnabled(pass);
}

// Whether this pass ends up with the mask on, resolved the way the helper resolves it: the pass's
// own value when it ticks the override, the global setting otherwise.
void PassDialog::updateSkinEnabled(uint32_t pass) {
    if (pass >= kMaxPasses || !hdr || !skinBox[pass] || !maskBox[pass]) return;
    const bool overridden = maskOverride[pass] && maskOverride[pass]->isChecked();
    const bool on = overridden ? maskBox[pass]->isChecked() : hdr->autoMask.load() != 0;
    // Only ever narrows: the row is already off whenever this pass does not name skin structure,
    // and the mask can take it away but never give it back.
    for (const Row& r : rows[pass])
        if (r.bit == kOverrideSkinStructure && r.on)
            skinBox[pass]->setEnabled(r.on->isChecked() && on);
    skinBox[pass]->setToolTip(FormatTip(
        on ? "Needs the auto skin mask below. With the mask off, strengths of 0, 2 and 4 were "
             "measured to give byte-identical output."
           : "Inert: this pass resolves to the auto skin mask being OFF, and with it off a skin "
             "strength was measured to change nothing. Turn the mask on, here or globally."));
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
            case kOverrideUiCorrection: pc.uiCorrection.store(v); break;
            default: break;
        }
    }
    pc.overrideMask.store(mask);
    hdr->controlSeq.fetch_add(1);
    // Bumped whatever changed, because this dialog writes a whole pass at once and cannot say which
    // field moved. The helper decides for itself whether a rebuild is actually owed -- it compares
    // the resolved tuning by value, and the ones the model reads at evaluate (sharpness, UI
    // correction) are absent from that comparison, so touching only those costs nothing.
    hdr->tuningSeq.fetch_add(1);
}
