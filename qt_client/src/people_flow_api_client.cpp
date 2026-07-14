#include "people_flow_api_client.h"

#include <QJsonDocument>
#include <QJsonParseError>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrlQuery>

namespace {
QString encodedSegment(const QString& value) {
    return QString::fromLatin1(QUrl::toPercentEncoding(value));
}
}

PeopleFlowApiClient::PeopleFlowApiClient(QObject* parent)
    : QObject(parent) {}

void PeopleFlowApiClient::setBaseUrl(const QUrl& baseUrl) {
    if (!baseUrl.isValid() || baseUrl.scheme().isEmpty() || baseUrl.host().isEmpty()) {
        return;
    }
    baseUrl_ = baseUrl;
}

QUrl PeopleFlowApiClient::baseUrl() const {
    return baseUrl_;
}

QUrl PeopleFlowApiClient::endpoint(const QString& path) const {
    QString root = baseUrl_.toString(QUrl::RemovePath | QUrl::RemoveQuery | QUrl::RemoveFragment);
    QString basePath = baseUrl_.path();
    if (basePath.endsWith('/')) {
        basePath.chop(1);
    }
    return QUrl(root + basePath + path);
}

void PeopleFlowApiClient::checkHealth() {
    sendJson(QStringLiteral("health"), "GET", QStringLiteral("/api/v1/health"));
}

void PeopleFlowApiClient::startSession(const QString& cameraProfile,
                                       const QString& cameraId,
                                       const QString& configVersion,
                                       int initialOccupancy) {
    sendJson(QStringLiteral("start"), "POST", QStringLiteral("/api/v1/people-flow/start"), {
        {QStringLiteral("camera_profile"), cameraProfile},
        {QStringLiteral("camera_id"), cameraId},
        {QStringLiteral("config_version"), configVersion},
        {QStringLiteral("initial_occupancy"), initialOccupancy}
    });
}

void PeopleFlowApiClient::stopSession(const QString& sessionId) {
    sendJson(QStringLiteral("stop"), "POST",
             QStringLiteral("/api/v1/people-flow/%1/stop").arg(encodedSegment(sessionId)));
}

void PeopleFlowApiClient::fetchStatus(const QString& sessionId) {
    sendJson(QStringLiteral("status"), "GET",
             QStringLiteral("/api/v1/people-flow/%1/status").arg(encodedSegment(sessionId)));
}

void PeopleFlowApiClient::fetchRealtime(const QString& cameraId) {
    sendJson(QStringLiteral("realtime"), "GET",
             QStringLiteral("/api/v1/people-flow/cameras/%1/realtime").arg(encodedSegment(cameraId)));
}

void PeopleFlowApiClient::fetchSnapshot(const QString& sessionId) {
    QNetworkRequest request(endpoint(
        QStringLiteral("/api/v1/people-flow/%1/snapshot").arg(encodedSegment(sessionId))));
    request.setRawHeader("Accept", "image/jpeg");
    request.setRawHeader("Cache-Control", "no-cache");
    QNetworkReply* reply = network_.get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray body = reply->readAll();
        if (reply->error() == QNetworkReply::NoError && status >= 200 && status < 300) {
            emit snapshotReceived(body);
        } else if (status != 404) {
            emit requestFailed(QStringLiteral("snapshot"), status,
                               errorMessage(body, reply->errorString()));
        }
        reply->deleteLater();
    });
}

void PeopleFlowApiClient::fetchSecurity(const QString& sessionId) {
    sendJson(QStringLiteral("security"), "GET",
             QStringLiteral("/api/v1/people-flow/%1/security").arg(encodedSegment(sessionId)));
}

void PeopleFlowApiClient::fetchEvents(const QString& cameraId, int limit) {
    QUrl url = endpoint(QStringLiteral("/api/v1/people-flow/cameras/%1/events")
                            .arg(encodedSegment(cameraId)));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("limit"), QString::number(qBound(1, limit, 1000)));
    url.setQuery(query);

    QNetworkRequest request(url);
    request.setRawHeader("Accept", "application/json");
    QNetworkReply* reply = network_.get(request);
    connect(reply, &QNetworkReply::finished, this,
            [this, reply]() { finishJson(QStringLiteral("events"), reply); });
}

void PeopleFlowApiClient::sendJson(const QString& operation,
                                   const QByteArray& method,
                                   const QString& path,
                                   const QJsonObject& body) {
    QNetworkRequest request(endpoint(path));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader("Accept", "application/json");
    const QByteArray payload = body.isEmpty() ? QByteArray() : QJsonDocument(body).toJson(QJsonDocument::Compact);

    QNetworkReply* reply = nullptr;
    if (method == "GET") {
        reply = network_.get(request);
    } else {
        reply = network_.sendCustomRequest(request, method, payload);
    }
    connect(reply, &QNetworkReply::finished, this,
            [this, operation, reply]() { finishJson(operation, reply); });
}

void PeopleFlowApiClient::finishJson(const QString& operation, QNetworkReply* reply) {
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray body = reply->readAll();
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
    const bool ok = reply->error() == QNetworkReply::NoError && status >= 200 && status < 300;

    if (operation == QStringLiteral("security") && status == 404) {
        reply->deleteLater();
        return;
    }

    if (!ok || parseError.error != QJsonParseError::NoError || !document.isObject()) {
        const QString fallback = parseError.error == QJsonParseError::NoError
            ? reply->errorString()
            : QStringLiteral("响应不是有效 JSON：%1").arg(parseError.errorString());
        emit requestFailed(operation, status, errorMessage(body, fallback));
        reply->deleteLater();
        return;
    }

    const QJsonObject payload = document.object();
    if (operation == QStringLiteral("health")) emit healthReceived(payload);
    else if (operation == QStringLiteral("start")) emit sessionStarted(payload);
    else if (operation == QStringLiteral("stop")) emit sessionStopped(payload);
    else if (operation == QStringLiteral("status")) emit statusReceived(payload);
    else if (operation == QStringLiteral("realtime")) emit realtimeReceived(payload);
    else if (operation == QStringLiteral("security")) emit securityReceived(payload);
    else if (operation == QStringLiteral("events")) emit eventsReceived(payload);
    reply->deleteLater();
}

QString PeopleFlowApiClient::errorMessage(const QByteArray& body, const QString& fallback) {
    const QJsonDocument document = QJsonDocument::fromJson(body);
    if (!document.isObject()) return fallback;
    const QJsonObject object = document.object();
    const QString code = object.value(QStringLiteral("error_code")).toString();
    const QString message = object.value(QStringLiteral("error")).toString(fallback);
    return code.isEmpty() ? message : QStringLiteral("%1：%2").arg(code, message);
}
