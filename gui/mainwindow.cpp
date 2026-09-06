#include "mainwindow.h"
#include "passdialog.h"
#include "../common/runner_discovery.h"
#include "shm_binder.h"
#include "../layer_linux/src/hotkey.h"

#include <QAction>
#include <QActionGroup>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
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
#include <QTabWidget>
#include <QTextStream>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QGroupBox>
#include <QScrollArea>
#include <QVariantMap>

#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

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
    runnerCombo->setToolTip("Proton or Wine for the helper process, which is a Windows executable "
                            "because the model is a Windows DLL. This is not what the game runs "
                            "under -- native Linux games work with this set too.");
    runnerPathEdit->setToolTip(runnerCombo->toolTip());
    runnerForm->addRow("Runner (for the helper)", runnerCombo);
    runnerForm->addRow("Path", runnerPathRow);
    root->addLayout(runnerForm);

    auto* buttons = new QHBoxLayout;
    startBtn = new QPushButton("Start helper", this);
    stopBtn = new QPushButton("Stop helper", this);
    buttons->addWidget(startBtn);
    buttons->addWidget(stopBtn);
    root->addLayout(buttons);

    root->addWidget(buildSettings(), 1);

    // The gear, bottom right: the things that act on the whole interface rather than one setting.
    gearBtn = new QToolButton(this);
    gearBtn->setIcon(gearIcon(this));
    gearBtn->setToolTip("Settings");
    gearBtn->setPopupMode(QToolButton::InstantPopup);
    auto* gearMenu = new QMenu(gearBtn);
    gearMenu->addAction("Reset all settings...", this, &MainWindow::resetAllSettings);
    gearMenu->addSeparator();
    gearMenu->addAction("Open helper log", this, [this] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(logPath));
    });
    gearBtn->setMenu(gearMenu);
    auto* bottom = new QHBoxLayout;
    bottom->addStretch(1);
    bottom->addWidget(gearBtn);
    root->addLayout(bottom);

    connect(startBtn, &QPushButton::clicked, this, &MainWindow::startHelper);
    connect(stopBtn, &QPushButton::clicked, this, &MainWindow::stopHelper);
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
}

void MainWindow::resetAllSettings() {
    if (QMessageBox::question(this, "DLSS5VKLayer",
                              "Reset every setting to its default? The helper will rebuild its "
                              "features from the defaults.") != QMessageBox::Yes)
        return;
    if (!hdr) return;
    ShmInitDefaults(hdr);
    hdr->controlSeq.fetch_add(1);
    hdr->tuningSeq.fetch_add(1);
    if (keyCombo) keyCombo->setCurrentIndex(0);
    if (binder) binder->Reload();
    updateCompositionVisibility();
    lastSettingsBlob = settingsBlob();
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

void MainWindow::updateStatus() {
    if (binder) binder->Reload();
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
        const quint64 frames = ShmLoad64(hdr->layerFramesLo, hdr->layerFramesHi);
        active = hdr->layerCompositionUp.load() && frames != lastFrames;
        lastFrames = frames;
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
// and the tools for looking at the result. Enabling the pass, the model's own controls, the cost and
// the composition are one story and share the Rendering tab; motion, color and inspection are each
// their own.
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
                       "answer. Every pass is another full run of the model and another feature "
                       "holding its own history, so the cost is close to linear.",
                       ShmBinder::AtCreate);
        binder->AddBool(f, "Lift the pass limit", &ShmHeader::unlockPasses,
                        QString("Raises the ceiling from %1 to %2. Past a few passes the model is "
                                "enhancing its own output, which is outside what it was trained for.")
                            .arg(kDefaultMaxPasses)
                            .arg(kMaxPasses));
        binder->AddInt(f, "Rebuild spacing (ms)", &ShmHeader::rebuildSettleMs, 0, 5000,
                       "How long the helper waits after a model setting changes before it rebuilds "
                       "the pass, and between one rebuild and the next. Rebuilding is expensive, and "
                       "back-to-back rebuilds have been seen to wedge the model on some drivers. "
                       "Lower is snappier; 0 rebuilds immediately and chains the rest back to back. "
                       "Raise it if the model ever stops answering after changing settings.",
                       ShmBinder::Live);
        binder->AddPercent(f, "Model resolution", &ShmHeader::workingScaleBits, 25, 200,
                           "What fraction of the frame the model works at. The frame itself is never "
                           "reduced. Below 100% also cuts what crosses shared memory, quadratically. "
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
    {
        // Bottom of the tab: what the model decided is one thing, how much of it lands is another,
        // and the answer is presented raw until this is switched on.
        auto* f = group(col, "Composition");
        compositionForm = f;
        bypassCheck = binder->AddBool(
            f, "Enabled", &ShmHeader::compositionBypass,
            "Off: the model's raw answer is presented as the frame. On: the answer is blended onto "
            "the frame under the limits below, which are hidden while this is off.",
            ShmBinder::Live, /*invert=*/true);
        connect(bypassCheck, &QCheckBox::toggled, this, [this] { updateCompositionVisibility(); });
        compositionRows << binder->AddFloat(f, "Detail strength", &ShmHeader::transferStrengthBits,
                                            0.0, 4.0, 0.05,
                                            "How much of the model's edit reaches the frame. At zero "
                                            "the frame is bit-identical to the game's own.");
        compositionRows << binder->AddFloat(f, "Color strength", &ShmHeader::colourStrengthBits,
                                            0.0, 4.0, 0.05,
                                            "How much of the model's color comes with its light. At "
                                            "zero the frame keeps the game's hue exactly.");
        compositionRows << binder->AddFloat(f, "Highlight guard", &ShmHeader::maxRatioBits, 1.0, 30.0,
                                            0.5,
                                            "The most the pass may brighten or darken a pixel. A "
                                            "detail pass has no business restyling a light source, "
                                            "whatever the model returns.");
        compositionRows << binder->AddChoice(f, "Enlargement", &ShmHeader::transfer,
                                             { "Classic", "Matched residual", "Native + edit" },
                                             "How a model that worked below the frame's size is "
                                             "brought back. Matched residual carries only the model's "
                                             "difference up, so the two pictures being composed are "
                                             "at the same scale. Native + edit composes nothing at "
                                             "all: the frame's own pixels are the result and only the "
                                             "model's difference is added to them, so geometry, text "
                                             "and edges the model left alone stay at native "
                                             "sharpness. Only does anything below a working scale "
                                             "of 1.");
    }

    scrollTab("Motion", &col);
    {
        auto* f = group(col, "Motion");
        binder->AddBool(f, "Estimate motion vectors", &ShmHeader::mvecEnabled,
                        "The model reasons about what moved between frames. A layer at present time "
                        "has no motion vectors from the engine, so they are estimated on the GPU's "
                        "optical-flow engine from the two frames the helper already has. Off hands "
                        "the model a zero field, which is what it used to get.");
        binder->AddChoice(f, "Motion quality", &ShmHeader::mvecQuality,
                          { "Fast", "Balanced", "Quality" },
                          "How much of the frame's budget the flow estimate may take.");
        binder->AddChoice(f, "Motion units", &ShmHeader::mvecScaleMode,
                          { "Normalised", "Pixels", "UV 0..1" },
                          "What the numbers in the field mean to the model. Pixels is what the "
                          "estimate produces; the others are for matching a model that expects them.");
    }

    scrollTab("Color", &col);
    {
        auto* f = group(col, "Color");
        binder->AddChoice(f, "Frame holds", &ShmHeader::colourMode,
                          { "Auto", "A finished picture", "Linear light" },
                          "Whether the swapchain carries a frame the game already tone mapped or "
                          "open-ended light. Auto decides from the format and is right for almost "
                          "every game.");
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
                        "and the composition on the same picture. The only clean way to compare two "
                        "settings.");
        binder->AddChoice(f, "Proxy", &ShmHeader::reversibleMode,
                          { "Soft knee", "Neutwo", "Neutwo, replace", "Hybrid", "Hybrid, replace" },
                          "Which picture the model is shown, and whether its answer is composed onto "
                          "the frame or substituted for it. Soft knee is the default and the two "
                          "replace modes are known to flash on bright lights.");
        binder->AddChoice(f, "Debug view", &ShmHeader::debugView,
                          { "Off", "The picture the model saw", "Its raw answer", "What it changed" },
                          "The last one is amplified and centred on grey, so both directions of the "
                          "edit are visible at once.");
        binder->AddFloat(f, "Debug scale", &ShmHeader::debugScaleBits, 0.01, 100.0, 0.1,
                         "What the debug views are multiplied by on their way out.");
        binder->AddChoice(f, "Compare", &ShmHeader::compareMode, { "Off", "Side by side", "Wipe" },
                          "Shows the pass against itself. The wipe cuts one frame and resamples "
                          "nothing, so it is the one to play with.");
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
        keyCombo->setToolTip("Toggles the pass in game. Read by the layer, which works on an X11 or "
                             "XWayland session and anywhere you are in the 'input' group. It cannot "
                             "work for a game presenting through winewayland.");
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
        captureBtn->setToolTip("Writes that many matched before/after pairs to the state directory. "
                               "Same frames, same run, one variable.");
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