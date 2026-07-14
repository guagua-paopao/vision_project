#pragma once

#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QObject>
#include <QUrl>

class QNetworkReply;

class PeopleFlowApiClient final : public QObject {
    Q_OBJECT

public:
    explicit PeopleFlowApiClient(QObject* parent = nullptr);

    void setBaseUrl(const QUrl& baseUrl);
    QUrl baseUrl() const;

    void checkHealth();
    void startSession(const QString& cameraProfile,
                      const QString& cameraId,
                      const QString& configVersion,
                      int initialOccupancy);
    void stopSession(const QString& sessionId);
    void fetchStatus(const QString& sessionId);
    void fetchRealtime(const QString& cameraId);
    void fetchSnapshot(const QString& sessionId);
    void fetchSecurity(const QString& sessionId);
    void fetchEvents(const QString& cameraId, int limit = 50);

signals:
    void healthReceived(const QJsonObject& payload);
    void sessionStarted(const QJsonObject& payload);
    void sessionStopped(const QJsonObject& payload);
    void statusReceived(const QJsonObject& payload);
    void realtimeReceived(const QJsonObject& payload);
    void snapshotReceived(const QByteArray& jpegBytes);
    void securityReceived(const QJsonObject& payload);
    void eventsReceived(const QJsonObject& payload);
    void requestFailed(const QString& operation, int httpStatus, const QString& message);

private:
    QUrl endpoint(const QString& path) const;
    void sendJson(const QString& operation,
                  const QByteArray& method,
                  const QString& path,
                  const QJsonObject& body = {});
    void finishJson(const QString& operation, QNetworkReply* reply);
    static QString errorMessage(const QByteArray& body, const QString& fallback);

    QNetworkAccessManager network_;
    QUrl baseUrl_{QStringLiteral("http://127.0.0.1:8087")};
};
