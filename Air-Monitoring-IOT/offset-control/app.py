from flask import Flask, request, render_template_string
import paho.mqtt.publish as publish
import os

app = Flask(__name__)
MQTT_SERVER = os.environ.get("MQTT_SERVER", "mosquitto")
MQTT_PORT = 1883

DEVICES = {
    "BME280 (Arsip Utama)": "arsip/sensor/cmd",
    "DHT11 (Node Baru)": "arsip/sensor_dht11/cmd"
}

PAGE = """
<!DOCTYPE html>
<html>
<head>
  <title>Kalibrasi Sensor</title>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <style>
    body { font-family: sans-serif; max-width: 400px; margin: 40px auto; padding: 0 20px; }
    h2 { text-align: center; }
    label { display: block; margin-top: 15px; font-weight: bold; }
    input, select { width: 100%; padding: 10px; font-size: 16px; margin-top: 5px; }
    button { width: 100%; padding: 12px; margin-top: 20px; font-size: 16px; background: #2563eb; color: white; border: none; border-radius: 6px; }
    .msg { text-align: center; margin-top: 15px; color: green; }
  </style>
</head>
<body>
  <h2>Kalibrasi Sensor Jarak Jauh</h2>
  <form method="POST">
    <label>Pilih Perangkat:</label>
    <select name="device">
      {% for name in devices %}
        <option value="{{ name }}">{{ name }}</option>
      {% endfor %}
    </select>

    <label>Offset Suhu (°C):</label>
    <input type="number" step="0.1" name="temp_offset" placeholder="contoh: 0.5" required>

    <label>Offset Kelembapan (%):</label>
    <input type="number" step="1" name="hum_offset" placeholder="contoh: -2" required>

    <button type="submit">Kirim ke Sensor</button>
  </form>
  {% if message %}
    <div class="msg">{{ message }}</div>
  {% endif %}
</body>
</html>
"""

@app.route("/", methods=["GET", "POST"])
def index():
    message = None
    if request.method == "POST":
        device = request.form["device"]
        temp_offset = request.form["temp_offset"]
        hum_offset = request.form["hum_offset"]
        topic = DEVICES[device]

        payload = f'{{"temp_offset": {temp_offset}, "hum_offset": {hum_offset}}}'
        publish.single(topic, payload, hostname=MQTT_SERVER, port=MQTT_PORT)
        message = f"Berhasil dikirim ke {device}!"

    return render_template_string(PAGE, devices=DEVICES.keys(), message=message)

if __name__ == "__main__":
    app.run(host="0.0.0.0", port=5002)