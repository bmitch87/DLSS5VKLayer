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
#include <QString>
#include <QStringList>
#include <functional>
#include <vector>

// Tooltips carry real prose, and a single unbroken sentence reads as one gigantic line that runs off
// the side of the screen. Callers write the natural break points as '\n'; this turns those into hard
// breaks the tooltip will honour, escaping the text first so a stray '&' or '<' in the prose cannot be
// mistaken for markup once the string becomes rich text. A tooltip with no '\n' is left as plain text,
// so short one-liners are untouched and single-line.
inline QString FormatTip(const QString& text) {
    if (text.isEmpty() || !text.contains(QLatin1Char('\n'))) return text;
    QString out = text;
    out.replace(QLatin1Char('&'), QLatin1String("&amp;"))
       .replace(QLatin1Char('<'), QLatin1String("&lt;"))
       .replace(QLatin1Char('>'), QLatin1String("&gt;"));
    out.replace(QLatin1String("\r\n"), QLatin1String("\n"))
       .replace(QLatin1Char('\r'), QLatin1String("\n"))
       .replace(QLatin1Char('\n'), QLatin1String("<br>"));
    return out;
}

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

    // Every control whose range does not contain the value ShmInitDefaults would put in its field.
    //
    // Three places state what a setting's sensible values are -- the header's default, this
    // control's bounds, and the entry in dlssnr-shmctl -- and nothing made them agree. When they
    // disagree the symptom is quiet and specific: the interface clamps a default it was never meant
    // to change, so a fresh profile silently becomes a different profile the first time the window
    // is opened, and "reset to defaults" produces something that is not the defaults.
    //
    // Checked against ShmInitDefaults rather than against the live header, because the live header
    // holds whatever the user last chose, and the question is about the defaults.
    const std::vector<QString>& DefaultProblems() const { return _defaultProblems; }

  private:
    void Write(Field field, uint32_t raw, Latch latch);
    // The value ShmInitDefaults puts in `field`. One scratch header, built once.
    uint32_t DefaultRaw(Field field) const;
    void NoteDefault(const QString& label, bool ok, const QString& detail);

    ShmHeader* _hdr = nullptr;
    QWidget* _parent = nullptr;
    std::vector<std::function<void()>> _reloaders;
    std::vector<QString> _defaultProblems;
    bool _reloading = false;
};
