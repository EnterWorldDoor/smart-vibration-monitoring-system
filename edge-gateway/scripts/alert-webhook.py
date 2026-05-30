#!/usr/bin/env python3
"""
EdgeVib Alert Webhook Bridge
Receives Prometheus Alertmanager webhook payloads and publishes to MQTT.

Pipeline:
  Prometheus Alertmanager → HTTP POST (webhook JSON) → This script → MQTT EdgeVib/system/monitoring/alert

The api-server WebSocket hub can consume this topic and push alerts to Web UI.
Future: digital_io alarm_service can consume for GPIO-based audio/visual alarm.

Usage:
  python alert-webhook.py --mqtt-broker mosquitto --port 8092
"""

import argparse
import json
import sys
from http.server import HTTPServer, BaseHTTPRequestHandler

import paho.mqtt.client as mqtt


MQTT_TOPIC = "EdgeVib/system/monitoring/alert"


class AlertHandler(BaseHTTPRequestHandler):
    mqtt_client: mqtt.Client = None

    def do_POST(self):
        content_length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(content_length)

        try:
            payload = json.loads(body)
        except json.JSONDecodeError:
            self.send_response(400)
            self.end_headers()
            self.wfile.write(b'{"status":"invalid json"}')
            return

        # Alertmanager webhook format:
        # { "receiver": "...", "status": "firing|resolved",
        #   "alerts": [{ "labels": {...}, "annotations": {...}, "startsAt": "..." }] }
        alerts = payload.get("alerts", [])
        for alert in alerts:
            status = payload.get("status", "firing")
            labels = alert.get("labels", {})
            annotations = alert.get("annotations", {})

            mqtt_msg = json.dumps({
                "severity": labels.get("severity", "unknown"),
                "alert_name": labels.get("alertname", "unknown"),
                "status": status,
                "summary": annotations.get("summary", ""),
                "description": annotations.get("description", ""),
                "timestamp": alert.get("startsAt", ""),
            })

            if self.mqtt_client:
                self.mqtt_client.publish(MQTT_TOPIC, mqtt_msg, qos=1)
                print(f"[alert] {status}: {labels.get('alertname')} ({labels.get('severity')})")

        self.send_response(200)
        self.end_headers()
        self.wfile.write(b'{"status":"ok"}')

    def log_message(self, format, *args):
        print(f"[webhook] {args[0]}")


def main():
    parser = argparse.ArgumentParser(description="EdgeVib Alert Webhook Bridge")
    parser.add_argument("--mqtt-broker", default="mosquitto", help="MQTT broker hostname")
    parser.add_argument("--mqtt-port", type=int, default=1883, help="MQTT broker port")
    parser.add_argument("--port", type=int, default=8092, help="HTTP listen port")
    args = parser.parse_args()

    # Connect MQTT
    mqtt_client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="edgevib-alert-webhook")
    mqtt_client.connect(args.mqtt_broker, args.mqtt_port, keepalive=60)
    mqtt_client.loop_start()
    print(f"[mqtt] Connected to {args.mqtt_broker}:{args.mqtt_port}")

    AlertHandler.mqtt_client = mqtt_client

    server = HTTPServer(("0.0.0.0", args.port), AlertHandler)
    print(f"[webhook] Listening on :{args.port}")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n[webhook] Shutting down")
        mqtt_client.loop_stop()
        mqtt_client.disconnect()
        server.shutdown()


if __name__ == "__main__":
    main()
