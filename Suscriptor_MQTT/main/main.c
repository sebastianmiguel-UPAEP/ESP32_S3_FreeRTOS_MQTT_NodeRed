
// 	Suscriptor_MQTT
/* * -------------------------------------------------------------------------
 * 	PROYECTO: NODO ACTUADOR ESP32-S3 (MQTT + GPIO Output)
 * --------------------------------------------------------------------------
 *	DESCRIPCIÓN:
 * 	Este código gestiona el control de dispositivos de salida y la recepción de datos.
 * 	Realiza la conexión a WiFi y posteriormente se conecta a un Broker MQTT
 * 	para operar en modo Suscriptor.
 *  
 * 	FUNCIONALIDAD:
 * 	1. SUSCRIPCIÓN: Escucha permanentemente dos tópicos específicos para
 * 	recibir comandos de control (Booleanos: "true"/"false").
 * 	2. ACTUACIÓN: Controla el estado lógico (ON/OFF) de un Ventilador y 
 * 	una Luz mediante salidas digitales, utilizando una Cola (Queue) para
 * 	desacoplar la recepción de la ejecución.
 *  
 * 	CONTEXTO DEL SISTEMA:
 *	Recibe órdenes desde un Dashboard en Node-RED. Estas órdenes pueden ser
 * 	manuales o automáticas (derivadas de la lógica de control basada en
 * 	los datos del NODO SENSOR).
 *
 * 	PINOUT:
 * 	- Ventilador: GPIO 10 (Salida Digital)
 * 	- Luz LED:    GPIO 21 (Salida Digital)
 *
 *	TÓPICOS MQTT:
 * -Suscrito: "Suscripcion/Actuadores/Ventilador/Estado"
 * -Suscrito: "Suscripcion/Actuadores/Luz/Estado"
 *
 * --------------------------------------------------------------------------
 */

// Librerias Estandar
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

// Librerias ESP32 (Chip)
#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"

// Sistema Operativo RTOS
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

// GPIO
#include "driver/gpio.h"

// Librerias Wifi
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "lwip/err.h"
#include "lwip/sys.h"

// MQTT Libreria
#include "mqtt_client.h"

// GPIO a utilizar 
#define VENTILADOR_GPIO 10
#define LUZ_GPIO 21

// Identificador de elemento 
#define ID_VENTILADOR 1
#define ID_LUZ 2

/* 
	Configuración de SSID (Nombre de Red) y PASSWORD WIFI
	SE HACE EN EL SDKCONFIG
*/
#define SSID_NOMBRE_WIFI      CONFIG_ESP_WIFI_SSID
#define PASSWORD_WIFI      CONFIG_ESP_WIFI_PASSWORD
#define INTENTOS_DE_CONEXION_MAX  CONFIG_ESP_MAXIMUM_RETRY

// Estructura para lectura de Booleanos 
typedef struct {
    int id_dispositivo; // 1 (Ventilador) o 2 (LED)
    int estado;         // 1 (ON) o 0 (OFF)
} mensaje_mqtt_t;


/* 	Utilizacion de FreeRTOS
	Banderas para sincronización  		*/
// Conexión a una Red WiFi 
static EventGroupHandle_t s_wifi_event_group;
// Conexión a broker MQTT
static EventGroupHandle_t s_mqtt_event_group;
// Cola para gestionar un búfer de mensajes (First In, First Out)
static QueueHandle_t cola_mensajes_mqtt;

/*
	Definición de Banderas 
	Bits Identificadores
 */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define MQTT_CONNECTED_BIT BIT0

static const char *TAG_WIFI = "ESP32_SUSCRIPTOR";
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

// Evento, Callbacks Conexion, Desconexion y Recepcion de datos
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
    
    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG_MQTT, "MQTT Dato Recibido");
        
        // Deteccion de ID dispositivo
        mensaje_mqtt_t msg;
   		msg.id_dispositivo = -1;
   		msg.estado=0;
        
        // Comparación de Tópicos
        if (strncmp(event->topic, "Suscripcion/Actuadores/Ventilador/Estado", event->topic_len) == 0) {
        msg.id_dispositivo = ID_VENTILADOR;
    	} else if(strncmp(event->topic, "Suscripcion/Actuadores/Luz/Estado", event->topic_len) == 0) {
        msg.id_dispositivo = ID_LUZ;
    	}
    	
    	// Se lee el dato True/False
    	if (strncmp(event->data, "true", event->data_len) == 0) {
        msg.estado = 1;
   		} else if (strncmp(event->data, "false", event->data_len) == 0) {
        msg.estado = 0;
    	}

    	// 3. ENVIAR A LA COLA (Solo si es válido)
    	if (msg.id_dispositivo != -1) {
        xQueueSend(cola_mensajes_mqtt, &msg, 0);
	    }
    	
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

// Rutina para inicializacion conexion a broker MQTT (Interrupcion)
static esp_mqtt_client_handle_t mqtt_app_start(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = CONFIG_BROKER_URL,
        .credentials.client_id = "ESP32_LECTURA",
    };
    
    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(client);
    
    return client;
}

// MAIN
void app_main(void){
	// Inicializar NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    
    // Creacion de cola y grupos de eventos
    cola_mensajes_mqtt = xQueueCreate(10, sizeof(mensaje_mqtt_t));
    s_mqtt_event_group = xEventGroupCreate();
    // Indicador de error al crear cola
    if (cola_mensajes_mqtt == NULL) {
        ESP_LOGE(TAG_MQTT, "Error creando la cola");
        return;
    }
    
    // Inicializar la capa de red global
    ESP_ERROR_CHECK(esp_netif_init());
    // Inicializar el event loop global
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    // Por si hay un error en conexion
    if (CONFIG_LOG_MAXIMUM_LEVEL > CONFIG_LOG_DEFAULT_LEVEL) {
        /* If you only want to open more logs in the wifi module, you need to make the max level greater than the default level,
         * and call esp_log_level_set() before esp_wifi_init() to improve the log level of the wifi module. */
        esp_log_level_set("wifi", CONFIG_LOG_MAXIMUM_LEVEL);
        
    }
	
	// Configuracion de GPIO Ventilador
	gpio_reset_pin(VENTILADOR_GPIO);
    gpio_set_direction(VENTILADOR_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(VENTILADOR_GPIO, 0);
    
    // Configuracion de GPIO LED
    gpio_reset_pin(LUZ_GPIO);
    gpio_set_direction(LUZ_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(LUZ_GPIO, 0);
    
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
        ESP_LOGI(TAG_MQTT, "Conectado al Broker. Suscribiendo...");
        
        // Suscribirse a los siguientes tópicos
        esp_mqtt_client_subscribe(client, "Suscripcion/Actuadores/Ventilador/Estado", 1);
        esp_mqtt_client_subscribe(client, "Suscripcion/Actuadores/Luz/Estado", 1);
        		
    }
    else {
        ESP_LOGE(TAG_MQTT, "Error: No se pudo conectar al broker MQTT");
    }
    
    // Recepcion de mensaje
    mensaje_mqtt_t msg_recibido;
    while (1) {
			if (xQueueReceive(cola_mensajes_mqtt, &msg_recibido, portMAX_DELAY)) {
			ESP_LOGI(TAG_MQTT, "Disp=%d, Estado=%d", msg_recibido.id_dispositivo, msg_recibido.estado);

            // Seleccionamos qué dispositivo actua
            switch (msg_recibido.id_dispositivo) {
                case ID_VENTILADOR:
                    gpio_set_level(VENTILADOR_GPIO, msg_recibido.estado);
                    break;
                    
                case ID_LUZ:
                    gpio_set_level(LUZ_GPIO, msg_recibido.estado);
                    break;
                    
                default:
                    ESP_LOGW(TAG_MQTT, "ID desconocido");
                    break;
            }
		}
		
	}    
    
}