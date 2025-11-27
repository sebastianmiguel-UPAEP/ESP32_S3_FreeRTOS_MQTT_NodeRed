
// 	Publicador_MQTT
/* * --------------------------------------------------------------------------
 * 	PROYECTO: NODO SENSOR ESP32-S3 (MQTT + ADC One-Shot)
 * --------------------------------------------------------------------------
 * 	DESCRIPCIÓN:
 * 	Este código gestiona la lectura de sensores y la comunicación inalámbrica.
 * 	Realiza la conexión a WiFi y posteriormente se conecta a un Broker MQTT (Mosquitto).
 * 
 * 	FUNCIONALIDAD:
 * 	1. TEMPERATURA: Lee un sensor LM35 cada 0.5 segundos (si no hay eventos).
 * 	Publica el valor en grados Celsius.
 * 	2. INTERACCIÓN: Detecta la pulsación de un botón (con interrupción) 
 * 	y publica el evento inmediatamente.
 * 	 
 * 	CONTEXTO DEL SISTEMA:
 * 	Las señales son enviadas a un Dashboard en Node-RED. Este actúa como
 * 	controlador central y reenvía comandos a una segunda tarjeta ESP32-S3
 * 	donde se encuentran los actuadores finales.
 *
 * 	PINOUT:
 * 	- Sensor LM35:    GPIO 8  (ADC1 Canal 7)
 * 	- Botón Pulsador: GPIO 47 (Configurado con Pull-Up Interno)
 * 	 
 * 	TÓPICOS MQTT:
 * 	- Publica: "Publicador/Sensores/Temperatura/LM35"
 * 	- Publica: "Publicador/Sensores/Luz/Boton"
 *
 * --------------------------------------------------------------------------
 */

// Librerias Estandar
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>

// Librerias ESP32 (Chip)
#include "esp_system.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"

// GPIO
#include "driver/gpio.h"

// Sistema Operativo RTOS
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"

// Librerias Wifi
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "lwip/err.h"
#include "lwip/sys.h"

// MQTT Libreria
#include "mqtt_client.h"

// Librerias ADC OneShot
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "hal/adc_types.h"

// GPIO de boton que controla LED 
#define BOTON_GPIO 47

/* 
	CONFIGURACION DE ADC
	Indicamos atenuacion y canal de ADC a utilizar 
*/
#define ADC_UNIT            ADC_UNIT_1
#define ADC_CHANNEL         ADC_CHANNEL_7 
#define ADC_ATTEN           ADC_ATTEN_DB_6 // Saturacion hasta 1.7 V 170 grados

/* 
	Configuración de SSID (Nombre de Red) y PASSWORD WIFI
	SE HACE EN EL SDKCONFIG
*/
#define SSID_NOMBRE_WIFI      CONFIG_ESP_WIFI_SSID
#define PASSWORD_WIFI      CONFIG_ESP_WIFI_PASSWORD
#define INTENTOS_DE_CONEXION_MAX  CONFIG_ESP_MAXIMUM_RETRY


/* 	Utilizacion de FreeRTOS
	Banderas para sincronización  		*/
// Conexion a Red WiFi
static EventGroupHandle_t s_wifi_event_group;
// Conexion a MQTT
static EventGroupHandle_t s_mqtt_event_group;
// Indicar si se presiona el boton durante una lectura
static QueueHandle_t cola_boton;

/*
	Definición de Banderas 
	Bits Identificadores
 */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define MQTT_CONNECTED_BIT BIT0

static const char *TAG_WIFI = "ESP32_PUBLICADOR";
static int s_retry_num = 0;

// Evento, Callbacks para conexion WiFi
static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < INTENTOS_DE_CONEXION_MAX) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG_WIFI, "Reintentando conexión WiFi...");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG_WIFI,"Conexión Fallida");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG_WIFI, "IP ESP32 Suscriptor:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

// Rutina para inicializacion WiFi (Interrupcion)
void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = SSID_NOMBRE_WIFI,
            .password = PASSWORD_WIFI,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,          
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );
    ESP_ERROR_CHECK(esp_wifi_start() );

    ESP_LOGI(TAG_WIFI, "Esperando conexión WiFi...");
    
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

    /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
     * happened. */
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG_WIFI, "Conectado a SSID: %s",
                 SSID_NOMBRE_WIFI);
                 
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG_WIFI, "Fallo al conectar a SSID: %s",
                 SSID_NOMBRE_WIFI);
    } else {
        ESP_LOGE(TAG_WIFI, "UNEXPECTED EVENT");
    }
}

// Eventos para conexión a broker MQTT
static const char *TAG_MQTT = "MQTT CONEXION: ";

// Identifica si a ocurrido un error al inicializar la conexion MQTT
static void log_error_if_nonzero(const char *message, int error_code){
	if (error_code != 0){
		ESP_LOGE(TAG_MQTT, "Last error %s: 0x%x", message, error_code);
	}
}

// // Evento, Callbacks Conexion, Desconexion y Envio de datos
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;
        
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG_MQTT, "MQTT Conectado");
        xEventGroupSetBits(s_mqtt_event_group, MQTT_CONNECTED_BIT);
        break;
        
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG_MQTT, "MQTT Desconectado");
        xEventGroupClearBits(s_mqtt_event_group, MQTT_CONNECTED_BIT);
        break;

    case MQTT_EVENT_ERROR:
        ESP_LOGI(TAG_MQTT, "MQTT_EVENT_ERROR");
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            log_error_if_nonzero("reported from esp-tls", event->error_handle->esp_tls_last_esp_err);
            log_error_if_nonzero("reported from tls stack", event->error_handle->esp_tls_stack_err);
            log_error_if_nonzero("captured as transport's socket errno",  event->error_handle->esp_transport_sock_errno);
            ESP_LOGI(TAG_MQTT, "Last errno string (%s)", strerror(event->error_handle->esp_transport_sock_errno));

        }
        break;
    default:
        break;
    }
}

static esp_mqtt_client_handle_t mqtt_app_start(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = CONFIG_BROKER_URL,
        .credentials.client_id = "ESP32_SENSORES",
    };
    
    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(client);
    
    return client;
}

const static char *TAG = "SENSOR LM35";
// ADC SUBRUTINAS (CALIBRACION ADC)
static bool adc_calibration_init(adc_unit_t unit, adc_channel_t channel, adc_atten_t atten, adc_cali_handle_t *out_handle)
{
    adc_cali_handle_t handle = NULL;
    esp_err_t ret = ESP_FAIL;
    bool calibrated = false;

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    if (!calibrated) {
        ESP_LOGI(TAG, "calibration scheme version is %s", "Curve Fitting");
        adc_cali_curve_fitting_config_t cali_config = {
            .unit_id = unit,
            .chan = channel,
            .atten = atten,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        ret = adc_cali_create_scheme_curve_fitting(&cali_config, &handle);
        if (ret == ESP_OK) {
            calibrated = true;
        }
    }
#endif

    *out_handle = handle;
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Calibracion Exitosa");
    } else if (ret == ESP_ERR_NOT_SUPPORTED || !calibrated) {
        ESP_LOGW(TAG, "Fallo en calibración o esquema no soportado");
    } else {
        ESP_LOGE(TAG, "Invalid arg or no memory");
    }

    return calibrated;
}

// INTERRUPCION BOTON
static void IRAM_ATTR boton_isr_handler(void* arg){
	static int64_t ult_t_us = 0;
	int64_t t_actual_us =esp_timer_get_time();
	
	// Filtro antirebote
	if(t_actual_us - ult_t_us > 200000){
		ult_t_us = t_actual_us;
		int msg = 1; // Enviamos un "1" para indicar que se presionó
    	xQueueSendFromISR(cola_boton, &msg, NULL);	
	}
}

// Main
void app_main(void)
{
    // Inicializar NVS y Netif
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    // Inicializar la capa de red global
    ESP_ERROR_CHECK(esp_netif_init());
    // Inicializar el event loop global
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    // Por si hay un error en conexion
    if (CONFIG_LOG_MAXIMUM_LEVEL > CONFIG_LOG_DEFAULT_LEVEL) {
        /* If you only want to open more logs in the wifi module, you need to make the max level greater than the default level,
         * and call esp_log_level_set) before esp_wifi_init() to improve the log level of the wifi module. */
        esp_log_level_set("wifi", CONFIG_LOG_MAXIMUM_LEVEL);
        
    }
    
    // Creacion de cola y grupos de eventos
    s_mqtt_event_group = xEventGroupCreate();
    cola_boton = xQueueCreate(10, sizeof(int));
          
    // Configuracion de GPIO de Boton
	gpio_reset_pin(BOTON_GPIO);
    gpio_set_direction(BOTON_GPIO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BOTON_GPIO, GPIO_PULLUP_ONLY);
    gpio_set_intr_type(BOTON_GPIO, GPIO_INTR_NEGEDGE);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(BOTON_GPIO, boton_isr_handler, NULL);
		
	// Configuracion de ADC
	// Inicializar la Unidad ADC1
	adc_oneshot_unit_handle_t adc1_handle;
	adc_oneshot_unit_init_cfg_t init_config1 = {
		.unit_id = ADC_UNIT,
	};
	ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config1, &adc1_handle));
	// Configurar el Canal 
	adc_oneshot_chan_cfg_t config = {
		.atten = ADC_ATTEN,
	    .bitwidth = ADC_BITWIDTH_DEFAULT, // 12 bits
	};
	ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, ADC_CHANNEL, &config));
	// Inicializar la Calibración (Para convertir a milivoltios)
	adc_cali_handle_t adc1_cali_handle = NULL;
	bool do_calibration = adc_calibration_init(ADC_UNIT, ADC_CHANNEL, ADC_ATTEN, &adc1_cali_handle);
	
	int PULSO_BOTON;
	int adc_raw, voltage;
	
	// Iniciar WiFi
    ESP_LOGI(TAG_WIFI, "Iniciando WIFI ...");
    wifi_init_sta();
    
    // Iniciar MQTT
   	ESP_LOGI(TAG_MQTT, "Iniciando MQTT ...");
    esp_mqtt_client_handle_t client = mqtt_app_start();
    
    // Espera la confirmacion de conexion MQTT
    EventBits_t bits = xEventGroupWaitBits(s_mqtt_event_group,
            MQTT_CONNECTED_BIT,
            pdFALSE,
            pdTRUE,
            portMAX_DELAY);
            
    // Condicion si se logra conectar al Broker
    if (bits & MQTT_CONNECTED_BIT) {
        ESP_LOGI(TAG_MQTT, "¡Conectado al Broker!");
   	    ESP_LOGI(TAG, "Iniciando lecturas en el GPIO...");
   	}	
   	else{
        ESP_LOGE(TAG_MQTT, "Error: No se pudo conectar al broker MQTT");
    }
	
	// Lectura y envio de datos a topicos
	while (1) {
		// Conversion de dato a texto
		char payload_buffer[50];
		
		// Revisar Botón (Espera hasta 500ms por un pulso)
		if (xQueueReceive(cola_boton, &PULSO_BOTON, pdMS_TO_TICKS(500)) == pdTRUE) {
	    	// --- A. LOGICA DE BOTON (Se ejecutó porque presionaste) ---
	    	ESP_LOGI(TAG, "¡Boton Presionado!");
	        esp_mqtt_client_publish(client, "Publicador/Sensores/Luz/Boton", "Pulso", 0, 1, 0);
	        // Pequeña espera anti-rebote simple
	        vTaskDelay(pdMS_TO_TICKS(250)); 
       	} 
       	else {
	    	// Leer el sensor
	        ESP_ERROR_CHECK(adc_oneshot_read(adc1_handle, ADC_CHANNEL, &adc_raw));
	        // Convertir a voltaje (si la calibración fue exitosa)
		    if (do_calibration) {
		    	ESP_ERROR_CHECK(adc_cali_raw_to_voltage(adc1_cali_handle, adc_raw, &voltage));
		        
		        // Conversion a grados
		        float tempC = (float)voltage/10.0;
		        snprintf(payload_buffer, sizeof(payload_buffer), "%.1f", tempC);
		        ESP_LOGI(TAG, "Temperatura: %.1f C (Voltaje: %d mV", tempC, voltage);
		        
		        // Publicacion 
				esp_mqtt_client_publish(client, "Publicador/Sensores/Temperatura/LM35", payload_buffer, 0, 1, 0);
			}
    	}
    }   
}