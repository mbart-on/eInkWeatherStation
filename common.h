#pragma once

#include <Arduino.h>
#include <esp_http_client.h>
#include <ArduinoJson.h>

#include <string>
#include <vector>
#include <ctime>
#include <iomanip>
#include <esp_http_client.h>
#include "esp_crt_bundle.h"

static const char* HA_TOKEN = "<insert_berrer_here>";
static const char* HA_BASE  = "http://HA_IP_Here:8123";

/* ============================================================
   Pomocnicza funkcja HTTP GET (IDF native)
   ============================================================ */
static String http_get(String url, bool auth = false)
{
    ESP_LOGD("HTTP", "URL: %s", url.c_str());

    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.timeout_ms = 4000;

    if (url.startsWith("https://")) {
        config.transport_type = HTTP_TRANSPORT_OVER_SSL;
        config.crt_bundle_attach = esp_crt_bundle_attach;
    }

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE("HTTP", "Client init failed");
        return "";
    }

    if (auth) {
        String header = "Bearer ";
        header += HA_TOKEN;
        esp_http_client_set_header(client, "Authorization", header.c_str());
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE("HTTP", "Open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return "";
    }

    int status = esp_http_client_fetch_headers(client);
    ESP_LOGD("HTTP", "Status code: %d", status);

    String response = "";
    char buffer[512];
    int read_len = 0;

    while ((read_len = esp_http_client_read(client, buffer, sizeof(buffer)-1)) > 0) {
        buffer[read_len] = 0;
        response += buffer;
    }

    ESP_LOGD("HTTP", "Payload size: %d", response.length());

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    return response;
}

/* ============================================================
   1. Aktualna wartość z HA
   ============================================================ */
String getCurrentWeather(String sensor)
{
    String url = String(HA_BASE) + "/api/states/" + sensor;
    String payload = http_get(url, true);
    if (payload.isEmpty()) return "-1";

    DynamicJsonDocument doc(2048);
    DeserializationError err = deserializeJson(doc, payload);
    if (err) return "-1";

    return doc["state"].as<String>();
}

float getCurrentValue(String sensor)
{
    String url = String(HA_BASE) + "/api/states/" + sensor;

    String payload = http_get(url, true);
    if (payload.isEmpty()) {
        ESP_LOGW("HA", "Empty payload for %s", sensor.c_str());
        return -1;
    }

    DynamicJsonDocument doc(1024);
    DeserializationError error = deserializeJson(doc, payload);

    if (error) {
        ESP_LOGW("HA", "JSON parse error for %s", sensor.c_str());
        return -1;
    }

    if (doc["state"].isNull()) {
        ESP_LOGW("HA", "No state field for %s", sensor.c_str());
        return -1;
    }

    String state_value = doc["state"].as<String>();
    float state_value_as_int = state_value.toFloat();

    ESP_LOGD("HA", "%s = %.2f", sensor.c_str(), state_value_as_int);

    return state_value_as_int;
}

/* ============================================================
   2. Historia (jedno wywołanie)
   ============================================================ */
float generateTimeShiftedValue(int hours_shift, int granulity, String sensor)
{
    std::time_t now = std::time(nullptr);
    std::time_t end_time = now - (hours_shift * granulity * 60);
    std::time_t start_time = end_time - (granulity * 60);

    char start_buf[40];
    char end_buf[40];

    strftime(start_buf, sizeof(start_buf), "%Y-%m-%dT%H:%M:%S%z", localtime(&start_time));
    strftime(end_buf, sizeof(end_buf), "%Y-%m-%dT%H:%M:%S%z", localtime(&end_time));

    String start = String(start_buf);
    String end   = String(end_buf);

    start.replace(":", "%3A");
    start.replace("+", "%2B");
    end.replace(":", "%3A");
    end.replace("+", "%2B");

    String url = String(HA_BASE) + "/api/history/period/" + start +
                 "?end_time=" + end +
                 "&filter_entity_id=" + sensor +
                 "&minimal_response&no_attributes";

    String payload = http_get(url, true);
    if (payload.isEmpty()) return -100;

    DynamicJsonDocument doc(4096);
    DeserializationError err = deserializeJson(doc, payload);
    if (err) return -100;

    if (doc.size() > 0 && doc[0].size() > 0)
        return doc[0][0]["state"].as<String>().toFloat();

    return -100;
}

/* ============================================================
   3. Open Meteo
   ============================================================ */
std::vector<std::vector<String>> getCurrentWeatherOpenMeteo(int threshold)
{
    String url = "https://api.open-meteo.com/v1/forecast?latitude=52.2546&longitude=20.9084&current=weather_code,wind_speed_10m,wind_direction_10m";

    String payload = http_get(url);
    if (payload.isEmpty()) return {};

    DynamicJsonDocument doc(4096);
    DeserializationError error = deserializeJson(doc, payload);
    if (error) return {};

    JsonObject current = doc["current"];

    int weatherCode = current["weather_code"];
    float windSpeed = current["wind_speed_10m"];
    int windDirection = current["wind_direction_10m"];

    std::vector<std::vector<String>> result(1, std::vector<String>(5));

    result[0][0] = String(weatherCode);
    result[0][1] = String(windSpeed, 1);
    result[0][2] = String(windDirection);
    result[0][3] = "";
    result[0][4] = "";

    // ===== Kierunek wiatru =====
    if (windDirection >= 0 && windDirection < 22) result[0][4] = "\uF058";
    else if (windDirection < 67) result[0][4] = "\uF057";
    else if (windDirection < 112) result[0][4] = "\uF04D";
    else if (windDirection < 157) result[0][4] = "\uF088";
    else if (windDirection < 202) result[0][4] = "\uF044";
    else if (windDirection < 247) result[0][4] = "\uF043";
    else if (windDirection < 292) result[0][4] = "\uF048";
    else result[0][4] = "\uF087";

    // ===== Ikona pogody =====
    if (threshold > 5 && threshold < 20)
    {
        if (weatherCode == 0 || weatherCode == 1) result[0][3] = "\uF00D";
        else if (weatherCode == 2 || weatherCode == 3) result[0][3] = "\uF00C";
        else if (weatherCode == 45 || weatherCode == 48) result[0][3] = "\uF003";
        else if (weatherCode == 51 || weatherCode == 53 || weatherCode == 55) result[0][3] = "\uF009";
        else if (weatherCode == 56 || weatherCode == 57) result[0][3] = "\uF017";
        else if (weatherCode == 61 || weatherCode == 63 || weatherCode == 65) result[0][3] = "\uF019";
        else if (weatherCode == 66 || weatherCode == 67) result[0][3] = "\uF01B";
        else if (weatherCode == 71 || weatherCode == 73 || weatherCode == 75) result[0][3] = "\uF01B";
        else if (weatherCode == 77) result[0][3] = "\uF01B";
        else if (weatherCode == 80 || weatherCode == 81 || weatherCode == 82) result[0][3] = "\uF01A";
        else if (weatherCode == 85 || weatherCode == 86) result[0][3] = "\uF01B";
        else if (weatherCode == 95) result[0][3] = "\uF01D";
        else if (weatherCode == 96 || weatherCode == 99) result[0][3] = "\uF01E";
    }
    else
    {
        if (weatherCode == 0 || weatherCode == 1) result[0][3] = "\uF02E";
        else if (weatherCode == 2 || weatherCode == 3) result[0][3] = "\uF086";
        else if (weatherCode == 45 || weatherCode == 48) result[0][3] = "\uF04A";
        else if (weatherCode == 51 || weatherCode == 53 || weatherCode == 55) result[0][3] = "\uF029";
        else if (weatherCode == 56 || weatherCode == 57) result[0][3] = "\uF017";
        else if (weatherCode == 61 || weatherCode == 63 || weatherCode == 65) result[0][3] = "\uF019";
        else if (weatherCode == 66 || weatherCode == 67) result[0][3] = "\uF01B";
        else if (weatherCode == 71 || weatherCode == 73 || weatherCode == 75) result[0][3] = "\uF01B";
        else if (weatherCode == 77) result[0][3] = "\uF01B";
        else if (weatherCode == 80 || weatherCode == 81 || weatherCode == 82) result[0][3] = "\uF01A";
        else if (weatherCode == 85 || weatherCode == 86) result[0][3] = "\uF01B";
        else if (weatherCode == 95) result[0][3] = "\uF01D";
        else if (weatherCode == 96 || weatherCode == 99) result[0][3] = "\uF01E";
    }

    return result;
}

/* ============================================================
   4. Godzinowa prognoza
   ============================================================ */
std::vector<std::vector<String>> getHourlyWeather()
{
    String url = "https://api.open-meteo.com/v1/forecast?latitude=52.2546&longitude=20.9084&hourly=temperature_2m,weather_code&forecast_days=1";

    String payload = http_get(url);
    if (payload.isEmpty()) return {};

    DynamicJsonDocument doc(16384);   // 48k niepotrzebne – 16k w zupełności wystarczy
    DeserializationError error = deserializeJson(doc, payload);
    if (error) return {};

    JsonArray temperatureArray = doc["hourly"]["temperature_2m"].as<JsonArray>();
    JsonArray timeArray = doc["hourly"]["time"].as<JsonArray>();
    JsonArray weatherCodeArray = doc["hourly"]["weather_code"].as<JsonArray>();

    int dataSize = temperatureArray.size();
    std::vector<std::vector<String>> result(dataSize, std::vector<String>(3));

    for (int i = 0; i < dataSize; i++)
    {
        float temperature = temperatureArray[i];
        String time = timeArray[i].as<String>();
        int weatherCode = weatherCodeArray[i];

        result[i][0] = time;
        result[i][1] = String(temperature, 1);
        result[i][2] = String(weatherCode);

        // ===== Mapowanie ikon (dzień/noc wg indeksu) =====
        if (i > 5 && i < 20)
        {
            if (weatherCode == 0 || weatherCode == 1) result[i][2] = "\uF00D";
            else if (weatherCode == 2 || weatherCode == 3) result[i][2] = "\uF00C";
            else if (weatherCode == 45 || weatherCode == 48) result[i][2] = "\uF003";
            else if (weatherCode == 51 || weatherCode == 53 || weatherCode == 55) result[i][2] = "\uF009";
            else if (weatherCode == 56 || weatherCode == 57) result[i][2] = "\uF017";
            else if (weatherCode == 61 || weatherCode == 63 || weatherCode == 65) result[i][2] = "\uF019";
            else if (weatherCode == 66 || weatherCode == 67) result[i][2] = "\uF01B";
            else if (weatherCode == 71 || weatherCode == 73 || weatherCode == 75) result[i][2] = "\uF01B";
            else if (weatherCode == 77) result[i][2] = "\uF01B";
            else if (weatherCode == 80 || weatherCode == 81 || weatherCode == 82) result[i][2] = "\uF01A";
            else if (weatherCode == 85 || weatherCode == 86) result[i][2] = "\uF01B";
            else if (weatherCode == 95) result[i][2] = "\uF01D";
            else if (weatherCode == 96 || weatherCode == 99) result[i][2] = "\uF01E";
        }
        else
        {
            if (weatherCode == 0 || weatherCode == 1) result[i][2] = "\uF02E";
            else if (weatherCode == 2 || weatherCode == 3) result[i][2] = "\uF086";
            else if (weatherCode == 45 || weatherCode == 48) result[i][2] = "\uF04A";
            else if (weatherCode == 51 || weatherCode == 53 || weatherCode == 55) result[i][2] = "\uF029";
            else if (weatherCode == 56 || weatherCode == 57) result[i][2] = "\uF017";
            else if (weatherCode == 61 || weatherCode == 63 || weatherCode == 65) result[i][2] = "\uF019";
            else if (weatherCode == 66 || weatherCode == 67) result[i][2] = "\uF01B";
            else if (weatherCode == 71 || weatherCode == 73 || weatherCode == 75) result[i][2] = "\uF01B";
            else if (weatherCode == 77) result[i][2] = "\uF01B";
            else if (weatherCode == 80 || weatherCode == 81 || weatherCode == 82) result[i][2] = "\uF01A";
            else if (weatherCode == 85 || weatherCode == 86) result[i][2] = "\uF01B";
            else if (weatherCode == 95) result[i][2] = "\uF01D";
            else if (weatherCode == 96 || weatherCode == 99) result[i][2] = "\uF01E";
        }
    }

    return result;
}
/* ============================================================
   5. Pozostałe funkcje
   ============================================================ */
std::string getDayOfWeek(int offset)
{
    std::time_t now = std::time(nullptr) + offset * 86400;
    std::tm* tm = std::localtime(&now);
    const char* days[] = { "Sun", "Mon", "Tu", "Wed", "Th", "Fr", "Sat" };
    return days[tm->tm_wday];
}

std::string testowaMetoda(std::string temperature)
{
    return temperature + " Test";
}
