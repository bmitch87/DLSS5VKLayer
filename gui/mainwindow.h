#pragma once
#include <QWidget>
#include <QString>
#include <QVector>
#include <QPair>
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
class ShmBinder;

class MainWindow : public QWidget {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent* event) override;

private slots:
    void startHelper();
    void stopHelper();
    void updateStatus();

private:
    QString findProjectDir() const;
    QString findHelperCli() const;
    QString configPath() const;
    QString defaultShmPath() const;
    QString defaultLogPath() const;
    QString pidFilePath() const;
    bool helperRunningNow() const;
    void loadConfig();
    void saveConfig();
    void restoreSettings();
    void saveSettingsIfChanged();
    void resetAllSettings();
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
    QPushButton* passBtn = nullptr;
    QPushButton* captureBtn = nullptr;
    QPushButton* browseRunnerBtn = nullptr;
    QToolButton* gearBtn = nullptr;
    QComboBox* runnerCombo = nullptr;
    QComboBox* keyCombo = nullptr;
    QLineEdit* runnerPathEdit = nullptr;
    QLabel* statusLabel = nullptr;
    QSpinBox* captureFrames = nullptr;
    QCheckBox* bypassCheck = nullptr;
    QFormLayout* compositionForm = nullptr;
    QVector<QWidget*> compositionRows;
    bool helperRunning = false;
    quint64 lastFrames = 0;

    ShmBinder* binder = nullptr;
    QString lastSettingsBlob;
};