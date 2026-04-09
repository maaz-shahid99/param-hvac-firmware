#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <Adafruit_MLX90640.h>

// ─── WI-FI CREDENTIALS ──────────────────────────────────────────────────────
const char* ssid = "CMF";
const char* password = "12345678";
// ────────────────────────────────────────────────────────────────────────────

WebServer server(80);
Adafruit_MLX90640 mlx;
float frame[32 * 24]; 
bool sensorReady = false; 

// HTML, CSS, and JavaScript for the browser interface
// HTML, CSS, and JavaScript for the browser interface
const char* htmlPage = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>MLX90640 Thermal Camera</title>
  <style>
    body { font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; text-align: center; background: #121212; color: #ffffff; margin: 0; padding: 20px; }
    h2 { color: #00e5ff; }
    canvas { 
        background: #000; 
        margin-top: 10px; 
        border-radius: 8px; 
        box-shadow: 0px 4px 20px rgba(0, 229, 255, 0.2); 
        max-width: 100%; 
        height: auto; 
        filter: blur(2px) contrast(1.2); 
    }
    #status { margin-top: 15px; font-size: 1.2em; font-weight: bold; }
    .error { color: #ff3d00; }
    .footer { margin-top: 20px; font-size: 0.9em; color: #888; }
  </style>
</head>
<body>
  <h2>XIAO Thermal Live Feed</h2>
  <canvas id="thermalCanvas" width="640" height="480"></canvas>
  <div id="status">Waiting for sensor data...</div>
  <div class="footer">MLX90640 &bull; Denoised 60FPS</div>

  <script>
    const canvas = document.getElementById('thermalCanvas');
    const ctx = canvas.getContext('2d');
    const cols = 32;
    const rows = 24;
    const blockW = canvas.width / cols;
    const blockH = canvas.height / rows;

    let targetTemps = new Array(768).fill(25); 
    let currentTemps = new Array(768).fill(25); 
    
    // Variables to smooth the color scale itself
    let globalMin = 20;
    let globalMax = 30;

    function getColor(val, min, max) {
      let ratio = (val - min) / (max - min);
      ratio = Math.max(0, Math.min(1, ratio)); 
      let hue = (1 - ratio) * 240; 
      return `hsl(${hue}, 100%, 50%)`;
    }

    async function fetchFrame() {
      try {
        const response = await fetch('/data');
        if (response.status === 503) {
            document.getElementById('status').innerHTML = "<span class='error'>Sensor not detected!</span>";
        } else if (!response.ok) {
            throw new Error("Network response was not ok");
        } else {
            targetTemps = await response.json(); 
        }
      } catch (error) {
        console.error("Error fetching frame:", error);
      }
      setTimeout(fetchFrame, 250); 
    }

    function renderLoop() {
      let rawMin = Math.min(...targetTemps);
      let rawMax = Math.max(...targetTemps);

      // FIX 1: Enforce a minimum temperature spread of 3°C. 
      // This stops the camera from hyper-focusing on the noise of a flat wall.
      if (rawMax - rawMin < 3.0) {
          rawMax = rawMin + 3.0;
      }

      // FIX 2: Smoothly adjust the color scale bounds so the background doesn't flash
      globalMin += (rawMin - globalMin) * 0.1;
      globalMax += (rawMax - globalMax) * 0.1;

      document.getElementById('status').innerText = `Min: ${globalMin.toFixed(1)}°C | Max: ${globalMax.toFixed(1)}°C`;

      for (let i = 0; i < rows; i++) {
        for (let j = 0; j < cols; j++) {
          let idx = i * cols + j;
          let diff = targetTemps[idx] - currentTemps[idx];
          
          // FIX 3: THE NOISE GATE
          // Only update the pixel if the temperature shifted by more than 0.4°C.
          // This completely kills the "boiling" effect on static objects.
          if (Math.abs(diff) > 0.4) {
             currentTemps[idx] += diff * 0.15;
          }
          
          ctx.fillStyle = getColor(currentTemps[idx], globalMin, globalMax);
          ctx.fillRect((cols - 1 - j) * blockW, i * blockH, blockW + 1, blockH + 1);
        }
      }
      requestAnimationFrame(renderLoop); 
    }

    fetchFrame();
    renderLoop();
  </script>
</body>
</html>
)rawliteral";

void handleRoot() {
  server.send(200, "text/html", htmlPage);
}

void handleData() {
  if (!sensorReady) {
    server.send(503, "text/plain", "Sensor not found");
    return;
  }

  if (mlx.getFrame(frame) != 0) {
    server.send(500, "text/plain", "Sensor read error");
    return;
  }

  String json;
  json.reserve(6000); 
  json += "[";
  for (int i = 0; i < 768; i++) {
    json += String(frame[i], 1); 
    if (i < 767) json += ",";
  }
  json += "]";
  
  server.send(200, "application/json", json);
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.print("Connecting to Wi-Fi: ");
  Serial.println(ssid);
  
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(ssid, password);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nConnected to Wi-Fi!");
    Serial.print("--- GO TO THIS IP ADDRESS: ");
    Serial.print(WiFi.localIP());
    Serial.println(" ---");
  } else {
    Serial.println("\n--- WI-FI CONNECTION FAILED ---");
    while(1) delay(100); 
  }
  
  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.begin();
  Serial.println("Web server started!");

  // Keep I2C safely at 400kHz and 4Hz
  Wire.begin(D4, D5); 
  Wire.setClock(400000); 
  Serial.println("Initializing MLX90640...");
  
  if (!mlx.begin(MLX90640_I2CADDR_DEFAULT, &Wire)) {
    Serial.println("WARNING: MLX90640 not found. Check wiring!");
    sensorReady = false; 
  } else {
    Serial.println("MLX90640 successfully initialized!");
    mlx.setMode(MLX90640_CHESS);
    mlx.setResolution(MLX90640_ADC_18BIT);
    mlx.setRefreshRate(MLX90640_4_HZ); // Safe, reliable 4Hz hardware limit
    sensorReady = true; 
  }
}

void loop() {
  server.handleClient();
  delay(2);
}