#include "main_window.h"

#include "people_flow_api_client.h"

#include <QCloseEvent>
#include <QDateTime>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonValue>
#include <QLabel>
#include <QLineEdit>
#include <QList>
#include <QPixmap>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSpinBox>
#include <QSplitter>
#include <QStatusBar>
#include <QTableWidget>
#include <QTextEdit>
#include <QTimer>
#include <QUrl>
#include <QVariant>
#include <QVBoxLayout>

namespace {
QWidget* metricCard(const QString& title, QLabel* value) {
    auto* frame = new QFrame;
    frame->setObjectName(QStringLiteral("metricCard"));
    auto* layout = new QVBoxLayout(frame);
    layout->setContentsMargins(14, 10, 14, 10);
    auto* titleLabel = new QLabel(title);
    titleLabel->setObjectName(QStringLiteral("metricTitle"));
    layout->addWidget(titleLabel);
    layout->addWidget(value);
    return frame;
}
}

MainWindow::MainWindow(const QString& initialBaseUrl, QWidget* parent)
    : QMainWindow(parent), api_(new PeopleFlowApiClient(this)), refreshTimer_(new QTimer(this)) {
    setWindowTitle(QStringLiteral("People Flow 流程体验台"));
    resize(1280, 820);
    setMinimumSize(980, 680);

    auto* central = new QWidget;
    auto* root = new QVBoxLayout(central);
    root->setContentsMargins(18, 16, 18, 16);
    root->setSpacing(12);
    root->addWidget(createConnectionPanel());
    root->addWidget(createStatusPanel());
    root->addWidget(createSecurityPanel());

    auto* splitter = new QSplitter(Qt::Horizontal);
    splitter->addWidget(createSnapshotPanel());
    splitter->addWidget(createEventsPanel());
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);
    root->addWidget(splitter, 1);

    logEdit_ = new QTextEdit;
    logEdit_->setReadOnly(true);
    logEdit_->setMaximumHeight(110);
    logEdit_->setPlaceholderText(QStringLiteral("操作日志"));
    root->addWidget(logEdit_);
    setCentralWidget(central);

    setStyleSheet(QStringLiteral(R"(
        QMainWindow, QWidget { background: #f4f6f8; color: #1e293b; font-size: 13px; }
        QGroupBox { background: white; border: 1px solid #dce2e8; border-radius: 8px;
                    margin-top: 12px; padding-top: 10px; font-weight: 600; }
        QGroupBox::title { subcontrol-origin: margin; left: 12px; padding: 0 5px; }
        QLineEdit, QSpinBox { background: white; border: 1px solid #cbd5e1; border-radius: 5px;
                             padding: 6px 8px; min-height: 22px; }
        QPushButton { background: #2563eb; color: white; border: none; border-radius: 5px;
                      padding: 7px 14px; font-weight: 600; }
        QPushButton:hover { background: #1d4ed8; }
        QPushButton:disabled { background: #94a3b8; }
        QPushButton#stopButton { background: #dc2626; }
        QFrame#metricCard { background: white; border: 1px solid #dce2e8; border-radius: 8px; }
        QLabel#metricTitle { color: #64748b; font-size: 12px; }
        QLabel#metricValue { font-size: 23px; font-weight: 700; color: #0f172a; }
        QLabel#sectionTitle { font-size: 15px; font-weight: 700; }
        QLabel#securityStageValue { font-size: 14px; font-weight: 700; color: #0f172a; }
        QLabel#demoStageValue { font-size: 14px; font-weight: 800; color: #d97706; }
        QTableWidget, QTextEdit { background: white; border: 1px solid #dce2e8; border-radius: 6px; }
        QHeaderView::section { background: #e8edf3; border: none; padding: 6px; font-weight: 600; }
    )"));

    loadSettings(initialBaseUrl);
    applyConnectionSettings();
    updateSessionControls();

    connect(healthButton_, &QPushButton::clicked, this, &MainWindow::applyConnectionSettings);
    connect(startButton_, &QPushButton::clicked, this, &MainWindow::startSession);
    connect(stopButton_, &QPushButton::clicked, this, &MainWindow::stopSession);
    connect(refreshTimer_, &QTimer::timeout, this, &MainWindow::refreshLiveData);
    refreshTimer_->setInterval(1000);
    refreshTimer_->start();

    connect(api_, &PeopleFlowApiClient::healthReceived, this, &MainWindow::showHealth);
    connect(api_, &PeopleFlowApiClient::sessionStarted, this, &MainWindow::showSessionStarted);
    connect(api_, &PeopleFlowApiClient::sessionStopped, this, &MainWindow::showSessionStopped);
    connect(api_, &PeopleFlowApiClient::statusReceived, this, &MainWindow::showStatus);
    connect(api_, &PeopleFlowApiClient::realtimeReceived, this, &MainWindow::showRealtime);
    connect(api_, &PeopleFlowApiClient::snapshotReceived, this, &MainWindow::showSnapshot);
    connect(api_, &PeopleFlowApiClient::securityReceived, this, &MainWindow::showSecurity);
    connect(api_, &PeopleFlowApiClient::eventsReceived, this, &MainWindow::showEvents);
    connect(api_, &PeopleFlowApiClient::requestFailed, this, &MainWindow::showRequestError);

    QTimer::singleShot(0, api_, &PeopleFlowApiClient::checkHealth);
}

QWidget* MainWindow::createConnectionPanel() {
    auto* box = new QGroupBox(QStringLiteral("连接与会话"));
    auto* grid = new QGridLayout(box);
    grid->setColumnStretch(1, 2);
    grid->setColumnStretch(3, 1);
    grid->setColumnStretch(5, 1);

    baseUrlEdit_ = new QLineEdit;
    cameraProfileEdit_ = new QLineEdit;
    cameraIdEdit_ = new QLineEdit;
    configVersionEdit_ = new QLineEdit;
    initialOccupancySpin_ = new QSpinBox;
    initialOccupancySpin_->setRange(0, 100000);
    healthButton_ = new QPushButton(QStringLiteral("检查服务"));
    startButton_ = new QPushButton(QStringLiteral("启动会话"));
    stopButton_ = new QPushButton(QStringLiteral("停止会话"));
    stopButton_->setObjectName(QStringLiteral("stopButton"));

    grid->addWidget(new QLabel(QStringLiteral("服务地址")), 0, 0);
    grid->addWidget(baseUrlEdit_, 0, 1);
    grid->addWidget(new QLabel(QStringLiteral("Camera Profile")), 0, 2);
    grid->addWidget(cameraProfileEdit_, 0, 3);
    grid->addWidget(new QLabel(QStringLiteral("Camera ID")), 0, 4);
    grid->addWidget(cameraIdEdit_, 0, 5);
    grid->addWidget(new QLabel(QStringLiteral("配置版本")), 1, 0);
    grid->addWidget(configVersionEdit_, 1, 1);
    grid->addWidget(new QLabel(QStringLiteral("初始人数")), 1, 2);
    grid->addWidget(initialOccupancySpin_, 1, 3);

    auto* buttons = new QHBoxLayout;
    buttons->addWidget(healthButton_);
    buttons->addWidget(startButton_);
    buttons->addWidget(stopButton_);
    grid->addLayout(buttons, 1, 4, 1, 2);
    return box;
}

QWidget* MainWindow::createStatusPanel() {
    auto* widget = new QWidget;
    auto* layout = new QGridLayout(widget);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    serviceStateValue_ = createMetricValue();
    sessionStateValue_ = createMetricValue();
    sessionIdValue_ = createMetricValue();
    sessionIdValue_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    occupancyValue_ = createMetricValue(QStringLiteral("0"));
    inValue_ = createMetricValue(QStringLiteral("0"));
    outValue_ = createMetricValue(QStringLiteral("0"));
    livePersonsValue_ = createMetricValue(QStringLiteral("0"));
    captureFpsValue_ = createMetricValue(QStringLiteral("0.0"));
    inferFpsValue_ = createMetricValue(QStringLiteral("0.0"));

    const QList<QPair<QString, QLabel*>> cards = {
        {QStringLiteral("服务"), serviceStateValue_},
        {QStringLiteral("会话"), sessionStateValue_},
        {QStringLiteral("Session ID"), sessionIdValue_},
        {QStringLiteral("当前占用"), occupancyValue_},
        {QStringLiteral("IN"), inValue_},
        {QStringLiteral("OUT"), outValue_},
        {QStringLiteral("画面人数"), livePersonsValue_},
        {QStringLiteral("采集 FPS"), captureFpsValue_},
        {QStringLiteral("推理 FPS"), inferFpsValue_}
    };
    for (int index = 0; index < cards.size(); ++index) {
        layout->addWidget(metricCard(cards[index].first, cards[index].second), index / 5, index % 5);
    }
    return widget;
}

QWidget* MainWindow::createSnapshotPanel() {
    auto* box = new QGroupBox(QStringLiteral("最新分析快照（1 FPS 刷新）"));
    auto* layout = new QVBoxLayout(box);
    snapshotLabel_ = new QLabel;
    snapshotLabel_->setAlignment(Qt::AlignCenter);
    snapshotLabel_->setMinimumSize(480, 300);
    snapshotLabel_->setStyleSheet(QStringLiteral("background:#111827; border-radius:6px; color:#cbd5e1;"));
    snapshotLabel_->setText(QStringLiteral("会话启动后显示快照"));
    snapshotHint_ = new QLabel(QStringLiteral("演示客户端使用 JPEG 快照，不承担视频转码。"));
    snapshotHint_->setStyleSheet(QStringLiteral("color:#64748b;"));
    layout->addWidget(snapshotLabel_, 1);
    layout->addWidget(snapshotHint_);
    return box;
}

QWidget* MainWindow::createSecurityPanel() {
    auto* box = new QGroupBox(QStringLiteral("同屏四阶段安全分析（阶段四为 DEMO）"));
    auto* layout = new QGridLayout(box);
    phase1Value_ = new QLabel(QStringLiteral("等待会话"));
    phase2Value_ = new QLabel(QStringLiteral("等待会话"));
    phase3Value_ = new QLabel(QStringLiteral("等待会话"));
    phase4Value_ = new QLabel(QStringLiteral("DEMO · 等待会话"));
    for (QLabel* value : {phase1Value_, phase2Value_, phase3Value_}) {
        value->setObjectName(QStringLiteral("securityStageValue"));
    }
    phase4Value_->setObjectName(QStringLiteral("demoStageValue"));
    layout->addWidget(metricCard(QStringLiteral("阶段一 · 电子围栏"), phase1Value_), 0, 0);
    layout->addWidget(metricCard(QStringLiteral("阶段二 · 人物追踪"), phase2Value_), 0, 1);
    layout->addWidget(metricCard(QStringLiteral("阶段三 · 姿态动作"), phase3Value_), 0, 2);
    layout->addWidget(metricCard(QStringLiteral("阶段四 · 时序动作"), phase4Value_), 0, 3);
    layout->setColumnStretch(0, 1);
    layout->setColumnStretch(1, 1);
    layout->setColumnStretch(2, 1);
    layout->setColumnStretch(3, 1);
    return box;
}

QWidget* MainWindow::createEventsPanel() {
    auto* box = new QGroupBox(QStringLiteral("最近事件"));
    auto* layout = new QVBoxLayout(box);
    auto* crossingTitle = new QLabel(QStringLiteral("过线计数事件"));
    crossingTitle->setObjectName(QStringLiteral("sectionTitle"));
    layout->addWidget(crossingTitle);
    eventsTable_ = new QTableWidget(0, 5);
    eventsTable_->setHorizontalHeaderLabels({QStringLiteral("时间"), QStringLiteral("方向"),
                                              QStringLiteral("轨迹"), QStringLiteral("置信度"),
                                              QStringLiteral("事件 ID")});
    eventsTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    eventsTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    eventsTable_->setAlternatingRowColors(true);
    eventsTable_->verticalHeader()->setVisible(false);
    eventsTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    eventsTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    eventsTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    eventsTable_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    eventsTable_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
    layout->addWidget(eventsTable_);

    auto* securityTitle = new QLabel(QStringLiteral("四阶段安全事件"));
    securityTitle->setObjectName(QStringLiteral("sectionTitle"));
    layout->addWidget(securityTitle);
    securityEventsTable_ = new QTableWidget(0, 6);
    securityEventsTable_->setHorizontalHeaderLabels({
        QStringLiteral("时间"), QStringLiteral("阶段"), QStringLiteral("事件"),
        QStringLiteral("轨迹"), QStringLiteral("置信度"), QStringLiteral("属性")
    });
    securityEventsTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    securityEventsTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    securityEventsTable_->setAlternatingRowColors(true);
    securityEventsTable_->verticalHeader()->setVisible(false);
    for (int column = 0; column < 5; ++column) {
        securityEventsTable_->horizontalHeader()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
    }
    securityEventsTable_->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Stretch);
    layout->addWidget(securityEventsTable_);
    return box;
}

QLabel* MainWindow::createMetricValue(const QString& initialValue) {
    auto* label = new QLabel(initialValue);
    label->setObjectName(QStringLiteral("metricValue"));
    label->setMinimumWidth(90);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return label;
}

void MainWindow::applyConnectionSettings() {
    if (!configureBaseUrl()) return;
    serviceStateValue_->setText(QStringLiteral("检查中"));
    appendLog(QStringLiteral("检查服务：%1").arg(api_->baseUrl().toString()));
    api_->checkHealth();
}

bool MainWindow::configureBaseUrl() {
    const QUrl url = QUrl::fromUserInput(baseUrlEdit_->text().trimmed());
    if (!url.isValid() || url.host().isEmpty()) {
        appendLog(QStringLiteral("服务地址无效。"));
        return false;
    }
    api_->setBaseUrl(url);
    return true;
}

void MainWindow::startSession() {
    if (!configureBaseUrl()) return;
    startButton_->setEnabled(false);
    sessionStateValue_->setText(QStringLiteral("提交中"));
    api_->startSession(cameraProfileEdit_->text().trimmed(), cameraIdEdit_->text().trimmed(),
                       configVersionEdit_->text().trimmed(), initialOccupancySpin_->value());
}

void MainWindow::stopSession() {
    if (sessionId_.isEmpty()) return;
    stopPending_ = true;
    updateSessionControls();
    api_->stopSession(sessionId_);
}

void MainWindow::refreshLiveData() {
    if (sessionId_.isEmpty()) return;
    ++refreshTick_;
    api_->fetchStatus(sessionId_);
    api_->fetchRealtime(cameraIdEdit_->text().trimmed());
    api_->fetchSnapshot(sessionId_);
    api_->fetchSecurity(sessionId_);
    if (refreshTick_ % 5 == 0) {
        api_->fetchEvents(cameraIdEdit_->text().trimmed());
    }
}

void MainWindow::showHealth(const QJsonObject& payload) {
    const bool ok = payload.value(QStringLiteral("success")).toBool();
    serviceStateValue_->setText(ok ? QStringLiteral("正常") : QStringLiteral("异常"));
    statusBar()->showMessage(ok ? QStringLiteral("后端服务可用") : QStringLiteral("后端返回异常状态"), 3000);
    appendLog(ok ? QStringLiteral("健康检查通过。") : QStringLiteral("健康检查未通过。"));
}

void MainWindow::showSessionStarted(const QJsonObject& payload) {
    stopPending_ = false;
    setSessionId(payload.value(QStringLiteral("session_id")).toString());
    sessionStateValue_->setText(payload.value(QStringLiteral("status")).toString(QStringLiteral("queued")));
    appendLog(QStringLiteral("会话已创建：%1").arg(sessionId_));
    refreshTick_ = 0;
    refreshLiveData();
}

void MainWindow::showSessionStopped(const QJsonObject& payload) {
    sessionStateValue_->setText(payload.value(QStringLiteral("status")).toString(QStringLiteral("stopping")));
    appendLog(QStringLiteral("已提交停止请求。"));
    updateSessionControls();
}

void MainWindow::showStatus(const QJsonObject& payload) {
    const QString state = payload.value(QStringLiteral("status")).toString();
    sessionStateValue_->setText(state);
    const QJsonObject flow = payload.value(QStringLiteral("flow")).toObject();
    if (!flow.isEmpty()) {
        inValue_->setText(displayNumber(flow.value(QStringLiteral("in"))));
        outValue_->setText(displayNumber(flow.value(QStringLiteral("out"))));
        occupancyValue_->setText(displayNumber(flow.value(QStringLiteral("occupancy"))));
    }
    const QJsonObject capture = payload.value(QStringLiteral("capture")).toObject();
    const QJsonObject inference = payload.value(QStringLiteral("inference")).toObject();
    captureFpsValue_->setText(displayNumber(capture.value(QStringLiteral("capture_fps")), 1));
    inferFpsValue_->setText(displayNumber(inference.value(QStringLiteral("infer_fps")), 1));
    livePersonsValue_->setText(displayNumber(inference.value(QStringLiteral("live_persons"))));

    if (state == QStringLiteral("stopped") || state == QStringLiteral("failed")) {
        appendLog(QStringLiteral("会话终态：%1").arg(state));
        setSessionId(QString());
    }
}

void MainWindow::showRealtime(const QJsonObject& payload) {
    inValue_->setText(displayNumber(payload.value(QStringLiteral("in"))));
    outValue_->setText(displayNumber(payload.value(QStringLiteral("out"))));
    occupancyValue_->setText(displayNumber(payload.value(QStringLiteral("occupancy"))));
    livePersonsValue_->setText(displayNumber(payload.value(QStringLiteral("live_persons"))));
    captureFpsValue_->setText(displayNumber(payload.value(QStringLiteral("capture_fps")), 1));
    inferFpsValue_->setText(displayNumber(payload.value(QStringLiteral("infer_fps")), 1));
}

void MainWindow::showSnapshot(const QByteArray& jpegBytes) {
    QPixmap pixmap;
    if (!pixmap.loadFromData(jpegBytes, "JPEG")) {
        snapshotHint_->setText(QStringLiteral("后端返回的快照无法解码。"));
        return;
    }
    snapshotLabel_->setPixmap(pixmap.scaled(snapshotLabel_->size(), Qt::KeepAspectRatio,
                                             Qt::SmoothTransformation));
    snapshotHint_->setText(QStringLiteral("快照更新时间：%1")
                               .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"))));
}

void MainWindow::showSecurity(const QJsonObject& payload) {
    const QJsonObject stages = payload.value(QStringLiteral("stages")).toObject();
    const QJsonObject phase1 = stages.value(QStringLiteral("phase1")).toObject();
    const QJsonObject phase2 = stages.value(QStringLiteral("phase2")).toObject();
    const QJsonObject phase3 = stages.value(QStringLiteral("phase3")).toObject();
    const QJsonObject phase4 = stages.value(QStringLiteral("phase4")).toObject();
    phase1Value_->setText(QStringLiteral("就绪 · 区内 %1 · 围栏 %2")
        .arg(displayNumber(phase1.value(QStringLiteral("inside_count"))))
        .arg(displayNumber(phase1.value(QStringLiteral("zone_count")))));
    phase2Value_->setText(QStringLiteral("就绪 · 轨迹 %1 · αβ预测")
        .arg(displayNumber(phase2.value(QStringLiteral("track_count")))));
    phase3Value_->setText(QStringLiteral("就绪 · 姿态 %1 · 动作 %2")
        .arg(displayNumber(phase3.value(QStringLiteral("pose_count"))))
        .arg(phase3.value(QStringLiteral("active_actions")).toArray().size()));
    phase4Value_->setText(QStringLiteral("DEMO · %1 · 动作 %2")
        .arg(phase4.value(QStringLiteral("label")).toString(QStringLiteral("RAPID_MOTION_DEMO")))
        .arg(phase4.value(QStringLiteral("active_actions")).toArray().size()));

    const QJsonArray events = payload.value(QStringLiteral("events")).toArray();
    securityEventsTable_->setRowCount(events.size());
    for (int row = 0; row < events.size(); ++row) {
        const QJsonObject event = events.at(row).toObject();
        const qint64 timestamp = event.value(QStringLiteral("event_time_ms")).toVariant().toLongLong();
        const QString category = event.value(QStringLiteral("category")).toString();
        QString stage = QStringLiteral("--");
        if (category == QStringLiteral("zone")) stage = QStringLiteral("阶段一");
        else if (category == QStringLiteral("pose_action")) stage = QStringLiteral("阶段三");
        else if (category == QStringLiteral("temporal_action")) stage = QStringLiteral("阶段四");
        const QStringList values = {
            QDateTime::fromMSecsSinceEpoch(timestamp).toString(QStringLiteral("HH:mm:ss")),
            stage,
            event.value(QStringLiteral("event_type")).toString(),
            displayNumber(event.value(QStringLiteral("track_id"))),
            displayNumber(event.value(QStringLiteral("confidence")), 2),
            event.value(QStringLiteral("demo_classifier")).toBool()
                ? QStringLiteral("DEMO")
                : event.value(QStringLiteral("zone_id")).toString()
        };
        for (int column = 0; column < values.size(); ++column) {
            securityEventsTable_->setItem(row, column, new QTableWidgetItem(values[column]));
        }
    }
}

void MainWindow::showEvents(const QJsonObject& payload) {
    const QJsonArray events = payload.value(QStringLiteral("events")).toArray();
    eventsTable_->setRowCount(events.size());
    for (int row = 0; row < events.size(); ++row) {
        const QJsonObject event = events.at(row).toObject();
        const qint64 timestamp = event.value(QStringLiteral("event_time_ms")).toVariant().toLongLong();
        const QString time = QDateTime::fromMSecsSinceEpoch(timestamp).toString(QStringLiteral("MM-dd HH:mm:ss"));
        const QStringList values = {
            time,
            event.value(QStringLiteral("direction")).toString(),
            displayNumber(event.value(QStringLiteral("track_id"))),
            displayNumber(event.value(QStringLiteral("confidence")), 2),
            event.value(QStringLiteral("event_id")).toString()
        };
        for (int column = 0; column < values.size(); ++column) {
            eventsTable_->setItem(row, column, new QTableWidgetItem(values[column]));
        }
    }
}

void MainWindow::showRequestError(const QString& operation, int httpStatus, const QString& message) {
    if (operation == QStringLiteral("health")) serviceStateValue_->setText(QStringLiteral("不可用"));
    if (operation == QStringLiteral("start")) sessionStateValue_->setText(QStringLiteral("启动失败"));
    if (operation == QStringLiteral("stop")) stopPending_ = false;
    appendLog(QStringLiteral("%1 失败（HTTP %2）：%3").arg(operation).arg(httpStatus).arg(message));
    updateSessionControls();
}

void MainWindow::setSessionId(const QString& sessionId) {
    sessionId_ = sessionId;
    if (sessionId_.isEmpty()) stopPending_ = false;
    sessionIdValue_->setText(sessionId_.isEmpty() ? QStringLiteral("--") : sessionId_);
    updateSessionControls();
}

void MainWindow::updateSessionControls() {
    const bool active = !sessionId_.isEmpty();
    startButton_->setEnabled(!active);
    stopButton_->setEnabled(active && !stopPending_);
    baseUrlEdit_->setEnabled(!active);
    cameraProfileEdit_->setEnabled(!active);
    cameraIdEdit_->setEnabled(!active);
    configVersionEdit_->setEnabled(!active);
    initialOccupancySpin_->setEnabled(!active);
}

void MainWindow::appendLog(const QString& text) {
    const QString timestamp = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"));
    logEdit_->append(QStringLiteral("[%1] %2").arg(timestamp, text));
}

void MainWindow::loadSettings(const QString& initialBaseUrl) {
    QSettings settings;
    baseUrlEdit_->setText(initialBaseUrl.isEmpty()
                              ? settings.value(QStringLiteral("baseUrl"), QStringLiteral("http://127.0.0.1:8087")).toString()
                              : initialBaseUrl);
    cameraProfileEdit_->setText(settings.value(QStringLiteral("cameraProfile"), QStringLiteral("entry_camera_01")).toString());
    cameraIdEdit_->setText(settings.value(QStringLiteral("cameraId"), QStringLiteral("entry_camera_01")).toString());
    QString savedConfigVersion = settings.value(
        QStringLiteral("configVersion"), QStringLiteral("entry-line-v3-security-demo")).toString();
    if (savedConfigVersion == QStringLiteral("entry-line-v2")) {
        savedConfigVersion = QStringLiteral("entry-line-v3-security-demo");
    }
    configVersionEdit_->setText(savedConfigVersion);
    initialOccupancySpin_->setValue(settings.value(QStringLiteral("initialOccupancy"), 0).toInt());
}

void MainWindow::saveSettings() const {
    QSettings settings;
    settings.setValue(QStringLiteral("baseUrl"), baseUrlEdit_->text().trimmed());
    settings.setValue(QStringLiteral("cameraProfile"), cameraProfileEdit_->text().trimmed());
    settings.setValue(QStringLiteral("cameraId"), cameraIdEdit_->text().trimmed());
    settings.setValue(QStringLiteral("configVersion"), configVersionEdit_->text().trimmed());
    settings.setValue(QStringLiteral("initialOccupancy"), initialOccupancySpin_->value());
}

void MainWindow::closeEvent(QCloseEvent* event) {
    saveSettings();
    QMainWindow::closeEvent(event);
}

QString MainWindow::displayNumber(const QJsonValue& value, int decimals) {
    if (value.isDouble()) return QString::number(value.toDouble(), 'f', decimals);
    if (value.isString()) return value.toString();
    return QStringLiteral("--");
}
