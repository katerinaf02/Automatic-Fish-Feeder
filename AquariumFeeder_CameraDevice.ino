#include "esp_camera.h"
#include <WiFi.h>
#include "esp_http_server.h"
#include "esp32s3/rom/rtc.h"

// -------- WIFI ----------
const char* ssid = "";
const char* password = "";

// -------- STATIC IP CONFIG ----------
IPAddress local_IP(192,168,1,50);   // ESP IP
IPAddress gateway(192,168,1,1);      // Router IP
IPAddress subnet(255,255,255,0);
IPAddress primaryDNS(8,8,8,8);
IPAddress secondaryDNS(8,8,4,4);

// -------- CAMERA PIN MAP (XIAO ESP32 S3 Sense) ----------
#define PWDN_GPIO_NUM     -1
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM     10
#define SIOD_GPIO_NUM     40
#define SIOC_GPIO_NUM     39

#define Y9_GPIO_NUM       48
#define Y8_GPIO_NUM       11
#define Y7_GPIO_NUM       12
#define Y6_GPIO_NUM       14
#define Y5_GPIO_NUM       16
#define Y4_GPIO_NUM       18
#define Y3_GPIO_NUM       17
#define Y2_GPIO_NUM       15
#define VSYNC_GPIO_NUM    38
#define HREF_GPIO_NUM     47
#define PCLK_GPIO_NUM     13

httpd_handle_t stream_httpd = NULL;

// ---------- STREAM HANDLER ----------
static esp_err_t stream_handler(httpd_req_t *req){

    camera_fb_t * fb = NULL;
    char part_buf[64];

    httpd_resp_set_type(req, "multipart/x-mixed-replace;boundary=frame");

    while(true){

        fb = esp_camera_fb_get();
        if(!fb){
            Serial.println("Camera capture failed");
            return ESP_FAIL;
        }

        size_t hlen = snprintf(part_buf, 64,
                               "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
                               fb->len);

        httpd_resp_send_chunk(req, part_buf, hlen);
        httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
        httpd_resp_send_chunk(req, "\r\n", 2);

        esp_camera_fb_return(fb);
    }

    return ESP_OK;
}

// ---------- START SERVER ----------
void startCameraServer(){

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    httpd_uri_t stream_uri = {
        .uri       = "/stream",
        .method    = HTTP_GET,
        .handler   = stream_handler,
        .user_ctx  = NULL
    };

    httpd_start(&stream_httpd, &config);
    httpd_register_uri_handler(stream_httpd, &stream_uri);
}

// ---------- CAMERA SETUP ----------
void setupCamera(){

    camera_config_t config;
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer   = LEDC_TIMER_0;

    config.pin_d0 = Y2_GPIO_NUM;
    config.pin_d1 = Y3_GPIO_NUM;
    config.pin_d2 = Y4_GPIO_NUM;
    config.pin_d3 = Y5_GPIO_NUM;
    config.pin_d4 = Y6_GPIO_NUM;
    config.pin_d5 = Y7_GPIO_NUM;
    config.pin_d6 = Y8_GPIO_NUM;
    config.pin_d7 = Y9_GPIO_NUM;

    config.pin_xclk = XCLK_GPIO_NUM;
    config.pin_pclk = PCLK_GPIO_NUM;
    config.pin_vsync = VSYNC_GPIO_NUM;
    config.pin_href = HREF_GPIO_NUM;

    config.pin_sccb_sda = SIOD_GPIO_NUM;
    config.pin_sccb_scl = SIOC_GPIO_NUM;

    config.pin_pwdn  = PWDN_GPIO_NUM;
    config.pin_reset = RESET_GPIO_NUM;

    config.xclk_freq_hz = 10000000;
    config.pixel_format = PIXFORMAT_JPEG;

    config.frame_size = FRAMESIZE_QVGA;
    config.jpeg_quality = 15;
    config.fb_count = 2;

    if(esp_camera_init(&config) != ESP_OK){
        Serial.println("Camera init failed");
        return;
    }

    // --- Adjust image settings ---
    sensor_t * s = esp_camera_sensor_get();  // <--- inside function
    //s->set_brightness(s, 2);   // brightest
    s->set_contrast(s, 1);     //  contrast
    s->set_sharpness(s, 2);    // sharp
    s->set_saturation(s, 1);   // optional
    //s->set_gainceiling(s, GAINCEILING_16X); // optional
    s->set_exposure_ctrl(s, 1);     // enable auto exposure
    
}



// ---------- SETUP ----------
void setup() {

    Serial.begin(115200);

    // Configure static IP BEFORE WiFi.begin
    if (!WiFi.config(local_IP, gateway, subnet, primaryDNS, secondaryDNS)) {
        Serial.println("Static IP Failed to configure");
    }

    setupCamera();

    WiFi.begin(ssid, password);

    Serial.print("Connecting to WiFi");
    while (WiFi.status() != WL_CONNECTED){
        delay(500);
        Serial.print(".");
    }

    Serial.println();
    Serial.println("WiFi connected");
    Serial.print("ESP IP: ");
    Serial.println(WiFi.localIP());

    startCameraServer();

    Serial.print("Stream URL: http://");
    Serial.print(WiFi.localIP());
    Serial.println("/stream");
}


// ---------- LOOP ----------
unsigned long lastTempPrint = 0;
void loop() {
    // Print internal temperature every 2 seconds
    static unsigned long lastTempPrint = 0;

    if(millis() - lastTempPrint >= 10000){
        lastTempPrint = millis();

        float tempC = temperatureRead(); // approximate °C
        Serial.print("Internal temperature: ");
        Serial.print(tempC, 1); // 1 decimal place
        Serial.println(" °C");
    }
}