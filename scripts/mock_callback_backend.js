"use strict";

const crypto = require("crypto");
const http = require("http");

const host = "127.0.0.1";
const port = Number.parseInt(
  process.env.YOLO11_MOCK_CALLBACK_PORT || "9095",
  10,
);
const secret = process.env.YOLO11_MOCK_CALLBACK_SECRET || "";
const controlToken = process.env.YOLO11_MOCK_CALLBACK_CONTROL_TOKEN || "";
const failFirst = Math.max(
  0,
  Number.parseInt(process.env.YOLO11_MOCK_CALLBACK_FAIL_FIRST || "0", 10),
);
const timestampToleranceMs = Math.max(
  1000,
  Number.parseInt(
    process.env.YOLO11_MOCK_CALLBACK_TIMESTAMP_TOLERANCE_MS || "300000",
    10,
  ),
);
const maxBodyBytes = 1024 * 1024;

if (!Number.isInteger(port) || port < 1 || port > 65535) {
  throw new Error("YOLO11_MOCK_CALLBACK_PORT must be a valid TCP port");
}
if (Buffer.byteLength(secret, "utf8") < 16) {
  throw new Error("YOLO11_MOCK_CALLBACK_SECRET must contain at least 16 bytes");
}
if (Buffer.byteLength(controlToken, "utf8") < 8) {
  throw new Error(
    "YOLO11_MOCK_CALLBACK_CONTROL_TOKEN must contain at least 8 bytes",
  );
}

const attempts = new Map();
const accepted = new Map();

function jsonResponse(response, status, body) {
  const bytes = Buffer.from(JSON.stringify(body), "utf8");
  response.writeHead(status, {
    "Content-Type": "application/json; charset=utf-8",
    "Content-Length": bytes.length,
    "Cache-Control": "no-store",
    "X-Content-Type-Options": "nosniff",
  });
  response.end(bytes);
}

function authorized(request) {
  const authorization = request.headers.authorization || "";
  const expected = `Bearer ${controlToken}`;
  const left = Buffer.from(authorization, "utf8");
  const right = Buffer.from(expected, "utf8");
  return left.length === right.length && crypto.timingSafeEqual(left, right);
}

function readBody(request, response, callback) {
  let size = 0;
  const chunks = [];
  request.on("data", (chunk) => {
    size += chunk.length;
    if (size > maxBodyBytes) {
      jsonResponse(response, 413, {
        success: false,
        error_code: "REQUEST_TOO_LARGE",
      });
      request.destroy();
      return;
    }
    chunks.push(chunk);
  });
  request.on("end", () => {
    if (!response.writableEnded) callback(Buffer.concat(chunks));
  });
  request.on("error", () => {
    if (!response.writableEnded) {
      jsonResponse(response, 400, {
        success: false,
        error_code: "REQUEST_READ_FAILED",
      });
    }
  });
}

function signatureValid(timestamp, body, supplied) {
  if (!/^[0-9]{10,16}$/.test(timestamp)) return false;
  if (!/^[0-9a-f]{64}$/.test(supplied)) return false;
  const expected = crypto
    .createHmac("sha256", Buffer.from(secret, "utf8"))
    .update(timestamp, "utf8")
    .update("\n", "utf8")
    .update(body)
    .digest("hex");
  return crypto.timingSafeEqual(
    Buffer.from(supplied, "ascii"),
    Buffer.from(expected, "ascii"),
  );
}

function callback(request, response) {
  readBody(request, response, (body) => {
    const eventId = String(request.headers["x-event-id"] || "");
    const idempotencyKey = String(request.headers["idempotency-key"] || "");
    const timestamp = String(request.headers["x-timestamp"] || "");
    const signatureVersion = String(
      request.headers["x-signature-version"] || "",
    );
    const signature = String(request.headers["x-signature"] || "");
    const timestampNumber = Number.parseInt(timestamp, 10);
    if (
      !eventId ||
      eventId !== idempotencyKey ||
      signatureVersion !== "1" ||
      !Number.isFinite(timestampNumber) ||
      Math.abs(Date.now() - timestampNumber) > timestampToleranceMs ||
      !signatureValid(timestamp, body, signature)
    ) {
      jsonResponse(response, 401, {
        success: false,
        error_code: "CALLBACK_AUTHENTICATION_FAILED",
      });
      return;
    }

    let payload;
    try {
      payload = JSON.parse(body.toString("utf8"));
    } catch {
      jsonResponse(response, 400, {
        success: false,
        error_code: "INVALID_JSON",
      });
      return;
    }
    if (
      !payload ||
      typeof payload !== "object" ||
      payload.event_id !== eventId
    ) {
      jsonResponse(response, 400, {
        success: false,
        error_code: "EVENT_ID_MISMATCH",
      });
      return;
    }

    const attempt = (attempts.get(eventId) || 0) + 1;
    attempts.set(eventId, attempt);
    if (attempt <= failFirst) {
      jsonResponse(response, 503, {
        success: false,
        error_code: "MOCK_RETRYABLE_FAILURE",
        attempt,
      });
      return;
    }
    if (accepted.has(eventId)) {
      jsonResponse(response, 200, {
        success: true,
        duplicate: true,
        event_id: eventId,
        attempt,
      });
      return;
    }

    accepted.set(eventId, {
      event_id: eventId,
      received_at_ms: Date.now(),
      attempt,
      payload,
    });
    process.stdout.write(`accepted event_id=${eventId} attempt=${attempt}\n`);
    response.writeHead(204, {
      "Cache-Control": "no-store",
      "X-Content-Type-Options": "nosniff",
    });
    response.end();
  });
}

const server = http.createServer((request, response) => {
  const url = new URL(request.url, `http://${host}:${port}`);
  if (request.method === "GET" && url.pathname === "/health") {
    jsonResponse(response, 200, {
      success: true,
      service: "vision-project-mock-callback",
      fail_first: failFirst,
      accepted_events: accepted.size,
    });
    return;
  }
  if (
    request.method === "POST" &&
    url.pathname === "/api/v1/algorithm-alerts"
  ) {
    callback(request, response);
    return;
  }
  if (!authorized(request)) {
    jsonResponse(response, 401, {
      success: false,
      error_code: "UNAUTHORIZED",
    });
    return;
  }
  if (request.method === "GET" && url.pathname === "/api/test/events") {
    jsonResponse(response, 200, {
      success: true,
      items: Array.from(accepted.values()),
    });
    return;
  }
  if (request.method === "POST" && url.pathname === "/api/test/reset") {
    attempts.clear();
    accepted.clear();
    jsonResponse(response, 200, { success: true });
    return;
  }
  jsonResponse(response, 404, {
    success: false,
    error_code: "NOT_FOUND",
  });
});

server.listen(port, host, () => {
  process.stdout.write(
    `mock callback backend listening on http://${host}:${port}\n`,
  );
});

function shutdown() {
  server.close(() => process.exit(0));
  setTimeout(() => process.exit(1), 5000).unref();
}

process.on("SIGINT", shutdown);
process.on("SIGTERM", shutdown);
