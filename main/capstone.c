#include "driver/spi_slave.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_websocket_client.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

#include "esp_netif.h"
#include "esp_eth.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_eth_driver.h"
#include "esp_check.h"
#include "esp_mac.h"

#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"
#include "lwip/ip4_addr.h"
#include "lwip/inet.h"

#include "esp_http_server.h"

// --- SPI Configuration ---
#define PIN_NUM_MISO 19
#define PIN_NUM_MOSI 23
#define PIN_NUM_CLK 18
#define PIN_NUM_CS 5
#define MESSAGE_SIZE 16 // 6 bytes per SPI transaction

static const char *SPI_TAG = "spi_slave";
static const char *WS_TAG = "websocket";

#define IP "ws://10.141.12.4:8765" //This changes depending on hotspot used (ipconfig on local Laptop)

#include "my_data.h"

// Queue handle for passing SPI data (each item is an array of MESSAGE_SIZE bytes)
QueueHandle_t spiQueue;

static httpd_handle_t server = NULL;
static const char *TAG = "websocket_server";

static int g_ws_fd = -1;

static esp_err_t websocket_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Handler invoked");
    if (req->method == HTTP_GET) {
        //HTTP GET here is the WebSocket handshake.
        ESP_LOGI(TAG, "WebSocket handshake complete, new connection opened");
        
        g_ws_fd = httpd_req_to_sockfd(req);

        ESP_LOGI(TAG, "G_REQ set to: %d", g_ws_fd);
        
        return ESP_OK;
    }

    //After handshake, handle frames
    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(ws_pkt));
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;

    //First call with buff = NULL to get frame length
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_ws_recv_frame failed: %d", ret);
        if (ret == ESP_ERR_INVALID_STATE) {
            ESP_LOGI(TAG, "WebSocket client disconnected");
            //g_ws_req = NULL;
        }
        return ret;
    }

    if (ws_pkt.len) {
        ws_pkt.payload = malloc(ws_pkt.len + 1);
        if (!ws_pkt.payload) {
            return ESP_ERR_NO_MEM;
        }

        ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
        if (ret != ESP_OK) {
            ws_pkt.payload[ws_pkt.len] = 0; // Null-terminate the payload
            ESP_LOGI(TAG, "Received WebSocket message: %s", (char *)ws_pkt.payload);       
        }
        free(ws_pkt.payload);
    }

    return ret;
}

void websocket_send(const char* message) {

    if (server == NULL || g_ws_fd < 0) {
        ESP_LOGW(WS_TAG, "WebSocket server not started or no client connected");
        return;
    }

    httpd_ws_frame_t ws_pkt;

    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.payload = (uint8_t *)message;
    ws_pkt.len = strlen(message);
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;

    esp_err_t ret = httpd_ws_send_frame_async(server, g_ws_fd, &ws_pkt); 
    if (ret != ESP_OK) {
        ESP_LOGE(WS_TAG, "Failed to send WebSocket message: %s", esp_err_to_name(ret));
    }
}

static esp_err_t start_websocket_server() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 8765;

    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t ws_uri = {
            .uri = "/ws",
            .method = HTTP_GET,
            .handler = websocket_handler,
            .user_ctx = NULL,
            .is_websocket = true
        };

        httpd_register_uri_handler(server, &ws_uri);
        ESP_LOGI("HTTP", "WebSocket server started on ws://192.168.4.1:8765/ws");
        return ESP_OK;
    }
    ESP_LOGE(WS_TAG, "Failed to start WebSocket server");
    return ESP_FAIL;
}

// --- SPI Task ---
void spi_task(void *pvParameters)
{
    // SPI Bus configuration
    spi_bus_config_t buscfg = {
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = PIN_NUM_MISO,
        .sclk_io_num = PIN_NUM_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = MESSAGE_SIZE,
    };

    // SPI Slave interface configuration
    spi_slave_interface_config_t slvcfg = {
        .mode = 0,
        .spics_io_num = PIN_NUM_CS,
        .queue_size = 3,
        .flags = 0,
        .post_setup_cb = NULL,
        .post_trans_cb = NULL,
    };

    // Initialize SPI slave
    esp_err_t ret = spi_slave_initialize(HSPI_HOST, &buscfg, &slvcfg, 0);
    if (ret != ESP_OK)
    {
        ESP_LOGE(SPI_TAG, "Failed to initialize SPI slave: %s", esp_err_to_name(ret));
        vTaskDelete(NULL);
    }
    ESP_LOGI(SPI_TAG, "SPI slave initialized");

    uint8_t rx_buffer[MESSAGE_SIZE];
    memset(rx_buffer, 0, sizeof(rx_buffer));

    while (1)
    {
        spi_slave_transaction_t t;
        memset(&t, 0, sizeof(t));
        t.length = MESSAGE_SIZE * 8; // bits
        t.rx_buffer = rx_buffer;
        t.tx_buffer = NULL;

        // Block until a transaction is received.
        ret = spi_slave_transmit(HSPI_HOST, &t, portMAX_DELAY);
        if (ret == ESP_OK)
        {
            if (t.trans_len != MESSAGE_SIZE * 8) {
                ESP_LOGW(SPI_TAG, "Bad frame: got %d bits (expected %d)",
                         t.trans_len, MESSAGE_SIZE*8);
                memset(rx_buffer, 0, sizeof(rx_buffer));
                continue; 
            }

            ESP_LOGI(SPI_TAG, "Received SPI data: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
                     rx_buffer[0], rx_buffer[1], rx_buffer[2],
                     rx_buffer[3], rx_buffer[4], rx_buffer[5],
                     rx_buffer[6], rx_buffer[7], rx_buffer[8],
                     rx_buffer[9], rx_buffer[10], rx_buffer[11],
                     rx_buffer[12], rx_buffer[13], rx_buffer[14], rx_buffer[15]);

            // Send the received data to the queue
            if (xQueueSend(spiQueue, &rx_buffer, portMAX_DELAY) != pdPASS)
            {
                ESP_LOGE(SPI_TAG, "Failed to send SPI data to queue");
            }

            // Clear the buffer for the next transaction
            memset(rx_buffer, 0, sizeof(rx_buffer));
        }
        else
        {
            ESP_LOGE(SPI_TAG, "SPI transmit error: %s", esp_err_to_name(ret));
        }
    }
}

static void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    switch (event_id)
    {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(WS_TAG, "WebSocket Connected");
        break;

    case WEBSOCKET_EVENT_DATA:
        ESP_LOGI(WS_TAG, "Received WebSocket frame: fin=%d, op_code=%d, len=%d", data->fin, data->op_code, data->data_len);
        if (data->op_code == 0x9)
        { // WebSocket Ping frame
            ESP_LOGI(WS_TAG, "Received WebSocket Ping Frame, ignoring...");
            break;
        }
        if (data->op_code == 0xA)
        { // WebSocket Pong frame
            ESP_LOGI(WS_TAG, "Received WebSocket Pong Frame, ignoring...");
            break;
        }

        if (data->data_len == 0)
        {
            ESP_LOGW(WS_TAG, "Received empty message, ignoring...");
            break;
        }

        // Ensure we only process complete messages
        if (data->fin == 0)
        { // If not final fragment, log warning and ignore
            ESP_LOGW(WS_TAG, "Received fragmented frame, ignoring...");
            break;
        }

        // Allocate memory dynamically (+1 for null terminator)
        char *received_message = malloc(data->data_len + 1);
        if (received_message == NULL)
        {
            ESP_LOGE(WS_TAG, "Memory allocation failed!");
            break;
        }

        memcpy(received_message, data->data_ptr, data->data_len);
        received_message[data->data_len] = '\0'; // Null-terminate the string

        ESP_LOGI(WS_TAG, "Received from server: %s", received_message);

        free(received_message); // Free allocated memory
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGI(WS_TAG, "WebSocket Disconnected");
        break;
    }
}

// --- WebSocket Task ---
void websocket_task(void *pvParameters) {

    if (start_websocket_server() != ESP_OK) {
        ESP_LOGE(WS_TAG, "Failed to start WebSocket server");
        vTaskDelete(NULL); // Terminate the task if the server fails to start
    }


    uint8_t spiData[MESSAGE_SIZE];

    while (1) {
        // Wait indefinitely until new SPI data is available from the queue
        if (xQueueReceive(spiQueue, &spiData, portMAX_DELAY) == pdTRUE) {
            // Convert the SPI data into a string
            char message[48];
            int len = snprintf(message, sizeof(message),
                     "%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
                     spiData[0], spiData[1], spiData[2], spiData[3],
                     spiData[4], spiData[5], spiData[6], spiData[7],
                     spiData[8], spiData[9], spiData[10], spiData[11],
                     spiData[12], spiData[13], spiData[14], spiData[15]);

            ESP_LOGI(WS_TAG, "Sending WebSocket message: %s", message);
            websocket_send(message);

            // Send the message to all connected WebSocket clients
            //httpd_req_t *req = (httpd_req_t *)pvParameters; this was a bit wonky

            
        }
    }
}

// --- Wi-Fi Initialization ---
static void wifi_event_handler(void *event_handler_arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    switch (event_id)
    {
    case WIFI_EVENT_STA_START:
        ESP_LOGI("WIFI", "WiFi connecting ...");
        break;
    case WIFI_EVENT_STA_CONNECTED:
        ESP_LOGI("WIFI", "WiFi connected");
        break;
    case WIFI_EVENT_STA_DISCONNECTED:
        ESP_LOGI("WIFI", "WiFi disconnected");
        break;
    case IP_EVENT_STA_GOT_IP:
        ESP_LOGI("WIFI", "Got IP");
        break;
    default:
        break;
    }
}

void wifi_connection() {
    
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    //Create a default Wi-Fi AP
    esp_netif_create_default_wifi_ap();

    //Initialize Wi-Fi with def config
    wifi_init_config_t wifi_init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_init_cfg));

    //Config SSID/PWD
    wifi_config_t wifi_ap_config = {
        .ap = {
            .ssid = "DrivingSim",          // Network SSID
            .ssid_len = strlen("DrivingSim"),
            .password = "88888888",          // Network password
            .channel = 1,                    // Wi-Fi channel
            .max_connection = 4,             // Max clients
            .authmode = WIFI_AUTH_WPA2_PSK,  // Security mode
        },
    };

    //Catch for OpenAuth if !PWD
    if (strlen((char *)wifi_ap_config.ap.password) == 0) {
        wifi_ap_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    //Set ESP to AP mode and start
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_AP, &wifi_ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI("WIFI", "Wi-Fi Access Point started. SSID: %s, Password: %s",
             wifi_ap_config.ap.ssid, wifi_ap_config.ap.password);
}

void app_main(void)
{
    // 1. Initialize Wi-Fi (needed for WebSocket)
    wifi_connection();
    //ethernet_setup();
    vTaskDelay(pdMS_TO_TICKS(1000)); // Give time for Wi-Fi connection

    // 2. Create a queue for SPI data (queue length 10)
    spiQueue = xQueueCreate(1, MESSAGE_SIZE * sizeof(uint8_t));
    if (spiQueue == NULL)
    {
        ESP_LOGE("MAIN", "Failed to create SPI queue");
        return;
    }

    // 3. Create the SPI task
    xTaskCreate(spi_task, "spi_task", 4096, NULL, 5, NULL);

    // 4. Create the WebSocket task
    xTaskCreate(websocket_task, "websocket_task", 8192, NULL, 5, NULL);
}