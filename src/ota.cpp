#include <Arduino.h>
#include <website.h>
#include <commonFS.h>
#include "scale.h"
#include "bambu.h"
#include "nfc.h"


// Globale Variablen für Config Backups hinzufügen
String bambuCredentialsBackup;
String spoolmanUrlBackup;

// Globale Variable für den Update-Typ
static int currentUpdateCommand = 0;

// Globale Update-Variablen
static size_t updateTotalSize = 0;
static size_t updateWritten = 0;
static bool isSpiffsUpdate = false;

/**
 * Compares two version strings and determines if version1 is less than version2
 * 
 * @param version1 First version string (format: x.y.z)
 * @param version2 Second version string (format: x.y.z)
 * @return true if version1 is less than version2
 */
bool isVersionLessThan(const String& version1, const String& version2) {
    int major1 = 0, minor1 = 0, patch1 = 0;
    int major2 = 0, minor2 = 0, patch2 = 0;
    
    // Parse version1
    sscanf(version1.c_str(), "%d.%d.%d", &major1, &minor1, &patch1);
    
    // Parse version2
    sscanf(version2.c_str(), "%d.%d.%d", &major2, &minor2, &patch2);
    
    // Compare major version
    if (major1 < major2) return true;
    if (major1 > major2) return false;
    
    // Major versions equal, compare minor
    if (minor1 < minor2) return true;
    if (minor1 > minor2) return false;
    
    // Minor versions equal, compare patch
    return patch1 < patch2;
}

void backupJsonConfigs() {
    // Bambu Credentials backup
    if (LittleFS.exists("/bambu_credentials.json")) {
        File file = LittleFS.open("/bambu_credentials.json", "r");
        if (file) {
            bambuCredentialsBackup = file.readString();
            file.close();
            Serial.println("Bambu credentials backed up");
        }
    }

    // Spoolman URL backup
    if (LittleFS.exists("/spoolman_url.json")) {
        File file = LittleFS.open("/spoolman_url.json", "r");
        if (file) {
            spoolmanUrlBackup = file.readString();
            file.close();
            Serial.println("Spoolman URL backed up");
        }
    }
}

void restoreJsonConfigs() {
    // Restore Bambu credentials
    if (bambuCredentialsBackup.length() > 0) {
        File file = LittleFS.open("/bambu_credentials.json", "w");
        if (file) {
            file.print(bambuCredentialsBackup);
            file.close();
            Serial.println("Bambu credentials restored");
        }
        bambuCredentialsBackup = ""; // Clear backup
    }

    // Restore Spoolman URL
    if (spoolmanUrlBackup.length() > 0) {
        File file = LittleFS.open("/spoolman_url.json", "w");
        if (file) {
            file.print(spoolmanUrlBackup);
            file.close();
            Serial.println("Spoolman URL restored");
        }
        spoolmanUrlBackup = ""; // Clear backup
    }
}

void espRestart() {
    yield();
    vTaskDelay(5000 / portTICK_PERIOD_MS);

    ESP.restart();
}


void sendUpdateProgress(int progress, const char* status = nullptr, const char* message = nullptr) {
    static int lastSentProgress = -1;
    
    // Verhindere zu häufige Updates
    if (progress == lastSentProgress && !status && !message) {
        return;
    }
    
    String progressMsg = "{\"type\":\"updateProgress\",\"progress\":" + String(progress);
    if (status) {
        progressMsg += ",\"status\":\"" + String(status) + "\"";
    }
    if (message) {
        progressMsg += ",\"message\":\"" + String(message) + "\"";
    }
    progressMsg += "}";
    
    if (progress >= 100) {
        // Sende die Nachricht nur einmal für den Abschluss
        ws.textAll("{\"type\":\"updateProgress\",\"progress\":100,\"status\":\"success\",\"message\":\"Update successful! Restarting device...\"}");
        delay(50);
    }

    // Sende die Nachricht mehrmals mit Verzögerung für wichtige Updates
    if (status || abs(progress - lastSentProgress) >= 10 || progress == 100) {
        for (int i = 0; i < 2; i++) {
            ws.textAll(progressMsg);
            delay(100);  // Längerer Delay zwischen Nachrichten
        }
    } else {
        ws.textAll(progressMsg);
        delay(50);
    }
    
    lastSentProgress = progress;
}

void handleUpdate(AsyncWebServer &server) {
    AsyncCallbackWebHandler* updateHandler = new AsyncCallbackWebHandler();
    updateHandler->setUri("/update");
    updateHandler->setMethod(HTTP_POST);
    
    // Check if current version is less than defined TOOLVERSION before proceeding with update
    if (isVersionLessThan(VERSION, TOOLDVERSION)) {
        updateHandler->onRequest([](AsyncWebServerRequest *request) {
            request->send(400, "application/json", 
                "{\"success\":false,\"message\":\"Your current version is too old. Please perform a full upgrade.\"}");
        });
        server.addHandler(updateHandler);
        return;
    }

    updateHandler->onUpload([](AsyncWebServerRequest *request, String filename,
                             size_t index, uint8_t *data, size_t len, bool final) {

        // Disable all Tasks
        if (BambuMqttTask != NULL) 
        {
            Serial.println("Delete BambuMqttTask");
            vTaskDelete(BambuMqttTask);
            BambuMqttTask = NULL;
        }
        if (ScaleTask) {
            Serial.println("Delete ScaleTask");
            vTaskDelete(ScaleTask);
            ScaleTask = NULL;
        }
        if (RfidReaderTask) {
            Serial.println("Delete RfidReaderTask");
            vTaskDelete(RfidReaderTask);
            RfidReaderTask = NULL;
        }

        if (!index) {
            updateTotalSize = request->contentLength();
            updateWritten = 0;
            isSpiffsUpdate = (filename.indexOf("website") > -1);
            
            if (isSpiffsUpdate) {
                // Backup vor dem Update
                sendUpdateProgress(0, "backup", "Backing up configurations...");
                vTaskDelay(200 / portTICK_PERIOD_MS);
                backupJsonConfigs();
                vTaskDelay(200 / portTICK_PERIOD_MS);
                
                const esp_partition_t *partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, NULL);
                if (!partition || !Update.begin(partition->size, U_SPIFFS)) {
                    request->send(400, "application/json", "{\"success\":false,\"message\":\"Update initialization failed\"}");
                    return;
                }
                sendUpdateProgress(5, "starting", "Starting SPIFFS update...");
                vTaskDelay(200 / portTICK_PERIOD_MS);
            } else {
                if (!Update.begin(updateTotalSize)) {
                    request->send(400, "application/json", "{\"success\":false,\"message\":\"Update initialization failed\"}");
                    return;
                }
                sendUpdateProgress(0, "starting", "Starting firmware update...");
                vTaskDelay(200 / portTICK_PERIOD_MS);
            }
        }

        if (len) {
            if (Update.write(data, len) != len) {
                request->send(400, "application/json", "{\"success\":false,\"message\":\"Write failed\"}");
                return;
            }
            
            updateWritten += len;
            int currentProgress;
            
            // Berechne den Fortschritt basierend auf dem Update-Typ
            if (isSpiffsUpdate) {
                // SPIFFS: 5-75% für Upload
                currentProgress = 6 + (updateWritten * 100) / updateTotalSize;
            } else {
                // Firmware: 0-100% für Upload
                currentProgress = 1 + (updateWritten * 100) / updateTotalSize;
            }
            
            static int lastProgress = -1;
            if (currentProgress != lastProgress && (currentProgress % 10 == 0 || final)) {
                sendUpdateProgress(currentProgress, "uploading");
                oledShowMessage("Update: " + String(currentProgress) + "%");
                vTaskDelay(50 / portTICK_PERIOD_MS);
                lastProgress = currentProgress;
            }
        }

        if (final) {
            if (Update.end(true)) {
                if (isSpiffsUpdate) {
                    restoreJsonConfigs();
                }
            } else {
                request->send(400, "application/json", "{\"success\":false,\"message\":\"Update finalization failed\"}");
            }
        }
    });

    updateHandler->onRequest([](AsyncWebServerRequest *request) {
        if (Update.hasError()) {
            request->send(400, "application/json", "{\"success\":false,\"message\":\"Update failed\"}");
            return;
        }

        // Erste 100% Nachricht
        ws.textAll("{\"type\":\"updateProgress\",\"progress\":100,\"status\":\"success\",\"message\":\"Update successful! Restarting device...\"}");
        vTaskDelay(2000 / portTICK_PERIOD_MS);
        
        AsyncWebServerResponse *response = request->beginResponse(200, "application/json", 
            "{\"success\":true,\"message\":\"Update successful! Restarting device...\"}");
        response->addHeader("Connection", "close");
        request->send(response);
        
        // Zweite 100% Nachricht zur Sicherheit
        ws.textAll("{\"type\":\"updateProgress\",\"progress\":100,\"status\":\"success\",\"message\":\"Update successful! Restarting device...\"}");
        
        espRestart();
    });

    server.addHandler(updateHandler);
}