#pragma once
#include <QWidget>
#include <QString>
#include <QVector>
#include <QPair>
#include <QStyledItemDelegate>
#include <QModelIndex>
#include <QPainter>
#include <QStyleOptionViewItem>
#include "../common/shm_protocol.h"

class QProcess;
class QPushButton;
class QToolButton;
class QCheckBox;
class QSpinBox;
class QDoubleSpinBox;
class QLabel;
class QTimer;
class QTabWidget;
class QComboBox;
class QLineEdit;
class QFormLayout;
class QAction;
class ShmBinder;

class ProfileDelegate : public QStyledItemDelegate {
public:
    explicit ProfileDelegate(QObject* parent = nullptr) : QStyledItemDelegate(parent) {}
    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;
private:
    QRect buttonRect(const QStyleOptionViewItem& option) const;
};

class MainWindow : public QWidget {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent* event) override;
    bool eventFilter(QObject* obj, QEvent* event) override;

private slots:
    void startHelper();
    void stopHelper();
    void updateStatus();
    void importBinaries();
    void openBinariesFolder();

private:
    QString findProjectDir() const;
    QString findHelperCli() const;
    QString configPath() const;
    QString profilesDir() const;
    QString dataDir() const;
    QString defaultBinariesDir() const;
    QString effectiveBinariesDir() const;
    bool binariesMissingNow() const;
    void maybePromptImport();
    QString defaultShmPath() const;
    QString defaultLogPath() const;
    QString pidFilePath() const;
    bool helperRunningNow() const;
    void loadConfig();
    void saveConfig();
    void restoreSettings();
    void saveSettingsIfChanged();
    void resetAllSettings();
    void applyDefaults();
    void saveSettingsToFile();
    void loadSettingsFromFile(const QString& path);
    void refreshProfileList();
    void updateReloadBtn();
    QString settingsBlob() const;
    void populateRunners();
    void applyRunnerSelection(int index);
    bool ensureShm();
    QWidget* buildSettings();
    void updateCompositionVisibility();

    QString projectDir;
    QString helperCliPath;
    QString configFilePath;
    QString runnerType;
    QString runnerPath;
    QString binariesPath;
    QString logPath;
    QString dxvkVendor;
    QString dxvkDevice;
    QString shmPath;
    int windowW = 620;
    int windowH = 680;
    QVector<QPair<QString, QString>> pendingSettings;
    void* shmBase = nullptr;
    ShmHeader* hdr = nullptr;

    QTimer* statusTimer = nullptr;

    QPushButton* startBtn = nullptr;
    QPushButton* stopBtn = nullptr;
    QComboBox* profileCombo = nullptr;
    QPushButton* profileSaveBtn = nullptr;
    QPushButton* passBtn = nullptr;
    QPushButton* captureBtn = nullptr;
    QPushButton* browseRunnerBtn = nullptr;
    QToolButton* gearBtn = nullptr;
    QComboBox* runnerCombo = nullptr;
    QComboBox* keyCombo = nullptr;
    QLineEdit* runnerPathEdit = nullptr;
    QAction* binariesPathAction = nullptr;
    QLabel* statusLabel = nullptr;
    QSpinBox* captureFrames = nullptr;
    QCheckBox* bypassCheck = nullptr;
    QFormLayout* compositionForm = nullptr;
    QVector<QWidget*> compositionRows;
    bool helperRunning = false;
    bool firstPoll = true;
    quint64 lastFrames = 0;
    QSpinBox* rebuildSpin = nullptr;

    ShmBinder* binder = nullptr;
    QString lastSettingsBlob;
    QString lastProfilePath;
    QString profileBlob;
    QString defaultsBlob;
    QToolButton* profileReloadBtn = nullptr;
};