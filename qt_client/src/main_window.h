#pragma once

#include <QJsonObject>
#include <QMainWindow>

class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTableWidget;
class QTextEdit;
class QTimer;
class PeopleFlowApiClient;

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(const QString& initialBaseUrl = {}, QWidget* parent = nullptr);

protected:
    void closeEvent(QCloseEvent* event) override;

private slots:
    void applyConnectionSettings();
    void startSession();
    void stopSession();
    void refreshLiveData();
    void showHealth(const QJsonObject& payload);
    void showSessionStarted(const QJsonObject& payload);
    void showSessionStopped(const QJsonObject& payload);
    void showStatus(const QJsonObject& payload);
    void showRealtime(const QJsonObject& payload);
    void showSnapshot(const QByteArray& jpegBytes);
    void showSecurity(const QJsonObject& payload);
    void showEvents(const QJsonObject& payload);
    void showRequestError(const QString& operation, int httpStatus, const QString& message);

private:
    QWidget* createConnectionPanel();
    QWidget* createStatusPanel();
    QWidget* createSecurityPanel();
    QWidget* createSnapshotPanel();
    QWidget* createEventsPanel();
    QLabel* createMetricValue(const QString& initialValue = QStringLiteral("--"));
    bool configureBaseUrl();
    void updateSessionControls();
    void setSessionId(const QString& sessionId);
    void appendLog(const QString& text);
    void loadSettings(const QString& initialBaseUrl);
    void saveSettings() const;
    static QString displayNumber(const QJsonValue& value, int decimals = 0);

    PeopleFlowApiClient* api_ = nullptr;
    QTimer* refreshTimer_ = nullptr;
    int refreshTick_ = 0;
    QString sessionId_;
    bool stopPending_ = false;

    QLineEdit* baseUrlEdit_ = nullptr;
    QLineEdit* cameraProfileEdit_ = nullptr;
    QLineEdit* cameraIdEdit_ = nullptr;
    QLineEdit* configVersionEdit_ = nullptr;
    QSpinBox* initialOccupancySpin_ = nullptr;
    QPushButton* healthButton_ = nullptr;
    QPushButton* startButton_ = nullptr;
    QPushButton* stopButton_ = nullptr;

    QLabel* serviceStateValue_ = nullptr;
    QLabel* sessionStateValue_ = nullptr;
    QLabel* sessionIdValue_ = nullptr;
    QLabel* occupancyValue_ = nullptr;
    QLabel* inValue_ = nullptr;
    QLabel* outValue_ = nullptr;
    QLabel* livePersonsValue_ = nullptr;
    QLabel* captureFpsValue_ = nullptr;
    QLabel* inferFpsValue_ = nullptr;
    QLabel* phase1Value_ = nullptr;
    QLabel* phase2Value_ = nullptr;
    QLabel* phase3Value_ = nullptr;
    QLabel* phase4Value_ = nullptr;
    QLabel* snapshotLabel_ = nullptr;
    QLabel* snapshotHint_ = nullptr;
    QTableWidget* eventsTable_ = nullptr;
    QTableWidget* securityEventsTable_ = nullptr;
    QTextEdit* logEdit_ = nullptr;
};
