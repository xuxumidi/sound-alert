#pragma once
// Copy to secrets.h, edit locally, and never share your authorization code.
namespace config {
constexpr char WIFI_SSID[] = "";
constexpr char WIFI_PASSWORD[] = "";
constexpr char WIFI_SSID_2[] = ""; // Optional second network; leave empty to disable.
constexpr char WIFI_PASSWORD_2[] = "";
constexpr char SMTP_HOST[] = "smtp.qq.com";
constexpr unsigned SMTP_PORT = 465; // Implicit TLS only; not STARTTLS/587.
constexpr char SMTP_USER[] = "";
constexpr char SMTP_AUTH_CODE[] = "";
constexpr char MAIL_FROM[] = "";
constexpr char MAIL_TO[] = "";
constexpr bool MAIL_ENABLED = false; // Explicitly enable after configuring local credentials.
// PEM root CA for your SMTP server; certificate validation is mandatory.
constexpr char SMTP_ROOT_CA[] = "";
constexpr float MATCH_THRESHOLD = 0.72f; // Field-tested starting point; tune to your environment.
constexpr int MIC_BCLK = 4, MIC_WS = 5, MIC_DATA = 6;
constexpr int MIC_SAMPLE_SHIFT = 16; // I2S 24-bit sample left-aligned in 32-bit word.
constexpr bool MIC_RIGHT_CHANNEL = false; // Microphone L/R tied to GND for left.
constexpr int SD_SCK = 12, SD_MISO = 13, SD_MOSI = 11, SD_CS = 10;
constexpr unsigned MIN_FREE_MB = 16;
constexpr bool KEEP_ACCEPTED_RECORDINGS = false; // Mail retains the attachment; free SD after SMTP acceptance.
}
