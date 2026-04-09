#include <Wire.h>
#include <Adafruit_MLX90640.h>

Adafruit_MLX90640 mlx;
float frame[32 * 24]; // Buffer for the 768 pixels

void setup() {
  Serial.begin(115200);
  delay(3000); // Wait 3 seconds so you have time to open the Serial Monitor

  Serial.println("--- MLX90640 Bare-Bones Test ---");

  // 1. Initialize I2C for XIAO ESP32-C6
  Wire.begin(D4, D5);
  
  // 2. CRITICAL FIX: Set I2C to 400kHz to handle the massive EEPROM dump
  Wire.setClock(400000); 

  // 3. Initialize the sensor
  Serial.println("Pinging sensor at 0x33...");
  if (!mlx.begin(MLX90640_I2CADDR_DEFAULT, &Wire)) {
    Serial.println("FAILED to start MLX90640. Halting program.");
    while (1) delay(10);
  }
  
  Serial.println("Sensor successfully initialized!");

  // 4. Configure sensor settings
  mlx.setMode(MLX90640_CHESS);
  mlx.setResolution(MLX90640_ADC_18BIT);
  mlx.setRefreshRate(MLX90640_2_HZ); // 2 frames per second is plenty for the Serial Monitor
}

void loop() {
  // Try to grab a frame
  if (mlx.getFrame(frame) != 0) {
    Serial.println("Error: Failed to read a frame from the sensor.");
    return;
  }

  // Variables to track the hot/cold spots
  float maxTemp = -100.0;
  float minTemp = 1000.0;
  
  // Scan all 768 pixels to find the min and max
  for (int i = 0; i < 768; i++) {
    if (frame[i] > maxTemp) maxTemp = frame[i];
    if (frame[i] < minTemp) minTemp = frame[i];
  }

  // Grab the temperature of the dead-center pixel (Row 12, Column 16)
  float centerTemp = frame[12 * 32 + 16];

  // Print the data cleanly
  Serial.print("Center: ");
  Serial.print(centerTemp, 1);
  Serial.print(" C  |  ");
  
  Serial.print("Coldest: ");
  Serial.print(minTemp, 1);
  Serial.print(" C  |  ");
  
  Serial.print("Hottest: ");
  Serial.print(maxTemp, 1);
  Serial.println(" C");

  delay(500); // Wait half a second
}