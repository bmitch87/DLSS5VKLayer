// The settings binder, tested without a window manager.
//
// Every control in the interface is one line of table, and the thing that line has to get right is
// invisible: the value has to reach the shared header, the right sequence number has to be bumped,
// and reloading has to not look like the user changing something. None of that is checked by the
// interface appearing on screen, which is the only test this project had before.
//
// Runs offscreen, so it needs no display:
//
//     QT_QPA_PLATFORM=offscreen ./build/binder_test
#include "shm_binder.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QSpinBox>
#include <QWidget>

#include <cstdio>
#include <cstring>

static int failures = 0;
static void check(const char* what, bool ok) {
    std::printf("  %-42s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) ++failures;
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);

    ShmHeader hdr{};
    ShmInitDefaults(&hdr);

    QWidget page;
    auto* form = new QFormLayout(&page);
    ShmBinder binder(&hdr, &page);

    auto* enabled = binder.AddBool(form, "Enabled", &ShmHeader::enabled, "");
    auto* passes = binder.AddInt(form, "Passes", &ShmHeader::passes, 1, 30, "");
    auto* detail = binder.AddFloat(form, "Detail", &ShmHeader::transferStrengthBits, 0, 4, 0.05, "");
    auto* style = binder.AddChoice(form, "Style", &ShmHeader::style, { "a", "b", "c" }, "",
                                   ShmBinder::AtCreate);
    auto* scale = binder.AddPercent(form, "Scale", &ShmHeader::workingScaleBits, 25, 200, "");

    const uint32_t tune0 = hdr.tuningSeq.load();

    enabled->setChecked(false);
    check("bool reaches the header", hdr.enabled.load() == 0);

    passes->setValue(4);
    check("int reaches the header", hdr.passes.load() == 4);

    detail->setValue(2.5);
    check("float reaches the header", BitsToFloat(hdr.transferStrengthBits.load()) == 2.5f);

    style->setCurrentIndex(2);
    check("choice reaches the header", hdr.style.load() == 2);

    scale->setValue(150);
    check("percent stores a fraction", BitsToFloat(hdr.workingScaleBits.load()) == 1.5f);

    check("a create-time setting bumps tuningSeq", hdr.tuningSeq.load() > tune0);

    const uint32_t tune1 = hdr.tuningSeq.load();
    detail->setValue(1.0);
    check("a live setting does not bump tuningSeq", hdr.tuningSeq.load() == tune1);

    // The other direction: change the header underneath and reload.
    hdr.enabled.store(1);
    hdr.passes.store(7);
    hdr.transferStrengthBits.store(FloatToBits(3.25f));
    hdr.workingScaleBits.store(FloatToBits(0.75f));
    const uint32_t ctrl = hdr.controlSeq.load();
    binder.Reload();
    check("reload pulls bool", enabled->isChecked());
    check("reload pulls int", passes->value() == 7);
    check("reload pulls float", detail->value() == 3.25);
    check("reload pulls percent", scale->value() == 75);
    check("reload does not write back", hdr.controlSeq.load() == ctrl);

    std::printf("%s\n", failures ? "FAILURES" : "all binder paths ok");
    return failures ? 1 : 0;
}
