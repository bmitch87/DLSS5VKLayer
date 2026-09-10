#include "mainwindow.h"
#include "passdialog.h"
#include "../common/runner_discovery.h"
#include "shm_binder.h"
#include "../layer_linux/src/hotkey.h"

#include <QAbstractItemView>
#include <QAction>
#include <QActionGroup>
#include <QCheckBox>
#include <QCloseEvent>
#include <QEvent>
#include <QMouseEvent>
#include <QComboBox>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontMetrics>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QIcon>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPixmap>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QSpinBox>
#include <QStandardPaths>
#include <QStyle>
#include <QStyleOptionComboBox>
#include <QTabWidget>
#include <QTextStream>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QGroupBox>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QVariantMap>
#include <QWidgetAction>

#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

// Hard ceiling on the profile combo's width, so even a capped name leaves the
// reload and Save buttons room in the row.
static constexpr int kProfileComboMaxWidth = 150;

// Size the profile combo to its current entry, capped so a long profile
// name can't crowd the buttons beside it.
static void fitProfileCombo(QComboBox* combo) {
    const QFontMetrics fm(combo->font());
    const int w = fm.horizontalAdvance(combo->currentText());
    QStyleOptionComboBox opt;
    opt.initFrom(combo);
    combo->setFixedWidth(qMin(
        combo->style()->sizeFromContents(QStyle::CT_ComboBox, &opt,
                                         QSize(w, fm.height()), combo).width(),
        kProfileComboMaxWidth));
}

// Pixels of text width the profile combo leaves for a name inside its cap.
static int profileTextPixelBudget(const QComboBox* combo) {
    const QFontMetrics fm(combo->font());
    QStyleOptionComboBox opt;
    opt.initFrom(const_cast<QComboBox*>(combo));
    const int chrome = combo->style()->sizeFromContents(
        QStyle::CT_ComboBox, &opt, QSize(0, fm.height()), combo).width();
    return qMax(fm.horizontalAdvance(QLatin1Char('W')), kProfileComboMaxWidth - chrome);
}

// Widen the dropdown so the longest entry is never elided: the popup's item rect
// loses a little room to the frame and margins, so reserve that. The reserve is
// deliberately small so the popup stays within the window's width.
static void fitProfilePopup(QComboBox* combo) {
    const QFontMetrics fm(combo->font());
    int longest = 0;
    for (int i = 0; i < combo->count(); ++i)
        longest = qMax(longest, fm.horizontalAdvance(combo->itemText(i)));
    combo->view()->setMinimumWidth(longest + 24);
}

static QIcon gearIcon(const QWidget* w) {
    QIcon icon = QIcon::fromTheme("preferences-system-symbolic", QIcon::fromTheme("preferences-system"));
    if (!icon.isNull()) return icon;

    QPixmap pm(32, 32);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.translate(16, 16);

    QColor c = w ? w->palette().color(QPalette::WindowText) : QColor(40, 40, 40);
    p.setPen(Qt::NoPen);
    p.setBrush(c);
    for (int i = 0; i < 8; ++i) {
        p.save();
        p.rotate(i * 45);
        p.drawRect(QRectF(-2.2, -15, 4.4, 7));
        p.restore();
    }
    p.drawEllipse(QPointF(0, 0), 10, 10);
    p.setCompositionMode(QPainter::CompositionMode_DestinationOut);
    p.drawEllipse(QPointF(0, 0), 4.5, 4.5);
    return QIcon(pm);
}

// Every header field the user can set, with the name it is stored under in config.ini. Floats are
// stored as the number they mean rather than their bit pattern, so the file stays readable and
// hand-editable. Deliberately absent: holdFrame and captureRequest, which are live testing controls
// rather than preferences, and everything either process writes about itself.
// invert: the config stores the positive reading of the field ("enabled") rather than the bit the
    // header holds, so the file says what the checkbox says.
struct SettingEntry {
    const char* key;
    ShmBinder::Field field;
    bool isFloat;
    bool invert = false;
};

static const SettingEntry kSettingsTable[] = {
    {"set_enabled", &ShmHeader::enabled, false},
    {"set_passes", &ShmHeader::passes, false},
    {"set_unlock_passes", &ShmHeader::unlockPasses, false},
    {"set_rebuild_settle_ms", &ShmHeader::rebuildSettleMs, false},
    {"set_model_resolution", &ShmHeader::workingScaleBits, true},
    {"set_down_leg_filter", &ShmHeader::scalingDownscaler, false},
    {"set_detail_strength", &ShmHeader::transferStrengthBits, true},
    {"set_colour_strength", &ShmHeader::colourStrengthBits, true},
    {"set_highlight_guard", &ShmHeader::maxRatioBits, true},
    {"set_enlargement", &ShmHeader::transfer, false},
    {"set_composition_enabled", &ShmHeader::compositionBypass, false, true},
    {"set_preset", &ShmHeader::preset, false},
    {"set_style", &ShmHeader::style, false},
    {"set_intensity", &ShmHeader::intensityBits, true},
    {"set_local_structure", &ShmHeader::localStructureBits, true},
    {"set_local_tone", &ShmHeader::localToneBits, true},
    {"set_skin_structure", &ShmHeader::skinStructureBits, true},
    {"set_auto_mask", &ShmHeader::autoMask, false},
    {"set_sharpness", &ShmHeader::sharpnessBits, true},
    {"set_motion_enabled", &ShmHeader::mvecEnabled, false},
    {"set_motion_quality", &ShmHeader::mvecQuality, false},
    {"set_motion_units", &ShmHeader::mvecScaleMode, false},
    {"set_colour_mode", &ShmHeader::colourMode, false},
    {"set_hdr_mode", &ShmHeader::hdrMode, false},
    {"set_white_point_source", &ShmHeader::whitePointSource, false},
    {"set_paper_white", &ShmHeader::whitePointBits, true},
    {"set_white_point_scale", &ShmHeader::whitePointScaleBits, true},
    {"set_white_point_trim", &ShmHeader::whitePointTrimBits, true},
    {"set_apply_model", &ShmHeader::applyModel, false},
    {"set_proxy", &ShmHeader::reversibleMode, false},
    {"set_debug_view", &ShmHeader::debugView, false},
    {"set_debug_scale", &ShmHeader::debugScaleBits, true},
    {"set_compare", &ShmHeader::compareMode, false},
    {"set_compare_split", &ShmHeader::compareSplitBits, true},
    {"set_compare_zoom", &ShmHeader::compareZoomBits, true},
    {"set_compare_swap", &ShmHeader::compareSwap, false},
    {"set_toggle_key", &ShmHeader::toggleKey, false},
};

static QString settingText(const SettingEntry& e, uint32_t raw) {
    if (e.invert) return QString::number(raw ? 0 : 1);
    return e.isFloat ? QString::number(BitsToFloat(raw), 'g', 9) : QString::number(raw);
}

// ── ProfileDelegate ──────────────────────────────────────────────────────
static const int kButtonDiameter = 14;

QRect ProfileDelegate::buttonRect(const QStyleOptionViewItem& option) const {
    const int r = kButtonDiameter / 2;
    return QRect(option.rect.right() - r * 2 - 2,
                 option.rect.center().y() - r,
                 kButtonDiameter, kButtonDiameter);
}

void ProfileDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option,
                            const QModelIndex& index) const {
    QStyledItemDelegate::paint(painter, option, index);
    if (index.row() == 0) return;

    if (!(option.state & QStyle::State_MouseOver)) return;

    const QRect br = buttonRect(option);
    painter->save();
    painter->setPen(option.palette.color(QPalette::Text));
    QFont xFont = option.font;
    xFont.setBold(true);
    painter->setFont(xFont);
    painter->drawText(br, Qt::AlignCenter, "x");
    painter->restore();
}

MainWindow::MainWindow(QWidget* parent) : QWidget(parent) {
    setWindowTitle("DLSS5VKLayer Helper");

    projectDir = findProjectDir();
    helperCliPath = findHelperCli();
    configFilePath = configPath();
    loadConfig();
    resize(windowW, windowH);
    setMinimumSize(520, 480);

    // Softened on purpose: a full-strength palette border behind every group and tab reads as a
    // wireframe over the controls. A translucent hairline and a quiet selected-tab pill carry the
    // structure without competing with the settings for attention.
    setStyleSheet(
        "QGroupBox { font-weight: 600; margin-top: 14px; padding-top: 6px; border: 1px solid rgba(128, 128, 128, 0.25); border-radius: 6px; }"
        "QGroupBox::title { subcontrol-origin: margin; left: 8px; padding: 0 4px; }"
        "QTabWidget::pane { border: none; border-top: 1px solid rgba(128, 128, 128, 0.25); }"
        "QTabBar::tab { padding: 5px 14px; border: none; background: transparent; }"
        "QTabBar::tab:selected { background: rgba(128, 128, 128, 0.22); border-radius: 4px; }"
        "QTabBar::tab:hover:!selected { background: rgba(128, 128, 128, 0.10); border-radius: 4px; }");

    // The environment wins over the stored config, which is the order the layer and the helper both
    // use -- they read DLSSNR_SHM first and fall back. Having the interface do the opposite meant
    // pointing everything at one mapping and watching the interface report on another, with the path
    // it was actually using printed on screen the whole time.
    const QString shmEnv = qEnvironmentVariable("DLSSNR_SHM");
    if (!shmEnv.isEmpty()) shmPath = shmEnv;
    if (shmPath.isEmpty()) shmPath = defaultShmPath();

    const QString logEnv = qEnvironmentVariable("DLSSNR_LOG");
    if (!logEnv.isEmpty()) logPath = logEnv;
    if (logPath.isEmpty()) logPath = defaultLogPath();
    ensureShm();
    restoreSettings();

    auto* root = new QVBoxLayout(this);

    statusLabel = new QLabel(this);
    statusLabel->setTextFormat(Qt::RichText);
    root->addWidget(statusLabel);

    auto* runnerForm = new QFormLayout;
    runnerCombo = new QComboBox(this);
    runnerPathEdit = new QLineEdit(this);
    browseRunnerBtn = new QPushButton("Browse...", this);
    auto* runnerPathRow = new QHBoxLayout;
    runnerPathRow->addWidget(runnerPathEdit);
    runnerPathRow->addWidget(browseRunnerBtn);
    // Said plainly, because the obvious reading is the wrong one: this is what the *helper* runs
    // under, not what the game runs under. The helper is a Windows executable -- it has to be, the
    // model is a Windows DLL -- so it needs Proton or Wine whatever the game is. A native Linux game
    // is unaffected by this setting; the layer inside it is a native library either way.
    runnerCombo->setToolTip(FormatTip(
        "Proton or Wine for the helper process, which is a Windows executable\n"
        "because the model is a Windows DLL.\n"
        "This is not what the game runs under -- native Linux games work with this set too."));
    runnerPathEdit->setToolTip(runnerCombo->toolTip());
    runnerForm->addRow("Runner", runnerCombo);
    runnerForm->addRow("Path", runnerPathRow);
    root->addLayout(runnerForm);

    auto* buttons = new QHBoxLayout;
    startBtn = new QPushButton("Start helper", this);
    stopBtn = new QPushButton("Stop helper", this);
    profileCombo = new QComboBox(this);
    profileCombo->setToolTip("Select a saved profile to load, or choose '(default)' to reset.");
    profileReloadBtn = new QToolButton(this);
    profileReloadBtn->setText(QString::fromUtf8("\xe2\x86\xbb"));  // "clockwise open circle arrow"
    profileReloadBtn->setToolTip("Settings changed since this selection was loaded. Click to reload it.");
    profileSaveBtn = new QPushButton("Save", this);
    profileSaveBtn->setToolTip("Save current settings to the selected profile.");
    profileReloadBtn->setVisible(false);
    buttons->addWidget(startBtn);
    buttons->addWidget(stopBtn);
    buttons->addStretch(1);
    buttons->addWidget(profileReloadBtn);
    buttons->addWidget(profileCombo);
    buttons->addWidget(profileSaveBtn);
    root->addLayout(buttons);
    connect(profileReloadBtn, &QToolButton::clicked, this, [this] {
        const int idx = profileCombo->currentIndex();
        if (idx > 0) loadSettingsFromFile(profileCombo->itemData(idx).toString());
        else applyDefaults();
    });

    // Install custom delegate that renders a ✕ button next to each profile.
    auto* delegate = new ProfileDelegate(this);
    profileCombo->setItemDelegate(delegate);

    // The QComboBox popup is a QAbstractItemView whose viewport handles the mouse.
    // We install an event filter there so we can detect clicks on the ✕ button.
    auto* listView = profileCombo->view();
    listView->viewport()->installEventFilter(this);

    refreshProfileList();
    // Re-fit once the window is shown: the font (and thus the metrics) the style
    // settles on can differ from the app default used during construction.
    QTimer::singleShot(0, this, [this] {
        fitProfileCombo(profileCombo);
        const int h = profileSaveBtn->height();
        profileCombo->setFixedHeight(h);
        profileReloadBtn->setFixedHeight(h);
    });

    root->addWidget(buildSettings(), 1);

    // The gear, bottom right: the things that act on the whole interface rather than one setting.
    //
    // Icon only, no arrow section: the menu opens attached under the button the way a submenu hangs off
    // a menu bar, not at the cursor like a context menu.
    gearBtn = new QToolButton(this);
    gearBtn->setIcon(gearIcon(this));
    gearBtn->setToolTip("Settings");
    gearBtn->setPopupMode(QToolButton::InstantPopup);
    auto* gearMenu = new QMenu(gearBtn);

    // Rebuild spacing lives here rather than on the Rendering tab: it is a knob for how the helper
    // behaves, not a setting about the picture, and it earns its keep exactly when a chain of
    // rebuilds has wedged the model -- which is not when you want to be digging through the tab.
    auto* rebuildAction = new QWidgetAction(gearMenu);
    auto* rebuildRow = new QWidget(gearBtn);
    auto* rebuildLay = new QHBoxLayout(rebuildRow);
    rebuildLay->setContentsMargins(8, 4, 8, 4);
    auto* rebuildLabel = new QLabel("Rebuild spacing (ms)", rebuildRow);
    rebuildSpin = new QSpinBox(rebuildRow);
    rebuildSpin->setRange(0, 5000);
    rebuildSpin->setToolTip(FormatTip(
        "How long the helper waits after a model setting changes before it rebuilds the pass,\n"
        "and between one rebuild and the next.\n"
        "Rebuilding is expensive, and back-to-back rebuilds have been seen to wedge the model\n"
        "on some drivers.\n"
        "Lower is snappier; 0 rebuilds immediately and chains the rest back to back.\n"
        "Raise it if the model ever stops answering after changing settings."));
    rebuildLabel->setToolTip(rebuildSpin->toolTip());
    if (hdr) rebuildSpin->setValue(int(hdr->rebuildSettleMs.load()));
    rebuildLay->addWidget(rebuildLabel);
    rebuildLay->addWidget(rebuildSpin);
    rebuildAction->setDefaultWidget(rebuildRow);
    gearMenu->addAction(rebuildAction);
    gearMenu->addSeparator();
    gearMenu->addAction("Reset all settings...", this, &MainWindow::resetAllSettings);
    gearMenu->addSeparator();
    gearMenu->addAction("Open helper log", this, [this] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(logPath));
    });

    // The NVIDIA NGX DLLs, tucked into the settings menu rather than crowding the top of the window.
    // The path shows as a disabled header so the folder the helper reads stays discoverable; it refreshes
    // after an import. These are NVIDIA's proprietary files and are not shipped, so Import copies the
    // ones the user already owns into that folder -- exactly as `dlssnr-helper import-binaries` does --
    // and the helper has to be restarted afterwards to load them.
    gearMenu->addSeparator();
    auto* binariesMenu = gearMenu->addMenu("NGX binaries");
    binariesPathAction = binariesMenu->addAction(effectiveBinariesDir());
    binariesPathAction->setEnabled(false);
    binariesPathAction->setToolTip(FormatTip(
        "Where the NVIDIA NGX DLLs live. The helper loads nvngx_dlssnr.dll from here.\n"
        "These are NVIDIA's proprietary files and are not shipped with this package -- import your own."));
    auto* importAction = binariesMenu->addAction("Import binaries...", this, &MainWindow::importBinaries);
    importAction->setToolTip(FormatTip(
        "Copy the NVIDIA NGX DLLs from a folder you choose into the binaries folder.\n"
        "Pick the folder that holds nvngx_dlssnr.dll (and nvngx.dll, nvapi64.dll, sl.*.dll if you have "
        "them).\n"
        "Restart the helper afterwards so it loads the new files."));
    auto* openBinariesAction = binariesMenu->addAction("Open binaries folder", this, &MainWindow::openBinariesFolder);
    openBinariesAction->setToolTip("Open the folder the helper loads the NGX DLLs from.");
    gearBtn->setMenu(gearMenu);
    connect(rebuildSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int v) {
        if (!hdr) return;
        hdr->rebuildSettleMs.store(uint32_t(v));
        hdr->controlSeq.fetch_add(1);
    });
    auto* bottom = new QHBoxLayout;
    bottom->addStretch(1);
    bottom->addWidget(gearBtn);
    root->addLayout(bottom);

    connect(startBtn, &QPushButton::clicked, this, &MainWindow::startHelper);
    connect(stopBtn, &QPushButton::clicked, this, &MainWindow::stopHelper);
    connect(profileSaveBtn, &QPushButton::clicked, this, &MainWindow::saveSettingsToFile);
    connect(profileCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int idx) {
        fitProfileCombo(profileCombo);
        lastProfilePath = (idx == 0) ? QString() : profileCombo->itemData(idx).toString();
        if (idx == 0) { applyDefaults(); return; }  // "(default)" — reset to defaults
        const QString path = profileCombo->itemData(idx).toString();
        loadSettingsFromFile(path);
    });
    connect(runnerCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::applyRunnerSelection);
    connect(runnerPathEdit, &QLineEdit::editingFinished, this, [this] {
        runnerPath = runnerPathEdit->text().trimmed();
        if (!runnerPath.isEmpty()) {
            runnerType = runnerPath.contains("proton", Qt::CaseInsensitive) ? "proton" : "wine";
            saveConfig();
        }
    });
    connect(browseRunnerBtn, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(this, "Select runner", QDir::homePath());
        if (path.isEmpty()) return;
        runnerPath = path;
        runnerType = runnerPath.contains("proton", Qt::CaseInsensitive) ? "proton" : "wine";
        runnerPathEdit->setText(runnerPath);
        saveConfig();
    });
    populateRunners();

    statusTimer = new QTimer(this);
    connect(statusTimer, &QTimer::timeout, this, &MainWindow::updateStatus);
    statusTimer->start(1000);
    updateStatus();

    // Ask for the NGX DLLs the first thing after the window is up, if they are not there -- but on the
    // next turn of the event loop, not inline, so the main window paints before a modal dialog covers
    // it. A fresh install with no nvngx_dlssnr.dll would otherwise open on a helper that can only ever
    // report "no NGX binaries" with no hint of what to do about it.
    QTimer::singleShot(0, this, &MainWindow::maybePromptImport);
}

MainWindow::~MainWindow() {
    if (statusTimer) statusTimer->stop();
    if (shmBase) munmap(shmBase, ShmTotalBytes());
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (statusTimer) statusTimer->stop();
    if (hdr) hdr->quit.store(1);
    windowW = width();
    windowH = height();
    saveConfig();
    if (helperCliPath.isEmpty()) helperCliPath = findHelperCli();
    if (!helperCliPath.isEmpty()) QProcess::startDetached(helperCliPath, {"stop"});
    QWidget::closeEvent(event);
}

bool MainWindow::eventFilter(QObject* obj, QEvent* event) {
    // Intercept mouse release on the profile combo box's list view to detect
    // clicks on the small ✕ delete button painted by ProfileDelegate.
    if (event->type() != QEvent::MouseButtonRelease) return false;

    auto* view = profileCombo->view();
    if (obj != view->viewport()) return false;

    const auto* me = static_cast<QMouseEvent*>(event);
    const QPoint vpPos = me->pos();
    const QModelIndex idx = view->indexAt(vpPos);
    if (idx.isValid() && idx.row() > 0) {
        // Build the same button rect the delegate uses.
        QStyleOptionViewItem opt;
        opt.rect = view->visualRect(idx);
        const int r = kButtonDiameter / 2;
        const QRect br(opt.rect.right() - r * 2 - 2,
                       opt.rect.center().y() - r,
                       kButtonDiameter, kButtonDiameter);
        if (br.contains(vpPos)) {
            const QString path = profileCombo->itemData(idx.row()).toString();
            const QString name = profileCombo->itemText(idx.row());
            if (path.isEmpty()) return false;

            const int ret = QMessageBox::question(
                this, "Delete profile",
                QString("Delete profile \"%1\"?").arg(name),
                QMessageBox::Yes | QMessageBox::No);
            if (ret != QMessageBox::Yes) return false;

            if (!QFile::remove(path)) {
                QMessageBox::warning(this, "DLSS5VKLayer",
                                     "Could not delete:\\n" + path);
                return false;
            }
            if (lastProfilePath == path) {
                applyDefaults();
            }
            refreshProfileList();
            return true;
        }
    }
    return false;
}

QString MainWindow::findProjectDir() const {
    QStringList candidates;
    candidates << QDir::currentPath() << QCoreApplication::applicationDirPath();
    QDir d(QCoreApplication::applicationDirPath());
    for (int i = 0; i < 6; ++i) {
        candidates << d.absolutePath();
        if (!d.cdUp()) break;
    }
    for (const QString& c : candidates) {
        if (QFile::exists(c + "/build/dlssnr_helper.exe")) return c;
    }
    return QDir::currentPath();
}

QString MainWindow::findHelperCli() const {
    const QString env = qEnvironmentVariable("DLSSNR_HELPER_CLI");
    if (!env.isEmpty() && QFile::exists(env)) return env;

    const QString found = QStandardPaths::findExecutable("dlssnr-helper");
    if (!found.isEmpty()) return found;

    const QStringList candidates = {
        projectDir + "/dlssnr-helper",
        QDir::homePath() + "/.local/bin/dlssnr-helper",
        "/usr/bin/dlssnr-helper",
        "/usr/local/bin/dlssnr-helper"
    };
    for (const QString& c : candidates) {
        if (QFile::exists(c)) return c;
    }
    return QString();
}

QString MainWindow::configPath() const {
    const QString base = qEnvironmentVariable("XDG_CONFIG_HOME", QDir::homePath() + "/.config");
    return base + "/dlssnr/config.ini";
}

// Returns ~/.config/dlssnr/profiles/ for saving/loading setting profiles.
QString MainWindow::profilesDir() const {
    const QString dir = qEnvironmentVariable("XDG_CONFIG_HOME", QDir::homePath() + "/.config")
                        + "/dlssnr/profiles";
    QDir().mkpath(dir);
    return dir;
}

// Must match DATA_DIR in dlssnr-helper, so the GUI and the CLI import into and read from the same
// folder -- otherwise the interface would report binaries imported while the helper looked elsewhere.
QString MainWindow::dataDir() const {
    const QString base = qEnvironmentVariable("XDG_DATA_HOME", QDir::homePath() + "/.local/share");
    return base + "/dlssnr";
}

QString MainWindow::defaultBinariesDir() const {
    return dataDir() + "/binaries";
}

// The folder the helper will actually read: whatever the config recorded, or the default when nothing
// was ever imported. The import always writes into defaultBinariesDir(), so a config path that points
// elsewhere is only ever a hand-edit or a leftover from the CLI's discovery.
QString MainWindow::effectiveBinariesDir() const {
    return binariesPath.isEmpty() ? defaultBinariesDir() : binariesPath;
}

// nvngx_dlssnr.dll is the one file the helper cannot work without -- neural processing stays disabled
// without it. nvngx.dll and nvapi64.dll are optional (the parameter allocator falls back, and under
// Proton DXVK-NVAPI supplies NVAPI), so their absence is not worth a prompt.
bool MainWindow::binariesMissingNow() const {
    return !QFile::exists(effectiveBinariesDir() + "/nvngx_dlssnr.dll");
}

QString MainWindow::defaultShmPath() const {
    // Shared with the layer and the helper -- see ShmDefaultPath() for why it is not $XDG_RUNTIME_DIR.
    return QString::fromStdString(ShmDefaultPath());
}

QString MainWindow::defaultLogPath() const {
    const QString base = qEnvironmentVariable("XDG_STATE_HOME", QDir::homePath() + "/.local/state");
    return base + "/dlssnr/helper.log";
}

// The same file the CLI's running_pid() checks: the PID the launcher wrote, alive, and still the
// helper. A PID alone is not enough -- PIDs are recycled -- and the shared header's state field is
// not either, because a helper that died mid-run leaves whatever state it last published behind.
QString MainWindow::pidFilePath() const {
    return QFileInfo(shmPath).absolutePath() + "/helper.pid";
}

bool MainWindow::helperRunningNow() const {
    QFile pidFile(pidFilePath());
    if (!pidFile.open(QIODevice::ReadOnly)) return false;
    const QByteArray pid = pidFile.readAll().trimmed();
    if (pid.isEmpty()) return false;

    bool ok = false;
    const int p = pid.toInt(&ok);
    if (!ok || p <= 0) return false;
    if (kill(pid_t(p), 0) != 0) return false;

    QFile cmd(QString("/proc/%1/cmdline").arg(p));
    if (cmd.open(QIODevice::ReadOnly)) {
        const QByteArray c = cmd.readAll();
        if (!c.contains("dlssnr_helper.exe")) return false;
    }
    return true;
}

void MainWindow::loadConfig() {
    QFile f(configFilePath);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return;

    QTextStream in(&f);
    while (!in.atEnd()) {
        QString line = in.readLine().trimmed();
        if (line.isEmpty() || line.startsWith('#') || !line.contains('=')) continue;
        const QString key = line.section('=', 0, 0).trimmed();
        QString value = line.section('=', 1).trimmed();
        if (value.size() >= 2 && value.startsWith('"') && value.endsWith('"')) {
            value = value.mid(1, value.size() - 2);
        }
        if (key == "runner_type") runnerType = value;
        else if (key == "runner_path") runnerPath = value;
        else if (key == "binaries") binariesPath = value;
        // A config written by an older build pins the mapping to $XDG_RUNTIME_DIR, which is exactly
        // the path a Steam game cannot see. Treat that one value as if it had never been written.
        else if (key == "shm") {
            const QString legacy = qEnvironmentVariable("XDG_RUNTIME_DIR") + "/dlssnr/shm.bin";
            shmPath = (value == legacy) ? QString() : value;
        }
        else if (key == "log") logPath = value;
        else if (key == "dxvk_vendor") dxvkVendor = value;
        else if (key == "dxvk_device") dxvkDevice = value;
        else if (key == "window_width") windowW = value.toInt();
        else if (key == "window_height") windowH = value.toInt();
        else if (key == "profile") lastProfilePath = value;
        else if (key.startsWith("set_")) pendingSettings.append({key, value});
    }
}

void MainWindow::saveConfig() {
    const QFileInfo info(configFilePath);
    QDir().mkpath(info.absolutePath());

    QFile f(configFilePath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) return;

    QTextStream out(&f);
    out << "runner_type=" << runnerType << "\n";
    out << "runner_path=" << runnerPath << "\n";
    out << "binaries=" << binariesPath << "\n";
    out << "shm=" << shmPath << "\n";
    out << "log=" << logPath << "\n";
    out << "dxvk_vendor=" << dxvkVendor << "\n";
    out << "dxvk_device=" << dxvkDevice << "\n";
    out << "window_width=" << (isVisible() ? width() : windowW) << "\n";
    out << "window_height=" << (isVisible() ? height() : windowH) << "\n";
    out << "profile=" << lastProfilePath << "\n";
    if (hdr) {
        for (const SettingEntry& e : kSettingsTable)
            out << e.key << "=" << settingText(e, (hdr->*e.field).load()) << "\n";
    }
}

// Put the saved settings into the header before any widget reads it, so the interface opens on what
// the last session left rather than on the built-in defaults. The header may have been re-initialised
// a moment ago (a version bump does that), which is exactly when this matters most.
void MainWindow::restoreSettings() {
    if (!hdr) {
        pendingSettings.clear();
        return;
    }
    bool touched = false;
    for (const SettingEntry& e : kSettingsTable) {
        for (const auto& kv : pendingSettings) {
            if (kv.first != QLatin1String(e.key)) continue;
            const uint32_t raw = e.invert ? (kv.second.toUInt() ? 0u : 1u)
                                          : (e.isFloat ? FloatToBits(kv.second.toFloat())
                                                       : kv.second.toUInt());
            (hdr->*e.field).store(raw);
            touched = true;
            break;
        }
    }
    pendingSettings.clear();
    if (touched) {
        hdr->controlSeq.fetch_add(1);
        hdr->tuningSeq.fetch_add(1);
    }
    lastSettingsBlob = settingsBlob();
}

QString MainWindow::settingsBlob() const {
    QString b;
    if (!hdr) return b;
    for (const SettingEntry& e : kSettingsTable)
        b += QString("%1=%2;").arg(QString::fromLatin1(e.key), settingText(e, (hdr->*e.field).load()));
    return b;
}

// The interface polls the header every second anyway; comparing the settings against the last saved
// snapshot is the cheapest way to notice a change made by anything -- this window, shmctl, a script
// -- and persist it without waiting for the window to close.
void MainWindow::saveSettingsIfChanged() {
    const QString b = settingsBlob();
    if (b == lastSettingsBlob) return;
    lastSettingsBlob = b;
    saveConfig();
    updateReloadBtn();
}

// The reload button exists only while the live settings have drifted from what
// the selection stands for -- the saved profile file, or the factory defaults
// when "(default)" is selected -- the state where re-loading is meaningful.
void MainWindow::updateReloadBtn() {
    if (!profileReloadBtn) return;
    if (!hdr) { profileReloadBtn->setVisible(false); return; }
    const QString& baseline = profileCombo->currentIndex() > 0 ? profileBlob : defaultsBlob;
    profileReloadBtn->setVisible(!baseline.isEmpty() && settingsBlob() != baseline);
}

void MainWindow::resetAllSettings() {
    if (QMessageBox::question(this, "DLSS5VKLayer",
                              "Reset every setting to its default? The helper will rebuild its "
                              "features from the defaults.") != QMessageBox::Yes)
        return;
    applyDefaults();
}

// Apply factory defaults directly (no confirmation dialog).
void MainWindow::applyDefaults() {
    if (!hdr) return;
    ShmInitDefaults(hdr);
    hdr->controlSeq.fetch_add(1);
    hdr->tuningSeq.fetch_add(1);
    if (keyCombo) keyCombo->setCurrentIndex(0);
    if (binder) binder->Reload();
    if (rebuildSpin) {
        QSignalBlocker block(rebuildSpin);
        rebuildSpin->setValue(int(hdr->rebuildSettleMs.load()));
    }
    updateCompositionVisibility();
    lastSettingsBlob = settingsBlob();
    profileBlob.clear();
    defaultsBlob = lastSettingsBlob;
    updateReloadBtn();
    saveConfig();
}

// Refresh the profile combo box with all .ini files in the profiles directory.
void MainWindow::refreshProfileList() {
    const QSignalBlocker blocker(profileCombo);
    profileCombo->clear();
    profileCombo->addItem("(default)", QVariant());

    const QDir dir(profilesDir());
    const QStringList files = dir.entryList(QDir::Files | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString& f : files) {
        if (f.endsWith(".ini", Qt::CaseInsensitive))
            profileCombo->addItem(f.left(f.size() - 4), dir.absoluteFilePath(f));
    }
    // Reopen on the profile the last session had selected. The signal is blocked here, so this
    // only moves the dropdown; the settings themselves come back via the config's set_* keys.
    const int saved = lastProfilePath.isEmpty() ? -1 : profileCombo->findData(lastProfilePath);
    if (saved > 0) {
        profileCombo->setCurrentIndex(saved);
        // The config's set_* keys are the profile's values as the last session left
        // them, so the reopened session starts in sync with the profile.
        profileBlob = settingsBlob();
    } else {
        defaultsBlob = settingsBlob();
    }
    fitProfileCombo(profileCombo);
    fitProfilePopup(profileCombo);
}

// Save settings: always prompt for a profile name, pre-populated with the selected profile.
void MainWindow::saveSettingsToFile() {
    if (!hdr) {
        QMessageBox::warning(this, "DLSS5VKLayer", "Shared memory not attached yet.");
        return;
    }

    // Pre-populate with the current profile name; empty when "(default)" is selected.
    const QString prompt = (profileCombo->currentIndex() == 0)
                           ? "" : profileCombo->currentText();

    QInputDialog dlg(this);
    dlg.setWindowTitle("Save profile");
    dlg.setLabelText("Profile name:");
    dlg.setInputMode(QInputDialog::TextInput);
    dlg.setTextValue(prompt);
    if (auto* edit = dlg.findChild<QLineEdit*>()) {
        // Cap by rendered width, not character count, and by whichever box is
        // tighter: the entry field itself or the combo's 200px budget.
        const QFontMetrics fm(edit->font());
        const int comboAvail = profileTextPixelBudget(profileCombo);
        edit->setMaxLength(200);
        QString accepted = edit->text();
        connect(edit, &QLineEdit::textChanged, edit,
                [edit, fm, comboAvail, accepted](const QString& t) mutable {
            const int avail = qMin(edit->contentsRect().width() - 8, comboAvail);
            if (fm.horizontalAdvance(t) > avail) {
                const QSignalBlocker block(edit);
                edit->setText(accepted);
                edit->end(false);
                return;
            }
            accepted = t;
        });
    }
    if (dlg.exec() != QDialog::Accepted) return;
    const QString name = dlg.textValue();
    if (name.trimmed().isEmpty()) return;

    QString fileName = name.trimmed();
    if (!fileName.endsWith(".ini", Qt::CaseInsensitive))
        fileName += ".ini";
    const QString path = profilesDir() + "/" + fileName;

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        QMessageBox::warning(this, "DLSS5VKLayer",
                             "Could not write to:\n" + path);
        return;
    }
    QTextStream out(&f);
    out << "# DLSS5VKLayer settings\n";
    out << "# Generated by dlssnr_gui\n\n";
    for (const SettingEntry& e : kSettingsTable)
        out << e.key << "=" << settingText(e, (hdr->*e.field).load()) << "\n";

    // Add or update the profile in the dropdown and select it. The dropdown shows the
// bare name; the ".ini" suffix lives only in the file on disk.
    const QString display = fileName.left(fileName.size() - 4);
    int existing = profileCombo->findText(display);
    if (existing < 0) {
        profileCombo->addItem(display, path);
        profileCombo->setCurrentIndex(profileCombo->count() - 1);
    } else {
        profileCombo->setItemData(existing, path);
        profileCombo->setCurrentIndex(existing);
    }
    fitProfileCombo(profileCombo);
    fitProfilePopup(profileCombo);
    // The file now holds exactly what is live, profile and settings in sync.
    profileBlob = settingsBlob();
    updateReloadBtn();
}

// Load settings from a specific file path and apply them immediately.
void MainWindow::loadSettingsFromFile(const QString& path) {
    if (!hdr) {
        QMessageBox::warning(this, "DLSS5VKLayer", "Shared memory not attached yet.");
        return;
    }

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QMessageBox::warning(this, "DLSS5VKLayer",
                             "Could not read:\n" + path);
        return;
    }

    QTextStream in(&f);
    int loaded = 0, skipped = 0;
    while (!in.atEnd()) {
        QString line = in.readLine().trimmed();
        if (line.isEmpty() || line.startsWith('#') || !line.contains('=')) continue;
        const QString key = line.section('=', 0, 0).trimmed();
        const QString value = line.section('=', 1).trimmed();
        bool found = false;
        for (const SettingEntry& e : kSettingsTable) {
            if (QLatin1String(e.key) != key) continue;
            const uint32_t raw = e.invert ? (value.toUInt() ? 0u : 1u)
                                          : (e.isFloat ? FloatToBits(value.toFloat())
                                                       : value.toUInt());
            (hdr->*e.field).store(raw);
            found = true;
            ++loaded;
            break;
        }
        if (!found) ++skipped;
    }

    hdr->controlSeq.fetch_add(1);
    hdr->tuningSeq.fetch_add(1);
    if (binder) binder->Reload();
    updateCompositionVisibility();
    lastSettingsBlob = settingsBlob();
    profileBlob = lastSettingsBlob;
    updateReloadBtn();
    saveConfig();
}

void MainWindow::populateRunners() {
    const QSignalBlocker blocker(runnerCombo);
    runnerCombo->clear();

    const auto runners = dlssnr::discoverCustomRunners();
    for (const auto& r : runners) {
        QVariantMap data;
        data["type"] = "proton";
        data["path"] = QString::fromStdString(r.path);
        runnerCombo->addItem(QString::fromStdString(dlssnr::runnerDisplayName(r)), data);
    }

    const QString wine = QStandardPaths::findExecutable("wine");
    if (!wine.isEmpty()) {
        QVariantMap data;
        data["type"] = "wine";
        data["path"] = wine;
        runnerCombo->addItem("System Wine", data);
    }

    bool selected = false;
    for (int i = 0; i < runnerCombo->count(); ++i) {
        if (runnerCombo->itemData(i).toMap().value("path").toString() == runnerPath) {
            runnerCombo->setCurrentIndex(i);
            selected = true;
            break;
        }
    }
    if (!selected && !runnerPath.isEmpty()) {
        QVariantMap data;
        data["type"] = runnerPath.contains("proton", Qt::CaseInsensitive) ? "proton" : "wine";
        data["path"] = runnerPath;
        runnerCombo->addItem("Custom: " + runnerPath, data);
        runnerCombo->setCurrentIndex(runnerCombo->count() - 1);
        selected = true;
    }
    if (!selected && runnerCombo->count() > 0) {
        runnerCombo->setCurrentIndex(0);
        applyRunnerSelection(0);
    }

    runnerPathEdit->setText(runnerPath);
}

void MainWindow::applyRunnerSelection(int index) {
    if (index < 0) return;
    const QVariantMap data = runnerCombo->itemData(index).toMap();
    runnerType = data.value("type").toString();
    runnerPath = data.value("path").toString();
    runnerPathEdit->setText(runnerPath);
    saveConfig();
}

bool MainWindow::ensureShm() {
    if (hdr) return true;
    // The launcher creates this directory too; doing it here as well means the interface can open
    // and show its settings before anything has ever been started, rather than reporting "shared
    // memory not attached" on a machine where the runtime dir simply does not exist yet.
    QDir().mkpath(QFileInfo(shmPath).absolutePath());
    const QByteArray p = shmPath.toUtf8();
    int fd = open(p.constData(), O_RDWR | O_CREAT, 0666);
    if (fd < 0) return false;
    const size_t total = ShmTotalBytes();
    struct stat st{};
    if (fstat(fd, &st) != 0 || size_t(st.st_size) < total) {
        if (ftruncate(fd, off_t(total)) != 0) {
            ::close(fd);
            return false;
        }
    }
    void* m = mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ::close(fd);
    if (m == MAP_FAILED) return false;
    shmBase = m;
    hdr = (ShmHeader*)m;
    if (hdr->magic.load() != kShmMagic || hdr->version.load() != kShmVersion ||
        hdr->passes.load() == 0)
        ShmInitDefaults(hdr);
    return true;
}

void MainWindow::startHelper() {
    if (helperCliPath.isEmpty()) helperCliPath = findHelperCli();
    if (helperCliPath.isEmpty()) {
        QMessageBox::warning(this, "DLSS5VKLayer", "dlssnr-helper CLI not found.");
        return;
    }
    if (!ensureShm()) {
        QMessageBox::warning(this, "DLSS5VKLayer", "Could not open shared memory file.");
        return;
    }
    if (hdr) hdr->quit.store(0);
    saveConfig();
    QProcess::startDetached(helperCliPath, {"start"});
    // The poll confirms it a second later; greying the button now stops a second click from looking
    // ignored while the launcher is still bringing the prefix up.
    startBtn->setEnabled(false);
    updateStatus();
}

void MainWindow::stopHelper() {
    if (helperCliPath.isEmpty()) helperCliPath = findHelperCli();
    if (hdr) hdr->quit.store(1);
    if (!helperCliPath.isEmpty()) {
        QProcess::startDetached(helperCliPath, {"stop"});
    }
    stopBtn->setEnabled(false);
    updateStatus();
}

// The GUI twin of `dlssnr-helper import-binaries`: copy the NVIDIA NGX DLLs the user already owns out
// of a folder they pick into the one folder the helper reads, then record it. Doing the copy here
// rather than shelling to the CLI means it works even when the CLI is not on PATH, and lets the result
// be reported per file instead of a single line on stdout. The helper reads the folder only at launch,
// so a running helper has to be restarted to see a fresh import.
void MainWindow::importBinaries() {
    const QString current = effectiveBinariesDir();
    const QString startDir = QDir(current).exists() ? current : QDir::homePath();
    const QString src = QFileDialog::getExistingDirectory(
        this, "Select the folder holding the NVIDIA NGX DLLs", startDir);
    if (src.isEmpty()) return;

    const QString dest = defaultBinariesDir();
    if (!QDir().mkpath(dest)) {
        QMessageBox::warning(this, "DLSS5VKLayer",
                             FormatTip("Could not create the binaries folder:\n" + dest));
        return;
    }

    // The same set the CLI copies. nvngx_dlssnr.dll is the only one the helper insists on; nvngx.dll
    // and nvapi64.dll serve the core/NVAPI path, sl.*.dll a Streamline route this build does not use,
    // and *.license.txt is NVIDIA's EULA carried along for redistribution.
    static const QStringList patterns = {
        "nvngx_dlssnr.dll", "nvngx.dll", "nvapi64.dll", "sl.*.dll", "*.license.txt"
    };
    QDir srcDir(src);
    QStringList copied, failed;
    for (const QString& pat : patterns) {
        const QStringList hits = srcDir.entryList(QStringList() << pat, QDir::Files, QDir::Name);
        for (const QString& name : hits) {
            const QString target = dest + "/" + name;
            if (QFile::exists(target)) QFile::remove(target);
            if (QFile::copy(src + "/" + name, target)) copied << name;
            else failed << name;
        }
    }

    if (copied.isEmpty()) {
        QMessageBox::warning(this, "DLSS5VKLayer",
                             FormatTip("No NVIDIA NGX DLLs were found in:\n" + src +
                                       "\n\nPick the folder that actually contains nvngx_dlssnr.dll."));
        return;
    }

    binariesPath = dest;
    if (binariesPathAction) binariesPathAction->setText(dest);
    saveConfig();

    const bool hasSnippet = QFile::exists(dest + "/nvngx_dlssnr.dll");
    QString detail = "Imported into:\n" + dest + "\n\n" + copied.join("\n");
    if (!failed.isEmpty()) detail += "\n\nCould not copy:\n" + failed.join("\n");
    if (!hasSnippet)
        detail += "\n\nnvngx_dlssnr.dll was not among them -- neural processing stays disabled.";
    else if (helperRunning)
        detail += "\n\nThe helper is running; restart it to load the new files.";
    else
        detail += "\n\nStart the helper to use them.";

    if (failed.isEmpty() && hasSnippet)
        QMessageBox::information(this, "DLSS5VKLayer", FormatTip(detail));
    else
        QMessageBox::warning(this, "DLSS5VKLayer", FormatTip(detail));
}

void MainWindow::openBinariesFolder() {
    const QString dir = effectiveBinariesDir();
    QDir().mkpath(dir);  // opening a folder that does not exist yet just fails; create it first
    QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
}

// The first-run nudge. With no nvngx_dlssnr.dll the helper can only report "no NGX binaries", which is
// a dead end nothing on the status line explains to someone who has never imported the DLLs. Offer to
// fix it the moment the window is up. It comes back every launch while the snippet is missing -- there
// is deliberately no "don't ask again", because the state that triggers it is one the user can resolve.
void MainWindow::maybePromptImport() {
    if (!binariesMissingNow()) return;
    const QString dir = effectiveBinariesDir();

    // A hand-built dialog rather than a QMessageBox: QMessageBox lays its buttons out by role per the
    // active style, which kept scattering them. A plain dialog with an explicit button row pins the
    // KDE / FreeDesktop convention -- dismiss on the far left, affirmative actions grouped on the right,
    // the primary one (Import) as the default focus on the far right -- and lets "Open folder" leave the
    // window up while "Import" and "Later" close it.
    QDialog dlg(this);
    dlg.setWindowTitle("NVIDIA NGX binaries not found");
    dlg.resize(520, 230);  // a comfortable default; the wrapped text and buttons sit inside this

    auto* root = new QVBoxLayout(&dlg);
    root->setContentsMargins(12, 12, 12, 12);
    auto* top = new QHBoxLayout;
    auto* icon = new QLabel(&dlg);
    icon->setPixmap(dlg.style()->standardIcon(QStyle::SP_MessageBoxWarning).pixmap(48, 48));
    top->addWidget(icon, 0, Qt::AlignTop);
    auto* textCol = new QVBoxLayout;
    auto* headline = new QLabel(
        "The neural model's DLL (nvngx_dlssnr.dll) is not in the binaries folder, so neural processing "
        "stays off and games present their own frames.", &dlg);
    headline->setWordWrap(true);
    auto* detail = new QLabel(FormatTip(
        "Import it from a folder you own, or open the folder to place the files yourself:\n" + dir +
        "\n\nThese are NVIDIA's proprietary files and are not shipped with this package."), &dlg);
    detail->setWordWrap(true);
    detail->setTextFormat(Qt::RichText);
    textCol->addWidget(headline);
    textCol->addWidget(detail);
    top->addLayout(textCol, 1);
    root->addLayout(top);

    auto* btnRow = new QHBoxLayout;
    auto* importBtn = new QPushButton("Import...", &dlg);
    auto* openBtn = new QPushButton("Open folder", &dlg);
    auto* laterBtn = new QPushButton("Later", &dlg);
    // [ Later ]                [ Open folder ] [ Import... ]
    btnRow->addWidget(laterBtn);   // dismiss, alone on the far side
    btnRow->addStretch(1);
    btnRow->addWidget(openBtn);    // affirmative group, right
    btnRow->addWidget(importBtn);  // primary, far right
    root->addSpacing(16);          // breathing room between the text and the buttons
    root->addLayout(btnRow);
    importBtn->setDefault(true);
    importBtn->setAutoDefault(true);

    connect(importBtn, &QPushButton::clicked, &dlg, &QDialog::accept);
    connect(laterBtn, &QPushButton::clicked, &dlg, &QDialog::reject);
    // "Open folder" opens the file manager and leaves the prompt up -- it resolves nothing on its own.
    connect(openBtn, &QPushButton::clicked, this, [this] { openBinariesFolder(); });

    // Import closes the prompt, then runs the import (which reports its own outcome).
    if (dlg.exec() == QDialog::Accepted) importBinaries();
}

void MainWindow::updateStatus() {
    if (binder) binder->Reload();
    // The menu spinbox is not a bound control, so the poll keeps it honest the same way the binder
    // keeps the bound ones honest -- unless the user is mid-edit on it, which is not the moment to
    // overwrite the number under their cursor.
    if (hdr && rebuildSpin && !rebuildSpin->hasFocus()) {
        QSignalBlocker block(rebuildSpin);
        rebuildSpin->setValue(int(hdr->rebuildSettleMs.load()));
    }
    updateCompositionVisibility();

    helperRunning = helperRunningNow();
    startBtn->setEnabled(!helperRunning);
    stopBtn->setEnabled(helperRunning);

    // A layer or helper from an older build re-initialises the mapping to its own version and keeps
    // running with its old field set -- every setting this side writes lands in a struct the other
    // side does not read, which looks exactly like a dead feature. Checking once at startup is not
    // enough because the mismatch can arrive the moment a game loads the stale layer, so check it on
    // every poll and say it plainly.
    const bool mismatch = hdr && (hdr->magic.load() != kShmMagic || hdr->version.load() != kShmVersion);

    QString state = "shared memory not attached";
    bool active = false;
    if (hdr && !mismatch) {
        static const char* kStates[] = { "starting", "no Vulkan device", "no NGX binaries",
                                         "the model would not start", "running", "stopped" };
        const uint32_t hs = hdr->helperState.load();
        state = hs < 6 ? kStates[hs] : "unknown";
        const QString why = QString::fromStdString(
            ShmLoadString(hdr->helperReasonSeq, hdr->helperReason, kReasonBytes));
        if (!why.isEmpty()) state += " -- " + why;

        // Active means a game is presenting through the layer right now: the composition is up and
        // the frame counter moved since the last poll. A counter that only ever grows would say
        // Active forever after one frame; movement is the point.
        //
        // The first poll seeds the counter rather than judging it. Measured against the zero it was
        // initialised to, any frame the layer had ever presented before this window opened read as
        // motion, so the interface opened claiming Active and corrected itself a second later.
        const quint64 frames = ShmLoad64(hdr->layerFramesLo, hdr->layerFramesHi);
        active = !firstPoll && hdr->layerCompositionUp.load() && frames != lastFrames;
        lastFrames = frames;
        firstPoll = false;
    }

    if (mismatch) {
        statusLabel->setText(QString("<span style=\"color:#e53935;\">Shared memory is v%1, this "
                                     "build is v%2 -- the layer or helper is out of date. Update "
                                     "them together.</span>")
                                 .arg(hdr->version.load()).arg(kShmVersion));
        return;  // do not save into a mapping the other side keeps resetting
    }

    const QString dot = active ? QString("<span style=\"color:#43a047;\">&#9679; Active</span>")
                               : QString("<span style=\"color:#9e9e9e;\">&#9675; Inactive</span>");
    statusLabel->setText(QString("Helper: %1&nbsp;&nbsp;&nbsp;%2").arg(state.toHtmlEscaped(), dot));

    saveSettingsIfChanged();
}

void MainWindow::updateCompositionVisibility() {
    if (!compositionForm) return;
    const bool bypass = hdr && hdr->compositionBypass.load() != 0;
    for (QWidget* w : compositionRows) compositionForm->setRowVisible(w, !bypass);
}

// The settings, on tabs.
//
// Grouped the way upstream groups them, because the grouping carries meaning: what the model was
// told and what a pass costs, how the answer is composed onto the frame, how color is interpreted,
// and the tools for looking at the result. Enabling the pass, the model's own controls and the cost
// share the Rendering tab; the composition and the color the composition works in share theirs --
// color strength, the white point and the guard are all part of how the answer lands, and none of
// them mean anything while the answer is presented raw; motion and inspection are each their own.
QWidget* MainWindow::buildSettings() {
    auto* tabs = new QTabWidget(this);
    binder = new ShmBinder(hdr, tabs);

    // Every tab scrolls: the Rendering tab outgrows any honest window height, and a tab that cannot
    // scroll just silently hides its bottom groups.
    const auto scrollTab = [&](const QString& title, QVBoxLayout** outCol) {
        auto* page = new QWidget;
        auto* inner = new QWidget;
        auto* col = new QVBoxLayout(inner);
        col->setContentsMargins(6, 6, 6, 6);
        auto* scroll = new QScrollArea;
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidget(inner);
        auto* lay = new QVBoxLayout(page);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->addWidget(scroll);
        tabs->addTab(page, title);
        *outCol = col;
    };

    const auto group = [](QVBoxLayout* col, const QString& title) {
        auto* box = new QGroupBox(title);
        auto* form = new QFormLayout(box);
        col->addWidget(box);
        return form;
    };

    QVBoxLayout* col = nullptr;
    scrollTab("Rendering", &col);
    {
        // Enabling the pass and telling the model what to do are one decision, so they share a
        // group. Style leads Preset because the profile matters more than the number.
        auto* f = group(col, "Neural rendering");
        binder->AddBool(f, "Enabled", &ShmHeader::enabled,
                        "Run the model at all. Off leaves the game's own frame untouched.");
        binder->AddChoice(f, "Style", &ShmHeader::style, { "Default", "Natural", "Cinematic" },
                          "The model's own processing profiles.", ShmBinder::AtCreate);
        binder->AddInt(f, "Preset", &ShmHeader::preset, 0, 15, "The model's own render preset.",
                       ShmBinder::AtCreate);
        binder->AddFloat(f, "Intensity", &ShmHeader::intensityBits, 0.0, 4.0, 0.05,
                         "How hard the model works.", ShmBinder::AtCreate);
        binder->AddFloat(f, "Local structure", &ShmHeader::localStructureBits, 0.0, 4.0, 0.05, "",
                         ShmBinder::AtCreate);
        binder->AddFloat(f, "Local tone", &ShmHeader::localToneBits, 0.0, 4.0, 0.05, "",
                         ShmBinder::AtCreate);
        binder->AddFloat(f, "Skin structure", &ShmHeader::skinStructureBits, -1.0, 4.0, 0.05,
                         "-1 follows local structure, which is the model's own default. It is not a "
                         "strength of zero.",
                         ShmBinder::AtCreate);
        binder->AddBool(f, "Auto skin mask", &ShmHeader::autoMask, "The model's automatic skin mask.",
                        ShmBinder::AtCreate);
        binder->AddFloat(f, "Sharpness", &ShmHeader::sharpnessBits, 0.0, 1.0, 0.05,
                         "The one strength the model reads every frame, so it takes effect at once.");
    }
    {
        auto* f = group(col, "Cost");
binder->AddInt(f, "Passes", &ShmHeader::passes, 1, int(kMaxPasses),
                       "How many times the model runs over one frame, each pass shown the last one's "
                       "answer.\n"
                       "Every pass is another full run of the model and another feature holding its "
                       "own history, so the cost is close to linear.",
                       ShmBinder::AtCreate);
        binder->AddBool(f, "Lift the pass limit", &ShmHeader::unlockPasses,
                         QString("Raises the ceiling from %1 to %2.\n"
                                 "Past a few passes the model is enhancing its own output, which is "
                                 "outside what it was trained for.")
                             .arg(kDefaultMaxPasses)
                             .arg(kMaxPasses));
        binder->AddPercent(f, "Model resolution", &ShmHeader::workingScaleBits, 25, 200,
                           "What fraction of the frame the model works at. The frame itself is never "
                           "reduced.\n"
                           "Below 100% also cuts what crosses shared memory, quadratically.\n"
                           "Above 100% the model supersamples, which on this transport is expensive: "
                           "at 200% on a 4K frame it is 132 MB each way, every frame.");
        binder->AddChoice(f, "Down-leg filter", &ShmHeader::scalingDownscaler,
                          { "(fsr1, unsupported)", "Bicubic", "Catmull-Rom", "Lanczos2", "Lanczos3",
                            "Kaiser2", "Kaiser3", "Magic" },
                          "How a supersampled answer is averaged back to the frame's size. Only used "
                          "above 100%.");
        passBtn = new QPushButton("Per-pass settings...", col->parentWidget());
        f->addRow(passBtn);
    }

    scrollTab("Motion", &col);
    {
        auto* f = group(col, "Motion");
        binder->AddBool(f, "Estimate motion vectors", &ShmHeader::mvecEnabled,
                        "The model reasons about what moved between frames.\n"
                        "A layer at present time has no motion vectors from the engine, so they are "
                        "estimated on the GPU's optical-flow engine from the two frames the helper "
                        "already has.\n"
                        "Off hands the model a zero field, which is what it used to get.");
        binder->AddChoice(f, "Motion quality", &ShmHeader::mvecQuality,
                          { "Fast", "Balanced", "Quality" },
                          "How much of the frame's budget the flow estimate may take.");
        binder->AddChoice(f, "Motion units", &ShmHeader::mvecScaleMode,
                          { "Normalised", "Pixels", "UV 0..1" },
                          "What the numbers in the field mean to the model.\n"
                          "Pixels is what the estimate produces; the others are for matching a model "
                          "that expects them.");
    }

    scrollTab("Composition", &col);
    {
        // What the model decided is one thing and how much of it lands is another; the answer is
        // presented raw until this is switched on. The color group sits under it because the color
        // the composition works in -- the white point it normalises by, how much of the model's hue
        // arrives -- is part of the same decision, and means nothing while the answer is raw.
        auto* f = group(col, "Composition");
        compositionForm = f;
        bypassCheck = binder->AddBool(
            f, "Enabled", &ShmHeader::compositionBypass,
            "Off: the model's raw answer is presented as the frame.\n"
            "On: the answer is blended onto the frame under the limits below, which are hidden while "
            "this is off.",
            ShmBinder::Live, /*invert=*/true);
        connect(bypassCheck, &QCheckBox::toggled, this, [this] { updateCompositionVisibility(); });
        compositionRows << binder->AddFloat(f, "Detail strength", &ShmHeader::transferStrengthBits,
                                            0.0, 4.0, 0.05,
                                            "How much of the model's edit reaches the frame.\n"
                                            "At zero the frame is bit-identical to the game's own.");
        compositionRows << binder->AddFloat(f, "Color strength", &ShmHeader::colourStrengthBits,
                                            0.0, 4.0, 0.05,
                                            "How much of the model's color comes with its light.\n"
                                            "At zero the frame keeps the game's hue exactly.");
        compositionRows << binder->AddFloat(f, "Highlight guard", &ShmHeader::maxRatioBits, 1.0, 30.0,
                                            0.5,
                                            "The most the pass may brighten or darken a pixel.\n"
                                            "A detail pass has no business restyling a light source, "
                                            "whatever the model returns.");
        compositionRows << binder->AddChoice(f, "Enlargement", &ShmHeader::transfer,
                                             { "Classic", "Matched residual", "Native + edit" },
                                             "How a model that worked below the frame's size is "
                                             "brought back.\n"
                                             "Matched residual carries only the model's difference up, "
                                             "so the two pictures being composed are at the same "
                                             "scale.\n"
                                             "Native + edit composes nothing at all: the frame's own "
                                             "pixels are the result and only the model's difference is "
                                             "added to them, so geometry, text and edges the model "
                                             "left alone stay at native sharpness.\n"
                                             "Only does anything below a working scale of 1.");
    }
    {
        auto* f = group(col, "Color");
        binder->AddChoice(f, "HDR input", &ShmHeader::hdrMode,
                          { "Auto", "Off", "Force float16" },
                          "Let the model see the frame's real light instead of a tone-mapped copy.\n"
                          "Auto turns it on when the swapchain is HDR -- a float swapchain, or 10-bit "
                          "with a PQ colour space -- and the proxy then crosses as float16 carrying "
                          "linear light, PQ-decoded first when the swapchain carries PQ.\n"
                          "Off keeps the 8-bit proxy whatever the game presents.\n"
                          "Force feeds the float proxy to an SDR swapchain too, which is an A/B tool "
                          "rather than a preference.\n"
                          "The model has the last word: if it refuses float input the pass falls back "
                          "to 8-bit on its own.");
        binder->AddChoice(f, "Frame holds", &ShmHeader::colourMode,
                          { "Auto", "A finished picture", "Linear light" },
                          "Whether the swapchain carries a frame the game already tone mapped or "
                          "open-ended light.\n"
                          "Auto decides from the format and is right for almost every game.");
        binder->AddChoice(f, "White point from", &ShmHeader::whitePointSource,
                          { "The slider below", "Measured off the frame" },
                          "Only meaningful on a linear frame; a finished picture has no white point "
                          "to find.");
        binder->AddFloat(f, "Paper white", &ShmHeader::whitePointBits, 0.01, 2000.0, 0.1,
                         "What the model should treat as white, when it is not being measured.");
        binder->AddFloat(f, "White point scale", &ShmHeader::whitePointScaleBits, 0.01, 100.0, 0.05,
                         "Multiplies whichever white point is in use. Higher means highlights sit "
                         "lower on the curve.");
        binder->AddFloat(f, "Trim (measured)", &ShmHeader::whitePointTrimBits, 0.01, 100.0, 0.05,
                         "Multiplies a measured white point only. Kept apart from the slider because "
                         "a value found against one is meaningless against the other.");
    }

    scrollTab("Inspect", &col);
    {
        auto* f = group(col, "Inspect");
        binder->AddBool(f, "Apply the model's edit", &ShmHeader::applyModel,
                        "Off keeps the whole pass running and shows the clean frame, so the cost is "
                        "unchanged and only the picture differs.");
        binder->AddBool(f, "Hold frame", &ShmHeader::holdFrame,
                        "Freeze the frame the pass works on, so changing a setting re-runs the model "
                        "and the composition on the same picture.\n"
                        "The only clean way to compare two settings.");
        binder->AddChoice(f, "Proxy", &ShmHeader::reversibleMode,
                          { "Soft knee", "Neutwo", "Neutwo, replace", "Hybrid", "Hybrid, replace" },
                          "Which picture the model is shown, and whether its answer is composed onto "
                          "the frame or substituted for it.\n"
                          "Soft knee is the default and the two replace modes are known to flash on "
                          "bright lights.");
        binder->AddChoice(f, "Debug view", &ShmHeader::debugView,
                          { "Off", "The picture the model saw", "Its raw answer", "What it changed" },
                          "The last one is amplified and centred on grey, so both directions of the "
                          "edit are visible at once.");
        binder->AddFloat(f, "Debug scale", &ShmHeader::debugScaleBits, 0.01, 100.0, 0.1,
                         "What the debug views are multiplied by on their way out.");
        binder->AddChoice(f, "Compare", &ShmHeader::compareMode, { "Off", "Side by side", "Wipe" },
                          "Shows the pass against itself.\n"
                          "The wipe cuts one frame and resamples nothing, so it is the one to play "
                          "with.\n"
                          "Works with composition off too: the model's raw answer is then shown "
                          "against the frame.");
        binder->AddFloat(f, "Split", &ShmHeader::compareSplitBits, 0.0, 1.0, 0.01, "");
        binder->AddFloat(f, "Zoom", &ShmHeader::compareZoomBits, 1.0, 2.0, 0.05,
                         "Side by side only. 1 fits the whole frame and accepts the bars; 2 fills the "
                         "half and crops.");
        binder->AddBool(f, "Swap sides", &ShmHeader::compareSwap,
                        "Which side the edited frame sits on. Worth having because the eye is not "
                        "even-handed about left and right.");

        // The in-game key, and an honest account of when it can work.
        //
        // The layer has no window, so what it can read depends on the session. On a Wayland desktop a
        // game's keys go to the compositor and never reach this process, and /dev/input is not
        // readable without the 'input' group -- keyboards get no uaccess ACL, deliberately, because
        // that would let any program keylog. Where the layer cannot read a key the desktop still can,
        // so the command below is offered as the way that always works.
        keyCombo = new QComboBox(col->parentWidget());
        keyCombo->addItem("None", 0u);
        for (const char* name : { "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11",
                                  "F12", "HOME", "END", "INSERT", "DELETE", "PAGEUP", "PAGEDOWN",
                                  "PAUSE", "SCROLLLOCK", "GRAVE" })
            keyCombo->addItem(name, dlssnr::KeyCodeFromName(name));
        if (hdr) {
            const int idx = keyCombo->findData(hdr->toggleKey.load());
            keyCombo->setCurrentIndex(idx >= 0 ? idx : 0);
        }
        keyCombo->setToolTip(FormatTip(
            "Toggles the pass in game. Read by the layer, which works on an X11 or XWayland\n"
            "session and anywhere you are in the 'input' group.\n"
            "It cannot work for a game presenting through winewayland."));
        f->addRow("Toggle key", keyCombo);
        connect(keyCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
            if (!hdr) return;
            hdr->toggleKey.store(keyCombo->currentData().toUInt());
            hdr->controlSeq.fetch_add(1);
        });

        auto* keyNote = new QLabel(
            QString("If that key does nothing -- a Wayland game, or not in the 'input' group -- bind "
                    "this to a shortcut in your desktop's own settings instead. That works over a "
                    "fullscreen game and needs no permissions:\n\n    dlssnr-shmctl %1 toggle enabled")
                .arg(shmPath),
            col->parentWidget());
        keyNote->setWordWrap(true);
        keyNote->setTextInteractionFlags(Qt::TextSelectableByMouse);
        f->addRow(keyNote);

        auto* capRow = new QHBoxLayout;
        captureFrames = new QSpinBox(col->parentWidget());
        captureFrames->setRange(1, 64);
        captureFrames->setValue(8);
        captureBtn = new QPushButton("Capture frames", col->parentWidget());
        captureBtn->setToolTip(FormatTip(
            "Writes that many matched before/after pairs to the state directory.\n"
            "Same frames, same run, one variable."));
        capRow->addWidget(captureFrames);
        capRow->addWidget(captureBtn);
        f->addRow(capRow);

        connect(captureBtn, &QPushButton::clicked, this, [this] {
            if (!hdr) return;
            hdr->captureRequest.store(uint32_t(captureFrames->value()));
            hdr->controlSeq.fetch_add(1);
        });
    }

    connect(passBtn, &QPushButton::clicked, this, [this] {
        if (!hdr) return;
        PassDialog dlg(hdr, this);
        dlg.exec();
    });

    col->addStretch(1);
    binder->Reload();
    updateCompositionVisibility();
    return tabs;
}